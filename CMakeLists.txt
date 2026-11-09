# ============================================================================
# CMakeLists.txt - idge 项目构建配置
# ============================================================================
#
# 项目概述：
#   idge 是一个基于 Rockchip RK3588 的智能视频分析系统，
#   支持多路视频解码（MPP 硬件解码）、NPU 推理（RKNN）、
#   OpenGL ES 渲染、报警管理等功能。
#
# 构建目标：
#   - 主程序：idge（可执行文件）
#   - 平台：Linux aarch64（ARM64，RK3588 开发板）
#   - 编译器：aarch64-linux-gnu-gcc/g++（交叉编译）
#
# 关键依赖：
#   - Qt5: GUI 框架（信号槽、OpenGL、图表）
#   - FFmpeg (rkmpp): 硬件视频解码（MPP 加速）
#   - RKNN RT: NPU 推理运行时（3 核心，18 TOPS）
#   - RGA: 2D 图形加速（色彩转换、缩放）
#   - OpenCV: 图像处理（备用方案）
#   - EGL/OpenGL ES: GPU 渲染（零拷贝显示）
#
# 构建命令：
#   ./build-linux.sh -t rk3588 -b Release
#
# ============================================================================

# ============================================================================
# 基础 CMake 配置
# ============================================================================
cmake_minimum_required(VERSION 3.10)   # 最低 CMake 版本要求
project(idge)                           # 项目名称

# ============================================================================
# 编译器和构建类型设置
# ============================================================================
# 注意：实际交叉编译时通过 build-linux.sh 覆盖这些值
# CC=aarch64-linux-gnu-gcc, CXX=aarch64-linux-gnu-g++
set(CMAKE_BUILD_TYPE Debug)             # 构建类型（Debug/Release）
set(CMAKE_SYSTEM_NAME "Linux")          # 目标操作系统

# ============================================================================
# Qt5 自动处理设置
# ============================================================================
# 启用 Qt5 的自动处理功能，简化构建流程：
# - AUTOUIC: 自动运行 uic 处理 .ui 文件（UI 布局文件）
# - AUTOMOC: 自动运行 moc 处理 Q_OBJECT 宏（信号槽机制）
# - AUTORCC: 自动运行 rcc 处理 .qrc 文件（资源文件）
set(CMAKE_INCLUDE_CURRENT_DIR ON)       # 将当前目录加入头文件搜索路径
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# ============================================================================
# EGL 平台配置
# ============================================================================
# 阻止 eglplatform.h 引入 X11 头文件：X11 的 KeyPress/FontChange/None/Unsorted
# 等宏会污染之后包含的 Qt 头文件（QEvent::Type、QDir::SortFlag 等）
add_compile_definitions(EGL_NO_X11)

# ============================================================================
# C++ 标准设置
# ============================================================================
set(CMAKE_CXX_STANDARD 17)              # 使用 C++17 标准
set(CMAKE_CXX_STANDARD_REQUIRED ON)     # 强制要求编译器支持 C++17

# ============================================================================
# Debug 模式编译选项
# ============================================================================
# -g: 生成调试信息
# -O0: 关闭优化（便于调试）
# -pg: 生成性能分析信息（gprof）
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

set(CMAKE_C_FLAGS_DEBUG "-g -O0")
# ============================================================================
# Address Sanitizer (ASan) 支持
# ============================================================================
# 用于检测内存错误（越界访问、内存泄漏、悬空指针等）
# 使用方法：cmake -DENABLE_ASAN=TRUE ..
# 注意：只在 Debug 模式下使用，Release 模式会严重影响性能
if (ENABLE_ASAN)
    message(STATUS "BUILD WITH ADDRESS SANITIZER")
    set (CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
    set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
    set (CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
endif ()

# ============================================================================
# 查找第三方依赖包
# ============================================================================

# Qt5 模块查找
# - Core: 核心功能（事件循环、信号槽、容器）
# - Gui: GUI 基础（窗口、事件、字体）
# - Sql: 数据库支持（可选，用于报警记录存储）
# - OpenGL: OpenGL 集成
# - Qml/Quick: QML 声明式 UI（可选）
# - Charts: 图表组件（数据看板）
# - LinguistTools: 国际化工具（多语言支持）
# - Network: 网络支持（RTSP 流、报警推送）
find_package(Qt5 COMPONENTS Core Gui Sql OpenGL Qml Quick Charts Widgets LinguistTools Network Sql Multimedia WebSockets REQUIRED)

# 线程库查找（POSIX 线程）
find_package(Threads REQUIRED)

# SDL2 查找（可选，用于音频播放）
find_package(SDL2)

# ============================================================================
# OpenCV 配置
# ============================================================================
# 使用项目自带的 OpenCV 4.10（交叉编译版本）
set(OpenCV_DIR ${CMAKE_SOURCE_DIR}/3rdparty/opencv4.10/lib/cmake/opencv4)
find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS})

# ============================================================================
# EGL/OpenGL ES 配置
# ============================================================================
# 使用 pkg-config 查找 EGL 和 OpenGL ES 2.0 库
# EGL: 嵌入式图形库接口（管理 GPU 上下文）
# OpenGL ES 2.0: 嵌入式 3D 图形 API（用于视频渲染）
find_package(PkgConfig REQUIRED)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLES2 REQUIRED glesv2)

# ZBar 查找（可选，用于 QR 码扫描 - 智能识别页）
# 板端发行版只有运行时包 libzbar0t64、没有 dev 包，故 3rdparty/zbar 随仓带
# 头文件与可重定位的 zbar.pc；libzbar.so/.a 属预编译二进制，被 .gitignore 挡住，
# 需按 3rdparty/README.md 的流程在联网机器上 apt-get download 后放回 lib/。
# 这里找不到（新克隆未补库）时 ZBAR_FOUND 为假、HAS_ZBAR 不定义，
# 智能识别页的二维码卡片自动降级为不可用，OCR 卡片不受影响。
set(ENV{PKG_CONFIG_PATH} "${CMAKE_SOURCE_DIR}/3rdparty/zbar/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
pkg_check_modules(ZBAR zbar)
if(ZBAR_FOUND)
    message(STATUS "ZBar found: QR code scanning enabled")
    add_compile_definitions(HAS_ZBAR)
    include_directories(${ZBAR_INCLUDE_DIRS})
    link_directories(${ZBAR_LIBRARY_DIRS})
else()
    message(STATUS "ZBar not found: QR code scanning disabled")
endif()

# 运行期资源入口链接（configure 时生成，幂等）
# 程序按"启动目录即工作区根"解析资源（run.sh 会 cd 到构建目录后运行 ./idge），
# 而模型在 model/、OCR 外部可执行在 3rdparty/ppocrv4/bin/、测试/默认视频源在
# assets/test/，都在源码根下，故在构建目录里挂软链指过去，省掉每次部署再拷一遍大文件。
# assets 这条是出厂默认源 assets/test/192.mp4 能解析的前提
# ——早先靠手工在构建目录放一个 192.mp4 软链，换机器即失效。
# 已存在则跳过，不覆盖手工放置的目录或链接。
foreach(_res model 3rdparty assets)
    if(NOT EXISTS "${CMAKE_BINARY_DIR}/${_res}")
        execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
                        "${CMAKE_SOURCE_DIR}/${_res}" "${CMAKE_BINARY_DIR}/${_res}")
        message(STATUS "link ${_res} -> ${CMAKE_SOURCE_DIR}/${_res}")
    endif()
endforeach()

