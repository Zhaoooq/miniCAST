#include "GasChannel.h"

QString GasChannel::statusText(FlowStatus value)
{
    switch (value) {
    case FlowStatus::Normal: return QStringLiteral("正常");
    case FlowStatus::Warning: return QStringLiteral("警告");
    case FlowStatus::Critical: return QStringLiteral("严重");
    case FlowStatus::Offline: return QStringLiteral("离线");
    case FlowStatus::Stabilizing: return QStringLiteral("调节中");
    case FlowStatus::ConfigurationRequired: return QStringLiteral("配置待确认");
    case FlowStatus::CommunicationError: return QStringLiteral("通信异常");
    }
    return QStringLiteral("未知");
}

QVariantMap GasChannel::toVariantMap() const
{
    QVariantMap map{{"id", id}, {"name", nameChinese}, {"chemicalName", chemicalName},
            {"unit", unit}, {"flowUnit", flowUnit}, {"fullScaleValue", fullScaleValue},
            {"currentFlow", currentFlow}, {"actualFlow", actualFlow},
            {"lastValidActualFlow", lastValidActualFlow},
            {"lastValidFlowTimestamp", lastValidFlowTimestamp.toString(Qt::ISODateWithMs)},
            {"flowFresh", flowFresh}, {"targetFlow", targetFlow},
            {"configuredTargetFlow", configuredTargetFlow}, {"activeTargetFlow", activeTargetFlow},
            {"percentFullScale", percentFullScale}, {"valveOpening", valveOpening},
            {"minimum", minimum}, {"maximum", maximum},
            {"setValue", setValue}, {"realValue", realValue},
            {"temperature", temperature}, {"deviation", deviation}, {"deviationPercent", deviationPercent},
            {"deviationAvailable", deviationAvailable}, {"status", static_cast<int>(status)},
            {"monitoringActive", monitoringActive},
            {"adjustable", adjustable}, {"online", online},
            {"actualValueAvailable", actualValueAvailable},
            {"currentFlowAvailable", currentFlowAvailable},
            {"hasValidActualFlow", hasValidActualFlow},
            {"actualFlowFresh", actualFlowFresh},
            {"waitingForActualFlow", waitingForActualFlow},
            {"targetFlowAvailable", targetFlowAvailable},
            {"configuredTargetFlowAvailable", configuredTargetFlowAvailable},
            {"valveOpeningAvailable", valveOpeningAvailable},
            {"addressDetected", addressDetected}, {"addressConfirmed", addressConfirmed},
            {"engineeringConfigured", engineeringConfigured},
            {"readOnly", readOnly}, {"address", address},
            {"gasType", gasType}, {"function", function},
            {"communicationState", communicationState}, {"communicationStateCode", communicationStateCode},
            {"configurationState", configurationState},
            {"controlState", controlState}, {"controlStateCode", controlStateCode},
            {"effectiveSetpointState", effectiveSetpointState},
            {"targetConfirmed", targetConfirmed},
            {"stopState", stopState}, {"stopFailure", stopFailure},
            {"operatingPointId", operatingPointId},
            {"responseTimeMs", responseTimeMs}, {"tolerancePercent", tolerancePercent}};
    // Communication health is intentionally presented in Chinese and never
    // falls back to an internal parser error such as checksum/instance.
    map["statusText"] = ((status == FlowStatus::Warning || status == FlowStatus::CommunicationError)
                           && (communicationState == QStringLiteral("通信异常，正在自动重试")
                               || communicationState == QStringLiteral("通信故障，正在自动重试")
                               || communicationState == QStringLiteral("通信恢复中")))
        ? communicationState
        : (!deviationAvailable && status == FlowStatus::Warning)
            ? QStringLiteral("非预期流量") : statusText(status);
    // This is the operator-facing comparison state.  Alarm severity remains
    // represented by FlowStatus, while direction is retained here.
    // Do not infer a process state from a cached value.  Communication and
    // monitoring state must both be valid before a direction is shown.
    map["comparisonStatusText"] = communicationStateCode == 0
            ? QStringLiteral("正在确认通信")
        : communicationStateCode != 1 || !online || status == FlowStatus::Offline
            || status == FlowStatus::CommunicationError
            ? QStringLiteral("通信异常\n自动重试中")
        : !targetFlowAvailable ? QStringLiteral("请选择运行点")
        : !monitoringActive ? QStringLiteral("未监测")
        : !actualFlowFresh ? QStringLiteral("正在获取实际流量")
        : !deviationAvailable ? (controlState == QStringLiteral("状态未知")
            ? QStringLiteral("有效设定未确认")
            : targetConfirmed ? QStringLiteral("有效设定已同步") : QStringLiteral("尚未应用设定"))
        : status == FlowStatus::Normal ? QStringLiteral("正常")
        : realValue >= setValue ? QStringLiteral("偏高") : QStringLiteral("偏低");
    return map;
}
