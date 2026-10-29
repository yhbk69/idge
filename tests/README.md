# tests

## 功能概述

C++ 测试源码目录。含五个测试程序：

- `test_database.cpp`：针对 `src/biz/db/` 的检测记录 DAO / 报警记录 DAO /
  DatabaseManager 做初始化、插入、批量插入、条件查询、统计、状态更新与维护
  （vacuum/过期清理）的黑盒断言测试，输出 `[PASS]/[FAIL]` 与通过/失败计数。
- `test_equipment.cpp`：设备盘点全链离线自测（无人值守板端验收），串起
  RollCallService 初始化(共库) → 盘点服务进程内模型加载(YOLO11Model) → 建任务 →
  processPhotos 检测计数 → 画框图落盘 → saveResult 落库 → getPhotos/getDetections
  回读一致性 → 热更新原子性（合法清单换模型 / 非法清单失败保持旧模型可检测）。
- `test_rollcall.cpp`：人员点名全链离线自测，先做**对照回归**（同一张 test.jpg
  经外部 `face_recognition` exe 与进程内 `InProcessFaceRecognizer` 推理，断言人脸数
  一致、按 IoU 同名配对后特征余弦 > 0.99——SCRFD/ArcFace 移植语义不变性守门），
  再走业务链：注册 processPhotos+saveTaskResult → 回读一致性 → 双图全局去重 →
  matchCancellation 一对一全命中 → confirmCancellation 落库回读。
- `test_latest_frame_slot.cpp`：`src/base/queue/latest_frame_slot.h` 的四条契约自检——
  代感知跳帧（慢消费者一次拿到最新帧、不重复消费同一代）、被持有输入不被提前回收、
  `close()` 能唤醒阻塞中的等待者并让它返回空、`reset()` 后不留旧游标残留。
  纯 STL + std::async，不依赖 Qt 与硬件，可在任意机器上跑。
- `test_channel_reload.cpp`：通道配置热更新的板端离线自测。驱动**真** `FFmpegVideoDecoder`
  （无 GUI）跑 `assets/test/1.mp4`，做 4 轮"两阶段停止 → `releaseTasks()` → 按新配置重建"，
  模型集合依次是 1 个 → 2 个 → 换 1 个 → 换回 2 个。断言三件事：`waitForStop()` 后
  `isLoopAlive()` 必须为 false；重建出的槽位数与模型路径与配置一一对上（解析
  `cascade slot ch …` 日志，生产代码没有 `tasks_` 访问器也不该有）；反复重开后线程数
  回到基线、RSS 增长有界。另外用 `updateSlotAlarmClasses()` 验证在线换报警类别的两条性质：
  形状相符才接受改写（不符必须拒绝，设置页据此分流"在线换 / 整路重开"），且写入一个
  不可能命中的类别名之后**仍要继续出帧**——这一步既确认没重连流，也确认那把
  `alarmClsMtx_` 没被用成长临界区把解码循环锁死；选不存在的类别名是为了整轮不产报警、
  不碰报警库。这条测试已经抓到过一个真回归：`start()` 里没有把
  对象线程归属搬回 GUI 线程，第二次 `moveToThread` 被 Qt 拒绝，重开的那路一帧都不出。

## 文件/子目录清单

| 文件 | 说明 |
|------|------|
| `test_database.cpp` | 数据库模块测试（4 组用例：初始化/检测/报警/维护），基于 Qt Core + 宏 `TEST_ASSERT` |
| `test_equipment.cpp` | 设备盘点全链离线自测（模型加载→检测计数→画框→落库→回读→热更新原子性），逐项 `[ok]/[FAIL]` |
| `test_rollcall.cpp` | 人员点名全链离线自测（exe 对照回归 + 注册/去重/注销匹配/落库回读），逐项 `[ok]/[FAIL]` |
| `test_latest_frame_slot.cpp` | LatestFrameSlot 单槽广播契约自检（跳帧/生命周期/close 唤醒/reset），末行打印一行 PASS 说明 |
| `test_channel_reload.cpp` | 通道热更新板端离线自测（停→releaseTasks→重建 ×4 轮，槽位集合/线程数/RSS 三项断言），须在 build 目录下跑 |

## 使用方法

五个测试都已接入根 `CMakeLists.txt`（各自一个 `add_executable`，随主程序一并构建）：

