# src/biz/geofence

> 所属域：**biz 业务域**（目录重组 S4d 迁入）

## 功能概述
电子围栏模块。支持在视频 overlay 上绘制矩形/多边形围栏，**每个形状自带角色**
（`forbidden` 禁区 / `workzone` 岗区域），并判断目标是"闯入了禁区"还是"离开了岗区域"。

**围栏不属于级联**：它不自带模型、不占推理任务，而是复用该通道已有模型产出的检测结果，
只做几何判定。触发类别是**单值** `geofence.alarmClass`（默认 `person`），
不是模型参数、也不是列表——围栏问的是"这一路里是不是人"。

**禁区与岗区域可以在同一通道混用**，各自的归约方式不同（这不是一个开关的两种取值）：
- 禁区 = **命中任意一个**就报（OR，逐形状判定，返回命中的 `shapeIndex`）；
- 岗区域 = **在所有岗区域之外**才报（AND，`shapeIndex` 恒为 -1，没有单一责任形状）。
  推论：人在**任一**岗区域内即不算离岗，即使他同时站在第二个岗区域之外。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `fence_shape.h` | 几何数据结构：`Point`/`Rectangle`/`ShapeType`/`FenceRole`/`FenceShape`/`ChannelFence`；`roleFromString`/`roleToString` 是无 Qt 依赖的小 helper（迁移与离线自测共用） |
| `fence_checker.h/.cpp` | 无状态算法：射线法命中、坐标映射、`checkDetection` 返回 `FenceViolation`（`None`/`Intrusion`/`Absence`） |
| `fence_manager.h/.cpp` | 单例数据管理：config.json 读写、per-channel 围栏与 overlay 尺寸 |
| `fence_overlay.h/.cpp` | 透明覆盖层 QWidget：绘制与鼠标交互（矩形/多边形/删除），按角色配色与 `setRole` |

## 核心类与数据流
- **FenceChecker**（全静态方法）：
  - `pointInPolygon`：射线法（crossing number），O(n)，支持凹多边形。
  - `isInsideFence`：取检测框**底边中点**（脚点，帧坐标）→ 按 letterbox 规则映射到
    widget 坐标 → 与围栏（widget 坐标）比较。
  - `checkDetection`：单遍扫描该通道所有形状，裁决顺序是**禁区优先**：
    1. 命中任一禁区 → `{Intrusion, Forbidden, 该形状下标}` 并立即返回；
    2. 否则该通道有岗区域且一个人都不在 → `{Absence, Workzone, -1}`；
    3. 否则 `None`。
    禁区优先的理由：禁区命中是正向几何事实，离岗是对整张形状表的负向推断；
    同一具身体同时满足两个条件时发两条，会把一次事件拆成两次处置。
    第 2 步的"该通道有岗区域"是**必需的守卫**——少了它，纯禁区通道会把每个人每帧都判离岗。
- **FenceManager**（单例）：`loadFromConfig`/`saveToConfig` 与 config.json `geofence` 段互转；
  `channelFence(ch)` 按值返回快照供解码线程只读使用；`alarmClass()` 是单值（旧 `alarmClasses()`
  列表与忽略入参、硬编码 `{"person"}` 的假 `setAlarmClasses()` 已删除）。
  **老配置迁移在 `loadFromConfig` 里做**，不在 `ConfigManager::migrateSchemaV2()`：后者是一次性闸门、
  板端已跑过、且不触碰 `channels/*/shapes`，重新武装它会误伤现场围栏。规则：形状有 `role` 键就以它为准，
  没有就按当时的 `geofence.mode` 推断（`outside_alarm`→岗区域，其余→禁区）并置脏标记，
  加载末尾落盘一次 → 幂等，第二次加载走显式分支，此后 `mode` 再也影响不到角色。
- **FenceOverlay**：绘制/交互，保存时经 `saveFences()` 把围栏顶点、角色与当前 widget 尺寸一并落盘。
  `setRole()` 只存值 + `update()`，**不清空**在画的多边形顶点（`setDrawMode` 会清，切角色不该丢顶点）。
  配色按角色：禁区红、岗区域绿；正在画的临时图形用**当前所选角色**的颜色，所见即所得。

数据流：
```
绘制：工具栏选角色 → FenceOverlay(鼠标，提交时盖 role) → saveFences → FenceManager → saveToConfig(config.json)
判定：解码线程（每帧，槽循环结束后一次，通道级而非槽位级）
      → 收集类别 == alarmClass 的检测候选
      → FenceManager::channelFence(ch)(快照) → FenceChecker::checkDetection → FenceViolation
      → 进入沿判定（IoU 事件表按角色分池，在解码器里，不在本模块）
      → AlarmManager::ingestFenceAlarm(..., role)（节流键含角色，下限 geofence.throttleSeconds）
```

## 报警语义：按角色判定 + 进入沿，不是电平

**两种角色各自的"报一次"含义：**
- 禁区：**一个人走进禁区 → 报 1 条「闯入禁区」；站在里面不动 → 不再报；离开 1.5 秒后再进 → 再报 1 条。**
- 岗区域：**检到一个人、而他在所有岗区域之外 → 报 1 条「离岗」；一直在外面站着 → 静默；
  回到岗区域内待 >1.5 秒再出来 → 再报 1 条。**
  注意离岗**仍以"检到一个人"为前提**——没人 = 岗位无人，本轮明确不做，别把它读成空岗告警。