# ============================================================================
# 头文件搜索路径
# ============================================================================
# 包含项目所有模块的头文件路径，按功能分组
include_directories(
    # === FFmpeg RKMPP 硬件解码库 ===
    ${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg-rkmpp/include

    # === 系统头文件 ===
    /usr/include                    # 标准系统头文件
    /usr/include/libdrm             # DRM（Direct Rendering Manager）头文件

    # === OpenCV ===
    ${OpenCV_INCLUDE_DIRS}

    # === EGL/OpenGL ES ===
    ${SDL2_INCLUDE_DIRS}
    ${GLES2_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}

    # === 项目根目录 ===
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/3rdparty/nlohmann/include   # nlohmann/json 单头（第三方，原仓库根 include/）

    # === OpenCV (备用路径) ===
    # (已删除：3rdparty/opencv 旧版备用路径不存在，OpenCV 由 3rdparty/opencv4.10 CMake 配置提供)

    # === Rockchip RGA 2D 加速库 ===
    ${CMAKE_SOURCE_DIR}/3rdparty/librga/include

    # === Rockchip RKNN NPU 推理库 ===
    ${CMAKE_SOURCE_DIR}/3rdparty/rknpu2/include

    # === Rockchip MPP 媒体处理平台 ===
    ${CMAKE_SOURCE_DIR}/3rdparty/mpp/include

    # === ZLMediaKit 流媒体服务器（内嵌 http 静态文件服务，供报警媒体出地址） ===
    ${CMAKE_SOURCE_DIR}/3rdparty/zlmediakit/include

    # === JsonCpp JSON 解析库 ===
    ${CMAKE_SOURCE_DIR}/3rdparty/jsoncpp/include

    # === OpenSSL 加密库 ===
    ${CMAKE_SOURCE_DIR}/3rdparty/openssl1.1.0/include

    # === 定时器工具 ===
    ${CMAKE_SOURCE_DIR}/3rdparty/timer

    # === STB 图像库（PNG/JPEG 读写）===
    ${CMAKE_SOURCE_DIR}/3rdparty/stb

    # === libjpeg-turbo JPEG 解码库 ===
    ${CMAKE_SOURCE_DIR}/3rdparty/jpeg_turbo/include

    # === SQLite 嵌入式数据库 ===
    ${CMAKE_SOURCE_DIR}/3rdparty/sqlite/sqlite

    # ====================================================================
    # 项目源码目录（目录重组后按功能域分组，依赖方向 base ← media ← ai ← biz ← ui）
    # include 约定：一律平铺（#include "xxx.h"），靠本列表解析；禁止 ../ 或模块前缀写法
    # ====================================================================

    ${CMAKE_SOURCE_DIR}/src                         # 入口（main.cpp / head.h）

    # --- base 基础设施域（无业务依赖，被所有域引用）---
    ${CMAKE_SOURCE_DIR}/src/base                    # runtime_paths.h 路径唯一事实源、perf_probe.h 采样开关
    ${CMAKE_SOURCE_DIR}/src/base/buffer             # DMA 缓冲池、帧缓冲区
    ${CMAKE_SOURCE_DIR}/src/base/config             # 配置管理（CLI/JSON 解析）
    ${CMAKE_SOURCE_DIR}/src/base/queue              # 阻塞队列/优先级队列/最新帧槽
    ${CMAKE_SOURCE_DIR}/src/base/imaging            # 图像读写/绘制/letterbox/Qt 安全解码（含文件读写原语）
    ${CMAKE_SOURCE_DIR}/src/base/sysinfo            # /proc 与 /sys 运行指标采样（看板与 Pad 接口共用同一份口径）

    # --- media 视频管线域 ---
    ${CMAKE_SOURCE_DIR}/src/media                 # 媒体运行时初始化
    ${CMAKE_SOURCE_DIR}/src/media/reader            # 视频读取（FFmpeg 硬解/MPP 解码/录像）
    ${CMAKE_SOURCE_DIR}/src/media/rga               # RGA 色彩转换
    ${CMAKE_SOURCE_DIR}/src/media/server            # 内嵌 http 文件服务（报警媒体出地址用；全工程唯一 include mk_*.h 的目录）

    # --- ai 推理与模型应用域 ---
    ${CMAKE_SOURCE_DIR}/src/ai/model_repo           # 模型库注册表（扫描/导入/级联槽位）
    ${CMAKE_SOURCE_DIR}/src/ai/recognition          # 人脸识别桥接（fork+execv 调 tools/face_recognition）
    ${CMAKE_SOURCE_DIR}/src/ai/task                 # 检测任务（PpeTask 及其数据结构）
    ${CMAKE_SOURCE_DIR}/src/ai/yolo11               # YOLOv11 RKNN 推理

    # --- biz 业务域 ---
    ${CMAKE_SOURCE_DIR}/src/biz/alarm               # 报警管理（去重、限流）
    ${CMAKE_SOURCE_DIR}/src/biz/db                  # 数据持久层（业务 sqlite3 + 报警 QtSQL 两套）
    ${CMAKE_SOURCE_DIR}/src/biz/geofence            # 电子围栏（形状、检测、覆盖层）
    ${CMAKE_SOURCE_DIR}/src/biz/pad                 # Pad 接口 v1 的服务端（WS 报警推送 + 出站 JSON 映射）
    ${CMAKE_SOURCE_DIR}/src/biz/qrcode              # 二维码解码（ZBar 封装，登录与智能识别共用）
    ${CMAKE_SOURCE_DIR}/src/biz/service             # 点名/盘点业务服务
    ${CMAKE_SOURCE_DIR}/src/biz/session             # 施工信息与登录会话
    ${CMAKE_SOURCE_DIR}/src/biz/sip                 # GB28181 国标视频推流（SIP/RTP/PS）
    ${CMAKE_SOURCE_DIR}/src/biz/ws                  # WebSocket 客户端（Pad↔Aibox 通信）

    # --- ui 界面域 ---
    ${CMAKE_SOURCE_DIR}/src/ui/core                 # 主题/Qt 助手/图标字体/启动钩子 + qss 皮肤与字体翻译资源
    ${CMAKE_SOURCE_DIR}/src/ui/form                 # 窗口界面（主窗口、视频窗口）
    ${CMAKE_SOURCE_DIR}/src/ui/widgets              # 可复用控件与 EGL/GL 渲染
)

# ============================================================================
# RKNN 运行时库路径
# ============================================================================
# RKNN RT（Runtime）是 Rockchip NPU 的推理运行时库
# 路径：3rdparty/rknpu2/Linux/aarch64/librknnrt.so
set(RKNNRT_LIB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/rknpu2/Linux/aarch64)
link_directories(${RKNNRT_LIB_PATH})

# ============================================================================
# 线程库配置（Linux 平台）
# ============================================================================
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(THREADS_PREFER_PTHREAD_FLAG ON)  # 优先使用 -pthread 编译选项
    find_package(Threads REQUIRED)
endif()

# ============================================================================
# OpenMP 并行计算支持
# ============================================================================
# OpenMP 用于 CPU 并行加速（如图像预处理、后处理）
find_package(OpenMP REQUIRED)

# ============================================================================
# DMA 缓冲区分配器
# ============================================================================
# DMA（Direct Memory Access）缓冲区用于零拷贝数据传输
# 在 RK3588 上，DMA-BUF 可以在 CPU/GPU/NPU/Display 之间共享内存
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/allocator/dma)

# ============================================================================
# RGA3 2D 加速器配置（4G 以上内存限制）
# ============================================================================
# RGA3 是 Rockchip 的第三代 2D 图形加速器
# 当系统内存 > 4GB 时，RGA2 无法直接访问高地址内存
# 需要使用 DMA32 模式（限制在 4GB 以内）
if (ENABLE_DMA32 STREQUAL "TRUE")
    add_definitions(-DDMA_ALLOC_DMA32)  # 定义宏，启用 32 位 DMA 分配
endif()

