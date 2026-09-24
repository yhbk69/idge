# IDGE - 施工行为监测与分析系统

基于 Rockchip RK3588 边缘计算平台的施工行为监测与分析系统，集成视频解码、YOLO 目标检测、电子围栏、报警管理、人员点名、设备盘点和 Qt GUI，用于实时监控与分析施工现场的安全行为。

---

# 一、快速开始（使用方法）

## 1. 一键运行

```bash
# 自动设置 Mali EGL / rkmpp FFmpeg / Qt5 库路径，并启动 GUI
./run.sh
```

> **注意**：必须加载 Mali EGL（而非 Mesa），否则渲染黑屏。`run.sh` 已自动处理。

## 2. 工作区目录约定（IDGE_WORKSPACE）

人员点名与设备盘点两个业务模块的所有资源路径以环境变量 `IDGE_WORKSPACE` 为根目录，**缺省回退到当前工作目录**（见 `src/form/frmmain.cpp`）：

```bash
export IDGE_WORKSPACE=/home/pi/cc/idge_work   # 示例
./run.sh
```

工作区内的目录结构约定：

```
$IDGE_WORKSPACE/
├── model/
│   ├── face/                      # 人员点名三件套
│   │   ├── detection.rknn         # SCRFD 人脸检测模型
│   │   ├── recognition.rknn       # 人脸特征比对模型
│   │   └── face_recognition       # 模型基路径（前缀约定）
│   ├── coco/                      # 设备盘点模型 1（COCO 通用类）
│   │   ├── rknn_yolo11_demo       # 模型基路径（前缀约定）
│   │   └── model/
│   │       ├── yolo11.rknn
│   │       └── coco_80_labels_list.txt
│   └── fire/                      # 设备盘点模型 2（可扩展其他类别）
│       └── model/ ...
└── roll_call_data/                # 点名/盘点业务数据
    └── roll_call.db               # 业务 SQLite 库（任务/人脸/设备表）
```

模型文件缺失时对应业务页初始化会弹窗告警，但视频监控主功能不受影响。

## 3. 主检测配置（config.json）

程序启动时读取项目根目录 `config.json`（可从 `config_example.json` 复制），在 **系统设置页** 修改后自动回写。关键配置：

| 配置节 | 说明 |
|--------|------|
| `detect` | 检测阈值：`conf_threshold` / `nms_threshold` / `threads`（NPU 并行数，RK3588 建议 3） |
| `video` | 4 路通道视频源（本地 mp4 或 RTSP 地址）与备注 |
| `cascade` | 级联模型列表（最多 5 组 path + label，多模型串行检测） |
| `geofence` | 每路通道的电子围栏：多边形顶点（widget 像素坐标）、报警类别 `alarmClasses`、开关 |
| `alarm` | 参与报警的类别列表 |

报警记录写入根目录 `idge.db`（Qt SQL 检测库），抓拍图片保存在 `alarms/YYYYMMDD/` 目录。

## 4. 其他常用脚本

```bash
./build-linux.sh -t rk3588 -b Release   # 构建（见下文"构建"）
./video-preview.sh                       # MIPI CSI 摄像头预览调试
./dump_hang.sh                           # 卡死现场抓取（gdb 堆栈）
```

## 5. 页面操作速览

| 页面 | 入口 | 功能 |
|------|------|------|
| 视频监控 | 顶部"视频监控" | 4 路实时画面 + FPS + 围栏叠加显示 |
| 系统设置 | 顶部"系统设置" | 通道/模型/阈值/围栏配置，即改即存 |
| 报警查询 | 顶部"报警查询" | 历史报警列表、图片查看、确认状态 |
| 人员点名 | 顶部"人员点名" | 人脸注册 / 现场点名 / 注销比对（摄像头抓拍） |
| 设备盘点 | 顶部"设备盘点" | 拍照 → YOLO 识别 → 盘点清单与明细 |
| 使用帮助 | 顶部"帮助" | 调试信息与运行日志 |

---

# 二、主要特性

