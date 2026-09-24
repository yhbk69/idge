# src/biz/geofence

> 所属域：**biz 业务域**（目录重组 S4d 迁入）

## 功能概述
电子围栏模块。支持在视频 overlay 上绘制矩形/多边形围栏，配置 per-channel 的
inside/outside 报警模式，并在检测阶段判断目标（人）是否侵入/离开围栏。

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
  `channelFence(ch)` 按值返回快照供解码线程只读使用。
- **FenceOverlay**：绘制/交互，保存时经 `saveFences()` 把围栏顶点与当前 widget 尺寸一并落盘。

数据流：
```
绘制：FenceOverlay(鼠标) → saveFences → FenceManager → saveToConfig(config.json)
检测：解码线程 → FenceManager::channelFence(ch)(快照) → FenceChecker::checkDetection → 报警
```

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
- `src/yolo11/common.hpp`：`object_detect_result`/`image_rect_t`。
- `src/config/ConfigManager.h`：`geofence` 配置段读写。
- Qt Widgets：`QWidget`/`QPainter`/`QPainterPath`（overlay 绘制）。

## 注意事项
- **坐标系约定（易错点）**：围栏顶点保存的是 **overlay/widget 像素坐标**，检测时被
  变换方向是"**帧坐标→widget 坐标**"（脚点），围栏本身**不参与**坐标变换。
- **letterbox 依赖 overlay 尺寸**：`saveFences` 与围栏配对保存绘制时的 `width()/height()`；
  窗口缩放后未重新保存会导致整体偏移。`overlayW/H<=0` 时退化为直接用帧坐标比较（精度下降）。
- 顶点数下限：矩形 2 个对角点（顺序不定，判定前 `toRect` 归一化）；多边形 ≥3，否则判为不在内部。
- 模式默认 `inside_alarm`；有围栏数据时 `loadFromConfig` 会自动把全局开关置 true。
- FenceManager 无锁，约定"主线程写、解码线程读快照"，跨线程只可经 `channelFence()` 取副本。
