#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVariantMap>

enum class OperatingPointType { FactoryDefault, Customer };
Q_DECLARE_METATYPE(OperatingPointType)

// A run point is limited to address-keyed target/reference flows.  The legacy
// member name is retained solely for on-disk compatibility with mfcSetpoints.
class OperatingPoint
{
public:
    QString id;
    QString name;
    OperatingPointType type{OperatingPointType::Customer};
    QMap<int, double> mfcSetpoints;
    bool requiresAddressMapping{false};
    QJsonObject legacyUnmappedValues;
    bool readOnly{false};
    QString description;
    QDateTime createdAt;
    QDateTime updatedAt;

    bool isFactoryDefault() const { return type == OperatingPointType::FactoryDefault; }
    bool hasValidFlowValues() const;
    double targetFlowForAddress(int address, double fallback = 0.0) const
    { return mfcSetpoints.value(address, fallback); }

    QVariantMap toVariantMap() const;
    QJsonObject toJson() const;
    static OperatingPoint fromJson(const QJsonObject &object);
};

Q_DECLARE_METATYPE(OperatingPoint)
