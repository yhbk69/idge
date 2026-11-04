# tests

## 功能概述

C++ 测试源码目录。含十四个测试程序与一个人工联调工具：

- `test_database.cpp`：针对 `src/biz/db/` 的检测记录 DAO / 报警记录 DAO /
  DatabaseManager 做初始化、插入、批量插入、条件查询、统计、状态更新与维护
  （vacuum/过期清理）的黑盒断言测试，输出 `[PASS]/[FAIL]` 与通过/失败计数。
- `test_equipment.cpp`：设备盘点全链离线自测（无人值守板端验收），串起
  RollCallService 初始化(共库) → 盘点服务进程内模型加载(YOLO11Model) → 建任务 →
  processPhotos 检测计数 → 画框图落盘 → saveResult 落库 → getPhotos/getDetections
  回读一致性 → 热更新原子性（合法清单换模型 / 非法清单失败保持旧模型可检测）。
- `test_rollcall.cpp`：人员点名全链离线自测，先做**对照回归**（同一张 test.jpg
  经外部 `face_recognition` exe 与进程内 `InProcessFaceRecognizer` 推理，断言人脸数
  一致、按 IoU 同名配对后特征余弦 > 0.99——SCRFD/ArcFace 移植语义不变性守门），
  再走业务链：注册 processPhotos+saveTaskResult → 回读一致性 → 双图全局去重 →
  matchCancellation 一对一全命中 → confirmCancellation 落库回读。
- `test_latest_frame_slot.cpp`：`src/base/queue/latest_frame_slot.h` 的四条契约自检——
  代感知跳帧（慢消费者一次拿到最新帧、不重复消费同一代）、被持有输入不被提前回收、
  `close()` 能唤醒阻塞中的等待者并让它返回空、`reset()` 后不留旧游标残留。
  纯 STL + std::async，不依赖 Qt 与硬件，可在任意机器上跑。
- `test_channel_reload.cpp`：通道配置热更新的板端离线自测。驱动**真** `FFmpegVideoDecoder`
  （无 GUI）跑 `assets/test/1.mp4`，做 4 轮"两阶段停止 → `releaseTasks()` → 按新配置重建"，
  模型集合依次是 1 个 → 2 个 → 换 1 个 → 换回 2 个。断言三件事：`waitForStop()` 后
  `isLoopAlive()` 必须为 false；重建出的槽位数与模型路径与配置一一对上（解析
  `cascade slot ch …` 日志，生产代码没有 `tasks_` 访问器也不该有）；反复重开后线程数
  回到基线、RSS 增长有界。另外用 `updateSlotParams()` 验证在线换槽位运行期参数（报警类别 +
  阈值）的两条性质：形状相符才接受改写（不符必须拒绝，设置页据此分流"在线换 / 整路重开"），
  且写入一个不可能命中的类别名之后**仍要继续出帧**——这一步既确认没重连流，也确认那把
  `slotParamMtx_` 没被用成长临界区把解码循环锁死；选不存在的类别名是为了整轮不产报警、
  不碰报警库。**每槽生效阈值**由 `slot params ch N slot k conf X nms Y` 这行独立前缀的日志断言：
  挂了 `conf_threshold` 覆盖的模型必须是覆盖值、其余槽位必须是全局值——回落链的两条分支
  一次跑完。⚠ 新字段**不能**并进 `cascade slot ch …` 那一行：测试用 `find_last_of(' ')`
  从行尾取模型路径，尾巴上多一个 token 会把路径解析坏。
  这条测试已经抓到过一个真回归：`start()` 里没有把
  对象线程归属搬回 GUI 线程，第二次 `moveToThread` 被 Qt 拒绝，重开的那路一帧都不出。
