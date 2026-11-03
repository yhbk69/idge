# src/biz/session

> 所属域：**biz 业务域**（本轮新增）

## 功能概述
施工信息与登录会话。扫码登录页解出一段 JSON 文本后，**唯一的**解析+校验入口就是这里；
校验通过才叫"登录成功"，主窗口的门禁才放行。会话内容随后被盖到每一条报警上
（落库、详情框、WebSocket 上行三处共用同一份 JSON）。

**这个模块存在的真正理由是"字段还没定稿"**：施工信息的键名/键数大概率还要改，
所以整套设计的目标是——**改字段只动 `construction_session.h`/`.cpp` 里的四处**
（`ConstructionInfo` 成员、`toJson()`、`fromJson()`、`.cpp` 里 `kPreferredFields[]` 那行
"这个键在界面上叫什么、排第几"），其余各处自动跟着走：

| 消费方 | 怎么拿到施工信息 | 加字段要不要改 |
| --- | --- | --- |
| 报警库 | `alarms.construction_info` 存**整份 JSON 文本**（外加 `worker_name` 一列供列表显示） | 不要 |
| 报警详情框 | `ConstructionInfo::fieldsFromJson()` 逐键渲染成行，不是按固定字段渲染 | 不要（要中文名才加一行 `kPreferredFields`） |
| 施工详情框（右上角登录人点开） | `ConstructionInfo::fields(info.toJson())`，与上面同一个函数 | 同上 |
| 标题栏登录人角标 | 只读 `worker_name` + `construction_id` 两项（标题栏抢不起横向空间） | 只有改名时要 |
| WebSocket 上行 | `alarm_info` 的 `data["construction"]` = 解析后的**嵌套子对象** | 不要 |
| 报警列表 / 侧栏卡片 / toast | 只读 `worker_name` 一个字段 | 只有改名时要 |

代价说清楚：**不能直接按 SQL 过滤电话/工作内容**（那要先 `json_extract`，本轮不做）。
`worker_name` 之所以单独成列，是因为列表那一列每帧都要显示它，不该为了一列文本去解析 JSON。

`fields()` 为什么放在 biz 层而不是某个对话框的 `.cpp` 里：键→中文名 + 显示顺序**本身就是字段契约
的一部分**，写进 UI 文件就等于契约有两处。它只依赖 QtCore（`QVector`/`QPair` 级别的纯数据），
不碰任何 widget，所以放这层不破坏"biz 不依赖 src/ui"的方向。

不做的事（明确非目标）：服务端登录、token、账号体系、按人/按施工id 的检索统计、
登录页手输施工信息（本机不做虚拟键盘，见 `src/ui/core/README.md` 触屏口径）。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `construction_session.h` | `ConstructionInfo`（字段契约的唯一真值源，键名以 `kKey*` 常量集中列出）+ `ConstructionField`（一行的 label/value）+ `ConstructionSession` 单例声明 |
| `construction_session.cpp` | 解析与校验（`fromJson`/`fromQrText`）、读写锁保护的会话状态、`toJson` 规范化、`fields()` 逐键渲染（`kPreferredFields[]` = 键的中文名与顺序） |

## 核心契约

- **必填只有两个**：`construction_id`（施工id）与 `worker_name`（负责人姓名）。缺任一个即
  **拒绝登录**，中文错误里点出缺的是哪一个（实测：`二维码缺少字段 施工id(construction_id)`）。
  校验规则**只写在 `fromJson` 一处**，登录页/测试登录/单测都走它，不分叉。
- 可空字段：`worker_phone`、`worker_id`、`work_content`。空串在界面统一显示 `—`（`display()`）。
- `toJson()` = `raw`（码里的完整载荷，原样留着）**再覆盖**五个具名字段。这样未知键
  （将来协议加字段、或现场码里本来就带的多余键）能原样透传，不会因为结构体没跟上而被吃掉。
- `startLogin()` 失败**不动会话**——不存在"半登录"状态；成功则把这份信息定死为会话内容，
  直到 `logout()`。**不退出登录就不换信息**：登录状态下再扫一张码不会静默改写归属。

## 线程与锁
写只发生在 UI 线程（登录页 `startLogin` / 退出登录 `logout`）。
读发生在**解码线程**：`AlarmManager` 的 `makeAlarmRecord()` 调 `snapshot()` 给每条报警打戳。
`snapshot()` 用 `QReadWriteLock` 的读锁**按值返回**，函数返回即释放——调用方不得持锁跨数据库写。

**打戳时机是"产生时"，不是"发送/显示时"**：报警一落地就把当时的施工信息固化进记录，
之后退出登录、重新扫码、上行重传都不会改写它。所以退出登录后仍在队列里的那条重传，
带的仍是**它产生时**的负责人——这正是归属应有的语义。

## 依赖方向
`src/biz/alarm` → `src/biz/session` 是同层合法边；本模块**不 include 任何 `src/ui` 头**，
只用 Qt Core（`QString`/`QJsonObject`/`QJsonDocument`），因此可离线自测：
`tests/test_construction_qr.cpp`（11 条契约，含"解一张真码 → 登录"与"缺字段的码解得开但过不了校验"）。
