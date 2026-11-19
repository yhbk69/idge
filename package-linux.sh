#!/bin/bash
# ============================================================================
# package-linux.sh — RK3588 自包含分发包
#
# 产物：<输出目录>/idge-<日期>-<git哈希>-rk3588.tar.gz
# 目标板解压后 ./run.sh 即用，不需要装 Qt / OpenCV / 设环境变量：
#   - 除 glibc 核心与 GPU/显示驱动栈（libEGL/GLESv2/drm/gbm，必须与板卡内核
#     驱动配套，不能随包带）外，ldd 闭包里的所有库都收进 lib/
#   - Qt 插件（platforms/sqldrivers/imageformats/audio 等）收进 plugins/，
#     bin/qt.conf 指过去；run.sh 再兜底导出 LD_LIBRARY_PATH/QT_PLUGIN_PATH
#   - 布局保持"cwd 即工作区根"约定（runtime_paths.h）：model/ assets/
#     3rdparty/ppocrv4/bin/ 都在包根，data/ 首次运行自建、配置自动生成
#
# 前置：先在板上跑过 build-linux.sh（本脚本会再做一次增量 make + make install，
# install 时 CMake 用 $ORIGIN/../lib 重链主程序，剥掉构建期绝对 RPATH）。
#
# 用法：./package-linux.sh [-o 输出目录]   （缺省输出到仓库上级目录）
# ============================================================================
set -e

ROOT_PWD=$(cd "$(dirname "$0")" && pwd)
OUT_DIR=$(cd "$ROOT_PWD/.." && pwd)
while getopts ":o:" opt; do
  case $opt in
    o) OUT_DIR=$(cd "$OPTARG" && pwd) ;;
    *) echo "用法: $0 [-o 输出目录]"; exit 1 ;;
  esac
done

BUILD_DIR=${ROOT_PWD}/build/build_rk3588_linux
INSTALL_DIR=${ROOT_PWD}/install/rk3588_linux

