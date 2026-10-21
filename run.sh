#!/bin/bash
# 运行 idge 应用程序

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD_DIR="${SCRIPT_DIR}/build/build_rk3588_linux"

if [ ! -f "${BUILD_DIR}/idge" ]; then
    echo "错误: 可执行文件不存在，请先编译："
    echo "  ./build-linux.sh -t rk3588 -b Release"
    exit 1
fi

cd "${BUILD_DIR}"
./idge "$@"
