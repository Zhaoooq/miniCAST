#include "MfcManager.h"
#include "CS200ADriver.h"
#include "DeviceDiscovery.h"
#include "MfcProtocol.h"
#include <QElapsedTimer>
#include <QSet>
#include <QThread>
#include <algorithm>
#include <cmath>

namespace {
bool sameGas(QString actual, QString configured)
{
    actual.remove(' '); configured.remove(' ');
    const auto canonical = [](QString value) {
        value = value.trimmed().toLower();
        if (value == QStringLiteral("空气") || value == QStringLiteral("air")) return QStringLiteral("air");
        if (value == QStringLiteral("氮气") || value == QStringLiteral("n2") || value == QStringLiteral("nitrogen")) return QStringLiteral("n2");
        if (value == QStringLiteral("丙烷") || value == QStringLiteral("propane")) return QStringLiteral("propane");
        return value;
    };
    actual = canonical(actual); configured = canonical(configured);
    return actual.compare(configured, Qt::CaseInsensitive) == 0;
}

QString metadataFailureStatus(const QString &error, const QVariantMap &transaction)
{
    const QString result = transaction.value("result").toString();
    const QString text = error.toLower();
    if (text.contains(QStringLiteral("empty_payload"))) return QStringLiteral("UNSUPPORTED_OR_EMPTY_RESPONSE");
    if (result == QStringLiteral("TIMEOUT") || text.contains(QStringLiteral("timeout"))) return QStringLiteral("TIMEOUT");
    if (text.contains(QStringLiteral("nak"))) return QStringLiteral("NAK");
    if (text.contains(QStringLiteral("checksum"))) return QStringLiteral("CHECKSUM_ERROR");
    if (text.contains(QStringLiteral("class mismatch")) || text.contains(QStringLiteral("instance mismatch"))
        || text.contains(QStringLiteral("attribute mismatch")) || text.contains(QStringLiteral("service mismatch")))
        return QStringLiteral("UNEXPECTED_RESPONSE");
    return QStringLiteral("READ_FAILED");
}

QString metadataStatusText(const QString &status)
{
    if (status == QStringLiteral("MATCH")) return QStringLiteral("一致");
    if (status == QStringLiteral("MISMATCH")) return QStringLiteral("配置不一致");
    if (status == QStringLiteral("READ_OK")) return QStringLiteral("读取成功");
    if (status == QStringLiteral("UNSUPPORTED_OR_EMPTY_RESPONSE")) return QStringLiteral("设备未返回数据");
    if (status == QStringLiteral("TIMEOUT")) return QStringLiteral("读取超时");
    if (status == QStringLiteral("NAK")) return QStringLiteral("设备拒绝请求（NAK）");
    if (status == QStringLiteral("CHECKSUM_ERROR")) return QStringLiteral("校验和错误");
    if (status == QStringLiteral("UNEXPECTED_RESPONSE")) return QStringLiteral("响应命令不匹配");
    return QStringLiteral("读取失败");
}

QVariantMap makeMetadataResult(const QString &status, const QString &expected,
                               const QString &reported, const QString &error = {})
{
    return {{"status", status}, {"statusText", metadataStatusText(status)},
            {"expected", expected}, {"reported", reported},
            {"reportedAvailable", status == QStringLiteral("MATCH") || status == QStringLiteral("MISMATCH")
                || status == QStringLiteral("READ_OK")}, {"error", error}};
}
}

double MfcManager::fullScaleSccm(const MfcDeviceConfig &config)
{
    return config.unit.compare(QStringLiteral("L/min"), Qt::CaseInsensitive) == 0
        ? config.fullScale * 1000.0 : config.fullScale;
}

void MfcManager::setControlState(MfcControlState state)
{
    for (auto &device : m_devices) if (device.config.enabled) device.controlState = state;
    if (m_trace) {
        const QString name = state == MfcControlState::PreparingControl ? QStringLiteral("PreparingControl")
            : state == MfcControlState::SettingDigitalMode ? QStringLiteral("Current CM = Digital")
            : state == MfcControlState::Holding ? QStringLiteral("Hold")
            : state == MfcControlState::LoadingSetpoints ? QStringLiteral("Digital Setpoint")
            : state == MfcControlState::VerifyingSetpoints ? QStringLiteral("Readback Digital Setpoint")
            : state == MfcControlState::StartingControl ? QStringLiteral("Follow")
            : state == MfcControlState::Controlling ? QStringLiteral("Controlling")
            : state == MfcControlState::ControlFault ? QStringLiteral("Preflight/Control Failed")
            : state == MfcControlState::ControlStopped ? QStringLiteral("Control Stopped")
            : QStringLiteral("Control transaction");
        m_trace(QStringLiteral("[CONTROL] state=%1").arg(name));
    }
}

QStringList MfcManager::metadataMismatchFields(const MfcDeviceConfig &config,
                                               const MfcDeviceInfo &info)
{
    QStringList mismatches;
    if (info.address != config.address) mismatches.append(QStringLiteral("RS485 Address"));
    if (!sameGas(info.targetGasName, config.gasType)) mismatches.append(QStringLiteral("Gas Name"));
    // Compare decoded UINT16 values, never the UI's zero-padded text.
    if (info.targetGasCode != config.expectedGasCode) mismatches.append(QStringLiteral("Target Gas Code"));
    if (std::abs(info.fullScale - config.expectedDeviceFullScaleSccm) > 0.5)
        mismatches.append(QStringLiteral("Target Full Scale"));
    return mismatches;
}