# ============================================================================
# 第三方库子目录
# ============================================================================
# 3rdparty 目录包含预编译的库和辅助 CMake 脚本
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/ 3rdparty.out)

# ============================================================================
# 运行时库搜索路径（RPATH）
# ============================================================================
# 设置可执行文件运行时搜索动态库的路径
# $ORIGIN: 表示可执行文件所在目录
# $ORIGIN/../lib: 表示可执行文件上级目录的 lib 子目录
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")

# 构建时的库搜索路径（编译时链接）
set(CMAKE_BUILD_RPATH
    "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg-rkmpp/lib"      # FFmpeg RKMPP 硬件解码库
    "${CMAKE_SOURCE_DIR}/3rdparty/zlmediakit/aarch64"    # ZLMediaKit 流媒体库（报警媒体的 http 文件服务）
    # libmk_api.so 自己的 DT_NEEDED 是版本化的 libssl.so.1.1/libcrypto.so.1.1，系统只有 OpenSSL 3。
    # 本文件下方给 idge 传了 LINKER:--disable-new-dtags → 写出来是老式 DT_RPATH，能**传递**地
    # 满足"依赖的依赖"（换成 DT_RUNPATH 就不行，那时只能靠 LD_LIBRARY_PATH）。
    "${CMAKE_SOURCE_DIR}/3rdparty/openssl1.1.0/lib"      # OpenSSL 1.1（仅供 libmk_api.so 运行期解析）
    "${CMAKE_SOURCE_DIR}/3rdparty/rknpu2/Linux/aarch64"  # RKNN NPU 推理库
    "${CMAKE_SOURCE_DIR}/3rdparty/librga/Linux/aarch64"  # RGA 2D 加速库
    "${CMAKE_SOURCE_DIR}/3rdparty/kaldi_native_fbank/Linux/aarch64"  # 语音特征库（可选）
    "${CMAKE_PREFIX_PATH}/lib"                            # 系统库路径
)

# ============================================================================
# 源文件收集
# ============================================================================
# 使用 GLOB 自动收集项目源文件（.cpp）、UI 文件（.ui）、资源文件（.qrc）
# CONFIGURE_DEPENDS: 当文件变化时自动重新运行 CMake
file(GLOB PROJECT_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/res/*.qrc              # Qt 资源文件（图标、图片等）
    ${CMAKE_SOURCE_DIR}/src/*.cpp              # 根目录源文件
    ${CMAKE_SOURCE_DIR}/src/*/*.cpp            # 一级子目录源文件
    ${CMAKE_SOURCE_DIR}/src/*/*/*.cpp          # 二级子目录源文件
    ${CMAKE_SOURCE_DIR}/src/*.ui               # UI 布局文件
    ${CMAKE_SOURCE_DIR}/src/*/*.ui             # 一级子目录 UI 文件
    ${CMAKE_SOURCE_DIR}/src/*/*/*.ui           # 二级子目录 UI 文件（域分组后 .ui 位于 src/ui/*）
)

# ============================================================================
# Qt5 大资源文件处理
# ============================================================================
# qt5_add_big_resources: 将资源文件嵌入到可执行文件中
# 使用大资源模式（big resources），适合包含大量资源的项目
qt5_add_big_resources(CORE_HELPER_SOURCES_QRC
    ${CMAKE_SOURCE_DIR}/src/ui/core/qrc/font.qrc    # 字体资源
    ${CMAKE_SOURCE_DIR}/src/ui/core/qrc/image.qrc   # 图片资源
    ${CMAKE_SOURCE_DIR}/src/ui/core/qrc/qm.qrc      # 翻译文件
    ${CMAKE_SOURCE_DIR}/src/ui/core/qrc/wasm.qrc    # 中文字体兜底（注册 /font/DroidSansFallback.ttf）
)

# 皮肤资源单独走 qt5_add_resources（**不是** big_resources）。
# 原因：big_resources 生成的是 `rcc --pass 1/2` 两段式规则，Makefile 里
# 那条 `qrc_qss.o:` 前置依赖为空 —— 改 blacksoft.css 后 rcc 根本不会重跑，
# 皮肤改动静默不进二进制（本轮触屏 QSS 改造就是这么白折腾了一次）。
# 普通 add_resources 会把 .qrc 里列出的每个文件都挂成前置依赖（见
# qrc_resources.cpp 规则：resources.qrc + shutter.wav + .depends）。
# qss.qrc 只有 1 个 21KB css + 23 张小图标，本来也不需要 big 模式。
qt5_add_resources(CORE_QRC
    ${CMAKE_SOURCE_DIR}/src/ui/core/qss.qrc         # blacksoft 皮肤（:/qss/...）
)

# 拍照快门音效资源（人员点名/设备盘点模块）
qt5_add_resources(SOUND_RESOURCES
    ${CMAKE_SOURCE_DIR}/sounds/resources.qrc
)

# ============================================================================
# 国际化翻译文件
# ============================================================================
# Qt Linguist 工具用于多语言翻译
set(TS_FILES ${CMAKE_SOURCE_DIR}/src/idge_zh_CN.ts)  # 中文翻译文件

# ============================================================================
# 最终头文件包含路径
# ============================================================================
include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${STB_INCLUDES} ${LIBJPEG_INCLUDES})
include_directories(${LIBRKNNRT_INCLUDES})

message(STATUS ${LIBRKNNRT})

# ============================================================================
# 创建可执行文件目标
# ============================================================================
# 将所有源文件、资源文件、翻译文件打包成一个可执行文件
add_executable(${PROJECT_NAME}
    ${PROJECT_SOURCES}                  # 项目源文件
    ${CORE_HELPER_SOURCES_QRC}          # 核心辅助资源（字体、图片等）
    ${CORE_QRC}                         # QSS 样式表资源
    ${SOUND_RESOURCES}                  # 快门音效资源
    ${TS_FILES}                         # 翻译文件
)

# ============================================================================
# 链接库目录
# ============================================================================
# 指定动态库的搜索路径（用于链接阶段）
target_link_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg-rkmpp/lib       # FFmpeg 硬件解码库
    ${CMAKE_SOURCE_DIR}/3rdparty/libyuv/lib             # libyuv 色彩转换库
    ${CMAKE_SOURCE_DIR}/3rdparty/zlmediakit/aarch64     # ZLMediaKit 流媒体库
    ${CMAKE_SOURCE_DIR}/3rdparty/librga/lib             # RGA 2D 加速库
    ${CMAKE_SOURCE_DIR}/3rdparty/rknpu2/Linux/aarch64   # RKNN NPU 推理库
    ${CMAKE_SOURCE_DIR}/3rdparty/jsoncpp/lib            # JsonCpp JSON 库
)

