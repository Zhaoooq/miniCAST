#pragma once

#include <QDateTime>
#include <QString>
#include <QVariantMap>

enum class AlarmSeverity { Info, Warning, Critical };
Q_DECLARE_METATYPE(AlarmSeverity)

class AlarmRecord
{
public:
    QString id;
    QDateTime timestamp;
    AlarmSeverity severity{AlarmSeverity::Info};
    QString channelId;
    QString channelName;
    QString parameter;
    double setValue{0.0};
    double realValue{0.0};
    double deviation{0.0};
    QString unit;
    QString message;
    QDateTime recoveryTime;
    QDateTime lastOccurrenceTime;
    int repeatCount{1};

    QVariantMap toVariantMap() const;
    static QString severityText(AlarmSeverity severity);
};

Q_DECLARE_METATYPE(AlarmRecord)
