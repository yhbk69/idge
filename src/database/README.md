# database

## 功能概述

检测/报警持久层（Qt SQL 封装 `idge.db`）。`DatabaseManager` 单例负责建库建表、
保留期维护（vacuum / 过期清理）；`DetectionDAO`、`AlarmDAO` 分别提供检测记录与
报警记录的增删改查。UI 报警查询页、看板统计、`AlarmManager` 落库都经由本目录。

## 文件清单

| 文件 | 职责 |
|------|------|
| `database_manager.h/.cpp` | 单例：initialize(dbPath)/close/vacuum/backup/cleanOld*(days)，Qt SQL 连接管理 |
| `detection_dao.h/.cpp` | 检测记录表 DAO：插入/批量插入/按时间·类别查询/统计 |
| `alarm_dao.h/.cpp` | 报警记录表 DAO：插入/查询/确认状态更新/误报标记 |

## 核心类与数据流

```
AlarmManager::storeAndNotify → AlarmDAO::insertAlarms → idge.db(alarms 表)
FFmpegVideoDecoder(检测明细) → DetectionDAO → idge.db(detections 表)
报警查询页/看板 ← DetectionDAO/AlarmDAO 查询 ← DatabaseManager::instance()
```

## 使用方法

```cpp
DatabaseManager::instance().initialize("idge.db");   // 启动时一次
AlarmDAO dao;
dao.insertAlarms(alarms);                            // 批量写入
auto rows = dao.query(...);                          // 条件查询
DatabaseManager::instance().cleanOldDetections(30);  // 保留期维护
```

## 依赖关系

- Qt5 Sql（QSqlDatabase/QSqlQuery）、`src/alarm`（AlarmRecord 结构）
- 配置：`ConfigManager` 的 `database.storeDetections` / `detectionRetentionDays`

## 注意事项

- 与 `src/db/business_db_manager`（业务库 roll_call.db，sqlite3 原生 API）是两套
  互不相干的数据库层；勿混用连接。
- 检测数据是否落库受 `storeDetections` 开关控制（对应提交 fc7ae46"仅存报警相关"）。
- 建表语句在 `initialize()` 内幂等执行；修改表结构需同步 tests/test_database.cpp。