- **全链路零拷贝**：从 FFmpeg 解码到 EGL 渲染，全程通过 DMA-BUF fd 传递，无 CPU 内存拷贝
- **3 NPU 核心并行推理**：充分利用 RK3588 的 3 个 NPU 核心，实现高吞吐量检测
- **RGA 硬件加速预处理**：色彩转换和图像缩放由 RGA 引擎完成，释放 CPU
- **电子围栏**：多边形围栏绘制，inside/outside 两种侵入判定模式，按类别过滤报警
- **报警管理**：限流去重、抓拍落盘、SQLite 持久化、GUI 悬浮提示与历史查询
- **人员点名**：SCRFD 人脸检测 + 特征比对，注册 / 点名 / 注销三阶段，矩阵批量相似度 + 贪心匹配
- **设备盘点**：复用 YOLO11 RKNN 模型做设备识别与盘点任务管理
- **Qt5 GUI**：blacksoft 换肤框架，无边框窗体，多页面导航
- **多路视频支持**：支持 4 路视频同时监控
- **模型灵活配置**：支持 YOLO11 nano/small/medium 不同大小模型与级联多模型

# 三、技术架构

## 核心技术栈

| 技术 | 用途 |
|------|------|
| C++17 | 主要开发语言 |
| Qt5 | GUI 框架 |
| RKNN Runtime (rknpu2) | Rockchip NPU 推理引擎 |
| FFmpeg (rkmpp) | 视频硬件解码 (h264_rkmpp) |
| RGA | 硬件 2D 图像加速（色彩转换 + 缩放） |
| OpenGL ES 2.0 / EGL | GPU 零拷贝视频渲染 |
| SQLite3 | 检测库 (idge.db) + 业务库 (roll_call.db) |
| Eigen | 人脸特征批量相似度矩阵计算 |
| CMake 3.10+ | 构建系统 |

## 视频处理全流程

### 输入格式
视频源可以是本地 `.mp4` 文件或 RTSP 摄像头流，编码格式为 **H.264 (AVC)**。文件通过 FFmpeg 的 `avformat` 打开，由 `h264_rkmpp` 硬件解码器在 RK3588 的 VPU 上解码。

### 格式转换链路
```
H.264 编码流
    ↓  FFmpeg h264_rkmpp 硬解码（VPU 硬件解码）
NV12 DMA-BUF fd（YUV 420 半平面，GPU 可直接访问）
    ↓  RGA 硬件色彩转换（fd → fd，零拷贝）
RGBA8888 DMA-BUF fd（RGBA 4 通道，OpenGL 可直接导入）
    ↓  CPU 侧画框 + 标签（draw_rectangle / draw_text，mmap 直写像素）
RGBA8888 DMA-BUF fd（带检测框的最终画面）
    ↓  dup(fd) 传递给渲染线程
    ├──→ EGLImage 导入 DMA-BUF（零拷贝）→ OpenGL ES 纹理绘制 → 屏幕显示
    └──→ RGA 缩放到 640×640 RGB → RKNN NPU 3 核并行推理（YOLO11）
             ↓
         NMS 后处理 → 检测结果队列 → 画框叠加到下一帧
             ↓
         电子围栏判定（脚点坐标映射到 widget 空间）→ 报警限流 → 抓拍落盘 + 写 idge.db
```

### 各阶段详解

| 阶段 | 输入格式 | 输出格式 | 执行硬件 | 关键代码 |
|------|----------|----------|----------|----------|
| 解码 | H.264 码流 | NV12 DMA-BUF | VPU (MPP) | `src/reader/ffmpeg_video_decoder.cpp` |
| 色彩转换 | NV12 DMA-BUF | RGBA DMA-BUF | RGA 硬件 | `src/rga/rga_converter.h` |
| 推理预处理 | RGBA DMA-BUF | 640×640 RGB DMA-BUF | RGA 硬件 | `RgaConverter::rgba_to_rgb_resize()` |
| NPU 推理 | 640×640 RGB | 检测结果 (80类) | NPU ×3 | `src/yolo11/yolo11_model.hpp` |
| 画框叠加 | RGBA DMA-BUF + 检测结果 | 带框的 RGBA DMA-BUF | CPU (mmap) | `src/utils/draw_utils.*` |
| 渲染 | RGBA DMA-BUF fd | 屏幕像素 | GPU (Mali) | `src/ui/EglImageRenderer.h` |
| 围栏/报警 | 检测结果 | 报警记录 + 抓拍图 | CPU | `src/geofence/`、`src/alarm/` |

