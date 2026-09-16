#include "AlarmService.h"
#include <QUuid>

AlarmService::AlarmService(QObject *parent) : QObject(parent) {}

QVariantList AlarmService::alarmMaps(int filter) const
{
    QVariantList result;
    for (auto it = m_records.crbegin(); it != m_records.crend(); ++it) {
        if (filter < 0 || static_cast<int>(it->severity) == filter)
            result.append(it->toVariantMap());
    }
    return result;
}

QVariantList AlarmService::activeAlarmMaps() const
{
    QVariantList result;
    for (auto it = m_records.crbegin(); it != m_records.crend(); ++it)
        if (!it->recoveryTime.isValid()) result.append(it->toVariantMap());
    return result;
}

void AlarmService::raiseOrUpdate(const QString &key, AlarmSeverity severity,
                                 const GasChannel &channel, const QString &parameter,
                                 const QString &message)
{
    const QDateTime now = QDateTime::currentDateTime();
    const QString activeId = m_activeAlarmIds.value(key);
    for (auto &record : m_records) {
        if (record.id != activeId) continue;
        // Escalation is a new historical event; its predecessor closes once.
        if (record.severity != severity) {
            record.recoveryTime = now;
            emit alarmRecovered(record);
            m_activeAlarmIds.remove(key);
            break;
        }
        record.lastOccurrenceTime = now;
        ++record.repeatCount;
        record.realValue = channel.realValue;
        record.deviation = channel.deviationPercent;
        return;
    }
    AlarmRecord record;
    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.timestamp = now;
    record.lastOccurrenceTime = now;
    record.severity = severity;
    record.channelId = channel.id;
    record.channelName = channel.nameChinese;
    record.parameter = parameter;
    record.setValue = channel.targetFlow;
    record.realValue = channel.realValue;
    record.deviation = channel.deviationPercent;
    record.unit = channel.unit;
    record.message = message;
    m_records.append(record);
    while (m_records.size() > 5000) m_records.removeFirst();
    m_activeAlarmIds[key] = record.id;
    emit alarmRaised(record);
}

bool AlarmService::recover(const QString &key)
{
    const QString id = m_activeAlarmIds.take(key);
    if (id.isEmpty()) return false;
    for (auto &record : m_records) {
        if (record.id != id) continue;
        record.recoveryTime = QDateTime::currentDateTime();
        emit alarmRecovered(record);
        return true;
    }
    return false;
}

void AlarmService::processFlows(const QList<GasChannel> &channels)
{
    bool changed = false;
    for (const auto &channel : channels) {
        const QString commKey = channel.id + QStringLiteral(":communication");
        const bool commFault = channel.communicationStateCode == 2 || channel.communicationStateCode == 3;
        // Keep an existing communication alarm active during the required
        // 2–3 response recovery confirmation; it clears only once green.
        const bool commRecovering = channel.communicationStateCode == 4 && m_activeAlarmIds.contains(commKey);
        if (commFault || commRecovering) {
            const bool fault = channel.communicationStateCode == 3;
            AlarmSeverity severity = fault ? AlarmSeverity::Critical : AlarmSeverity::Warning;
            if (commRecovering) {
                const QString activeId = m_activeAlarmIds.value(commKey);
                for (const auto &record : m_records)
                    if (record.id == activeId) { severity = record.severity; break; }
            }
            raiseOrUpdate(commKey, severity, channel,
                          QStringLiteral("通信"),
                          QStringLiteral("%1 地址%2 %3").arg(channel.nameChinese).arg(channel.address)
                              .arg(fault ? QStringLiteral("通信故障，正在自动重试")
                                         : QStringLiteral("通信异常，正在自动重试")));
            changed = true;
        } else {
            changed |= recover(commKey);
        }

        const QString flowKey = channel.id + QStringLiteral(":flow");
        const bool flowAbnormal = channel.communicationStateCode == 1
            && (channel.status == FlowStatus::Warning || channel.status == FlowStatus::Critical)
            && (channel.deviationAvailable || channel.targetFlowAvailable);
        if (flowAbnormal) {
            raiseOrUpdate(flowKey, channel.status == FlowStatus::Critical ? AlarmSeverity::Critical : AlarmSeverity::Warning,
                          channel, QStringLiteral("流量"), channel.deviationAvailable
                              ? (channel.realValue >= channel.targetFlow ? QStringLiteral("实际流量偏高")
                                                                          : QStringLiteral("实际流量偏低"))
                              : QStringLiteral("存在非预期流量"));
            changed = true;
        } else {
            changed |= recover(flowKey);
        }
    }
    if (changed) emit alarmsChanged();
}

void AlarmService::addInformation(const QString &channel, const QString &message)
{
    AlarmRecord record;
    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.timestamp = QDateTime::currentDateTime();
    record.channelName = channel;
    record.message = message;
    record.recoveryTime = record.timestamp;
    m_records.append(record);
    while (m_records.size() > 5000) m_records.removeFirst();
    emit alarmRaised(record);
    emit alarmsChanged();
}

void AlarmService::triggerTest(AlarmSeverity severity)
{
    AlarmRecord record;
    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.timestamp = QDateTime::currentDateTime();
    record.severity = severity;
    record.channelId = QStringLiteral("test");
    record.channelName = QStringLiteral("模拟测试");
    record.parameter = QStringLiteral("测试信号");
    record.message = severity == AlarmSeverity::Critical ? QStringLiteral("测试严重报警") : QStringLiteral("测试警告");
    m_records.append(record);
    while (m_records.size() > 5000) m_records.removeFirst();
    emit alarmRaised(record);
    emit alarmsChanged();
}

void AlarmService::clear() { m_records.clear(); m_activeAlarmIds.clear(); emit alarmsChanged(); }
