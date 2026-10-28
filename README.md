# IDGE - 施工行为监测与分析系统

基于 Rockchip RK3588 边缘计算平台的施工行为监测与分析系统，集成视频解码、YOLO 目标检测、电子围栏、报警管理、人员点名、设备盘点、二维码/铭牌 OCR 识别、GB/T 28181 国标推流和 Qt GUI，用于实时监控与分析施工现场的安全行为。

---

# 一、快速开始（使用方法）

## 1. 一键运行

```bash
/build-linux.sh -t rk3588 -b Release

# 仅做两件事：cd 进 build/build_rk3588_linux，然后执行 ./idge
./run.sh
```

库路径不在这里设置——构建目录里的 `idge` 由 CMake 写入了绝对 RPATH
（`3rdparty/*/lib` 与 Qt5 路径），所以直接跑就能找到 rkmpp FFmpeg、librga、RKNN、ZBar。
装到 `install/rk3588_linux/` 的那份则用 `$ORIGIN/../lib` 相对 RPATH，可整目录搬走。

> **注意**：必须加载 **Mali EGL**（而非 Mesa），否则渲染黑屏。本板靠系统 ldconfig /
> `/usr/lib/aarch64-linux-gnu/mali/` 生效，`run.sh` 并不处理它——若换了显示栈请自行核对
> `ldd ./idge | grep -i egl` 落到哪个实现。

## 2. 工作区目录约定（IDGE_WORKSPACE）

人员点名、设备盘点、智能识别三个业务模块的资源路径以环境变量 `IDGE_WORKSPACE` 为根目录，
**缺省回退到当前工作目录**（见 `src/ui/form/frmmain.cpp`）：

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
│       ├── yolo11n-coco/          # 当前盘点主力权重（明火模型入库后自动加入盘点清单）
│       │   ├── model.rknn
│       │   └── labels.txt
│       └── ppocrv4-ocr/           # 智能识别铭牌 OCR 权重（det/rec + labels）
│           ├── det.rknn
│           ├── rec.rknn
│           └── labels.txt
├── 3rdparty/
│   └── ppocrv4/bin/               # OCR 外部可执行（QProcess 调起，见 3rdparty/ppocrv4/README.md）
└── data/
    └── roll_call_data/            # 点名/盘点业务数据
        └── roll_call.db               # 业务 SQLite 库（任务/人脸/设备表）
