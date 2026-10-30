# src/biz/ws

> 所属域：**biz 业务域**

## 功能概述
Pad↔Aibox 的 WebSocket 客户端 + **本机报警上行**。本程序作为 Pad 端连到远端服务，接收报警推送
（`type=alarm`）、心跳保活、断线自动重连；同时把本机 `AlarmManager` 产生的报警**两段式**送出去：
第一段 `alarm_info`（报警文本 + 媒体预告），第二段在文件真的可读之后补 `media_begin`。

媒体怎么交出去有**两档**（配置 `alarm_uplink.media_transport`，**分叉点只有一处**：
`AlarmUplink::attach()` 把它折算成 `Settings::mediaViaUrl` 一个 bool，引擎内不再判字符串）：

| 档 | 媒体内容 | 地址/字节 | 状态 |
| --- | --- | --- | --- |
| `url`（默认） | 抓拍 `.jpg` 与片段 `.mp4` | JSON 里给本机内嵌 http 文件服务的**地址**，字节由对端下载 | 出厂默认，板端端到端实测通过 |
| `ws_binary` | 同上 | `media_begin` + N 个二进制分块帧 + `media_end` | 已实测保留，**默认不启用**（MB 级灌入会占 GUI 线程写缓冲，且要求对端能收大帧） |

两档**共用同一条队列/水位/ack/退避链路**，差别只在第二段的内容与字段。

当前状态：上行链路（`alarm_info`/`media_begin`/`media_failed` + 队列 + 重连补发）是通的；
下行（alarm/pong/streams_list 解析）与心跳重连也是通的。上行的
`sendGetStreams()` / `sendSetFence()` **没有任何调用方**，因此 `streamsReceived`
在现网配置下不会触发（设置页那段"视频流列表"日志永远不会打印）。这两处属于预留接口，
改动前先确认对接方协议，别当作已交付功能。

接收侧的实际动作也很轻：`frmMain::onWsAlarmReceived` 只把 `alarm_type`/`image_url`/
`video_url` 打进调试页日志并回一条 `ack`，**不弹提示、不写 `idge.db`、不参与本机报警限流**；
`timestamp` 字段读出来后直接 `Q_UNUSED` 丢弃。

不做的事（明确非目标）：鉴权/token、落库 outbox、进程重启后的历史补推、`wss://`、
报警/录制/文件服务的设置页 UI（只能改 JSON）、远端报警入库。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `ws_client.h` | `WsClient` 单例声明、信号与四个上行方法（ack / sync_request / get_streams / set_fence） |
| `ws_client.cpp` | 连接/心跳/重连、`type` 分发（含下行 `uplink_ack`）、JSON 编解码；`UplinkTransport` 的当前实现 |
| `uplink_transport.h` | 引擎只认的抽象接口：`isConnected/canAcceptMore/sendText/sendBinary`——引擎不 include `QWebSocket`，才可能只链 Qt5::Core 跑离线自检；当前唯一实现是 `WsClient` |
| `alarm_uplink_engine.h/.cpp` | 报文组装 + 待发队列 + 水位闸门 + ack 对账与退避（纯逻辑，无 QObject、不读配置）；协议字面量全集中在 `namespace uplink` |
| `alarm_uplink.h/.cpp` | GUI 线程单例的编排层：订阅 `AlarmManager`、读配置装配引擎、两档分叉、把 `AlarmRecord` 折成 `AlarmInfoInput` |
| `media_payload.h/.cpp` | 把磁盘上的媒体读成"字节 + sha256"，含"等文件写稳"（连续两次 size 相同）与超时 |
| `media_pump.h/.cpp` | 后台读盘线程（`std::thread` + 有界 deque，满 6 丢最旧），成品投回 GUI 线程才能发；被挤掉的一方必须收到 error |

## 核心类与数据流
- **WsClient**（`QObject` 函数内 static 单例）：首次 `instance()` 发生在 GUI 线程，
  `QWebSocket` 与两个 `QTimer` 都挂在 GUI 线程，**全程单线程**，`start()`/`stop()`
  只能从 GUI 线程调。

