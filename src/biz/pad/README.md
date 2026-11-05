# src/biz/pad

> 所属域：**biz 业务域**（本轮新增）

## 功能概述
Pad 接口 v1（`plan/edge-pad-v1.md`）里**本机作为服务端**的那一面：Pad 连进来，本机推报警、
回心跳、给断线补漏的快照。这是与 `src/biz/ws/`（本机主动连出去的旧客户端 + 旧上行）
方向完全相反的一条链路，两者**不是新旧两档、不能并存**（见审查报告冲突 B1）。

本轮落了 **WS 报警面（v1 §3）+ `/api/` HTTP 面（§4 报警处置、§5 围栏、§6 实时视频、§7 设备/健康、§9 媒体地址口径）**。
仍然没有实现的是 §8 App 升级与 §11 清点/施工计划/销点——那两节 2026-11-03 定案**先不做**（A1 关闭）。
端口分配 A4 与 `taskId` 来源 B2 已在 2026-11-04 定案并落地（见下面第 2 条与配置段一节）。
机制已就位的部分：出站报警 JSON 只有一个出口、游标查询只有一条 SQL、
入站 HTTP 只有一张路由表、幂等只有一个配置段。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `pad_wire.h` / `.cpp` | `AlarmRecord` → v1 `alarm`/`alarm_update` 的**唯一**映射层：时区补齐、状态四值→三值、类型表、证据 URL、信封。纯函数，离线可测 |
| `pad_ws_server.h` / `.cpp` | `QWebSocketServer` 单例：`hello_ack`/`pong`/`error`/`sync_result`/`alarm`/`alarm_update`，连接表与广播 |
| `pad_http_message.h` / `.cpp` | HTTP **传输核**：请求解码（Content-Length/chunked/multipart）、头与正文的字节上限、百分号解码、§2.4 信封与 §2.4 错误码出口。只依赖 QtCore，不起 socket |
| `pad_http_server.h` / `.cpp` | `QTcpServer` 单例：监听、keep-alive 会话、空闲回收、`{param}` 路由表与分发 |
| `pad_request_cache.h` / `.cpp` | §2.5 `requestId` 幂等表（`pad_request_cache`）+ `runIdempotent()` 这条唯一入口 |
| `pad_http_api.h` / `.cpp` | HTTP 面的**装配点**：`HttpApiContext`（deviceId、媒体底址、根目录、凭据开关、通道在线判据）与 `registerPadHttpRoutes()`；通道身份 `channelIdentity()`（streamId/cameraId/liveUrl/协议）也在这 |
| `pad_http_alarms.cpp` | §4 四路由：`/dispose-options`、`/alarms/{id}/dispose`（multipart 收照片）、`/false-alarm`、`/read` |
| `pad_http_fences.cpp` | §5 六路由：`/fences`、`/fences/{id}`、`save-draft`、`apply`、`disable`、`delete-draft`（状态机在 `biz/geofence/pad_fence_store`） |
| `pad_http_streams.cpp` | §6 两路由：`/streams`、`/streams/{streamId}/status` |
| `pad_http_device.cpp` | §7 两路由：`/device/status`（**snake_case**，全规约唯一一处）与 `/health`（能力表照实报） |

## 六条必须先读的口径
1. **`rectified` 不等于 `DISPOSED`，处置是内部第四态 `disposed`**。本机"确认"的语义是
   "我看过了"，读一条报警不改状态，所以 `rectified` 出站仍是 **`PENDING` + `readAt`**；
   `DISPOSED` 只能由 `AlarmDAO::dispose()` 写的 `status='disposed'` 出（§3.9 三值 + §13.3.2
   "处置后状态固定为 DISPOSED"）。2026-11-05 起 `dispose()` 有了调用方：
   `POST /api/alarms/{id}/dispose` → `AlarmManager::disposeAlarmById()`。
   终态保护写在 SQL 里（`WHERE … AND status NOT IN ('disposed','false_alarm')`），
   界面上"全部确认"也不会覆盖已处置的行（`acknowledgeAllPending` 跳过 `disposed`），
   否则一句"全部确认"就会把已处置退成待处置，正是 §3.6 明令禁止的那种回退。
   ⚠ **写状态必须同时改库和内存窗口那份副本**，顺序是「库 → 副本 → 发 `alarmStatusChanged`」：
   关机时 `syncToDatabase()` 会把 RAM 窗口整批回写，只改库的那条会在第二天早上神秘复原。
