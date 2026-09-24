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

人员点名与设备盘点两个业务模块的所有资源路径以环境变量 `IDGE_WORKSPACE` 为根目录，**缺省回退到当前工作目录**（见 `src/ui/form/frmmain.cpp`）：

```bash
export IDGE_WORKSPACE=/home/pi/cc/idge_work   # 示例
./run.sh
```

工作区内的目录结构约定：

```
$IDGE_WORKSPACE/
├── model/
│   ├── face/                      # 人员点名权重（两件套 .rknn，进程内推理直接加载）
│   │   ├── detection.rknn         # SCRFD 人脸检测模型
│   │   ├── recognition.rknn       # 人脸特征比对模型
│   │   └── face_recognition       # CLI 对照工具构建产物（点名已进程内化，主程序不再调起）
│   └── library/                   # 模型库：设备盘点权重经 ModelRegistry 从这里解析
│       └── yolo11n-coco/          # 当前盘点主力权重（明火模型入库后自动加入盘点清单）
│           ├── model.rknn
│           └── labels.txt
└── data/
    └── roll_call_data/            # 点名/盘点业务数据
        └── roll_call.db               # 业务 SQLite 库（任务/人脸/设备表）
```

模型文件缺失时对应业务页初始化会弹窗告警，但视频监控主功能不受影响。

## 3. 主检测配置（config.json）

程序启动时读取 `data/config.json`（首次可从 `config_example.json` 复制：`mkdir -p data && cp config_example.json data/config.json`；旧版根目录 config.json 启动时自动迁移），在 **系统设置页** 修改后自动回写。关键配置：

| 配置节 | 说明 |
|--------|------|
| `detect` | 检测阈值：`conf_threshold` / `nms_threshold` / `threads`（NPU 并行数，RK3588 建议 3） |
| `video` | 4 路通道视频源（本地 mp4 或 RTSP 地址）与备注 |
| `cascade` | 级联模型列表（最多 5 组 path + label，多模型串行检测） |
| `geofence` | 每路通道的电子围栏：多边形顶点（widget 像素坐标）、报警类别 `alarmClasses`、开关 |
| `alarm` | 参与报警的类别列表 |

报警记录写入 `data/idge.db`（Qt SQL 检测库），抓拍图片保存在 `data/alarms/YYYYMMDD/` 目录。

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
- **人员点名**：SCRFD 人脸检测 + 特征比对（进程内 RKNN 常驻推理），注册 / 点名 / 注销三阶段，余弦相似度 + 贪心一对一匹配
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
| 解码 | H.264 码流 | NV12 DMA-BUF | VPU (MPP) | `src/media/reader/ffmpeg_video_decoder.cpp` |
| 色彩转换 | NV12 DMA-BUF | RGBA DMA-BUF | RGA 硬件 | `src/media/rga/rga_converter.h` |
| 推理预处理 | RGBA DMA-BUF | 640×640 RGB DMA-BUF | RGA 硬件 | `RgaConverter::rgba_to_rgb_resize()` |
| NPU 推理 | 640×640 RGB | 检测结果 (80类) | NPU ×3 | `src/ai/yolo11/yolo11_model.hpp` |
| 画框叠加 | RGBA DMA-BUF + 检测结果 | 带框的 RGBA DMA-BUF | CPU (mmap) | `src/base/utils/draw_utils.*` |
| 渲染 | RGBA DMA-BUF fd | 屏幕像素 | GPU (Mali) | `src/ui/widgets/EglImageRenderer.h` |
| 围栏/报警 | 检测结果 | 报警记录 + 抓拍图 | CPU | `src/biz/geofence/`、`src/biz/alarm/` |

## 业务模块调用链

```
人员点名：摄像头预览(src/media/reader/camera_preview_decoder) → 抓拍 JPEG
    → SCRFD 检测 + 五点仿射对齐 + 512 维特征提取(进程内常驻
      src/ai/recognition/in_process_face_recognizer)
    → 注册/比对/注销(src/biz/service/roll_call_service)
    → 余弦相似度(归一化点积) + 贪心一对一匹配 → roll_call.db

设备盘点：拍照 → 进程内 YOLO11 识别(src/ai/yolo11，权重=model/library/yolo11n-coco)
    → 结果去重汇总 → src/biz/service/equipment_inventory_service → roll_call.db 任务/设备表
```

---

# 四、目录结构与模块文档索引

**每个源码目录都有 README.md 详述功能、数据流、使用方法与注意事项**，下表为速查索引。

### 功能域总览（依赖方向 base ← media ← ai ← biz ← ui）

| 域 | 模块 |
|------|------|
| `src/base/` 基础设施 | buffer、queue、threadpool、config、utils、runtime_paths.h |
| `src/media/` 视频管线 | reader、rga、model(ModelPool) |
| `src/ai/` 推理与模型应用 | yolo11、model_repo、recognition、task |
| `src/biz/` 业务域 | alarm、geofence、service、db(报警库+业务库) |
| `src/ui/` 界面层 | form、widgets(原 src/ui)、core_helper、core_qss |

### 模块速查