## 业务模块调用链

```
人员点名：摄像头预览(src/reader/camera_preview_decoder) → 抓拍 JPEG
    → SCRFD 检测(src/reader/scrfd_face_detector) → 人脸对齐裁剪
    → 特征提取(src/recognition/face_recognizer)
    → 注册/比对/注销(src/service/roll_call_service)
    → 余弦相似度矩阵(Eigen) + 贪心一对一匹配 → roll_call.db

设备盘点：拍照 → YOLO11 识别(多模型 coco/fire) → 结果去重汇总
    → src/service/equipment_inventory_service → roll_call.db 任务/设备表
```

---

# 四、目录结构与模块文档索引

**每个源码目录都有 README.md 详述功能、数据流、使用方法与注意事项**，下表为速查索引。

| 目录 | 职责 | 文档 |
|------|------|------|
| `src/main.cpp` | 应用入口（CLI/GUI 双模式、EGL 初始化） | — |
| `src/form/` | 主窗口与全部页面/对话框（视频、设置、点名、盘点） | [README](src/form/README.md) |
| `src/ui/` | EGL/OpenGL ES 零拷贝渲染、视频控件、看板、报警列表 | [README](src/ui/README.md) |
| `src/reader/` | FFmpeg 解码线程、摄像头预览解码、SCRFD 人脸检测 | [README](src/reader/README.md) |
| `src/yolo11/` | YOLO11 RKNN 推理核心、前后处理 | [README](src/yolo11/README.md) |
| `src/model/` | 推理模型池（NPU 核心借还调度） | [README](src/model/README.md) |
| `src/task/` | 检测任务体系（安全帽/PPE 任务与任务池） | [README](src/task/README.md) |
| `src/recognition/` | 人脸特征提取与比对 | [README](src/recognition/README.md) |
| `src/service/` | 人员点名 / 设备盘点业务服务 | [README](src/service/README.md) |
| `src/alarm/` | 报警管理（限流、去重、抓拍、SQLite 持久化） | [README](src/alarm/README.md) |
| `src/geofence/` | 电子围栏（形状、判定、绘制 overlay） | [README](src/geofence/README.md) |
| `src/db/` | 业务数据库 BusinessDBManager（roll_call.db） | [README](src/db/README.md) |
| `src/database/` | 检测/报警持久层（idge.db：DatabaseManager + DAO） | [README](src/database/README.md) |
| `src/config/` | ConfigManager 单例 + config.json 解析 | [README](src/config/README.md) |
| `src/buffer/` | DMA-BUF 分配与帧缓冲池 | [README](src/buffer/README.md) |
| `src/queue/` | 阻塞队列 / 帧队列 / 优先级队列 | [README](src/queue/README.md) |
| `src/threadpool/` | 通用线程池 | [README](src/threadpool/README.md) |
| `src/rga/` | RGA 硬件加速封装 | [README](src/rga/README.md) |
| `src/utils/` | 图像/绘制/路径/日志等通用工具 | [README](src/utils/README.md) |
| `src/core_helper/` | 无边框窗体、图标字体、QSS 换肤组件 | [README](src/core_helper/README.md) |
| `src/core_qss/` | blacksoft 皮肤资源 | [README](src/core_qss/README.md) |
| `model/` | 检测模型与标签（yolo11n/s/m、coco 标签等） | [README](model/README.md) |
| `python/` | ONNX→RKNN 转换与验证脚本 | [README](python/README.md) |
| `tests/` | 数据库层测试（test_database，随主构建生成） | [README](tests/README.md) |
| `docs/` | 技术知识库 | [README](docs/README.md) |
| `include/` | nlohmann/json 单头文件 | [README](include/README.md) |
| `res/` | Qt 资源（qrc：图片/字体/GL 着色器/音效） | [README](res/README.md) |
| `sounds/` | 提示音文件 | [README](sounds/README.md) |
| `3rdparty/` | 第三方库 | [README](3rdparty/README.md) |