2. **`taskId` ≡ `construction_id`，同物两名**（B2 于 2026-11-04 定案：对外一律 `taskId`）。
   库里它已从 `construction_info` 的 JSON 子字段升成 `alarms.task_id` 一等列，`taskIdOf()`
   优先读列、列空才回落解 JSON（迁移前的老行 + 内存里还没落库的行）。所有按计划过滤的 SQL
   都走 `task_id = ?` 等值，`instr(construction_info, ...)` 那套文本匹配已经删干净。
   未登录期间产生的报警 `taskId` 是空串（回填收尾把 NULL 写成 `''`）：它不属于任何施工计划，
   对端按计划过滤时看不到它——这是事实而不是缺陷。
   未登录时它同样不该被伪造，所以 `alarmJson()` 里没有"兜底一个 local id"这种写法。
   反过来说，`sync_request` **不许带空 `taskId`**：§13.1.5 把它列为必填，空值一律 `error 2001`（口径 A19）。
   ⚠ 扫码载荷从 2026-11-04 起是 `qr-login-edge-v1.md` 的嵌套形状，计划 ID 在 **`plan.taskId`**；
   `construction_id` 现在只剩历史库里的老行还在用（`taskIdFromJsonText()` 读两代形状）。
   所以"两名之间唯一的转换缝"仍在 `taskIdFromJsonText()`，但它已经是单向的兼容回落。
3. **片段晚到，用 `alarm_update` 把视频补上**（2026-11-05 现场指定的流程）。
   报警当刻先发 `alarm`（信息 + 抓拍图，`clipUrl` 为 `null`）；视频 ≈2 秒后才由 ClipWriter 写完整，
   这时**只发一条 `alarm_update`**，里面带 `clipUrl`，不再重发整条 `alarm`。对端按
   `alarmId + updatedAt` 就地合并（§3.5/§3.6），其余字段不动。
   ⚠ 两个边界：①`clipUrl` 只在能给出地址时才出现，**不发 null**（§13.2.2"未出现的字段表示未提供"，
   发 null 会把对端已拿到的视频清掉）；②`§13.2.2` 的字段表里目前**没有 `clipUrl`**，
   这是本机按现场指令先落的一处补充，规约待对端补上。
4. **`hello_ack` 的两个"必填"键不靠配置**。`deviceId` §13.1.3 规定必填，而 `pad_server.device_id`
   出厂是空串——空串的原因不是没 ID，而是配置文件会被拷到别的板子、写死会撞车。所以配置留空时
   按 `/etc/machine-id` 派生（`pad::defaultDeviceId()`，本机实测 `EDGE-1030A3F99414`），
   **配置填了就用配置的**。`capabilities.alarm.evidenceUrl` 反过来：它据实反映媒体服务在不在，
   服务起不来就报 `false`（口径 A17b："证据不可用 ≠ 协议违规"），不为了凑规约撒谎。
5. **状态变更要立刻变成一条 `alarm_update`**。UI/界面确认、误报驳回都走
   `AlarmManager::alarmStatusChanged(AlarmRecord)` → `PadWsServer::handleAlarmStatusChanged`，
   否则 §13.2.2 的 `alarm_update` 在本机是死代码（审查报告 P1-1）。
   这条消息**带 `syncState`**：本机恒 `SUBMITTED`（已接收待上传）——没有上传环节就不报 `UPLOADED`。
   它同时也带 `dispose.photoUrls`/`personId`（与完整 `alarm` 共用同一个 builder），
   否则别的 Pad 只收到状态变化，处置现场照片永远补不上。
   **例外**：`acknowledgeAll()`（一键全部已读）故意不发信号——一次可能几千条，广播会把 GUI 线程钉住，
   这批增量留给下一次 `sync_request` 补漏。
