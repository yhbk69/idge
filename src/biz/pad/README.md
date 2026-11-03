# src/biz/pad

> 所属域：**biz 业务域**（本轮新增）

## 功能概述
Pad 接口 v1（`plan/idge-pad-v1.md`）里**本机作为服务端**的那一面：Pad 连进来，本机推报警、
回心跳、给断线补漏的快照。这是与 `src/biz/ws/`（本机主动连出去的旧客户端 + 旧上行）
方向完全相反的一条链路，两者**不是新旧两档、不能并存**（见审查报告冲突 B1）。

本轮只落 WS 报警面（v1 §3）。`/api/` HTTP 面（处置/误报/已读/围栏/流/设备/升级）**没有实现**，
原因不是工作量，而是五条口径还没人回答（端口分配 A4、`taskId` 来源 B2、围栏生效画面 A11、
§8/§11 要不要现在做 A1、报警类型取值表 A6）。机制已就位的部分：出站 JSON 只有一个出口、
游标查询只有一条 SQL、开关只有一个配置段。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `pad_wire.h` / `.cpp` | `AlarmRecord` → v1 `alarm`/`alarm_update` 的**唯一**映射层：时区补齐、状态三值、类型表、证据 URL、信封。纯函数，离线可测 |
| `pad_ws_server.h` / `.cpp` | `QWebSocketServer` 单例：`hello_ack`/`pong`/`error`/`sync_result`/`alarm`/`alarm_update`，连接表与广播 |

## 三条必须先读的口径
1. **`rectified` 不等于 `DISPOSED`**。本机"确认"的语义是"我看过了"，v1 只认
   `PENDING/DISPOSED/FALSE_ALARM`（§3.10）→ 出站映射成 **`PENDING` + `readAt`**。
   真要上报"已处置"，得等处置表单（`dispose_*` 五列 + `AlarmDAO::dispose()` 现在，全工程零调用方）。
2. **`taskId` 只有 `construction_id` 一个来源**（扫码登录定死，已快照进每条报警）。
   未登录期间产生的报警 `taskId` 是空串：它不属于任何施工计划，对端按计划过滤时看不到它——
   这是事实而不是缺陷，但 B2 没确认之前它随时可能换成别的键名。
   未登录时它同样不该被伪造，所以 `alarmJson()` 里没有"兜底一个 local id"这种写法。
3. **片段晚到，`alarm_update` 带不了证据**。v1 §13.2.3 的 `alarm_update` 只有
   `status/readAt/dispose/updatedAt`，而本机的视频片段是"报警后 2 秒"才由 ClipWriter 落盘的。
   → 片段到位后**再发一条完整 `alarm`**（§3.8 明说重复下发同一报警是安全的、按 `updatedAt` 合并）。
   这条替代方案本身是对规约的一处补充，需要 Pad 确认（审查报告歧义 A16）。

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

Pad: hello → hello_ack（serverTime/protocolVersion/capabilities.alarm）
     ping  → pong
     sync_request{lastEventTime,taskId} → AlarmDAO::queryUpdatedSince → 一个 sync_result
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

## 自测
- `tests/test_pad_wire.cpp` — 15 组映射契约（纯 Qt Core，与 cwd 无关）
- `tests/test_pad_sync.cpp` — 8 组游标与已读语义（内存库）
- `tests/test_pad_ws_server.cpp` — 11 组线上行为（loopback 起真服务端 + 真客户端）

三个都在 `tests/README.md` 里有一句话摘要；改协议字段时按"映射 → 库 → 服务端"的顺序跑。

## 依赖方向
`biz/pad → biz/alarm`、`biz/db`、`biz/ws`（仅 `mediaUrlFor`）、`base`，全部同层或向下；
**不 include `src/ui`**，因此可离线自测。服务端只在创建它的线程（GUI 线程）收发，
跨线程的 `AlarmManager` 信号靠 `AlarmRecord` 的 `Q_DECLARE_METATYPE` 自动排队。
