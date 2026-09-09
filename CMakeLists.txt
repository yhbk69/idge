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
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -pg")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -pg")

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
find_package(Qt5 COMPONENTS Core Gui Sql OpenGL Qml Quick Charts Widgets LinguistTools Network Sql REQUIRED)

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
    ${CMAKE_SOURCE_DIR}/include

    # === OpenCV (备用路径) ===
    ${CMAKE_SOURCE_DIR}/3rdparty/opencv/include/opencv4

    # === Rockchip RGA 2D 加速库 ===
    ${CMAKE_SOURCE_DIR}/3rdparty/librga/include

    # === Rockchip RKNN NPU 推理库 ===
    ${CMAKE_SOURCE_DIR}/3rdparty/rknpu2/include

    # === Rockchip MPP 媒体处理平台 ===
    ${CMAKE_SOURCE_DIR}/3rdparty/mpp/include

    # === ZLMediaKit 流媒体服务器 ===
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

    # ====================================================================
    # 项目源码目录（按模块分组）
    # ====================================================================

    # --- 核心模块 ---
    ${CMAKE_SOURCE_DIR}/src                         # 根目录
    ${CMAKE_SOURCE_DIR}/src/config                  # 配置管理（CLI/JSON 解析）
    ${CMAKE_SOURCE_DIR}/src/core_helper             # 核心辅助（单例、样式、日志）
    ${CMAKE_SOURCE_DIR}/src/core_qss                # QSS 样式表
    ${CMAKE_SOURCE_DIR}/src/draw                    # 绘图模块
    ${CMAKE_SOURCE_DIR}/src/engine                  # 引擎模块
    ${CMAKE_SOURCE_DIR}/src/model                   # 模型管理（RKNN 模型池）
    ${CMAKE_SOURCE_DIR}/src/process                 # 处理模块
    ${CMAKE_SOURCE_DIR}/src/rkmedia                 # RK 媒体库封装
    ${CMAKE_SOURCE_DIR}/src/rkmedia/utils           # RK 媒体工具
    ${CMAKE_SOURCE_DIR}/src/task                    # 任务管理（检测任务、线程池）
    ${CMAKE_SOURCE_DIR}/src/RtspWorker              # RTSP 流处理
    ${CMAKE_SOURCE_DIR}/src/stream                  # 流处理
    ${CMAKE_SOURCE_DIR}/src/threadpool              # 线程池
    ${CMAKE_SOURCE_DIR}/src/types                   # 类型定义
    ${CMAKE_SOURCE_DIR}/src/utils                   # 工具函数（文件、图像、路径）
    ${CMAKE_SOURCE_DIR}/src/context                 # 上下文管理
    ${CMAKE_SOURCE_DIR}/src/counter                 # 计数器

    # --- 视频播放模块 ---
    ${CMAKE_SOURCE_DIR}/src/videoplay_gl            # OpenGL 视频播放
    ${CMAKE_SOURCE_DIR}/src/VideoPlay               # 视频播放器
    ${CMAKE_SOURCE_DIR}/src/player                  # 播放器控件
    ${CMAKE_SOURCE_DIR}/src/form                    # 窗口界面（主窗口、视频窗口）

    # --- YOLO 检测模块 ---
    ${CMAKE_SOURCE_DIR}/src/reader                  # 视频读取（FFmpeg/MPP 解码）
    ${CMAKE_SOURCE_DIR}/src/yolo11                  # YOLOv11 检测
    ${CMAKE_SOURCE_DIR}/src/yolo8                   # YOLOv8 检测
    ${CMAKE_SOURCE_DIR}/src/yolo8_pose              # YOLOv8 姿态估计
    ${CMAKE_SOURCE_DIR}/src/yolo8_seg               # YOLOv8 实例分割
    ${CMAKE_SOURCE_DIR}/src/yolo                    # YOLO 通用

    # --- 缓冲区管理 ---
    ${CMAKE_SOURCE_DIR}/src/buffer                  # DMA 缓冲池、帧缓冲区
    ${CMAKE_SOURCE_DIR}/src/queue                   # 帧队列（生产者-消费者）
    ${CMAKE_SOURCE_DIR}/src/infer                   # 推理模块
    ${CMAKE_SOURCE_DIR}/src/rknn                    # RKNN 封装
    ${CMAKE_SOURCE_DIR}/src/rga                     # RGA 色彩转换

    # --- 报警模块 ---
    ${CMAKE_SOURCE_DIR}/src/alarm                   # 报警管理（去重、限流）

    # --- UI 模块 ---
    ${CMAKE_SOURCE_DIR}/src/ui                      # OpenGL 渲染、EGL、数据看板
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
    "${CMAKE_SOURCE_DIR}/3rdparty/zlmediakit/aarch64"    # ZLMediaKit 流媒体库
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
)

# ============================================================================
# Qt5 大资源文件处理
# ============================================================================
# qt5_add_big_resources: 将资源文件嵌入到可执行文件中
# 使用大资源模式（big resources），适合包含大量资源的项目
qt5_add_big_resources(CORE_HELPER_SOURCES_QRC
    ${CMAKE_SOURCE_DIR}/src/core_helper/qrc/font.qrc    # 字体资源
    ${CMAKE_SOURCE_DIR}/src/core_helper/qrc/image.qrc   # 图片资源
    ${CMAKE_SOURCE_DIR}/src/core_helper/qrc/qm.qrc      # 翻译文件
    ${CMAKE_SOURCE_DIR}/src/core_helper/qrc/wasm.qrc    # WebAssembly 资源（可选）
)

qt5_add_big_resources(CORE_QRC
    ${CMAKE_SOURCE_DIR}/src/core_qss/qss.qrc            # QSS 样式表资源
)

# ============================================================================
# 国际化翻译文件
# ============================================================================
# Qt Linguist 工具用于多语言翻译
set(TS_FILES ${CMAKE_SOURCE_DIR}/src/idge_zh_CN.ts)  # 中文翻译文件

# ============================================================================
# 最终头文件包含路径
# ============================================================================
include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/utils ${STB_INCLUDES} ${LIBJPEG_INCLUDES})
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
    mk_api                      # ZLMediaKit 流媒体 API
    jsoncpp                     # JsonCpp JSON 解析库

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
