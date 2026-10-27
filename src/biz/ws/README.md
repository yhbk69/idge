# src/biz/ws

> 所属域：**biz 业务域**

## 功能概述
Pad↔Aibox 的 WebSocket 客户端。本程序作为 Pad 端连到远端 Aibox 服务，接收报警推送
（`type=alarm`）、心跳保活、断线自动重连，并保留查询视频流列表、下发围栏、对报警回 ACK
的上行口子。

当前状态：链路可用但只有一半——下行（alarm/pong/streams_list 解析）与心跳重连是通的，
上行的 `sendGetStreams()` / `sendSetFence()` **没有任何调用方**，因此 `streamsReceived`
在现网配置下不会触发（设置页那段"视频流列表"日志永远不会打印）。这两处属于预留接口，
改动前先确认对接方协议，别当作已交付功能。

接收侧的实际动作也很轻：`frmMain::onWsAlarmReceived` 只把 `alarm_type`/`image_url`/
`video_url` 打进调试页日志并回一条 `ack`，**不弹提示、不写 `idge.db`、不参与本机报警限流**；
`timestamp` 字段读出来后直接 `Q_UNUSED` 丢弃。

不做的事（明确非目标）：鉴权/token、消息排队重发、离线缓存、报警落库、围栏双向同步。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `ws_client.h` | `WsClient` 单例声明、信号与四个上行方法（ack / sync_request / get_streams / set_fence） |
| `ws_client.cpp` | 连接/心跳/重连、`type` 分发、JSON 编解码 |

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
| `server_url` | string | 空（不连） | `ws://` 或 `wss://`；空 = 这条链路整个不启动 |
| `heartbeat_interval` | int（秒） | 30 | 连上后起心跳；发的是应用层 `{"type":"ping"}`，不是 WebSocket 协议 ping |
| `reconnect_interval` | int（秒） | 5 | 非主动断开后的重连延时；单次定时器，连不上会再排一次 |

界面侧：**调试设置页**的 WebSocket 分组提供地址输入框 + 保存 + 连接/断开按钮 + 状态灯。
保存与手动连接都会先 `setWsServerUrl()` + `save()` 再操作 `WsClient`；
`heartbeat_interval`/`reconnect_interval` **只能在 JSON 里改**，界面无控件。

## 依赖关系
- `ConfigManager`（`src/base/config`）：三个 `websocket` 键的唯一来源；本模块不自己读文件。
- Qt5 `QtWebSockets`（根 `CMakeLists.txt` 已 `find_package`）。
- 被 `src/ui/form/frmmain.cpp` 单向调用与连接，无其他消费者。
- 与 `src/biz/alarm`、`src/biz/geofence` **零耦合**：远端报警不入 `AlarmManager`，
  本机围栏不从这里同步。

## 注意事项
- **`onSslErrors` 无条件 `ignoreSslErrors()`**：`wss://` 下等于放弃证书校验
  （自签/过期/域名不符照连）。内网调试可接受，公网对接前必须改成白名单或直接拒绝。
- **`set_fence` 协议与本机围栏模型不兼容**：上行参数是矩形 `x1/y1/x2/y2`，本机
  `geofence::FenceManager` 用的是任意多边形 + inside/outside 判定 + 类别过滤，
  两者对不上，这也是它至今没有调用方的原因。要真做围栏下发得先扩协议。
- **消息不排队**：`sendJson()` 在未连接时直接丢弃并记 warning。断线期间点"确认报警"
  发的 `ack` 就是丢了，服务端不会收到。
- **协议里没有鉴权字段**：任何能连上该端口的客户端都能推 `type=alarm`。部署时靠网络隔离兜。
- **`stop()` 后仍可 `start()`**：`QWebSocket` 对象复用，设置页"保存即重连"依赖这点；
  已连接时再调 `start()` 会触发 Qt 自身的 "Socket already connected" 警告（无害但会刷屏）。
- **本目录随 `src/biz` 的 GLOB 自动参与编译**，新增文件无需改 `CMakeLists.txt`。
