#pragma once

#include "models/AlarmRecord.h"
#include "models/GasChannel.h"
#include "services/IDeviceService.h"
#include <QFile>
#include <QObject>

class DataLoggingService : public QObject
{
    Q_OBJECT
public:
    explicit DataLoggingService(const QString &dataRoot, QObject *parent = nullptr);
    ~DataLoggingService() override;
    bool start();
    void stop();
    void logFlows(const QList<GasChannel> &channels, DeviceStatus device, FlameStatus flame);
    void logAlarm(const AlarmRecord &record, const QString &state);
    void logApplication(const QString &message);
    bool setDataRoot(const QString &dataRoot);
    QString lastError() const { return m_lastError; }
private:
    bool ensureFlowFile();
    QString m_root;
    QFile m_flowFile;
    QFile m_appFile;
    QString m_flowDate;
    QString m_lastError;
    bool m_active{false};
};
