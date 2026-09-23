# tests

## 功能概述

C++ 单元测试源码目录。当前仅有一个数据库模块测试程序 `test_database.cpp`，针对"检测记录 DAO / 报警记录 DAO / 数据库管理器"做初始化、插入、批量插入、条件查询、统计、状态更新与维护（vacuum/过期清理）的黑盒断言测试，输出 `[PASS]/[FAIL]` 与通过/失败计数。

> **当前状态警示（重要）**：本测试依赖的 `src/database/`（`database/database_manager.h`、`database/detection_dao.h`、`database/alarm_dao.h`）**在仓库中不存在**，且根 `CMakeLists.txt` 的源文件 GLOB 只收集 `src/**`，**未接入 tests/ 目录**。因此该文件目前**不可编译、不可运行**，仅作设计参考保留。实际的数据库代码现位于 `src/db/`、`src/alarm/` 等处，接口命名与本测试并不一致。

## 文件/子目录清单

| 文件 | 说明 | 可构建性 |
|------|------|----------|
| `test_database.cpp` | 数据库模块测试（4 组用例：初始化/检测/报警/维护），基于 Qt Core + 宏 `TEST_ASSERT` | **不可构建**（缺失 `src/database/` 头文件与实现，且无 CMake 目标） |

## 使用方法

以下为该测试**理论上**的用法（需先补齐 `src/database/` 模块并新增 CMake 目标后才能执行）：

```bash
# 参考主程序构建方式配置一个独立可执行目标（示例，未在本仓库提供）：
#   add_executable(test_database tests/test_database.cpp ${DATABASE_SOURCES})
#   target_link_libraries(test_database Qt5::Core Qt5::Sql ...)
./build-linux.sh -t rk3588 -b Debug          # 现有主程序构建命令（不含 tests）
./test_database                              # 退出码：失败数>0 时为 1，否则 0
```

测试使用内存数据库（`DatabaseManager::instance().initialize(":memory:")`），不会写磁盘文件。

## 依赖关系

- Qt5：`QCoreApplication`、`QDateTime`、`QDebug`；
- 缺失依赖：`database/database_manager.h`、`database/detection_dao.h`、`database/alarm_dao.h`（含 `DetectionRecord`/`AlarmRecord` 结构体与全部 DAO 方法）；
- 不与主程序 `idge` 目标共享构建产物。

## 注意事项

- **不要**误以为本目录参与 CI 或可随 `build-linux.sh` 一起构建——它不在任何构建脚本/CMake 目标内。
- `test_database.cpp` 内部断言（如"批量插入10条"后 `queryByTimeRange` 期望 ≥11 条）基于 `src/database/` 旧接口的行为假设，若将来按 `src/db/` 现有接口恢复测试，需要整体重写用例而不是直接编译。
- 测试进程为单线程顺序执行，`testsPassed/testsFailed` 为全局计数，多线程化时需加锁。
- 新增测试时请在根 `CMakeLists.txt` 显式 `add_executable`/`enable_testing()+add_test`，并避免依赖不存在的头文件路径。
