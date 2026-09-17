#include "SerialTransport.h"
#include "MfcProtocol.h"
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSerialPortInfo>
#include <atomic>

namespace {
QString hex(const QByteArray &data) { return QString::fromLatin1(data.toHex(' ').toUpper()); }
std::atomic<quint64> nextRequestId{1};
// At 19200/8N1 a 12-byte READ_FLOW response occupies about 6.25 ms on the
// wire.  Twenty milliseconds covers the observed FTDI batch gaps without
// adding latency to healthy transactions (the window is used only on error).
constexpr int RxQuietWindowMs = 20;
constexpr int RxRecoveryTimeoutMs = 250;
constexpr int MaxReasonableRxBacklog = 128; // > 3 maximum protocol frames.
constexpr int ConsecutiveErrorHardRecoveryThreshold = 3;
constexpr int RecoveryReadSliceMs = 5;
QString compactHex(const QByteArray &data)
{
    constexpr int limit = 256;
    if (data.size() <= limit) return hex(data);
    return QStringLiteral("%1 ... %2 (total=%3 bytes)")
        .arg(hex(data.first(limit / 2)), hex(data.last(limit / 2))).arg(data.size());
}
QString errorType(const QString &detail, const QString &result = QString())
{
    const QString text = detail.toLower();
    if (result == QStringLiteral("TIMEOUT") || text.contains(QStringLiteral("timeout"))) return QStringLiteral("timeout");
    if (text.contains(QStringLiteral("checksum"))) return QStringLiteral("checksum_error");
    if (text.contains(QStringLiteral("service"))) return QStringLiteral("invalid_service");
    if (text.contains(QStringLiteral("class"))) return QStringLiteral("invalid_class");
    if (text.contains(QStringLiteral("attribute"))) return QStringLiteral("invalid_attribute");
    if (text.contains(QStringLiteral("instance"))) return QStringLiteral("invalid_instance");
    if (text.contains(QStringLiteral("length")) || text.contains(QStringLiteral("short"))) return QStringLiteral("length_error");
    if (text.contains(QStringLiteral("terminator")) || text.contains(QStringLiteral("trailing"))) return QStringLiteral("trailing_byte_error");
    if (text.contains(QStringLiteral("ack byte")) || text.contains(QStringLiteral("unexpected"))) return QStringLiteral("unexpected_data");
    if (result == QStringLiteral("SERIAL_ERROR")) return QStringLiteral("serial_error");
    return QStringLiteral("other_protocol_error");
}
QString commandName(quint8 service)
{
    if (service == MfcProtocol::ReadService) return QStringLiteral("READ");
    if (service == MfcProtocol::WriteService) return QStringLiteral("WRITE");
    return QStringLiteral("UNKNOWN");
}
}

SerialTransport::SerialTransport(TraceSink trace) : m_trace(std::move(trace))
{
    m_monotonicClock.start();
}
SerialTransport::~SerialTransport() { close(); }

QString SerialTransport::cancelReasonName(CancelReason reason)
{
    switch (reason) {
    case CancelReason::MonitoringStop: return QStringLiteral("MONITORING_STOP");
    case CancelReason::DeviceDisconnect: return QStringLiteral("DEVICE_DISCONNECT");
    case CancelReason::ApplicationShutdown: return QStringLiteral("APPLICATION_SHUTDOWN");
    case CancelReason::ExperimentStop: return QStringLiteral("EXPERIMENT_STOP");
    case CancelReason::WorkerWatchdog: return QStringLiteral("WORKER_WATCHDOG");
    case CancelReason::Unspecified: return QStringLiteral("UNSPECIFIED");
    }
    return QStringLiteral("UNSPECIFIED");
}

void SerialTransport::recordPreCreateCancellation(const char *context)
{
    // No PendingRequest exists in this path.  Replace rather than augment
    // the previous snapshot so a metadata/UI consumer cannot mistake the
    // preceding successful transaction's bytes for this cancelled one.
    const QDateTime timestamp = QDateTime::currentDateTime();
    const auto reason = static_cast<CancelReason>(m_cancelReason.load());
    m_lastTransaction = {
        {"requestId", static_cast<qulonglong>(0)}, {"transactionCreated", false},
        {"result", QStringLiteral("CANCELLED")}, {"detail", QStringLiteral("transaction cancelled")},
        {"cancellationReason", cancelReasonName(reason)},
        {"cancellationContext", QString::fromLatin1(context)},
        {"txRaw", QString()}, {"ackRaw", QString()}, {"rxRaw", QString()},
        {"responseFrameRaw", QString()}, {"payloadRaw", QString()},
        {"responseAddress", -1}, {"responseDataLength", -1}, {"rxTotalLength", 0},
        {"checksumValid", false}, {"attemptCount", 0}, {"retryCount", 0},
        {"transactionState", stateName(m_state)},
        {"finishTimestamp", timestamp.toString(Qt::ISODateWithMs)},
        {"serialConfiguration", runtimeSerialConfiguration()}
    };
    m_lastResult = QStringLiteral("CANCELLED");
}

[[noreturn]] void SerialTransport::throwCancelled(const char *context)
{
    const auto reason = static_cast<CancelReason>(m_cancelReason.load());
    if (m_trace && m_pending) {
        const auto &pending = *m_pending;
        m_trace(QStringLiteral("[transport] REQUEST_CANCELLED request_id=%1 logical_channel=%2 address=%3 command=%4 "
                               "service=0x%5 class=0x%6 instance=%7 attribute=0x%8 state=%9 reason=%10 context=%11")
            .arg(pending.requestId).arg(pending.logicalChannel).arg(pending.protocolAddress)
            .arg(commandName(pending.service)).arg(pending.service, 2, 16, QLatin1Char('0'))
            .arg(pending.commandClass, 2, 16, QLatin1Char('0')).arg(pending.instance)
            .arg(pending.attribute, 2, 16, QLatin1Char('0')).arg(stateName(m_state))
            .arg(cancelReasonName(reason), QString::fromLatin1(context)));
    } else if (m_trace) {
        m_trace(QStringLiteral("[transport] REQUEST_CANCELLED request_id=NONE state=%1 reason=%2 context=%3")
            .arg(stateName(m_state), cancelReasonName(reason), QString::fromLatin1(context)));
    }
    if (!m_pending)
        recordPreCreateCancellation(context);
    throw Cancelled("transaction cancelled");
}

void SerialTransport::open(const QString &portName, qint32 baudRate)
{
    const bool reopening = !m_openedPortPath.isEmpty();
    close();
    m_parser.reset();
    m_pending.reset();
    m_rxHistory.clear();
    m_rxSequence = 0;
    clearCancellation();
    m_port.setPortName(portName);
    m_port.setBaudRate(baudRate);
    m_port.setDataBits(QSerialPort::Data8);
    m_port.setParity(QSerialPort::NoParity);
    m_port.setStopBits(QSerialPort::OneStop);
    m_port.setFlowControl(QSerialPort::NoFlowControl);
    if (!m_port.open(QIODevice::ReadWrite))
        throw Error(QStringLiteral("无法打开 %1：%2").arg(portName, m_port.errorString()).toStdString());
    m_port.clear(QSerialPort::Input);
    if (reopening) ++m_recoveryCounters.serialReopenCount;
    m_openedPortPath = QSerialPortInfo(m_port).systemLocation();
    if (m_openedPortPath.isEmpty()) m_openedPortPath = portName;
    if (m_trace) {
        const auto config = runtimeSerialConfiguration();
        if (reopening)
            m_trace(QStringLiteral("[transport] SERIAL_REOPEN port=%1 runtime_baud=%2")
                .arg(portName).arg(config.value("baudRate").toInt()));
        m_trace(QStringLiteral("[transport] OPEN port=%1 runtime_baud=%2 data_bits=%3 parity=%4 stop_bits=%5 flow_control=%6")
            .arg(portName).arg(config.value("baudRate").toInt()).arg(config.value("dataBits").toString(),
                config.value("parity").toString(), config.value("stopBits").toString(),
                config.value("flowControl").toString()));
    }
}

