#pragma once

#include <QMetaType>
#include <QDateTime>
#include <QString>
#include <QVariantMap>

enum class FlowStatus { Normal, Warning, Critical, Offline, Stabilizing, ConfigurationRequired, CommunicationError };
Q_DECLARE_METATYPE(FlowStatus)

class GasChannel
{
public:
    QString id;
    QString nameChinese;
    QString chemicalName;
    QString unit;
    // Engineering-flow fields are intentionally distinct from the raw CS200
    // percentage.  The legacy names below remain for alarms/log export.
    QString flowUnit;
    double fullScaleValue{0.0};
    double currentFlow{0.0};
    // actualFlow is exclusively a decoded, valid READ_FLOW result.  It is
    // never changed by start/stop control, link changes, or UI refreshes.
    double actualFlow{0.0};
    double lastValidActualFlow{0.0};
    QDateTime lastValidFlowTimestamp;
    // User-defined comparison baseline from the selected operating point.
    // It is never read from or written to the MFC.
    double targetFlow{0.0};
    double configuredTargetFlow{0.0};
    double activeTargetFlow{0.0};
    double percentFullScale{0.0};
    double valveOpening{0.0};
    double minimum{0.0};
    double maximum{0.0};
    double setValue{0.0};
    double realValue{0.0};
    double temperature{0.0};
    double deviationPercent{0.0};
    double deviation{0.0};
    FlowStatus status{FlowStatus::Normal};
    bool adjustable{true};
    bool online{true};
    bool actualValueAvailable{false};
    bool currentFlowAvailable{false};
    // A legal READ_FLOW sample is valid even when its engineering value is
    // exactly zero (for example, while the gas source is closed).
    bool hasValidActualFlow{false};
    bool actualFlowFresh{false};
    bool waitingForActualFlow{true};
    bool flowFresh{false};
    bool targetFlowAvailable{false};
    bool configuredTargetFlowAvailable{false};
    bool valveOpeningAvailable{false};
    bool addressDetected{false};
    bool addressConfirmed{false};
    bool engineeringConfigured{false};
    bool readOnly{false};
    bool deviationAvailable{true};
    // Presentation guard: comparison values are meaningful only during an
    // active read-only monitoring session.  It does not affect MFC protocol.
    bool monitoringActive{false};
    int address{0};
    QString gasType{QStringLiteral("待确认")};
    QString function{QStringLiteral("待确认")};
    QString communicationState;
    // MfcCommunicationState is carried separately from the translated text so
    // each card/alarm can make an address-local decision without inferring it
    // from global device health.
    int communicationStateCode{0};
    QString configurationState;
    QString controlState;
    QString effectiveSetpointState;
    int controlStateCode{0};
    bool targetConfirmed{false};
    QString stopState;
    QString stopFailure;
    QString operatingPointId;
    double responseTimeMs{0.0};
    double tolerancePercent{5.0};

    QVariantMap toVariantMap() const;
    static QString statusText(FlowStatus status);
};

Q_DECLARE_METATYPE(GasChannel)
Q_DECLARE_METATYPE(QList<GasChannel>)