6. **来源检查与鉴权是两件事**。§2.3 免的是鉴权，不是免来源检查：`originAllowed()` 只放行空 Origin
   与 `android-app://*`，带 `http(s)://` Origin 的握手直接 403（口径 A20）。不判的等价后果是
   "局域网里任何网页的 JS 都能连进来实时收报警"，而报警里带人名、电话、位置与截图地址。
   Qt 5.12.8 的 `QWebSocket` **没有** `requestOrigin()`，唯一入口是
   `QWebSocketServer::originAuthenticationRequired`；板端实测它在 `newConnection` 之前生效，
   被拒的连接不进 `clients_`。

> 有争议的 12 条口径（连同**不同意时只改哪一处**）汇总在
> `plan/口径确认单_2026-11-03.md`，改协议语义前先去看那张表，别在代码里另立口径。

## HTTP 面（`/api/`）再加七条

1. **入站零鉴权，但口令不出站**。§2.3 免鉴权覆盖 HTTP，所以 `/api/` 是"局域网里任何机器都能
   处置报警、下发围栏"的口——这个开关是 `pad_http.enabled`（出厂 true 是因为 §4/§5 不做就等于
   HTTP 面整体没有；要收紧就关掉它）。**但两件事不受它豁免**：① WS 握手仍查 Origin（口径 6）；
   ② `liveUrl` 默认**剥掉** `user:pass`（`pad_http.expose_stream_credentials=false`）——
   摄像头口令被显示、缓存、进对端日志是另一回事，不在"免鉴权收报警"那句的范围里。
   剥完对端多半播不起来，那是**可见的失败**，比口令出门好。
2. **幂等键只有 `requestId`，不比较内容**（§2.5 原话）。板端实测：同一个 `requestId` 打到
   **另一个** `alarmId`，返回的是首次那份响应——这是规约要求的行为，不是本机漏了校验。
   命中表里连 4xx/5xx 也存（"返回第一次的结果"，错误也是结果），保留 ≥24h，启动时 `prune()`。
3. **错误码分档**（§2.4 那张表 + 冲突⑤的解法）：字段缺失/类型错 → `400/2001`；
   值或数量不合规（照片 >5 张、单张 >5MB、`measure` 空串）→ `400/4001`；
   围栏形状/点数/坐标/`regionId` 业务校验 → `422/4001`。同一 HTTP 状态可以配不同业务码
   （围栏那三档 2004/2005/2006 都是 409）。
4. **照片落盘路径完全由本机决定**：`<媒体根>/dispose/<alarmId>/disposeN.<ext>`，
   `ext` 取自 part 的 **MIME** 而不是 `filename`。multipart 的 `filename` 在这套实现里
   只是数据，一个字都不进路径（离线契约给的正是 `../../etc/passwd`）。
   `alarmId`/`fenceId` 这些会拼进路径的段先过 `^[A-Za-z0-9_-]{1,64}$`。
5. **时间两个口径不混**：库里恒为定宽本地文本 `yyyy-MM-ddTHH:mm:ss`，出门前才补 `+08:00`
   （`pad::wireTime`）；对端进来的带偏移串先按本机时区换算回定宽（`pad::internalTimeFromWire`），
   直接截前 19 个字符在"对端不是 +08:00"时会错 8 小时。
6. **能力表照实报**（§7.2）。没实现的一律 `false`：`stream.playback`（本机没有按时间线录的产物）、
   `stream.startStop`（规约要求恒 false）、`count.*`/`checkout`（§11 整节没装）。
   报 `true` 的代价是 Pad 把按钮点下去拿 404，比 `false`（入口直接不显示）坏得多。
   `GET /device/status` 里**取不到的源不写那个键**（§7.1"只返回实际字段"），
   本机 NPU 利用率没有可读的 procfs 节点 → `npu_usage` 整条不给，不拿 CPU 顶上。
