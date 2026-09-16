#include "AlarmRecord.h"

QString AlarmRecord::severityText(AlarmSeverity value)
{
    switch (value) {
    case AlarmSeverity::Info: return QStringLiteral("提示");
    case AlarmSeverity::Warning: return QStringLiteral("警告");
    case AlarmSeverity::Critical: return QStringLiteral("严重");
    }
    return QStringLiteral("提示");
}

QVariantMap AlarmRecord::toVariantMap() const
{
    return {{"id", id}, {"timestamp", timestamp.toString("yyyy-MM-dd HH:mm:ss")},
            {"time", timestamp.toString("HH:mm:ss")}, {"severity", static_cast<int>(severity)},
            {"severityText", severityText(severity)}, {"channelId", channelId},
            {"channelName", channelName}, {"parameter", parameter}, {"setValue", setValue},
            {"realValue", realValue}, {"deviation", deviation}, {"unit", unit},
            {"message", message},
            {"firstOccurrenceTime", timestamp.toString("yyyy-MM-dd HH:mm:ss")},
            {"lastOccurrenceTime", (lastOccurrenceTime.isValid() ? lastOccurrenceTime : timestamp).toString("yyyy-MM-dd HH:mm:ss")},
            {"repeatCount", repeatCount},
            {"durationSeconds", timestamp.secsTo(recoveryTime.isValid() ? recoveryTime : QDateTime::currentDateTime())},
            {"recoveryTime", recoveryTime.isValid() ? recoveryTime.toString("yyyy-MM-dd HH:mm:ss") : QStringLiteral("未恢复")},
            {"recovered", recoveryTime.isValid()}};
}