QStringList MfcManager::readAndReportMetadata(MfcDeviceState &state, CS200ADriver &driver,
                                              bool includeCalibration)
{
    state.metadataVerificationAttempted = true;
    state.metadataVerificationComplete = false;
    state.metadataAvailable = false;
    state.metadataResults.clear();
    state.info.model.clear();
    state.info.serialNumber.clear();
    state.info.targetGasName.clear();
    state.info.targetGasCode = 0;
    state.info.targetGasFullScale = 0;
    state.info.calibrationGasName.clear();
    state.info.calibrationGasCode = 0;
    state.info.calibrationGasFullScale = 0;
    state.info.calibrationGasReadFromDevice = false;
    state.info.fullScale = 0.0;
    state.info.conversionFactor = 0.0;
    state.info.fullScaleReadFromDevice = false;
    QStringList mismatches;
    QStringList failures;
    const int address = state.config.address;
    const auto recordEvidence = [this, address](const QString &name, const QString &parseResult,
                                                const QString &parsedValue, const QVariantMap &transaction) {
        if (!m_trace) return;
        // Device ownership is intentionally the address captured in the one
        // pending request, all the way to this state object.  CS200 response
        // address (normally 0x00) is logged as evidence but never consulted.
        m_trace(QStringLiteral("[META][%1][%2] timestamp=%3 REQUEST_ADDRESS=%4 PENDING_ADDRESS=%5 "
                               "STATE_DESTINATION_ADDRESS=%6 RESPONSE_ADDRESS=0x%7 TX=%8 ACK=%9 RX=%10 "
                               "RX_TOTAL_LENGTH=%11 RX_DATA_LENGTH=%12 CLASS=0x%13 INSTANCE=0x%14 "
                               "ATTRIBUTE=0x%15 PAYLOAD=%16 PARSED_VALUE=%17 CHECKSUM=%18 "
                               "beforeTxRxBufferSize=%19 afterResponseRxBufferSize=%20 PARSE_RESULT=%21")
            .arg(address).arg(name, QDateTime::currentDateTime().toString(Qt::ISODateWithMs))
            .arg(transaction.value("address").toInt()).arg(transaction.value("address").toInt()).arg(address)
            .arg(transaction.value("responseAddress").toInt(), 2, 16, QLatin1Char('0'))
            .arg(transaction.value("txRaw").toString(), transaction.value("ackRaw").toString(),
                 transaction.value("rxRaw").toString())
            .arg(transaction.value("rxTotalLength").toInt()).arg(transaction.value("responseDataLength").toInt())
            .arg(transaction.value("receivedClass").toInt(), 2, 16, QLatin1Char('0'))
            .arg(transaction.value("receivedInstance").toInt(), 2, 16, QLatin1Char('0'))
            .arg(transaction.value("receivedAttribute").toInt(), 2, 16, QLatin1Char('0'))
            .arg(transaction.value("payloadRaw").toString())
            .arg(parsedValue)
            .arg(transaction.value("checksumValid").toBool() ? QStringLiteral("OK") : QStringLiteral("NOT_VALIDATED"))
            .arg(transaction.value("rxBytesAvailableBeforeTx").toInt())
            .arg(transaction.value("afterResponseRxBufferSize").toInt()).arg(parseResult));
        if (transaction.value("rxBytesAvailableBeforeTx").toInt() > 0
            || transaction.value("afterResponseRxBufferSize").toInt() > 0)
            m_trace(QStringLiteral("[META][%1][%2] META_STALE_RX before=%3 after=%4; transport recovery is retained")
                .arg(address).arg(name).arg(transaction.value("rxBytesAvailableBeforeTx").toInt())
                .arg(transaction.value("afterResponseRxBufferSize").toInt()));
    };
    const auto pauseAfterComplete = [this] {
        // Each reader has returned or thrown only after SerialTransport's
        // FinishGuard cleared its single pending request.
        if (!m_transport.transactionPending()) QThread::msleep(m_settings.metadataInterRequestDelayMs);
    };
    const auto read = [&](const QString &name, const QString &expected, auto reader, auto store,
                          auto matches) {
        try {
            const auto value = reader();
            store(value);
            const QString reported = QVariant::fromValue(value).toString();
            const bool comparison = expected.isEmpty() || matches(value);
            const QString status = expected.isEmpty() ? QStringLiteral("READ_OK")
                : comparison ? QStringLiteral("MATCH") : QStringLiteral("MISMATCH");
            state.metadataResults.insert(name, makeMetadataResult(status, expected, reported));
            if (status == QStringLiteral("MISMATCH")) mismatches.append(name);
            recordEvidence(name, status, reported, m_transport.diagnostics().value("lastTransaction").toMap());
        } catch (const std::exception &error) {
            const QString detail = QString::fromUtf8(error.what());
            const QVariantMap transaction = m_transport.diagnostics().value("lastTransaction").toMap();
            const QString status = metadataFailureStatus(detail, transaction);
            state.metadataResults.insert(name, makeMetadataResult(status, expected, QStringLiteral("--"), detail));
            failures.append(name + QStringLiteral("（") + metadataStatusText(status) + QStringLiteral("）"));
            recordEvidence(name, status + QStringLiteral(": ") + detail, QStringLiteral("--"), transaction);
        }
        pauseAfterComplete();
    };

    read(QStringLiteral("Model"), {}, [&] { return driver.readModelIdentifier(); },
         [&](const QString &v) { state.info.model = v; }, [](const auto &) { return true; });
    read(QStringLiteral("Serial"), {}, [&] { return driver.readSerialNumber(); },
         [&](const QString &v) { state.info.serialNumber = v; }, [](const auto &) { return true; });
    read(QStringLiteral("Target Gas Name"), state.config.gasType, [&] { return driver.readTargetGasName(); },
         [&](const QString &v) { state.info.targetGasName = v; }, [&](const QString &v) { return sameGas(v, state.config.gasType); });
    read(QStringLiteral("Target Gas Code"), QString::number(state.config.expectedGasCode), [&] { return driver.readTargetGasCode(); },
         [&](quint16 v) { state.info.targetGasCode = v; }, [&](quint16 v) { return v == state.config.expectedGasCode; });
    read(QStringLiteral("Target Full Scale"), QString::number(state.config.expectedDeviceFullScaleSccm), [&] { return driver.readTargetGasFullScale(); },
         [&](quint16 v) { state.info.targetGasFullScale = v; state.info.fullScale = v; state.info.fullScaleUnit = QStringLiteral("sccm"); state.info.fullScaleReadFromDevice = true; },
         [&](quint16 v) { return std::abs(static_cast<double>(v) - state.config.expectedDeviceFullScaleSccm) <= 0.5; });
    if (includeCalibration) {
        read(QStringLiteral("Calibration Gas Code"), {}, [&] { return driver.readCalibrationGasCode(); },
             [&](quint16 v) { state.info.calibrationGasCode = v; }, [](const auto &) { return true; });
        read(QStringLiteral("Calibration Full Scale"), {}, [&] { return driver.readCalibrationGasFullScale(); },
             [&](quint16 v) { state.info.calibrationGasFullScale = v; state.info.calibrationGasReadFromDevice = true; },
             [](const auto &) { return true; });
        read(QStringLiteral("Calibration Gas Name"), {}, [&] { return driver.readCalibrationGasName(); },
             [&](const QString &v) { state.info.calibrationGasName = v; }, [](const auto &) { return true; });
    }
    read(QStringLiteral("Conversion Factor"), {}, [&] { return driver.readConversionFactor(); },
         [&](double v) { state.info.conversionFactor = v; }, [](const auto &) { return true; });
    read(QStringLiteral("RS485 Address"), QString::number(state.config.address), [&] { return driver.readRs485MacAddress(); },
         [&](quint16 v) { state.info.address = static_cast<quint8>(v); }, [&](quint16 v) { return v == state.config.address; });
    read(QStringLiteral("Baud"), {}, [&] { return driver.readBaudRate(); },
         [&](quint16 v) { state.info.baudRate = v; }, [](const auto &) { return true; });

    const bool allRead = failures.isEmpty();
    state.metadataVerificationComplete = allRead;
    state.metadataAvailable = allRead;
    if (!failures.isEmpty()) state.lastError = QStringLiteral("设备信息无法完成核验：") + failures.join(QStringLiteral("、"));
    else if (!mismatches.isEmpty()) {
        // A Target register is a user/device parameter, not a claim that the
        // physical MFC model or its calibrated range is wrong.
        QStringList words = mismatches;
        words.replaceInStrings(QStringLiteral("Target Full Scale"),
                               QStringLiteral("Target Gas Full Scale 与现场配置不一致"));
        state.lastError = QStringLiteral("设备配置与现场配置不一致：") + words.join(QStringLiteral(", "));
    }
    else state.lastError.clear();
    return mismatches;
}