7. **`data.versions` 就是 `fence.versions`**（§13.5 明写"与 `fence.versions` 相同"）：
   实现里是从同一个 `QJsonArray` 拷出来的，物理上不可能分叉，别改成两份各自拼一遍。

## HTTP 面还没定、也没在代码里偷偷定的事
- **Pad 围栏与本机检测是两套，没有打通**。`pad_fences`/`pad_fence_versions` 只是把 Pad 画的
  区域存下来并按 v1 管版本；本机判定用的是 `geofence` 段那套归一化形状（`src/biz/geofence/`）。
  下发围栏后**检测行为不变**。要不要让前者驱动判定，规约没写，本机也不猜。
- **`frameUrl` 底下没有真实视频帧**。§5.3 要"合成生效画面 PNG"，本机渲的是
  背景网格 + 区域填色 + 编号（`biz/geofence/fence_frame_renderer.cpp`），没有叠任何一路实时帧——
  那需要 `biz → ui` 反向拿最新帧，本轮没做。
- **`GET /api/streams?taskId=` 不按 taskId 过滤**：本机没有"哪一路属于哪个计划"这层绑定
  （报警有 `task_id`，通道没有），所以要求带上、原样回显、返回全部通道。
- **`/dispose-options` 的 `alarmType` 不参与分组**：§13.3.1 收了它但没给"哪种报警配哪些选项"的表，
  所以本机对所有类型回同一份选项（配置 `pad_http.dispose_options` 可整组替换）。
- `plan.taskId` 与 body 顶层 `taskId` 不一致时**不校验**（围栏快照照 body 的存）。规约没说谁赢。
- 复制草稿（`sourceVersion`）时**沿用请求里的 `regions`**，不是把源版本的形状拷过来——
  §5.1 那句"复制版本"没讲形状从哪来。
- `syncState` 恒 `SUBMITTED`：本机没有"再往中心服务器上传"这一步（§8/§11 未装），
  没有 UPLOADED/FAILED 的产出路径。

## 核心流程

```
AlarmManager::alarmGenerated（解码线程）
    → AutoConnection 排队到 GUI 线程 → PadWsServer::handleAlarmGenerated
        → pad::alarmJson(record, mediaBaseUrl, mediaRoot)
        → 广播给所有已连接客户端                       ← 抓拍路径此刻已在记录里

AlarmManager::videoClipAttached（片段线程，约 +2s）
    → PadWsServer::handleVideoClipAttached
        → AlarmDAO::queryById 从库里重读（update_time 已被 updateVideoPath 推前）
        → 再广播一条完整 alarm（这次带 clipUrl）

AlarmManager::alarmStatusChanged（UI 线程：确认/误报成功之后）
    → PadWsServer::handleAlarmStatusChanged
        → pad::alarmUpdateJson → 广播 alarm_update（acknowledgeAll 不发，见口径 5）

握手：带 http(s):// Origin → 403，不进 clients_；空 Origin / android-app://* → 101
Pad: hello → hello_ack（serverTime/protocolVersion/capabilities.alarm）
     hello 缺 deviceId / 缺 token 键 / 缺 clientVersion → error 2001，连接不关
     ping  → pong（信封带 code=0/message=ok）
     sync_request{lastEventTime,taskId} → AlarmDAO::queryUpdatedSince（计划过滤在 SQL 里）→ 一个 sync_result
     sync_request 缺 taskId → error 2001，连接不关
     alarm_ack → 只记日志，不回复（§3.3 硬要求）
     未知 type → error 2001（信封层带 code/message，data.detail 带那个 type）
     protocolVersion != 1 → hello_ack 带 code=1001 并关闭连接
```