- `test_video_sources.cpp`：视频流注册表（`video.sources[]`）与通道引用解析的**离线契约自检**（5 组 12 条）。
  只编 `ConfigManager.cpp`、纯 `Qt5::Core`、不碰 FFmpeg/模型/屏幕，配置读写全在 `QDir::tempPath()`
  下的临时文件（不会碰运行时 `data/config.json`）。钉的是那些**改代码时最容易顺手改错**的口径：
  三层兜底（引用命中带全部十个参数 / 内联兜底 `type` 必须留空走嗅探 / 两者都空不开流）、
  **悬空 `source_id` 不回退内联**、`setChannelSource()` 必须同时清 `source_id`（否则手改地址是假开关）、
  `setChannelSourceId()` 不得抹掉内联地址、`setVideoSource()` 拒空 id/空 url 且把重复 id 收口成一条、
  `removeVideoSource()` 连带清引用、`videoSourceReferrerCount()` 计数、`type=mp4` 归一成 `file`、
  setter→`save()`→重新 `load()` 的十字段往返一致、以及迁移**只补空的 `sources` 段**且幂等
  （现场改过的流不会被启动流程改回默认）。出厂默认那条也钉住：新配置就带 `file-demo` 且四路引用它。
- `test_stream_params.cpp`：`video.sources` 那四个**打开参数**的板端实测与回归（6 组，无 GUI）。
  驱动**真** `FFmpegVideoDecoder`，用 `InferenceMode::PreviewOnly`（一个 RKNN 模型都不加载、
  不建 DMA 池、不画框），只验"参数到底落没落到 FFmpeg 上"。单独做成一个测试的理由是这四个参数的
  失效方向**全是静默的**：`loop` 不生效=演示片播完黑屏且无错误、`timeoutMs` 不生效=界面永久停在
  "正在接入…"、`reconnectMs` 不生效=摄像头拔插后这一路死掉只能重启、`hardwareDecode=false` 不生效=
  软解档形同虚设。六组分别是 `loop=false` 单遍收尾 / `loop=true` EOF 回卷 / 黑洞地址
  （`rtsp://192.0.2.1:554/live`，TEST-NET-1 不可路由）在 ≈`timeoutMs` 内失败返回 /
  失败开流按 `reconnectMs` 重试且 `signalStop` 能在一个分片内脱身 / 软解档出帧并与硬解档并排打出
  速率与 RSS（**这两个数只记录不断言**，素材只有 3 秒，不足以支撑成本结论）/
  只给 `url` 的内联旧口径照样出帧（三层兜底没被注册表改坏）。
  ⚠ **cwd 必须是构建目录**（`assets/` 相对路径与 `librockchip_mpp` 的 rpath 都按那儿算）；
  缺 3 秒素材会 SKIP 1/2/5 三组并打印生成命令。2026-11-03 实测值记在
  `plan/代码审查_2026-11-02_协议对齐idge-pad-v1.md` §十-1。
- `test_gb28181_source_ref.cpp`：国标流**引用**视频流注册表的离线自检（9 组，无 FFmpeg）。
  `gb28181.streams[]` 现在只写国标身份 + `source_id`，取流字段全在 `video.sources[]`，于是新增三类
  "只在运行时才暴露"的错，三种表现必须不同：**忘写** `source_id`、**指向不存在的 id**、
  以及**进程跑起来之后注册表被删**——第一种在 `load()` 丢、第二种也在 `load()` 丢（各自一行不同原因），
  第三种要等 INVITE，`takeSource()` 返回空 cfg 让媒体层报错。钉的内容还包括：
  旧取流键（`type`/`source`/`width`/`height`/`fps`/`loop`/`cameraIndex`）**写了也被忽略**（两套口径并存
  正是这次要消灭的东西）、`type` 为空时按 url 前缀补型（国标侧靠它选 v4l2 参数 / rtsp 强制 tcp /
  `file` 才看 `loop`）、注册表把 `mp4` 归一成 `file`、全部被丢弃时 `load()` 返回 false 且 error
  带丢弃条数、`videoSourceReferrerCount()` 把国标引用一起数（删一条正被平台点的流要有人拦）、
  以及顶层 `platform`/`streams` 老布局走同一套引用判据。
  ⚠ 测试配置里 `platform.localHost` 必须给非通配值：`advertiseHost` 缺省取它，留空就是 `0.0.0.0`，
  会被"通配地址平台回不了话"那条判据整段拒掉，测的就不是引用而是 IPv4 校验了。