bool MfcManager::validateControlPreflight(const QMap<int, double> &targets, QString *errorMessage)
{
    if (!isConnected()) { if (errorMessage) *errorMessage = QStringLiteral("控制预检失败：串口未连接"); return false; }
    QStringList failures;
    for (auto &state : m_devices) {
        if (!state.config.enabled) continue;
        if (!targets.contains(state.config.address) || !state.communicationOnline || !state.config.addressConfirmed
            || !state.engineeringConfigured) {
            failures.append(QStringLiteral("%1 未在线或配置未确认").arg(state.config.displayName));
            continue;
        }
        const double target = targets.value(state.config.address);
        const double maximum = state.config.maximumSetpoint > 0 ? state.config.maximumSetpoint : state.config.fullScale;
        if (!std::isfinite(target) || target < state.config.minimumSetpoint || target > maximum) {
            failures.append(QStringLiteral("%1 目标流量超过满量程").arg(state.config.displayName));
            continue;
        }
        try {
            m_transport.setLogicalChannel(state.config.logicalChannel);
            CS200ADriver driver(m_transport, state.config.address, m_trace);
            driver.setTransactionOptions(m_settings.ackTimeoutMs, m_settings.responseTimeoutMs, 0);
            const QStringList mismatches = readAndReportMetadata(state, driver);
            if (!state.metadataVerificationComplete)
                failures.append(QStringLiteral("%1（设备信息无法完成核验：%2）")
                    .arg(state.config.displayName, state.lastError));
            else if (!mismatches.isEmpty())
                failures.append(QStringLiteral("%1（%2）").arg(state.config.displayName, mismatches.join(QStringLiteral(", "))));
        } catch (const std::exception &e) {
            state.metadataAvailable = false;
            failures.append(QStringLiteral("%1 读取设备信息失败：%2").arg(state.config.displayName,
                QString::fromUtf8(e.what())));
        }
    }
    if (!failures.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("控制预检失败（未发送任何控制写命令）：")
            + failures.join(QStringLiteral("；"));
        return false;
    }
    return true;
}

bool MfcManager::applyOperatingPoint(const QMap<int, double> &targets, QString *errorMessage,
                                     bool *writesStarted)
{
    if (writesStarted) *writesStarted = false;
    setControlState(MfcControlState::PreparingControl);
    if (!validateControlPreflight(targets, errorMessage)) { setControlState(MfcControlState::ControlFault); return false; }
    try {
        if (writesStarted) *writesStarted = true;
        setControlState(MfcControlState::SettingDigitalMode);
        for (auto &state : m_devices) if (state.config.enabled) {
            m_transport.setLogicalChannel(state.config.logicalChannel); CS200ADriver d(m_transport, state.config.address, m_trace);
            d.setTransactionOptions(m_settings.ackTimeoutMs, m_settings.responseTimeoutMs, m_settings.retryCount);
            if (d.readCurrentControlMode() != MfcControlMode::Digital) { d.setCurrentControlMode(MfcControlMode::Digital); if (d.readCurrentControlMode() != MfcControlMode::Digital) throw MfcProtocol::Error("Digital Mode verify failed"); }
        }
        setControlState(MfcControlState::Holding);
        for (auto &state : m_devices) if (state.config.enabled) {
            m_transport.setLogicalChannel(state.config.logicalChannel); CS200ADriver d(m_transport,state.config.address,m_trace);
            d.setTransactionOptions(m_settings.ackTimeoutMs,m_settings.responseTimeoutMs,m_settings.retryCount);
            d.setHoldFollow(false);
            if (d.readHoldFollow()) throw MfcProtocol::Error("Hold verification failed");
        }
        setControlState(MfcControlState::LoadingSetpoints);
        for (auto &state : m_devices) if (state.config.enabled) { const double ratio = targets.value(state.config.address) / state.config.fullScale; const quint16 raw = MfcProtocol::encodeUfrac16(ratio); m_transport.setLogicalChannel(state.config.logicalChannel); CS200ADriver d(m_transport,state.config.address,m_trace); d.setTransactionOptions(m_settings.ackTimeoutMs,m_settings.responseTimeoutMs,m_settings.retryCount); d.setDigitalSetpoint(raw); state.targetFlow = targets.value(state.config.address); state.targetConfirmed = false; }
        setControlState(MfcControlState::VerifyingSetpoints);
        for (auto &state : m_devices) if (state.config.enabled) { const quint16 expected = MfcProtocol::encodeUfrac16(targets.value(state.config.address)/state.config.fullScale); m_transport.setLogicalChannel(state.config.logicalChannel); CS200ADriver d(m_transport,state.config.address,m_trace); d.setTransactionOptions(m_settings.ackTimeoutMs,m_settings.responseTimeoutMs,m_settings.retryCount); if (d.readDigitalSetpoint() != expected) throw MfcProtocol::Error("Digital Setpoint verification failed"); }
        setControlState(MfcControlState::StartingControl);
        for (auto &state : m_devices) if (state.config.enabled) {
            m_transport.setLogicalChannel(state.config.logicalChannel); CS200ADriver d(m_transport,state.config.address,m_trace);
            d.setTransactionOptions(m_settings.ackTimeoutMs,m_settings.responseTimeoutMs,m_settings.retryCount);
            d.setHoldFollow(true);
            if (!d.readHoldFollow()) throw MfcProtocol::Error("Follow verification failed");
        }
        for (auto &state : m_devices) if (state.config.enabled) { const quint16 expected = MfcProtocol::encodeUfrac16(targets.value(state.config.address)/state.config.fullScale); m_transport.setLogicalChannel(state.config.logicalChannel); CS200ADriver d(m_transport,state.config.address,m_trace); d.setTransactionOptions(m_settings.ackTimeoutMs,m_settings.responseTimeoutMs,m_settings.retryCount); if (d.readActiveSetpoint() != expected) throw MfcProtocol::Error("设定已写入，但当前有效设定不一致"); state.targetConfirmed = true; state.lastControlConfirmation = QDateTime::currentDateTime(); }
        m_controlActive = true; setControlState(MfcControlState::Controlling); return true;
    } catch (const std::exception &e) {
        m_controlActive = false; m_controlFailure = QString::fromUtf8(e.what()); setControlState(MfcControlState::ControlFault);
        if (errorMessage) *errorMessage = QStringLiteral("运行点下发失败：") + m_controlFailure; return false;
    }
}