HTTP 面（`/api/`，同一份 `AlarmRecord`，出站报警 JSON 仍只从 `pad_wire` 出）：

```
一条 TCP 连接 → PadHttpSession：RequestDecoder 增量解（头/正文各有字节上限）
    → 取干缓冲里所有请求（keep-alive 流水线一次到齐也解得开）
    → PadHttpServer::dispatch：METHOD + 段数 + {param} 匹配 → 路由
    → 序列化响应（恒写 Content-Length）；对端要 close 或 HTTP/1.0 就关

GET  /api/health                    → ok/serverTime/wsClients/uptimeSec/capabilities{fence,stream,count,checkout}
GET  /api/device/status             → snake_case；取不到的键不写；cameras[] 一路一行
GET  /api/streams?taskId=…          → 回显 taskId + 全部通道（不按 taskId 过滤，见上）
GET  /api/streams/{streamId}/status → CH1..CHn；未知 id → 404/2002
GET  /api/dispose-options           → 选项来自 pad_http.dispose_options，空则内置两条
POST /api/alarms/{id}/dispose       → multipart(payload + 0..5 photos) → 照片落盘 → AlarmManager::disposeAlarmById
POST /api/alarms/{id}/false-alarm   → markedAt → markAsFalsePositiveById
POST /api/alarms/{id}/read          → readAt：**库里更早的就保留库里的**
GET  /api/fences?taskId=… / /api/fences/{id} → 围栏 + 版本（versions 与 fence.versions 同一份）
POST /api/fences/save-draft         → 新建/编辑/复制三种语义按 fenceId+sourceVersion+有无 DRAFT 分派
                                     校验 8 条 → 422/4001；状态冲突 → 409/2004|2005|2006
POST /api/fences/{id}/apply         → 先渲 PNG（失败 503/3003）→ 旧 ACTIVE 转 DISABLED → 草稿转 ACTIVE → 回填 frameUrl
POST /api/fences/{id}/disable       → 终态 DISABLED（不可再 apply）
POST /api/fences/{id}/delete-draft  → 只删 DRAFT；删 ACTIVE → 409/2005

四个 POST 写接口都套 runIdempotent(route, body, work)：先查 pad_request_cache，命中就把首次
那份响应原样写回去（含 4xx/5xx），不执行 work；未执行 == 不重复建版本、不重复落照片。
状态写成功之后统一 emit AlarmManager::alarmStatusChanged → WS 那边广播 alarm_update，
所以 HTTP 与 WS 两条链对同一 alarmId 收敛到同一个状态。
```

## 与媒体服务的关系
证据只给 URL，字节由 `src/media/server/`（内嵌 ZLM 静态服务，8090）出。按 §13.2.1（2026-11-05 改版）
两个字段**平铺在 `alarm` 顶层**：`evidenceSnapshotUrl`（string，**必填**，取不到给空串而不是省略键）
与 `clipUrl`（string?，没有给 `null`）。**没有 `evidence` 对象**，也**不再有 `snapshotUrls` 数组**。
⚠ 这一处改过三轮，别再凭"更好判空"动它：
   1. 2026-11-04 本机换成 `evidence.evidenceSnapshotUrl`（单键，理由是"数组取不到时还要判长度"）；
   2. 2026-11-05 上午按当时的规约改成 `evidence.snapshotUrls`（数组）；
   3. 2026-11-05 下午规约定为现在这版（顶层单键），本机跟着改。
   中间那轮的实测最值得记：真机按规约读 `snapshotUrls`，于是图片恒空，并且**连一次 8090 都没去连**
   （板端 `ss` 实测 Pad 只建立了 8899/8900 的连接，8090 零连接）。键名与嵌套是协议边界上的对端契约，
   本机单方面"改得更好判空"不构成理由；要改形状必须先让对方同步改规约。
   `tests/test_pad_wire.cpp` 第 11–13 组钉住现在的形状。