void SerialTransport::close()
{
    if (m_port.isOpen()) {
        const QString name = m_port.portName();
        m_port.close();
        if (m_trace) m_trace(QStringLiteral("[transport] CLOSE port=%1 pending=%2 state=%3")
                             .arg(name).arg(m_pending.has_value()).arg(stateName(m_state)));
    }
    m_parser.reset();
    m_pending.reset();
}

bool SerialTransport::isOpen() const { return m_port.isOpen(); }
QString SerialTransport::portName() const { return m_port.portName(); }

bool SerialTransport::physicalPortExists() const
{
    return !m_openedPortPath.isEmpty() && QFileInfo::exists(m_openedPortPath);
}

qint64 SerialTransport::write(const QByteArray &bytes)
{
    if (!isOpen()) throw Error("serial port is not open");
    const qint64 written = m_port.write(bytes);
    if (written != bytes.size())
        throw Error(QStringLiteral("串口写入失败：%1").arg(m_port.errorString()).toStdString());
    QElapsedTimer timer;
    timer.start();
    while (m_port.bytesToWrite() > 0) {
        if (m_cancelRequested.load()) throwCancelled("WRITE_WAIT_FOR_BYTES_WRITTEN");
        const int remaining = 100 - static_cast<int>(timer.elapsed());
        if (remaining <= 0 || !m_port.waitForBytesWritten(qMin(10, remaining))) {
            if (m_port.error() != QSerialPort::NoError && m_port.error() != QSerialPort::TimeoutError)
                throw Error(QStringLiteral("串口写入失败：%1").arg(m_port.errorString()).toStdString());
            if (remaining <= 0) throw Timeout("serial write timeout");
        }
    }
    if (m_trace && !m_errorRawOnly) m_trace(QStringLiteral("[transport] TX_RAW bytes=%1").arg(hex(bytes)));
    return written;
}

QByteArray SerialTransport::read(qint64 maximum, int timeoutMs)
{
    if (!isOpen()) throw Error("serial port is not open");
    QElapsedTimer timer;
    timer.start();
    while (m_port.bytesAvailable() == 0) {
        if (m_cancelRequested.load()) throwCancelled("READ_WAIT_FOR_READY_READ");
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) throw Timeout("serial read timeout");
        if (!m_port.waitForReadyRead(qMin(10, remaining))
            && m_port.error() != QSerialPort::NoError
            && m_port.error() != QSerialPort::TimeoutError) {
            throw Error(QStringLiteral("串口读取失败：%1").arg(m_port.errorString()).toStdString());
        }
    }
    const QByteArray bytes = m_port.read(maximum);
    noteRxBytes(bytes);
    return bytes;
}

void SerialTransport::noteRxBytes(const QByteArray &bytes)
{
    if (bytes.isEmpty()) return;
    const QDateTime timestamp = QDateTime::currentDateTime();
    if (!m_firstRxTimestamp.isValid()) m_firstRxTimestamp = timestamp;
    m_lastRxTimestamp = timestamp;
    const quint64 firstSequence = m_rxSequence;
    m_rxSequence += static_cast<quint64>(bytes.size());
    m_rxHistory += bytes;
    if (m_rxHistory.size() > 512) m_rxHistory.remove(0, m_rxHistory.size() - 512);
    if (m_pending) {
        m_lastRx += bytes;
        m_rxBatches.append(QVariantMap{{"firstSequence", static_cast<qulonglong>(firstSequence)},
            {"lastSequence", static_cast<qulonglong>(m_rxSequence - 1)},
            {"timestamp", timestamp.toString(Qt::ISODateWithMs)}, {"raw", hex(bytes)}});
    }
}

QVariantMap SerialTransport::runtimeSerialConfiguration() const
{
    const QString parity = m_port.parity() == QSerialPort::NoParity ? QStringLiteral("NONE")
        : m_port.parity() == QSerialPort::EvenParity ? QStringLiteral("EVEN")
        : m_port.parity() == QSerialPort::OddParity ? QStringLiteral("ODD") : QStringLiteral("OTHER");
    const QString stopBits = m_port.stopBits() == QSerialPort::OneStop ? QStringLiteral("1")
        : m_port.stopBits() == QSerialPort::TwoStop ? QStringLiteral("2") : QStringLiteral("OTHER");
    const QString flow = m_port.flowControl() == QSerialPort::NoFlowControl ? QStringLiteral("NONE")
        : m_port.flowControl() == QSerialPort::HardwareControl ? QStringLiteral("HARDWARE")
        : QStringLiteral("SOFTWARE");
    return {{"port", m_port.portName()}, {"baudRate", m_port.baudRate()},
        {"dataBits", m_port.dataBits() == QSerialPort::Data8 ? QStringLiteral("8") : QString::number(m_port.dataBits())},
        {"parity", parity}, {"stopBits", stopBits}, {"flowControl", flow}};
}

QByteArray SerialTransport::readExact(qint64 count, int timeoutMs)
{
    QByteArray result;
    QElapsedTimer timer;
    timer.start();
    while (result.size() < count) {
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) throw Timeout("serial response timeout");
        result += read(count - result.size(), remaining);
    }
    return result;
}

QString SerialTransport::stateName(TransactionState state)
{
    switch (state) {
    case TransactionState::Idle: return QStringLiteral("IDLE");
    case TransactionState::PreTxCheck: return QStringLiteral("PRE_TX_CHECK");
    case TransactionState::Tx: return QStringLiteral("TX");
    case TransactionState::WaitAck: return QStringLiteral("WAIT_ACK");
    case TransactionState::ReadFrame: return QStringLiteral("READ_FRAME");
    case TransactionState::Validate: return QStringLiteral("VALIDATE");
    case TransactionState::Success: return QStringLiteral("SUCCESS");
    case TransactionState::Recovery: return QStringLiteral("BUS_RECOVERY");
    case TransactionState::Retry: return QStringLiteral("RETRY");
    case TransactionState::SerialDegraded: return QStringLiteral("SERIAL_DEGRADED");
    case TransactionState::Failed: return QStringLiteral("FAILED");
    }
    return QStringLiteral("UNKNOWN");
}

void SerialTransport::recordProtocolFailure(const QString &reason)
{
    ++m_consecutiveProtocolErrors;
    m_recoveryCounters.consecutiveProtocolErrorMax = qMax<quint64>(
        m_recoveryCounters.consecutiveProtocolErrorMax, m_consecutiveProtocolErrors);
    if (reason.contains(QStringLiteral("ACK"), Qt::CaseInsensitive)) {
        ++m_consecutiveInvalidAcks;
        m_recoveryCounters.consecutiveInvalidAckMax = qMax<quint64>(
            m_recoveryCounters.consecutiveInvalidAckMax, m_consecutiveInvalidAcks);
    } else {
        m_consecutiveInvalidAcks = 0;
    }
}

