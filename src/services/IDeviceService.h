#pragma once

#include "models/GasChannel.h"
#include "models/OperatingPoint.h"
#include <QObject>
#include <QVariantMap>

enum class DeviceStatus {
    Disconnected,
    Scanning,
    Connecting,
    Connected,
    Monitoring,
    Warning,
    Alarm,
    CommunicationError
};
enum class FlameStatus { Unknown, Normal, Off, Fault };
Q_DECLARE_METATYPE(DeviceStatus)
Q_DECLARE_METATYPE(FlameStatus)

class IDeviceService : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    ~IDeviceService() override = default;
    virtual QList<GasChannel> currentFlows() const = 0;
    virtual DeviceStatus deviceStatus() const = 0;
    virtual FlameStatus flameStatus() const = 0;
    virtual QVariantMap deviceInfo() const = 0;
    virtual QString modeText() const = 0;

public slots:
    virtual void connectDevice() = 0;
    virtual void disconnectDevice() = 0;
    virtual void startMonitoring() = 0;
    virtual void stopMonitoring() = 0;
    // Selects local target/reference flows for comparison only.  This must not
    // result in a serial write or any MFC state change.
    virtual void selectOperatingPoint(const OperatingPoint &point) = 0;
    virtual void rescan() = 0;

signals:
    void flowDataUpdated(const QList<GasChannel> &channels);
    void deviceStatusChanged(DeviceStatus status);
    void flameStatusChanged(FlameStatus status);
    void communicationError(const QString &message);
    // Address-level health notifications are separate from actionable errors.
    // An empty value withdraws a previously shown automatic-retry notice.
    void communicationNoticeChanged(const QString &message);
    void deviceInfoChanged(const QVariantMap &info);
    void monitoringActiveChanged(bool active);
};
