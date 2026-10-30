# src/biz/db — 数据持久层（两套相互独立的库）

> 目录重组 S4d：原 `src/db`（业务库）与原 `src/database`（报警库）并入同一目录，
> **只并目录、不合类**——两套管理器/DAO 保持独立，生命周期与打开方式均不同。

| 子模块 | 原目录 | 技术 | 库文件 | 职责 |
| --- | --- | --- | --- | --- |
| 业务库 | src/db | 原生 sqlite3 C API | roll_call.db (data/roll_call_data/) | 人脸登记/注销、装备盘点两阶段持久化 |
| 报警库 | src/database | Qt SQL | data/idge.db | 检测/报警记录、看板统计、备份清理 |

## 一、业务库（BusinessDBManager）


#### 功能概述
人员点名 / 设备盘点业务数据库模块。用**原生 sqlite3 C API**（非 Qt SQL）管理人脸登记/注销、
装备盘点两阶段的持久化。与检测报警库相互独立，通过 `open(db_path)` 打开专用业务库文件。

#### 文件清单

| 文件 | 说明 |
| --- | --- |
| `business_db_manager.h` | 6 个记录结构体 + `BusinessDBManager` 类声明 |
| `business_db_manager.cpp` | 建表、任务/人脸/注销/装备 CRUD、两个原子替换事务 |

#### 核心类与数据流
`BusinessDBManager` 持有单个 `sqlite3* db_` 与 `initialized_` 标志；所有接口先检查
`initialized_`，未初始化则返回空/-1。查询用预编译语句成对 `finalize`，文本 bind 全用
`SQLITE_TRANSIENT` 避免悬垂。

#### 表结构概览（6 张表）
```
tasks(id PK, name, type, create_time, folder_path,
      registered_count, cancelled_count, is_cancelled, parent_task_id)
  │
  ├─ face_records(id PK, task_id→tasks, feature_blob,
  │     original/processed/face_photo_path, face_index,
  │     is_duplicate, similar_to_id)              人脸特征记录
  ├─ cancellation_photos(id PK, task_id→tasks, original/processed_photo_path)
  ├─ cancellation_matches(id PK, task_id→tasks, registration_record_id,
  │     registration/cancellation_image, similarity, status)
  └─ equipment_photos(id PK, task_id→tasks, phase, original/processed_photo_path)
        └─ equipment_detections(id PK, photo_id→equipment_photos,
              class_index, label, confidence, x1,y1,x2,y2)
```
关系：`tasks 1—N 各子表`；`equipment_photos 1—N equipment_detections`（唯一二级子表）。

数据流：
```
上层业务 → createTask → 采集/推理 → replaceCancellationData / replaceEquipmentData
                                    (事务内先删后插、原子) → get*ByTask 查询回显
```

#### 使用方法
```cpp
BusinessDBManager db;
db.open("roll_call.db");
int tid = db.createTask("一班点名", "registration", "/data/roll_call/t1");

// 装备：detections[].photo_id 填的是 photos 数组下标，函数内自动重映射为真实 rowid
std::vector<EquipmentPhotoRecord> photos = {...};
std::vector<EquipmentDetectionRecord> dets = {{0, /*photo_id=下标*/0, ...}};
db.replaceEquipmentData(tid, /*phase=0登记*/0, photos, dets);
```

#### 依赖关系
- `sqlite3`（3rdparty/sqlite）：原生 C API。
- 上层点名/盘点服务（`src/biz/service` 内 roll_call / equipment_inventory）为调用方。
- 无 Qt 依赖。

#### 注意事项
- **外键未强制**：建表写了 `ON DELETE CASCADE`，但全程未 `PRAGMA foreign_keys=ON`
  （sqlite 默认 OFF），级联删除全靠 `deleteTask()`/`deleteXxxByTask()` 手工按序删除，
  删除顺序"先子后父"，不可调换，否则残留孤儿行。
- **photo_id 语义**：`replaceEquipmentData` 入参中 `detections[].photo_id` 是 `photos`
  的**数组下标**，事务内收集真实 `rowid` 后重映射写入；越界视为脏数据并回滚整批。
- **事务与回滚**：两个 `replace*` 用 `BEGIN IMMEDIATE`（进入即抢写锁，冲突在 BEGIN 处
  提前暴露），任一步失败 `ROLLBACK` 撤销，避免"删旧插新"半更新态。其余单语句走自动提交。
- **未开 WAL / 非线程安全**：默认 journal 模式、单连接、无应用层锁，约定单线程或串行调用。
- **BLOB 裸拷贝**：人脸特征以 `float*` 原始字节存取，隐含同机 IEEE754/字节序假设，跨端导入不可移植。
- **列序号硬编码**：SELECT 用位置索引读取，改表结构需同步修改对应解析代码。

## 二、报警库（DatabaseManager + DAO）


### 功能概述

检测/报警持久层（Qt SQL 封装 `idge.db`）。`DatabaseManager` 单例负责建库建表、
保留期维护（vacuum / 过期清理）；`DetectionDAO`、`AlarmDAO` 分别提供检测记录与
报警记录的增删改查。UI 报警查询页、看板统计、`AlarmManager` 落库都经由本目录。