```

模型文件缺失时对应业务页初始化会弹窗告警，但视频监控主功能不受影响。
智能识别的权重与可执行分处 `model/`（模型库）和 `3rdparty/`（第三方产物）两处，
缺一即卡片显示"未部署"并短路——判据是五个文件同时存在（`det.rknn`/`rec.rknn`/`labels.txt`
+ 两个 demo），见 `SmartRecognitionWidget::ocrModelsReady()`。

## 3. 主检测配置（config.json）

程序启动时读取 **当前工作目录**下的 `data/config.json`（首次可从 `config_example.json` 复制：`mkdir -p data && cp config_example.json data/config.json`；旧版根目录 config.json 启动时自动迁移），在 **设置页** 修改后自动回写。⚠ `./run.sh` 会先 `cd` 进 `build/build_rk3588_linux`，于是它读的是 `build/build_rk3588_linux/data/config.json`，与源码根的 `data/` 是两份互不相干的副本——"改了配置没生效"先确认改的是哪一份。

**schema V2（按通道 + 按模型）**：配置里不再出现任何 `.rknn`/`labels.txt` 路径，模型的唯一事实源是
`model/library/<库id>/{model.rknn,labels.txt,meta.json}`，配置只用**库目录名**引用它。
老结构（`model.path` + `cascade.models[5]` + `alarm.classes` + `video.channel1..4`/`notes`）由
`ConfigManager::migrateSchemaV2()` 在首次启动时一次性改写并落盘，之后不再触发。关键配置：

| 配置节 | 说明 |
|--------|------|
| `video.models` | 模型参数表，**以库目录名为键**：`{"yolo11n-ppe": {"alarm_class": "un_helmet", "note": "…"}}`。`alarm_class` 是**这个模型的参数**（不是槽位、不是通道），留空 = 只画框不报警；设置页"模型参数"表按行编辑 |
| `video.channels` | 数组，**下标即通道号（0-based）**，条目里的 `channel` 字段只给人看。每路 = `source`/`note`/`enabled`/`models`。`models` 的顺序就是级联槽位顺序（画面上 `M1/M2/…` 前缀、报警 `result_id`），重排会改号；`enabled=false` 只决定启动时是否开这一路（改完需重启） |
| `detect` | 检测参数。⚠ 整节当前都不生效：`conf_threshold`/`nms_threshold` 是**假开关**（推理侧不传参，实际用 `common.hpp` 的编译期 `BOX_THRESH`/`NMS_THRESH`）；`class_num`/`threads` 已连 getter 一起删掉（类别数属于每个模型，推理线程数 = 各通道模型数之和，设置页显示的是真实值），老配置里残留的这两个键程序不再读取 |
| （无 `video.render`） | 推理间隔是编译期的 `frame_count % 3`（`ffmpeg_video_decoder.cpp` 的 4.6 节），**没有配置键**，别在 JSON 里加 `render.sampleInterval`——它只会变成一个改了不生效的假开关 |
| `geofence` | 围栏**不属于级联**：复用该通道已有的检测结果、只做几何判定，不额外占用 NPU。`enabled` 总开关、`mode`（`inside_alarm`/`outside_alarm`）、`alarmClass` **单值触发类别**（默认 `person`，必须是该通道某个模型 `labels.txt` 里的叫法）、`throttleSeconds` 报警节流窗口（默认 10 秒，每通道每类别一条）、`channels` 每路的矩形/多边形顶点（overlay 像素坐标，随存 `overlay_w`/`overlay_h`，改窗口布局会偏移） |
| `database` | `storeDetections`（报警相关检测明细是否入 `detections` 表）与 `detectionRetentionDays`（保留天数）；写成 `null` 等于吃默认值 true/30 |
| `recording` | 报警视频硬件编码录制（默认 `enabled=false`，见下）；`buffer_seconds` 需 ≥ 片段窗口 3+2 秒 |
| `smartRecognition` | 智能识别四张卡片（二维码/OCR/点名/盘点）共用的取流地址；键缺省时自动探测摄像头节点（MJPEG 优先，其次 CSI/rkisp 的 NV12、YUYV），探测不到退回 `/dev/video0`。仅 getter，设置页不编辑 |
| `websocket` | Pad↔Aibox 客户端：`server_url`（空=不启用，设置页可改）+ 心跳/重连间隔（仅 JSON） |

另有一整段 **`gb28181`** 不经 `ConfigManager`，由 `src/biz/sip/gb28181_config.cpp` 直接读文件
（国标推流的平台参数与通道列表；无此段即不启用推流），但会被 `save()` 原样保留。明细见
[src/base/config/README.md](src/base/config/README.md)、[src/biz/sip/README.md](src/biz/sip/README.md)、
[src/biz/ws/README.md](src/biz/ws/README.md)。

报警记录写入 `data/idge.db`（Qt SQL 检测库），抓拍图片保存在 `data/alarms/YYYYMMDD/` 目录。
打开 `recording.enabled` 后，同一目录还会多出报警前后共 5 秒、**带检测框**的视频片段：
`ch{N}_{类别}_{HHmmss_zzz}.h265`（Annex-B 裸流，用于上传/国标）与同名 `.mp4`（网页播放副本）。
片段由后台线程在报警后 2 秒落盘，路径回填到 `alarms.video_path`，详情对话框的"视频"行可见。

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
| 数据看板 | 顶部"数据看板" | 报警统计 KPI、类别分布、通道在线状态（2 秒刷新，切走即停表） |
| 报警数据 | 顶部"报警数据" | 历史报警列表、图片查看、确认/误报操作 |
| 人员点名 | 顶部"人员点名" | 人脸注册 / 现场点名 / 注销比对（摄像头抓拍） |
| 设备盘点 | 顶部"设备盘点" | 拍照 → YOLO 识别 → 盘点清单与明细 |
| 智能识别 | 顶部"智能识别" | 二维码实时扫描（ZBar）+ 设备铭牌文字识别（PP-OCRv4 两阶段） |
| 调试设置 | 顶部"调试设置" | 通道/模型/阈值/围栏配置（即改即存）+ 运行日志 |
| 退出 | 顶部"退出" | 退出确认 |

顶部导航 8 项，按 `QToolButton` 文本分发（`frmMain::buttonClick()`）；页面控件指针
不依赖 stackedWidget 索引，插页不会错位。"智能识别"两项能力都受部署文件影响：
缺 ZBar 库则二维码卡片置灰，缺 OCR 权重/可执行则文字卡片置灰，页面本身照常可进。

---

# 二、构建

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

> 一般**不需要**手工 export：构建目录的 `idge` 已带绝对 RPATH。上面这行只在
> RPATH 里的 Qt 路径（本机是 `/usr/local/Qt-5.15.18/lib`，实际装在哪看 `ldd`）
> 与你的环境不符、需要覆盖时才用。注意直接在源码根执行本命令时，cwd 就是源码根，
> 读写的是 `./data/`；经 `run.sh` 启动则读写 `build/build_rk3588_linux/data/`。

### 关键环境变量

| 变量 | 值 | 说明 |
|------|------|------|
| `DISPLAY` | `:0` | X11 显示服务器 |
| `LD_LIBRARY_PATH` | 见上 | 动态库搜索路径 |
| `XDG_RUNTIME_DIR` | `/run/user/$(id -u)` | 用户运行时目录 |
| `IDGE_WORKSPACE` | 业务资源根目录 | 点名/盘点/智能识别模型与数据（缺省=当前目录） |
| `IDGE_PERF` | 任意非空值 | 打开性能探针日志（`paintGL` 间隔、面板刷新耗时、锁等待），板端定位卡顿时用；默认关闭且开销仅是一次 `QElapsedTimer::start()` |
| `EGL_NO_X11` | 构建时全局定义 | 强制 EGL 走 DRM 后端（CMake 已处理） |

### 库路径说明

| 库 | 路径 | 说明 |
|----|------|------|
| Mali EGL/GLES | `/usr/lib/aarch64-linux-gnu/mali/` | GPU 渲染驱动（系统安装） |
| ffmpeg-rkmpp | `3rdparty/ffmpeg-rkmpp/lib/` | 带 rkmpp 支持的 FFmpeg 6.1（板子原生编译） |
| Qt5 | `/usr/local/Qt-5.15.18/lib/` | Qt5 运行库（板子安装） |
| librga | `/usr/lib/` | RGA 硬件加速库（系统自带） |

---

# 三、主要特性

- **全链路零拷贝**：从 FFmpeg 解码到 EGL 渲染，全程通过 DMA-BUF fd 传递，无 CPU 内存拷贝
- **3 NPU 核心并行推理**：充分利用 RK3588 的 3 个 NPU 核心，实现高吞吐量检测
- **RGA 硬件加速预处理**：色彩转换和图像缩放由 RGA 引擎完成，释放 CPU
- **报警视频硬件编码**：带检测框的 RGBA → RGA → `hevc_rkmpp` 零拷贝硬编 + 环形缓冲，报警时切出"前 3 秒 + 后 2 秒"裸流片段（CPU 软编 x264 已彻底移出链路）
- **电子围栏**：多边形/矩形围栏绘制，inside/outside 两种侵入判定模式；围栏**独立于级联**——
  复用该通道已有模型的检测结果做几何判定，触发类别是 `geofence.alarmClass` 单值（默认 `person`），
  按 `geofence.throttleSeconds`（默认 10 秒）每通道每类别节流
- **报警管理**：限流去重、抓拍落盘、SQLite 持久化、GUI 悬浮提示与历史查询
- **人员点名**：SCRFD 人脸检测 + 特征比对（进程内 RKNN 常驻推理），注册 / 点名 / 注销三阶段，余弦相似度 + 贪心一对一匹配
- **设备盘点**：复用 YOLO11 RKNN 模型做设备识别与盘点任务管理
- **智能识别**：二维码实时扫描（ZBar，三帧稳定判定）+ 设备铭牌文字识别（PP-OCRv4 det/rec 两阶段，外部 demo 经 QProcess 调用）
- **GB/T 28181 国标推流**：SIP 注册/心跳/目录 + MPEG-PS 复用 + RTP/PT96 直通 H.264 码流，按需启用（无配置段即静默跳过）
- **Pad↔Aibox 对接**：WebSocket 客户端接收远端报警推送并回 ACK，应用层心跳 + 断线自动重连（未配置地址即整条链路缺席）
- **Qt5 GUI**：blacksoft 换肤框架，无边框窗体，多页面导航
- **多路视频支持**：支持 4 路视频同时监控
- **模型灵活配置**：支持 YOLO11 nano/small/medium 不同大小模型与级联多模型

# 四、技术架构

## 核心技术栈

| 技术 | 用途 |
|------|------|
| C++17 | 主要开发语言 |
| Qt5 | GUI 框架 |
| RKNN Runtime (rknpu2) | Rockchip NPU 推理引擎 |
| FFmpeg (rkmpp) | 视频硬件解码 (h264/hevc_rkmpp) 与硬件编码 (hevc/h264_rkmpp) |
| RGA | 硬件 2D 图像加速（色彩转换 + 缩放） |
| OpenGL ES 2.0 / EGL | GPU 零拷贝视频渲染 |
| SQLite3 | 检测库 (idge.db) + 业务库 (roll_call.db) |
| Eigen | 人脸特征批量相似度矩阵计算 |
| CMake 3.10+ | 构建系统 |

## 视频处理全流程

### 输入格式
视频源可以是本地 `.mp4` 文件或 RTSP 摄像头流，通过 FFmpeg 的 `avformat` 打开，由 Rockchip MPP
后端（VPU）硬解输出 DRM_PRIME(NV12) DMA-BUF。
⚠️ **硬解器名目前写死优先 `h264_rkmpp`**（不按流的 `codec_id` 选路），所以 **H.264 源是唯一已验证路径**；
4 路摄像头请统一投 H.264 基线，H.265 源要等"按 codec_id 选硬解器"改造后才能用（第八节已知问题第 7 条，P6 首项）。

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

带检测框的 RGBA8888 DMA-BUF fd
    ↓  RGA rgba_to_nv12（fd → fd，零拷贝）+ DRM_PRIME 池帧
    ↓  hevc_rkmpp 硬编码（VPU 硬件编码，CBR / GOP 1s / bframes=0）
Annex-B H.265（或 h264_rkmpp → H.264）AVPacket —— **同一份包两路消费，不重复编码**
    ├──→ 环形缓冲区（默认 5s，drop-front）
    │        ↓  报警 +2s 到点后由 ClipWriter 后台线程取"前 3s + 后 2s"窗口（起点对齐 IDR）
    │    .h265 裸流（上传/国标）+ 同名 .mp4（网页副本）→ 路径回填 alarms.video_path
    └──→ 上传服务（P4，服务器 URL 预留接口）
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
| 反向色彩转换 | RGBA DMA-BUF (带框) | NV12 DMA-BUF | RGA 硬件 | `RgaConverter::rgba_to_nv12()` |
| 硬编码 | NV12 DMA-BUF | H.265/H.264 Annex-B 包 | VPU (MPP) | `src/media/reader/video_recorder.cpp` |
| 片段封装 | 环形缓冲窗口包 | `.h265` 裸流 + `.mp4` 副本 | CPU (仅 memcpy/remux) | `VideoRecorder::dumpAnnexB/dumpMp4`、`src/media/reader/` ClipWriter |

## 业务模块调用链

```
人员点名：摄像头预览(src/media/reader/camera_preview_decoder) → 抓拍 JPEG
    → SCRFD 检测 + 五点仿射对齐 + 512 维特征提取(进程内常驻
      src/ai/recognition/in_process_face_recognizer)
    → 注册/比对/注销(src/biz/service/roll_call_service)
    → 余弦相似度(归一化点积) + 贪心一对一匹配 → roll_call.db