bool MfcManager::verifyDeviceInformation(QString *errorMessage)
{
    if (!isConnected()) {
        if (errorMessage) *errorMessage = QStringLiteral("设备信息核验失败：串口未连接");
        return false;
    }
    QStringList failures;
    for (auto &state : m_devices) {
        if (!state.config.enabled) continue;
        try {
            m_transport.setLogicalChannel(state.config.logicalChannel);
            CS200ADriver driver(m_transport, state.config.address, m_trace);
            driver.setTransactionOptions(m_settings.ackTimeoutMs, m_settings.responseTimeoutMs,
                                         m_settings.retryCount);
            // Calibration attributes are a diagnosis-only read.  Preflight
            // keeps its established Target/identity checks and lock behavior.
            (void)readAndReportMetadata(state, driver, true);
            if (!state.metadataVerificationComplete)
                failures.append(QStringLiteral("%1：%2").arg(state.config.displayName, state.lastError));
        } catch (const std::exception &e) {
            state.metadataAvailable = false;
            failures.append(QStringLiteral("%1：%2").arg(state.config.displayName, QString::fromUtf8(e.what())));
        }
    }
    if (!failures.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("设备信息无法完成核验：") + failures.join(QStringLiteral("；"));
        return false;
    }
    return true;
}

bool MfcManager::runFullScaleDiagnostics(QString *errorMessage)
{
    if (!isConnected()) {
        if (errorMessage) *errorMessage = QStringLiteral("Full Scale 诊断失败：串口未连接");
        return false;
    }
    if (m_controlActive) {
        if (errorMessage) *errorMessage = QStringLiteral("控制会话期间不能执行 Full Scale 诊断");
        return false;
    }

    // The requested repeat experiment uses 100 ms; do not make a configured
    // metadata delay shorter than that diagnostic isolation boundary.
    const int repeatDelayMs = qMax(100, m_settings.metadataInterRequestDelayMs);
    QStringList failures;
    const auto pause = [this, repeatDelayMs] {
        if (!m_transport.transactionPending()) QThread::msleep(repeatDelayMs);
    };
    const auto sample = [this, &failures, &pause](MfcDeviceState &state, const QString &series,
                                                   QVariantList &samples) {
        try {
            m_transport.setLogicalChannel(state.config.logicalChannel);
            CS200ADriver driver(m_transport, state.config.address, m_trace);
            driver.setTransactionOptions(m_settings.ackTimeoutMs, m_settings.responseTimeoutMs,
                                         m_settings.retryCount);
            const quint16 value = driver.readTargetGasFullScale();
            const QVariantMap transaction = m_transport.diagnostics().value("lastTransaction").toMap();
            const QVariantMap evidence{{"value", value}, {"payload", transaction.value("payloadRaw")},
                {"requestAddress", transaction.value("address")},
                {"pendingAddress", transaction.value("address")},
                {"destinationStateAddress", state.config.address},
                {"responseAddress", transaction.value("responseAddress")},
                {"staleRxBefore", transaction.value("rxBytesAvailableBeforeTx")},
                {"staleRxAfter", transaction.value("afterResponseRxBufferSize")},
                {"class", transaction.value("receivedClass")}, {"instance", transaction.value("receivedInstance")},
                {"attribute", transaction.value("receivedAttribute")}, {"tx", transaction.value("txRaw")},
                {"rx", transaction.value("rxRaw")}};
            samples.append(evidence);
            if (m_trace) m_trace(QStringLiteral("[META_DIAG][%1][%2] REQUEST_ADDRESS=%3 PENDING_ADDRESS=%4 "
                                                 "STATE_DESTINATION_ADDRESS=%5 RESPONSE_ADDRESS=0x%6 TX=%7 RX=%8 "
                                                 "CLASS=0x%9 INSTANCE=0x%10 ATTRIBUTE=0x%11 PAYLOAD=%12 PARSED_VALUE=%13")
                .arg(static_cast<int>(state.config.address)).arg(series).arg(transaction.value("address").toInt())
                .arg(transaction.value("address").toInt()).arg(state.config.address)
                .arg(transaction.value("responseAddress").toInt(), 2, 16, QLatin1Char('0'))
                .arg(transaction.value("txRaw").toString(), transaction.value("rxRaw").toString())
                .arg(transaction.value("receivedClass").toInt(), 2, 16, QLatin1Char('0'))
                .arg(transaction.value("receivedInstance").toInt(), 2, 16, QLatin1Char('0'))
                .arg(transaction.value("receivedAttribute").toInt(), 2, 16, QLatin1Char('0'))
                .arg(transaction.value("payloadRaw").toString()).arg(value));
        } catch (const std::exception &error) {
            const QString detail = QString::fromUtf8(error.what());
            samples.append(QVariantMap{{"error", detail}, {"destinationStateAddress", state.config.address}});
            failures.append(QStringLiteral("地址 %1 %2：%3").arg(static_cast<int>(state.config.address)).arg(series, detail));
        }
        pause();
    };

    for (auto &state : m_devices) {
        if (!state.config.enabled) continue;
        QVariantList repeated;
        for (int i = 0; i < 10; ++i) sample(state, QStringLiteral("repeat_%1").arg(i + 1), repeated);
        QSet<int> values;
        for (const QVariant &entry : repeated) {
            const QVariantMap item = entry.toMap();
            if (item.contains("value")) values.insert(item.value("value").toInt());
        }
        state.metadataDiagnostics.insert(QStringLiteral("targetFullScaleSamples"), repeated);
        state.metadataDiagnostics.insert(QStringLiteral("targetFullScaleConsistent"),
                                         repeated.size() == 10 && values.size() == 1);
        state.metadataDiagnostics.insert(QStringLiteral("repeatDelayMs"), repeatDelayMs);
    }

    MfcDeviceState *address32 = nullptr;
    MfcDeviceState *address34 = nullptr;
    for (auto &state : m_devices) {
        if (state.config.enabled && state.config.address == 32) address32 = &state;
        if (state.config.enabled && state.config.address == 34) address34 = &state;
    }
    if (address32 && address34) {
        QVariantList sequence;
        for (int i = 0; i < 5; ++i) {
            sample(*address32, QStringLiteral("alternate_%1_32").arg(i + 1), sequence);
            sample(*address34, QStringLiteral("alternate_%1_34").arg(i + 1), sequence);
        }
        address32->metadataDiagnostics.insert(QStringLiteral("interleaved32_34Samples"), sequence);
        address34->metadataDiagnostics.insert(QStringLiteral("interleaved32_34Samples"), sequence);
    }
    if (!failures.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("Full Scale 诊断未完成：") + failures.join(QStringLiteral("；"));
        return false;
    }
    return true;
}