### 文件清单

| 文件 | 职责 |
|------|------|
| `database_manager.h/.cpp` | 单例：initialize(dbPath)/close/vacuum/backup/cleanOld*(days)，Qt SQL 连接管理 |
| `detection_dao.h/.cpp` | 检测记录表 DAO：插入/批量插入/按时间·类别查询/统计 |
| `alarm_dao.h/.cpp` | 报警记录表 DAO：插入/查询/确认状态更新/误报标记/`updateVideoPath` 片段回填 |

### 核心类与数据流

```
AlarmManager::storeAndNotify → AlarmDAO::insertAlarms → idge.db(alarms 表)
ClipWriter(报警+2s) → AlarmManager::attachVideoClip → AlarmDAO::updateVideoPath → alarms.video_path
FFmpegVideoDecoder(检测明细) → DetectionDAO → idge.db(detections 表)
报警查询页/看板 ← DetectionDAO/AlarmDAO 查询 ← DatabaseManager::instance()
```

`alarms.video_path` 在 insert 时必为空（片段要等"报警后 2 秒"才生成），只能事后 UPDATE。
这条 UPDATE 路径由 P2 首次接通，`updateVideoPath` 此前无任何调用方。

### 使用方法

```cpp
DatabaseManager::instance().initialize("idge.db");   // 启动时一次
AlarmDAO dao;
dao.insertAlarms(alarms);                            // 批量写入（video_path 先留空）
dao.updateVideoPath(id, rawClipPath);                // 片段落盘后按 id 回填
auto rows = dao.query(...);                          // 条件查询
DatabaseManager::instance().cleanOldDetections(30);  // 保留期维护
```

板端核对片段闭环（无需 GUI）：
```sh
sqlite3 data/idge.db "select count(*) from alarms where video_path<>''"
```

### 依赖关系

- Qt5 Sql（QSqlDatabase/QSqlQuery）、`src/biz/alarm`（AlarmRecord 结构）
- 配置：`ConfigManager` 的 `database.storeDetections` / `detectionRetentionDays`

### 注意事项

- 与 `business_db_manager`（业务库 roll_call.db，sqlite3 原生 API）是两套
  互不相干的数据库层；勿混用连接。
- 检测数据是否落库受 `storeDetections` 开关控制（对应提交 fc7ae46"仅存报警相关"）。
- 建表语句在 `initialize()` 内幂等执行；修改表结构需同步 tests/test_database.cpp。

### 表结构演进：`ensureColumns()`（本轮新增，此前全仓没有任何迁移机制）

`alarms` 新增两列，都是 `TEXT`、可空、无默认、无外键：

| 列 | 装什么 | 为什么只有这一列不行 |
| --- | --- | --- |
| `worker_name` | 负责人姓名 | 列表那一列每帧都要显示它，不该为一段文本去解析 JSON |
| `construction_info` | **整份施工信息 JSON**（详情框与 WebSocket 上行都从它渲染） | 见下 |

**为什么是 2 列而不是 5 列**：施工信息的字段还没定稿，5 列意味着改一次字段要动
5 次 `ALTER` + 2 处 INSERT 列表 + `recordFromQuery` 5 行；2 列意味着只改
`src/biz/session/construction_session.h` 里的 `ConstructionInfo` 一处，详情框与上行自动跟。
代价：**不能直接按 SQL 过滤电话/工作内容**（要过滤得先 `json_extract`，本轮不做）。

**为什么必须新增机制**：建表语句有**两份**且都是 `CREATE TABLE IF NOT EXISTS`
（`sql/idge_init.sql` 与 `database_manager.cpp`），已部署的库不会自动长列。
所以两份建表**同时补上这两列**（新库根本不需要 ALTER，但两份必须一致），
老库由 `DatabaseManager::ensureColumns(table, columnDefs)` 补：`PRAGMA table_info` 逐行取列名，
缺哪列补哪列 `ALTER TABLE … ADD COLUMN`，在 `initialize()` 里紧跟建表之后、
纳秒转毫秒那段之前调用。

- **幂等**（实测）：第一次启动日志两条 `Migrated: "ALTER TABLE alarms ADD COLUMN …"`，
  第二次启动**零条**且不报错。
- **补列失败只 warn，不中断初始化**：在老库上因一条 ALTER 失败就把整个报警库关掉，
  比少两列严重得多。
- **老行不回填**：`worker_name`/`construction_info` 为 NULL → `recordFromQuery` 读成空串 →
  界面统一显示 `—`。历史上确实没有施工信息，编一个值填进去比留空更坏。
- **写入侧无覆盖风险**：两处 INSERT 都是显式列名 + `bindValue`；`recordFromQuery` 按**列名**取
  （不是位置索引），所以加列不影响既有解析。报警的更新全走显式 `UPDATE alarms SET …`，
  没有"读-改-写回"，批量插入的 `INSERT OR REPLACE` 也就不会把施工信息冲空（逐条核过）。