运行时数据目录（已 gitignore，不入库）：`alarms/`（按日报警抓拍）、`roll_call_data/`（业务库与照片）、`backups/`、`build*/`、`install/`、根目录 `idge.db*`。

### 根目录文件

```
├── CMakeLists.txt              # 主构建配置
├── build-linux.sh              # 构建脚本（-t rk3588 -b Release）
├── run.sh                      # 一键启动（自动设置库路径）
├── video-preview.sh            # MIPI CSI 摄像头预览脚本
├── dump_hang.sh                # 卡死堆栈抓取
├── config.json                 # 运行配置（UI 即改即存）
├── config_example.json         # 配置模板
└── yolo11_videocapture_demo.cc # 独立 YOLO11 摄像头检测示例
```

---

# 五、构建

## 系统要求

- **硬件**：Rockchip RK3588/RK3576/RK356x，建议 4GB+ 内存
- **操作系统**：Linux (aarch64)
- **依赖库**：见 `3rdparty/` 目录（版本明细见其 [README](3rdparty/README.md)）

## 1. 构建 rkmpp FFmpeg（板子上原生编译）

系统自带的 FFmpeg 4.x 不支持 rkmpp 硬解码，需从源码编译带 rkmpp 支持的 FFmpeg 6.1：

```bash
cd /tmp
git clone https://github.com/nyanmisaka/ffmpeg-rockchip.git -b 6.1 ffmpeg-rockchip
cd ffmpeg-rockchip
./configure \
    --enable-rkmpp --enable-libdrm --enable-version3 \
    --enable-shared --disable-static \
    --disable-doc --disable-debug \
    --prefix=/path/to/idge-main/3rdparty/ffmpeg-rkmpp
make -j$(nproc) && make install
```

## 2. 构建项目

```bash
./build-linux.sh -t rk3588 -b Release

# 可选参数：
# -t <target>   目标平台: rk356x / rk3588 / rk3576
# -b <type>     构建类型: Debug / Release
# -m            启用 Address Sanitizer (需要 Debug 模式)
# -r            禁用 RGA, 使用 CPU 缩放
# -d            启用 DMA32 (RGA2, 低于4G内存)
```

构建产物安装到 `install/rk3588_linux/`，可执行文件 RPATH 已设为 `$ORIGIN/../lib`。

## 3. 手动运行（开发调试）

```bash
export LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu/mali:3rdparty/ffmpeg-rkmpp/lib:/usr/local/Qt-5.15.18/lib:$LD_LIBRARY_PATH
./build/build_rk3588_linux/idge
```

### 关键环境变量

| 变量 | 值 | 说明 |
|------|------|------|
| `DISPLAY` | `:0` | X11 显示服务器 |
| `LD_LIBRARY_PATH` | 见上 | 动态库搜索路径 |
| `XDG_RUNTIME_DIR` | `/run/user/$(id -u)` | 用户运行时目录 |
| `IDGE_WORKSPACE` | 业务资源根目录 | 点名/盘点模型与数据（缺省=当前目录） |
| `EGL_NO_X11` | 构建时全局定义 | 强制 EGL 走 DRM 后端（CMake 已处理） |

### 库路径说明

| 库 | 路径 | 说明 |
|----|------|------|
| Mali EGL/GLES | `/usr/lib/aarch64-linux-gnu/mali/` | GPU 渲染驱动（系统安装） |
| ffmpeg-rkmpp | `3rdparty/ffmpeg-rkmpp/lib/` | 带 rkmpp 支持的 FFmpeg 6.1（板子原生编译） |
| Qt5 | `/usr/local/Qt-5.15.18/lib/` | Qt5 运行库（板子安装） |
| librga | `/usr/lib/` | RGA 硬件加速库（系统自带） |