设备盘点：拍照 → 进程内 YOLO11 识别(src/ai/yolo11，权重=model/library/yolo11n-coco)
    → 结果去重汇总 → src/biz/service/equipment_inventory_service → roll_call.db 任务/设备表

智能识别：对话框打开时新建 FFmpegVideoDecoder(InferenceMode::PreviewOnly——只解码取帧，
    不 buildCascadeTasks，一个 YOLO/RKNN 模型都不加载) 拉 smartRecognition.camera(缺省自动探测摄像头节点)
    → 二维码：captureCurrentFramePtr 取帧 → RGBA→灰度 → ZBar 扫描 → 三帧内容一致才判定
      （模态对话框期间才有工作线程，关窗即释放）
    → 铭牌 OCR：整幅拍照图 → 3rdparty/ppocrv4/bin/rknn_ppocr_det_demo 出文本框
      → 按行聚成选区供用户点选 → 裁图 → rknn_ppocr_system_demo 出行文本

国标推流：main() 判据(Gb28181Config::load 能否凑出 platform + streams) → 命中才 start()
    → Gb28181Manager 单 UDP socket 事件循环（REGISTER/Digest、Keepalive、MANSCDP 查询）
    → 平台 INVITE → 每路一个 Gb28181MediaSource 独立取流(不共用主链路帧)
      源本身是 H.264 → h264_mp4toannexb 直通；否则 rkmpp 硬解(退软解) + h264_rkmpp 重编(退 libx264)
    → PsMuxer(PS 打包，等齐 SPS+PPS+IDR 才出帧) → RtpSender(PT96) → 平台