```
frmMain 构造 → WsClient::instance().start()
    → ConfigManager.wsServerUrl() 为空 → 仅一条 warning 日志，整条链路缺席（正常态）
    → 非空 → QWebSocket::open(url)（异步）
         ├─ onConnected    → 起心跳定时器（{"type":"ping"} 应用层帧）+ 有 lastAlarmId_ 则补 sync_request
         ├─ onTextMessage  → 按 "type" 分发：
         │     alarm        → 记 lastAlarmId_ → emit alarmReceived(data) → frmMain::onWsAlarmReceived → 日志 + sendAck
         │     pong         → 忽略
         │     streams_list → emit streamsReceived（当前无触发路径，见"功能概述"）
         │     其他         → 一条"未知消息类型"日志，不断连
         └─ onDisconnected  → 停心跳；stopping_==false 时起**单次**重连定时器
```

`stopping_` 是主动停与掉线的唯一区分依据：`stop()` 先置位再 `close()`，反过来会让每次
主动退出都白排一次重连。JSON 解析失败只记 warning 并丢弃该帧。

### 报警上行：两段式与队列

```
frmMain 构造 → WsClient::start() →（url 档才有的两步）
    HttpFileService::start(data/alarms 绝对路径, media_http_port)
    AlarmUplink::setMediaBaseUrl(svc.baseUrl(media_base_url))   ← 必须在 attach 之前
  → AlarmUplink::instance().attach()
      enabled=false → 只记一条日志，一个信号都不接（出厂态零负载）
      否则接 AlarmManager::alarmGenerated / videoClipAttached / videoClipSkipped（Qt::QueuedConnection）

报警产生（GUI 线程收到）
  → engine.submit(AlarmInfoInput)
      按 (kind, 批次键=抓拍图路径) 归并 media slot：一份字节只登记一次、只申报一次
      url 档：snapshot 子对象当场带 url（路径此刻已定死，文件还没写完 → planned=true）
              clip 子对象只承诺：name/mime/url 全不给（文件名由 submitAlarmClip 自己拼时间戳）
      入队 alarm_info（PendingItem.media=false）
  → MediaPump::submit(snapshot)  后台等文件写稳 → 读全量 → sha256
      成功 → engine.submitMediaUrl(job, url, size, sha)   发 media_begin(transfer:"http")
      失败/被挤掉 → engine.submitMediaFailed(...)          发 media_failed

片段就绪（报警后约 2~5 秒，ClipWriter dump 完）
  → AlarmManager::videoClipAttached(ids, 裸流路径, mp4 副本路径)
      url 档交给 pump 的是 **mp4Path**；第三参为空 = 副本没写成 → media_failed
```

**第二段的两条不变量**（改队列代码前先对齐，错了会凭空造帧或重复申报）：
1. url 档的每一项都是**单个文本帧**，必须 `PendingItem.media=false` 入队、`itemId=media_id`。
   标成 `media=true` 会让 `pumpOut()` 的收尾判据（`head.media && cursor >= frames.size()`）
   凭空合成一条 `media_end{seqs_sent:0,total_chunks:0}`；
2. `restartInFlightMedia()` 只重置 `media=true` 的游标——标错的话每次重连都会把同一个地址
   重复申报一遍。

队列语义：内存队列，`max_pending` 满了丢最旧（丢到**已申报的媒体**时日志升到 `error` 并点明
"对端会拿不到"）；`pumpOut()` 受 `UplinkTransport::canAcceptMore()` 的 1MB 写缓冲水位闸门约束，
没退下来就按 `nextWaitMs()` 再醒一次（水位空转 20ms，等 ack/退避则等到那个点，不空转）；
`expect_ack=true` 时发完帧留在队首等 `uplink_ack`，`ok` 出队、`retry` 整项重发并进
`min(2^n×2s, 30s)` 退避梯子、`reject` 直接丢弃（重发只会再来一次 reject）、id 对不上队首只记
warning 不认领任何东西。

### 上行报文（`{type, message_id, ts, data}` 信封）

