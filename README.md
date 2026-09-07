# IDGE - 施工行为监测与分析系统

基于 Rockchip RK3588 边缘计算平台的施工行为监测与分析系统，集成视频解码、YOLO 目标检测、GPU 渲染和 Qt GUI，用于实时监控施工现场的安全行为。

## 主要特性

- **全链路零拷贝**：从 FFmpeg 解码到 EGL 渲染，全程通过 DMA-BUF fd 传递，无 CPU 内存拷贝
- **3 NPU 核心并行推理**：充分利用 RK3588 的 3 个 NPU 核心，实现高吞吐量检测
- **RGA 硬件加速预处理**：色彩转换和图像缩放由 RGA 引擎完成，释放 CPU
- **Qt5 GUI**：提供完整的视频监控、系统设置、报警查询等界面
- **施工安全应用**：安全帽检测、PPE 个人防护设备检测等实际场景任务
- **多路视频支持**：支持 4 路视频同时监控
- **模型灵活配置**：支持 YOLO11 nano/small/medium 不同大小模型

## 技术架构

### 核心技术栈
| 技术 | 用途 |
|------|------|
| C++17 | 主要开发语言 |
| Qt5 | GUI 框架 |
| RKNN Runtime (rknpu2) | Rockchip NPU 推理引擎 |
| FFmpeg (rkmpp) | 视频硬件解码 (h264_rkmpp) |
| RGA | 硬件 2D 图像加速（色彩转换 + 缩放） |
| OpenGL ES 2.0 / EGL | GPU 零拷贝视频渲染 |
| CMake 3.10+ | 构建系统 |

### 视频处理全流程

#### 输入格式
视频源可以是本地 `.mp4` 文件或 RTSP 摄像头流，编码格式为 **H.264 (AVC)**。文件通过 FFmpeg 的 `avformat` 打开，由 `h264_rkmpp` 硬件解码器在 RK3588 的 VPU 上解码。

#### 格式转换链路
```
H.264 编码流
    ↓  FFmpeg h264_rkmpp 硬解码（VPU 硬件解码）
NV12 DMA-BUF fd（YUV 420 半平面，GPU 可直接访问）
    ↓  RGA 硬件色彩转换（fd → fd，零拷贝）
RGBA8888 DMA-BUF fd（RGBA 4 通道，OpenGL 可直接导入）
    ↓  CPU 侧画框 + 标签（draw_rectangle / draw_text，mmap 直写像素）
RGBA8888 DMA-BUF fd（带检测框的最终画面）
    ↓  dup(fd) 传递给渲染线程
    ↓
    ├──→ EGLImage 导入 DMA-BUF（零拷贝，无需 CPU 拷贝）
    │        ↓
    │    OpenGL ES 纹理绘制（带宽高比保持的 quad 绘制）
    │        ↓
    │    屏幕显示 + QPainter 叠加 FPS 帧率
    │
    └──→ RGA 缩放到 640×640 RGB（用于 YOLO 推理）
             ↓
         RKNN NPU 3 核并行推理（YOLO11）
             ↓
         NMS 后处理 → 检测结果队列 → 画框叠加到下一帧
```

#### 各阶段详解

| 阶段 | 输入格式 | 输出格式 | 执行硬件 | 关键代码 |
|------|----------|----------|----------|----------|
| 解码 | H.264 码流 | NV12 DMA-BUF | VPU (MPP) | `ffmpeg_video_decoder.cpp:280-294` |
| 色彩转换 | NV12 DMA-BUF | RGBA DMA-BUF | RGA 硬件 | `RgaConverter::convertNV12ToRGBAbyRGA()` |
| 推理预处理 | RGBA DMA-BUF | 640×640 RGB DMA-BUF | RGA 硬件 | `RgaConverter::rgba_to_rgb_resize()` |
| NPU 推理 | 640×640 RGB | 检测结果 (80类) | NPU ×3 | `YOLO11Model::detect()` |
| 画框叠加 | RGBA DMA-BUF + 检测结果 | 带框的 RGBA DMA-BUF | CPU (mmap) | `draw_rectangle()` + `draw_text()` |
| 渲染 | RGBA DMA-BUF fd | 屏幕像素 | GPU (Mali) | EGLImage → GL texture → quad |

## 系统要求