装配点在 `frmMain`：
`pad_server.enabled` 或 `pad_http.enabled` 且没写 `pad_server.media_base_url` 时，顺手把那台文件服务起起来
（与 `alarm_uplink` 的 url 档共用同一台，不起第二个服务器）。
**底址为空就一条地址都不发**——那台服务是零鉴权的，宁可让 Pad 显示"无证据"，
也不能发一个对端取不到、或指向 root 之外的地址。路径换算复用 `uplink::mediaUrlFor()`
（"越界一律不给"的判据有离线契约覆盖）；旧上行整块退役时要把这个函数挪进本目录，别复制。
片段要"报警后约 2 秒"才落盘，所以首帧 `clipUrl` 是 `null`、写完后发一条 `alarm_update` 带 `clipUrl`
（口径 3）。对端按 §3.5/§3.6 的 `alarmId + updatedAt` 就地合并。

同一台 8090 现在还多担两件事，都在 `data/alarms` 那棵树里（rootDir 之外一律不给）：
- **处置照片**（`dispose/<alarmId>/disposeN.<ext>`）：入站 multipart 的字节由 HTTP 面写盘，
  取字节仍走 8090，所以 §4.2 的 `photoUrls` 与 §3.6 的证据 URL 是同一套地址规则。
- **围栏合成画面**（`fence-frames/<fenceId>/v<n>.png`）：`apply` 渲完才回 `frameUrl`，
  底址没配就渲不出可点开的地址 → 直接 `503/3003`，不写一个对端取不到的 URL。

## 配置

### `pad_server` 段（WS 面，详见 `src/base/config/README.md`）
`enabled` 出厂 **true**（这台设备的用途就是被 Pad 连）。
`port` 默认 **8900**（A4 于 2026-11-04 定案：`http://<host>:8899/api/` 归 HTTP、`ws://<host>:8900/` 归 WS），
第三个口是 `alarm_uplink.media_http_port`（8090，媒体字节出站）。规约 §2.1 原本把 WS 与 `/api/` 都写成
8899 占位，`QWebSocketServer` 独占监听器所以同口做不到，现在按拆口解决。
⚠ 现场 `data/config.json` 里显式写了 `pad_server.port` 的话，改默认值无效——那份配置要一起改，
否则 WS 继续占住对方留给 `/api/` 的 8899。
WS 侧无 setter：改端口/开关要重启进程，避免"界面改了、其实连的还是旧口"。
监听是 `QHostAddress::Any`，**唯一的入站判据是 Origin 白名单**（口径 6），它没有配置开关：
放行别的来源要改 `originAllowed()` 并同步根 `README.md`「网络端口口径」那一行。

### `pad_http` 段（HTTP `/api/` 面）
`enabled` 出厂 **true**，代价写在配置注释里：这是**写**面，局域网内任何设备都能改报警状态与围栏。
`port` 默认 **8899**；`max_body_mb` 默认 **26**（§4.2 五张 5MB 照片 + JSON 余量）；
`expose_stream_credentials` 默认 **false**（`liveUrl` 剥口令）；`fence_frames` 默认 **true**
（关掉后 `apply` 一律 `503/3003`，用于现场不想渲图的场合）；`dispose_options` 空数组 = 用内置两条。
这一段的六个键**都有 setter**，调试设置页「Pad HTTP 服务端（/api/ 入站）」分组里改完点
「保存并应用」会 `stop()` 再按新配置 `start()`（HTTP 用自己的 `QTcpServer`，可以重绑；
8090 那台是 ZLM 进程级一次监听，**不能**重绑，所以端口不在这里给）。

## 自测
- `tests/test_pad_wire.cpp` — 16 组映射契约（纯 Qt Core，与 cwd 无关；会打印派生 deviceId 真值）
- `tests/test_pad_sync.cpp` — 9 组游标、已读与计划过滤语义（内存库）
- `tests/test_pad_ws_server.cpp` — 16 组线上行为（loopback 起真服务端 + 真客户端，含 Origin 拒绝、
  2001 两类、`alarmStatusChanged` → `alarm_update`）