| type | 什么时候发 | `data` 关键字段 |
| --- | --- | --- |
| `alarm_info` | 报警产生那一刻 | `alarm_id`、`alarm_ids`（共用这份媒体的批次）、`alarm_type`、`alarm_level`、`alarm_time`、`channel`（**1-based**）、`class_id`、`class_name`、`confidence`、`is_fence_alarm`、`status`，外加 `snapshot` / `clip` 两个 `MediaRef` 子对象与 `construction` 子对象（见下） |
| `media_begin` | 媒体真的可取了 | `media_id`、`alarm_id`、`kind`、`name`（落盘后的真实名）、`mime`、`total_bytes`、`transfer`；`http` 档加 `url`（`sha256` 算得出就带），`binary_chunks` 档加 `chunk_size`/`total_chunks` |
| `media_end` | **仅 `ws_binary` 档**：块发完 | `media_id`、`seqs_sent`、`total_chunks`、`sha256`。url 档没有帧序列要收尾，**不发这条** |
| `media_failed` | 承诺落空（两档都发） | `media_id`、`alarm_id`、`kind`、`reason`（人能读的一句话，绝不含字节内容） |

`MediaRef`（`alarm_info` 的子对象）= `media_id` + `planned:true` + `transfer`，再按档位补：
`name`/`mime`（url 档的 clip 不给）、`expected_bytes`/`available_in_ms`（预告值，真实值以
`media_begin` 为准）、`url`（url 档的 snapshot 当场给）。

下行 `uplink_ack` 的字段是 `message_id`/`media_id`/`phase`/`status`/`error`，
`phase=="media"` 用 `media_id` 认领队首、否则用 `message_id`。

### `construction` 子对象（本轮新增，**键名待接口方确认**）

`alarm_info.data.construction` = 该报警产生时**登录会话里的整份施工信息**，
一个嵌套 JSON 对象，内容由 `src/biz/session` 的 `ConstructionInfo` 决定（当前五个键：
`construction_id`/`worker_name`/`worker_phone`/`worker_id`/`work_content`，外加码里自带的未知键原样透传）。

实测（板端 loopback，`ws://127.0.0.1:8765`）一条真实 `alarm_info` 的 `data` 键集：

```
['alarm_id','alarm_ids','alarm_level','alarm_time','alarm_type','channel','class_id',
 'class_name','confidence','construction','is_fence_alarm','snapshot','status']
construction = {"construction_id":"SG-2026-11","work_content":"隧道道床修补",
                "worker_id":"GZ-0042","worker_name":"张三","worker_phone":"13800001111"}
```

三条口径，改动前先看清为什么这么定：

- **嵌套而不是平铺**：平铺进 `data{}` 会和 `alarm_id`/`status`/`channel`/`class_name` 抢键，
  而且现场码的字段一改，线上协议就跟着漂。嵌套后协议里多一个稳定的容器键，字段增减都在容器内。
  ⚠ **这个容器键名要接口方点头**，本轮先按 `construction` 落。
- **未登录不发这个键**（不是发空对象）：`AlarmInfoInput::constructionJson` 为空时
  `buildAlarmInfoMessage()` 整个跳过，对端"键不存在"= 没有归属，比"键存在但全空"更好判。
- **取的是报警产生时的那一份**：字段在 `makeAlarmRecord()` 就固化进 `AlarmRecord`，
  `onAlarmGenerated` 只是搬运，发送时不回查会话。所以退出登录/重新扫码之后仍在队列里补发的报警，
  带的仍是它自己产生时的负责人。`constructionJson` 里是脏 JSON 时也只跳过这个键，不影响整条报文。

### `media_failed` 的落空来源（五条路径，四条跑得出来）
闭合的洞是"对端手里攥着一条永远兑现不了的 `planned`"。四条落空路径：
截图读盘超时（`上行: 媒体文件等待超时 … size=-1`）、读盘队列被挤掉（`读盘队列已满…`）、
ClipWriter 取窗口失败（`取窗口写裸流失败`，含"录像器未就绪"与"片段队列已满被挤掉"两个同族原因）、
`.mp4` 副本没写成（`mp4 副本没写成`）。
前三条板端把 `data/alarms/<当天>` 改成只读就跑出来了（45 条，见 `plan/websocket.md` §四）；
第四条由 `videoClipAttached` 第三参为空触发，板端没能单独制造（裸流会先失败），
由离线自检组 16 锁形状。
还有第五条是纯防御：`mediaUrlFor()` 判定路径不在服务根目录之下（返回空串）→ 直接
`media_failed`，绝不发一个可能越出根的地址。

