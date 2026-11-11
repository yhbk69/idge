# ladderguard — 登高守护规则引擎

## 功能概述

判定「梯上有人（攀爬者）且梯下无人看护（扶梯人）」的违规作业。
移植自用户提供的 `detect_ladderguard.py` / `登高守护实现方案.md`（原件收在 `docs/ladderguard/`），
纯逻辑实现：不依赖 Qt / OpenCV / RKNN，时间由调用方喂单调秒值，可离线单测。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `ladder_guard.h` | `Box`/`Params`/`State`/`GuardView` + `Tracker` 声明；几何与时间默认常量 |
| `ladder_guard.cpp` | 梯框聚合、攀爬者/扶梯人分类、三态状态机与跨帧 IoU 跟踪 |

## 核心数据流

```
解码线程 4.9c：按类名子串认槽位角色（含 ladder → 梯框槽；含 worker/person/people/pedestrian → 人框槽）
  → 槽循环里按 od.time 对齐收集两路框（时间戳不等不配对；count==0 也记时间戳）
解码线程 4.10c：mergeLadders(梯框) → Tracker::step(梯框, 人框, steady 秒)
  → 每把梯子一个 GuardView：画覆盖层（绿梯框 / 红橙攀爬者 / 蓝扶梯人）
  → enteredAlarm（进入 ALARM 的沿）才 ingestLadderAlarm → deliverAlarms（截图+片段）
```

规则要点（与参考实现同口径）：

- **几何阈值全是梯框自身 W/H 的比例**，与分辨率/机位/梯距无关：
  攀爬者 = 人框水平中心在梯框 ±0.2W 内且脚高于梯底 ≥0.2H；
  扶梯人 = 非攀爬者、中心到梯框最近边水平距 ≤0.8W 且脚与梯底基线差 ≤0.15H。
- **先判攀爬者再判扶梯人**：攀爬者绝不能自己当自己的看护，否则违规永远报不出。
- **重复梯框先聚合并集**（IoU≥0.1 或 交集/较小框面积≥0.5）：整梯框+上半段子框
  同时检出时，子框底边不是真梯底，不聚合会凭空多出违规。
- **三态状态机**：IDLE→CLIMBING（出现攀爬者）→ALARM（无人看护持续 `graceSec`）；
  ALARM→CLIMBING（扶梯人连续在场 `clearSec`）；攀爬者离开任何态都回 IDLE。
  报警只在进入 ALARM 的沿出一次，电平持续不重复报。
- **跨帧身份**：贪心最大 IoU（>0.3）匹配同一把梯子，一帧一个状态机只认领一次；
  未匹配的老状态机连续丢失超 `lostSec` 才销毁（短暂遮挡不丢已累计的宽限计时）。
  多梯独立计时独立告警。`reset()` 清状态机但 **id 计数不复零**（报警去重键含 guardId）。

## 使用方法

```cpp
ladderguard::Tracker tracker;                       // 默认 Params
tracker.setParams(p);                               // 运行期改宽限/消警秒数
auto views = tracker.step(ladderguard::mergeLadders(ladders), persons, nowSec);
for (const auto &v : views)
    if (v.enteredAlarm) { /* 报警：det 取 v.climbers[0]，guardId = v.id */ }
```

离线单测（无 Qt 依赖，合成框 + 假时间轴）：

```bash
cmake --build build/build_rk3588_linux --target test_ladder_guard
./build/build_rk3588_linux/test_ladder_guard        # rc=0 全过
```

## 依赖关系

- 被 `src/media/reader/ffmpeg_video_decoder.cpp`（4.9c/4.10c 段）调用，每解码器一个 `Tracker`；
- 报警走 `AlarmManager::ingestLadderAlarm`（类型 `ladder_no_spotter`，键 `通道:ladder:guardId`）；
- 配置段 `ladderguard`（enabled/graceSeconds/clearSeconds，见 `src/base/config/README.md`），
  设置页「登高守护」分组写入；解码线程逐帧现读，改动即时生效；
- 激活前提：同一通道级联里同时挂梯子模型（`yolo11n-ladder`）与人模型（ppe/coco 均可），
  角色按类名子串识别，**不新增槽位配置键**。

## 注意事项

- onnx 输入固定 640×640：参考方案实测 1080p 全图 imgsz=640 会漏梯上的人、1280 才稳，
  板端级联统一 640 letterbox，**梯上人漏检风险需现场验证**（见 `model/onnx/README.md`）。
- 两槽结果靠 `od.time` 相等对齐；`tryPop` 可能拿到旧一轮结果，时间戳不等即本帧不判定，
  不会拿错帧的人框配梯框。
- 停用（或凑不齐槽位）即 `reset()`：否则停用期间挂起的宽限计时器会在重新启用时立刻误报。