bool MfcManager::stopControl(QString *errorMessage, std::function<void()> progress)
{
    setControlState(MfcControlState::StoppingControl);
    QStringList failures;
    // The loop intentionally has no all-or-nothing exception boundary.  A
    // missing MFC2, for example, must never prevent MFC1/3/4/5 from receiving
    // their finite, whitelisted Digital Setpoint=0 transaction.
    for (auto &state : m_devices) {
        if (!state.config.enabled) continue;
        state.targetFlow = 0.0; // command model only; READ_FLOW is untouched.
        state.targetConfirmed = false;
        state.stopFailure.clear();
        state.stopStage = MfcStopStage::WritingZero;
        if (progress) progress();
        try {
            m_transport.setLogicalChannel(state.config.logicalChannel);
            CS200ADriver driver(m_transport, state.config.address, m_trace);
            driver.setTransactionOptions(m_settings.ackTimeoutMs, m_settings.responseTimeoutMs,
                                         m_settings.retryCount);
            // UFRAC16 zero is 0x4000, serialized by uint16Le as 00 40.
            driver.setDigitalSetpoint(0x4000);
            if (driver.readDigitalSetpoint() != 0x4000)
                throw MfcProtocol::Error("Digital Setpoint zero verification failed");
            state.stopStage = MfcStopStage::DigitalVerified;
            if (progress) progress();
            if (driver.readActiveSetpoint() != 0x4000)
                throw MfcProtocol::Error("目标已写入，但当前有效设定尚未归零");
            state.stopStage = MfcStopStage::ActiveVerified;
            state.targetConfirmed = true;
            state.lastControlConfirmation = QDateTime::currentDateTime();
            state.controlState = MfcControlState::ControlStopped;
            if (progress) progress();
            if (m_trace) m_trace(QStringLiteral("SETPOINT_ZERO_CONFIRMED address=%1").arg(state.config.address));
            if (m_trace) m_trace(QStringLiteral("ACTIVE_SETPOINT_ZERO_CONFIRMED address=%1").arg(state.config.address));
        } catch (const std::exception &e) {
            state.stopStage = MfcStopStage::Unconfirmed;
            state.stopFailure = QString::fromUtf8(e.what());
            state.controlState = MfcControlState::ControlDegraded;
            failures.append(QStringLiteral("%1(地址%2)：%3").arg(state.config.displayName)
                            .arg(state.config.address).arg(state.stopFailure));
            if (m_trace) m_trace(QStringLiteral("STOP_CONTROL_PARTIAL_FAILURE address=%1 error=%2")
                                 .arg(static_cast<int>(state.config.address)).arg(state.stopFailure));
            if (progress) progress();
        }
    }
    const bool complete = failures.isEmpty();
    m_controlActive = !complete;
    if (complete) {
        setControlState(MfcControlState::ControlStopped);
        if (m_trace) m_trace(QStringLiteral("STOP_CONTROL_COMPLETED"));
        return true;
    }
    // Preserve per-device success states; only the unconfirmed addresses are
    // degraded.  The caller keeps Stop Control available for another finite
    // operator-initiated attempt.
    m_controlFailure = failures.join(QStringLiteral("；"));
    if (errorMessage) *errorMessage = QStringLiteral("部分 MFC 停止控制未确认：") + m_controlFailure;
    return false;
}

MfcManager::MfcManager(QList<MfcDeviceConfig> devices, Settings settings,
                       SerialTransport::TraceSink trace)
    : m_settings(std::move(settings)), m_transport(trace), m_trace(std::move(trace))
{
    for (auto &config : devices) {
        MfcDeviceState state;
        if (config.logicalChannel <= 0) config.logicalChannel = m_devices.size() + 1;
        state.config = std::move(config);
        state.info.address = state.config.address;
        state.refreshCapabilities();
        m_devices.append(std::move(state));
    }
}

int MfcManager::onlineCount() const
{
    return static_cast<int>(std::count_if(m_devices.cbegin(), m_devices.cend(),
        [](const MfcDeviceState &state) { return state.config.enabled && state.communicationOnline; }));
}

bool MfcManager::allEnabledDevicesOffline() const
{
    bool found = false;
    for (const auto &state : m_devices) {
        if (!state.config.enabled) continue;
        found = true;
        if (state.communicationState != MfcCommunicationState::Fault) return false;
    }
    return found;
}

QString MfcManager::communicationNotice() const
{
    QList<int> faults;
    QList<int> warnings;
    for (const auto &state : m_devices) {
        if (!state.config.enabled) continue;
        if (state.communicationState == MfcCommunicationState::Fault)
            faults.append(state.config.address);
        else if (state.communicationState == MfcCommunicationState::Degraded
                 || state.communicationState == MfcCommunicationState::Recovering)
            warnings.append(state.config.address);
    }
    const auto addresses = [](const QList<int> &values) {
        QStringList text;
        for (const int address : values) text.append(QString::number(address));
        return text.join(QStringLiteral("、"));
    };
    if (!faults.isEmpty())
        return QStringLiteral("地址 %1 通信故障，正在自动重试").arg(addresses(faults));
    if (!warnings.isEmpty())
        return QStringLiteral("地址 %1 通信异常，正在自动重试").arg(addresses(warnings));
    return {};
}