## 使用方法
`data/config.json` 的 `websocket` 段（由 `ConfigManager` 的 `ws*()` 一组 getter 读取）：

```json
{
  "websocket": {
    "server_url": "ws://192.168.1.100:8765",
    "heartbeat_interval": 30,
    "reconnect_interval": 5
  }
}
```

| 键 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `server_url` | string | 空（不连） | **只填 `ws://`**；空 = 这条链路整个不启动。`wss://` 在本机不可用——`/opt/qt5.12.8` 的 QtNetwork 按 OpenSSL 3 头编，但符号解析表仍找 OpenSSL 1.1 的老名字，`SSL_get_peer_certificate`/`EVP_PKEY_base_id` 两个 dlsym 失败，走 TLS 握手的路径等于拿空函数指针。已登记待办，暂不适配 |
| `heartbeat_interval` | int（秒） | 30 | 连上后起心跳；发的是应用层 `{"type":"ping"}`，不是 WebSocket 协议 ping |
| `reconnect_interval` | int（秒） | 5 | 非主动断开后的重连延时；单次定时器，连不上会再排一次 |

界面侧：**调试设置页**的 WebSocket 分组提供地址输入框 + 保存 + 连接/断开按钮 + 状态灯。
保存与手动连接都会先 `setWsServerUrl()` + `save()` 再操作 `WsClient`；
`heartbeat_interval`/`reconnect_interval` **只能在 JSON 里改**，界面无控件。

### 启用报警上行
`data/config.json` 的 `alarm_uplink` 段，十一个键的默认值与逐条含义见
`src/base/config/README.md`（**那张表是唯一维护处，这里不重复**），要点四条：

- `enabled` 出厂 `false`：这是往外发数据，且链路明文零鉴权，必须人来打开；
- 要出媒体还得起 `websocket.server_url`——上行帧走的是同一条 WS 连接，没连上就只在本地日志里；
- `media_transport="url"`（默认）会让本机**监听 `media_http_port`**，见 `src/media/server/README.md`；
- 迁移只补缺失段：现场老配置里不会有 `media_*` 三个新键，全靠 getter 默认值兜住。

### 离线自检与本机联调
```
./build/build_rk3588_linux/test_alarm_uplink      # 十八条契约，rc 0=PASS，不依赖网络与磁盘
./build/build_rk3588_linux/ws_sink 8765 /tmp/sink_out   # 本机回环收端（QCoreApplication，无需 DISPLAY）
```
`ws_sink` 两档都认：`transfer="binary_chunks"` 才开文件拼块并核 `media_end`，
`transfer="http"` 只把 `url/sha256/total_bytes` 打出来（**一个字节都不落盘**），
由外部 `curl` 取回再与 `data/alarms/` 下的同名文件对拍 sha256。它是"对端最低标准对照物"，
不是产品代码。日志口径与产品侧一致：**只打摘要，媒体字节永不进日志**。

## 依赖关系
- `ConfigManager`（`src/base/config`）：`websocket` 三个键 + `alarm_uplink` 十一个键的唯一来源；
  本模块不自己读文件。
- Qt5 `QtWebSockets`（根 `CMakeLists.txt` 已 `find_package`）。只有 `ws_client.*` 与 `ws_sink` 用它，
  `alarm_uplink_engine.*`/`media_payload.*`/`media_pump.*` 只链 Qt5::Core（离线自检的前提）。
- 被 `src/ui/form/frmmain.cpp` 单向调用与连接，无其他消费者。
- 与 `src/biz/alarm`：**发送侧有耦合**（`AlarmUplink` 订阅 `alarmGenerated`/`videoClipAttached`/
  `videoClipSkipped`，并把 `AlarmRecord` 逐字段折成 `AlarmInfoInput`），**接收侧零耦合**
  （远端报警不入 `AlarmManager`，本机围栏不从这里同步）。
