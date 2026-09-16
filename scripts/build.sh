#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(dirname "$script_dir")"
build_dir="$project_dir/build"

cmake_args=(-S "$project_dir" -B "$build_dir" -GNinja -DCMAKE_BUILD_TYPE=Release)

# Older development builds may have cached a partial/private Qt installation
# that does not contain Qt SerialPort. Reconfigure that generated build tree
# against the system Qt instead of silently leaving the old executable in place.
if [[ -f "$build_dir/CMakeCache.txt" ]] \
   && grep -q '^Qt6SerialPort_DIR:PATH=Qt6SerialPort_DIR-NOTFOUND$' "$build_dir/CMakeCache.txt"; then
    echo "检测到旧 Qt 缓存缺少 SerialPort，正在刷新构建目录配置。"
    cmake_args=(--fresh "${cmake_args[@]}")
fi

cmake "${cmake_args[@]}"
cmake --build "$build_dir" --parallel 4
echo "构建完成：$build_dir/minicast-monitor"