- `test_alarm_uplink.cpp`：报警上行（`src/biz/ws/alarm_uplink_engine`）的**离线契约自检**，
  用 `FakeTransport` 顶掉 `WsClient`，所以不链 WebSockets、不建线程、不要 GUI，纯 `Qt5::Core`。
  十八组断言覆盖四类事：①`alarm_info` 报文形状（两种 `media_transport` 各一套字段）；
  ②`ws_binary` 档的 `media_begin`/分块/`media_end` 帧序与游标；③`url` 档只发地址——
  `transfer:"http"` + `url`、**不产出 `media_end`**、`mediaUrlFor()` 的路径逃逸（根外 / 含 `..`
  必须返回空串）与尾斜杠拼接，以及"相对 `imagePath` 必须先按 cwd 绝对化再换算"（这条抓到过板端
  真 404：`QDir(root).relativeFilePath(相对路径)` 是按 root 解析的，不绝对化就得到双重前缀）；
  ④队列语义——水位闸门、上限丢最旧、`uplink_ack` 只对账队首、退避梯子 `min(2^n×2s, 30s)`、
  断线时帧不越序、`media_failed` 的各成因。
  **其中 `ws_binary` 那 11 组是"代码保留但默认关"的回归保护，改协议时不许顺手删。**
- `test_geofence_role.cpp`：电子围栏**按形状角色**判定的离线契约自检（`FenceChecker::checkDetection`
  返回 `FenceViolation`）。八组断言：禁区命中优先于离岗（一人一帧一条）、**通道无岗区域时永不出
  `Absence`**（少了这条守卫，纯禁区通道会把每个人每帧都判离岗）、人在任一岗区域内即不算离岗、
  离岗的 `shapeIndex` 恒为 -1、多禁区时命中下标正确、禁用/空形状出 `None`、凹多边形命中与迁移前
  逐位一致（守 letterbox + 射线法，同时证明多边形没被降级成 `toRect`）、
  `roleFromString` 对缺键/空串/垃圾一律回落 `forbidden`（迁移的安全失效方向）。
  只链 `Qt5::Core`、无 GUI、与 cwd 无关，任意机器可跑。
- `test_construction_qr.cpp`：扫码登录门的**全部契约**离线自检（12 组）。施工信息的
  解析/校验/会话状态机（`ConstructionInfo::fromJson`/`fromQrText` + `ConstructionSession`）
  加上真解一张码图：用 `segno` 生成含中文的 PNG → `qrcode::scanQrFromImageFile()` → 喂给
  与登录页完全同一条 `startLogin()` 路径。另钉住四条易塌的口径：失败**不动**会话（不存在半登录）、
  `toJson()` 往返保留未知键（字段没定稿，加字段不许吃掉多余键）、图片读不到时给**原因**而不是
  返回空列表（空列表与"没有码"同形，会把路径错判成码印坏）、
  `fields()` 逐键渲染的形状（已知键按声明顺序给中文名、空值成行显示 `—`、未知键用键名排在后面
  —— 报警详情框与施工详情框共用这一份，渲染口径散成两处就等于字段契约也有两处）。
  只链 `Qt5::Core` + `Qt5::Gui` + zbar，不开摄像头、不起 GUI 线程。
- `test_pad_wire.cpp`：Pad 接口 v1 **出站 JSON 映射**的离线契约自检（16 组，含 `deviceId` 派生）。
  `AlarmRecord` → `alarm` / `alarm_update` 的唯一出口（`src/biz/pad/pad_wire.cpp`）：
  时区偏移补齐与游标反向换算的往返一致（对端按 `updatedAt` 合并与推进游标，偏移缺失=整批漏同步）、
  `rectified` 映射成 `PENDING+readAt` 而不是 `DISPOSED`（"我看过了"不许上报成"我处理过了"）、
  `taskId` 只取 `construction_info.construction_id`（取错键对端按施工计划过滤一条都不显示）、
  类型表（`un_helmet→HELMET/未戴安全帽/PPE违规`、`fence_in→INTRUSION`、`fence_out→CROSS`、
  **裸 `fence` 老行按前缀归 `INTRUSION`**，报告 P2-4：老库里那两万多行不设这条就会被推成 `PERSON`）
  与认不出时的兜底（抬大写 code + 原名 + `其他`，绝不给空标签——v1 规定 Pad 不做本地映射）；
  **合规类 `helmet`/`vest`/`workWear` 不在表里**（报告 P2-2：把它们标成违规标签，设置页一改就产出
  方向相反的假报警），走兜底等 A6 取值表；
  证据 URL 三条：只放 1 张、`.mp4` 还没落盘就不写 `clipUrl`、**root 之外的路径一律返回空数组**
  （那台文件服务零鉴权，拼一个越界地址等于本机任意文件可读）、没配底址时同样不给地址；
  信封：`alarm`/`sync_result`/`pong` 一律带 `code=0`+`message=ok`（§3.2，P3-1——`pong` 空 `data`
  也不豁免，`data` 键本身不出现）。
  只链 `Qt5::Core` + `pad_wire.cpp` + `alarm_uplink_engine.cpp`（借 `mediaUrlFor` 那份逃逸判据），
  不起服务端、不开数据库、与 cwd 无关，任意机器可跑。
