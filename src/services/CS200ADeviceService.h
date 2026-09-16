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
    void requestTransactionCancellation() noexcept;
    void requestExperimentStop() noexcept;

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
