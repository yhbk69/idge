# tests

## 功能概述

C++ 测试源码目录。当前有一个数据库模块测试程序 `test_database.cpp`，针对
`src/database/` 的检测记录 DAO / 报警记录 DAO / DatabaseManager 做初始化、插入、
批量插入、条件查询、统计、状态更新与维护（vacuum/过期清理）的黑盒断言测试，
输出 `[PASS]/[FAIL]` 与通过/失败计数。

## 文件/子目录清单

| 文件 | 说明 |
|------|------|
| `test_database.cpp` | 数据库模块测试（4 组用例：初始化/检测/报警/维护），基于 Qt Core + 宏 `TEST_ASSERT` |

## 使用方法

已接入根 `CMakeLists.txt`（`add_executable(test_database ...)`，随主程序一并构建）：

```bash
./build-linux.sh -t rk3588 -b Release
./build/build_rk3588_linux/test_database    # 退出码：失败数>0 时为 1，否则 0
```

测试使用内存数据库（`DatabaseManager::instance().initialize(":memory:")`），不会写磁盘文件。

## 依赖关系

- Qt5 Core / Qt5 Sql、`3rdparty/sqlite`、`3rdparty/jsoncpp`
- `src/database/`（database_manager、detection_dao、alarm_dao）

## 注意事项

- 断言基于 `src/database/` 接口行为（如"批量插入10条"后 `queryByTimeRange` 期望 ≥11 条），
  改动 DAO 语义时需同步核对用例假设。
- 测试进程为单线程顺序执行，`testsPassed/testsFailed` 为全局计数，多线程化时需加锁。
- 注意与 `src/db/business_db_manager`（业务库 roll_call.db）区分：本测试覆盖的是
  `src/database/`（检测/报警库 idge.db）。