bool SerialTransport::recoverBus(const QString &reason, const PendingRequest &pending, int attempt,
                                 bool hardRecovery)
{
    m_state = TransactionState::Recovery;
    ++m_recoveryCounters.busRecoveryCount;
    const int pendingAtStart = static_cast<int>(m_port.bytesAvailable());
    if (pendingAtStart > MaxReasonableRxBacklog) hardRecovery = true;
    if (hardRecovery) ++m_recoveryCounters.hardRecoveryCount;
    QByteArray discarded;
    QElapsedTimer timer;
    timer.start();
    qint64 lastRxMs = 0;
    bool sawBytes = false;
    bool success = false;

    // A reset alone cannot establish a byte-stream boundary.  Keep draining
    // until no new batch has appeared for a full quiet window.
    while (timer.elapsed() < RxRecoveryTimeoutMs) {
        if (m_cancelRequested.load()) throwCancelled("BUS_RECOVERY_WAIT");
        const qint64 available = m_port.bytesAvailable();
        if (available > 0) {
            const QByteArray bytes = m_port.read(available);
            noteRxBytes(bytes);
            discarded += bytes;
            sawBytes = true;
            lastRxMs = timer.elapsed();
            continue;
        }
        const qint64 quietMs = timer.elapsed() - lastRxMs;
        if (quietMs >= RxQuietWindowMs) {
            success = true;
            break;
        }
        const int waitMs = qMax(1, qMin(RecoveryReadSliceMs,
            RxQuietWindowMs - static_cast<int>(quietMs)));
        if (!m_port.waitForReadyRead(waitMs)
            && m_port.error() != QSerialPort::NoError
            && m_port.error() != QSerialPort::TimeoutError) {
            break;
        }
    }

    const qint64 quietWaitMs = timer.elapsed();
    ++m_recoveryCounters.rxQuietWaitCount;
    m_recoveryCounters.rxQuietWaitTotalMs += static_cast<quint64>(quietWaitMs);
    m_recoveryCounters.rxQuietWaitMaxMs = qMax<quint64>(m_recoveryCounters.rxQuietWaitMaxMs, quietWaitMs);
    if (sawBytes) {
        ++m_recoveryCounters.rxDrainCount;
        m_recoveryCounters.rxDiscardedBytes += static_cast<quint64>(discarded.size());
        m_recoveryCounters.streamResyncCount++;
        m_recoveryCounters.streamResyncDiscardedBytes += static_cast<quint64>(discarded.size());
    }
    if (!success && hardRecovery && isOpen()) {
        // Last-resort bounded cleanup for a continuously noisy input.  It is
        // intentionally not the normal recovery path and is still followed by
        // a failed transaction rather than blindly transmitting again.
        m_port.clear(QSerialPort::Input);
    }
    m_parser.reset();
    const int pendingAtEnd = static_cast<int>(m_port.bytesAvailable());
    if (success && pendingAtEnd == 0) {
        ++m_recoveryCounters.busRecoverySuccessCount;
        m_consecutiveRecoveryFailures = 0;
        m_serialDegraded = false;
    }
    else {
        success = false;
        ++m_recoveryCounters.busRecoveryFailureCount;
        if (++m_consecutiveRecoveryFailures >= 2) m_serialDegraded = true;
    }
    const QVariantMap event{{"event", "bus_recovery"}, {"reason", reason},
        {"address", pending.protocolAddress}, {"requestId", static_cast<qulonglong>(pending.requestId)},
        {"attempt", attempt}, {"rxPendingAtStart", pendingAtStart},
        {"discardedBytes", discarded.size()}, {"discardedRaw", compactHex(discarded)},
        {"quietWaitMs", quietWaitMs}, {"recoveryDurationMs", timer.elapsed()},
        {"rxPendingAtEnd", pendingAtEnd}, {"hardRecovery", hardRecovery},
        {"result", success ? "SUCCESS" : "FAILED"}};
    m_recoveryEvents.append(event);
    if (m_trace) m_trace(QStringLiteral("[transport] BUS_RECOVERY request_id=%1 logical_channel=%2 address=%3 attempt=%4 reason=%5 pending_start=%6 "
                                        "discarded=%7 quiet_ms=%8 pending_end=%9 result=%10")
        .arg(pending.requestId).arg(pending.logicalChannel).arg(pending.protocolAddress).arg(attempt).arg(reason)
        .arg(pendingAtStart).arg(discarded.size()).arg(quietWaitMs).arg(pendingAtEnd)
        .arg(success ? QStringLiteral("SUCCESS") : QStringLiteral("FAILED")));
    return success;
}

bool SerialTransport::verifyRxClean(const PendingRequest &pending, int attempt)
{
    m_state = TransactionState::PreTxCheck;
    if (m_serialDegraded) {
        // No normal command is allowed while the transport has recently failed
        // to establish idle twice.  A successful bounded hard recovery is the
        // only transition out of SERIAL_DEGRADED.
        m_state = TransactionState::SerialDegraded;
        return recoverBus(QStringLiteral("SERIAL_DEGRADED"), pending, attempt, true);
    }
    const int pendingBytes = static_cast<int>(m_port.bytesAvailable());
    const bool parserDirty = m_parser.state() != MfcProtocol::ParserState::WaitStart00;
    if (pendingBytes == 0 && !parserDirty) return true;
    if (pendingBytes > 0) {
        ++m_recoveryCounters.rxPendingBeforeTxCount;
        m_recoveryCounters.rxPendingBeforeTxMax = qMax<quint64>(
            m_recoveryCounters.rxPendingBeforeTxMax, pendingBytes);
    }
    const bool hard = pendingBytes > MaxReasonableRxBacklog;
    const QString reason = hard ? QStringLiteral("RX_BACKLOG")
        : parserDirty ? QStringLiteral("PARSER_PARTIAL_FRAME")
                      : QStringLiteral("RX_PENDING_BEFORE_TX");
    if (recoverBus(reason, pending, attempt, hard)) return true;
    // A stream that never becomes quiet is degraded.  One bounded hard pass
    // stops normal TX; it never turns into unbounded retransmission.
    return hard ? false : recoverBus(QStringLiteral("RECOVERY_TIMEOUT"), pending, attempt, true);
}

void SerialTransport::flush()
{
    m_parser.reset();
    if (isOpen()) m_port.clear(QSerialPort::Input);
}

