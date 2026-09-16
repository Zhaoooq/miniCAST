#include "OperatingPoint.h"

#include <cmath>

QVariantMap OperatingPoint::toVariantMap() const
{
    QVariantMap targetFlows;
    for (auto it = mfcSetpoints.cbegin(); it != mfcSetpoints.cend(); ++it)
        targetFlows[QString::number(it.key())] = it.value();
    return {{"id", id}, {"name", name}, {"type", static_cast<int>(type)},
            {"typeText", isFactoryDefault() ? QStringLiteral("系统预设") : QStringLiteral("客户运行点")},
            {"mfcTargetFlows", targetFlows},
            // Legacy QML/extensions may still read this key.  Its values are
            // targets, never MFC control setpoints.
            {"mfcSetpoints", targetFlows}, {"requiresAddressMapping", requiresAddressMapping},
            {"isDefault", isFactoryDefault()}, {"legacyUnmappedValues", legacyUnmappedValues.toVariantMap()},
            {"readOnly", readOnly || isFactoryDefault()}, {"description", description},
            {"createdAt", createdAt.toString(Qt::ISODate)}, {"updatedAt", updatedAt.toString(Qt::ISODate)}};
}

QJsonObject OperatingPoint::toJson() const
{
    auto object = QJsonObject::fromVariantMap(toVariantMap());
    object["type"] = isFactoryDefault() ? QStringLiteral("FactoryDefault") : QStringLiteral("Customer");
    QJsonObject targetFlows;
    for (auto it = mfcSetpoints.cbegin(); it != mfcSetpoints.cend(); ++it)
        targetFlows[QString::number(it.key())] = it.value();
    object["mfcTargetFlows"] = targetFlows;
    // Keep the historical key for lossless downgrade/import compatibility.
    object["mfcSetpoints"] = targetFlows;
    object["legacyUnmappedValues"] = legacyUnmappedValues;
    object["schemaVersion"] = 4;
    return object;
}

OperatingPoint OperatingPoint::fromJson(const QJsonObject &object)
{
    OperatingPoint point;
    point.id = object["id"].toString();
    point.name = object["name"].toString();
    // Legacy titles can expose retired process terminology even though their
    // values are no longer part of a run point. Replace only such titles while
    // retaining the original address-mapping record for operator review.
    if (point.name.contains(QStringLiteral("颗粒")) || point.name.contains(QStringLiteral("粒径"))
        || point.name.contains(QStringLiteral("particle"), Qt::CaseInsensitive))
        point.name = QStringLiteral("旧运行点（待地址确认）");
    const bool legacyDefault = object["isDefault"].toBool(false);
    point.type = object["type"].toString() == QStringLiteral("FactoryDefault")
            || object["type"].toInt(-1) == static_cast<int>(OperatingPointType::FactoryDefault)
            || legacyDefault ? OperatingPointType::FactoryDefault : OperatingPointType::Customer;
    const auto targetFlows = object.contains("mfcTargetFlows")
        ? object["mfcTargetFlows"].toObject() : object["mfcSetpoints"].toObject();
    for (auto it = targetFlows.begin(); it != targetFlows.end(); ++it) {
        bool addressOk = false;
        const int address = it.key().toInt(&addressOk);
        if (addressOk && address >= 0x20 && address <= 0x5f && it.value().isDouble()
            && std::isfinite(it.value().toDouble()))
            point.mfcSetpoints[address] = it.value().toDouble();
    }
    point.legacyUnmappedValues = object["legacyUnmappedValues"].toObject();
    for (const char *key : {"fuelGas", "mixingGas", "oxidationAir", "dilutionAir", "quenchGas"}) {
        const auto name = QLatin1String(key);
        if (object.contains(name) && !point.legacyUnmappedValues.contains(name))
            point.legacyUnmappedValues.insert(name, object.value(name));
    }
    point.requiresAddressMapping = object["requiresAddressMapping"].toBool(false)
        || !point.legacyUnmappedValues.isEmpty();
    point.readOnly = object["readOnly"].toBool(point.isFactoryDefault());
    point.description = object["description"].toString();
    point.createdAt = QDateTime::fromString(object["createdAt"].toString(), Qt::ISODate);
    point.updatedAt = QDateTime::fromString(object["updatedAt"].toString(), Qt::ISODate);
    return point;
}

bool OperatingPoint::hasValidFlowValues() const
{
    if (mfcSetpoints.isEmpty()) return false;
    for (auto it = mfcSetpoints.cbegin(); it != mfcSetpoints.cend(); ++it)
        if (it.key() < 0x20 || it.key() > 0x5f || !std::isfinite(it.value()) || it.value() < 0.0)
            return false;
    return !requiresAddressMapping;
}