- `test_pad_sync.cpp`：Pad 接口 v1 **断线补漏的库侧**自检（9 组，内存库）。
  `queryUpdatedSince()` 的游标语义：空游标=全量且按**生效更新时间升序**、老行 `update_time` 为空时
  按 `alarm_time` 参与过滤与排序（少了这条，"重连后历史一条都不回来"且完全不报错）、
  边界用 `>=`（同一秒内的更新不许被漏）、建行那一刻就写 `update_time` 才能被增量看到。
  `markRead()` 的两条语义：`read_time` 保留最早值（重复已读不推后、墙钟回拨取更早者），
  但 `update_time` **每次都往前走**。第 9 组钉计划过滤**下推进 SQL**（报告 P1-2 的活锁）：
  库里混有别的施工计划的 40 行时，按 `constructionId` 取到的只有本计划的行，且 `limit` 是
  **过滤之后**才截断——截在前面会让本计划的行永远被别的计划挤在窗口外、增量游标原地不动。
  判据用 `instr(construction_info, '"construction_id":"…"')`，因为 Qt 自带的 SQLite 3.31.1
  **没有 JSON1**（板端实测 0 个 `json` 符号），不能用 `json_extract`。
  只链 `Qt5::Core` + `Qt5::Sql` + DAO 依赖闭包。
- `test_pad_ws_server.cpp`：Pad 接口 v1 的 **WS 服务端线上自检**（16 组，loopback 单进程）。
  起真 `PadWsServer` 在 127.0.0.1 的临时口上，用真 `QWebSocket` 按 §3 走一遍：
  `hello→hello_ack`（`serverTime` 带偏移、`capabilities` 只有 `alarm`、`detectionBoxes` 恒 false）、
  版本不匹配 `code=1001` 且连接被关闭、`ping→pong` 信封带 `code=0/message=ok`、每个 `sync_request`
  **恰好一个** `sync_result` 且 `alarms` 是数组而不是缺键（缺键时 Pad 保留旧缓存，看起来"补漏成功"其实没有）、
  未知 `type` 回 `error 2001`（`code/message` 在信封层、`data.detail` 带上那个 type）、
  `hello` 缺必填项同样 2001 且**不关连接**、`detail` 要点名缺的是哪个键（1001 与 2001 两个判据不能混）、
  `sync_request` 空 `taskId` 回 2001（口径 A19，静默不回是活锁）、
  `alarm_ack` **不回复**（用"紧随其后的 ping 的 pong 是 ack 之后的第一帧"来证明中间没插话）、
  `AlarmManager::storeAndNotify` → 连接上收到完整 `alarm` 快照、
  `acknowledgeAlarmById`/`markAsFalsePositiveById` → 之后**必须收到一帧 `alarm_update`**（第 16 组，
  报告 P1-1：这个分支以前是死代码）、`stop()` 后还能再 `start()`
  （与 ZLM 那种进程级一次、停了不能再起明确不同）。
  第 15 组测 **Origin 白名单**：`http://` Origin 连不上、空 Origin 能连，且被拒的连接不进 `clients_`
  ——取基线前要把前序连接排空（`disconnected` → `clients_.removeAll` 等一轮事件循环，
  立刻数会把上一组的收尾时序当成这一组的行为）。链 `Qt5::Core/Network/Sql/WebSockets`，无 GUI。
