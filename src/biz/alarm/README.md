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
| `alarm_manager.h` | `AlarmRecord` 结构体 + `AlarmManager` 单例声明、限流常量、`SlotContext` |
| `alarm_manager.cpp` | `ingest()`/`ingestFenceAlarm()` 判定与限流、`storeAndNotify()` 存储与发信号、`attachVideoClip()` 片段回填、`cleanOldData()` 数据与媒体清理 |

## 核心类与数据流
- **AlarmManager**（`QObject` 单例）：全局唯一。
- **AlarmRecord**：一条报警（时间戳、通道、类别、置信度、确认位、截图路径、视频片段路径、是否围栏报警）。
  围栏报警的 `alarmType` 是 `fence_in`/`fence_out`（闯入禁区/离岗），普通检测报警是 `"detection"`。

数据流：
```
解码线程（每帧 × 每个级联槽位）
  → ingest(channel, od, SlotContext{alarmClass, classNames})   ← 上下文由解码器按通道局部组装，栈上传入
  → 生成 newAlarms(未存/未截图)
  → 收集本帧围栏候选（类别 == geofence.alarmClass 的目标）
  → 槽循环结束后一次通道级合并判定：ingestFenceAlarm(channel, det, ctx, timeNs, role)
  → storeAndNotify(newAlarms) → 存入内存环形列表(≤1000) + AlarmDAO::insertAlarms 落库
  → emit alarmGenerated / statsUpdated → UI 更新
  → (解码侧另投 ClipWriter 任务)

ClipWriter 后台线程(报警 +2s 到点) → dump 裸流/MP4 → attachVideoClip(ids, 裸流路径, mp4 副本路径)
         → 改内存 alarms_ 中同批记录的 videoPath + AlarmDAO::updateVideoPath 落库
         → emit videoClipAttached(ids, 裸流路径, mp4 副本路径)（**当前只有报警上行 url 档是消费者**）
         （UI 不发信号：详情对话框打开时读 alarms() 自然拿到最新路径）
```
两条判定路径的限流键与窗口**互相独立**：

| 路径 | 键 | 窗口 |
| --- | --- | --- |
| 普通报警 `ingest` | `"通道:槽下标:cls_id"`（如 `"0:1:0"`） | `kAlarmThrottleNs` = 2 秒 |
| 围栏报警 `ingestFenceAlarm` | `"通道:fence:角色:类别名"`（如 `"0:fence:in:person"`、`"0:fence:out:person"`） | `geofence.throttleSeconds`，默认 10 秒 |

普通报警键里带槽下标，不同级联槽恰好同类号不再互相压制；围栏键刻意**用类名而不是槽号**，
配置重排槽位顺序后限流窗口依然稳定。
**围栏键里还必须带角色段**：少了它，一个人闯禁区后 10 秒内离开岗区域，那条离岗会被同一个窗口吞掉。
围栏报警的 `alarmType` 按角色分成 `fence_in`（闯入禁区）与 `fence_out`（离岗），
下游（列表 Tab、DB 回读、WS 上行的 `alarm_type`）靠 `startsWith("fence")` **前缀**认围栏——
历史数据存的是裸 `"fence"`，改成精确相等会让老围栏报警重启后掉进「类别」Tab。
**围栏的"只报一次"不是这个窗口给的**：一个人站在围栏里只出一条报警，靠的是解码器里的
进入沿判定（`FFmpegVideoDecoder::fenceTracks_` 用 IoU 把同一目标的连续命中归并成一次事件，
离开 1.5 秒后事件结束、再进来算新事件；**事件表按角色分池，匹配前先比角色**，
否则闯入 track 会把同一个人的离岗命中续期掉、离岗永久静默）。`throttleSeconds` 是它之下的兜底——
进入沿抖动、遮挡后重匹配、或两人几乎同时进入时，每通道每角色每类别窗口内只留第一条。
（历史：早期实现寄生在级联里且无限流，40 秒出 200~340 条；改成节流窗口后仍是"每 10 秒一条"的电平语义，
人不动就一直报。`bypassThrottle` 形参已删除，不存在"豁免限流"的调用路径。）

## 使用方法
```cpp
// 解码线程中：上下文每帧按通道局部组装一次，槽循环内逐槽传入
AlarmManager::SlotContext ctx;                 // alarmClass 空 = 该模型只画框不报警
ctx.alarmClass = slotAlarmClasses_[k];
for (const auto &n : slotClassNames_[k]) ctx.classNames << QString::fromStdString(n);

QVector<AlarmRecord> news = AlarmManager::instance().ingest(ch, od, ctx);
if (!news.isEmpty()) {
    AlarmManager::instance().storeAndNotify(news);
    // 片段在报警后 2 秒才生成，路径由 ClipWriter 落盘后回填，此处不要读 videoPath
}

// 围栏判定（通道级，槽循环结束后一次；几何判定出违规才调，role 来自 FenceViolation）
alarms += AlarmManager::instance().ingestFenceAlarm(ch, det, ctx, timeNs, v.role);

// ClipWriter 后台线程（片段写完后；mp4 副本没写成时第三参传空串）
AlarmManager::instance().attachVideoClip(ids, rawClipPath, mp4Path);

// UI 侧
int unread = AlarmManager::instance().unacknowledgedCount();
AlarmManager::instance().acknowledgeAll();
connect(&AlarmManager::instance(), &AlarmManager::alarmGenerated, ...);

// 退出时（src/main.cpp）
AlarmManager::instance().cleanOldData(30);
```

## 依赖关系
- `src/ai/yolo11/common.hpp`：`object_detect_result(_list)` 检测结果类型、时间戳字段。
- `src/base/config/ConfigManager.h`：`ingestFenceAlarm` 读 `geofence.throttleSeconds`
  （本模块已不持有任何类别名表，也不再读旧的 `alarm.classes`）。
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
- **`attachVideoClip` 的第三参（`.mp4` 副本）只用于转发、不入库**：`alarms.video_path` 与内存态的
  `AlarmRecord::videoPath` 至今存的仍是 Annex-B 裸流路径，详情对话框的"视频"行读的也是它。
  入库口径改成 mp4 会让历史数据的关联错位，也让国标/上传那套按裸流写的注释全部失真，所以不改。
  mp4 目前只有一个去处：**报警上行 url 档申报的那个地址**（浏览器/播放器打不开裸流，这才需要副本）。
- `acknowledged`/`videoPath` 为内存态，重启后以 DB 为准；截图 `imgPath` 由外部异步回填，可能短暂为空。
- **`SlotContext` 没有拥有权也不共享**：由解码器每帧在栈上构造、按槽传入，`AlarmManager` 只在调用期间
  读取，不缓存。因此 4 个解码器并发报警不会互相覆盖类别名表（旧实现把全局槽表写进单例，
  按通道配置后通道 3 会覆盖通道 0 的表，报警判定串频道）。
- `detections.frame_id` 存的是**产出该目标的槽下标**；围栏报警走 `ingestFenceAlarm`，
  该列固定为 `-1`（围栏是通道级判定，不属于任何槽位）。
- `ingestFenceAlarm` **不重复累计**统计（`totalDetections_`/`classCount_` 已由同帧的 `ingest` 记过），
  它只出记录与明细。