| 目录 | 职责 | 文档 |
|------|------|------|
| `src/main.cpp` | 应用入口（CLI/GUI 双模式、EGL 初始化、data/ 旧布局迁移） | — |
| `src/base/runtime_paths.h` | 运行时数据路径唯一事实源（data/ 约定） | [README](src/base/README.md) |
| `src/ui/form/` | 主窗口与全部页面/对话框（视频、设置、点名、盘点） | [README](src/ui/form/README.md) |
| `src/ui/widgets/` | EGL/OpenGL ES 零拷贝渲染、视频控件、看板、报警列表 | [README](src/ui/widgets/README.md) |
| `src/ui/core_helper/` | 无边框窗体、图标字体、QSS 换肤组件 | [README](src/ui/core_helper/README.md) |
| `src/ui/core_qss/` | blacksoft 皮肤资源 | [README](src/ui/core_qss/README.md) |
| `src/media/reader/` | FFmpeg 解码线程、摄像头预览解码、SCRFD 人脸检测 | [README](src/media/reader/README.md) |
| `src/media/rga/` | RGA 硬件加速封装 | [README](src/media/rga/README.md) |
| `src/media/model/` | 推理模型池（NPU 核心借还调度） | [README](src/media/model/README.md) |
| `src/ai/yolo11/` | YOLO11 RKNN 推理核心、前后处理 | [README](src/ai/yolo11/README.md) |
| `src/ai/model_repo/` | 模型库注册表（library 扫描/元数据/级联槽位解析） | [README](src/ai/model_repo/README.md) |
| `src/ai/task/` | 检测任务体系（安全帽/PPE 任务与任务池） | [README](src/ai/task/README.md) |
| `src/ai/recognition/` | 人脸识别：进程内 `InProcessFaceRecognizer`（点名生产链）+ 外部 exe 桥接 `FaceRecognitionWrapper`（对照/CLI） | [README](src/ai/recognition/README.md) |
| `src/biz/service/` | 人员点名 / 设备盘点业务服务 | [README](src/biz/service/README.md) |
| `src/biz/alarm/` | 报警管理（限流、去重、抓拍、SQLite 持久化） | [README](src/biz/alarm/README.md) |
| `src/biz/geofence/` | 电子围栏（形状、判定、绘制 overlay） | [README](src/biz/geofence/README.md) |
| `src/biz/db/` | 数据持久层：报警库 idge.db（QtSQL DAO）+ 业务库 roll_call.db（sqlite3 原生） | [README](src/biz/db/README.md) |
| `src/base/config/` | ConfigManager 单例 + data/config.json 解析 | [README](src/base/config/README.md) |
| `src/base/buffer/` | DMA-BUF 分配与帧缓冲池 | [README](src/base/buffer/README.md) |
| `src/base/queue/` | 阻塞队列 / 帧队列 / 优先级队列 | [README](src/base/queue/README.md) |
| `src/base/threadpool/` | 通用线程池 | [README](src/base/threadpool/README.md) |
| `src/base/utils/` | 图像/绘制/路径/日志等通用工具 | [README](src/base/utils/README.md) |
| `tools/face_recognition/` | 人脸识别命令行工具（CLI 验证/点名对照回归基准，产物输出 model/face/） | [README](tools/face_recognition/README.md) |
| `model/` | 模型库 `library/<id>/`（yolo11n/s/m 收编）+ 专项标签 | [README](model/README.md) |
| `data/` | 运行时数据（config/idge.db/alarms/backups/roll_call_data，内容不入库） | — |
| `python/` | ONNX→RKNN 转换与验证脚本 | [README](python/README.md) |
| `tests/` | 测试程序（test_database 数据库层；test_equipment 设备盘点、test_rollcall 人员点名全链离线自测，均随主构建生成） | [README](tests/README.md) |
| `docs/` | 技术知识库 | [README](docs/README.md) |
| `include/` | nlohmann/json 单头文件 | [README](include/README.md) |
| `res/` | Qt 资源（qrc：图片/字体/GL 着色器/音效） | [README](res/README.md) |
| `sounds/` | 提示音文件 | [README](sounds/README.md) |
| `3rdparty/` | 第三方库 | [README](3rdparty/README.md) |

运行时数据统一在 `data/`（内容已 gitignore，不入库）：`data/config.json`、`data/idge.db*`、`data/alarms/`（按日抓拍）、`data/backups/`、`data/roll_call_data/`。旧布局（散落在根目录）启动时由 `RuntimePaths::migrateLegacy()` 自动迁入。构建目录 `build*/`、`install/` 同样不入库。

### 根目录文件

```
├── CMakeLists.txt              # 主构建配置
├── build-linux.sh              # 构建脚本（-t rk3588 -b Release）
├── run.sh                      # 一键启动（自动设置库路径）
├── video-preview.sh            # MIPI CSI 摄像头预览脚本
├── dump_hang.sh                # 卡死堆栈抓取
├── data/                       # 运行时数据根（config.json/idge.db/alarms/backups/roll_call_data）
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
4. `src/base/queue/priority_queue` 的 `waitAndPop` 出队不移除元素、push 不 notify（`tryPop` 已修复为取出即删除）；`src/base/queue/frame_queue` 的 `pushAndReplace` 存在错序释放隐患
5. `src/ui/form/photo_selection_dialog.h` 为无引用遗留文件；识别结果对话框存在未调用的画框死代码
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
