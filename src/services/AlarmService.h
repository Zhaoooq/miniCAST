#pragma once

#include "models/AlarmRecord.h"
#include "models/GasChannel.h"
#include <QObject>
#include <QHash>

class AlarmService : public QObject
{
    Q_OBJECT
public:
    explicit AlarmService(QObject *parent = nullptr);
    QVariantList alarmMaps(int severityFilter = -1) const;
    QVariantList activeAlarmMaps() const;
    QList<AlarmRecord> records() const { return m_records; }
    void processFlows(const QList<GasChannel> &channels);
    void addInformation(const QString &channel, const QString &message);
    void triggerTest(AlarmSeverity severity);
    void clear();
signals:
    void alarmsChanged();
    void alarmRaised(const AlarmRecord &record);
    void alarmRecovered(const AlarmRecord &record);
private:
    void raiseOrUpdate(const QString &key, AlarmSeverity severity, const GasChannel &channel,
                       const QString &parameter, const QString &message);
    bool recover(const QString &key);
    QList<AlarmRecord> m_records;
    QHash<QString, QString> m_activeAlarmIds;
};
