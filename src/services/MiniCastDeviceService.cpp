#include "MiniCastDeviceService.h"

// TODO:
// 此安全骨架只保留给 miniCAST 6204C / PDM-U 主机本身。Sevenstar
// CS200-A MFC 已由独立的 CS200ADeviceService/CS200ADriver 实现。
// 禁止在未验证的情况下发送任何真实设备指令。
// 获得厂商协议、SDK 或经确认的抓包数据后，只在此设备层实现真实连接。
void MiniCastDeviceService::connectDevice() { emit communicationError(QStringLiteral("真实设备协议未配置")); emit deviceStatusChanged(DeviceStatus::Disconnected); }
void MiniCastDeviceService::disconnectDevice() { emit deviceStatusChanged(DeviceStatus::Disconnected); }
void MiniCastDeviceService::startMonitoring() { emit communicationError(QStringLiteral("真实设备协议未配置，无法启动采集")); }
void MiniCastDeviceService::stopMonitoring() {}
void MiniCastDeviceService::selectOperatingPoint(const OperatingPoint &) { emit communicationError(QStringLiteral("真实设备协议未配置")); }
