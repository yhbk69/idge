# src/base/config

> 所属域：**base 基础设施域**（目录重组 S4a 迁入，依赖方向 base ← media/ai/biz/ui）

## 功能概述
配置模块。分两部分：
- **ConfigManager**（Qt/JSON）：`data/config.json` 的统一读写入口（路径常量见 `src/base/runtime_paths.h`），供 GUI/检测/报警/围栏共享。
- **ConfigParser**（CLI）：基于 `getopt_long` 的命令行参数解析，产出 `AppConfig`。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `ConfigManager.h/.cpp` | 单例，整份 config.json 载入内存 `root_`，分节 getter/setter + `save()` |
| `parse_config.hpp/.cpp` | `ConfigParser::parse_arguments` 解析命令行 → `AppConfig` |
| `SharedTypes.hpp` | 共享枚举/常量（`NPU_CORE_NUM`、加速/输入/引擎枚举）与 `AppConfig` |

## 核心类与数据流
- **ConfigManager**（单例）：`load()` 读入 `QJsonObject root_`；各 setter 只改内存，
  `save()` 才整体序列化写回文件。配置节：`video`（含 `models` 参数表与 `channels` 列表）/
  `detect` / `geofence` / `recording` / `database` / `websocket` / `smartRecognition`
  （另有一整段 `gb28181` 由国标模块自己读，见下）。文件不存在时按 **schema V2** 写入内置默认并
  `save()`；JSON 解析失败置空 `root_`，getter 回退默认值（0.25 / 0.45 / "inside_alarm" / "person" / 10 …）。
- **一次性迁移**：两类，都在 `load()` 内、有变化才 `saveUnsafe()` 落盘：
  1. `migrateModelPaths()` / `migrateRecordingDefaults()` 等段级补齐——只在整段不存在时写默认值，
     目的是让新开关在文件里可见可改，程序本身仍靠 getter 兜底；
  2. `migrateSchemaV2()`——把老 V1 结构（`model.path` + 全局 5 槽 `cascade.models[]` +
     `alarm.classes` + `video.channel1..4`/`notes`）一次性改写成 V2，键名与语义见根 README §3。
     守卫是 `video.channels` 是否存在：迁移后旧键全部 `remove`，第二次启动即永久跳过
     （用 `md5sum` 对比可验证幂等）。它跑在 `migrateModelPaths()` **之后**——必须先归一化老路径
     才能反查库 id；且**只允许用纯字符串**的 `pathToLibraryId()` 反查，不得回调 `ModelRegistry`
     （`mutex_` 不可重入，回调即启动死锁）。
- **ConfigParser**（CLI 用）：解析 `-m/-i/-a/-t/-c/-d/-r/-s/-p/-v/-h` 等选项填充 `AppConfig`，
  缺 `-m`/`-i` 或文件不存在即报错 `exit`。

### video 段（schema V2：按通道级联 + 模型参数）

| 键 | 类型 | 说明 |
| --- | --- | --- |
| `models.<库目录名>.alarm_class` | string | **模型的参数**：该模型检出这一类才出报警；空 = 只画框不报警。不是通道或槽位属性，同一个模型挂在哪一路触发的都是同一个类别 |
| `models.<库目录名>.label` | string | 标签文件覆盖，库外标签的逃生口；留空用库内 `labels.txt`（优先级：此值 > 库内） |
| `models.<库目录名>.note` | string | 备注，仅供设置页展示 |
| `channels[i]` | object | **数组下标即通道号（0-based）**；条目里的 `channel` 字段只给人看、不参与解析 |
| `channels[i].source` | string | 本地文件 / `/dev/videoN` / RTSP，交给 `avformat_open_input` |
| `channels[i].enabled` | bool | 只影响启动时是否打开这一路（改完需重启，不做运行期起停） |
| `channels[i].models` | array | 有序库 id 列表，**顺序即级联槽位顺序**（画面上 M1/M2/… 前缀与报警 `result_id`），重排会改号 |

对应 API：`channelSource/Note/Enabled/ModelIds(int ch)`（一律 0-based）+
`modelParam/libId` 系列 + `modelParamUsers(id)`（供删除守卫统计引用）。
**段里不出现任何模型/标签路径**，`model/library/<id>/{model.rknn,labels.txt,meta.json}` 是唯一事实源，
解析出口只有 `ModelRegistry::resolveChannelSlots(ch)`。
**推理间隔没有配置键**：`ffmpeg_video_decoder.cpp` 的 4.6 节写死 `frame_count % 3`，
别在 `video` 下加 `render.sampleInterval` 之类的键——那只会造出一个改了不生效的假开关。

### geofence 段（通道级、独立于级联）

