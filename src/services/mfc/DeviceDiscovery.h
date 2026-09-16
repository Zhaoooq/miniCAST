#pragma once

#include <QList>
#include <QString>
#include <QVariantMap>

struct SerialPortDescriptor
{
    QString device;
    QString description;
    QString manufacturer;
    quint16 vendorId{0};
    quint16 productId{0};
    QString serialNumber;
    QVariantMap toVariantMap() const;
};

class DeviceDiscovery
{
public:
    static QList<SerialPortDescriptor> candidates();
};
