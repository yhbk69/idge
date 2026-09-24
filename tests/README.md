# tests

## 功能概述

C++ 测试源码目录。含三个测试程序：

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

## 文件/子目录清单

| 文件 | 说明 |
|------|------|
| `test_database.cpp` | 数据库模块测试（4 组用例：初始化/检测/报警/维护），基于 Qt Core + 宏 `TEST_ASSERT` |
| `test_equipment.cpp` | 设备盘点全链离线自测（模型加载→检测计数→画框→落库→回读→热更新原子性），逐项 `[ok]/[FAIL]` |
| `test_rollcall.cpp` | 人员点名全链离线自测（exe 对照回归 + 注册/去重/注销匹配/落库回读），逐项 `[ok]/[FAIL]` |

## 使用方法

已接入根 `CMakeLists.txt`（`add_executable(test_database ...)` /
`add_executable(test_equipment ...)` / `add_executable(test_rollcall ...)`，
随主程序一并构建）：

```bash
./build-linux.sh -t rk3588 -b Release
./build/build_rk3588_linux/test_database    # 退出码：失败数>0 时为 1，否则 0

# 盘点/点名自测必须在仓库根运行（依赖 assets/test/ + model/library/yolo11n-coco +
# model/face/ 两件套权重；点名对照还需 model/face/face_recognition）；
# 直接跑主程序同目录产物即可（test_equipment 内部已 qputenv offscreen）
cd /home/ztl/dltt/code/idge-main
LD_LIBRARY_PATH="/usr/local/Qt-5.15.18/lib:$LD_LIBRARY_PATH" \
    ./build/build_rk3588_linux/test_equipment   # 末行 PASS + rc=0 / FAIL + rc=1
LD_LIBRARY_PATH="/usr/local/Qt-5.15.18/lib:$LD_LIBRARY_PATH" \
    ./build/build_rk3588_linux/test_rollcall    # 同上
```

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

## 注意事项

- 断言基于 `src/biz/db/` 接口行为（如"批量插入10条"后 `queryByTimeRange` 期望 ≥11 条），
  改动 DAO 语义时需同步核对用例假设。
- 测试进程为单线程顺序执行，`testsPassed/testsFailed` 为全局计数，多线程化时需加锁。
- 注意与 `src/biz/db/business_db_manager`（业务库 roll_call.db）区分：本测试覆盖的是
  `src/biz/db/`（检测/报警库 idge.db）。