QByteArray SerialTransport::readResponseFrame(int timeoutMs, const PendingRequest &pending,
                                               QByteArray *allReceived)
{
    QElapsedTimer timer;
    timer.start();
    QString lastProtocolError;
    for (;;) {
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) {
            if (!lastProtocolError.isEmpty()) throw ProtocolFailure(lastProtocolError.toStdString());
            throw Timeout("serial response timeout");
        }
        QByteArray bytes;
        const QByteArray historyBeforeRead = m_rxHistory;
        try {
            bytes = read(261, remaining);
        } catch (const Timeout &) {
            if (!lastProtocolError.isEmpty())
                throw ProtocolFailure(lastProtocolError.toStdString());
            throw;
        }
        if (allReceived) *allReceived += bytes;
        if (m_trace && !m_errorRawOnly) m_trace(QStringLiteral("[transport] RX_BYTES request_id=%1 logical_channel=%2 address=%3 bytes=%4 parser=%5")
            .arg(pending.requestId).arg(pending.logicalChannel).arg(pending.protocolAddress)
            .arg(hex(bytes), m_parser.stateName()));
        const auto events = m_parser.feed(bytes);
        for (const auto &event : events) {
            if (event.type == MfcProtocol::ParseEvent::Type::Error) {
                lastProtocolError = event.error;
                m_lastProtocolError = event.error;
                m_lastParserState = MfcProtocol::parserStateName(event.state);
                m_lastChecksumExpected = event.checksumExpected;
                m_lastChecksumReceived = event.checksumReceived;
                if (event.bytes.size() > 2) m_lastReceivedService = static_cast<quint8>(event.bytes[2]);
                if (event.bytes.size() > 4) m_lastReceivedClass = static_cast<quint8>(event.bytes[4]);
                if (event.bytes.size() > 5) m_lastReceivedInstance = static_cast<quint8>(event.bytes[5]);
                if (event.bytes.size() > 6) m_lastReceivedAttribute = static_cast<quint8>(event.bytes[6]);
                const int payloadExpected = event.length >= 3 ? event.length - 3 : -1;
                const int payloadReceived = event.bytes.size() > 7
                    ? qMin(qMax(0, event.bytes.size() - 7), qMax(0, payloadExpected)) : 0;
                m_attemptErrors.append(QVariantMap{{"attempt", m_attemptCount},
                    {"errorType", errorType(event.error)}, {"errorMessage", event.error},
                    {"parserState", m_lastParserState}, {"length", event.length},
                    {"payloadExpected", payloadExpected}, {"payloadReceived", payloadReceived},
                    {"currentFrame", hex(event.bytes)}, {"rxHistoryBeforeError", hex(historyBeforeRead)},
                    {"rxHistoryFirstSequence", static_cast<qulonglong>(m_rxSequence - historyBeforeRead.size() - bytes.size())},
                    {"rxOffsetAfterError", m_lastRx.size() - bytes.size() + event.inputOffset + 1},
                    {"expectedService", pending.service},
                    {"receivedService", m_lastReceivedService}, {"expectedClass", pending.commandClass},
                    {"receivedClass", m_lastReceivedClass}, {"expectedInstance", pending.instance},
                    {"receivedInstance", m_lastReceivedInstance}, {"expectedAttribute", pending.attribute},
                    {"receivedAttribute", m_lastReceivedAttribute}, {"expectedChecksum", event.checksumExpected},
                    {"receivedChecksum", event.checksumReceived}, {"txRaw", hex(pending.tx)},
                    {"errorTimestamp", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)}});
                if (m_trace && !m_errorRawOnly) m_trace(QStringLiteral("#%1 REQUEST_PROTOCOL_ERROR logical_channel=%2 address=%3 state=%4 len=%5 "
                                                    "checksum_expected=%6 checksum_received=%7 tx=%8 rx=%9")
                    .arg(pending.requestId).arg(pending.logicalChannel).arg(pending.protocolAddress)
                    .arg(MfcProtocol::parserStateName(event.state)).arg(event.length)
                    .arg(event.checksumExpected).arg(event.checksumReceived)
                    .arg(hex(pending.tx), hex(event.bytes)));
                // Do not continue parsing this attempt after a corrupt frame.
                // Its unread tail may still arrive in later USB batches; the
                // transaction state machine will drain it before retransmit.
                throw ProtocolFailure(event.error.toStdString());
            }
            if (m_trace && !m_errorRawOnly) m_trace(QStringLiteral("[transport] RX_FRAME_COMPLETE request_id=%1 logical_channel=%2 address=%3 frame=%4 checksum_expected=%5 checksum_received=%6")
                .arg(pending.requestId).arg(pending.logicalChannel).arg(pending.protocolAddress).arg(hex(event.bytes))
                .arg(event.checksumExpected).arg(event.checksumReceived));
            m_frameCompleteTimestamp = QDateTime::currentDateTime();
            if (event.bytes.size() > 2) m_lastReceivedService = static_cast<quint8>(event.bytes[2]);
            if (event.bytes.size() > 4) m_lastReceivedClass = static_cast<quint8>(event.bytes[4]);
            if (event.bytes.size() > 5) m_lastReceivedInstance = static_cast<quint8>(event.bytes[5]);
            if (event.bytes.size() > 6) m_lastReceivedAttribute = static_cast<quint8>(event.bytes[6]);
            m_lastChecksumExpected = event.checksumExpected;
            m_lastChecksumReceived = event.checksumReceived;
            try {
                m_state = TransactionState::Validate;
                (void)MfcProtocol::parseResponse(event.bytes, pending.commandClass,
                                                 pending.instance, pending.attribute);
                // feed() has already consumed the whole serial chunk.  A
                // valid frame followed by any extra byte is still a broken
                // transaction boundary, even if that byte looks harmless.
                if (event.inputOffset >= 0 && event.inputOffset + 1 < bytes.size())
                    throw ProtocolFailure("trailing bytes after response frame");
                return event.bytes;
            } catch (const MfcProtocol::Error &error) {
                lastProtocolError = QString::fromUtf8(error.what());
                m_lastProtocolError = lastProtocolError;
                m_lastParserState = QStringLiteral("FRAME_VALIDATION");
                if (event.bytes.size() > 2) m_lastReceivedService = static_cast<quint8>(event.bytes[2]);
                if (event.bytes.size() > 4) m_lastReceivedClass = static_cast<quint8>(event.bytes[4]);
                if (event.bytes.size() > 5) m_lastReceivedInstance = static_cast<quint8>(event.bytes[5]);
                if (event.bytes.size() > 6) m_lastReceivedAttribute = static_cast<quint8>(event.bytes[6]);
                m_attemptErrors.append(QVariantMap{{"attempt", m_attemptCount},
                    {"errorType", errorType(lastProtocolError)}, {"errorMessage", lastProtocolError},
                    {"parserState", m_lastParserState}, {"length", event.length},
                    {"payloadExpected", event.length >= 3 ? event.length - 3 : -1},
                    {"payloadReceived", event.length >= 3 ? qMin(event.length - 3, qMax(0, event.bytes.size() - 7)) : 0},
                    {"currentFrame", hex(event.bytes)}, {"rxHistoryBeforeError", hex(historyBeforeRead)},
                    {"rxHistoryFirstSequence", static_cast<qulonglong>(m_rxSequence - historyBeforeRead.size() - bytes.size())},
                    {"rxOffsetAfterError", m_lastRx.size() - bytes.size() + event.inputOffset + 1},
                    {"expectedService", pending.service},
                    {"receivedService", m_lastReceivedService}, {"expectedClass", pending.commandClass},
                    {"receivedClass", m_lastReceivedClass}, {"expectedInstance", pending.instance},
                    {"receivedInstance", m_lastReceivedInstance}, {"expectedAttribute", pending.attribute},
                    {"receivedAttribute", m_lastReceivedAttribute}, {"expectedChecksum", event.checksumExpected},
                    {"receivedChecksum", event.checksumReceived}, {"txRaw", hex(pending.tx)},
                    {"errorTimestamp", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)}});
                if (m_trace && !m_errorRawOnly) m_trace(QStringLiteral("#%1 REQUEST_PROTOCOL_ERROR address=%2 "
                                                    "expected=%3/0x%4/0x%5 received=%6 error=%7 tx=%8 frame=%9")
                    .arg(pending.requestId).arg(pending.protocolAddress)
                    .arg(pending.instance).arg(pending.commandClass, 2, 16, QLatin1Char('0'))
                    .arg(pending.attribute, 2, 16, QLatin1Char('0'))
                    .arg(event.bytes.size() >= 7
                        ? QStringLiteral("%1/0x%2/0x%3")
                            .arg(static_cast<quint8>(event.bytes[5]))
                            .arg(static_cast<quint8>(event.bytes[4]), 2, 16, QLatin1Char('0'))
                            .arg(static_cast<quint8>(event.bytes[6]), 2, 16, QLatin1Char('0'))
                        : QStringLiteral("short"))
                    .arg(lastProtocolError).arg(hex(pending.tx), hex(event.bytes)));
                throw ProtocolFailure(lastProtocolError.toStdString());
            }
        }
    }
}

