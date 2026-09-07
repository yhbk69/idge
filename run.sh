#!/bin/bash
# IDGE 启动脚本 - 设置 Mali EGL 和 rkmpp FFmpeg 库路径
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

export DISPLAY="${DISPLAY:-:0}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export LD_LIBRARY_PATH="/usr/lib/aarch64-linux-gnu/mali:${SCRIPT_DIR}/3rdparty/ffmpeg-rkmpp/lib:/usr/local/Qt-5.15.18/lib:${LD_LIBRARY_PATH}"

exec "${SCRIPT_DIR}/build/build_rk3588_linux/idge" "$@"
