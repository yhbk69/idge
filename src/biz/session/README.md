# src/biz/session

> 所属域：**biz 业务域**

## 功能概述
施工信息与登录会话。扫码登录页解出一段 JSON 文本后，**唯一的**解析+校验入口就是这里；
校验通过才叫"登录成功"，主窗口的门禁才放行。会话内容随后被盖到每一条报警上
（落库、详情框、WebSocket 上行三处共用同一份 JSON）。

**契约出处**：`plan/qr-login-edge-v1.md`（2026-11-04），示例载荷 `plan/qr-login.json`。
形状是 `person{}` + `plan{}` 的嵌套对象，顶层另有 `issuedAt/expiresAt/signatureAlg/signature`。
旧扁平形状（`construction_id`/`worker_name`/`worker_phone`/`worker_id`/`work_content`）
**只作为历史库的读取兼容**存在，不再是登录输入。

**改字段只动 `construction_session.h`/`.cpp` 里的三处**（`ConstructionInfo` 成员、
`toJson()`、`readObject()`/`fields()` 里的键表），其余各处自动跟着走：

| 消费方 | 怎么拿到施工信息 | 加字段要不要改 |
| --- | --- | --- |
| 报警库 | `alarms.construction_info` 存**整份嵌套 JSON**（外加 `worker_name`/`task_id` 两列供列表与过滤） | 不要 |
| 报警详情框 | `ConstructionInfo::fieldsFromJson()` 逐键渲染成行，不是按固定字段渲染 | 不要（要中文名才加一行 `kPlanFields`） |
| 施工详情框（右上角登录人点开） | `ConstructionInfo::fields(info.toJson())`，与上面同一个函数 | 同上 |
| 标题栏登录人角标 | 只读 `person.name` 一项（标题栏抢不起横向空间） | 只有改名时要 |
| WebSocket 上行（旧链路） | `alarm_info` 的 `data["construction"]` = 解析后的**嵌套子对象** | 不要 |
| 报警列表 / 侧栏卡片 / toast | 只读 `worker_name` 一个字段（= `person.name`） | 只有改名时要 |
| Pad 的 `taskId` | `alarms.task_id` 列，写入兜底走 `taskIdFromJsonText()` | 不要 |

代价说清楚：**不能直接按 SQL 过滤计划时间/线路/车站**（那要先 `json_extract`，本机 QSQLITE
没有 JSON1）。唯一需要被过滤的键（`plan.taskId`）单独升成了 `alarms.task_id` 一列。

`fields()` 为什么放在 biz 层而不是某个对话框的 `.cpp` 里：键→中文名 + 显示顺序**本身就是字段契约
的一部分**，写进 UI 文件就等于契约有两处。它只依赖 QtCore，不碰任何 widget。

不做的事（明确非目标）：服务端登录、token、账号体系、按人/按施工id 的检索统计、
登录页手输施工信息（本机不做虚拟键盘，见 `src/ui/core/README.md` 触屏口径）。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `construction_session.h` | `ConstructionInfo`（字段契约的唯一真值源）+ `ConstructionField`（一行的 label/value）+ `LoginPolicy`（§6.6 计划比对）+ `ConstructionSession` 单例声明 |
| `construction_session.cpp` | 解析与校验（`fromJson`/`fromQrText`）、§5 计划窗口推导、读写锁保护的会话状态、`toJson` 规范化、`fields()` 逐键渲染 |

## 核心契约

- **必填（§6 第 2 步）**：`person.id`、`person.name`、`plan.taskId`，且 `plan.totalPersonCount > 0`。
  缺一项即**拒绝登录**，提示按规约 §9 的表（`二维码缺少人员信息` / `二维码缺少施工计划` /
  `二维码计划人数无效`），**一次只报第一条**——现场补一项就能再试。
- **时间必须可推导（§6 第 3 步）**：`year/month/days[0]/endtime` 合法，且能算出 `start < end`，
  否则 `二维码施工计划时间无效`。推导结果存进 `startsAt`/`endsAt`（固定 +08:00），
  详情框的「计划时间」一行就是它，后续任何使用方都不各自重算。
- **§5 推导规则**：基准日期 = `year-month-days 第一个非空值`；开始时间取 `starttime`，
  空则 `constrainttime=="4"` → `23:00`；`endmorrow` 让结束日期 +1 天；
  结束不晚于开始且 `endmorrow=false` 时自动跨零点 +1 天（只加一次）；
  最后必须 `end > start`。多日期只取第一个非空值（§5.3）。
- **§6.4 有效期与 §6.5 验签本轮不做**（2026-11-04 裁定：先不做过期校验和签名校验）。
  `issuedAt/expiresAt/signatureAlg/signature` 仍照原样解析、随会话与 `construction_info` 保存
  （规约 §8 要求会话保留签发/过期时间），但不据此拒绝登录。
- **§6.6 计划比对**：`LoginPolicy::expectedTaskId` 非空时 `plan.taskId` 必须相等，
  否则 `二维码施工计划与当前设备任务不一致`。这是"设备绑定到某个计划"的唯一入口，
  值来自 `config.json` 的 `qr_login.expected_task_id`（默认空 = 不比对）。
- **历史行兼容**：`taskIdFromJsonText()` 先读 `plan.taskId`、读不到再回落旧键 `construction_id`；
  `fields()` 遇到旧扁平形状按"施工id（旧）/负责人（旧）…"渲染。历史行**只用于显示与 taskId**，
  旧扁平载荷不能再用来登录（缺 `person.id`）。
- `toJson()` = 码里的嵌套形状，**未知键原样透传**（顶层未知键、`person`/`plan` 下的未知子键都保留）；
  旧扁平键不会再被写出去（读得进、写不出）。
- `startLogin()` 失败**不动会话**——不存在"半登录"状态；成功则把这份信息定死为会话内容，
  直到 `logout()`。**不退出登录就不换信息**：登录状态下再扫一张码不会静默改写归属。

## 线程与锁
写只发生在 UI 线程（登录页 `startLogin` / 退出登录 `logout`）。
读发生在**解码线程**：`AlarmManager::makeAlarmRecord()` 调 `snapshot()` 给每条报警打戳。
`snapshot()` 用 `QReadWriteLock` 的读锁**按值返回**，函数返回即释放——调用方不得持锁跨数据库写。

**打戳时机是"产生时"，不是"发送/显示时"**：报警一落地就把当时的施工信息固化进记录，
之后退出登录、重新扫码、上行重传都不会改写它。所以退出登录后仍在队列里的那条重传，
带的仍是**它产生时**的负责人——这正是归属应有的语义。

## 依赖方向
`src/biz/alarm` → `src/biz/session` 是同层合法边；本模块**不 include 任何 `src/ui` / `src/base` 头**，
只用 Qt Core（`QString`/`QJsonObject`/`QDateTime`），因此可离线自测：
`tests/test_construction_qr.cpp`（13 组契约，含"解一张真码 → 登录"与"缺字段的码解得开但过不了校验"）。