void MfcManager::setMonitoringActive(bool active)
{
    if (active) m_transport.clearCancellation();
    else m_transport.requestCancel();
    for (auto &state : m_devices) state.monitoringActive = active;
}

void MfcManager::recordSuccess(MfcDeviceState &state, qint64 responseTimeMs)
{
    const MfcCommunicationState previousState = state.communicationState;
    const QString recoveryReason = state.lastError;
    state.responseTimeMs = responseTimeMs;
    state.totalResponseTimeMs += responseTimeMs;
    state.maximumResponseTimeMs = qMax(state.maximumResponseTimeMs,
                                       static_cast<double>(responseTimeMs));
    state.lastUpdateTime = state.lastSuccessTime = QDateTime::currentDateTime();
    state.communicationOnline = true;
    state.addressDetected = true;
    state.dataFresh = true;
    if (previousState == MfcCommunicationState::Unknown) {
        ++state.consecutiveSuccesses;
        state.communicationState = state.consecutiveSuccesses
                >= m_settings.communicationRecoverySuccessThreshold
            ? MfcCommunicationState::Normal : MfcCommunicationState::Recovering;
    } else if (previousState == MfcCommunicationState::Degraded
        || previousState == MfcCommunicationState::Fault
        || previousState == MfcCommunicationState::Recovering) {
        ++state.consecutiveSuccesses;
        state.communicationState = state.consecutiveSuccesses
                >= m_settings.communicationRecoverySuccessThreshold
            ? MfcCommunicationState::Normal : MfcCommunicationState::Recovering;
        if (state.communicationState == MfcCommunicationState::Normal) {
            state.consecutiveFailures = 0;
            state.firstFailureTime = {};
            if (m_trace) m_trace(QStringLiteral("MFC[%1] COMMUNICATION_RECOVERED at=%2 successes=%3 previous_error=%4")
                .arg(state.config.address).arg(state.lastSuccessTime.toString(Qt::ISODateWithMs))
                .arg(state.consecutiveSuccesses).arg(recoveryReason));
        }
    } else {
        state.communicationState = MfcCommunicationState::Normal;
        state.consecutiveFailures = 0;
        state.consecutiveSuccesses = 0;
        state.firstFailureTime = {};
    }
    state.linkState = MfcLinkState::Online;
    state.nextRetryTime = state.communicationState == MfcCommunicationState::Normal
        ? QDateTime{} : state.lastSuccessTime.addMSecs(1000);
    ++state.successCount;
    state.refreshCapabilities();
}

bool MfcManager::setAddressConfirmed(int address, bool confirmed)
{
    for (auto &state : m_devices) {
        if (state.config.address != address) continue;
        state.config.addressConfirmed = confirmed;
        state.refreshCapabilities();
        return true;
    }
    return false;
}

int MfcManager::enabledCount() const
{
    int count = 0;
    for (const auto &device : m_devices) if (device.config.enabled) ++count;
    return count;
}

void MfcManager::setExperimentLogging(bool enabled)
{
    m_experimentMode = enabled;
    m_transport.setErrorRawOnly(enabled);
    m_transport.setExperimentDiagnostics(enabled);
}

bool MfcManager::probeCurrentPort()
{
    bool any = false;
    for (auto &state : m_devices) {
        if (!state.config.enabled) continue;
        QElapsedTimer elapsed;
        elapsed.start();
        try {
            m_transport.setLogicalChannel(state.config.logicalChannel);
            CS200ADriver driver(m_transport, state.config.address, m_trace);
            driver.setTransactionOptions(m_settings.ackTimeoutMs, m_settings.responseTimeoutMs, 0);
            state.reading = driver.readFlow();
            recordSuccess(state, elapsed.elapsed());
            if (m_trace) m_trace(QStringLiteral("MFC[%1] PROBE READ_FLOW OK percent_fs=%2 response_ms=%3")
                .arg(state.config.address).arg(state.reading.flowPercent).arg(state.responseTimeMs));
            any = true;
        } catch (const std::exception &error) {
            state.linkState = state.addressDetected ? MfcLinkState::Offline : MfcLinkState::NotDiscovered;
            state.communicationOnline = false;
            state.lastError = QString::fromUtf8(error.what());
            state.lastUpdateTime = QDateTime::currentDateTime();
            state.nextRetryTime = state.lastUpdateTime.addMSecs(1000);
            state.refreshCapabilities();
        }
    }
    return any;
}

bool MfcManager::connectBus()
{
    disconnectBus();
    QList<SerialPortDescriptor> ports;
    if (!m_settings.serialPort.isEmpty() && m_settings.serialPort != QStringLiteral("auto")) {
        ports.append({m_settings.serialPort, QString(), QString(), 0, 0, QString()});
    } else {
        ports = DeviceDiscovery::candidates();
    }
    QList<qint32> baudRates{m_settings.baudRate};
    SerialPortDescriptor fallbackPort;
    bool haveOpenablePort = false;
    for (const auto &port : ports) {
        for (const auto baud : baudRates) {
            try {
                m_transport.open(port.device, baud);
                if (!haveOpenablePort) {
                    fallbackPort = port;
                    haveOpenablePort = true;
                }
                if (probeCurrentPort()) {
                    m_connectedBaud = baud;
                    // Initial connection only probes READ_FLOW.  Identity is
                    // read later, after an explicit Start Control request.
                    for (auto &state : m_devices) {
                        state.info.port = port.device;
                        state.info.portDescription = port.description;
                        state.info.portManufacturer = port.manufacturer;
                        state.info.usbSerialNumber = port.serialNumber;
                        state.info.vendorId = port.vendorId;
                        state.info.productId = port.productId;
                        state.info.baudRate = baud;
                        state.info.fullScale = state.config.fullScale;
                        state.info.fullScaleUnit = state.config.unit;
                        state.info.fullScaleReadFromDevice = false;
                        state.metadataAvailable = false;
                        const bool engineeringScale = state.info.fullScale > 0.0
                            && !state.info.fullScaleUnit.trimmed().isEmpty();
                        state.reading.flowValue = engineeringScale
                            ? state.reading.flowPercent / 100.0 * state.info.fullScale
                            : state.reading.flowPercent;
                        state.refreshCapabilities();
                    }
                    return true;
                }
            } catch (const std::exception &error) {
                if (m_trace) m_trace(QStringLiteral("OPEN/PROBE ERROR port=%1 baud=%2 %3")
                                     .arg(port.device).arg(baud).arg(QString::fromUtf8(error.what())));
            }
            m_transport.close();
        }
    }
    // Preserve the distinction between an open USB serial port and an MFC that
    // did not answer.  The service will periodically reprobe this openable port.
    if (haveOpenablePort) {
        try {
            m_transport.open(fallbackPort.device, m_settings.baudRate);
            m_connectedBaud = m_settings.baudRate;
            return true;
        } catch (const std::exception &) {}
    }
    return false;
}