- `ws_sink.cpp`：**不是自检，是人工对拍的接收端**（`QWebSocketServer`）。板子出网受限，联调只能走
  loopback：`./ws_sink 8765 /tmp/sink_out` 起一个假对端，把 `websocket.server_url` 指到
  `ws://127.0.0.1:8765`。`ws_binary` 档它按 `media_id` 把二进制分块拼回文件并与报文里的 sha256 对拍；
  `url` 档只打印 `media_begin` 给出的地址/字节数/哈希、一个字节都不落盘——那种模式下"上传成功"的
  判据是拿这条 URL `curl` 下来的字节与盘上文件 `sha256sum` 一致。两种档都会把 `alarm_info`、
  `media_failed`、`ack` 原样打成一行行日志供人读。

## 文件/子目录清单

| 文件 | 说明 |
|------|------|
| `test_database.cpp` | 数据库模块测试（4 组用例：初始化/检测/报警/维护），基于 Qt Core + 宏 `TEST_ASSERT`；报警组覆盖检索层（`AlarmFilter` 计数与取行同口径、分页、scope 不重不漏、围栏角色、负责人/施工id 过滤、三类 facet 候选值、`acknowledgeAllPending`） |
| `test_equipment.cpp` | 设备盘点全链离线自测（模型加载→检测计数→画框→落库→回读→热更新原子性），逐项 `[ok]/[FAIL]` |
| `test_rollcall.cpp` | 人员点名全链离线自测（exe 对照回归 + 注册/去重/注销匹配/落库回读），逐项 `[ok]/[FAIL]` |
| `test_latest_frame_slot.cpp` | LatestFrameSlot 单槽广播契约自检（跳帧/生命周期/close 唤醒/reset），末行打印一行 PASS 说明 |
| `test_channel_reload.cpp` | 通道热更新板端离线自测（停→releaseTasks→重建 ×4 轮，槽位集合/线程数/RSS/每槽生效阈值四项断言），须在 build 目录下跑 |
| `test_alarm_uplink.cpp` | 报警上行离线自检（18 组：两种 transport 报文形状 / 帧序 / 地址换算与路径逃逸 / 队列·ack·退避），纯 Qt Core，任意机器可跑 |
| `test_geofence_role.cpp` | 围栏角色判定离线自检（8 组：禁区优先 / 无岗区域守卫 / 任一岗区域内 / shapeIndex 口径 / 凹多边形回归 / role 回落），纯 Qt Core，任意机器可跑 |
| `test_construction_qr.cpp` | 扫码登录门离线自检（12 组：施工信息解析/校验/会话状态机/逐键渲染 + 真解一张含中文的码图），Qt Core+Gui+zbar，无摄像头无 GUI |
| `test_pad_wire.cpp` | Pad 接口 v1 出站 JSON 映射离线自检（16 组：时区往返 / 状态三值 / readAt 回落 / taskId / 类型表与兜底 / 必填键 / 证据 URL 与越界拒绝 / 信封无 v 无 requestId / deviceId 派生非空且稳定），纯 Qt Core |
| `test_pad_sync.cpp` | Pad 断线补漏的库侧自检（9 组：全量与增量游标 / 老行按 alarm_time 兜底 / `>=` 边界 / 新行可见 / markRead 保留最早已读但必推 update_time / 计划过滤下推进 SQL 且 limit 在过滤之后），Qt Core+Sql，内存库 |
| `test_pad_ws_server.cpp` | Pad WS 服务端线上自检（16 组：握手与能力位 / 1001 关闭 / pong 带 code / sync_result 恰好一条 / 未知 type 与缺必填项与空 taskId 各出 2001 / alarm_ack 不回复 / 报警广播 / 状态变更推 alarm_update / Origin 白名单 / 空 device_id 也出必填 deviceId），Qt Core+Network+Sql+WebSockets，loopback |
| `test_video_sources.cpp` | 视频流注册表与通道引用解析离线自检（5 组：默认种子 / 三层兜底与悬空不回退 / 内联与引用互斥 / 注册表写入规则 / 存盘往返与迁移幂等），纯 Qt Core，临时文件不碰 `data/config.json` |
| `test_stream_params.cpp` | `video.sources` 四个静默打开参数的板端实测（6 组：loop / EOF 回卷 / timeoutMs / reconnectMs+分片停止 / 软解档出帧 / 内联旧口径兜底），须在构建目录下跑，缺 3 秒素材 SKIP 三组 |
| `test_gb28181_source_ref.cpp` | 国标流引用 `video.sources[]` 的离线自检（9 组：三类丢弃各给一行原因 / 全丢时 load 判 false / `takeSource()` 取注册表 / 旧取流键被忽略 / 空 type 按 url 前缀补型 / mp4→file / 运行中删流得空 cfg / 引用计数含国标 / 顶层老布局同判据），Qt Core+Network，无 FFmpeg |
| `ws_sink.cpp` | 人工联调用的假对端（`QWebSocketServer`）：收 text+binary、按 media_id 拼文件对拍 sha256，url 档只打印地址 |

