#!/usr/bin/env bash
set -euo pipefail

if ! command -v apt-get >/dev/null 2>&1; then
    echo "错误：未找到 apt-get。本脚本仅适用于 Raspberry Pi OS / Debian 系统。" >&2
    exit 1
fi

packages=(
    build-essential
    cmake
    ninja-build
    qt6-base-dev
    qt6-declarative-dev
    qt6-serialport-dev
    libusb-1.0-0-dev
    libxkbcommon-dev
    qml6-module-qtquick
    qml6-module-qtquick-controls
    qml6-module-qtquick-layouts
    qml6-module-qtquick-window
)

echo "将为 Raspberry Pi OS 64-bit 安装高浓度碳烟发生器监测系统开发依赖。"
sudo apt-get update
if ! sudo apt-get install -y "${packages[@]}"; then
    echo "依赖安装失败。部分 Debian/Qt 版本的软件包名称可能不同。" >&2
    echo "请运行 apt-cache search qt6-declarative 检查当前仓库中的 Qt 6 包。" >&2
    exit 1
fi
echo "依赖安装完成。"