Pad↔Aibox：frmMain 构造 → WsClient::start() → QWebSocket 连 websocket.server_url
    → 收 type=alarm → frmMain::onWsAlarmReceived → 调试页日志 + sendAck（不入 idge.db）
    → 应用层 ping 心跳 + 断开后定时重连
```

---

# 五、目录结构与模块文档索引

**每个源码目录都有 README.md 详述功能、数据流、使用方法与注意事项**，下表为速查索引。

### 功能域总览（依赖方向 base ← media ← ai ← biz ← ui）

| 域 | 模块 |
|------|------|
| `src/base/` 基础设施 | buffer、queue、threadpool、config、utils、runtime_paths.h |
| `src/media/` 视频管线 | reader、rga、model(ModelPool) |
| `src/ai/` 推理与模型应用 | yolo11、model_repo、recognition、task |
| `src/biz/` 业务域 | alarm、geofence、service、db(报警库+业务库)、sip(GB28181)、ws(Pad↔Aibox) |
| `src/ui/` 界面层 | form、widgets(原 src/ui)、core_helper、core_qss |

### 模块速查

| 目录 | 职责 | 文档 |
|------|------|------|
| `src/main.cpp` | 应用入口（CLI/GUI 双模式、EGL 初始化、data/ 旧布局迁移） | — |
| `src/base/runtime_paths.h` | 运行时数据路径唯一事实源（data/ = **当前工作目录**相对，非源码根） | [README](src/base/README.md) |
| `src/ui/form/` | 主窗口与全部页面/对话框（视频、看板、报警、点名、盘点、智能识别、设置） | [README](src/ui/form/README.md) |
| `src/ui/widgets/` | EGL/OpenGL ES 零拷贝渲染、视频控件、看板、报警列表 | [README](src/ui/widgets/README.md) |
| `src/ui/core_helper/` | 无边框窗体、图标字体、QSS 换肤组件 | [README](src/ui/core_helper/README.md) |
| `src/ui/core_qss/` | blacksoft 皮肤资源 | [README](src/ui/core_qss/README.md) |
| `src/media/reader/` | FFmpeg 解码线程、硬编环形录像(VideoRecorder/ClipWriter)、摄像头预览解码、SCRFD 人脸检测 | [README](src/media/reader/README.md) |
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
| `src/biz/sip/` | GB/T 28181 设备侧：SIP 注册心跳、MANSCDP、PS 复用、RTP 推流 | [README](src/biz/sip/README.md) |
| `src/biz/ws/` | Pad↔Aibox WebSocket 客户端：报警推送、心跳重连（预留 get_streams/set_fence） | [README](src/biz/ws/README.md) |
| `src/base/config/` | ConfigManager 单例 + data/config.json 解析 | [README](src/base/config/README.md) |
| `src/base/buffer/` | DMA-BUF 分配与帧缓冲池 | [README](src/base/buffer/README.md) |
| `src/base/queue/` | 阻塞队列 / 帧队列 / 优先级队列 | [README](src/base/queue/README.md) |
| `src/base/threadpool/` | 通用线程池 | [README](src/base/threadpool/README.md) |
| `src/base/utils/` | 图像/绘制/路径/日志等通用工具 | [README](src/base/utils/README.md) |
| `tools/face_recognition/` | 人脸识别命令行工具（CLI 验证/点名对照回归基准，产物输出 model/face/） | [README](tools/face_recognition/README.md) |
| `model/` | 模型库 `library/<id>/`（yolo11n/s/m、ppocrv4-ocr 收编）+ 专项标签 | [README](model/README.md) |
| `data/` | 运行时数据（config/idge.db/alarms/backups/roll_call_data，内容不入库） | — |
| `python/` | ONNX→RKNN 转换与验证脚本 | [README](python/README.md) |
| `tests/` | 测试程序（test_database 数据库层；test_equipment 设备盘点、test_rollcall 人员点名全链离线自测；test_latest_frame_slot 帧广播槽跳帧/生命周期，均随主构建生成） | [README](tests/README.md) |
| `docs/` | 技术知识库 | [README](docs/README.md) |
| `include/` | nlohmann/json 单头文件 | [README](include/README.md) |
| `res/` | Qt 资源（qrc：图片/字体/GL 着色器/音效） | [README](res/README.md) |
| `sounds/` | 提示音文件 | [README](sounds/README.md) |
| `3rdparty/` | 第三方库；`zbar/`（二维码）与 `ppocrv4/`（OCR demo）的**二进制产物不入库**，新克隆需按其 README 手工还原 | [README](3rdparty/README.md) |

运行时数据统一在 **当前工作目录下**的 `data/`（内容已 gitignore，不入库）：`data/config.json`、`data/idge.db*`、`data/alarms/`（按日抓拍）、`data/backups/`、`data/roll_call_data/`。因而是"每个启动位置一套"——`run.sh` 启动的那份在 `build/build_rk3588_linux/data/`。旧布局（散落在根目录）启动时由 `RuntimePaths::migrateLegacy()` 自动迁入。构建目录 `build*/`、`install/` 同样不入库。

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

# 六、模型信息

- **模型格式**：RKNN (Rockchip Neural Network)，转换方法见 [python/README.md](python/README.md)
- **检测模型**：YOLO11 nano/small/medium（COCO 80 类），输入 640×640（LetterBox 预处理）
- **人脸模型**：SCRFD 检测 + 特征识别，放置于 `$IDGE_WORKSPACE/model/face/`
- **OCR 模型**：PP-OCRv4 det/rec 两件套（`model/library/ppocrv4-ocr/` 的 `det.rknn`/`rec.rknn`/`labels.txt`），
  由 `3rdparty/ppocrv4/bin/` 的两个 demo 进程加载，主程序只传图路径、解析 stdout
- **量化支持**：uint8 / int8 / float32
- **NPU 核心**：可指定 RK3588 的 3 个 NPU 核心（`detect.threads` 配置）

# 七、第三方库版本

明细见 [3rdparty/README.md](3rdparty/README.md)。摘要：FFmpeg(rkmpp) 6.1.1、MPP 1.3.10、RKNN Runtime、librga 1.10.0、OpenCV、Qt5 5.15.18、jsoncpp 1.9.7、libjpeg-turbo、libyuv、libsndfile、OpenSSL、ZeroMQ、FFTW、stb、OpenCL stub、DRM 分配头文件、sqlite3、ZBar（二维码，头文件与 `zbar.pc` 入库、`libzbar.so` 不入库）、PP-OCRv4 RKNN demo（第三方，见 `3rdparty/ppocrv4/`）。

# 八、已知问题与技术债

> 需求缺口与实施顺序见 `plan/requirements.md` / `plan/roadmap.md`；
> 本节只列**代码级**仍开的问题，均已（或应在）源码注释中警示，细节另见各目录 README 的"注意事项"。

1. `DmaBufferPool::acquire` 返回裸指针，调用方须在池销毁前回借出；池析构会强制回收未归还项
   并打印警告（悬垂风险仍在，靠约定规避）。`DmaFrameBuffer` 浅拷贝双释放已用 `= delete` 堵死
2. **JSON 不能加注释**：`QJsonDocument::fromJson` 遇到 `//` 直接返回 null，而解析失败走的是
   "warning + 全默认值"分支——写注释不是启动报错，而是**静默丢掉整份现场配置**。且 `save()`
   是整文件重写、按键名字母序输出，任何排版都留不住。要放说明只能用数据键
   （`config_example.json` 里的 `_readme` 字符串数组即可原样往返）