## 使用方法

十四个测试与 `ws_sink` 都已接入根 `CMakeLists.txt`（各自一个 `add_executable`，随主程序一并构建）：

```bash
./build-linux.sh -t rk3588 -b Release
./build/build_rk3588_linux/test_database    # 退出码：失败数>0 时为 1，否则 0
./build/build_rk3588_linux/test_latest_frame_slot   # 同上；纯 CPU，不依赖板子外设
./build/build_rk3588_linux/test_alarm_uplink        # 同上；纯 Qt Core，不依赖外设与 cwd

# 视频流注册表自检只链 ConfigManager.cpp，改解析口径（三层兜底/悬空/内联互斥）时先跑它
cmake --build build/build_rk3588_linux --target test_video_sources -j"$(nproc)"
./build/build_rk3588_linux/test_video_sources       # 无 GUI、与 cwd 无关；配置读写在 /tmp 下的临时文件

# 国标引用注册表的自检：只编 ConfigManager.cpp + gb28181_config.cpp（后者用 QHostAddress，故链 Network）
# 改 gb28181_config.cpp 的丢弃判据或 takeSource() 的补型规则时跑它
cmake --build build/build_rk3588_linux --target test_gb28181_source_ref -j"$(nproc)"
./build/build_rk3588_linux/test_gb28181_source_ref  # 无 GUI、与 cwd 无关；测试配置写在 /tmp，不碰 data/config.json

# 流参数实测/回归：源列表与 test_channel_reload 同一套（ffmpeg_video_decoder 的依赖闭包），
# 跑之前必须 cd 到构建目录（assets/ 相对路径与 librockchip_mpp 的 rpath 都按那儿算）。
# 1/2/5 组要一段 3 秒素材，缺了 SKIP 并打印生成命令：
#   ffmpeg -y -v error -ss 0 -t 3 -i assets/test/1.mp4 -c copy /tmp/idge_probe_3s.mp4
cmake --build build/build_rk3588_linux --target test_stream_params -j"$(nproc)"
cd build/build_rk3588_linux && ./test_stream_params [3秒素材] [黑洞地址]   # 默认 /tmp/idge_probe_3s.mp4 + rtsp://192.0.2.1:554/live

# Pad 接口 v1（src/biz/pad）三连：映射层 → 库侧游标 → 服务端线上。改协议字段时按顺序跑，
# 前两个不需要网络，第三个会在 127.0.0.1:18899 上起真服务端
./build/build_rk3588_linux/test_pad_wire            # 纯 Qt Core，与 cwd 无关
./build/build_rk3588_linux/test_pad_sync            # 内存库，无 config.json 依赖
./build/build_rk3588_linux/test_pad_ws_server       # loopback WS，无 GUI

# 围栏判定自检只链 fence_checker.cpp，改判定逻辑时优先跑它（目标级构建，不整树重编）
cmake --build build/build_rk3588_linux --target test_geofence_role -j"$(nproc)"
./build/build_rk3588_linux/test_geofence_role       # 同上；无 GUI、与 cwd 无关

# 登录门自检：解一张真码 + 校验/会话状态机/逐键渲染。第 10 组要一张码图，缺了只 SKIP 那一组并打出命令：
#   /home/ubuntu/miniconda3/bin/python -m segno -o /tmp/idge_test_qr.png --scale 8 --border 4 \
#     '{"worker_name":"张三","worker_phone":"13800001111","worker_id":"GZ-0042",
#       "work_content":"隧道道床修补","construction_id":"SG-2026-11"}'
cmake --build build/build_rk3588_linux --target test_construction_qr -j"$(nproc)"
./build/build_rk3588_linux/test_construction_qr [码图路径...]   # 无 GUI、与 cwd 无关；路径含 "bad" 的按坏码处理

# 盘点/点名自测必须在仓库根运行（依赖 assets/test/ + model/library/yolo11n-coco +
# model/face/ 两件套权重；点名对照还需 model/face/face_recognition）；
# 直接跑主程序同目录产物即可（test_equipment 内部已 qputenv offscreen）
cd "$(git rev-parse --show-toplevel)"
./build/build_rk3588_linux/test_equipment   # 末行 PASS + rc=0 / FAIL + rc=1
./build/build_rk3588_linux/test_rollcall    # 同上
./build/build_rk3588_linux/test_channel_reload  # 同上；需 NPU/RGA，工作目录必须是构建目录（./model 软链）
```