bool MfcManager::connectBusForExperiment()
{
    if (isConnected()) return true;
    QList<SerialPortDescriptor> ports;
    if (!m_settings.serialPort.isEmpty() && m_settings.serialPort != QStringLiteral("auto"))
        ports.append({m_settings.serialPort, QString(), QString(), 0, 0, QString()});
    else
        ports = DeviceDiscovery::candidates();
    for (const auto &port : ports) {
        try {
            m_transport.open(port.device, m_settings.baudRate);
            m_connectedBaud = m_settings.baudRate;
            if (m_trace) m_trace(QStringLiteral("EXPERIMENT_SERIAL_OPEN port=%1 baud=%2 no_probe=true")
                                 .arg(port.device).arg(m_connectedBaud));
            return true;
        } catch (const std::exception &error) {
            if (m_trace) m_trace(QStringLiteral("EXPERIMENT_OPEN_ERROR port=%1 error=%2")
                                 .arg(port.device, QString::fromUtf8(error.what())));
        }
    }
    return false;
}

void MfcManager::disconnectBus()
{
    m_transport.close();
    m_connectedBaud = 0;
    for (auto &state : m_devices) {
        state.linkState = MfcLinkState::Offline;
        state.communicationOnline = false;
        state.addressDetected = false;
        state.monitoringActive = false;
        state.reading.communicationOk = false;
        state.refreshCapabilities();
    }
}

void MfcManager::recordFailure(MfcDeviceState &state, const std::exception &error)
{
    const MfcCommunicationState previousState = state.communicationState;
    ++state.errorCount;
    ++state.consecutiveFailures;
    state.consecutiveSuccesses = 0;
    state.lastUpdateTime = QDateTime::currentDateTime();
    state.lastError = QString::fromUtf8(error.what());
    if (m_controlActive) {
        // A previously accepted target may still be running inside the MFC,
        // but its actual flow is no longer known to software.
        state.controlState = MfcControlState::ControlDegraded;
        state.targetConfirmed = false;
    }
    state.nextRetryTime = state.lastUpdateTime.addMSecs(1000);
    if (!state.firstFailureTime.isValid()) state.firstFailureTime = state.lastUpdateTime;
    const bool sustainedFailure = state.firstFailureTime.msecsTo(state.lastUpdateTime)
        >= m_settings.sustainedFailureTimeoutMs;
    const bool hardFault = state.consecutiveFailures >= qMax(5, m_settings.offlineFailureThreshold)
        || sustainedFailure;
    const bool degraded = state.consecutiveFailures >= m_settings.communicationWarningFailureThreshold;
    if (hardFault) state.communicationState = MfcCommunicationState::Fault;
    else if (degraded) state.communicationState = MfcCommunicationState::Degraded;
    // A failed READ_FLOW never turns into a zero, but it is immediately not a
    // realtime value.  The reading fields retain the last legal sample for
    // display/history while this freshness flag prevents it being presented as
    // current process feedback.
    state.dataFresh = false;
    state.reading.communicationOk = false;
    if (state.communicationState == MfcCommunicationState::Fault)
        state.communicationOnline = false;
    if (dynamic_cast<const SerialTransport::Timeout *>(&error)) {
        ++state.timeoutCount;
        state.linkState = state.communicationState == MfcCommunicationState::Fault
            ? MfcLinkState::Offline : MfcLinkState::Timeout;
    } else if (dynamic_cast<const MfcProtocol::Error *>(&error)
               || dynamic_cast<const SerialTransport::ProtocolFailure *>(&error)) {
        ++state.protocolErrorCount;
        if (state.lastError.contains(QStringLiteral("checksum"), Qt::CaseInsensitive))
            ++state.checksumErrorCount;
        if (state.lastError.contains(QStringLiteral("service"), Qt::CaseInsensitive))
            ++state.serviceErrorCount;
        state.linkState = state.communicationState == MfcCommunicationState::Fault
            ? MfcLinkState::Offline : MfcLinkState::Error;
    } else if (dynamic_cast<const SerialTransport::Cancelled *>(&error)) {
        state.linkState = state.communicationOnline ? MfcLinkState::Online : MfcLinkState::Offline;
    } else {
        ++state.serialErrorCount;
        state.linkState = state.communicationState == MfcCommunicationState::Fault
            ? MfcLinkState::Offline : MfcLinkState::Error;
    }
    state.refreshCapabilities();
    if (m_trace && previousState != state.communicationState
        && (state.communicationState == MfcCommunicationState::Degraded
            || state.communicationState == MfcCommunicationState::Fault)) {
        m_trace(QStringLiteral("MFC[%1] COMMUNICATION_%2 at=%3 failures=%4 first_failure=%5 error=%6")
            .arg(state.config.address)
            .arg(state.communicationState == MfcCommunicationState::Fault ? QStringLiteral("FAULT")
                                                                        : QStringLiteral("ABNORMAL"))
            .arg(state.lastUpdateTime.toString(Qt::ISODateWithMs)).arg(state.consecutiveFailures)
            .arg(state.firstFailureTime.toString(Qt::ISODateWithMs)).arg(state.lastError));
    }
    if (m_trace) m_trace(QStringLiteral("MFC[%1] TRANSACTION FAIL failures=%2 timeout=%3 protocol=%4 error=%5")
        .arg(state.config.address).arg(state.errorCount).arg(state.timeoutCount)
        .arg(state.protocolErrorCount).arg(state.lastError));
}

