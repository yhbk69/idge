# src/biz/geofence

> 所属域：**biz 业务域**（目录重组 S4d 迁入）

## 功能概述
电子围栏模块。支持在视频 overlay 上绘制矩形/多边形围栏，配置 per-channel 的
inside/outside 报警模式，并判断目标是否侵入/离开围栏。

**围栏不属于级联**：它不自带模型、不占推理任务，而是复用该通道已有模型产出的检测结果，
只做几何判定。触发类别是**单值** `geofence.alarmClass`（默认 `person`），
不是模型参数、也不是列表——围栏问的是"这一路里是不是人"。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `fence_shape.h` | 几何数据结构：`Point`/`Rectangle`/`ShapeType`/`FenceShape`/`ChannelFence` |
| `fence_checker.h/.cpp` | 无状态算法：射线法命中、坐标映射、`checkDetection` 违规判定 |
| `fence_manager.h/.cpp` | 单例数据管理：config.json 读写、per-channel 围栏与 overlay 尺寸 |
| `fence_overlay.h/.cpp` | 透明覆盖层 QWidget：绘制与鼠标交互（矩形/多边形/删除） |

## 核心类与数据流
- **FenceChecker**（全静态方法）：
  - `pointInPolygon`：射线法（crossing number），O(n)，支持凹多边形。
  - `isInsideFence`：取检测框**底边中点**（脚点，帧坐标）→ 按 letterbox 规则映射到
    widget 坐标 → 与围栏（widget 坐标）比较。
  - `checkDetection`：遍历该通道所有形状，任一命中即 insideAny；再按模式决定报警：
    `inside_alarm`→insideAny 报警；`outside_alarm`→!insideAny 报警。
- **FenceManager**（单例）：`loadFromConfig`/`saveToConfig` 与 config.json `geofence` 段互转；
  `channelFence(ch)` 按值返回快照供解码线程只读使用；`alarmClass()` 是单值（旧 `alarmClasses()`
  列表与忽略入参、硬编码 `{"person"}` 的假 `setAlarmClasses()` 已删除）。
- **FenceOverlay**：绘制/交互，保存时经 `saveFences()` 把围栏顶点与当前 widget 尺寸一并落盘。

数据流：
```
绘制：FenceOverlay(鼠标) → saveFences → FenceManager → saveToConfig(config.json)
判定：解码线程（每帧，槽循环结束后一次，通道级而非槽位级）
      → 收集类别 == alarmClass 的检测候选
      → FenceManager::channelFence(ch)(快照) → FenceChecker::checkDetection
      → 进入沿判定（IoU 事件表，在解码器里，不在本模块）
      → AlarmManager::ingestFenceAlarm()（节流下限 geofence.throttleSeconds）
```

## 报警语义：进入沿，不是电平

**一个人走进围栏 → 报 1 条；站在里面不动 → 不再报；走出去 1.5 秒后再进来 → 再报 1 条。**
事件状态由 `FFmpegVideoDecoder::fenceTracks_` 维护（每通道一份、只被该通道解码线程访问）：
命中围栏的目标与进行中的事件做 IoU 匹配（阈值 0.3，比绘图平滑的 0.2 严——这里误匹配会把
两次独立入侵并成一条报警），匹配上只续期不报警；匹配不上即"新进入"，报警并建事件；
连续 1.5 秒没被命中就认为人已离开、事件结束。

`geofence.throttleSeconds`（默认 10 秒）退到事件之下做**兜底下限**：进入沿抖动、短暂遮挡后
重匹配、或两个人几乎同时进入时，每通道每类别窗口内只留第一条。想"每次进入都单独留档"就把它调小。
调试页那行围栏日志会把"栏内 N 个 / 新进入 M 个 / 报警 K 条"并排打出来，K<M 就说明被下限兜住了。

本模块只提供几何判定与形状/开关/模式/类别，**不参与事件跟踪**。

## 使用方法
```cpp
FenceManager::instance().loadFromConfig();
FenceShape s; s.type = ShapeType::Polygon;
s.points = {{10,10},{200,10},{100,150}};
FenceManager::instance().addShape(ch, s);
FenceManager::instance().saveToConfig();

bool alarm = FenceChecker::checkDetection(det, srcW, srcH, modelW, modelH,
                                          ovW, ovH, cf, /*alarmInside=*/true);
```

## 依赖关系
- `src/ai/yolo11/common.hpp`：`object_detect_result`/`image_rect_t`。
- `src/base/config/ConfigManager.h`：`geofence` 配置段读写。
- Qt Widgets：`QWidget`/`QPainter`/`QPainterPath`（overlay 绘制）。

## 注意事项
- **坐标系约定（易错点）**：围栏顶点保存的是 **overlay/widget 像素坐标**，检测时被
  变换方向是"**帧坐标→widget 坐标**"（脚点），围栏本身**不参与**坐标变换。
- **letterbox 依赖 overlay 尺寸**：`saveFences` 与围栏配对保存绘制时的 `width()/height()`；
  窗口缩放后未重新保存会导致整体偏移。`overlayW/H<=0` 时退化为直接用帧坐标比较（精度下降）。
- 顶点数下限：矩形 2 个对角点（顺序不定，判定前 `toRect` 归一化）；多边形 ≥3，否则判为不在内部。
- 模式默认 `inside_alarm`；有围栏数据时 `loadFromConfig` 会自动把全局开关置 true。
- FenceManager 无锁，约定"主线程写、解码线程读快照"，跨线程只可经 `channelFence()` 取副本。
- **围栏静默不触发的最常见原因**：该通道挂的模型产不出 `alarmClass`（例如只挂了 PPE 模型，
  标签里没有 `person`）。`FFmpegVideoDecoder::buildCascadeTasks()` 在启动时会打一条
  `[geofence] 通道N 无模型产出类别 person，围栏不会触发`，先看日志别改几何。
- **围栏报警的限流与事件跟踪都不在本模块**：`geofence.throttleSeconds`（默认 10 秒）由
  `AlarmManager::ingestFenceAlarm()` 读取执行，"同一个人只报一次"由解码器的进入沿事件表决定；
  FenceManager 只管形状、开关、模式与类别。
  旧实现借用级联报警路径且无限流（每帧一条），后来加了节流但仍是电平语义（人不动就一直报），现已改为进入沿。