3. `PriorityQueue::waitAndPop` 名为 Pop 实为 Top（只读堆顶不出队），且 `push` 不 notify 挂起的
   消费者。现网只用 `tryPop`，`waitAndPop` 零调用方——列在 roadmap §六 清理项，不是待修 bug
4. `src/ui/form/photo_selection_dialog.h`、`src/ai/task/helmet_task.*`/`base_task.*`/`task_pool.h`、
   `src/ui/form/video_decoder.*`、`src/base/queue/frame_queue.h`、`src/media/model/ModelPool.hpp`、
   `src/base/threadpool/ThreadPool.hpp` 都在 GLOB 里被编译但无实例化点（清单见 roadmap §六）
5. `detect.conf_threshold` / `detect.nms_threshold` 是**假开关**：写进 JSON、设置页可改，
   但 `ppe_task.cpp` 调 `model_->detect()` 不传参，实际生效的是编译期 `BOX_THRESH/NMS_THRESH`
6. `YOLO11Model::output_mems[9]` 固定长度，换多于 9 个输出头的模型（分割/姿态）会越界
7. **GB28181 MANSCDP 编码声明与实际不符**：XML 头写 `encoding="GB2312"`，实际发出的是 UTF-8 字节
   且 `Content-Length` 也按 UTF-8 长度算（`gb28181_manager.cpp` 心跳/应答处）。宽容平台无感，
   严格平台在中文 `DeviceName` 上会解析错乱。真要对接，改一处即需全模块统一口径
