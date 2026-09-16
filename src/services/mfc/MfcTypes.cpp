#include "MfcTypes.h"
#include <cmath>

namespace {
bool sameGasName(QString actual, QString configured)
{
    actual.remove(' '); configured.remove(' ');
    const auto canonical = [](QString value) {
        value = value.trimmed().toLower();
        if (value == QStringLiteral("空气") || value == QStringLiteral("air")) return QStringLiteral("air");
        if (value == QStringLiteral("氮气") || value == QStringLiteral("n2") || value == QStringLiteral("nitrogen")) return QStringLiteral("n2");
        if (value == QStringLiteral("丙烷") || value == QStringLiteral("propane")) return QStringLiteral("propane");
        return value;
    };
    return canonical(actual) == canonical(configured);
}
}

QVariantMap MfcDeviceConfig::toVariantMap() const
{
    return {{"logicalChannel", logicalChannel}, {"protocolAddress", address}, {"address", address},
            {"addressText", QStringLiteral("%1 (0x%2)").arg(address).arg(address, 2, 16, QLatin1Char('0')).toUpper()},
            {"id", stableId()}, {"displayName", displayName}, {"gasType", gasType},
            {"function", function}, {"unit", unit}, {"fullScale", fullScale},
            {"expectedGasCode", expectedGasCode}, {"expectedDeviceFullScaleSccm", expectedDeviceFullScaleSccm},
            {"minSetpoint", minimumSetpoint}, {"maxSetpoint", maximumSetpoint},
            {"absoluteTolerance", absoluteTolerance}, {"relativeTolerancePercent", relativeTolerancePercent},
            {"enabled", enabled}, {"required", required}, {"addressConfirmed", addressConfirmed}};
}

void MfcDeviceState::refreshCapabilities()
{
    const auto confirmedText = [](const QString &value) {
        const QString text = value.trimmed();
        return !text.isEmpty() && text != QStringLiteral("待确认");
    };
    const double scale = config.fullScale;
    engineeringConfigured = confirmedText(config.gasType) && confirmedText(config.function)
        && !config.unit.trimmed().isEmpty() && scale > 0.0;
}

QVariantMap MfcDeviceInfo::toVariantMap() const
{
    const QString control = controlMode == MfcControlMode::Digital ? QStringLiteral("数字")
        : controlMode == MfcControlMode::AnalogVoltage ? QStringLiteral("模拟电压")
        : controlMode == MfcControlMode::AnalogCurrent ? QStringLiteral("模拟电流") : QStringLiteral("--");
    const QString valve = valveState == MfcValveState::Normal ? QStringLiteral("Normal")
        : valveState == MfcValveState::Closed ? QStringLiteral("Closed")
        : valveState == MfcValveState::Purge ? QStringLiteral("Purge") : QStringLiteral("--");
    return {{"port", port}, {"portDescription", portDescription},
            {"portManufacturer", portManufacturer}, {"usbSerialNumber", usbSerialNumber},
            {"vid", vendorId}, {"pid", productId}, {"address", address},
            {"addressText", QStringLiteral("%1 (0x%2)").arg(address).arg(address, 2, 16, QLatin1Char('0')).toUpper()},
            {"baudRate", baudRate}, {"manufacturer", manufacturer}, {"model", model},
            {"serialNumber", serialNumber}, {"firmware", firmware}, {"pcbRevision", pcbRevision},
            {"manufacturingDate", manufacturingDate}, {"calibrationDate", calibrationDate},
            {"targetGasName", targetGasName}, {"targetGasCode", targetGasCode},
            {"targetGasFullScale", targetGasFullScale},
            {"calibrationGasName", calibrationGasName}, {"calibrationGasCode", calibrationGasCode},
            {"calibrationGasFullScale", calibrationGasFullScale},
            {"calibrationGasReadFromDevice", calibrationGasReadFromDevice},
            {"fullScale", fullScale}, {"fullScaleUnit", fullScaleUnit},
            {"fullScaleReadFromDevice", fullScaleReadFromDevice},
            {"conversionFactor", conversionFactor}, {"controlMode", control}, {"valveState", valve}};
}

