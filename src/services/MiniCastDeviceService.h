#pragma once

#include "IDeviceService.h"

class MiniCastDeviceService final : public IDeviceService
{
    Q_OBJECT
public:
    using IDeviceService::IDeviceService;
    QList<GasChannel> currentFlows() const override { return {}; }
    DeviceStatus deviceStatus() const override { return DeviceStatus::Disconnected; }
    FlameStatus flameStatus() const override { return FlameStatus::Unknown; }
    QVariantMap deviceInfo() const override { return {}; }
    QString modeText() const override { return QStringLiteral("未配置"); }
public slots:
    void connectDevice() override;
    void disconnectDevice() override;
    void startMonitoring() override;
    void stopMonitoring() override;
    void selectOperatingPoint(const OperatingPoint &point) override;
    void rescan() override { connectDevice(); }
};
