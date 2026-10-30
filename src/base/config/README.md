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
  `save()`；JSON 解析失败置空 `root_`，getter 回退默认值（0.25 / 0.45 / "person" / 10 …，
  遗留的 `"inside_alarm"` 只在迁移分支里出现）。
- **一次性迁移**：两类，都在 `load()` 内、有变化才 `saveUnsafe()` 落盘：
  1. `migrateModelPaths()` / `migrateRecordingDefaults()` 等段级补齐——只在整段不存在时写默认值，
     目的是让新开关在文件里可见可改，程序本身仍靠 getter 兜底；
  2. `migrateSchemaV2()`——把老 V1 结构（`model.path` + 全局 5 槽 `cascade.models[]` +
     `alarm.classes` + `video.channel1..4`/`notes`）一次性改写成 V2，键名与语义见根 README §3。
     守卫是 `video.channels` 是否存在：迁移后旧键全部 `remove`，第二次启动即永久跳过
     （用 `md5sum` 对比可验证幂等）。它跑在 `migrateModelPaths()` **之后**——必须先归一化老路径
     才能反查库 id；且**只允许用纯字符串**的 `pathToLibraryId()` 反查，不得回调 `ModelRegistry`
     （`mutex_` 不可重入，回调即启动死锁）。
- **围栏形状的角色迁移不在这里**：`geofence` 段 `channels/*/shapes[]` 缺 `role` 时由
  `FenceManager::loadFromConfig()` 自己按老 `mode` 补齐并落盘。`migrateSchemaV2()` 是一次性闸门、
  板端已跑过，且不触碰 shapes；为角色重新武装它会误伤现场已画好的围栏。
- **ConfigParser**（CLI 用）：解析 `-m/-i/-a/-t/-c/-d/-r/-s/-p/-v/-h` 等选项填充 `AppConfig`，
  缺 `-m`/`-i` 或文件不存在即报错 `exit`。

### video 段（schema V2：按通道级联 + 模型参数）

| 键 | 类型 | 说明 |
| --- | --- | --- |
| `models.<库目录名>.alarm_class` | string | **模型的参数**：该模型检出这一类才出报警；空 = 只画框不报警。不是通道或槽位属性，同一个模型挂在哪一路触发的都是同一个类别。设置页改动对引用它的通道即时生效（见 `frmMain::applyAlarmClassNow`） |
| `models.<库目录名>.label` | string | 标签文件覆盖，库外标签的逃生口；留空用库内 `labels.txt`（优先级：此值 > 库内） |
| `models.<库目录名>.note` | string | 备注，仅供设置页展示 |
| `channels[i]` | object | **数组下标即通道号（0-based）**；条目里的 `channel` 字段只给人看、不参与解析 |
| `channels[i].source` | string | 本地文件 / `/dev/videoN` / RTSP，交给 `avformat_open_input` |
| `channels[i].enabled` | bool | 这一路是否开流。设置页改完即时生效（停用=停本路，启用=按 `source` 重开），手改文件需重启或让该路重开一次 |
| `channels[i].models` | array | 有序库 id 列表，**顺序即级联槽位顺序**（画面上 M1/M2/… 前缀与报警 `result_id`），重排会改号。设置页改动即时生效：该路 stop→`releaseTasks()`→start 重开一次 |

对应 API：`channelSource/Note/Enabled/ModelIds(int ch)`（一律 0-based）+
`modelParam/libId` 系列 + `modelParamUsers(id)`（供删除守卫统计引用）。
**段里不出现任何模型/标签路径**，`model/library/<id>/{model.rknn,labels.txt,meta.json}` 是唯一事实源，
解析出口只有 `ModelRegistry::resolveChannelSlots(ch)`。
**推理间隔没有配置键**：`ffmpeg_video_decoder.cpp` 的 4.6 节写死 `frame_count % 3`，
别在 `video` 下加 `render.sampleInterval` 之类的键——那只会造出一个改了不生效的假开关。

### geofence 段（开关/类别通道级、角色**形状级**、独立于级联）

| 键 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `enabled` | bool | `true` | 总开关；有围栏数据时 `FenceManager::loadFromConfig` 会自动置 true。**通道级**——混用两种角色的通道无法只关其中一种 |
| `mode` | string | `inside_alarm` | **已退役**：运行期无任何判断读它。唯一读者是 `FenceManager::loadFromConfig` 的迁移分支，用来给没有 `role` 键的老形状补角色；`saveToConfig` 固定写 `"inside_alarm"` 只为不让键消失。`geofenceMode()`/`setGeofenceMode()` 因此是遗留 API，**别再拿它做判断** |
| `alarmClass` | string | `person` | 触发围栏报警的**单一类别**（旧列表 `alarmClasses` 已废弃）。产不出这个类名的通道，禁区和岗区域两条规则**都**静默失效 |
| `throttleSeconds` | int | `10` | 围栏报警**节流下限**：每通道**每角色**每类别窗口内只出一条（节流键含角色段，闯禁区后 10 秒内的离岗不会被吞），由 `AlarmManager::ingestFenceAlarm` 执行。"同一个人站进去只报一次"不靠它，靠解码器的进入沿事件表（见 `src/biz/geofence/README.md`） |
| `channels.<ch>` | object | — | 围栏形状 + `overlay_w/h`（顶点是 overlay 像素坐标）。每个形状带 `role`：`forbidden` 禁区（进去就报）/ `workzone` 岗区域（待在外面就报）；**缺键时按 `mode` 推断、并立即落盘补齐**（幂等），解析不出来的值一律回落 `forbidden`。角色创建后不可改，改角色 = 删了重画 |

