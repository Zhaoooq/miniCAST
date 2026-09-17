#pragma once

#include "models/GasChannel.h"
#include "services/IDeviceService.h"
#include "services/mfc/MfcManager.h"
#include <QObject>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>

class MonitoringController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList gasChannels READ gasChannels NOTIFY gasChannelsChanged)
    Q_PROPERTY(bool monitoring READ monitoring NOTIFY monitoringChanged)
    Q_PROPERTY(QString deviceStatusText READ deviceStatusText NOTIFY deviceStatusChanged)
    Q_PROPERTY(QString flameStatusText READ flameStatusText NOTIFY flameStatusChanged)
    Q_PROPERTY(int deviceStatusCode READ deviceStatusCode NOTIFY deviceStatusChanged)
    Q_PROPERTY(int flameStatusCode READ flameStatusCode NOTIFY flameStatusChanged)
    Q_PROPERTY(QVariantMap deviceInfo READ deviceInfo NOTIFY deviceInfoChanged)
public:
    MonitoringController(int sampleInterval, double warning, double critical,
                         double zeroTolerance, QObject *parent = nullptr,
                         const QString &logDirectory = QString(), QList<MfcDeviceConfig> devices = {},
                         const QString &serialPort = QStringLiteral("auto"), int preferredBaud = 19200,
                         int ackTimeoutMs = 40, int responseTimeoutMs = 180, int retries = 1,
                         int offlineThreshold = 3, int reconnectIntervalMs = 3000,
                         int settlingTimeMs = 3000, int interRequestDelayMs = 20,
                         int freshnessTimeoutMs = 3000, int metadataInterRequestDelayMs = 20);
    ~MonitoringController() override;
    QVariantList gasChannels() const;
    QList<GasChannel> channelValues() const { return m_channels; }
    bool monitoring() const { return m_monitoring; }
    DeviceStatus deviceStatus() const { return m_deviceStatus; }
    FlameStatus flameStatus() const { return m_flameStatus; }
    QString deviceStatusText() const;
    QString flameStatusText() const;
    int deviceStatusCode() const { return static_cast<int>(m_deviceStatus); }
    int flameStatusCode() const { return static_cast<int>(m_flameStatus); }
    QVariantMap deviceInfo() const { return m_deviceInfo; }
    // The monitoring watchdog protects the polling worker, not bootstrap
    // metadata reads.  A completed READ_FLOW is the boundary at which the
    // polling worker is known to be running.
    static bool shouldArmMonitoringWatchdog(bool monitoringActive,
                                            bool readFlowObserved) noexcept {
        return monitoringActive && readFlowObserved;
    }
    static bool shouldArmMonitoringWatchdog(bool monitoringActive,
                                            bool deviceInfoScanOrResumePending,
                                            bool readFlowObserved) noexcept {
        return monitoringActive && !deviceInfoScanOrResumePending && readFlowObserved;
    }
    void selectOperatingPoint(const OperatingPoint &point);
    void setLogDirectory(const QString &directory);
    void setAddressConfirmed(int address, bool confirmed);
    void startCommunicationExperiment(int delayMs, int durationSeconds, int selectedAddress);
    void stopCommunicationExperiment();
    void startControl(const OperatingPoint &point);
    void stopControl();
    void verifyDeviceInformation();
    void runFullScaleDiagnostics();

public slots:
    void startMonitoring();
    void stopMonitoring();
    void rescan();
signals:
    void gasChannelsChanged();
    void flowSampleReceived(const QList<GasChannel> &channels);
    void monitoringChanged();
    void deviceStatusChanged();
    void flameStatusChanged();
    void errorOccurred(const QString &message);
    void communicationNoticeChanged(const QString &message);
    void deviceInfoChanged();
    void fullScaleDiagnosticsFinished();
private slots:
    void onFlows(const QList<GasChannel> &channels);
    void onDeviceStatus(DeviceStatus status);
    void onFlameStatus(FlameStatus status);
    void onDeviceInfo(const QVariantMap &info);
    void onMonitoringActive(bool active);
    void onReadFlowSucceeded();
    void onDeviceInfoScanFinished();
private:
    void disarmMonitoringWatchdogForDeviceInfo();
    QThread m_deviceThread;
    QTimer m_communicationWatchdog;
    QTimer m_guiHeartbeat;
    QElapsedTimer m_lastGuiHeartbeat;
    QElapsedTimer m_lastCommunicationProgress;
    quint64 m_lastFinishedRequestId{0};
    bool m_monitoringWatchdogArmed{false};
    bool m_waitingForPostDeviceInfoReadFlow{false};
    IDeviceService *m_service;
    QList<GasChannel> m_channels;
    QVariantMap m_deviceInfo;
    bool m_monitoring{false};
    DeviceStatus m_deviceStatus{DeviceStatus::Disconnected};
    FlameStatus m_flameStatus{FlameStatus::Unknown};
};
