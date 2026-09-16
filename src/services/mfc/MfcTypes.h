#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QList>

enum class MfcControlMode { Unknown = 0, Digital = 1, AnalogVoltage = 2, AnalogCurrent = 3 };
enum class MfcValveState { Normal = 0, Closed = 1, Purge = 2, Unknown = 255 };
enum class MfcLinkState { Online, Timeout, Error, Offline, NotDiscovered };
// This is deliberately independent of the transport/link result for one
// transaction.  It represents the health trend for one configured address.
enum class MfcCommunicationState { Unknown, Normal, Degraded, Fault, Recovering };
enum class MfcControlState { Monitoring, PreparingControl, SettingDigitalMode, Holding,
    LoadingSetpoints, VerifyingSetpoints, StartingControl, Controlling, ControlDegraded,
    StoppingControl, ControlStopped, ControlFault };

struct MfcDeviceConfig
{
    int logicalChannel{0};
    quint8 address{0x20};
    QString displayName;
    QString gasType{QStringLiteral("待确认")};
    QString function{QStringLiteral("待确认")};
    QString unit;
    double fullScale{0.0};
    // Site-verified configuration; all conversion, UI and identity checks
    // consume these values rather than branching on address elsewhere.
    quint16 expectedGasCode{0};
    double expectedDeviceFullScaleSccm{0.0};
    double minimumSetpoint{0.0};
    double maximumSetpoint{0.0};
    double absoluteTolerance{0.0};
    double relativeTolerancePercent{5.0};
    bool enabled{true};
    bool required{true};
    bool addressConfirmed{false};

    QString stableId() const { return QStringLiteral("mfc_%1").arg(address); }
    QVariantMap toVariantMap() const;
};

struct MfcDeviceInfo
{
    QString port;
    QString portDescription;
    QString portManufacturer;
    QString usbSerialNumber;
    quint16 vendorId{0};
    quint16 productId{0};
    quint8 address{0x20};
    qint32 baudRate{19200};
    QString manufacturer;
    QString model;
    QString serialNumber;
    QString firmware;
    QString pcbRevision;
    QString manufacturingDate;
    QString calibrationDate;
    QString targetGasName;
    quint16 targetGasCode{0};
    // Class 0x66 Target Gas attributes (0x01..0x03).  fullScale remains as
    // the legacy alias used by existing flow displays and is always the
    // Target Gas full scale, never a calibration value.
    quint16 targetGasFullScale{0};
    QString calibrationGasName;
    quint16 calibrationGasCode{0};
    quint16 calibrationGasFullScale{0};
    bool calibrationGasReadFromDevice{false};
    double fullScale{0.0};
    QString fullScaleUnit;
    // Production monitoring deliberately does not read this metadata register;
    // conversion comes from operator-confirmed configuration.
    bool fullScaleReadFromDevice{false};
    double conversionFactor{0.0};
    MfcControlMode controlMode{MfcControlMode::Unknown};
    MfcValveState valveState{MfcValveState::Unknown};
    QVariantMap toVariantMap() const;
};

struct MfcReading
{
    QDateTime timestamp;
    quint16 flowRaw{0};
    double flowPercent{0.0};
    double flowValue{0.0};
    double temperature{0.0};
    QStringList warnings;
    QStringList alarms;
    bool communicationOk{false};
};

// State of one address in a Stop Control transaction.  These are deliberately
// separate from communication state: an MFC can be online yet have an
// unconfirmed zero setpoint, and a failed address must not prevent the other
// addresses from being brought to zero.
enum class MfcStopStage { NotRequested, WritingZero, DigitalVerified, ActiveVerified, Unconfirmed };

struct MfcDeviceState
{
    MfcDeviceConfig config;
    MfcDeviceInfo info;
    MfcReading reading;
    MfcLinkState linkState{MfcLinkState::NotDiscovered};
    // These flags deliberately describe independent state dimensions.  In
    // particular, an online/detected MFC may be readable while its engineering
    // conversion configuration still needs confirmation.
    bool communicationOnline{false};
    bool addressDetected{false};
    bool engineeringConfigured{false};
    bool monitoringActive{false};
    bool metadataAvailable{false};
    // Metadata is read field-by-field.  Keeping the result for each field
    // prevents an unavailable register from being presented as a mismatch.
    bool metadataVerificationAttempted{false};
    bool metadataVerificationComplete{false};
    QVariantMap metadataResults;
    // Read-only evidence from the repeat/alternating Full Scale experiment.
    // It is deliberately separate from the site configuration and control
    // state so it can never alter address mapping or a setpoint.
    QVariantMap metadataDiagnostics;
    bool dataFresh{false};
    QDateTime lastUpdateTime;
    QDateTime lastSuccessTime;
    QDateTime firstFailureTime;
    // A failed address is retried by the single serial scheduler no more
    // frequently than this timestamp.  It never creates a parallel request.
    QDateTime nextRetryTime;
    quint64 successCount{0};
    quint64 errorCount{0};
    quint64 timeoutCount{0};
    quint64 protocolErrorCount{0};
    quint64 checksumErrorCount{0};
    quint64 serviceErrorCount{0};
    quint64 serialErrorCount{0};
    double totalResponseTimeMs{0.0};
    double maximumResponseTimeMs{0.0};
    int consecutiveFailures{0};
    int consecutiveSuccesses{0};
    MfcCommunicationState communicationState{MfcCommunicationState::Unknown};
    double responseTimeMs{0.0};
    QString lastError;
    MfcControlState controlState{MfcControlState::Monitoring};
    double targetFlow{0.0};
    bool targetConfirmed{false};
    QDateTime lastControlConfirmation;
    MfcStopStage stopStage{MfcStopStage::NotRequested};
    QString stopFailure;

    void refreshCapabilities();
    QVariantMap toVariantMap() const;
};
