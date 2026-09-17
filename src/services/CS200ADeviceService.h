#pragma once

#include "IDeviceService.h"
#include "mfc/MfcLog.h"
#include "mfc/MfcManager.h"
#include "mfc/Cs200CommExperiment.h"
#include <QElapsedTimer>
#include <QTimer>
#include <atomic>
#include <memory>
#include <mutex>

class CS200ADeviceService final : public IDeviceService
{
    Q_OBJECT
public:
    CS200ADeviceService(int sampleIntervalMs, double warningPercent,
                        double criticalPercent, double zeroTolerance,
                        QList<MfcDeviceConfig> devices, MfcManager::Settings settings,
                        int reconnectIntervalMs, int settlingTimeMs,
                        QString logDirectory, QObject *parent = nullptr);
    ~CS200ADeviceService() override;

    QList<GasChannel> currentFlows() const override { return m_channels; }
    DeviceStatus deviceStatus() const override { return m_status; }
    FlameStatus flameStatus() const override { return FlameStatus::Unknown; }
    QVariantMap deviceInfo() const override;
    QString modeText() const override { return QStringLiteral("CS200 真机"); }
    // Thread-safe emergency signal used by the GUI thread to interrupt a
    // finite serial wait before the queued stop slot is serviced.
    void requestTransactionCancellation(SerialTransport::CancelReason reason =
                                        SerialTransport::CancelReason::Unspecified) noexcept;
    void requestExperimentStop() noexcept;
    // Called on the GUI thread before a metadata job is queued.  Reserving the
    // mode switch closes the small interval in which the GUI watchdog could
    // otherwise fire just as the worker is about to leave NORMAL_POLLING.
    void reserveDeviceInfoScan() noexcept;
    bool monitoringWatchdogSuspended() const noexcept {
        return m_deviceInfoScanReserved.load() || m_deviceInfoScanActive.load()
            || m_waitingForPostDeviceInfoReadFlow.load();
    }

public slots:
    void connectDevice() override;
    void disconnectDevice() override;
    void startMonitoring() override;
    void stopMonitoring() override;
    void selectOperatingPoint(const OperatingPoint &point) override;
    void rescan() override;
    void setLogDirectory(const QString &directory);
    void setAddressConfirmed(int address, bool confirmed);
    void recoverCommunicationFromWatchdog();
    void startCommunicationExperiment(int delayMs, int durationSeconds, int selectedAddress);
    void stopCommunicationExperiment();
    void startControl(const OperatingPoint &point);
    void stopControl();
    void verifyDeviceInformation();
    void runFullScaleDiagnostics();
    void recordExperimentGuiStall(qint64 stallMs);
    void recordExperimentWatchdogTrigger();

signals:
    // This is a UI lifecycle notification only.  The diagnostic itself stays
    // on the existing single serial worker and does not change control state.
    void fullScaleDiagnosticsFinished();
    // Emitted only for a successful READ_FLOW.  It is deliberately separate
    // from flowDataUpdated(), which is also emitted after failed attempts and
    // UI rebuilds.
    void readFlowSucceeded();
    void deviceInfoScanFinished();

private slots:
    void poll();
    void retryUnhealthyAddress();
    void reconnectIfNeeded();
    void pollExperiment();

private:
    void setStatus(DeviceStatus status);
    void rebuildChannels();
    void updateCommunicationNotice();
    void setOperationState(const QString &state);
    void beginDeviceInfoScan(const QString &origin);
    void finishDeviceInfoScan(const QString &origin, bool success);
    QList<MfcDeviceConfig> m_configs;
    QList<GasChannel> m_channels;
    std::unique_ptr<MfcManager> m_manager;
    // requestTransactionCancellation() may run directly on the GUI thread.
    // This mutex protects only the manager object's lifetime; normal manager
    // operations remain confined to the communication worker thread.
    mutable std::mutex m_managerLifetimeMutex;
    MfcManager::Settings m_settings;
    MfcLog m_log;
    QTimer *m_pollTimer{nullptr};
    QTimer *m_recoveryTimer{nullptr};
    QTimer *m_reconnectTimer{nullptr};
    QTimer *m_experimentTimer{nullptr};
    DeviceStatus m_status{DeviceStatus::Disconnected};
    int m_sampleIntervalMs;
    double m_warningPercent;
    double m_criticalPercent;
    double m_zeroTolerance;
    int m_settlingTimeMs;
    bool m_monitoring{false};
    // The service thread owns mode changes, while the GUI watchdog reads these
    // atomics directly.  They are intentionally not a second serial worker.
    std::atomic_bool m_deviceInfoScanReserved{false};
    std::atomic_bool m_deviceInfoScanActive{false};
    std::atomic_bool m_waitingForPostDeviceInfoReadFlow{false};
    std::atomic_bool m_lifecycleCancellationRequested{false};
    bool m_resumePollingAfterDeviceInfo{false};
    bool m_resumeRecoveryAfterDeviceInfo{false};
    std::atomic_bool m_experimentStopRequested{false};
    Cs200CommExperiment m_experiment;
    QString m_experimentJsonReport;
    QString m_experimentTextReport;
    QString m_operationState{QStringLiteral("IDLE")};
    QString m_communicationNotice;
    QMap<int, double> m_targetFlows;
    QMap<int, double> m_configuredTargetFlows;
    bool m_controlSession{false};
    QString m_activeOperatingPointId;
    void finishCommunicationExperiment(const QString &reason);
};