bool MfcManager::pollDevice(int index, QString *errorMessage)
{
    auto &state = m_devices[index];
    QElapsedTimer elapsed;
    elapsed.start();
    try {
        m_transport.setLogicalChannel(state.config.logicalChannel);
        CS200ADriver driver(m_transport, state.config.address, m_trace);
        driver.setTransactionOptions(m_settings.ackTimeoutMs, m_settings.responseTimeoutMs,
                                     m_settings.retryCount);
        const double fullScale = state.config.fullScale;
        const QString flowUnit = state.config.unit;
        driver.setEngineeringScale(fullScale, flowUnit);
        const MfcCommunicationState previousCommunication = state.communicationState;
        MfcReading reading = driver.readFlow();
        const bool engineeringScale = !flowUnit.trimmed().isEmpty() && fullScale > 0.0;
        reading.flowValue = engineeringScale ? reading.flowPercent / 100.0 * fullScale
                                             : reading.flowPercent;
        // Monitoring is deliberately READ_FLOW-only. No MFC setpoint, valve,
        // mode, warning or metadata commands may be interleaved here.
        reading.communicationOk = true;
        state.reading = reading;
        recordSuccess(state, elapsed.elapsed());
        // Only after a lost/degraded address comes back do the bounded control
        // verification reads run. Stable control is READ_FLOW-only.
        if (m_controlActive && previousCommunication != MfcCommunicationState::Normal) {
            const auto cm = driver.readCurrentControlMode();
            const quint16 active = driver.readActiveSetpoint();
            const quint16 expected = MfcProtocol::encodeUfrac16(state.targetFlow / state.config.fullScale);
            state.targetConfirmed = cm == MfcControlMode::Digital && active == expected;
            state.lastControlConfirmation = QDateTime::currentDateTime();
            state.controlState = state.targetConfirmed ? MfcControlState::Controlling
                                                       : MfcControlState::ControlDegraded;
            if (!state.targetConfirmed) state.lastError = QStringLiteral("设备恢复通信，但控制状态与目标不一致");
        }
        if (m_trace && !m_experimentMode) m_trace(QStringLiteral("MFC[%1] READ_FLOW OK value=%2 percent_fs=%3 unit=%4 response_ms=%5")
            .arg(state.config.address).arg(reading.flowValue).arg(reading.flowPercent)
            .arg(state.config.unit.isEmpty() ? QStringLiteral("UNCONFIRMED") : state.config.unit)
            .arg(state.responseTimeMs));
        return true;
    } catch (const std::exception &error) {
        recordFailure(state, error);
        if (errorMessage) *errorMessage = QStringLiteral("地址 %1 通信异常，正在自动重试")
            .arg(state.config.address);
        return false;
    }
}

bool MfcManager::pollNext(QString *errorMessage)
{
    if (!isConnected() || enabledCount() == 0) return false;
    for (int checked = 0; checked < m_devices.size(); ++checked) {
        const int index = m_pollIndex++ % m_devices.size();
        // Background recovery owns non-normal addresses and limits them to
        // one attempt per deadline.  Normal round-robin never turns a lost
        // MFC into a rapid retry loop or delays the healthy devices.
        if (!m_devices[index].config.enabled
            || m_devices[index].communicationState != MfcCommunicationState::Normal
            || (m_devices[index].nextRetryTime.isValid()
                && m_devices[index].nextRetryTime > QDateTime::currentDateTime())) continue;
        return pollDevice(index, errorMessage);
    }
    return false;
}

bool MfcManager::pollRecoveryDue(QString *errorMessage)
{
    if (!isConnected() || enabledCount() == 0) return false;
    const QDateTime now = QDateTime::currentDateTime();
    for (int checked = 0; checked < m_devices.size(); ++checked) {
        const int index = m_recoveryIndex++ % m_devices.size();
        const auto &state = m_devices[index];
        if (!state.config.enabled || state.communicationState == MfcCommunicationState::Normal)
            continue;
        if (state.nextRetryTime.isValid() && state.nextRetryTime > now)
            continue;
        return pollDevice(index, errorMessage);
    }
    return false;
}

bool MfcManager::pollExperimentFlow(int address, QString *errorMessage)
{
    if (!isConnected() || address < 32 || address > 36) return false;
    for (int index = 0; index < m_devices.size(); ++index) {
        if (m_devices[index].config.address == address)
            return pollDevice(index, errorMessage);
    }
    if (errorMessage) *errorMessage = QStringLiteral("实验地址 %1 不在当前配置中").arg(address);
    return false;
}

QVariantMap MfcManager::lastTransactionDiagnostics() const
{
    return m_transport.diagnostics().value(QStringLiteral("lastTransaction")).toMap();
}

QVariantMap MfcManager::serialRuntimeConfiguration() const
{
    return m_transport.diagnostics().value(QStringLiteral("serialConfiguration")).toMap();
}

QList<int> MfcManager::scanConfiguredAddresses()
{
    QList<int> result;
    if (!isConnected()) return result;
    for (auto &state : m_devices) {
        if (!state.config.enabled) continue;
        QElapsedTimer elapsed;
        elapsed.start();
        try {
            m_transport.setLogicalChannel(state.config.logicalChannel);
            CS200ADriver driver(m_transport, state.config.address, m_trace);
            driver.setTransactionOptions(m_settings.ackTimeoutMs, m_settings.responseTimeoutMs, 0);
            const MfcReading reading = driver.readFlow();
            state.reading = reading;
            recordSuccess(state, elapsed.elapsed());
            result.append(state.config.address);
        } catch (const std::exception &error) {
            recordFailure(state, error);
        }
    }
    return result;
}

QVariantMap MfcManager::diagnostics() const
{
    QVariantList list;
    int online = 0;
    int communicationWarnings = 0;
    int communicationFaults = 0;
    for (const auto &state : m_devices) {
        list.append(state.toVariantMap());
        if (state.communicationOnline) ++online;
        if (state.communicationState == MfcCommunicationState::Degraded
            || state.communicationState == MfcCommunicationState::Recovering)
            ++communicationWarnings;
        else if (state.communicationState == MfcCommunicationState::Fault)
            ++communicationFaults;
    }
    const bool serialConnected = isConnected();
    const int deviceCount = enabledCount();
    const int health = !serialConnected ? 0 : online == 0 ? 1
        : communicationWarnings > 0 || communicationFaults > 0 || online < deviceCount ? 2 : 3;
    QVariantMap result{{"port", portName()}, {"baudRate", m_connectedBaud},
            {"serialConnected", serialConnected}, {"communicationOnline", online > 0},
            {"readOnlyMonitoring", !m_controlActive}, {"controlActive", m_controlActive},
            {"controlFailure", m_controlFailure}, {"communicationHealth", health},
            {"deviceCount", deviceCount}, {"onlineCount", online},
            {"communicationWarningCount", communicationWarnings},
            {"communicationFaultCount", communicationFaults}, {"devices", list}};
    const QVariantMap transaction = m_transport.diagnostics();
    for (auto it = transaction.cbegin(); it != transaction.cend(); ++it)
        result[QStringLiteral("transaction_") + it.key()] = it.value();
    return result;
}
