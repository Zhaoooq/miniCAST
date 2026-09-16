#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(dirname "$script_dir")"
binary_path="${1:-$project_dir/build/minicast-monitor}"
autostart_dir="${XDG_CONFIG_HOME:-$HOME/.config}/autostart"
desktop_path="$autostart_dir/minicast-monitor.desktop"

if [[ ! -x "$binary_path" ]]; then
    echo "错误：未找到可执行程序：$binary_path" >&2
    echo "请先运行 scripts/build.sh，或将可执行程序路径作为第一个参数传入。" >&2
    exit 1
fi

mkdir -p "$autostart_dir"
sed "s|^Exec=.*|Exec=$binary_path|" "$project_dir/deployment/minicast-monitor.desktop" > "$desktop_path"
chmod 644 "$desktop_path"
echo "已启用桌面会话开机启动：$desktop_path"