# ============================================================================
# 链接库列表
# ============================================================================
# 按功能分组列出所有需要链接的库
target_link_libraries(${PROJECT_NAME}
    # ==================== Qt5 模块 ====================
    Qt5::Gui                    # GUI 模块（窗口、事件、字体）
    Qt5::Network                # 网络模块（RTSP、HTTP）
    Qt5::Core                   # 核心模块（事件循环、信号槽）
    Qt5::Charts                 # 图表模块（数据看板）
    Qt5::OpenGL                 # OpenGL 模块
    Qt5::Sql                    # 数据库模块（报警记录）
    Qt5::Multimedia             # 多媒体模块（拍照快门音效）
    Qt5::WebSockets             # WebSocket 模块（Pad↔Aibox 通信）

    # ==================== 业务模块依赖（caichao 分支合并） ====================
    sqlite3                     # 业务 SQLite（人员点名/设备盘点）

    # ==================== FFmpeg 硬件解码库 ====================
    avformat                    # 格式处理（MP4/RTSP/FLV 容器）
    avcodec                     # 编解码器（H.264/H.265/VP8/VP9）
    avutil                      # 工具函数（内存、数学、时间）
    swscale                     # 图像缩放和格式转换（YUV→RGB）
    avfilter                    # 音视频滤镜
    avdevice                    # 设备采集（V4L2/X11）
    swresample                  # 音频重采样
    postproc                    # 后处理（画质增强）

    # ==================== 系统库 ====================
    pthread                     # POSIX 线程库
    dl                          # 动态库加载（dlopen/dlsym）
    drm                         # DRM 直接渲染管理（显示输出）
    crypto                      # OpenSSL 加密库
    ssl                         # OpenSSL SSL/TLS

    # ==================== 第三方库 ====================
    mk_api                      # ZLMediaKit 流媒体 API（报警媒体的内嵌 http 文件服务）
    jsoncpp                     # JsonCpp JSON 解析库
    sqlite3                     # SQLite 嵌入式数据库

    # ==================== Rockchip 硬件加速库 ====================
    rknnrt                      # RKNN NPU 推理运行时（3 核心，18 TOPS）
    rga                         # RGA 2D 图形加速（色彩转换、缩放）

    # ==================== 图像处理库 ====================
    ${OpenCV_LIBS}              # OpenCV（备用图像处理）

    # ==================== OpenGL ES 渲染库 ====================
    ${GLES2_LIBRARIES}          # OpenGL ES 2.0（嵌入式 3D 图形）
    ${EGL_LIBRARIES}            # EGL（嵌入式图形接口，管理 GPU 上下文）

    # ==================== 音频库 ====================
    ${SDL2_LIBRARIES}           # SDL2（音频播放，可选）

    # ==================== QR 码扫描（可选）====================
    ${ZBAR_LIBRARIES}           # ZBar（QR 码识别，智能识别模块）

    # ==================== 线程库 ====================
    Threads::Threads            # POSIX 线程（跨平台封装）

    # ==================== JPEG 库 ====================
    ${LIBJPEG}                  # libjpeg-turbo（JPEG 硬件加速解码）

    # ==================== RGA 库 ====================
    ${LIBRGA}                   # RGA 2D 加速（重复链接，确保兼容）
)

# ============================================================================
# 编译宏定义
# ============================================================================
# ZERO_COPY: 启用零拷贝模式
# 在 RK3588 上，通过 DMA-BUF 实现 MPP→RGA→RKNN→EGL 的零拷贝流水线
# 数据全程在 GPU/NPU/Display 之间共享，CPU 不参与像素拷贝
target_compile_definitions(${PROJECT_NAME} PRIVATE ZERO_COPY)

# ============================================================================
# 链接器选项
# ============================================================================
# --allow-shlib-undefined: 允许共享库中有未定义符号（某些旧库需要）
# --disable-new-dtags: 使用 DT_RPATH 而非 DT_RUNPATH（兼容旧系统）
target_link_options(${PROJECT_NAME} PRIVATE "LINKER:--allow-shlib-undefined" "LINKER:--disable-new-dtags")