围栏不占推理任务：它复用该通道已有模型产出的检测结果，只做几何判定。若某通道挂了围栏却
没有任何模型能产出 `alarmClass`，启动日志打 `[geofence] 通道N 无模型产出类别 person，围栏不会触发`。

### recording 段（报警视频硬件编码录制）
| 键 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `enabled` | bool | **`true`** | 出厂即开：报警必带关联片段是需求验收项（`plan/requirements.md` §三①）。关掉会让 `alarm_uplink.send_clip` 自动降级为不发片段 |
| `codec` | string | `hevc` | `hevc`→`hevc_rkmpp`（主用，带宽减半）/ `h264`→`h264_rkmpp`；其余值一律按 `hevc` 处理 |
| `buffer_seconds` | int | `8` | 环形缓冲区时长（1~60 秒），决定能回溯多久。缺键与启动补齐都是 **8**：报警窗口"前3+后2"共 5 秒，但缓冲区按**包数**算（`buffer_seconds` × 流报帧率），入包节拍被 pacer 限到 ≤30fps，`gop_seconds=1` 的关键帧对齐最坏再吃掉前向 1 秒——5 秒是零余量卡边。**⚠ 当前 `config_example.json` 模板里写的是 5**，照模板装的新板会拿到 5（模板值优先于缺键默认）；代码注释认定 5 太紧，这个不一致本轮只记录不改值 |
| `bitrate` | int | `2000000` | CBR 码率 bps（20 万~1600 万） |
| `gop_seconds` | int | `1` | 关键帧间隔（1~10 秒），越小报警片段起点对齐误差越小 |

片段**没有目录配置项**：裸流与网页副本固定落在 `RuntimePaths::alarmsDir()/yyyyMMdd`，与抓拍图
同一棵树，好处是报警媒体只有一套按天保留 + 容量回收逻辑（见 `src/biz/alarm/README.md`）。

### alarm_uplink 段（本机报警上行）
| 键 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `enabled` | bool | `false` | 总开关。**默认关的理由与 `recording` 相反**：这是往外发数据，且链路明文零鉴权，装完就自动推等于默认把数据送上网，必须人来打开。false 时 `AlarmUplink` 不订阅信号、不建线程、零分配 |
| `send_snapshot` | bool | `true` | 第一段 `alarm_info` 是否申报抓拍图 |
| `send_clip` | bool | `true` | 第二段是否申报片段；`recording.enabled=false` 时自动降级为不发，并打一条 warning |
| `media_transport` | string | `url` | `url`=本机内嵌 http 文件服务、JSON 只发地址（**默认**）；`ws_binary`=媒体字节走 WS 二进制分块帧。getter 只归一化成这两个值；`ws_binary` 档下 `mk_*` 一次都不调、不监听端口 |
| `media_http_port` | int | `8090` | `url` 档的服务口，`qBound(1024, _, 65535)`。取 8090 而非 80：板端 uid 1000 + `net.ipv4.ip_unprivileged_port_start=1024`，80/443 bind 不上。**改口必须重启进程**（ZLM 运行时进程级一次） |
| `media_base_url` | string | 空 | 地址前缀，空 = 自动（第一个非回环 IPv4 + 实际绑定端口）；多网卡猜错、对端经 NAT/域名访问时在这里写死 |
| `chunk_size` | int | `262144` | 仅 `ws_binary` 档有效：单个二进制帧大小，对端 `maxFrameSize` 更小时调这里 |
| `ack_timeout_ms` | int | `15000` | 仅 `expect_ack=true` 有意义：等 `uplink_ack` 的超时 |
| `max_attempts` | int | `3` | 同一项最多重投几次，超过判失败并 warning |
| `max_pending` | int | `16` | 待发队列上限，满了丢最旧（与 `ClipWriter`/`SnapWriter` 同口径） |
| `expect_ack` | bool | `false` | 对端当前不回 `uplink_ack`；打开后发完帧要留在队首等确认才算完成 |

**本段与 `recording` 段的迁移口径都是「只补缺失段、不覆盖已有段」**（`migrateAlarmUplinkDefaults()`
`ConfigManager.cpp:580`）：所以现场那台已经跑过一次的 `config.json` 里，后加的
`media_transport`/`media_http_port`/`media_base_url` 三个键**永远不会自动出现**，只能靠上面这些
getter 的默认值兜住。要显式改必须手改 JSON 并重启——报警上行、录制、文件服务都没有设置页控件。

### smartRecognition 段（智能识别取流）
| 键 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `camera` | string | 探测摄像头节点（MJPEG 优先，其次 NV12/YUYV），探测不到取 `/dev/video0` | 二维码实时扫描与 OCR 拍照、人员点名/设备盘点预览共用的取流地址 |

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
| `server_url` | string | 空 | **只填 `ws://`** 地址（`wss://` 在本机不可用，Qt 的 OpenSSL 符号缺口未适配，详见 `src/biz/ws/README.md`）；空 = 整条链路不启动。**有 setter**（`setWsServerUrl`），调试设置页的 WebSocket 分组可直接编辑 |
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

// schema V2：按通道 + 按模型（ch 一律 0-based）。本类只负责存值，
// "改完要不要重开该路"是设置页的事（见 README §八 第 10 条）
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
