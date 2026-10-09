# src/biz/alarm

> 所属域：**biz 业务域**（目录重组 S4d 迁入）

## 功能概述
报警管理模块。负责把检测流水线的原始结果转成"报警"，完成去重限流判定、类别统计、
内存报警列表维护、通道在线状态跟踪，并通过 Qt 信号驱动界面刷新。

注意：本模块负责**判定、内存态报警列表与持久化编排**。它不自己拼 SQL，也不直接落盘
媒体文件——写库统一走 `AlarmDAO`（`src/biz/db`），抓拍图由解码侧的 `SnapWriter`、
视频片段由 `ClipWriter`（`src/media/reader`）异步生成后回填路径。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `alarm_manager.h` | `AlarmRecord` 结构体 + `AlarmManager` 单例声明、限流常量 |
| `alarm_manager.cpp` | `ingest()` 判定/限流、`storeAndNotify()` 存储与发信号、`attachVideoClip()` 片段回填、`cleanOldData()` 数据与媒体清理 |

## 核心类与数据流
- **AlarmManager**（`QObject` 单例）：全局唯一。
- **AlarmRecord**：一条报警（时间戳、通道、类别、置信度、确认位、截图路径、视频片段路径、是否围栏报警）。

数据流：
```
解码线程 → ingest(channel, results) → 生成 newAlarms(未存/未截图)
         → storeAndNotify(newAlarms) → 存入内存环形列表(≤1000) + AlarmDAO::insertAlarms 落库
         → emit alarmGenerated / statsUpdated → UI 更新
         → (解码侧另投 ClipWriter 任务)

ClipWriter 后台线程(报警 +2s 到点) → dump 裸流/MP4 → attachVideoClip(ids, 裸流路径)
         → 改内存 alarms_ 中同批记录的 videoPath + AlarmDAO::updateVideoPath 落库
         （不发信号：详情对话框打开时读 alarms() 自然拿到最新路径）
```
去重限流键：`"通道号:槽位:类别ID"`（如 `"0:1:0"` = 通道 0、级联第 2 槽的 cls_0），
不同级联槽恰好同类号不再互相压制。
`bypassThrottle` 形参**当前所有调用方都传 `false`**，围栏报警同样受 2 秒窗口限流；
若要围栏豁免需调用方显式传 `true`。

## 使用方法
```cpp
// 解码线程中
QVector<AlarmRecord> news = AlarmManager::instance().ingest(ch, results);
if (!news.isEmpty()) {
    AlarmManager::instance().storeAndNotify(news);
    // 片段在报警后 2 秒才生成，路径由 ClipWriter 落盘后回填，此处不要读 videoPath
}

// ClipWriter 后台线程（片段写完后）
AlarmManager::instance().attachVideoClip(ids, rawClipPath);

// UI 侧
AlarmManager::instance().setAlarmClasses({"person", "helmet"});
int unread = AlarmManager::instance().unacknowledgedCount();
AlarmManager::instance().acknowledgeAll();
connect(&AlarmManager::instance(), &AlarmManager::alarmGenerated, ...);

// 退出时（src/main.cpp）
AlarmManager::instance().cleanOldData(30);
```

## 依赖关系
- `src/ai/yolo11/common.hpp`：`object_detect_result(_list)` 检测结果类型、时间戳字段。
- `src/base/config/ConfigManager.h`：构造时读取 `alarm.classes` 缺省报警类别。
- `src/biz/db/alarm_dao.h`：`insertAlarms` / `updateVideoPath` / `cleanOldAlarms`（本模块唯一的写库出口）。
- `src/base/runtime_paths.h`：`alarmsDir()` —— 抓拍图与视频片段共用的媒体根目录。
- Qt：`QMutex`/`QVector`/`QMap`/信号槽；`AlarmRecord` 经 `qRegisterMetaType` 支持跨线程。

## 注意事项
- **时钟源**：限流窗口比较用 `steady_clock` 纳秒（`kAlarmThrottleNs = 2e9`），`lastAlarmTime_`
  存的也是 steady 纳秒。早期实现用 `results.time`（墙钟纳秒）相减，NTP 回拨会让差值变负、
  报警可被抑制数天；注释里"time 为毫秒"是文档误差，量纲本就一致。展示用的
  `timestamp/alarmTime` 仍由 `results.time/1e6` 换算成毫秒，并对非法值兜底为当前墙钟。
- **清理是双闸门**：`cleanOldData(days)` 先 `pruneAlarmMedia()` 再清库。媒体侧只认识
  `alarmsDir()/yyyyMMdd` 这种可解析为日期的目录（其它文件一律不碰），先按天数删，删完仍
  超过 `4 GB` 才从最旧目录继续删。4 GB 目前是常量而非配置项。
- **DB 与文件不保证同步消失**：`cleanOldAlarms` 与 `pruneAlarmMedia` 各自独立执行，中途失败
  会留下"有记录无文件"的报警；详情对话框对这种片段显示"已清理"。
- 报警列表是内存 FIFO：超 1000 条 `removeFirst` 淘汰最旧，`removeFirst` 为 O(n) 搬移。
  若某条报警已被挤出内存后才生成片段，内存侧改不到，但 DB 的 `video_path` 仍会更新。
- 线程安全：内部用 `QMutex` 保护；截图开关用 `std::atomic<bool>`。`attachVideoClip` 先解锁
  再写库；`DatabaseManager` 每线程独立连接，故后台线程 `new AlarmDAO` 安全。
- `acknowledged`/`videoPath` 为内存态，重启后以 DB 为准；截图 `imgPath` 由外部异步回填，可能短暂为空。
