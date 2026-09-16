#pragma once

#include "models/OperatingPoint.h"
#include "services/mfc/MfcTypes.h"
#include <QObject>

class OperatingPointService : public QObject
{
    Q_OBJECT
public:
    explicit OperatingPointService(const QString &dataRoot, QObject *parent = nullptr,
                                   QList<MfcDeviceConfig> devices = {});
    QVariantList pointMaps() const;
    QList<OperatingPoint> points() const { return m_points; }
    OperatingPoint point(const QString &id) const;
    bool addOrUpdate(const OperatingPoint &point);
    bool remove(const QString &id);
    OperatingPoint duplicate(const QString &id);
    QString nextCustomerName() const;
    bool customerPointFull() const;
    int customerPointCount() const;
    bool setDataRoot(const QString &dataRoot);
    static constexpr int MaximumCustomerPoints = 6;
signals:
    void pointsChanged();
private:
    void load();
    bool save() const;
    bool targetsWithinConfiguredRanges(const OperatingPoint &point) const;
    QList<OperatingPoint> m_points;
    QString m_path;
    QList<MfcDeviceConfig> m_devices;
};