`ws_sink` 不参与断言、要人工看输出，所以单独一段。板端端到端联调的最小姿势（两端都在这块板子上，走 loopback）：

```bash
./build/build_rk3588_linux/ws_sink 8765 /tmp/sink_out &   # 假对端：先起来
# data/config.json: websocket.server_url = ws://127.0.0.1:8765
#                   alarm_uplink.enabled = true, media_transport = "url"
./run.sh                                                 # 触发一次报警，看 sink 的日志行
curl -o /tmp/x.jpg '<sink 打出的 snapshot/media_begin url>' && sha256sum /tmp/x.jpg
```

`url` 档的"上传成功"判据就是最后这条 curl：取回的字节 sha256 必须与 `media_begin` 里申报的
`sha256` 与 `data/alarms/` 下同名文件三者一致。`ws_binary` 档则由 sink 自己拼文件并比哈希。

主程序用 `./run.sh` 启动（它会 `cd` 进构建目录，所以"启动目录=工作区根"；构建目录下
的 `model`、`3rdparty` 两个软链由 CMake 在 configure 时创建，测试也按同样方式跑）。
若 Qt 装在非系统路径，需要自行 `export LD_LIBRARY_PATH=<Qt>/lib:$LD_LIBRARY_PATH`
——本仓库不再假设某个固定版本号或某台机器的绝对路径。

`test_database` 使用内存数据库（`DatabaseManager::instance().initialize(":memory:")`），不会写磁盘文件；
内存库没有文件可 stat，所以 `databaseSize()` 必然返回 0 —— 那条断言写的就是 `== 0`，
早先写 `> 0` 是测试自己的历史 bug（在内存库上永远失败）。
`test_database` 的源文件是**显式列在 CMakeLists 里**的（不像主目标那样 GLOB），
`alarm_dao` 一旦引用新模块就会链接期报 undefined reference —— 它现在额外链
`src/biz/session/construction_session.cpp`（`constructionFacets` 要用 `ConstructionInfo::kKey*`）。
`test_equipment` 的 db 与任务目录全部落在 `/tmp/equip_check_<pid>/`、`test_rollcall` 落在
`/tmp/rollcall_check_<pid>/`（含 test.jpg 副本，隔离 exe 的 `_faces.json` 残留），跑完即删，不碰正式 `data/roll_call_data`。

## 依赖关系

- Qt5 Core / Qt5 Sql、`3rdparty/sqlite`、`3rdparty/jsoncpp`
- `src/biz/db/`（database_manager、detection_dao、alarm_dao）
- `test_equipment` 另链：盘点/点名服务全套（`src/biz/service`、`src/biz/db/business_db_manager`、
  `src/base/imaging`、`src/ai/recognition`、`src/ai/model_repo`、`src/base/config`）+
  rknnrt / OpenCV / libturbojpeg / RGA（CMake 里用 `-Wl,--exclude-libs,ALL`
  阻止静态 turbojpeg 的 `jpeg_*` 符号劫持系统 OpenCV imgcodecs 的 libjpeg ABI）
- `test_rollcall` 另链：点名服务 + 两套识别器（`face_recognizer` 对照用 exe 链路与
  结构体、`in_process_face_recognizer` 被测），其余同 `test_equipment`
- `test_latest_frame_slot` 只链 `Threads::Threads`，include 路径仅 `src/base/queue`
- `test_alarm_uplink` 只链 `Qt5::Core`，源 = `alarm_uplink_engine.cpp` + `media_payload.cpp` 两个文件
  （**故意不链** `ws_client`/`alarm_manager`/DAO/`media_pump`：前者是 QObject + 真 socket，
  后者的成品回调要投回 GUI 线程，都留给板端端到端验，不进离线自检）