void SerialTransport::finishCurrentTransaction(const QString &result, const QString &detail,
                                                qint64 elapsedMs)
{
    if (!m_pending) return;
    const auto pending = *m_pending;
    const QDateTime finishedAt = QDateTime::currentDateTime();
    const qint64 finishedNs = m_monotonicClock.nsecsElapsed();
    const double gapMs = m_lastFinishNs >= 0 && m_currentTxNs >= 0
        ? (m_currentTxNs - m_lastFinishNs) / 1000000.0 : -1.0;
    for (int i = 0; i < m_attemptErrors.size(); ++i) {
        QVariantMap event = m_attemptErrors[i].toMap();
        const int offset = event.take("rxOffsetAfterError").toInt();
        event["rxAfterError"] = hex(m_lastRx.mid(qBound(0, offset, m_lastRx.size())));
        event["allRxRaw"] = hex(m_lastRx);
        event["rxHistoryAtFinish"] = hex(m_rxHistory);
        event["rxHistoryFinishFirstSequence"] = static_cast<qulonglong>(m_rxSequence - m_rxHistory.size());
        event["rxHistoryFinishLastSequence"] = m_rxHistory.isEmpty()
            ? QVariant{} : QVariant::fromValue(static_cast<qulonglong>(m_rxSequence - 1));
        m_attemptErrors[i] = event;
    }
    const double txToFirstRxMs = m_firstRxTimestamp.isValid()
        ? m_currentTxTimestamp.msecsTo(m_firstRxTimestamp) : -1.0;
    const double txToFrameCompleteMs = m_frameCompleteTimestamp.isValid()
        ? m_currentTxTimestamp.msecsTo(m_frameCompleteTimestamp) : -1.0;
    const int responseDataLength = m_lastResponseFrame.size() >= 4
        ? static_cast<quint8>(m_lastResponseFrame[3]) : -1;
    const QByteArray responsePayload = responseDataLength >= 3
        ? m_lastResponseFrame.mid(7, responseDataLength - 3) : QByteArray{};
    // CS200 read responses conventionally carry 0x00 here.  Keep it as raw
    // protocol evidence only; it must never be used to select a device.
    const int responseAddress = m_lastResponseFrame.isEmpty()
        ? -1 : static_cast<quint8>(m_lastResponseFrame[0]);
    m_lastTransaction = {
        {"requestId", static_cast<qulonglong>(pending.requestId)},
        {"origin", m_transactionOrigin},
        {"previousRequestId", static_cast<qulonglong>(m_previousRequestId)},
        {"address", pending.protocolAddress}, {"service", pending.service},
        {"commandClass", pending.commandClass}, {"instance", pending.instance},
        {"attribute", pending.attribute}, {"result", result}, {"detail", detail},
        {"elapsedMs", elapsedMs}, {"attemptCount", m_attemptCount},
        {"retryCount", qMax(0, m_attemptCount - 1)},
        {"retrySuccess", result == QStringLiteral("SUCCESS") && m_attemptCount > 1},
        {"previousFinishToNextTxMs", gapMs}, {"txRaw", hex(pending.tx)},
        {"ackRaw", hex(m_lastAck)}, {"rxRaw", hex(m_lastRx)},
        {"responseFrameRaw", hex(m_lastResponseFrame)},
        {"responseAddress", responseAddress},
        {"responseDataLength", responseDataLength}, {"payloadRaw", hex(responsePayload)},
        {"checksumValid", !m_lastResponseFrame.isEmpty()},
        {"rxTotalLength", m_lastRx.size()},
        {"afterResponseRxBufferSize", static_cast<int>(m_port.bytesAvailable())},
        {"parserState", m_lastParserState.isEmpty() ? m_parser.stateName() : m_lastParserState},
        {"parserStateBeforeTx", m_parserStateBeforeTx},
        {"rxBytesAvailableBeforeTx", m_rxBytesAvailableBeforeTx}, {"preTxRxPeek", hex(m_preTxRxPeek)},
        {"lateRxAfterFinishCount", m_lateRxAfterFinishCount}, {"lateRxAfterFinishBytes", m_lateRxAfterFinishBytes},
        {"rxHistoryBeforeRequest", hex(m_historyAtRequestStart)}, {"rxHistoryAtFinish", hex(m_rxHistory)},
        {"rxBatches", m_rxBatches}, {"attempts", m_attemptRecords}, {"attemptErrors", m_attemptErrors},
        {"recoveryEvents", m_recoveryEvents}, {"transactionState", stateName(m_state)},
        {"serialDegraded", m_serialDegraded}, {"consecutiveRecoveryFailures", m_consecutiveRecoveryFailures},
        {"busRecoveryCount", static_cast<qulonglong>(m_recoveryCounters.busRecoveryCount)},
        {"busRecoverySuccessCount", static_cast<qulonglong>(m_recoveryCounters.busRecoverySuccessCount)},
        {"busRecoveryFailureCount", static_cast<qulonglong>(m_recoveryCounters.busRecoveryFailureCount)},
        {"rxDrainCount", static_cast<qulonglong>(m_recoveryCounters.rxDrainCount)},
        {"rxDiscardedBytes", static_cast<qulonglong>(m_recoveryCounters.rxDiscardedBytes)},
        {"rxPendingBeforeTxCount", static_cast<qulonglong>(m_recoveryCounters.rxPendingBeforeTxCount)},
        {"rxPendingBeforeTxMax", static_cast<qulonglong>(m_recoveryCounters.rxPendingBeforeTxMax)},
        {"rxQuietWaitCount", static_cast<qulonglong>(m_recoveryCounters.rxQuietWaitCount)},
        {"rxQuietWaitTotalMs", static_cast<qulonglong>(m_recoveryCounters.rxQuietWaitTotalMs)},
        {"rxQuietWaitMaxMs", static_cast<qulonglong>(m_recoveryCounters.rxQuietWaitMaxMs)},
        {"streamResyncCount", static_cast<qulonglong>(m_recoveryCounters.streamResyncCount)},
        {"streamResyncDiscardedBytes", static_cast<qulonglong>(m_recoveryCounters.streamResyncDiscardedBytes)},
        {"consecutiveProtocolErrorMax", static_cast<qulonglong>(m_recoveryCounters.consecutiveProtocolErrorMax)},
        {"consecutiveInvalidAckMax", static_cast<qulonglong>(m_recoveryCounters.consecutiveInvalidAckMax)},
        {"hardRecoveryCount", static_cast<qulonglong>(m_recoveryCounters.hardRecoveryCount)},
        {"serialReopenCount", static_cast<qulonglong>(m_recoveryCounters.serialReopenCount)},
        {"expectedService", pending.service}, {"receivedService", m_lastReceivedService},
        {"expectedClass", pending.commandClass}, {"receivedClass", m_lastReceivedClass},
        {"expectedInstance", pending.instance}, {"receivedInstance", m_lastReceivedInstance},
        {"expectedAttribute", pending.attribute}, {"receivedAttribute", m_lastReceivedAttribute},
        {"expectedChecksum", m_lastChecksumExpected}, {"receivedChecksum", m_lastChecksumReceived},
        {"previousFinishTimestamp", m_previousFinishTimestamp.toString(Qt::ISODateWithMs)},
        {"currentTxTimestamp", m_currentTxTimestamp.toString(Qt::ISODateWithMs)},
        {"txTimestamp", m_currentTxTimestamp.toString(Qt::ISODateWithMs)},
        {"firstRxTimestamp", m_firstRxTimestamp.toString(Qt::ISODateWithMs)},
        {"lastRxTimestamp", m_lastRxTimestamp.toString(Qt::ISODateWithMs)},
        {"frameCompleteTimestamp", m_frameCompleteTimestamp.toString(Qt::ISODateWithMs)},
        {"finishTimestamp", finishedAt.toString(Qt::ISODateWithMs)},
        {"txToFirstRxMs", txToFirstRxMs}, {"txToFrameCompleteMs", txToFrameCompleteMs},
        {"serialConfiguration", runtimeSerialConfiguration()}
    };
    if (m_trace) m_trace(QStringLiteral("[transport] REQUEST_FINISHED request_id=%1 result=%2 logical_channel=%3 address=%4 service=0x%5 class=0x%6 instance=%7 attribute=0x%8 state=%9 elapsed_ms=%10 "
                                        "previous_request_id=%11 next_request_id=%12 previousFinishToNextTxMs=%13 retry_count=%14 checksum_expected=%15 checksum_received=%16 detail=%17")
        .arg(pending.requestId).arg(result).arg(pending.logicalChannel)
        .arg(pending.protocolAddress).arg(pending.service, 2, 16, QLatin1Char('0'))
        .arg(pending.commandClass, 2, 16, QLatin1Char('0')).arg(pending.instance)
        .arg(pending.attribute, 2, 16, QLatin1Char('0')).arg(stateName(m_state)).arg(elapsedMs).arg(m_previousRequestId)
        .arg(nextRequestId.load()).arg(gapMs < 0 ? QStringLiteral("N/A") : QString::number(gapMs, 'f', 3))
        .arg(qMax(0, m_attemptCount - 1)).arg(m_lastChecksumExpected).arg(m_lastChecksumReceived).arg(detail));
    if (m_trace && m_errorRawOnly && !m_attemptErrors.isEmpty()) {
        const QString errors = QString::fromUtf8(
            QJsonDocument::fromVariant(m_attemptErrors).toJson(QJsonDocument::Compact));
        m_trace(QStringLiteral("EXPERIMENT_ERROR_RAW requestId=%1 address=%2 final_result=%3 TX_RAW=%4 RX_RAW=%5 errors=%6")
            .arg(pending.requestId).arg(pending.protocolAddress).arg(result)
            .arg(hex(pending.tx), hex(m_lastRx), errors));
    }
    m_previousRequestId = pending.requestId;
    m_lastFinishedRequestId = pending.requestId;
    m_lastResult = result;
    m_lastFinishNs = finishedNs;
    m_previousFinishTimestamp = finishedAt;
    m_pending.reset();
    // Every completed transaction leaves a clean parser boundary.  Physical RX
    // cleanup is handled by BUS_RECOVERY, never inferred from this reset.
    m_parser.reset();
    m_state = TransactionState::Idle;
}

