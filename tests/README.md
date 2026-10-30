# tests

## 功能概述

C++ 测试源码目录。含七个测试程序与一个人工联调工具：

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
- `test_alarm_uplink.cpp`：报警上行（`src/biz/ws/alarm_uplink_engine`）的**离线契约自检**，
  用 `FakeTransport` 顶掉 `WsClient`，所以不链 WebSockets、不建线程、不要 GUI，纯 `Qt5::Core`。
  十八组断言覆盖四类事：①`alarm_info` 报文形状（两种 `media_transport` 各一套字段）；
  ②`ws_binary` 档的 `media_begin`/分块/`media_end` 帧序与游标；③`url` 档只发地址——
  `transfer:"http"` + `url`、**不产出 `media_end`**、`mediaUrlFor()` 的路径逃逸（根外 / 含 `..`
  必须返回空串）与尾斜杠拼接，以及"相对 `imagePath` 必须先按 cwd 绝对化再换算"（这条抓到过板端
  真 404：`QDir(root).relativeFilePath(相对路径)` 是按 root 解析的，不绝对化就得到双重前缀）；
  ④队列语义——水位闸门、上限丢最旧、`uplink_ack` 只对账队首、退避梯子 `min(2^n×2s, 30s)`、
  断线时帧不越序、`media_failed` 的各成因。
  **其中 `ws_binary` 那 11 组是"代码保留但默认关"的回归保护，改协议时不许顺手删。**
- `test_geofence_role.cpp`：电子围栏**按形状角色**判定的离线契约自检（`FenceChecker::checkDetection`
  返回 `FenceViolation`）。八组断言：禁区命中优先于离岗（一人一帧一条）、**通道无岗区域时永不出
  `Absence`**（少了这条守卫，纯禁区通道会把每个人每帧都判离岗）、人在任一岗区域内即不算离岗、
  离岗的 `shapeIndex` 恒为 -1、多禁区时命中下标正确、禁用/空形状出 `None`、凹多边形命中与迁移前
  逐位一致（守 letterbox + 射线法，同时证明多边形没被降级成 `toRect`）、
  `roleFromString` 对缺键/空串/垃圾一律回落 `forbidden`（迁移的安全失效方向）。
  只链 `Qt5::Core`、无 GUI、与 cwd 无关，任意机器可跑。
- `test_construction_qr.cpp`：扫码登录门的**全部契约**离线自检（11 组）。施工信息的
  解析/校验/会话状态机（`ConstructionInfo::fromJson`/`fromQrText` + `ConstructionSession`）
  加上真解一张码图：用 `segno` 生成含中文的 PNG → `qrcode::scanQrFromImageFile()` → 喂给
  与登录页完全同一条 `startLogin()` 路径。另钉住三条易塌的口径：失败**不动**会话（不存在半登录）、
  `toJson()` 往返保留未知键（字段没定稿，加字段不许吃掉多余键）、图片读不到时给**原因**而不是
  返回空列表（空列表与"没有码"同形，会把路径错判成码印坏）。
  只链 `Qt5::Core` + `Qt5::Gui` + zbar，不开摄像头、不起 GUI 线程。
- `ws_sink.cpp`：**不是自检，是人工对拍的接收端**（`QWebSocketServer`）。板子出网受限，联调只能走
  loopback：`./ws_sink 8765 /tmp/sink_out` 起一个假对端，把 `websocket.server_url` 指到
  `ws://127.0.0.1:8765`。`ws_binary` 档它按 `media_id` 把二进制分块拼回文件并与报文里的 sha256 对拍；
  `url` 档只打印 `media_begin` 给出的地址/字节数/哈希、一个字节都不落盘——那种模式下"上传成功"的
  判据是拿这条 URL `curl` 下来的字节与盘上文件 `sha256sum` 一致。两种档都会把 `alarm_info`、
  `media_failed`、`ack` 原样打成一行行日志供人读。

## 文件/子目录清单

