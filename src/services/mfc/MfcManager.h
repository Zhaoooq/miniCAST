#pragma once

#include "MfcTypes.h"
#include "SerialTransport.h"
#include <QList>
#include <QMap>
#include <functional>

class CS200ADriver;

class MfcManager
{
public:
    struct Settings {
        QString serialPort{QStringLiteral("auto")};
        qint32 baudRate{19200};
        int ackTimeoutMs{40};
        int responseTimeoutMs{180};
        int retryCount{1};
        // Legacy configuration value is retained for compatibility.  A
        // device is now only considered hard-faulted after at least five
        // failed transactions (or the sustained-failure deadline below).
        int offlineFailureThreshold{5};
        int communicationWarningFailureThreshold{3};
        int communicationRecoverySuccessThreshold{3};
        int sustainedFailureTimeoutMs{3000};
        int interRequestDelayMs{20};
        // This is intentionally independent from READ_FLOW scheduling.
        int metadataInterRequestDelayMs{20};
        int freshnessTimeoutMs{3000};
    };

    MfcManager(QList<MfcDeviceConfig> devices, Settings settings,
               SerialTransport::TraceSink trace = {});
    bool connectBus();
    bool connectBusForExperiment();
    void disconnectBus();
    bool isConnected() const { return m_transport.isOpen(); }
    QString portName() const { return m_transport.portName(); }
    qint32 baudRate() const { return m_connectedBaud; }
    const QList<MfcDeviceState> &devices() const { return m_devices; }
    int onlineCount() const;
    bool hasOnlineDevice() const { return onlineCount() > 0; }
    bool allEnabledDevicesOffline() const;
    QString communicationNotice() const;
    void setMonitoringActive(bool active);
    void cancelCurrentTransaction() noexcept { m_transport.requestCancel(); }
    bool setAddressConfirmed(int address, bool confirmed);
    bool pollNext(QString *errorMessage = nullptr);
    // Poll exactly one non-normal address when its retry deadline is due.
    // Used before monitoring is started and alongside normal round-robin
    // polling so a missing MFC cannot block healthy addresses.
    bool pollRecoveryDue(QString *errorMessage = nullptr);
    bool pollExperimentFlow(int address, QString *errorMessage = nullptr);
    void setExperimentLogging(bool enabled);
    void resetExperimentSpacing() { m_transport.resetTransactionSpacing(); }
    bool physicalPortExists() const { return m_transport.physicalPortExists(); }
    QVariantMap lastTransactionDiagnostics() const;
    QVariantMap serialRuntimeConfiguration() const;
    QList<int> scanConfiguredAddresses();
    QVariantMap diagnostics() const;
    // Synchronous only on the serial worker thread.  Every command goes
    // through SerialTransport, preserving its one-pending-request invariant.
    // writesStarted is false when metadata preflight rejected the request;
    // callers must not present Stop Control for that no-write outcome.
    bool applyOperatingPoint(const QMap<int, double> &targets, QString *errorMessage = nullptr,
                             bool *writesStarted = nullptr);
    bool stopControl(QString *errorMessage = nullptr, std::function<void()> progress = {});
    // Read-only maintenance transaction.  It deliberately shares the same
    // transport and therefore cannot race a flow/control request.
    bool verifyDeviceInformation(QString *errorMessage = nullptr);
    // Read-only isolation experiment: ten Target Full Scale samples per
    // configured address followed by 32/34 alternating samples.  It does
    // not touch Preflight, address confirmation, control state or EEPROM.
    bool runFullScaleDiagnostics(QString *errorMessage = nullptr);
    bool controlling() const { return m_controlActive; }
    // Pure comparison helper used by preflight and regression tests. Gas code
    // inputs are decoded UINT16 values, not zero-padded UI strings.
    static QStringList metadataMismatchFields(const MfcDeviceConfig &config,
                                              const MfcDeviceInfo &info);

private:
    bool probeCurrentPort();
    bool pollDevice(int index, QString *errorMessage);
    void recordFailure(MfcDeviceState &state, const std::exception &error);
    void recordSuccess(MfcDeviceState &state, qint64 responseTimeMs);
    int enabledCount() const;
    QList<MfcDeviceState> m_devices;
    Settings m_settings;
    SerialTransport m_transport;
    SerialTransport::TraceSink m_trace;
    qint32 m_connectedBaud{0};
    int m_pollIndex{0};
    int m_recoveryIndex{0};
    bool m_experimentMode{false};
    bool m_controlActive{false};
    QString m_controlFailure;
    bool validateControlPreflight(const QMap<int, double> &targets, QString *errorMessage);
    QStringList readAndReportMetadata(MfcDeviceState &state, CS200ADriver &driver,
                                      bool includeCalibration = false);
    static double fullScaleSccm(const MfcDeviceConfig &config);
    void setControlState(MfcControlState state);
};
