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
| FFmpeg | 视频解码 (h264_rkmpp/h264 硬解码) |
| RGA | 硬件 2D 图像加速 |
| OpenGL ES 3.0 / EGL | GPU 零拷贝视频渲染 |
| OpenCV 4.10 | 图像处理辅助 |
| CMake 3.10+ | 构建系统 |

### 系统架构流程
```
摄像头/视频文件
    ↓
FFmpeg RKMPP 硬解码 (Drm Prime)
    ↓
RGA 零拷贝色彩转换 + 缩放
    ↓
┌─────────────────┐  ┌─────────────────────┐
│ EGL 渲染 (零拷贝) │  │ 检测任务队列          │
│ DMA-BUF fd      │  │ 3个NPU核心并行推理    │
│ -> EGLImage     │  │ YOLO11n 推理 (RKNN)  │
│ -> GL 纹理      │  │ NMS 后处理            │
└─────────────────┘  └─────────────────────┘
```

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

### 2. 构建项目
```bash
# 在 RK3588 板子上交叉编译
./build-linux.sh -t rk3588 -b Release

# 可选参数：
# -t <target>   目标平台: rk356x / rk3588 / rk3576
# -b <type>     构建类型: Debug / Release
# -m            启用 Address Sanitizer (需要 Debug 模式)
# -r            禁用 RGA, 使用 CPU 缩放
# -d            启用 DMA32 (RGA2, 低于4G内存)
```

### 3. 构建产物
构建完成后，可执行文件将安装到 `install/rk3588_linux/` 目录。

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

### 依赖库说明
| 库 | 来源 | 用途 |
|----|------|------|
| Mali EGL/GLES | `/usr/lib/aarch64-linux-gnu/mali/` | GPU 零拷贝渲染 |
| ffmpeg-rkmpp | `3rdparty/ffmpeg-rkmpp/lib/` | h264_rkmpp 硬件解码 |
| librga | 系统 `/usr/lib/` | RGA 硬件加速 |

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