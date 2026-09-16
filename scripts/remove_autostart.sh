#!/usr/bin/env bash
set -euo pipefail

desktop_path="${XDG_CONFIG_HOME:-$HOME/.config}/autostart/minicast-monitor.desktop"
if [[ -f "$desktop_path" ]]; then
    rm "$desktop_path"
    echo "已取消高浓度碳烟发生器监测系统开机启动。"
else
    echo "未发现高浓度碳烟发生器监测系统的开机启动项。"
fi
