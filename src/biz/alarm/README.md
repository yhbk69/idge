# src/biz/alarm

> 所属域：**biz 业务域**（目录重组 S4d 迁入）

## 功能概述
报警管理模块。负责把检测流水线的原始结果转成"报警"，完成去重限流判定、类别统计、
内存报警列表维护、通道在线状态跟踪，并通过 Qt 信号驱动界面刷新。

注意：本模块只负责**判定与产出内存态报警记录**（供 UI 展示），不直接写 SQLite、
不直接落盘抓拍图；持久化由外部 DB 路径与 SnapWriter 截图线程异步完成。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `alarm_manager.h` | `AlarmRecord` 结构体 + `AlarmManager` 单例声明、限流常量 |
| `alarm_manager.cpp` | `ingest()` 判定/限流、`storeAndNotify()` 存储与发信号、统计与查询 |

## 核心类与数据流
- **AlarmManager**（`QObject` 单例）：全局唯一。
- **AlarmRecord**：一条报警（时间戳、通道、类别、置信度、确认位、截图路径、是否围栏报警）。

数据流：
```
解码线程 → ingest(channel, results) → 生成 newAlarms(未存/未截图)
         → storeAndNotify(newAlarms) → 存入内存环形列表(≤1000)
         → emit alarmGenerated / statsUpdated → UI 更新
```
去重限流键：`"通道号:类别ID"`（如 `"0:0"` = 通道0 的 person），各通道各类别互不影响。
围栏报警传入 `bypassThrottle=true`，不走限流窗口。

## 使用方法
```cpp
// 解码线程中
QVector<AlarmRecord> news = AlarmManager::instance().ingest(ch, results);
if (!news.isEmpty()) AlarmManager::instance().storeAndNotify(news);

// UI 侧
AlarmManager::instance().setAlarmClasses({"person", "helmet"});
int unread = AlarmManager::instance().unacknowledgedCount();
AlarmManager::instance().acknowledgeAll();
connect(&AlarmManager::instance(), &AlarmManager::alarmGenerated, ...);
```

## 依赖关系
- `src/yolo11/common.hpp`：`object_detect_result(_list)` 检测结果类型、时间戳字段。
- `src/config/ConfigManager.h`：构造时读取 `alarm.classes` 缺省报警类别。
- Qt：`QMutex`/`QVector`/`QMap`/信号槽；`AlarmRecord` 经 `qRegisterMetaType` 支持跨线程。

## 注意事项
- **单位隐患（重要）**：`kAlarmThrottleNs` 按纳秒定义（2e9），而 `ingest()` 中比较的
  `results.time`（见 common.hpp 注释为"毫秒"）。若确为毫秒，限流窗口被放大约百万倍，
  "2 秒"实际不成立——依赖限流行为前务必核查/统一单位。围栏报警因 bypass 不受影响。
- 报警列表是内存 FIFO：超 1000 条 `removeFirst` 淘汰最旧，`removeFirst` 为 O(n) 搬移。
- 线程安全：内部用 `QMutex` 保护；截图开关用 `std::atomic<bool>`。
- `acknowledged` 为纯内存确认位，重启即丢；截图 `imgPath` 由外部异步回填，可能短暂为空。