# ============================================================================
# 安装配置
# ============================================================================
# 安装 FFmpeg RKMPP 动态库到 lib 目录
# FILES_MATCHING PATTERN "*.so*": 匹配所有 .so 和 .so.x 文件
install(DIRECTORY ${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg-rkmpp/lib/
        DESTINATION lib
        FILES_MATCHING PATTERN "*.so*")

# ============================================================================
# 测试程序
# ============================================================================
# 数据库模块测试
add_executable(test_database
    tests/test_database.cpp
    src/biz/db/database_manager.cpp
    src/biz/db/detection_dao.cpp
    src/biz/db/alarm_dao.cpp
    # alarm_dao 的 constructionFacets 要用 ConstructionInfo 的键名常量，
    # 而测试目标显式列源文件（不像主目标那样 GLOB），漏了就链接期 undefined reference。
    src/biz/session/construction_session.cpp
)

target_include_directories(test_database PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/biz/db
    ${CMAKE_SOURCE_DIR}/src/ai/yolo11
    ${CMAKE_SOURCE_DIR}/src/alarm
    ${CMAKE_SOURCE_DIR}/3rdparty/jsoncpp/include
    ${CMAKE_SOURCE_DIR}/3rdparty/sqlite
)

target_link_libraries(test_database
    Qt5::Core
    Qt5::Sql
    sqlite3
    ${CMAKE_SOURCE_DIR}/3rdparty/jsoncpp/lib/libjsoncpp.a
    pthread
)

target_compile_definitions(test_database PRIVATE ZERO_COPY)

# ============================================================================
# 设备盘点全链离线自测（模型加载→检测计数→画框落盘→落库→回读，/tmp 工作区）
# 仓库根运行：./build/build_rk3588_linux/test_equipment，rc 0=PASS 1=FAIL
# 直接编译盘点链依赖源（同 idge 主目标的平铺 include 约定）
# ============================================================================
add_executable(test_equipment
    tests/test_equipment.cpp
    src/biz/service/equipment_inventory_service.cpp
    src/biz/service/roll_call_service.cpp
    src/biz/db/business_db_manager.cpp
    src/biz/service/task_manager.cpp
    src/base/imaging/draw_utils.cpp
    src/base/imaging/image_utils.cpp
    src/base/imaging/image_drawing.cpp
    src/base/imaging/file_utils.cpp
    src/ai/recognition/face_recognizer.cpp
    src/ai/recognition/in_process_face_recognizer.cpp
    src/ai/model_repo/model_registry.cpp
    src/base/config/ConfigManager.cpp
)

target_link_libraries(test_equipment
    Qt5::Core
    Qt5::Gui
    sqlite3
    rknnrt
    ${OpenCV_LIBS}
    ${LIBJPEG}
    ${LIBRGA}
    ${CMAKE_DL_LIBS}
    pthread
)

# 静态 libturbojpeg 会向动态符号表导出 jpeg_* 符号，劫持系统 OpenCV imgcodecs
# 依赖的 libjpeg ABI，导致 cv::imread 解码返回空（主程序动态链接 jpeg 故无此患）。
# --exclude-libs 把静态库符号移出动态表：内部仍用自带 turbojpeg，OpenCV 恢复解析系统 libjpeg。
target_link_options(test_equipment PRIVATE -Wl,--exclude-libs,ALL)

# ============================================================================
# 人员点名全链离线自测（外部 exe 对照回归 + 注册/去重/注销匹配/落库回读）
# 仓库根运行：./build/build_rk3588_linux/test_rollcall，rc 0=PASS 1=FAIL
# face_recognizer.cpp 提供 FaceRecognitionWrapper：拉起外部 exe 作移植前后基准
# ============================================================================
add_executable(test_rollcall
    tests/test_rollcall.cpp
    src/biz/service/roll_call_service.cpp
    src/biz/db/business_db_manager.cpp
    src/biz/service/task_manager.cpp
    src/base/imaging/draw_utils.cpp
    src/base/imaging/image_utils.cpp
    src/base/imaging/image_drawing.cpp
    src/base/imaging/file_utils.cpp
    src/ai/recognition/face_recognizer.cpp
    src/ai/recognition/in_process_face_recognizer.cpp
    src/base/config/ConfigManager.cpp
)

target_link_libraries(test_rollcall
    Qt5::Core
    Qt5::Gui
    sqlite3
    rknnrt
    ${OpenCV_LIBS}
    ${LIBJPEG}
    ${LIBRGA}
    ${CMAKE_DL_LIBS}
    pthread
)

# 同 test_equipment：防静态 libturbojpeg 导出 jpeg_* 劫持系统 libjpeg ABI
target_link_options(test_rollcall PRIVATE -Wl,--exclude-libs,ALL)

# ============================================================================
# 人脸识别工具（原 cc_face，命令行接口：<det.rknn> <rec.rknn> <image> [out.json]）
# 点名链已进程内化（src/ai/recognition/in_process_face_recognizer.cpp），本 exe
# 保留为 CLI 手工验证工具与 test_rollcall 对照回归的基准
# 产物固定输出到 model/face/face_recognition，与主程序约定路径一致
# ============================================================================
add_executable(face_recognition tools/face_recognition/main.cpp)

target_include_directories(face_recognition PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/face_recognition
    ${CMAKE_SOURCE_DIR}/3rdparty/rknpu2/include
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(face_recognition
    rknnrt
    ${OpenCV_LIBS}
)

set_target_properties(face_recognition PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/model/face
)

# ============================================================================
# LatestFrameSlot 单元测试（header-only 广播邮箱，无第三方依赖）
# 覆盖四条契约：代感知跳帧、被持有输入不被回收、close 唤醒等待者、reset 后重启
# 仓库根运行：./build/build_rk3588_linux/test_latest_frame_slot，rc 0=PASS 1=FAIL
# ============================================================================
add_executable(test_latest_frame_slot
    tests/test_latest_frame_slot.cpp
)

target_include_directories(test_latest_frame_slot PRIVATE
    ${CMAKE_SOURCE_DIR}/src/base/queue
)

target_link_libraries(test_latest_frame_slot
    Threads::Threads
)

# ============================================================================
# 报警上行离线自检（alarm_info 报文 + 媒体分块 + 读盘 + url 档地址换算）
# 只编 alarm_uplink_engine.cpp 与 media_payload.cpp：用 FakeTransport 顶掉 WsClient，
# 所以本目标不链 Qt5::WebSockets、不链 AlarmManager/DAO，也不需要 GUI 线程。
# media_pump.cpp 不在这里——它是 QObject + 后台线程，成品回调要投回 GUI，留给板端端到端。
# 两种 media_transport（ws_binary 分块帧 / url 只发地址）都在断言里，
# ws_binary 那 11 组是"保留代码但默认关"的回归保护，改协议时不许顺手删。
# 契约清单见 tests/test_alarm_uplink.cpp 文件头
# 仓库根运行：./build/build_rk3588_linux/test_alarm_uplink，rc 0=PASS 1=FAIL
# ============================================================================
add_executable(test_alarm_uplink
    tests/test_alarm_uplink.cpp
    src/biz/ws/alarm_uplink_engine.cpp
    src/biz/ws/media_payload.cpp
)

target_include_directories(test_alarm_uplink PRIVATE
    ${CMAKE_SOURCE_DIR}/src/biz/ws
)

target_link_libraries(test_alarm_uplink
    Qt5::Core
)

# ============================================================================
# Pad 接口 v1 出站 JSON 映射离线自检（时区/状态三值/类型表/证据 URL 安全判据/信封）
# 只编 pad_wire.cpp + alarm_uplink_engine.cpp（借 mediaUrlFor 那份"越界即不给"的实现）：
# 不起 WS 服务端、不开数据库、不碰 GUI。协议边界写错在板端看不出来，见文件头十五条。
# 仓库根运行：./build/build_rk3588_linux/test_pad_wire，rc 0=PASS 1=FAIL
# ============================================================================
add_executable(test_pad_wire
    tests/test_pad_wire.cpp
    src/biz/pad/pad_wire.cpp
    src/biz/alarm/alarm_labels.cpp
    src/biz/alarm/alarm_media_prune.cpp
    src/biz/ws/alarm_uplink_engine.cpp
    src/biz/session/construction_session.cpp   # pad_wire 的 taskId 取值判据复用这一处实现
)

target_include_directories(test_pad_wire PRIVATE
    ${CMAKE_SOURCE_DIR}/src/biz/pad
    ${CMAKE_SOURCE_DIR}/src/biz/ws
    ${CMAKE_SOURCE_DIR}/src/biz/alarm
    ${CMAKE_SOURCE_DIR}/src/biz/session
)

target_link_libraries(test_pad_wire
    Qt5::Core
)

# ============================================================================
# Pad 接口 v1 断线补漏的库侧自检（queryUpdatedSince 游标 + markRead 语义，内存库）
# 契约清单见 tests/test_pad_sync.cpp 文件头；源列表照抄 test_database（DAO 的依赖闭包）
# 仓库根运行：./build/build_rk3588_linux/test_pad_sync，rc 0=PASS 1=FAIL
# ============================================================================
add_executable(test_pad_sync
    tests/test_pad_sync.cpp
    src/biz/db/database_manager.cpp
    src/biz/db/alarm_dao.cpp
    src/biz/session/construction_session.cpp
)

target_include_directories(test_pad_sync PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/biz/db
    ${CMAKE_SOURCE_DIR}/src/ai/yolo11
    ${CMAKE_SOURCE_DIR}/src/biz/alarm
    ${CMAKE_SOURCE_DIR}/3rdparty/jsoncpp/include
    ${CMAKE_SOURCE_DIR}/3rdparty/sqlite
)

target_link_libraries(test_pad_sync
    Qt5::Core
    Qt5::Sql
    sqlite3
    ${CMAKE_SOURCE_DIR}/3rdparty/jsoncpp/lib/libjsoncpp.a
    pthread
)

# ============================================================================
# Pad 接口 v1 的 WS 服务端线上自检（loopback 起真服务端 + 真 QWebSocket 客户端）
# 源列表 = pad_ws_server 的依赖闭包（PadWsServer 订阅 AlarmManager，所以连 alarm/db/session
# 一起编）；不链 GUI、不链 RKNN/OpenCV 符号。契约清单见 tests/test_pad_ws_server.cpp 头。
# 仓库根运行：./build/build_rk3588_linux/test_pad_ws_server，rc 0=PASS 1=FAIL
# ============================================================================
add_executable(test_pad_ws_server
    tests/test_pad_ws_server.cpp
    src/biz/pad/pad_ws_server.cpp
    src/biz/pad/pad_wire.cpp
    src/biz/alarm/alarm_labels.cpp
    src/biz/alarm/alarm_media_prune.cpp
    src/biz/ws/alarm_uplink_engine.cpp
    src/biz/alarm/alarm_manager.cpp
    src/biz/db/database_manager.cpp
    src/biz/db/alarm_dao.cpp
    src/biz/db/detection_dao.cpp
    src/biz/session/construction_session.cpp
    src/base/config/ConfigManager.cpp
)

target_include_directories(test_pad_ws_server PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/biz/pad
    ${CMAKE_SOURCE_DIR}/src/biz/ws
    ${CMAKE_SOURCE_DIR}/src/biz/alarm
    ${CMAKE_SOURCE_DIR}/src/biz/db
    ${CMAKE_SOURCE_DIR}/src/biz/session
    ${CMAKE_SOURCE_DIR}/src/base
    ${CMAKE_SOURCE_DIR}/src/base/config
    ${CMAKE_SOURCE_DIR}/src/ai/yolo11
    ${CMAKE_SOURCE_DIR}/3rdparty/jsoncpp/include
    ${CMAKE_SOURCE_DIR}/3rdparty/sqlite
)

target_link_libraries(test_pad_ws_server
    Qt5::Core
    Qt5::Network
    Qt5::Sql
    Qt5::WebSockets
    sqlite3
    ${CMAKE_SOURCE_DIR}/3rdparty/jsoncpp/lib/libjsoncpp.a
    pthread
)

# ============================================================================
# Pad 接口 v1 HTTP 面的报文解析离线自检（不起 socket、不碰库）
# 只编 pad_http_message.cpp：那一层的全部判据（chunked / multipart / 上限 / 百分号 /
# §2.4 信封）都能靠喂字节验，见 tests/test_pad_http_message.cpp 文件头的十九条。
# 仓库根运行：./build/build_rk3588_linux/test_pad_http_message，rc 0=PASS 1=FAIL
# ============================================================================
add_executable(test_pad_http_message
    tests/test_pad_http_message.cpp
    src/biz/pad/pad_http_message.cpp
)

target_include_directories(test_pad_http_message PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/biz/pad
)

target_link_libraries(test_pad_http_message
    Qt5::Core
)

# ============================================================================
# Pad 接口 v1 HTTP 面的线上自检（loopback 起真服务端 + 真 QTcpSocket 客户端）
# 源列表 = 四条章节路由的依赖闭包（PadHttpServer 走 AlarmManager/AlarmDAO/围栏库/设备采样，
# 所以连 db、session、geofence、sysinfo、ai/model_repo 一起编）。
# 不链 GUI 之外的东西：Qt5::Gui 是**必须**的——fence_frame_renderer 用 QImage/QPainter
# 画 §5.3 要求的那张合成画面，它在 offscreen 之外不需要任何显示设备。
# 契约清单见 tests/test_pad_http_api.cpp 文件头。
# 仓库根运行：./build/build_rk3588_linux/test_pad_http_api，rc 0=PASS 1=FAIL
# ============================================================================
add_executable(test_pad_http_api
    tests/test_pad_http_api.cpp
    src/biz/pad/pad_http_message.cpp
    src/biz/pad/pad_http_server.cpp
    src/biz/pad/pad_http_api.cpp
    src/biz/pad/pad_http_alarms.cpp
    src/biz/pad/pad_http_fences.cpp
    src/biz/pad/pad_http_streams.cpp
    src/biz/pad/pad_http_device.cpp
    src/biz/pad/pad_request_cache.cpp
    src/biz/pad/pad_wire.cpp
    src/biz/alarm/alarm_labels.cpp
    src/biz/alarm/alarm_media_prune.cpp
    src/biz/pad/pad_ws_server.cpp
    src/biz/ws/alarm_uplink_engine.cpp
    src/biz/geofence/pad_fence_store.cpp
    src/biz/geofence/native_fence_store.cpp
    src/biz/geofence/fence_frame_renderer.cpp
    # §5.3 的 apply/disable 现在会把 Pad 围栏并进检测快照（FenceManager::refreshFromStores），
    # 所以判定那两文件也进了本目标的依赖闭包。
    src/biz/geofence/fence_manager.cpp
    src/biz/geofence/fence_checker.cpp
    src/biz/alarm/alarm_manager.cpp
    src/biz/db/database_manager.cpp
    src/biz/db/alarm_dao.cpp
    src/biz/db/detection_dao.cpp
    src/biz/session/construction_session.cpp
    src/base/config/ConfigManager.cpp
    src/base/sysinfo/device_stats.cpp
    src/ai/model_repo/model_registry.cpp
    # pad_http_api.cpp 现在引用 RtspLiveService（§6.1 liveUrl 优先指带框 RTSP 直播源），
    # 所以把它的实现 + ZLM 环境门 + HttpFileService（firstUsableIpv4）一并拉进测试目标。
    src/media/server/rtsp_live_service.cpp
    src/media/server/zlm_env.cpp
    src/media/server/http_file_service.cpp
    src/media/server/net_route.cpp
)

target_include_directories(test_pad_http_api PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/biz/pad
    ${CMAKE_SOURCE_DIR}/src/biz/ws
    ${CMAKE_SOURCE_DIR}/src/biz/alarm
    ${CMAKE_SOURCE_DIR}/src/biz/db
    ${CMAKE_SOURCE_DIR}/src/biz/geofence
    ${CMAKE_SOURCE_DIR}/src/biz/session
    ${CMAKE_SOURCE_DIR}/src/base
    ${CMAKE_SOURCE_DIR}/src/base/config
    ${CMAKE_SOURCE_DIR}/src/base/sysinfo
    ${CMAKE_SOURCE_DIR}/src/ai/model_repo
    ${CMAKE_SOURCE_DIR}/src/ai/yolo11
    ${CMAKE_SOURCE_DIR}/src/media/server
    ${CMAKE_SOURCE_DIR}/3rdparty/zlmediakit/include
    ${CMAKE_SOURCE_DIR}/3rdparty/jsoncpp/include
    ${CMAKE_SOURCE_DIR}/3rdparty/sqlite
)

target_link_directories(test_pad_http_api PRIVATE
    ${CMAKE_SOURCE_DIR}/3rdparty/zlmediakit/aarch64
    ${CMAKE_SOURCE_DIR}/3rdparty/openssl1.1.0/lib
)

target_link_libraries(test_pad_http_api
    Qt5::Core
    Qt5::Gui
    Qt5::Network
    Qt5::Sql
    Qt5::WebSockets
    sqlite3
    ${CMAKE_SOURCE_DIR}/3rdparty/jsoncpp/lib/libjsoncpp.a
    mk_api        # RtspLiveService / HttpFileService 依赖
    ssl crypto    # libmk_api.so 运行期需要 OpenSSL 1.1
    pthread
)

# ============================================================================
# 电子围栏"每形状角色"判定离线自检（禁区/岗区域的归并与角色字符串回落）# 只编 fence_checker.cpp：它只用 object_detect_result 的三个整数字段，不调任何
# OpenCV/RKNN 符号，因此本目标不链 GUI、不链 AlarmManager，也不需要 config.json。
# 为什么值得单独钉：禁区与岗区域的违规条件不是互补关系，归并写反会静默产出
# "纯禁区通道把每个人每帧判成离岗"这类板端肉眼看不出来的行为（契约 2）。
# 契约清单见 tests/test_geofence_role.cpp 文件头
# 仓库根运行：./build/build_rk3588_linux/test_geofence_role，rc 0=PASS 1=FAIL
# ============================================================================
add_executable(test_geofence_role
    tests/test_geofence_role.cpp
    src/biz/geofence/fence_checker.cpp
)

target_include_directories(test_geofence_role PRIVATE
    ${CMAKE_SOURCE_DIR}/src/biz/geofence
)

target_link_libraries(test_geofence_role
    Qt5::Core
)

target_compile_definitions(test_geofence_role PRIVATE ZERO_COPY)

# ============================================================================
# 报警媒体按天目录裁剪的离线自检（过期整目录 / 超上限只端大文件保缩略图）
# 为什么值得单独钉：这道闸门的后果只有两种，而且都是**事后**才看得见——删多了是
# "三周前的报警点开没截图"，删少了是分区被 4GB 闸门之外的老库写满、落库直接失败。
# 板子上既没法造 4GB 来验第一遍，也不方便等到过期才看得见第二遍，所以判据拆成
# (root, days, cap, today) 四个入参在这里跑。契约清单见 tests/test_media_prune.cpp 头。
# 仓库根运行：./build/build_rk3588_linux/test_media_prune，rc 0=PASS 1=FAIL
# ============================================================================
add_executable(test_media_prune
    tests/test_media_prune.cpp
    src/biz/alarm/alarm_media_prune.cpp
)

target_include_directories(test_media_prune PRIVATE
    ${CMAKE_SOURCE_DIR}/src/biz/alarm
)

target_link_libraries(test_media_prune
    Qt5::Core
)

target_compile_definitions(test_media_prune PRIVATE ZERO_COPY)

# ============================================================================
# 电子围栏"两个来源合并进检测快照"离线自检（native_fences + pad_fences）
# 为什么值得单独钉：Pad 的 cameraId 是 §7.1 的 **1-based**，本机手绘表是 **0-based**，
# 两套编号合并错了就是"在另一路画面上报警"；而合并入口 refreshFromStores 既被登录
# 路径（整段持锁）又被 HTTP 路由（不持锁）调，QMutex 不可重入，写错一边就是启动自死锁。
# 这三条板端肉眼都看不出来，只能在离线钉死。契约清单见 tests/test_fence_merge.cpp 头。
# 仓库根运行：./build/build_rk3588_linux/test_fence_merge，rc 0=PASS 1=FAIL
# ============================================================================
add_executable(test_fence_merge
    tests/test_fence_merge.cpp
    src/biz/geofence/fence_manager.cpp
    src/biz/geofence/fence_checker.cpp
    src/biz/geofence/pad_fence_store.cpp
    src/biz/geofence/native_fence_store.cpp
    src/biz/db/database_manager.cpp
    src/biz/session/construction_session.cpp
    src/base/config/ConfigManager.cpp
)

target_link_libraries(test_fence_merge
    Qt5::Core
    Qt5::Gui
    Qt5::Sql
    sqlite3
    pthread
)

target_compile_definitions(test_fence_merge PRIVATE ZERO_COPY)

# ============================================================================
# 施工信息 + 二维码离线自检（解析/校验/会话状态机 + 真解一张码图）
# 只编 construction_session.cpp 与 qr_decoder.cpp：两头都只用 Qt，不开摄像头、
# 不碰 GUI 线程，所以本目标不链 AlarmManager/DAO/ConfigManager，也不需要 X11。
# 为什么值得单独钉：登录门的全部正确性压在"码里原文 → 校验 → 会话"这条短链上，
# 而它在板端只靠肉眼验证一次就会定型（契约清单见 tests/test_construction_qr.cpp 头）。
# 仓库根运行：./build/build_rk3588_linux/test_construction_qr，rc 0=PASS 1=FAIL
# ============================================================================
add_executable(test_construction_qr
    tests/test_construction_qr.cpp
    src/biz/session/construction_session.cpp
    src/biz/qrcode/qr_decoder.cpp
)

target_include_directories(test_construction_qr PRIVATE
    ${CMAKE_SOURCE_DIR}/src/biz/session
    ${CMAKE_SOURCE_DIR}/src/biz/qrcode
)

target_link_libraries(test_construction_qr
    Qt5::Core
    Qt5::Gui
    ${ZBAR_LIBRARIES}
)

# ============================================================================
# 报警上行接收端（人工对拍，不是自检）
# QWebSocketServer 收 text + binary：ws_binary 档按 media_id 拼成文件落 outdir 并比 sha256；
# url 档只打印 media_begin 里的地址与 sha256，一个字节都不落盘（对拍改用 curl 取回）。
# 板子出网受限，联调只能 loopback：idge 的 server_url 指 ws://127.0.0.1:8765 即可。
# 无 GUI 依赖（QCoreApplication），./build/build_rk3588_linux/ws_sink [port] [outdir]
# ============================================================================
add_executable(ws_sink
    tests/ws_sink.cpp
)

target_link_libraries(ws_sink
    Qt5::Core
    Qt5::WebSockets
)

# ============================================================================
# 视频流注册表与通道引用解析离线自检（video.sources + channels[].source_id）
# 只编 ConfigManager.cpp：这一段全是读 JSON/改 JSON，不碰 FFmpeg、模型、屏幕，
# 所以在任何机器上都能跑，也是唯一能把"这一路到底开什么"的三层兜底逐条钉死的地方。
# 悬空 id 不回退、写内联即撤引用这两条是最容易被"顺手优化"掉的，必须有断言挡着。
# 契约清单见 tests/test_video_sources.cpp 文件头；配置读写都在 QDir::tempPath() 下的
# 临时文件，不会碰运行时的 data/config.json。
# 仓库根运行：./build/build_rk3588_linux/test_video_sources，rc 0=PASS 1=FAIL
# ============================================================================
add_executable(test_video_sources
    tests/test_video_sources.cpp
    src/base/config/ConfigManager.cpp
)

target_link_libraries(test_video_sources
    Qt5::Core
)

# ============================================================================
# 国标流引用视频流注册表的离线自检（无 GUI、无 FFmpeg）
# gb28181.streams[] 现在只写身份 + source_id，取流字段全在 video.sources[]。这个目标钉的是
# 由此新增的三类"只在运行时才暴露"的错：忘写 source_id、指向不存在的 id、进程跑起来之后
# 注册表被删——三种表现必须不同，否则下次有人加"顺手兜底"就把它们混成一个静默失败。
# 只编 ConfigManager.cpp + gb28181_config.cpp（后者用 QHostAddress，所以链 Network）。
# 契约清单见 tests/test_gb28181_source_ref.cpp 文件头；配置读写在 QDir::tempPath() 下。
#   ./build/build_rk3588_linux/test_gb28181_source_ref，rc 0=PASS 1=FAIL
# ============================================================================
add_executable(test_gb28181_source_ref
    tests/test_gb28181_source_ref.cpp
    src/base/config/ConfigManager.cpp
    src/biz/sip/gb28181_config.cpp
)

target_link_libraries(test_gb28181_source_ref
    Qt5::Core
    Qt5::Network
)

# ============================================================================
# 通道配置热更新自测（真解码器 + 真推理任务，无 GUI）
# 覆盖四条契约：停止判据 isLoopAlive、releaseTasks 后按新配置重建槽位、
# 反复重开线程数与 RSS 不涨、每槽生效阈值（slot params 那行日志的 conf/nms）；
# 其中第四条兼带用 updateSlotParams 的返回值验证形状判据——形状不符必须返回 false，不能静默 no-op
# cwd 必须是 build 目录（模型库按 ./model/library 解析）：
#   ./build/build_rk3588_linux/test_channel_reload，rc 0=PASS 1=FAIL
# 源列表 = ffmpeg_video_decoder.cpp 的依赖闭包（平铺 include 与主目标同套库）
# ============================================================================
add_executable(test_channel_reload
    tests/test_channel_reload.cpp
    src/media/reader/ffmpeg_video_decoder.cpp
    src/media/reader/video_recorder.cpp
    src/ai/task/ppe_task.cpp
    src/ai/task/base_task.cpp
    src/ai/model_repo/model_registry.cpp
    src/base/config/ConfigManager.cpp
    src/base/config/parse_config.cpp
    src/base/buffer/dma_alloc.cpp
    src/base/buffer/DmaFrameBuffer.cpp
    src/biz/alarm/alarm_manager.cpp
    src/biz/alarm/alarm_labels.cpp
    src/biz/alarm/alarm_media_prune.cpp
    # alarm_manager.cpp 的 makeAlarmRecord() 现在给每条报警盖施工信息戳，
    # 于是本目标的依赖闭包里多了一个会话实现。漏了它就是三个 undefined reference
    # （instance / snapshot / toJson），而链接错误比编译错误更难归因到这里。
    src/biz/session/construction_session.cpp
    src/biz/db/alarm_dao.cpp
    src/biz/db/detection_dao.cpp
    src/biz/db/database_manager.cpp
    src/biz/geofence/fence_manager.cpp
    src/biz/geofence/native_fence_store.cpp
    src/biz/geofence/pad_fence_store.cpp
    src/biz/geofence/fence_checker.cpp
    src/base/imaging/image_utils.cpp
    src/base/imaging/image_drawing.cpp
    src/base/imaging/draw_utils.cpp
    src/base/imaging/file_utils.cpp
    src/biz/service/task_manager.cpp
    # ffmpeg_video_decoder.cpp 现在引用 RtspLiveService（带框码流推 RTSP 直播），
    # 所以把它的实现 + ZLM 环境门 + HttpFileService（firstUsableIpv4）补进本目标的依赖闭包。
    src/media/server/rtsp_live_service.cpp
    src/media/server/zlm_env.cpp
    src/media/server/http_file_service.cpp
    src/media/server/net_route.cpp
)

# 主目标的库搜索路径是 target_link_directories(${PROJECT_NAME})，不会传给本目标，
# 这里同一套再来一遍（av*/rga/jsoncpp 都只在这些目录里）。
target_link_directories(test_channel_reload PRIVATE
    ${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg-rkmpp/lib
    ${CMAKE_SOURCE_DIR}/3rdparty/libyuv/lib
    ${CMAKE_SOURCE_DIR}/3rdparty/librga/lib
    ${CMAKE_SOURCE_DIR}/3rdparty/rknpu2/Linux/aarch64
    ${CMAKE_SOURCE_DIR}/3rdparty/jsoncpp/lib
    ${CMAKE_SOURCE_DIR}/3rdparty/zlmediakit/aarch64
    ${CMAKE_SOURCE_DIR}/3rdparty/openssl1.1.0/lib
)

target_link_libraries(test_channel_reload
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Sql
    Qt5::Network
    avformat
    avcodec
    avutil
    swscale
    swresample
    pthread
    dl
    drm
    jsoncpp
    sqlite3
    rknnrt
    rga
    mk_api
    ssl crypto
    ${OpenCV_LIBS}
    ${GLES2_LIBRARIES}
    ${EGL_LIBRARIES}
    ${LIBJPEG}
    ${LIBRGA}
    Threads::Threads
)
# 与主目标同宏：零拷贝流水线（DMA-BUF/RGA）在 decodeLoop 里是编译期分支
target_compile_definitions(test_channel_reload PRIVATE ZERO_COPY)

target_link_options(test_channel_reload PRIVATE "LINKER:--allow-shlib-undefined" "LINKER:--disable-new-dtags")

# ============================================================================
# 视频流参数板端实测/回归（真解码器 + 无 GUI + 不挂推理）
# 钉的是 video.sources 那四个"静默失效"的参数：loop 不生效=播完黑屏、timeoutMs 不生效=
# 界面永远"正在接入…"、reconnectMs 不生效=拔插后永久死路、hardwareDecode=false 不生效=软解档
# 形同虚设。这四条肉眼各看一眼都判不出来，所以做成可重复的数（判据清单见文件头）。
# cwd 必须是构建目录；需要一段 3 秒素材，缺了会 SKIP 相关组并打印生成命令。
#   ./build/build_rk3588_linux/test_stream_params，rc 0=PASS 1=FAIL
# 源列表与 test_channel_reload 同一套（ffmpeg_video_decoder.cpp 的依赖闭包）
# ============================================================================
add_executable(test_stream_params
    tests/test_stream_params.cpp
    src/media/reader/ffmpeg_video_decoder.cpp
    src/media/reader/video_recorder.cpp
    src/ai/task/ppe_task.cpp
    src/ai/task/base_task.cpp
    src/ai/model_repo/model_registry.cpp
    src/base/config/ConfigManager.cpp
    src/base/config/parse_config.cpp
    src/base/buffer/dma_alloc.cpp
    src/base/buffer/DmaFrameBuffer.cpp
    src/biz/alarm/alarm_manager.cpp
    src/biz/alarm/alarm_labels.cpp
    src/biz/alarm/alarm_media_prune.cpp
    src/biz/session/construction_session.cpp
    src/biz/db/alarm_dao.cpp
    src/biz/db/detection_dao.cpp
    src/biz/db/database_manager.cpp
    src/biz/geofence/fence_manager.cpp
    src/biz/geofence/native_fence_store.cpp
    src/biz/geofence/pad_fence_store.cpp
    src/biz/geofence/fence_checker.cpp
    src/base/imaging/image_utils.cpp
    src/base/imaging/image_drawing.cpp
    src/base/imaging/draw_utils.cpp
    src/base/imaging/file_utils.cpp
    src/biz/service/task_manager.cpp
    # 同 test_channel_reload：ffmpeg_video_decoder.cpp 依赖 RtspLiveService
    src/media/server/rtsp_live_service.cpp
    src/media/server/zlm_env.cpp
    src/media/server/http_file_service.cpp
    src/media/server/net_route.cpp
)

target_link_directories(test_stream_params PRIVATE
    ${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg-rkmpp/lib
    ${CMAKE_SOURCE_DIR}/3rdparty/libyuv/lib
    ${CMAKE_SOURCE_DIR}/3rdparty/librga/lib
    ${CMAKE_SOURCE_DIR}/3rdparty/rknpu2/Linux/aarch64
    ${CMAKE_SOURCE_DIR}/3rdparty/jsoncpp/lib
    ${CMAKE_SOURCE_DIR}/3rdparty/zlmediakit/aarch64
    ${CMAKE_SOURCE_DIR}/3rdparty/openssl1.1.0/lib
)

target_link_libraries(test_stream_params
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Sql
    Qt5::Network
    avformat
    avcodec
    avutil
    swscale
    swresample
    pthread
    dl
    drm
    jsoncpp
    sqlite3
    rknnrt
    rga
    mk_api
    ssl crypto
    ${OpenCV_LIBS}
    ${GLES2_LIBRARIES}
    ${EGL_LIBRARIES}
    ${LIBJPEG}
    ${LIBRGA}
    Threads::Threads
)
# 与主目标同宏：零拷贝流水线（DMA-BUF/RGA）在 decodeLoop 里是编译期分支
target_compile_definitions(test_stream_params PRIVATE ZERO_COPY)

target_link_options(test_stream_params PRIVATE "LINKER:--allow-shlib-undefined" "LINKER:--disable-new-dtags")

# ============================================================================
# 本机对外网卡的判据离线自检（/proc/net/route 文本进、网卡名顺序出）
# 为什么单独一个 suite：这段判据决定了媒体地址（evidenceSnapshotUrl / clipUrl /
# liveUrl）自动口径挑哪张口，挑错 = Pad 收到的地址全是 4G 模块那个点对点口，
# 图片和直播一起挂（2026-11-05 现场）。它不碰 mk_*、也不碰 socket，
# 所以只编 net_route.cpp 一个文件就能把十一条合成用例 + 一条真表自洽跑完。
# 仓库根运行：./build/build_rk3588_linux/test_net_route，rc 0=PASS 1=FAIL
# ============================================================================
add_executable(test_net_route
    tests/test_net_route.cpp
    src/media/server/net_route.cpp
)

target_include_directories(test_net_route PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/media/server
)

target_link_libraries(test_net_route
    Qt5::Core
    Qt5::Network
)

# ============================================================================
# 电子围栏图文说明的离屏出图工具（tools/geofence_shots）
# 底图用板端真实报警快照，围栏由真 FenceOverlay + 合成鼠标事件提交，判定由真
# FenceChecker 出，落库写到临时 idge.db —— 不碰现场的库和 config.json。
# 仓库根运行：./build/build_rk3588_linux/geofence_shots <出图目录> <底图jpg>
# ============================================================================
add_executable(geofence_shots
    tools/geofence_shots/main.cpp
    src/biz/geofence/fence_overlay.cpp
    src/biz/geofence/fence_manager.cpp
    src/biz/geofence/fence_checker.cpp
    src/biz/geofence/fence_frame_renderer.cpp
    src/biz/geofence/pad_fence_store.cpp
    src/biz/geofence/native_fence_store.cpp
    src/biz/db/database_manager.cpp
    src/biz/session/construction_session.cpp
    src/base/config/ConfigManager.cpp
)

target_link_libraries(geofence_shots
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Sql
    sqlite3
    pthread
)

target_compile_definitions(geofence_shots PRIVATE ZERO_COPY)
