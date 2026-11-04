# src/biz/pad

> 所属域：**biz 业务域**（本轮新增）

## 功能概述
Pad 接口 v1（`plan/idge-pad-v1.md`）里**本机作为服务端**的那一面：Pad 连进来，本机推报警、
回心跳、给断线补漏的快照。这是与 `src/biz/ws/`（本机主动连出去的旧客户端 + 旧上行）
方向完全相反的一条链路，两者**不是新旧两档、不能并存**（见审查报告冲突 B1）。

本轮只落 WS 报警面（v1 §3）。`/api/` HTTP 面（处置/误报/已读/围栏/流/设备/升级）**没有实现**，
原因不是工作量，而是四条口径还没人回答（端口分配 A4、`taskId` 来源 B2、围栏生效画面 A11、
报警类型取值表 A6）。§8 升级与 §11 清点/施工计划/销点确认**先不做**（2026-11-03 定案，A1 关闭）。
机制已就位的部分：出站 JSON 只有一个出口、
游标查询只有一条 SQL、开关只有一个配置段。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `pad_wire.h` / `.cpp` | `AlarmRecord` → v1 `alarm`/`alarm_update` 的**唯一**映射层：时区补齐、状态三值、类型表、证据 URL、信封。纯函数，离线可测 |
| `pad_ws_server.h` / `.cpp` | `QWebSocketServer` 单例：`hello_ack`/`pong`/`error`/`sync_result`/`alarm`/`alarm_update`，连接表与广播 |

## 六条必须先读的口径
1. **`rectified` 不等于 `DISPOSED`**。本机"确认"的语义是"我看过了"，v1 只认
   `PENDING/DISPOSED/FALSE_ALARM`（§3.10）→ 出站映射成 **`PENDING` + `readAt`**。
   真要上报"已处置"，得等处置表单（`dispose_*` 五列 + `AlarmDAO::dispose()` 现在，全工程零调用方）。
2. **`taskId` 只有 `construction_id` 一个来源**（扫码登录定死，已快照进每条报警）。
   未登录期间产生的报警 `taskId` 是空串：它不属于任何施工计划，对端按计划过滤时看不到它——
   这是事实而不是缺陷，但 B2 没确认之前它随时可能换成别的键名。
   未登录时它同样不该被伪造，所以 `alarmJson()` 里没有"兜底一个 local id"这种写法。
   反过来说，`sync_request` **不许带空 `taskId`**：§13.1.5 把它列为必填，空值一律 `error 2001`（口径 A19）。
3. **片段晚到，`alarm_update` 带不了证据**。v1 §13.2.3 的 `alarm_update` 只有
   `status/readAt/dispose/updatedAt`，而本机的视频片段是"报警后 2 秒"才由 ClipWriter 落盘的。
   → 片段到位后**再发一条完整 `alarm`**（§3.8 明说重复下发同一报警是安全的、按 `updatedAt` 合并）。
   这条替代方案本身是对规约的一处补充，需要 Pad 确认（审查报告歧义 A16）。
4. **`hello_ack` 的两个"必填"键不靠配置**。`deviceId` §13.1.3 规定必填，而 `pad_server.device_id`
   出厂是空串——空串的原因不是没 ID，而是配置文件会被拷到别的板子、写死会撞车。所以配置留空时
   按 `/etc/machine-id` 派生（`pad::defaultDeviceId()`，本机实测 `EDGE-1030A3F99414`），
   **配置填了就用配置的**。`capabilities.alarm.evidenceUrl` 反过来：它据实反映媒体服务在不在，
   服务起不来就报 `false`（口径 A17b："证据不可用 ≠ 协议违规"），不为了凑规约撒谎。
5. **状态变更要立刻变成一条 `alarm_update`**。UI/界面确认、误报驳回都走
   `AlarmManager::alarmStatusChanged(AlarmRecord)` → `PadWsServer::handleAlarmStatusChanged`，
   否则 §13.2.3 的 `alarm_update` 在本机是死代码（审查报告 P1-1）。
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

## 与媒体服务的关系
证据只给 URL，字节由 `src/media/server/`（内嵌 ZLM 静态服务）出。装配点在 `frmMain`：
`pad_server.enabled` 且没写 `pad_server.media_base_url` 时，顺手把那台文件服务起起来
（与 `alarm_uplink` 的 url 档共用同一台，不起第二个服务器）。
**底址为空就一条地址都不发**——那台服务是零鉴权的，宁可让 Pad 显示"无证据"，
也不能发一个对端取不到、或指向 root 之外的地址。路径换算复用 `uplink::mediaUrlFor()`
（"越界一律不给"的判据有离线契约覆盖）；旧上行整块退役时要把这个函数挪进本目录，别复制。

## 配置（`pad_server` 段，见 `src/base/config/README.md`）
`enabled` 出厂 **false**：开一个明文零鉴权的入站口必须人来决定。
`port` 默认 8899（v1 占位），与 `alarm_uplink.media_http_port`（8090，媒体出站）是两个口；
v1 §2.1 想让 WS 与 `/api/` 同占 8899，在这套栈上做不到（`QWebSocketServer` 独占监听器）。
无 setter：改端口/开关要重启进程，避免"界面改了、其实连的还是旧口"。
监听是 `QHostAddress::Any`，**唯一的入站判据是 Origin 白名单**（口径 6），它没有配置开关：
放行别的来源要改 `originAllowed()` 并同步根 `README.md`「网络端口口径」那一行。

## 自测
- `tests/test_pad_wire.cpp` — 16 组映射契约（纯 Qt Core，与 cwd 无关；会打印派生 deviceId 真值）
- `tests/test_pad_sync.cpp` — 9 组游标、已读与计划过滤语义（内存库）
- `tests/test_pad_ws_server.cpp` — 16 组线上行为（loopback 起真服务端 + 真客户端，含 Origin 拒绝、
  2001 两类、`alarmStatusChanged` → `alarm_update`）

三个都在 `tests/README.md` 里有一句话摘要；改协议字段时按"映射 → 库 → 服务端"的顺序跑。
整机组装分支（`frmMain` 里那段起服务 + 连信号）离线测试覆盖不到，2026-11-03 用
`QT_QPA_PLATFORM=offscreen` 起真 `idge`、外部裸 WebSocket 探针实测过 18 项，记录在审查报告 §十-2；
其中 Origin 403、2001 两类、`deviceId` 派生值都是真进程里拿到的，不是 mock。

## 依赖方向
`biz/pad → biz/alarm`、`biz/db`、`biz/ws`（仅 `mediaUrlFor`）、`base`，全部同层或向下；
**不 include `src/ui`**，因此可离线自测。服务端只在创建它的线程（GUI 线程）收发，
跨线程的 `AlarmManager` 信号靠 `AlarmRecord` 的 `Q_DECLARE_METATYPE` 自动排队。