- `test_geofence_role` 只链 `Qt5::Core`，源 = `fence_checker.cpp` 一个文件：它只依赖
  `fence_shape.h` 与 `common.hpp` 里的 `image_rect_t` 字段，不碰 OpenCV/RKNN 符号，
  所以围栏判定改动能在不链主程序的前提下验证
- `test_video_sources` 只链 `Qt5::Core`，源 = `ConfigManager.cpp` 一个文件（`runtime_paths.h`
  是头文件唯一事实源，不用额外编译）。它跑的是纯 JSON 读写，所以与 cwd 无关、任何机器可跑，
  也**不会**碰运行时的 `data/config.json`（临时文件名前缀 `idge_video_sources_`）
- `test_gb28181_source_ref` 源 = `ConfigManager.cpp` + `gb28181_config.cpp` 两个文件，链
  `Qt5::Core` + `Qt5::Network`（后者只为一个 `QHostAddress` 的 IPv4 校验），**不链 FFmpeg**：
  引用判据与补型规则全在 JSON 层，媒体层怎么开流由 `test_stream_params` 与板端联调负责。
  临时文件前缀 `idge_gb_ref_`
- `test_construction_qr` 链 `Qt5::Core` + `Qt5::Gui` + `${ZBAR_LIBRARIES}`，源 =
  `construction_session.cpp` + `qr_decoder.cpp` 两个文件：解码器刻意只用 `QImage`/`QRect`
  （不拖 OpenCV/RKNN 头），这是它能脱离摄像头与 GUI 线程自测的前提
- `ws_sink` 链 `Qt5::Core` + `Qt5::WebSockets`，`QCoreApplication` 无 GUI 依赖
- `test_channel_reload` 链的是主流水线的依赖闭包：`ffmpeg_video_decoder` + `video_recorder` +
  `ppe_task`/`base_task` + `model_registry` + `ConfigManager`/`parse_config` +
  DMA 缓冲 + `alarm_manager`/`alarm_dao`/`detection_dao`/`database_manager` + 围栏两件 +
  `construction_session.cpp`（`makeAlarmRecord()` 给每条报警盖施工信息戳，于是
  `alarm_manager.cpp` 多了一个 TU 依赖——手写源列表漏它就是三个 undefined reference，
  而链接错误比编译错误更难归因）+
  `src/base/imaging`，库侧与主目标同一套（FFmpeg-rkmpp / rknnrt / rga / sqlite3 / jsoncpp / OpenCV /
  GLES/EGL），并同样带 `ZERO_COPY` 与 `--allow-shlib-undefined`；库搜索目录要单独
  `target_link_directories` 一遍——主目标那份是 target 级的，不会传给别的 target
- `test_stream_params` 与 `test_channel_reload` **同一套源列表与依赖闭包**（也带 `ZERO_COPY`）：
  它驱动的是同一个 `FFmpegVideoDecoder`，只是用 `InferenceMode::PreviewOnly` 把模型加载与 DMA 池绕开。
  两份源列表是手抄的，**给 `test_channel_reload` 加 TU 时另一份要一起加**，否则新加的测试
  在别人机器上以链接错误的形式出现

## 注意事项

- **新增测试源必须同时在 `.gitignore` 加 `!tests/xxx.cpp`**：根 `.gitignore` 里有
  `test_*.cpp` 这条"临时验证文件"规则，会把正式测试源一并挡掉，而根 CMakeLists 已经
  引用它 —— 结果是本地能编、别人克隆后 `No such file` 编不过。十四个测试都已放行。
  **`ws_sink.cpp` 不在 `test_*.cpp` 的射程里（文件名不以 `test_` 开头），不需要豁免**——
  当初 `test_alarm_uplink.cpp` 就是这么被静默挡住过一轮，`git status` 里根本没有它。
- 断言基于 `src/biz/db/` 接口行为（如"批量插入10条"后 `queryByTimeRange` 期望 ≥11 条），
  改动 DAO 语义时需同步核对用例假设。
- 测试进程为单线程顺序执行，`testsPassed/testsFailed` 为全局计数，多线程化时需加锁。
- 注意与 `src/biz/db/business_db_manager`（业务库 roll_call.db）区分：本测试覆盖的是
  `src/biz/db/`（检测/报警库 idge.db）。