- `tests/test_pad_http_message.cpp` — 19 组**传输层**契约（只喂字节，不起 socket）：chunked、
  头/正文上限、multipart 三 part、非法转义原样保留、keep-alive 连解两个请求、信封两形状
- `tests/test_pad_http_api.cpp` — 21 组**线上**契约（loopback 起真服务端 + 真 QTcpSocket + 临时库）：
  §4~§7 每一档错误码、幂等重放逐字节相同、照片 filename 是 `../../etc/passwd` 也只当数据、
  `liveUrl` 不含源口令、cameraId 用 `CHn` 不用注册表 id、apply 渲出真 PNG 且 8090 取得到、
  处置成功后模拟"关机整批回写"状态仍是 disposed

五个都在 `tests/README.md` 里有一句话摘要；改协议字段时按"映射 → 库 → 服务端"的顺序跑，
改 HTTP 报文形状时先跑 `test_pad_http_message`（它不需要库和 socket，秒级）。
⚠ `test_pad_http_api` 里有真渲染（`QPainter`），所以它必须 `QT_QPA_PLATFORM=offscreen` +
`QGuiApplication`——用 `QCoreApplication` 会在字体库里段错误（板端实测 gdb #0 在 libQt5Gui）。
整机组装分支（`frmMain` 里那段起服务 + 连信号）离线测试覆盖不到，2026-11-03 用
`QT_QPA_PLATFORM=offscreen` 起真 `idge`、外部裸 WebSocket 探针实测过 18 项，记录在审查报告 §十-2；
其中 Origin 403、2001 两类、`deviceId` 派生值都是真进程里拿到的，不是 mock。

2026-11-05 在同一台板子上对 HTTP 面又跑了一遍真进程（`build/build_rk3588_linux/data/idge.db`，
`run.sh` 实际加载的那份库）：8899/8900/8090 三口同进程监听；`/api/health` 的
`capabilities.fence.maxRegions=8`、`uptimeSec` 非 0；`/api/device/status` 14 个键、
`temperature_c=62.8`、`cpu_usage=35.5`、`npu_usage` 整条不给；CH3 的 `liveUrl` 是
`rtsp://192.168.124.36:554/live`（口令已剥）；`save-draft` 重放逐字节相同、`apply` 200 +
`http://192.168.124.43:8090/fence-frames/FENCE-002/v1.png` 取到 PNG；三条 pending 报警分别走
dispose（两张照片，filename 给的是 `../../etc/passwd`）/ dispose（一张）/ false-alarm →
库里 `status=disposed|false_alarm`、`dispose_photo` 是 JSON 数组、`dispose_person_id='05-0101'`
原样、`read_time` 保住更早的那个值；终态再 dispose/标误报 → `409/2003`；20KB 头一次到达 →
`400/2001`（旧实现在这里会放行）；`SIGTERM` 退出后这四行的状态在库里没被整批回写打回。

## 依赖方向
`biz/pad → biz/alarm`、`biz/db`、`biz/ws`（仅 `mediaUrlFor`）、`biz/geofence`（`pad_fence_store`
与 `fence_frame_renderer`，都是本域内的向下调用）、`base`，全部同层或向下；
**不 include `src/ui`**，因此可离线自测。服务端只在创建它的线程（GUI 线程）收发，
跨线程的 `AlarmManager` 信号靠 `AlarmRecord` 的 `Q_DECLARE_METATYPE` 自动排队。
HTTP 面只有 `pad_http_server.cpp` include `QTcpSocket/QTcpServer`，`pad_http_message` 与
`pad_http_alarms|fences|streams|device` 都不碰 socket——这就是为什么路由能拿一个裸 `Request`
结构体离线测（`test_pad_http_api` 起真服务端只是为了验 Content-Length 与 keep-alive 那一层）。