QByteArray SerialTransport::transaction(const QByteArray &request, int ackTimeoutMs,
                                        int responseTimeoutMs, int retries)
{
    retries = qBound(0, retries, 1);
    if (request.size() < 9) throw Error("request too short");
    if (m_pending) throw Error("a serial transaction is already pending");
    if (m_cancelRequested.load()) throwCancelled("REQUEST_BEFORE_CREATE");
    PendingRequest pending;
    pending.requestId = nextRequestId.fetch_add(1);
    pending.protocolAddress = static_cast<quint8>(request[0]);
    pending.service = static_cast<quint8>(request[2]);
    pending.commandClass = static_cast<quint8>(request[4]);
    pending.instance = static_cast<quint8>(request[5]);
    pending.attribute = static_cast<quint8>(request[6]);
    pending.logicalChannel = m_logicalChannel;
    pending.tx = request;
    m_pending = pending;
    m_attemptCount = 0;
    m_lastParserState.clear();
    m_lastChecksumExpected = -1;
    m_lastChecksumReceived = -1;
    m_lastReceivedService = -1;
    m_lastReceivedClass = -1;
    m_lastReceivedInstance = -1;
    m_lastReceivedAttribute = -1;
    m_lastProtocolError.clear();
    m_lastRx.clear();
    m_lastAck.clear();
    m_lastResponseFrame.clear();
    m_rxBatches.clear();
    m_attemptRecords.clear();
    m_attemptErrors.clear();
    m_preTxObservations.clear();
    m_recoveryEvents.clear();
    m_firstRxTimestamp = {};
    m_lastRxTimestamp = {};
    m_frameCompleteTimestamp = {};
    m_historyAtRequestStart = m_rxHistory;
    m_lateRxAfterFinishCount = 0;
    m_lateRxAfterFinishBytes = 0;
    m_rxBytesAvailableBeforeTx = 0;
    m_parserStateBeforeTx.clear();
    m_preTxRxPeek.clear();
    QElapsedTimer transactionTimer;
    transactionTimer.start();
    QString finalResult = QStringLiteral("SERIAL_ERROR");
    QString finalDetail = QStringLiteral("transaction failed");
    struct FinishGuard {
        SerialTransport *owner;
        QString *result;
        QString *detail;
        QElapsedTimer *timer;
        ~FinishGuard() { owner->finishCurrentTransaction(*result, *detail, timer->elapsed()); }
    } finishGuard{this, &finalResult, &finalDetail, &transactionTimer};
    if (m_trace && !m_errorRawOnly) m_trace(QStringLiteral("[transport] REQUEST_CREATED request_id=%1 logical_channel=%2 address=%3 command=%4 service=0x%5 class=0x%6 "
                                        "instance=%7 attr=0x%8 retries=%9")
        .arg(pending.requestId).arg(pending.logicalChannel).arg(pending.protocolAddress)
        .arg(commandName(pending.service)).arg(pending.service, 2, 16, QLatin1Char('0'))
        .arg(pending.commandClass, 2, 16, QLatin1Char('0')).arg(pending.instance)
        .arg(pending.attribute, 2, 16, QLatin1Char('0')).arg(retries));
    std::string lastError = "transaction failed";
    bool lastWasTimeout = false;
    bool lastWasProtocol = false;
    for (int attempt = 0; attempt <= retries; ++attempt) {
        m_attemptCount = attempt + 1;
        if (attempt > 0 && m_trace) m_trace(QStringLiteral("[transport] RETRY_BEGIN request_id=%1 attempt=%2")
            .arg(pending.requestId).arg(attempt + 1));
        const int attemptRxStart = m_lastRx.size();
        const int errorCountAtStart = m_attemptErrors.size();
        const QDateTime attemptStartedAt = QDateTime::currentDateTime();
        QByteArray acknowledge;
        // QSerialPort may not populate its userspace buffer until it is polled;
        // a bounded 1 ms poll makes PRE_TX_CHECK see bytes already waiting in
        // the kernel/FTDI queue without imposing a recovery quiet window on
        // healthy traffic.
        if (m_port.bytesAvailable() == 0)
            (void)m_port.waitForReadyRead(1);
        const int availableBeforeTx = static_cast<int>(m_port.bytesAvailable());
        const QString parserBeforeTx = m_parser.stateName();
        const QByteArray preTxPeek = availableBeforeTx > 0
            ? m_port.peek(qMin(availableBeforeTx, 512)) : QByteArray{};
        m_preTxObservations.append(QVariantMap{{"attempt", attempt + 1},
            {"rxBytesAvailableBeforeTx", availableBeforeTx}, {"parserStateBeforeTx", parserBeforeTx},
            {"preTxRxPeek", hex(preTxPeek)},
            {"timestamp", attemptStartedAt.toString(Qt::ISODateWithMs)}});
        if (attempt == 0) {
            m_rxBytesAvailableBeforeTx = availableBeforeTx;
            m_parserStateBeforeTx = parserBeforeTx;
            m_preTxRxPeek = preTxPeek;
            if (m_experimentDiagnostics && m_lastFinishNs >= 0 && availableBeforeTx > 0) {
                ++m_lateRxAfterFinishCount;
                m_lateRxAfterFinishBytes += availableBeforeTx;
                if (m_trace) m_trace(QStringLiteral("STALE_RX_AFTER_FINISH previous_request_id=%1 next_request_id=%2 bytes=%3 raw=%4")
                    .arg(m_previousRequestId).arg(pending.requestId).arg(availableBeforeTx).arg(hex(preTxPeek)));
            }
        }
        try {
            // A shared port may never transmit across an unverified boundary.
            // This applies to diagnostic mode as well: diagnostics now record
            // the recovery instead of deliberately preserving the fault.
            if (!verifyRxClean(pending, attempt + 1)) {
                finalResult = QStringLiteral("RECOVERY_FAILED");
                finalDetail = QStringLiteral("RX recovery failed before TX");
                lastError = finalDetail.toStdString();
                lastWasProtocol = true;
                m_attemptErrors.append(QVariantMap{{"attempt", attempt + 1},
                    {"errorType", "recovery_failed"}, {"errorMessage", finalDetail},
                    {"parserState", m_parser.stateName()}, {"txRaw", hex(pending.tx)},
                    {"rxPending", availableBeforeTx}, {"recoveryReason", "PRE_TX_CHECK"}});
                m_attemptRecords.append(QVariantMap{{"attempt", attempt + 1},
                    {"result", "RECOVERY_FAILED"}, {"rxRaw", hex(m_lastRx.mid(attemptRxStart))}});
                break;
            }
            const qint64 attemptTxNs = m_monotonicClock.nsecsElapsed();
            if (attempt == 0) {
                m_currentTxNs = attemptTxNs;
                m_currentTxTimestamp = QDateTime::currentDateTime();
            }
            const double gapMs = m_lastFinishNs >= 0 ? (attemptTxNs - m_lastFinishNs) / 1000000.0 : -1.0;
            if (m_trace && !m_errorRawOnly) m_trace(QStringLiteral("[transport] TX_BEGIN request_id=%1 logical_channel=%2 address=%3 attempt=%4 tx_timestamp=%5 previousFinishToNextTxMs=%6")
                .arg(pending.requestId).arg(pending.logicalChannel).arg(pending.protocolAddress).arg(attempt + 1)
                .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs))
                .arg(gapMs < 0 ? QStringLiteral("N/A") : QString::number(gapMs, 'f', 3)));
            if (m_trace && !m_errorRawOnly) m_trace(QStringLiteral("[transport] TX request_id=%1 logical_channel=%2 address=%3 attempt=%4 bytes=%5")
                .arg(pending.requestId).arg(pending.logicalChannel).arg(pending.protocolAddress).arg(attempt + 1).arg(hex(request)));
            m_state = TransactionState::Tx;
            write(request);
            m_state = TransactionState::WaitAck;
            acknowledge = readExact(1, ackTimeoutMs);
            m_lastAck = acknowledge;
            const quint8 ack = static_cast<quint8>(acknowledge[0]);
            if (m_trace && !m_errorRawOnly) m_trace(ack == MfcProtocol::Ack
                ? QStringLiteral("[transport] ACK request_id=%1 logical_channel=%2 address=%3 value=06")
                    .arg(pending.requestId).arg(pending.logicalChannel).arg(pending.protocolAddress)
                : QStringLiteral("[transport] RX_CONTROL request_id=%1 logical_channel=%2 address=%3 value=%4")
                    .arg(pending.requestId).arg(pending.logicalChannel).arg(pending.protocolAddress).arg(hex(acknowledge)));
            if (ack == MfcProtocol::Nak) throw NegativeAcknowledge("MFC returned NAK");
            if (ack != MfcProtocol::Ack) throw ProtocolFailure("invalid ACK byte");
            QByteArray received;
            m_state = TransactionState::ReadFrame;
            QByteArray packet = readResponseFrame(responseTimeoutMs, pending, &received);
            m_lastResponseFrame = packet;
            if (m_trace && !m_errorRawOnly) m_trace(QStringLiteral("[transport] RX request_id=%1 logical_channel=%2 address=%3 frame=%4")
                .arg(pending.requestId).arg(pending.logicalChannel).arg(pending.protocolAddress).arg(hex(packet)));
            finalResult = QStringLiteral("SUCCESS");
            finalDetail = QStringLiteral("valid response");
            m_state = TransactionState::Success;
            m_consecutiveProtocolErrors = 0;
            m_consecutiveInvalidAcks = 0;
            m_attemptRecords.append(QVariantMap{{"attempt", attempt + 1}, {"result", "SUCCESS"},
                {"txTimestamp", attemptStartedAt.toString(Qt::ISODateWithMs)}, {"ackRaw", hex(acknowledge)},
                {"rxRaw", hex(m_lastRx.mid(attemptRxStart))}, {"responseFrame", hex(packet)},
                {"protocolEvents", m_attemptErrors.size() - errorCountAtStart}});
            if (m_trace && !m_errorRawOnly) m_trace(QStringLiteral("[transport] REQUEST_SUCCESS request_id=%1 logical_channel=%2 address=%3 attempt=%4 checksum_expected=%5 checksum_received=%6")
                .arg(pending.requestId).arg(pending.logicalChannel).arg(pending.protocolAddress).arg(attempt + 1)
                .arg(m_lastChecksumExpected).arg(m_lastChecksumReceived));
            return packet;
        } catch (const NegativeAcknowledge &error) {
            finalResult = QStringLiteral("PROTOCOL_ERROR");
            finalDetail = QStringLiteral("MFC returned NAK");
            m_attemptErrors.append(QVariantMap{{"attempt", attempt + 1}, {"errorType", "negative_acknowledge"},
                {"errorMessage", finalDetail}, {"parserState", "ACK"}, {"currentFrame", hex(acknowledge)},
                {"txRaw", hex(pending.tx)}, {"allRxRaw", hex(m_lastRx)}, {"rxOffsetAfterError", m_lastRx.size()},
                {"rxHistoryBeforeError", hex(m_historyAtRequestStart)},
                {"errorTimestamp", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)}});
            m_attemptRecords.append(QVariantMap{{"attempt", attempt + 1}, {"result", "PROTOCOL_ERROR"},
                {"detail", finalDetail}, {"ackRaw", hex(acknowledge)}, {"rxRaw", hex(m_lastRx.mid(attemptRxStart))}});
            if (m_trace) m_trace(QStringLiteral("NAK attempt=%1").arg(attempt + 1));
            recordProtocolFailure(finalDetail);
            (void)recoverBus(QStringLiteral("NEGATIVE_ACK"), pending, attempt + 1,
                             m_consecutiveProtocolErrors >= ConsecutiveErrorHardRecoveryThreshold);
            Q_UNUSED(error)
            throw;
        } catch (const Cancelled &error) {
            finalResult = QStringLiteral("CANCELLED");
            finalDetail = QString::fromUtf8(error.what());
            m_attemptRecords.append(QVariantMap{{"attempt", attempt + 1}, {"result", "CANCELLED"},
                {"detail", finalDetail}, {"rxRaw", hex(m_lastRx.mid(attemptRxStart))}});
            throw;
        } catch (const ProtocolFailure &error) {
            lastError = error.what();
            lastWasTimeout = false;
            lastWasProtocol = true;
            finalResult = QStringLiteral("PROTOCOL_ERROR");
            finalDetail = QString::fromUtf8(error.what());
            if (m_attemptErrors.size() == errorCountAtStart) {
                m_attemptErrors.append(QVariantMap{{"attempt", attempt + 1}, {"errorType", errorType(finalDetail)},
                    {"errorMessage", finalDetail}, {"parserState", acknowledge.isEmpty() ? m_parser.stateName() : QStringLiteral("ACK")},
                    {"currentFrame", acknowledge.isEmpty() ? hex(m_parser.partialFrame()) : hex(acknowledge)},
                    {"txRaw", hex(pending.tx)}, {"rxOffsetAfterError", m_lastRx.size()},
                    {"rxHistoryBeforeError", hex(m_historyAtRequestStart)},
                    {"errorTimestamp", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)}});
            }
            m_attemptRecords.append(QVariantMap{{"attempt", attempt + 1}, {"result", "PROTOCOL_ERROR"},
                {"detail", finalDetail}, {"ackRaw", hex(acknowledge)}, {"rxRaw", hex(m_lastRx.mid(attemptRxStart))}});
            if (m_trace) m_trace(QStringLiteral("#%1 PROTOCOL_FAILURE attempt=%2 error=%3")
                .arg(pending.requestId).arg(attempt + 1).arg(finalDetail));
            recordProtocolFailure(finalDetail);
            const QString recoveryReason = finalDetail.contains(QStringLiteral("invalid ACK"), Qt::CaseInsensitive)
                ? QStringLiteral("INVALID_ACK") : errorType(finalDetail).toUpper();
            const bool recovered = recoverBus(recoveryReason, pending, attempt + 1,
                m_consecutiveProtocolErrors >= ConsecutiveErrorHardRecoveryThreshold);
            if (!recovered) {
                finalResult = QStringLiteral("RECOVERY_FAILED");
                finalDetail = QStringLiteral("BUS_RECOVERY failed after: ") + finalDetail;
                lastError = finalDetail.toStdString();
                break;
            }
            if (m_trace) m_trace(QStringLiteral("[transport] RETRY_DECISION request_id=%1 origin=%2 reason=%3 attempt=%4 max_attempts=%5 will_retry=%6")
                .arg(pending.requestId).arg(m_transactionOrigin.isEmpty() ? QStringLiteral("UNSPECIFIED") : m_transactionOrigin)
                .arg(recoveryReason).arg(attempt + 1).arg(retries + 1).arg(attempt < retries));
            if (attempt == retries) break;
            m_state = TransactionState::Retry;
        } catch (const Timeout &error) {
            lastError = error.what();
            lastWasTimeout = true;
            lastWasProtocol = false;
            finalResult = QStringLiteral("TIMEOUT");
            finalDetail = QString::fromUtf8(error.what());
            m_attemptErrors.append(QVariantMap{{"attempt", attempt + 1}, {"errorType", "timeout"},
                {"errorMessage", finalDetail}, {"parserState", m_parser.stateName()},
                {"currentFrame", hex(m_parser.partialFrame())}, {"txRaw", hex(pending.tx)},
                {"rxOffsetAfterError", m_lastRx.size()}, {"rxHistoryBeforeError", hex(m_historyAtRequestStart)},
                {"errorTimestamp", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)}});
            m_attemptRecords.append(QVariantMap{{"attempt", attempt + 1}, {"result", "TIMEOUT"},
                {"detail", finalDetail}, {"ackRaw", hex(acknowledge)}, {"rxRaw", hex(m_lastRx.mid(attemptRxStart))}});
            if (m_trace) m_trace(QStringLiteral("TIMEOUT attempt=%1 %2")
                                 .arg(attempt + 1).arg(QString::fromUtf8(error.what())));
            const bool recovered = recoverBus(QStringLiteral("TIMEOUT"), pending, attempt + 1,
                m_consecutiveProtocolErrors >= ConsecutiveErrorHardRecoveryThreshold);
            if (!recovered) {
                finalResult = QStringLiteral("RECOVERY_FAILED");
                finalDetail = QStringLiteral("BUS_RECOVERY failed after timeout");
                lastError = finalDetail.toStdString();
                lastWasTimeout = false;
                lastWasProtocol = true;
                break;
            }
            if (m_trace) m_trace(QStringLiteral("[transport] RETRY_DECISION request_id=%1 origin=%2 reason=RESPONSE_TIMEOUT attempt=%3 max_attempts=%4 will_retry=%5")
                .arg(pending.requestId).arg(m_transactionOrigin.isEmpty() ? QStringLiteral("UNSPECIFIED") : m_transactionOrigin)
                .arg(attempt + 1).arg(retries + 1).arg(attempt < retries));
            if (attempt == retries) break;
            m_state = TransactionState::Retry;
        } catch (const std::exception &error) {
            lastError = error.what();
            lastWasTimeout = false;
            lastWasProtocol = false;
            finalResult = QStringLiteral("SERIAL_ERROR");
            finalDetail = QString::fromUtf8(error.what());
            m_attemptErrors.append(QVariantMap{{"attempt", attempt + 1}, {"errorType", "serial_error"},
                {"errorMessage", finalDetail}, {"parserState", m_parser.stateName()},
                {"currentFrame", hex(m_parser.partialFrame())}, {"txRaw", hex(pending.tx)},
                {"rxOffsetAfterError", m_lastRx.size()}, {"rxHistoryBeforeError", hex(m_historyAtRequestStart)},
                {"errorTimestamp", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)}});
            m_attemptRecords.append(QVariantMap{{"attempt", attempt + 1}, {"result", "SERIAL_ERROR"},
                {"detail", finalDetail}, {"ackRaw", hex(acknowledge)}, {"rxRaw", hex(m_lastRx.mid(attemptRxStart))}});
            if (m_trace) m_trace(QStringLiteral("TRANSACTION ERROR attempt=%1 %2")
                                 .arg(attempt + 1).arg(QString::fromUtf8(error.what())));
            const bool recovered = recoverBus(QStringLiteral("SERIAL_ERROR"), pending, attempt + 1, true);
            if (!recovered) {
                finalResult = QStringLiteral("RECOVERY_FAILED");
                finalDetail = QStringLiteral("BUS_RECOVERY failed after serial error");
                lastError = finalDetail.toStdString();
                lastWasProtocol = true;
                break;
            }
            if (attempt == retries) break;
            m_state = TransactionState::Retry;
        }
    }
    m_state = TransactionState::Failed;
    if (lastWasTimeout) throw Timeout(lastError);
    if (lastWasProtocol) throw ProtocolFailure(lastError);
    throw Error(lastError);
}