8. **GB28181 配置写错只表现为"不启用"**：`serverHost`/`advertiseHost`/SDP `c=` 必须是 IPv4 字面量，
   `inet_pton` 失败即静默 `return` 且无日志；`platform.transport` 是假开关（值不影响发包）。
   排障先看 `data/config.json` 的 `gb28181` 段，别等错误提示
9. **本机摄像头无互斥**：`smartRecognition.camera`（二维码/OCR 拍照 + 人员点名/设备盘点预览）、
   `video.channels[].source`、`gb28181.streams[].source` 三条取流路径各自 `avformat_open_input`，
   同一设备被两路同时打开时后开的那路失败，报错只落在各自日志里
10. **二维码扫描无去重/冷却**：`QRScanWorker` 每 100ms 采样、连续 3 帧内容一致即判定稳定，
    结果对话框关闭后若码仍在镜头前，约 0.8 秒会再次弹出同一结果
11. **换模型/改通道模型仍需重启**：推理任务（`PpeTask` + 结果队列 + 类名表 + 报警类别）只在解码器
    首次 `start()` 时按 `video.channels[ch].models` 建立，此后配置改动不影响在跑的任务；
    `PpeTask::reloadModel()` 保留了唯一安全的换模型实现但全工程无调用方（roadmap P1-2）；
    模型测试按钮会短时独占一次 NPU 上下文（GUI 阻塞秒级）