| 键 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `enabled` | bool | `true` | 总开关；有围栏数据时 `FenceManager::loadFromConfig` 会自动置 true |
| `mode` | string | `inside_alarm` | `inside_alarm` / `outside_alarm` |
| `alarmClass` | string | `person` | 触发围栏报警的**单一类别**（旧列表 `alarmClasses` 已废弃） |
| `throttleSeconds` | int | `10` | 围栏报警**节流下限**：每通道每类别窗口内只出一条，由 `AlarmManager::ingestFenceAlarm` 执行。"同一个人站进去只报一次"不靠它，靠解码器的进入沿事件表（见 `src/biz/geofence/README.md`） |
| `channels.<ch>` | object | — | 围栏形状 + `overlay_w/h`（顶点是 overlay 像素坐标） |

围栏不占推理任务：它复用该通道已有模型产出的检测结果，只做几何判定。若某通道挂了围栏却
没有任何模型能产出 `alarmClass`，启动日志打 `[geofence] 通道N 无模型产出类别 person，围栏不会触发`。

### recording 段（报警视频硬件编码录制）
| 键 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `enabled` | bool | `false` | 常驻硬编要占 VPU，必须显式打开；关闭时不分配任何硬件资源 |
| `codec` | string | `hevc` | `hevc`→`hevc_rkmpp`（主用，带宽减半）/ `h264`→`h264_rkmpp`；其余值一律按 `hevc` 处理 |
| `buffer_seconds` | int | `5` | 环形缓冲区时长（1~60 秒），决定能回溯多久 |
| `bitrate` | int | `2000000` | CBR 码率 bps（20 万~1600 万） |
| `gop_seconds` | int | `1` | 关键帧间隔（1~10 秒），越小报警片段起点对齐误差越小 |

片段**没有目录配置项**：裸流与网页副本固定落在 `RuntimePaths::alarmsDir()/yyyyMMdd`，与抓拍图
同一棵树，好处是报警媒体只有一套按天保留 + 容量回收逻辑（见 `src/biz/alarm/README.md`）。

### smartRecognition 段（智能识别取流）
| 键 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `camera` | string | 探测摄像头节点（MJPEG 优先，其次 NV12/YUYV），探测不到取 `/dev/video0` | 二维码/OCR 拍照与人员点名/设备盘点预览共用的取流地址 |

只有 getter（`smartRecognitionCamera()`），没有 setter：该页每次打开对话框才读一次，
运行中改内存值没有意义。值直接交给 `FFmpegVideoDecoder::start()` /
`CameraPreviewDecoder::start()`，经 `avformat_open_input` 自动探测格式，所以本机设备节点、
本地视频文件、RTSP 地址都能填（后两种主要给没有摄像头的机器自测用；点名/盘点的
`CameraPreviewDecoder` 只走 v4l2，填文件或 RTSP 打不开）。设备节点依赖 `main()` 里
`initializeMediaRuntime()` 注册的 libavdevice v4l2 输入源。

键缺省（或填了空串）时不再退回写死的编号，而是遍历 `/dev/video*` 按编号升序实探测：
先找支持 MJPEG 的节点（USB UVC 摄像头），一个都没有再退而取支持 NV12/YUYV 的节点
（RK3588 的 CSI 摄像头经 `rkisp_mainpath` 出流，本板上是 `/dev/video22`，它**不提供
MJPEG**，只认 MJPEG 会把这颗物理摄像头整个漏掉）。只出 10-bit Bayer 的 `rkcif` 节点
两档都不命中，因此不会被选中。节点号随插拔、USB 口、ISP/CSI 接线和内核版本漂移，
板端曾稳定落在 `/dev/video41`，换环境即打不开——这就是自动探测取代硬编码的原因。

**与其它取流地址互不相干**：监控 4 路走 `video.channels[].source`、国标推流走
`gb28181.streams[].source`。各处没有设备注册表，同一台设备被两处同时打开会有一方失败。

### websocket 段（Pad↔Aibox 客户端）
| 键 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `server_url` | string | 空 | `ws://`/`wss://` 地址；空 = 整条链路不启动。**有 setter**（`setWsServerUrl`），调试设置页的 WebSocket 分组可直接编辑 |
| `heartbeat_interval` | int（秒） | 30 | 应用层 `{"type":"ping"}` 周期；界面无控件，只能改 JSON |
| `reconnect_interval` | int（秒） | 5 | 非主动断开后的重连延时；界面无控件，只能改 JSON |

### 不由 ConfigManager 管的段
`data/config.json` 里还有段落是模块自己直接读文件的，`ConfigManager` 不提供 getter/setter，
`save()` 也不会替它们兜底——但 **`save()` 是整文件截断覆写 `root_`**，而 `root_` 是载入时的
整份 JSON，所以只要进程没在别处删键，这些段会被原样写回：