### 硬件要求
- **处理器**：Rockchip RK3588/RK3576/RK356x
- **内存**：建议 4GB+ RAM
- **存储**：至少 2GB 可用空间
- **摄像头**：支持 MIPI CSI 或 USB 摄像头

### 软件要求
- **操作系统**：Linux (aarch64)
- **编译器**：aarch64-linux-gnu-gcc/g++
- **依赖库**：见 `3rdparty/` 目录

## 构建说明

### 1. 环境准备
确保系统已安装交叉编译工具链：
```bash
# 检查编译器
aarch64-linux-gnu-gcc --version
aarch64-linux-gnu-g++ --version
```

### 2. 构建 rkmpp FFmpeg（板子上原生编译）
系统自带的 FFmpeg 4.x 不支持 rkmpp 硬解码，需从源码编译带 rkmpp 支持的 FFmpeg 6.1：
```bash
# 克隆源码（在板子上）
cd /tmp
git clone https://github.com/nyanmisaka/ffmpeg-rockchip.git -b 6.1 ffmpeg-rockchip
cd ffmpeg-rockchip

# 配置：启用 rkmpp + libdrm，关闭不需要的模块
./configure \
    --enable-rkmpp --enable-libdrm --enable-version3 \
    --enable-shared --disable-static \
    --disable-doc --disable-debug \
    --prefix=/path/to/idge-main/3rdparty/ffmpeg-rkmpp

# 编译安装
make -j$(nproc) && make install
```

### 3. 构建项目
```bash
# 在 RK3588 板子上构建（使用板载 aarch64 编译器）
./build-linux.sh -t rk3588 -b Release

# 可选参数：
# -t <target>   目标平台: rk356x / rk3588 / rk3576
# -b <type>     构建类型: Debug / Release
# -m            启用 Address Sanitizer (需要 Debug 模式)
# -r            禁用 RGA, 使用 CPU 缩放
# -d            启用 DMA32 (RGA2, 低于4G内存)
```

### 4. 构建产物
构建完成后，可执行文件将安装到 `install/rk3588_linux/` 目录。动态库搜索路径（RPATH）已设为 `$ORIGIN/../lib`。

## 运行说明

### GUI 模式（推荐）
```bash
# 一键启动（自动设置 Mali EGL 和 rkmpp FFmpeg 库路径）
./run.sh
```

### 手动运行
```bash
# 开发目录直接运行，需要手动设置 Mali EGL 和 rkmpp FFmpeg 库路径
export LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu/mali:3rdparty/ffmpeg-rkmpp/lib:/usr/local/Qt-5.15.18/lib:$LD_LIBRARY_PATH
./build/build_rk3588_linux/idge
```

> **注意**：必须加载 Mali EGL（而非 Mesa），否则渲染会黑屏。`run.sh` 已自动处理。

### 关键环境变量
| 变量 | 值 | 说明 |
|------|------|------|
| `DISPLAY` | `:0` | X11 显示服务器 |
| `LD_LIBRARY_PATH` | 见下 | 动态库搜索路径 |
| `XDG_RUNTIME_DIR` | `/run/user/$(id -u)` | 用户运行时目录 |

### 库路径说明
| 库 | 路径 | 说明 |
|----|------|------|
| Mali EGL/GLES | `/usr/lib/aarch64-linux-gnu/mali/` | GPU 渲染驱动（系统安装） |
| ffmpeg-rkmpp | `3rdparty/ffmpeg-rkmpp/lib/` | 带 rkmpp 支持的 FFmpeg 6.1（板子原生编译） |
| Qt5 | `/usr/local/Qt-5.15.18/lib/` | Qt5 运行库（板子安装） |
| librga | `/usr/lib/` | RGA 硬件加速库（系统自带） |

## 目录结构