- **不 include `src/media` 的任何头**：分层是 `base ← media ← ai ← biz ← ui`，反着来就是破例。
  所以 http 文件服务由 `frmMain` 起、`baseUrl` 用 `setMediaBaseUrl()` 注进来、
  片段的 `.mp4` 路径由 `AlarmManager::videoClipAttached` 的第三个参数带过来。

## 注意事项
- **传输不加密，靠网络隔离兜**：`server_url` 只填 `ws://`（原因见上表），报文里的报警/围栏
  在链路上是明文。当前口径是内网部署，不做 TLS 适配。
- **url 档会让本机多监听一个端口**（`media_http_port`，默认 8090，明文零鉴权）：
  能连通本机的任何人都可按路径取走 `data/alarms` 下的全部报警媒体。开启前先确认网段隔离，
  细节与实测语义见 `src/media/server/README.md`。
- **地址是 best-effort，404 是正常终态**：`AlarmManager::pruneAlarmMedia` 按天删 + 4GB 硬上限，
  **上限能删掉当天的整个目录**（实测出现过 `alarm media over cap, dropped "20261027"`）。
  对端必须"收到 `media_begin` 立刻取回"；本轮不做落库 outbox、不做失效重发。
- **`media_failed` 不能省**：截图写失败、读盘超时、pump 被挤掉、`.mp4` 副本没写成——
  任何一条静默过去，对端手里就是一条永远兑现不了的 `planned`。新增落空路径时必须接上这条。
- **媒体字节永不进日志**：引擎与 `ws_sink` 都只打 `media_id/name/kind/字节数/sha256` 摘要；
  一次报警的片段是 MB 级，`qInfo` 一行就能把日志盘刷爆。**也不要用 `QString::fromUtf8` 碰二进制**
  （那是上一轮分块链路里最容易犯的错）。
- **全程 GUI 线程**：`AlarmUplink` 与引擎都不加锁，靠"只在 GUI 线程被调用"这条约束成立；
  解码线程发来的信号用 `Qt::QueuedConnection` 收。把任何一步挪到别的线程就等于同时改两件事。
- **`sendJson()` 不排队**（下行 `ack` 那条老路）：未连接时直接丢弃并记 warning，断线期间点的
  "确认报警"就是丢了。**报警上行不走这条路**——上行有自己的内存队列 + 重连补发，
  但队列是内存的，进程重启即重新开始。
- **`server_url` 只填 `ws://`**：`wss://` 在本机不可用的根本原因是 TLS 后端半残（见上表）。
  `onSslErrors` 无条件 `ignoreSslErrors()`，填 `ws://` 时该回调不会被触发，这条现在是死代码；
  将来若真启用 `wss://`，它等于放弃证书校验（自签/过期/域名不符照连），必须先修 TLS 后端
  再改成指纹白名单或直接拒绝。
- **`set_fence` 协议与本机围栏模型不兼容**：上行参数是矩形 `x1/y1/x2/y2`，本机
  `geofence::FenceManager` 用的是任意多边形 + **每形状角色**（禁区/岗区域）+ 类别过滤，
  两者对不上（协议里连角色字段都没有），这也是它至今没有调用方的原因。要真做围栏下发得先扩协议。
- **协议里没有鉴权字段**：任何能连上该端口的客户端都能推 `type=alarm`。部署时靠网络隔离兜。
- **`stop()` 后仍可 `start()`**：`QWebSocket` 对象复用，设置页"保存即重连"依赖这点；
  已连接时再调 `start()` 会触发 Qt 自身的 "Socket already connected" 警告（无害但会刷屏）。
  （注意这与 `HttpFileService` 相反：那边的 `mk_env_init1` 进程级一次，`stop()` 后不能再生。）
- **本目录随 `src/biz` 的 GLOB 自动参与编译**，新增文件无需改 `CMakeLists.txt`；
  `tests/test_alarm_uplink.cpp` 与 `tests/ws_sink.cpp` 要在根 `CMakeLists.txt` 里各自加 target，
  且前者被 `.gitignore` 的 `test_*.cpp` 规则挡住，已用 `!tests/test_alarm_uplink.cpp` 显式放行。