QVariantMap MfcDeviceState::toVariantMap() const
{
    QVariantMap map = config.toVariantMap();
    const QString linkText = linkState == MfcLinkState::Online ? QStringLiteral("通信正常")
        : linkState == MfcLinkState::Timeout ? QStringLiteral("通信超时")
        : linkState == MfcLinkState::Error ? QStringLiteral("通信异常")
        : linkState == MfcLinkState::NotDiscovered ? QStringLiteral("未发现") : QStringLiteral("离线");
    const QString communicationText = communicationState == MfcCommunicationState::Normal
        ? QStringLiteral("正常")
        : communicationState == MfcCommunicationState::Degraded
            ? QStringLiteral("通信异常，正在自动重试")
        : communicationState == MfcCommunicationState::Fault
            ? QStringLiteral("通信故障，正在自动重试")
        : communicationState == MfcCommunicationState::Recovering
            ? QStringLiteral("通信恢复中") : QStringLiteral("未确认");
    map["linkState"] = static_cast<int>(linkState);
    map["linkStateText"] = linkText;
    map["communicationOnline"] = communicationOnline;
    map["addressDetected"] = addressDetected;
    map["addressConfirmed"] = config.addressConfirmed;
    map["engineeringConfigured"] = engineeringConfigured;
    map["monitoringActive"] = monitoringActive;
    map["metadataAvailable"] = metadataAvailable;
    map["metadataVerificationAttempted"] = metadataVerificationAttempted;
    map["metadataVerificationComplete"] = metadataVerificationComplete;
    map["metadataResults"] = metadataResults;
    map["metadataDiagnostics"] = metadataDiagnostics;
    map["dataFresh"] = dataFresh;
    map["readOnly"] = true;
    map["addressSource"] = addressDetected ? QStringLiteral("配置地址（只读响应已验证）")
                                             : QStringLiteral("配置候选地址");
    map["configurationStateText"] = config.addressConfirmed && engineeringConfigured
        ? QStringLiteral("配置完整，只读监测")
        : config.addressConfirmed ? QStringLiteral("工程配置待确认") : QStringLiteral("地址映射待确认");
    map["actualValueAvailable"] = reading.communicationOk && dataFresh;
    map["flowPercent"] = reading.flowPercent;
    map["actualPercentFS"] = reading.flowPercent;
    // READ_FLOW is normalized with the device's own full scale.  Do not use a
    // configured number as a fallback: configuration can still be pending or
    // stale, while register 0x66/0x03 is the authoritative CS200 source.
    const double engineeringFullScale = config.fullScale;
    const QString engineeringUnit = config.unit;
    const bool engineeringFlowAvailable = reading.communicationOk && engineeringFullScale > 0.0
        && !engineeringUnit.trimmed().isEmpty();
    map["percentFullScale"] = reading.flowPercent;
    map["fullScaleValue"] = engineeringFullScale;
    map["fullScaleSource"] = QStringLiteral("工程配置（监测不读取量程寄存器）");
    map["flowUnit"] = engineeringUnit;
    map["currentFlow"] = engineeringFlowAvailable
        ? reading.flowPercent / 100.0 * engineeringFullScale : 0.0;
    map["currentFlowAvailable"] = engineeringFlowAvailable;
    // Valve state is metadata only in the current read-only polling cycle;
    // there is no valve-opening register being presented as flow.
    map["valveOpeningAvailable"] = false;
    map["actualFlow"] = reading.flowValue;
    map["actualFlowSccm"] = info.fullScale > 0.0
        ? reading.flowPercent / 100.0 * info.fullScale : 0.0;
    map["gasNameFromDevice"] = info.targetGasName;
    map["deviceFullScaleSccm"] = info.fullScaleReadFromDevice ? info.fullScale : 0.0;
    map["deviceInfo"] = info.toVariantMap();
    const auto fieldMatches = [&map](const QString &field) {
        return map.value("metadataResults").toMap().value(field).toMap().value("status").toString()
            == QStringLiteral("MATCH");
    };
    const bool addressMatches = !metadataVerificationAttempted || fieldMatches(QStringLiteral("RS485 Address"));
    const bool gasMatches = !metadataVerificationAttempted
        || (fieldMatches(QStringLiteral("Target Gas Name")) && fieldMatches(QStringLiteral("Target Gas Code")));
    const bool scaleMatches = !metadataVerificationAttempted || fieldMatches(QStringLiteral("Target Full Scale"));
    map["verificationAddressMatches"] = addressMatches;
    map["verificationGasMatches"] = gasMatches;
    map["verificationFullScaleMatches"] = scaleMatches;
    map["verificationMatches"] = addressMatches && gasMatches && scaleMatches;
    map["processFunction"] = config.function;
    map["temperature"] = reading.temperature;
    map["warnings"] = reading.warnings.join(QStringLiteral("、"));
    map["alarms"] = reading.alarms.join(QStringLiteral("、"));
    map["lastUpdateTime"] = lastUpdateTime.toString(Qt::ISODateWithMs);
    map["lastSuccessTime"] = lastSuccessTime.toString(Qt::ISODateWithMs);
    map["firstFailureTime"] = firstFailureTime.toString(Qt::ISODateWithMs);
    map["successCount"] = static_cast<qulonglong>(successCount);
    map["errorCount"] = static_cast<qulonglong>(errorCount);
    map["timeoutCount"] = static_cast<qulonglong>(timeoutCount);
    map["protocolErrorCount"] = static_cast<qulonglong>(protocolErrorCount);
    map["checksumErrorCount"] = static_cast<qulonglong>(checksumErrorCount);
    map["serviceErrorCount"] = static_cast<qulonglong>(serviceErrorCount);
    map["serialErrorCount"] = static_cast<qulonglong>(serialErrorCount);
    map["consecutiveFailureCount"] = consecutiveFailures; // legacy diagnostics key
    map["consecutiveFailures"] = consecutiveFailures;
    map["consecutiveSuccesses"] = consecutiveSuccesses;
    map["communicationStateCode"] = static_cast<int>(communicationState);
    map["communicationState"] = communicationText;
    map["responseTimeMs"] = responseTimeMs;
    map["averageResponseTimeMs"] = successCount > 0 ? totalResponseTimeMs / successCount : 0.0;
    map["maximumResponseTimeMs"] = maximumResponseTimeMs;
    map["lastError"] = lastError;
    const QString control = controlState == MfcControlState::Controlling ? QStringLiteral("正常")
        : controlState == MfcControlState::ControlDegraded ? QStringLiteral("状态未知")
        : controlState == MfcControlState::ControlFault ? QStringLiteral("运行点未完整应用")
        : controlState == MfcControlState::ControlStopped ? QStringLiteral("目标已归零")
        : controlState == MfcControlState::Monitoring ? QStringLiteral("监测模式") : QStringLiteral("控制事务中");
    map["controlStateCode"] = static_cast<int>(controlState);
    map["controlState"] = control;
    map["targetFlow"] = targetFlow;
    map["targetConfirmed"] = targetConfirmed;
    map["lastControlConfirmation"] = lastControlConfirmation.toString(Qt::ISODateWithMs);
    const QString stopState = stopStage == MfcStopStage::DigitalVerified ? QStringLiteral("目标归零已确认")
        : stopStage == MfcStopStage::ActiveVerified ? QStringLiteral("有效设定已归零")
        : stopStage == MfcStopStage::WritingZero ? QStringLiteral("正在归零")
        : stopStage == MfcStopStage::Unconfirmed ? QStringLiteral("停止控制状态未确认") : QString();
    map["stopStage"] = static_cast<int>(stopStage);
    map["stopState"] = stopState;
    map["effectiveSetpointState"] = stopStage == MfcStopStage::ActiveVerified
        ? QStringLiteral("有效设定已归零")
        : controlState == MfcControlState::Controlling && targetConfirmed
            ? QStringLiteral("有效设定已同步")
        : controlState == MfcControlState::ControlDegraded
            ? QStringLiteral("有效设定未确认")
        : QStringLiteral("尚未应用设定");
    map["stopFailure"] = stopFailure;
    return map;
}