# Qt 实际路径以 CMakeCache 为准（build-linux.sh 里写的路径可能已失效回退）
QT_LIB_DIR=$(grep '^Qt5_DIR:PATH=' "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2)
QT_DIR=$(cd "$QT_LIB_DIR/../../.." && pwd)
echo "Qt 目录: $QT_DIR"

VER=$(git -C "$ROOT_PWD" rev-parse --short HEAD 2>/dev/null || echo nogit)
git -C "$ROOT_PWD" diff --quiet 2>/dev/null || VER="${VER}-dirty"
PKG_NAME=idge-$(date +%Y%m%d)-${VER}-rk3588
STAGE=${ROOT_PWD}/build/package/${PKG_NAME}

# ----------------------------------------------------------------------------
# 1. 增量编译 + install（主程序在此被重链成 $ORIGIN/../lib 的部署 RPATH）
# ----------------------------------------------------------------------------
make -C "$BUILD_DIR" -j4
make -C "$BUILD_DIR" install

if readelf -d "$INSTALL_DIR/bin/idge" | grep -q 'RPATH.*\$ORIGIN/\.\./lib' \
   && ! readelf -d "$INSTALL_DIR/bin/idge" | grep -q "RPATH.*${ROOT_PWD}"; then
  echo "RPATH 校验通过: \$ORIGIN/../lib"
else
  echo "错误: install 后的 idge RPATH 不是 \$ORIGIN/../lib，检查 CMAKE_INSTALL_RPATH" >&2
  exit 1
fi

# ----------------------------------------------------------------------------
# 2. 目录骨架与资源
# ----------------------------------------------------------------------------
rm -rf "$STAGE"
mkdir -p "$STAGE"/{bin,lib,plugins,model,3rdparty}

cp "$INSTALL_DIR/bin/idge" "$STAGE/bin/"

# model 只带运行期需要的：face 两件套权重 + library 模型库。
# onnx/train/tools/audio 是开发期产物（源权重/训练脚本），不进分发包。
cp -a "$ROOT_PWD/model/face" "$ROOT_PWD/model/library" "$ROOT_PWD/model/README.md" "$STAGE/model/"
# CLI 对照工具用 install 重链过的那份（源码树里那份 RPATH 还是绝对路径）
cp "$INSTALL_DIR/model/face/face_recognition" "$STAGE/model/face/"

cp -a "$ROOT_PWD/assets" "$STAGE/"
# ppocrv4 保持 bin+lib 原结构：demo 的 RUNPATH 是 $ORIGIN/../lib，自带
# librknnrt.so 与主程序用的 rknpu2 版本可能不同，绝不能混进中央 lib/
cp -a "$ROOT_PWD/3rdparty/ppocrv4" "$STAGE/3rdparty/"
rm -rf "$STAGE/3rdparty/ppocrv4/src" 2>/dev/null || true

cp "$ROOT_PWD/config_example.json" "$STAGE/"
cp -a "$ROOT_PWD/sql" "$STAGE/"
# 桌面入口图标（关于对话框那枚云标）
cp "$ROOT_PWD/res/image/main_about.png" "$STAGE/icon.png"

# ----------------------------------------------------------------------------
# 3. Qt 插件（按需精选，避免带入 gstreamer/virtualkeyboard/wayland 长尾）
#    跳过 mediaservice：快门音 QSoundEffect 走 audio 插件，不需要 GStreamer
# ----------------------------------------------------------------------------
copy_plugins() { # $1=插件子目录 $2...=文件名（缺省整目录）
  local sub=$1; shift
  mkdir -p "$STAGE/plugins/$sub"
  if [ $# -eq 0 ]; then
    cp -a "$QT_DIR/plugins/$sub/." "$STAGE/plugins/$sub/"
  else
    for f in "$@"; do cp -a "$QT_DIR/plugins/$sub/$f" "$STAGE/plugins/$sub/"; done
  fi
}
copy_plugins platforms libqxcb.so libqeglfs.so libqlinuxfb.so libqminimal.so \
                     libqminimalegl.so libqoffscreen.so libqvnc.so
copy_plugins xcbglintegrations
copy_plugins sqldrivers
copy_plugins imageformats
copy_plugins iconengines
copy_plugins audio
copy_plugins generic
copy_plugins egldeviceintegrations
copy_plugins platforminputcontexts libcomposeplatforminputcontextplugin.so \
                                   libibusplatforminputcontextplugin.so

cat > "$STAGE/bin/qt.conf" <<'EOF'
[Paths]
Prefix = ..
Plugins = plugins
EOF

# ----------------------------------------------------------------------------
# 4. 收集 ldd 闭包进 lib/
#    排除两类：glibc 核心（随板子系统，带了反而出事）与 GPU/显示驱动栈
#    （libEGL/GLESv2/drm/gbm 必须用板卡 BSP 那份，和内核驱动配套）
# ----------------------------------------------------------------------------
EXCLUDE='^(ld-linux-.*|libc\.so\..*|libm\.so\..*|libpthread\.so\..*|libdl\.so\..*|librt\.so\..*|libresolv\.so\..*|libutil\.so\..*|libcrypt\.so\..*|libanl\.so\..*|libnsl\.so\..*|libEGL\.so.*|libGLESv2\.so.*|libGLX\.so.*|libGLdispatch\.so.*|libOpenGL\.so.*|libGL\.so.*|libdrm\.so.*|libgbm\.so.*|libmali.*)$'

# 首轮解析需要这些源目录兜底（收进 lib/ 之后就不再依赖它们）
SRC_LIBS="$ROOT_PWD/3rdparty/ffmpeg-rkmpp/lib:$ROOT_PWD/3rdparty/zlmediakit/aarch64:$ROOT_PWD/3rdparty/openssl1.1.0/lib:$ROOT_PWD/3rdparty/rknpu2/Linux/aarch64:$ROOT_PWD/3rdparty/librga/Linux/aarch64:$ROOT_PWD/3rdparty/kaldi_native_fbank/Linux/aarch64:$ROOT_PWD/3rdparty/zbar/lib:$ROOT_PWD/3rdparty/libyuv/lib:$ROOT_PWD/3rdparty/jsoncpp/lib"

collect() { # $1=种子文件；ldd 是递归闭包，一遍即可
  LD_LIBRARY_PATH="$STAGE/lib:$QT_DIR/lib:$SRC_LIBS" ldd "$1" 2>/dev/null \
  | awk '/=> \//{print $3}' | while read -r lib; do
    base=$(basename "$lib")
    echo "$base" | grep -Eq "$EXCLUDE" && continue
    [ -e "$STAGE/lib/$base" ] && continue
    cp -L "$lib" "$STAGE/lib/$base"
  done
}

collect "$STAGE/bin/idge"
collect "$STAGE/model/face/face_recognition"
find "$STAGE/plugins" -name '*.so' | while read -r p; do collect "$p"; done

# 依赖在本机就解析不了的插件直接剔除（目标板同基线更加载不了，Qt 对加载
# 失败的插件是优雅跳过）。实例：/opt 的 Qt 5.12.8 是老系统时代编的，
# libqtiff 要 libtiff.so.5 而板上只有 .so.6——它在本板也从来没能用过。
find "$STAGE/plugins" -name '*.so' | while read -r p; do
  if LD_LIBRARY_PATH="$STAGE/lib" ldd "$p" 2>/dev/null | grep -q 'not found'; then
    echo "警告: 剔除插件 $(basename "$p")（缺 $(LD_LIBRARY_PATH="$STAGE/lib" ldd "$p" 2>/dev/null | awk '/not found/{print $1}' | tr '\n' ' ')）"
    rm "$p"
  fi
done

# ----------------------------------------------------------------------------
# 5. 自包含校验：只给包内 lib/，所有种子不得再出现 not found，
#    且解析结果要么在包内、要么属于排除清单（板子系统提供）
# ----------------------------------------------------------------------------
FAIL=0
check() { # 只给包内 lib/；"not found" 行的 $3 是 "not"
  LD_LIBRARY_PATH="$STAGE/lib" ldd "$1" 2>/dev/null | awk '/=>/{print $1, $3}' \
  | while read -r name path; do
      if [ "$path" = "not" ]; then echo "缺失: $1 需要 $name" >&2; exit 1; fi
      case "$path" in
        "$STAGE"/*) ;;
        *) echo "$name" | grep -Eq "$EXCLUDE" || { echo "包外解析: $1 -> $path" >&2; exit 1; } ;;
      esac
    done
}
check "$STAGE/bin/idge" || FAIL=1
check "$STAGE/model/face/face_recognition" || FAIL=1
while read -r p; do check "$p" || FAIL=1; done < <(find "$STAGE/plugins" -name '*.so')
[ "$FAIL" = 0 ] || { echo "错误: 自包含校验失败" >&2; exit 1; }
echo "自包含校验通过"

# ----------------------------------------------------------------------------
# 6. 启动脚本与版本信息
# ----------------------------------------------------------------------------
cat > "$STAGE/run.sh" <<'EOF'
#!/bin/bash
# 解压即跑：cd 到包根满足"cwd 即工作区根"约定，data/ 首次运行自动创建
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 单实例锁：触屏场景图标可能被连点/自启与手点重叠，第二个实例直接退出。
# 锁 fd 不带 CLOEXEC，exec 后仍由 idge 持有，进程退出自动释放
exec 9>"$DIR/.idge.lock"
flock -n 9 || exit 0
export LD_LIBRARY_PATH="$DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$DIR/plugins"
cd "$DIR"
exec ./bin/idge "$@"
EOF
chmod +x "$STAGE/run.sh"

# 一键安装（开发/产线一次性操作，之后设备纯触屏使用：点图标或开机自启）。
# 用户级安装，不需要 root：版本目录 + current 软链便于升级回滚。
cat > "$STAGE/install.sh" <<'EOF'
#!/bin/bash
# 用法: ./install.sh [--no-autostart]
set -e
SRC="$(cd "$(dirname "$0")" && pwd)"
NAME=$(basename "$SRC")
BASE="$HOME/idge"
APP="$BASE/current"

if [ -e "$APP/.idge.lock" ] && ! flock -n "$APP/.idge.lock" true 2>/dev/null; then
  echo "程序正在运行，请先退出再安装" >&2; exit 1
fi

mkdir -p "$BASE/releases"
if [ "$SRC" != "$BASE/releases/$NAME" ]; then
  rm -rf "$BASE/releases/$NAME"
  cp -a "$SRC" "$BASE/releases/$NAME"
fi
ln -sfn "releases/$NAME" "$APP"

mkdir -p "$HOME/.local/share/applications" "$HOME/.config/autostart"
cat > "$HOME/.local/share/applications/idge.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=施工行为监测与分析
Comment=idge Aibox
Exec=$APP/run.sh
Icon=$APP/icon.png
Terminal=false
Categories=Utility;
StartupNotify=true
DESKTOP
desktop-file-validate "$HOME/.local/share/applications/idge.desktop" 2>/dev/null || true
update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true

if [ "$1" != "--no-autostart" ]; then
  cp "$HOME/.local/share/applications/idge.desktop" "$HOME/.config/autostart/idge.desktop"
fi

# GNOME dash 固定图标：触屏一点即开
if command -v gsettings >/dev/null 2>&1; then
  fav=$(gsettings get org.gnome.shell favorite-apps 2>/dev/null || true)
  case "$fav" in
    *idge.desktop*) ;;
    '[]'|'@as []') gsettings set org.gnome.shell favorite-apps "['idge.desktop']" ;;
    *']') gsettings set org.gnome.shell favorite-apps "${fav%]}, 'idge.desktop']" ;;
  esac
fi

echo "已安装: $APP"
echo "桌面入口: 应用网格/dash「施工行为监测与分析」"
if [ "$1" != "--no-autostart" ]; then
  echo "开机自启: ~/.config/autostart/idge.desktop（删除即取消）"
fi
EOF
chmod +x "$STAGE/install.sh"

{
  echo "package: $PKG_NAME"
  echo "built:   $(date '+%F %T')"
  echo "git:     $(git -C "$ROOT_PWD" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "qt:      $QT_DIR"
  echo "target:  rk3588 aarch64 (GPU/NPU 驱动由板卡 BSP 提供)"
} > "$STAGE/VERSION"

# ----------------------------------------------------------------------------
# 7. 打包
# ----------------------------------------------------------------------------
mkdir -p "$OUT_DIR"
tar -C "$(dirname "$STAGE")" -czf "$OUT_DIR/${PKG_NAME}.tar.gz" "$PKG_NAME"
echo "==================================="
du -sh "$STAGE" "$OUT_DIR/${PKG_NAME}.tar.gz"
echo "产物: $OUT_DIR/${PKG_NAME}.tar.gz"
echo "目标板用法: tar xzf ${PKG_NAME}.tar.gz && cd ${PKG_NAME} && ./run.sh"