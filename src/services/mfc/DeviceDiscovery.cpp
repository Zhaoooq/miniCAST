#include "DeviceDiscovery.h"
#include <QSerialPortInfo>
#include <QDir>
#include <QFileInfo>
#include <QSet>

QVariantMap SerialPortDescriptor::toVariantMap() const
{
    return {{"device", device}, {"description", description}, {"manufacturer", manufacturer},
            {"vid", vendorId}, {"pid", productId}, {"serialNumber", serialNumber}};
}

QList<SerialPortDescriptor> DeviceDiscovery::candidates()
{
    QList<SerialPortDescriptor> result;
    QSet<QString> canonicalDevices;
    const QDir byId(QStringLiteral("/dev/serial/by-id"));
    for (const auto &entry : byId.entryInfoList(QDir::System | QDir::Files | QDir::NoDotAndDotDot,
                                                QDir::Name)) {
        const QString canonical = entry.canonicalFilePath();
        if (canonical.isEmpty() || canonicalDevices.contains(canonical)) continue;
        canonicalDevices.insert(canonical);
        // Keep the stable symlink as the opened path and its filename as a useful description.
        result.append({entry.absoluteFilePath(), entry.fileName(), QString(), 0, 0, QString()});
    }
    for (const auto &port : QSerialPortInfo::availablePorts()) {
        const QString path = port.systemLocation();
        if (!path.startsWith(QStringLiteral("/dev/ttyUSB"))
            && !path.startsWith(QStringLiteral("/dev/ttyACM"))) continue;
        const QString canonical = QFileInfo(path).canonicalFilePath();
        if (!canonical.isEmpty() && canonicalDevices.contains(canonical)) continue;
        if (!canonical.isEmpty()) canonicalDevices.insert(canonical);
        result.append({path, port.description(), port.manufacturer(),
                       port.hasVendorIdentifier() ? port.vendorIdentifier() : quint16(0),
                       port.hasProductIdentifier() ? port.productIdentifier() : quint16(0),
                       port.serialNumber()});
    }
    return result;
}