```bash
./build-linux.sh -t rk3588 -b Release
./build/build_rk3588_linux/test_database    # 退出码：失败数>0 时为 1，否则 0
./build/build_rk3588_linux/test_latest_frame_slot   # 同上；纯 CPU，不依赖板子外设

# 盘点/点名自测必须在仓库根运行（依赖 assets/test/ + model/library/yolo11n-coco +
# model/face/ 两件套权重；点名对照还需 model/face/face_recognition）；
# 直接跑主程序同目录产物即可（test_equipment 内部已 qputenv offscreen）
cd "$(git rev-parse --show-toplevel)"
./build/build_rk3588_linux/test_equipment   # 末行 PASS + rc=0 / FAIL + rc=1
./build/build_rk3588_linux/test_rollcall    # 同上
./build/build_rk3588_linux/test_channel_reload  # 同上；需 NPU/RGA，工作目录必须是构建目录（./model 软链）
```

主程序用 `./run.sh` 启动（它会 `cd` 进构建目录，所以"启动目录=工作区根"；构建目录下
的 `model`、`3rdparty` 两个软链由 CMake 在 configure 时创建，测试也按同样方式跑）。
若 Qt 装在非系统路径，需要自行 `export LD_LIBRARY_PATH=<Qt>/lib:$LD_LIBRARY_PATH`
——本仓库不再假设某个固定版本号或某台机器的绝对路径。

`test_database` 使用内存数据库（`DatabaseManager::instance().initialize(":memory:")`），不会写磁盘文件；
`test_equipment` 的 db 与任务目录全部落在 `/tmp/equip_check_<pid>/`、`test_rollcall` 落在
`/tmp/rollcall_check_<pid>/`（含 test.jpg 副本，隔离 exe 的 `_faces.json` 残留），跑完即删，不碰正式 `data/roll_call_data`。

## 依赖关系

- Qt5 Core / Qt5 Sql、`3rdparty/sqlite`、`3rdparty/jsoncpp`
- `src/biz/db/`（database_manager、detection_dao、alarm_dao）
- `test_equipment` 另链：盘点/点名服务全套（`src/biz/service`、`src/biz/db/business_db_manager`、
  `src/base/utils`、`src/ai/recognition`、`src/ai/model_repo`、`src/base/config`）+
  rknnrt / OpenCV / libturbojpeg / RGA（CMake 里用 `-Wl,--exclude-libs,ALL`
  阻止静态 turbojpeg 的 `jpeg_*` 符号劫持系统 OpenCV imgcodecs 的 libjpeg ABI）
- `test_rollcall` 另链：点名服务 + 两套识别器（`face_recognizer` 对照用 exe 链路与
  结构体、`in_process_face_recognizer` 被测），其余同 `test_equipment`
- `test_latest_frame_slot` 只链 `Threads::Threads`，include 路径仅 `src/base/queue`
- `test_channel_reload` 链的是主流水线的依赖闭包：`ffmpeg_video_decoder` + `video_recorder` +
  `ppe_task`/`base_task`/`helmet_task` + `model_registry` + `ConfigManager`/`parse_config` +
  DMA 缓冲 + `alarm_manager`/`alarm_dao`/`detection_dao`/`database_manager` + 围栏两件 +
  `src/base/utils`，库侧与主目标同一套（FFmpeg-rkmpp / rknnrt / rga / sqlite3 / jsoncpp / OpenCV /
  GLES/EGL），并同样带 `ZERO_COPY` 与 `--allow-shlib-undefined`；库搜索目录要单独
  `target_link_directories` 一遍——主目标那份是 target 级的，不会传给别的 target

## 注意事项

- **新增测试源必须同时在 `.gitignore` 加 `!tests/xxx.cpp`**：根 `.gitignore` 里有
  `test_*.cpp` 这条"临时验证文件"规则，会把正式测试源一并挡掉，而根 CMakeLists 已经
  引用它 —— 结果是本地能编、别人克隆后 `No such file` 编不过。五个测试目前都已放行。
- 断言基于 `src/biz/db/` 接口行为（如"批量插入10条"后 `queryByTimeRange` 期望 ≥11 条），
  改动 DAO 语义时需同步核对用例假设。
- 测试进程为单线程顺序执行，`testsPassed/testsFailed` 为全局计数，多线程化时需加锁。
- 注意与 `src/biz/db/business_db_manager`（业务库 roll_call.db）区分：本测试覆盖的是
  `src/biz/db/`（检测/报警库 idge.db）。