| 文件 | 说明 |
|------|------|
| `test_database.cpp` | 数据库模块测试（4 组用例：初始化/检测/报警/维护），基于 Qt Core + 宏 `TEST_ASSERT` |
| `test_equipment.cpp` | 设备盘点全链离线自测（模型加载→检测计数→画框→落库→回读→热更新原子性），逐项 `[ok]/[FAIL]` |
| `test_rollcall.cpp` | 人员点名全链离线自测（exe 对照回归 + 注册/去重/注销匹配/落库回读），逐项 `[ok]/[FAIL]` |
| `test_latest_frame_slot.cpp` | LatestFrameSlot 单槽广播契约自检（跳帧/生命周期/close 唤醒/reset），末行打印一行 PASS 说明 |
| `test_channel_reload.cpp` | 通道热更新板端离线自测（停→releaseTasks→重建 ×4 轮，槽位集合/线程数/RSS 三项断言），须在 build 目录下跑 |
| `test_alarm_uplink.cpp` | 报警上行离线自检（18 组：两种 transport 报文形状 / 帧序 / 地址换算与路径逃逸 / 队列·ack·退避），纯 Qt Core，任意机器可跑 |
| `test_geofence_role.cpp` | 围栏角色判定离线自检（8 组：禁区优先 / 无岗区域守卫 / 任一岗区域内 / shapeIndex 口径 / 凹多边形回归 / role 回落），纯 Qt Core，任意机器可跑 |
| `test_construction_qr.cpp` | 扫码登录门离线自检（11 组：施工信息解析/校验/会话状态机 + 真解一张含中文的码图），Qt Core+Gui+zbar，无摄像头无 GUI |
| `ws_sink.cpp` | 人工联调用的假对端（`QWebSocketServer`）：收 text+binary、按 media_id 拼文件对拍 sha256，url 档只打印地址 |

## 使用方法

七个测试与 `ws_sink` 都已接入根 `CMakeLists.txt`（各自一个 `add_executable`，随主程序一并构建）：

```bash
./build-linux.sh -t rk3588 -b Release
./build/build_rk3588_linux/test_database    # 退出码：失败数>0 时为 1，否则 0
./build/build_rk3588_linux/test_latest_frame_slot   # 同上；纯 CPU，不依赖板子外设
./build/build_rk3588_linux/test_alarm_uplink        # 同上；纯 Qt Core，不依赖外设与 cwd

# 围栏判定自检只链 fence_checker.cpp，改判定逻辑时优先跑它（目标级构建，不整树重编）
cmake --build build/build_rk3588_linux --target test_geofence_role -j"$(nproc)"
./build/build_rk3588_linux/test_geofence_role       # 同上；无 GUI、与 cwd 无关

# 登录门自检：解一张真码 + 校验/会话状态机。第 9 组要一张码图，缺了只 SKIP 那一组并打出命令：
#   /home/ubuntu/miniconda3/bin/python -m segno -o /tmp/idge_test_qr.png --scale 8 --border 4 \
#     '{"worker_name":"张三","worker_phone":"13800001111","worker_id":"GZ-0042",
#       "work_content":"隧道道床修补","construction_id":"SG-2026-11"}'
cmake --build build/build_rk3588_linux --target test_construction_qr -j"$(nproc)"
./build/build_rk3588_linux/test_construction_qr [码图路径...]   # 无 GUI、与 cwd 无关；路径含 "bad" 的按坏码处理

# 盘点/点名自测必须在仓库根运行（依赖 assets/test/ + model/library/yolo11n-coco +
# model/face/ 两件套权重；点名对照还需 model/face/face_recognition）；
# 直接跑主程序同目录产物即可（test_equipment 内部已 qputenv offscreen）
cd "$(git rev-parse --show-toplevel)"
./build/build_rk3588_linux/test_equipment   # 末行 PASS + rc=0 / FAIL + rc=1
./build/build_rk3588_linux/test_rollcall    # 同上
./build/build_rk3588_linux/test_channel_reload  # 同上；需 NPU/RGA，工作目录必须是构建目录（./model 软链）
```

`ws_sink` 不参与断言、要人工看输出，所以单独一段。板端端到端联调的最小姿势（两端都在这块板子上，走 loopback）：

```bash
./build/build_rk3588_linux/ws_sink 8765 /tmp/sink_out &   # 假对端：先起来
# data/config.json: websocket.server_url = ws://127.0.0.1:8765
#                   alarm_uplink.enabled = true, media_transport = "url"
./run.sh                                                 # 触发一次报警，看 sink 的日志行
curl -o /tmp/x.jpg '<sink 打出的 snapshot/media_begin url>' && sha256sum /tmp/x.jpg
```