12. **`wss://` 放弃证书校验**：`WsClient::onSslErrors` 无条件 `ignoreSslErrors()`，只适合内网调试；
    WS 发送也不排队，断线期间的 `ack` 直接丢弃（详见 `plan/websocket.md`）
13. **NPU 负载按"各通道模型数之和"计**：4 路 × 每路 3 个模型 = 12 个推理任务打在 3 个核上，
    配置不做硬拦截；任务数超过 6 时启动日志打软告警，核掩码按进程级轮转分配（跨通道共享一份
    模型上下文服务多路，仍是未做的优化）

> 已闭环、不再在此罗列的历史项：解码线程与 `player_widget` 关闭（现 `closeEvent` 两阶段并行停 4 路）、
> 硬解按 `codecpar->codec_id` 选 `h264_rkmpp`/`hevc_rkmpp`、
> 围栏报警改为独立通道级判定 + `geofence.throttleSeconds` 节流（原先传 `bypassThrottle=true` 每帧一条，
> 40 秒能出三百多条，并把同通道的普通检测报警整个吞掉）、
> 围栏触发类别改为 `geofence.alarmClass` 单值（V1 的 `alarmClasses` 列表已废弃）、
> 报警类别从槽位属性迁到模型属性、配置里不再出现模型/标签路径（绝对↔相对路径拉锯问题连根消失）、
> 时间戳量纲注释已统一为 epoch 纳秒、`FrameQueue::pushAndReplace` 顺序、`dma_mutex` 跨 TU 单例、
> `QSqlDatabase` 每线程独立连接、NPU 核掩码位标志修正、级联每槽位独立标签与类别数。
> DMA/RGA/命令注入/ns-ms 口径等 2026-09-24 批次的修复明细原载 `plan/update_log.md`（已删，
> 结论都沉淀在上文源码注释与各目录 README 里）。

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