---

# 六、模型信息

- **模型格式**：RKNN (Rockchip Neural Network)，转换方法见 [python/README.md](python/README.md)
- **检测模型**：YOLO11 nano/small/medium（COCO 80 类），输入 640×640（LetterBox 预处理）
- **人脸模型**：SCRFD 检测 + 特征识别，放置于 `$IDGE_WORKSPACE/model/face/`
- **量化支持**：uint8 / int8 / float32
- **NPU 核心**：可指定 RK3588 的 3 个 NPU 核心（`detect.threads` 配置）

# 七、第三方库版本

明细见 [3rdparty/README.md](3rdparty/README.md)。摘要：FFmpeg(rkmpp) 6.1.1、MPP 1.3.10、RKNN Runtime、librga 1.10.0、OpenCV、Qt5 5.15.18、jsoncpp 1.9.7、libjpeg-turbo、libyuv、libsndfile、OpenSSL、ZeroMQ、FFTW、stb、OpenCL stub、DRM 分配头文件、sqlite3。

# 八、已知问题与技术债

以下问题已在源码中以注释警示（详见各目录 README 的"注意事项"）：

1. 点击关闭按钮后，解码线程和 player_widget 没有完全关闭，解码线程可能仍在运行
2. 报警时间戳口径：生产端 `results.time` 为 epoch 纳秒（common.hpp"毫秒"注释是文档误差）；限流窗口已改用 steady_clock 单调纳秒、与墙钟解耦；detections 表写入口已统一换算为毫秒
3. `DmaBufferPool::acquire` 返回裸指针有生命周期约束（池回收后悬垂）；`DmaFrameBuffer` 浅拷贝双释放隐患已通过 `= delete` 修复
4. `src/queue/priority_queue` 的 `waitAndPop` 出队不移除元素、push 不 notify（`tryPop` 已修复为取出即删除）；`src/queue/frame_queue` 的 `pushAndReplace` 存在错序释放隐患
5. `src/form/photo_selection_dialog.h` 为无引用遗留文件；识别结果对话框存在未调用的画框死代码
6. 围栏报警 `bypassThrottle` 当前所有调用方均传 false——围栏报警同样受 2 秒限流窗口约束，"围栏不限流"的设计意图尚未落地

**已修复批次（2026-09-24，明细见 `plan/update_log.md`）**：DmaBufferPool fd 缺省 0→-1（close(0) 误关 stdin）、free 顺序致 DMA 泄漏、dma_alloc 失败路径 fd 泄漏、rga_converter 恒真返回值/错误路径句柄泄漏/letterbox 未填灰边、shell 命令注入（face_recognizer 改 fork+execv、roll_call 回退改 QProcess）、detections 表 ns 存值 vs ms 清理失配（含存量数据一次性迁移）、报警限流墙钟回拨致抑制数天（改 steady_clock）。注：批内曾按"get_bpp_from_format 返回位宽"结论做 /8 缓冲换算，板端实测证伪（该函数返回**字节**/像素）并已回退，详见 update_log 回归实录。

# 九、文档

- 各模块文档：见上文[目录结构与模块文档索引](#四目录结构与模块文档索引)
- `docs/knowledge/` 技术知识库（详见 [docs/README.md](docs/README.md)）：AlertGateway 技术总结、Docker 部署、H264 封装图解、NPU 全局调度、RGA 预处理加速、YOLOv8s-RK3588 性能优化、NV12 渲染原理、WebRTC 远程桌面、多媒体硬解记录等

# 十、贡献指南

1. Fork 本仓库
2. 创建特性分支 (`git checkout -b feature/AmazingFeature`)
3. 提交更改 (`git commit -m 'Add some AmazingFeature'`)
4. 推送到分支 (`git push origin feature/AmazingFeature`)
5. 创建 Pull Request

# 许可证

本项目未指定许可证，请联系项目维护者获取许可信息。

# 联系方式

项目托管在 GitLab：https://146.56.223.127:30000/iwatcher/idge