void SerialTransport::transactionAck(const QByteArray &request, int ackTimeoutMs, int retries)
{
    // The existing state machine owns one pending transaction.  An ACK-only
    // request is represented by a short, valid synthetic response timeout of
    // zero only after ACK; implement it inline rather than allowing raw write.
    retries = qBound(0, retries, 1);
    if (request.size() < 9 || static_cast<quint8>(request[2]) != MfcProtocol::WriteService)
        throw Error("invalid ACK-only request");
    if (m_pending) throw Error("a serial transaction is already pending");
    if (m_cancelRequested.load()) throwCancelled("ACK_ONLY_BEFORE_CREATE");
    PendingRequest pending;
    pending.requestId = nextRequestId.fetch_add(1); pending.protocolAddress = static_cast<quint8>(request[0]);
    pending.service = static_cast<quint8>(request[2]); pending.commandClass = static_cast<quint8>(request[4]);
    pending.instance = static_cast<quint8>(request[5]); pending.attribute = static_cast<quint8>(request[6]);
    pending.logicalChannel = m_logicalChannel; pending.tx = request; m_pending = pending;
    m_lastRx.clear(); m_lastAck.clear(); m_lastResponseFrame.clear();
    QElapsedTimer timer; timer.start(); QString result = QStringLiteral("SERIAL_ERROR"), detail = QStringLiteral("write failed");
    struct Guard { SerialTransport *t; QString *r; QString *d; QElapsedTimer *e; ~Guard(){ t->finishCurrentTransaction(*r,*d,e->elapsed()); } } guard{this,&result,&detail,&timer};
    for (int attempt = 0; attempt <= retries; ++attempt) {
        m_attemptCount = attempt + 1;
        if (!verifyRxClean(pending, attempt + 1)) throw ProtocolFailure("RX recovery failed before write");
        try {
            m_state = TransactionState::Tx; write(request); m_state = TransactionState::WaitAck;
            const QByteArray ackBytes = readExact(1, ackTimeoutMs);
            m_lastAck = ackBytes;
            const quint8 ack = static_cast<quint8>(ackBytes[0]);
            if (ack == MfcProtocol::Nak) throw NegativeAcknowledge("MFC returned NAK");
            if (ack != MfcProtocol::Ack) throw ProtocolFailure("invalid ACK byte");
            result = QStringLiteral("SUCCESS"); detail = QStringLiteral("write ACK"); m_state = TransactionState::Success;
            return;
        } catch (const Cancelled &) { result = QStringLiteral("CANCELLED"); detail = QStringLiteral("transaction cancelled"); throw; }
        catch (const std::exception &e) {
            detail = QString::fromUtf8(e.what()); result = dynamic_cast<const Timeout *>(&e) ? QStringLiteral("TIMEOUT") : QStringLiteral("PROTOCOL_ERROR");
            if (attempt == retries) throw;
            (void)recoverBus(QStringLiteral("WRITE_RETRY"), pending, attempt + 1);
        }
    }
}