```
idge-main/
├── CMakeLists.txt              # 主构建配置
├── build-linux.sh              # Linux 交叉编译脚本
├── run.sh                      # 一键启动脚本（自动设置库路径）
├── video-preview.sh            # MIPI CSI 摄像头预览脚本
├── yolo11_videocapture_demo.cc # 独立 YOLO11 视频检测示例
│
├── src/                        # 主源代码
│   ├── main.cpp                # 应用入口
│   ├── form/                   # 窗口/表单
│   ├── ui/                     # UI 渲染组件
│   ├── yolo11/                 # YOLO11 推理核心
│   ├── task/                   # 检测任务系统
│   ├── model/                  # 模型管理
│   ├── reader/                 # 视频解码器
│   ├── buffer/                 # DMA 缓冲区管理
│   ├── rga/                    # RGA 硬件加速
│   ├── queue/                  # 队列系统
│   ├── threadpool/             # 线程池
│   └── utils/                  # 工具库
│
├── model/                      # 预训练模型
│   ├── yolo11n.rknn            # YOLO11-nano
│   ├── yolo11s.rknn            # YOLO11-small
│   ├── yolo11m.rknn            # YOLO11-medium
│   └── coco_80_labels_list.txt # COCO 80类标签
│
├── python/                     # Python 工具脚本
│   ├── convert.py              # ONNX->RKNN 模型转换
│   └── yolo11.py               # YOLO11 推理验证
│
├── 3rdparty/                   # 第三方库
├── res/                        # Qt 资源文件
└── docs/                       # 文档
    ├── knowledge/              # 知识库
    ├── bug.txt                 # 已知 Bug 记录
    └── ref.txt                 # 参考项目
```

## 模型信息

- **模型格式**：RKNN (Rockchip Neural Network)
- **预训练模型**：YOLO11 nano/small/medium (COCO 80类)
- **输入尺寸**：640x640 (LetterBox 预处理)
- **支持的量化**：uint8, int8, float32
- **NPU 核心**：支持指定到 RK3588 的 3 个 NPU 核心

## 第三方库版本

| 库 | 版本 | 路径 | 用途 |
|----|------|------|------|
| FFmpeg (rkmpp) | 6.1.1 | `3rdparty/ffmpeg-rkmpp/` | h264_rkmpp 硬件解码 |
| MPP | 1.3.10 | `3rdparty/mpp/` | Rockchip 多媒体处理平台 |
| RKNN Runtime (rknpu2) | 运行时查询 | `3rdparty/rknpu2/` | NPU 推理引擎 |
| librga | 1.10.0 | `3rdparty/librga/` | RGA 硬件 2D 加速 |
| OpenCV | 4.2.0 | 系统 `/usr/lib/` | 图像处理 |
| Qt5 | 5.15.18 | `/usr/local/Qt-5.15.18/` | GUI 框架 |
| jsoncpp | 1.9.7 | `3rdparty/jsoncpp/` | JSON 解析 |
| libjpeg-turbo | 2.1.3 | `3rdparty/jpeg_turbo/` | JPEG 编解码 |
| libyuv | 1882 | `3rdparty/libyuv/` | YUV 图像缩放 |
| libsndfile | 1.2.2 | `3rdparty/libsndfile/` | 音频文件读写 |
| OpenSSL | 1.1.0l / 3.0.21 | `3rdparty/openssl1.1.0/` `3rdparty/openssl3.0.21/` | 加密/SSL |
| ZeroMQ | 4.3.5 | `3rdparty/zmq/` | 消息队列 |
| FFTW | 3.3.10 | `3rdparty/fftw/` | 快速傅里叶变换 |
| stb_image | 2.26 | `3rdparty/stb_image/` | 图片读写 (header-only) |
| stb_image_write | 1.15 | `3rdparty/stb_image/` | 图片写入 (header-only) |
| OpenCL | 2.2 (stub) | `3rdparty/opencl/` | GPU 计算接口 |
| allocator | DRM 内核头文件 | `3rdparty/allocator/` | DRM/DMA-BUF 分配 |

## 已知问题

- 点击关闭按钮后，解码线程和 player_widget 没有关闭，导致解码线程还在运行

## 文档

`docs/knowledge/` 目录包含详细的技术文档：
- AlertGateway 项目技术总结
- Docker 打包与部署方案
- H264 封装格式转换图解
- NPU 全局调度
- RGA 预处理加速方案
- YOLOv8s-RK3588 推理性能优化路线
- NV12 渲染原理图解
- WebRTC 远程桌面方案
- 多媒体硬解完成记录

## 贡献指南

1. Fork 本仓库
2. 创建特性分支 (`git checkout -b feature/AmazingFeature`)
3. 提交更改 (`git commit -m 'Add some AmazingFeature'`)
4. 推送到分支 (`git push origin feature/AmazingFeature`)
5. 创建 Pull Request

## 许可证

本项目未指定许可证，请联系项目维护者获取许可信息。

## 联系方式

项目托管在 GitLab：https://146.56.223.127:30000/iwatcher/idge