#!/bin/bash
# IDGE 启动脚本 - 设置 Mali EGL 和 rkmpp FFmpeg 库路径
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

export DISPLAY="${DISPLAY:-:0}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export LD_LIBRARY_PATH="/usr/lib/aarch64-linux-gnu/mali:${SCRIPT_DIR}/3rdparty/ffmpeg-rkmpp/lib:/usr/local/Qt-5.15.18/lib:${LD_LIBRARY_PATH}"

# 每次运行单独存一份日志：logs/idge_<时间戳>_pid<pid>.log
# tee 保证终端照常可见，文件里是完整 stdout+stderr 副本。
LOG_DIR="${SCRIPT_DIR}/logs"
mkdir -p "$LOG_DIR"
LOG_FILE="${LOG_DIR}/idge_$(date +%Y%m%d_%H%M%S)_pid$$.log"
echo "[run.sh] 日志保存到: $LOG_FILE"
"${SCRIPT_DIR}/build/build_rk3588_linux/idge" "$@" 2>&1 | tee "$LOG_FILE"