QVariantMap SerialTransport::diagnostics() const
{
    QVariantMap result{{"transactionPending", m_pending.has_value()},
                       {"lastFinishedRequestId", static_cast<qulonglong>(m_lastFinishedRequestId)},
                       {"lastTransactionResult", m_lastResult},
                       {"lastProtocolError", m_lastProtocolError},
                       {"lastRxHex", hex(m_lastRx)},
                       {"parserState", m_parser.stateName()},
                       {"rxHistoryHex", hex(m_rxHistory)},
                       {"rxSequence", static_cast<qulonglong>(m_rxSequence)},
                       {"transactionState", stateName(m_state)},
                       {"serialDegraded", m_serialDegraded},
                       {"consecutiveRecoveryFailures", m_consecutiveRecoveryFailures},
                       {"busRecoveryCount", static_cast<qulonglong>(m_recoveryCounters.busRecoveryCount)},
                       {"busRecoverySuccessCount", static_cast<qulonglong>(m_recoveryCounters.busRecoverySuccessCount)},
                       {"busRecoveryFailureCount", static_cast<qulonglong>(m_recoveryCounters.busRecoveryFailureCount)},
                       {"rxDrainCount", static_cast<qulonglong>(m_recoveryCounters.rxDrainCount)},
                       {"rxDiscardedBytes", static_cast<qulonglong>(m_recoveryCounters.rxDiscardedBytes)},
                       {"rxPendingBeforeTxCount", static_cast<qulonglong>(m_recoveryCounters.rxPendingBeforeTxCount)},
                       {"rxPendingBeforeTxMax", static_cast<qulonglong>(m_recoveryCounters.rxPendingBeforeTxMax)},
                       {"rxQuietWaitCount", static_cast<qulonglong>(m_recoveryCounters.rxQuietWaitCount)},
                       {"rxQuietWaitTotalMs", static_cast<qulonglong>(m_recoveryCounters.rxQuietWaitTotalMs)},
                       {"rxQuietWaitMaxMs", static_cast<qulonglong>(m_recoveryCounters.rxQuietWaitMaxMs)},
                       {"streamResyncCount", static_cast<qulonglong>(m_recoveryCounters.streamResyncCount)},
                       {"streamResyncDiscardedBytes", static_cast<qulonglong>(m_recoveryCounters.streamResyncDiscardedBytes)},
                       {"consecutiveProtocolErrorMax", static_cast<qulonglong>(m_recoveryCounters.consecutiveProtocolErrorMax)},
                       {"consecutiveInvalidAckMax", static_cast<qulonglong>(m_recoveryCounters.consecutiveInvalidAckMax)},
                       {"hardRecoveryCount", static_cast<qulonglong>(m_recoveryCounters.hardRecoveryCount)},
                       {"serialReopenCount", static_cast<qulonglong>(m_recoveryCounters.serialReopenCount)},
                       {"serialConfiguration", runtimeSerialConfiguration()}};
    result["physicalPortExists"] = physicalPortExists();
    result["lastTransaction"] = m_lastTransaction;
    if (m_pending) {
        result["currentRequestId"] = static_cast<qulonglong>(m_pending->requestId);
        result["currentProtocolAddress"] = m_pending->protocolAddress;
        result["currentClass"] = m_pending->commandClass;
        result["currentAttribute"] = m_pending->attribute;
    }
    return result;
}