`url` 档的"上传成功"判据就是最后这条 curl：取回的字节 sha256 必须与 `media_begin` 里申报的
`sha256` 与 `data/alarms/` 下同名文件三者一致。`ws_binary` 档则由 sink 自己拼文件并比哈希。

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
  `src/base/imaging`、`src/ai/recognition`、`src/ai/model_repo`、`src/base/config`）+
  rknnrt / OpenCV / libturbojpeg / RGA（CMake 里用 `-Wl,--exclude-libs,ALL`
  阻止静态 turbojpeg 的 `jpeg_*` 符号劫持系统 OpenCV imgcodecs 的 libjpeg ABI）
- `test_rollcall` 另链：点名服务 + 两套识别器（`face_recognizer` 对照用 exe 链路与
  结构体、`in_process_face_recognizer` 被测），其余同 `test_equipment`
- `test_latest_frame_slot` 只链 `Threads::Threads`，include 路径仅 `src/base/queue`
- `test_alarm_uplink` 只链 `Qt5::Core`，源 = `alarm_uplink_engine.cpp` + `media_payload.cpp` 两个文件
  （**故意不链** `ws_client`/`alarm_manager`/DAO/`media_pump`：前者是 QObject + 真 socket，
  后者的成品回调要投回 GUI 线程，都留给板端端到端验，不进离线自检）
- `test_geofence_role` 只链 `Qt5::Core`，源 = `fence_checker.cpp` 一个文件：它只依赖
  `fence_shape.h` 与 `common.hpp` 里的 `image_rect_t` 字段，不碰 OpenCV/RKNN 符号，
  所以围栏判定改动能在不链主程序的前提下验证
- `test_construction_qr` 链 `Qt5::Core` + `Qt5::Gui` + `${ZBAR_LIBRARIES}`，源 =
  `construction_session.cpp` + `qr_decoder.cpp` 两个文件：解码器刻意只用 `QImage`/`QRect`
  （不拖 OpenCV/RKNN 头），这是它能脱离摄像头与 GUI 线程自测的前提
- `ws_sink` 链 `Qt5::Core` + `Qt5::WebSockets`，`QCoreApplication` 无 GUI 依赖
- `test_channel_reload` 链的是主流水线的依赖闭包：`ffmpeg_video_decoder` + `video_recorder` +
  `ppe_task`/`base_task` + `model_registry` + `ConfigManager`/`parse_config` +
  DMA 缓冲 + `alarm_manager`/`alarm_dao`/`detection_dao`/`database_manager` + 围栏两件 +
  `construction_session.cpp`（`makeAlarmRecord()` 给每条报警盖施工信息戳，于是
  `alarm_manager.cpp` 多了一个 TU 依赖——手写源列表漏它就是三个 undefined reference，
  而链接错误比编译错误更难归因）+
  `src/base/imaging`，库侧与主目标同一套（FFmpeg-rkmpp / rknnrt / rga / sqlite3 / jsoncpp / OpenCV /
  GLES/EGL），并同样带 `ZERO_COPY` 与 `--allow-shlib-undefined`；库搜索目录要单独
  `target_link_directories` 一遍——主目标那份是 target 级的，不会传给别的 target

## 注意事项

- **新增测试源必须同时在 `.gitignore` 加 `!tests/xxx.cpp`**：根 `.gitignore` 里有
  `test_*.cpp` 这条"临时验证文件"规则，会把正式测试源一并挡掉，而根 CMakeLists 已经
  引用它 —— 结果是本地能编、别人克隆后 `No such file` 编不过。七个测试都已放行。
  **`ws_sink.cpp` 不在 `test_*.cpp` 的射程里（文件名不以 `test_` 开头），不需要豁免**——
  当初 `test_alarm_uplink.cpp` 就是这么被静默挡住过一轮，`git status` 里根本没有它。
- 断言基于 `src/biz/db/` 接口行为（如"批量插入10条"后 `queryByTimeRange` 期望 ≥11 条），
  改动 DAO 语义时需同步核对用例假设。
- 测试进程为单线程顺序执行，`testsPassed/testsFailed` 为全局计数，多线程化时需加锁。
- 注意与 `src/biz/db/business_db_manager`（业务库 roll_call.db）区分：本测试覆盖的是
  `src/biz/db/`（检测/报警库 idge.db）。
