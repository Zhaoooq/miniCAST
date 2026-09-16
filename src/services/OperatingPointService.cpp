#include "OperatingPointService.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <QRegularExpression>
#include <QSet>
#include <cmath>

namespace {
}

OperatingPointService::OperatingPointService(const QString &root, QObject *parent,
                                             QList<MfcDeviceConfig> devices)
    : QObject(parent), m_path(root + "/operating_points.json"), m_devices(std::move(devices)) { load(); }

bool OperatingPointService::setDataRoot(const QString &root)
{
    const QString newPath = QDir::cleanPath(root) + QStringLiteral("/operating_points.json");
    if (newPath == m_path) return true;
    const QString previousPath = m_path;
    m_path = newPath;
    if (save()) return true;
    m_path = previousPath;
    return false;
}

void OperatingPointService::load()
{
    // A configuration placeholder is not a valid operating point.  The list
    // starts empty until an actual customer point is created or loaded.
    m_points.clear();
    QFile file(m_path);
    if (file.open(QIODevice::ReadOnly)) {
        const auto array = QJsonDocument::fromJson(file.readAll()).array();
        for (const auto &value : array) {
            auto point = OperatingPoint::fromJson(value.toObject());
            if (point.id.isEmpty() || point.isFactoryDefault()) continue;
            point.type = OperatingPointType::Customer;
            point.readOnly = false;
            if (!point.createdAt.isValid()) point.createdAt = QDateTime::currentDateTimeUtc();
            if (!point.updatedAt.isValid()) point.updatedAt = point.createdAt;
            m_points.append(point);
        }
    }
    save();
}

bool OperatingPointService::save() const
{
    QJsonArray array;
    for (const auto &point : m_points)
        if (!point.isFactoryDefault()) array.append(point.toJson());
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    if (file.write(QJsonDocument(array).toJson(QJsonDocument::Indented)) < 0) return false;
    return file.commit();
}

QVariantList OperatingPointService::pointMaps() const
{
    QVariantList result;
    for (const auto &point : m_points) result.append(point.toVariantMap());
    return result;
}

OperatingPoint OperatingPointService::point(const QString &id) const
{
    for (const auto &point : m_points) if (point.id == id) return point;
    return {};
}

bool OperatingPointService::addOrUpdate(const OperatingPoint &input)
{
    OperatingPoint point = input;
    point.name = point.name.trimmed();
    if (point.name.isEmpty() || !point.hasValidFlowValues()
        || !targetsWithinConfiguredRanges(point))
        return false;
    const auto now = QDateTime::currentDateTimeUtc();
    point.type = OperatingPointType::Customer;
    point.readOnly = false;
    if (point.id.isEmpty()) {
        if (customerPointFull()) return false;
        point.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        point.createdAt = now;
    }
    point.updatedAt = now;
    for (auto &existing : m_points) {
        if (existing.id == point.id) {
            if (existing.isFactoryDefault() || existing.readOnly) return false;
            if (!point.createdAt.isValid()) point.createdAt = existing.createdAt;
            existing = point;
            const bool ok = save();
            if (ok) emit pointsChanged();
            return ok;
        }
    }
    m_points.append(point);
    const bool ok = save();
    if (ok) emit pointsChanged();
    return ok;
}

bool OperatingPointService::targetsWithinConfiguredRanges(const OperatingPoint &point) const
{
    // The service is also used by import/legacy tests without a plant
    // configuration.  In production it receives the fixed five MFC devices
    // and rejects targets that would exceed their current field ranges.
    if (m_devices.isEmpty()) return true;

    QSet<int> configuredAddresses;
    for (const auto &device : m_devices) configuredAddresses.insert(device.address);
    for (auto it = point.mfcSetpoints.cbegin(); it != point.mfcSetpoints.cend(); ++it)
        if (!configuredAddresses.contains(it.key())) return false;

    for (const auto &device : m_devices) {
        if (!device.enabled) continue;
        if (!point.mfcSetpoints.contains(device.address)) {
            if (device.required) return false;
            continue;
        }
        const double maximum = device.maximumSetpoint > 0.0
            ? device.maximumSetpoint : device.fullScale;
        const double value = point.mfcSetpoints.value(device.address);
        if (maximum <= 0.0 || !std::isfinite(value)
            || value < device.minimumSetpoint || value > maximum)
            return false;
    }
    return true;
}

bool OperatingPointService::remove(const QString &id)
{
    for (qsizetype i = 0; i < m_points.size(); ++i) {
        if (m_points[i].id == id && !m_points[i].isFactoryDefault() && !m_points[i].readOnly) {
            m_points.removeAt(i);
            const bool ok = save();
            if (ok) emit pointsChanged();
            return ok;
        }
    }
    return false;
}

OperatingPoint OperatingPointService::duplicate(const QString &id)
{
    if (customerPointFull()) return {};
    auto copy = point(id);
    if (copy.id.isEmpty()) return {};
    copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    copy.name += QStringLiteral(" 副本");
    copy.type = OperatingPointType::Customer;
    copy.readOnly = false;
    copy.createdAt = copy.updatedAt = QDateTime::currentDateTimeUtc();
    if (!addOrUpdate(copy)) copy.id.clear();
    return copy;
}

int OperatingPointService::customerPointCount() const
{
    int count = 0;
    for (const auto &point : m_points)
        if (!point.isFactoryDefault()) ++count;
    return count;
}

bool OperatingPointService::customerPointFull() const
{
    return customerPointCount() >= MaximumCustomerPoints;
}

QString OperatingPointService::nextCustomerName() const
{
    QSet<int> usedSlots;
    const QRegularExpression expression(QStringLiteral("^客户运行点\\s*(\\d+)$"));
    for (const auto &point : m_points) {
        if (point.isFactoryDefault()) continue;
        const auto match = expression.match(point.name);
        if (match.hasMatch()) usedSlots.insert(match.captured(1).toInt());
    }
    for (int slot = 1; slot <= MaximumCustomerPoints; ++slot)
        if (!usedSlots.contains(slot)) return QStringLiteral("客户运行点 %1").arg(slot);
    return QStringLiteral("客户运行点 %1").arg(customerPointCount() + 1);
}