| 段 | 读取方 | 说明 |
| --- | --- | --- |
| `gb28181` | `src/biz/sip/gb28181_config.cpp` 自己 `QJsonDocument::fromJson` | 国标推流的平台参数与通道列表，见 `src/biz/sip/README.md`；`main()` 用它做"要不要启动 manager"的判据 |

数据流：
```
GUI/启动 → ConfigManager::load() → 各模块 getter 读取 → setter 改内存 → save() 落盘
PlayerWidget::open() → recording*() 组装 RecorderConfig → FFmpegVideoDecoder::setRecordingConfig()
CLI 模式 → ConfigParser::parse_arguments(argc,argv) → AppConfig → 检测线程
```

## 使用方法
```cpp
ConfigManager::instance().load("config.json");
double conf = ConfigManager::instance().confThreshold();   // 默认 0.25
ConfigManager::instance().setConfThreshold(0.3);           // 仅改内存
ConfigManager::instance().save();                          // 显式落盘

// 围栏整段由 geofence::FenceManager 经 setGeofenceChannels()+save() 落盘

// schema V2：按通道 + 按模型（ch 一律 0-based；改完需重启才生效）
ConfigManager::instance().setChannelSource(0, "rtsp://192.168.1.10/stream1");
ConfigManager::instance().setChannelModelIds(0, {"yolo11n-ppe", "yolo11n-coco"}); // 顺序即槽位
ConfigManager::instance().setModelAlarmClass("yolo11n-ppe", "un_helmet");         // 模型参数
QString cls = ConfigManager::instance().modelAlarmClass("yolo11n-coco");           // 无条目返回 ""

// 报警录制：只有 getter（见下方"无 setter"说明）
RecorderConfig rc;
rc.enabled = ConfigManager::instance().recordingEnabled();
rc.codec = ConfigManager::instance().recordingCodec();
```
CLI：`./app -m model/yolo11n.rknn -i video.mp4 -t 3 -c true`

## 依赖关系
- Qt：`QFile`/`QJsonDocument`/`QJsonObject`/`QJsonArray`（ConfigManager）。
- `getopt.h`/标准库（ConfigParser）。
- 被 `AlarmManager`、`FenceManager`、检测/模型初始化等广泛引用。

## 注意事项
- **内存态 vs 落盘**：setter 只改 `root_`，不调 `save()` 则重启丢失；但 getter 读同一内存，
  改完立刻可见——"看着生效了其实没存盘"是常见坑。
- **写盘时机与风险**：`save()` 是整文件截断覆写（Indented），非原子；进程在写入中崩溃会
  损坏配置；`save()` 打开文件失败被静默忽略（仅 `load` 失败 qWarning），须保证路径可写。
- **线程安全**：公开接口内部由 `mutex_` 加锁，可跨线程调用；`saveUnsafe()` 仅供已持锁的
  内部路径复用，外部直接调用会与 `save()` 双重加锁死锁（std::mutex 不可重入）。
- **缺省值兜底**：getter 用带默认值的 `toXxx(default)`，缺键不崩、返回兜底；文件损坏时
  `root_` 置空后全走默认，会静默丢弃用户原有配置。
- **通道号统一 0-based**：`video.channels` 的数组下标、`channelSource/ch` 系列形参、
  `geofence.channels` 的键、解码器 `channel_`、UI `openVideo(0..3)` 全是同一个口径。
  1-based 的遗留键 `video.channel1..4` / `video.notes[N-1]` 由 `migrateSchemaV2()` 消掉，
  跨模块传值不再需要换算。
- **假开关要写清楚**：类别数属于**每个模型**（`labels.txt` 行数），推理线程数 = 各通道模型数之和
  （一个模型一个任务线程），所以 `detect.class_num`/`detect.threads` 的 getter 已删除、
  默认配置块也不再写入；老配置里残留的这两个键程序根本不读（`migrateSchemaV2` 只删
  `model`/`cascade`/`alarm`/`video.channelN`/`video.notes`，不清理 `detect` 段内的多余键）。
  `detect.conf_threshold`/`nms_threshold` 仍有 getter/setter，但是**假开关**：写进 JSON、
  设置页可改，推理侧不传参，实际用编译期 `BOX_THRESH`/`NMS_THRESH`。
  同理 `video.render.sampleInterval` **不存在**，推理间隔是 `ffmpeg_video_decoder.cpp` 里
  写死的 `frame_count % 3`。
- **recording 段刻意只有 getter、没有 setter**：编码器在解码线程打开流时
  按参数一次性创建，运行期改值不会重建它，提供 setter 只会造出
  "改了不生效"的假接口；要换参数请改文件后重启。
- **recording 段的键全部可选**：缺键由 getter 兜底，`codec` 只认 `h264`，其它值（含拼错）一律
  归一到 `hevc`；数值键在 getter 内 `qBound`，写出格子的值不会被透传到编码器。