事件状态由 `FFmpegVideoDecoder::fenceTracks_` 维护（每通道一份、只被该通道解码线程访问）：
违规目标与进行中的事件做 IoU 匹配（阈值 0.3，比绘图平滑的 0.2 严——这里误匹配会把
两次独立入侵并成一条报警），匹配上只续期不报警；匹配不上即"新事件"，报警并建事件；
连续 1.5 秒没被命中就认为事件结束。**匹配前先比角色、不同角色直接跳过**：
漏掉这一步，同一个人的闯入 track 会把他随后的离岗命中"续期"掉，离岗永久静默——
这是最难查的失效方向，而且 track 数量按角色翻倍是预期的。

`geofence.throttleSeconds`（默认 10 秒）退到事件之下做**兜底下限**，节流键是
`通道:角色:类别`（`alarmType` 的 `fence_in`/`fence_out` 同源），所以**闯入后 10 秒内的离岗不会被吞**——
键里少了角色段就会互相吞。想"每次进入都单独留档"把窗口调小。
调试页那行围栏日志会把"候选/违规(侵入x/离岗y)/新事件/报警条数"并排打出来，
报警数 < 违规数就说明被下限兜住了。

本模块只提供几何判定与形状/角色/开关/类别，**不参与事件跟踪**。
`geofence.mode` 已退役（唯一读者是 `loadFromConfig` 的迁移分支），运行期判断一律看形状角色。

## 使用方法
```cpp
FenceManager::instance().loadFromConfig();
FenceShape s; s.type = ShapeType::Polygon;
s.role   = geofence::FenceRole::Workzone;        // 不给就是禁区（安全的失效方向）
s.points = {{10,10},{200,10},{100,150}};
FenceManager::instance().addShape(ch, s);
FenceManager::instance().saveToConfig();

auto v = FenceChecker::checkDetection(det, srcW, srcH, modelW, modelH, ovW, ovH, cf);
if (v.verdict != geofence::FenceVerdict::None) { /* v.role / v.shapeIndex 决定报什么、报哪个 */ }
```
`tests/test_geofence_role.cpp` 是这套裁决的离线契约自检（8 组断言，无 GUI 依赖）。

## 依赖关系
- `src/ai/yolo11/common.hpp`：`object_detect_result`/`image_rect_t`。
- `src/base/config/ConfigManager.h`：`geofence` 配置段读写。
- Qt Widgets：`QWidget`/`QPainter`/`QPainterPath`（overlay 绘制）。

## 注意事项
- **坐标系约定（易错点）**：围栏顶点保存的是 **overlay/widget 像素坐标**，检测时被
  变换方向是"**帧坐标→widget 坐标**"（脚点），围栏本身**不参与**坐标变换。
- **letterbox 依赖 overlay 尺寸**：`saveFences` 与围栏配对保存绘制时的 `width()/height()`；
  窗口缩放后未重新保存会导致整体偏移。`overlayW/H<=0` 时退化为直接用帧坐标比较（精度下降）。
  角色引入后这个偏移的代价变大了：偏移在禁区一侧是**漏报闯入**，在岗区域一侧还多会**误报离岗**
  （人实际在岗区域内，映射后落到外面）。本轮只记录、不改值。
- 顶点数下限：矩形 2 个对角点（顺序不定，判定前 `toRect` 归一化）；多边形 ≥3，否则判为不在内部。
- **角色创建后不可改**：形状没有稳定 id，删除模式按下标命中，所以 UI 上只能在画之前选角色，
  **改角色 = 删了重画**。默认 `forbidden`（禁区）——解析不出角色键时按"要报警"处理是更安全的失效方向。
- 有围栏数据时 `loadFromConfig` 会自动把总开关置 true。`ChannelFence.enabled` 仍是**通道级**：
  混用两种角色的通道无法只关其中一种。
- FenceManager 无锁，约定"主线程写、解码线程读快照"，跨线程只可经 `channelFence()` 取副本。
- **围栏静默不触发的最常见原因**：该通道挂的模型产不出 `alarmClass`（例如只挂了 PPE 模型，
  标签里没有 `person`）。`FFmpegVideoDecoder::buildCascadeTasks()` 在启动时会打一条
  `[geofence] 通道N 无模型产出类别 person，围栏不会触发`，先看日志别改几何。
  对岗区域这条尤其致命：候选为空时连"人在岗区域外"都无从判定，**整条离岗规则静默失效**。
- **围栏报警的限流与事件跟踪都不在本模块**：`geofence.throttleSeconds`（默认 10 秒）由
  `AlarmManager::ingestFenceAlarm()` 读取执行，"同一个人同一角色只报一次"由解码器的进入沿事件表决定；
  FenceManager 只管形状、角色、开关与类别。
  旧实现借用级联报警路径且无限流（每帧一条），后来加了节流但仍是电平语义（人不动就一直报），现已改为进入沿。
- **`isFenceAlarm` 靠 `alarmType` 前缀恢复**（`alarm_dao.cpp` 读回时判 `startsWith("fence")`）：
  老数据存的是裸 `"fence"`，新数据是 `fence_in`/`fence_out`。改成精确相等会让历史围栏报警
  在重启后掉进「类别」Tab 显示成"person 检测到报警"。
