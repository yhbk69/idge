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
| `database_manager.h/.cpp` | 单例：initialize(dbPath)/close/vacuum/backup/cleanOld*(days)，Qt SQL 连接管理；建 **5 张表**（alarms/detections + Pad 三张）与补列 |
| `detection_dao.h/.cpp` | 检测记录表 DAO：插入/批量插入/按时间·类别查询/统计 |
| `alarm_dao.h/.cpp` | 报警记录表 DAO：插入/查询/确认状态更新/误报标记/`updateVideoPath` 片段回填 + v1 的三条写路径（`dispose`/`markFalseAlarm`/`markRead`） |

### 核心类与数据流

```
AlarmManager::storeAndNotify → AlarmDAO::insertAlarms → idge.db(alarms 表)
ClipWriter(报警+2s) → AlarmManager::attachVideoClip → AlarmDAO::updateVideoPath → alarms.video_path
FFmpegVideoDecoder(检测明细) → DetectionDAO → idge.db(detections 表)
报警查询页/看板 ← DetectionDAO/AlarmDAO 查询 ← DatabaseManager::instance()

HTTP /api/（v1 §4）→ AlarmManager::{disposeAlarmById,markAlarmReadById,markAsFalsePositiveById}
    → AlarmDAO::{dispose,markRead,markFalseAlarm} → alarms 那三列状态写法
    ⚠ 每条都**先改库、再改内存窗口那份副本、最后 emit alarmStatusChanged**，
      只改库的那条会在第二天开机的整批回写里被打回原状（见下「终态保护」）
HTTP /api/（v1 §5）→ biz/geofence/PadFenceStore → pad_fences + pad_fence_versions
HTTP /api/（v1 §2.5）→ biz/pad/RequestIdCache → pad_request_cache
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
- 建表语句在 `initialize()` 内幂等执行；修改表结构需同步 **三处**：`sql/idge_init.sql`、
  `database_manager.cpp` 里的 `CREATE TABLE`、以及 `ensureColumns()` 的补列清单（见下）。
  漏前两处的任一份，新老库结构就会分叉；漏第三处，已部署的库永远长不出这一列。
  `tests/test_database.cpp` 跟着改断言。
- `alarms` / `detections` 每一列都在两份建表语句里带了一行注释（谁写、什么格式、坑在哪）。
  改列必须连注释一起同步——注释里的口径（时间无偏移、`update_time` 是补漏游标、
  `rectified` ≠ 已处置）一旦和代码分叉，比没注释更有害。

### 表结构演进：`ensureColumns()`（此前全仓没有任何迁移机制）

`alarms` 现在共补四列，都是 `TEXT`、可空、无默认、无外键：

| 列 | 装什么 | 为什么单开一列 |
| --- | --- | --- |
| `worker_name` | 负责人姓名 | 列表那一列每帧都要显示它，不该为一段文本去解析 JSON |
| `construction_info` | **整份施工信息 JSON**（详情框与 WebSocket 上行都从它渲染）。2026-11-04 起是 qr-login 的 `person`/`plan` 嵌套形状，历史行仍是扁平 `construction_id`/`worker_name` 形状，两代都读得回 | 它是"档案"，字段还没定稿，整份存最省事 |
| `task_id` | 施工计划 ID（= v1 的 `taskId`，B2 定案同物两名） | 它是**过滤条件**，不是档案。见下 |
| `dispose_person_id` | 处置人/误报人的**字符串工号**（v1 的 `dispose.personId`，例 `05-0101`、`P-001`） | 老的 `dispose_user_id` 是 `INTEGER`，装不下工号；改类型要重建表，而 v1 的口径明确是 string，所以**新列走 TEXT、旧列留着不动**（`dispose_user_name` 仍在用） |

**为什么是"档案 2 列 + 身份 1 列"**：施工信息里除了计划 ID（`plan.taskId`）之外的字段还没定稿，全开成列
意味着改一次字段要动 N 次 `ALTER` + 2 处 INSERT 列表 + `recordFromQuery` N 行；整份存 JSON
意味着只改 `src/biz/session/construction_session.h` 里的 `ConstructionInfo` 一处，详情框与上行自动跟。
代价是 SQL 过滤不了 JSON 里的字段——所以**唯一一个要被过滤的键（计划 ID）单独升了列**。
它此前走 `instr(construction_info, '"construction_id":"X"') > 0`：QSQLITE 内建的 SQLite 3.31.1
**没编 JSON1**，`json_*` 函数在这条路径上根本不存在（本仓自带的 3.49.1 只服务业务库那条 C API 直连），
`LIKE` 又会把 `%`/`_` 当通配符，所以当时只能 `instr`。三个问题让它站不住：整表扫描无索引可用、
键名一换过滤就静默失效、以及和 §13.1.5 要求的"按 taskId 过滤"之间多了一层文本约定。
现在是 `task_id = ?` 等值 + `idx_alarms_task_id`。

**为什么必须新增机制**：建表语句有**两份**且都是 `CREATE TABLE IF NOT EXISTS`
（`sql/idge_init.sql` 与 `database_manager.cpp`），已部署的库不会自动长列。
所以两份建表**同时补上这几列**（新库根本不需要 ALTER，但两份必须一致），
老库由 `DatabaseManager::ensureColumns(table, columnDefs)` 补：`PRAGMA table_info` 逐行取列名，
缺哪列补哪列 `ALTER TABLE … ADD COLUMN`。

**调用位置是实测出来的，不是排版选择**：`ensureColumns` 必须夹在
`CREATE TABLE alarms` 与 `CREATE INDEX … ON alarms(worker_name)` **之间**（现在就在 `createTables()` 内部）。
放在 `initialize()` 里建表之后曾实测失败：老库上先建索引就报
`SQL execution failed: "no such column: worker_name"`，那一整次运行 `idx_alarms_worker` 是缺的，
翻页只能退回单列索引 + 临时排序。

- **幂等**（实测）：第一次启动日志两条 `Migrated: "ALTER TABLE alarms ADD COLUMN …"`，
  第二次启动**零条**且不报错。
- **补列失败只 warn，不中断初始化**：在老库上因一条 ALTER 失败就把整个报警库关掉，
  比少两列严重得多。
- **`worker_name`/`construction_info` 老行不回填**：为 NULL → `recordFromQuery` 读成空串 →
  界面统一显示 `—`。历史上确实没有施工信息，编一个值填进去比留空更坏。
- **`task_id` 老行回填**（唯一回填的一列）：见下节。
- **写入侧无覆盖风险**：两处 INSERT 都是显式列名 + `bindValue`；`recordFromQuery` 按**列名**取
  （不是位置索引），所以加列不影响既有解析。报警的更新全走显式 `UPDATE alarms SET …`，
  没有"读-改-写回"，批量插入的 `INSERT OR REPLACE` 也就不会把施工信息冲空（逐条核过）。

### `task_id` 回填：`backfillAlarmTaskIds()`

ALTER 出来的新列在老行上全是 NULL，而 JSON 里那个值本来就在，所以从 JSON 里取出来写进列。
一次 `SELECT` 只捞 `task_id IS NULL AND construction_info IS NOT NULL` 的行，逐条
`UPDATE … WHERE id = ?`，值只由 `session::ConstructionInfo::taskIdFromJsonText()` 产出
（C++ 侧唯一的 JSON→taskId 转换缝，解析失败返回空串）。**不在 SQL 里做**：那条路径没有 JSON1。

四条实测出来的坑，改动这段代码前先读：

1. **`commit()` 会因为"还有活动语句"整笔失败**。同一连接上
   `initialize()` 开头那句 `PRAGMA journal_mode=WAL` 的 `QSqlQuery` 若不 `finish()`，
   commit 就报 `cannot commit transaction - SQL statements in progress`。
2. **失败时不能 `rollback()`**。SQLite 的 DDL 是事务性的，同一连接上刚 `ALTER` 出来的三列
   会被一句 rollback 一起吞掉，后续每条 SQL 都跟着报 `no such column: task_id`——
   老库升级路径实测就是这么坏的（本机自己写出来的 P0）。回填只碰 `task_id IS NULL` 的行，
   提交没成的最坏结果是下次启动重来一遍，所以这里**只 warn、只返回 -1，不回滚**。
3. **收尾必须把"确实没有计划身份"的行写成 `''`**。`QString()` 在 Qt 里是 **null 串**，绑进 SQL 就是
   NULL，不写这一条，那些行每次开机都还留在 `task_id IS NULL` 的扫描结果里被逐条重写。
   实测 3432 行的老库每开机重跑一次 ~35ms，现场几万行就是几十万行；写成 `''` 之后第二次启动扫到 0 行。
4. **`''` 还会被关机回写打回 NULL**（第 3 条只挡住了读侧）。`AlarmManager::syncToDatabase()` 关机时把
   RAM 窗口（1000 条）整批 `insertAlarms()` 回写，未登录行的 `taskId` 是空 QString → 绑成 NULL →
   `INSERT OR REPLACE` 把刚收尾好的 1000 行覆盖回 NULL。实测第二次启动于是又打
   `Alarm task_id backfill: 0 from JSON + 1000 sealed empty, in 12 ms`——收尾白做，每开机重跑。
   所以这一列的"永不为 NULL"不变式**写在 SQL 里**：两条 INSERT 的值位是 `IFNULL(:task_id, '')`，
   不依赖 C++ 侧空串到底是不是 null 这种细枝末节。

板端复测（同一份 3432 行老库副本，2026-11-04 16:55）：

- 首启：`Migrated: ALTER TABLE alarms ADD COLUMN worker_name/construction_info/task_id` ×3 →
  `Alarm task_id backfill: 0 from JSON + 3432 sealed empty, in 14 ms` → 关机 `Synced 1000 alarms`。
- 第二次启动：**0 条 `Migrated`、0 条 `backfill`**，关机照样 `Synced 1000 alarms`（回写不再破坏幂等）。
- 库里 `typeof(task_id)` 全表 3432 行都是 `text`，NULL 行 0。
- 另一份真实副本（联调库形状，38 行全带 `construction_id":"SG-2026-11"`）首启打
  `Alarm task_id backfill: 38 from JSON + 0 sealed empty, in 2 ms`，`task_id` 分布 `[('SG-2026-11', 38)]`。

- `NULL` 与 `''` 在这张表里语义相同（都不属于任何计划），查询侧一律按"非空才算有值"处理；
  `taskIdFacets` 的 `WHERE task_id IS NOT NULL AND task_id <> ''` 两个条件都留着，
  因为历史库里两种写法都存在过。
- 回归测试：`tests/test_database.cpp` 的 `testLegacyTaskIdUpgrade()` 用**文件库**跑真实升级——
  插一行只带 `construction_info` 的老行、把 `task_id` 强制打回 NULL、重新 `initialize()`，
  断言按 `taskId` 能查到它、NULL 行归零、第三次 `initialize()` 无害、被收尾的行 `typeof` 是 `text` 而非 NULL。
  这条测试是上面三个坑存在的原因；内存库（每次都是新库）覆盖不到它。

### Pad HTTP 面带来的三张表与三条写语义（2026-11-05）

三张新表都在报警库里（同一个 `idge.db`、同一套 Qt SQL 连接），**不 service 本机自己的围栏判定**——
本机检测用的围栏仍存在 `geofence` 配置段里，这三张表只服务 v1 §5 的出入站（口径见
`src/biz/pad/README.md`「HTTP 面还没定、也没在代码里偷偷定的事」）。

| 表 | 谁写 | 关键列与坑 |
| --- | --- | --- |
| `pad_request_cache` | `RequestIdCache`（§2.5 幂等） | `request_id` 是主键，**命中只按它、不比较内容**；`http_status`/`body` 存首次那份响应（含 4xx/5xx），重放时一字不改地写回；`created_at` 定宽文本，`prune()` 按字典序删 ≥24h 前的 |
| `pad_fences` | `PadFenceStore`（§5） | 围栏身份 `(task_id, camera_id)`：**唯一索引**，`camera_id` 是 §6 的 `CHn` 口径而不是注册表 id（注册表 id 会被两路引用，撞在唯一键上就是"第二个围栏建不出来"）；`next_fence_id()` 生成 `FENCE-%03d`，分配后必须稳定 |
| `pad_fence_versions` | 同上 | 主键 `(fence_id, version)`；`status` 三值 `ACTIVE/DRAFT/DISABLED`，**ACTIVE 每围栏至多一条**（靠 apply 前把旧的转 DISABLED，不是靠约束）；`regions_json` 原样存 Pad 给的 JSON（`regionId` 不许改写）；`frame_path` 只存相对路径，URL 在出门口拼 |

报警侧这三条写语义是 HTTP 面新加的，`alarm_dao.cpp` 里都有对应函数：

- **终态保护写在 SQL 里**：`dispose()` 与 `markFalseAlarm()` 的 `WHERE` 都带
  `status NOT IN ('disposed','false_alarm')`。判据放在 C++ 侧就会有两线程同时过一次判据的窗口，
  放在 SQL 里 `numRowsAffected()==0` 就是没写成 → 路由回 `409/2003`。
- **`markRead(id, candidateTime)` 收"候选值"**：v1 §4.4 要"保留最早 `readAt`"，所以是
  「库里已有的更早 → 保留已有的；否则用对端报的那个」，并且**必推 `update_time`**
  （它是 §3.8 断线补漏的游标）。墙钟回拨时不能把已有值往后放。
- **`dispose_photo` 从"单张裸路径"改成 JSON 数组文本**（v1 允许 0~5 张）。列没换类型（还是 TEXT），
  所以**读侧两种形状都要认**：`[` 开头按 JSON 数组解，否则当历史单张裸路径。
  这处判据只有**一份**，在 `src/biz/pad/pad_wire.cpp:287` 那段（`alarm` 与 `alarm_update` 共用的
  dispose 对象）；写入侧唯一入口是 `AlarmDAO::dispose()` 的 `photoPathsJson` 形参
  （`alarm_dao.cpp:954` 那句 `dispose_photo = :photo`），别的函数不许往这列塞东西。
- ⚠ **任何状态写入都要连内存窗口那份副本一起改**（`AlarmManager` 的三个 `*ById` 都是
  「库 → 副本 → `emit alarmStatusChanged`」）。只改库的写法在第二次开机的
  `syncToDatabase()` 整批回写里会被打回原状——这条实测过，`tests/test_pad_http_api.cpp` 契约 21 钉着它。

板端实测（真 `idge` + 真库，2026-11-05 12:25）：dispose 两张照片后库里是
`status=disposed | dispose_result=已现场纠正 | dispose_person_id=05-0101 |
dispose_photo=["dispose/<id>/dispose1.jpg","dispose/<id>/dispose2.png"]`；
`SIGTERM` 退出后这四行状态没被回写打回；`pad_request_cache` 25 条。

### 检索层：`AlarmFilter` / `DetectionFilter`（2026-10-31）

列表页原先读的是 `AlarmManager` 的 RAM 窗口（几千条），3.5 万条历史根本翻不到。现在两侧各有一个
filter 结构体，是**唯一**的取数入口：

```cpp
struct AlarmFilter {          // alarm_dao.h
    long startTime = 0, endTime = 0;      // 0 = 不限；单位毫秒，内部转 ISO 文本再比
    int channel = -1;                     // < 0 = 全部
    QString className, status;            // 空 = 全部；status 空 ⇒ "不含误报"而不是"全部状态"
    QString workerName, taskId;             // 空 = 不限（taskId 就是 v1 的 taskId，B2 定案）
    FenceRole fenceRole;                  // RoleAll / RoleIntrusion(fence|fence_in) / RoleLeave(fence_out)
    Scope scope;                          // All / ClassOnly / FenceOnly（按 alarm_type 的 "fence" 前缀）
    int limit = 500, offset = 0;
};
```

`DetectionFilter` 是它的子集（时间/通道/类别/分页）。旧的
`queryByTimeRange / queryByChannel / queryByClass / queryByChannelAndClass` 已删——四个近亲函数
是每个新维度都要再复制一遍的根源。

- **时间比较为什么能用字典序**：`alarm_time` 是定宽 `yyyy-MM-ddTHH:mm:ss`（全库核过：19 字符、
  恒有 `T`、无小数秒），所以 `BETWEEN` 的文本序 == 时间序，且能吃 `idx_alarms_time`。
  改成带毫秒或换分隔符都会静默失效，别动格式。
- **排序必须带唯一列兜底**：`ORDER BY alarm_time DESC, id ASC`（检测表同理 `timestamp, id`）。
  `alarm_time` 只到秒，而 30fps 检测 + 2s 限流让同一秒多条成为常态（实测截图里就有 3 条同秒），
  只按秒排的话 `OFFSET` 翻页会在页边界上重复一行或漏一行。
- **facet 查询是候选值的唯一来源**：`classFacets` / `workerFacets` / `taskIdFacets`
  （+ 检测侧 `classFacets`）返回 `(取值, 条数)`，`LIMIT 60` 兜底，界面靠点选不需要打字。
  `taskIdFacets` 就是 `SELECT task_id, COUNT(*) … GROUP BY task_id`（B2 定案加了一等列之后才有这条；
  此前是 `GROUP BY construction_info` 后在 C++ 里用 `QJsonDocument` 取 `construction_id` 再合并，
  因为 SQL 侧没有 JSON1，见上）。`WHERE task_id IS NOT NULL AND task_id <> ''` 两个条件都留着，
  回填前后两种形状的历史库里都存在过。
- **新增两条索引**（两份建表都写）：`idx_alarms_channel_time(channel, alarm_time)`、
  `idx_alarms_worker(worker_name)`。实测 35383 行的库上「通道+时间档」翻页 34ms、「按负责人」28ms
  都发生在 GUI 线程，单列索引组合不出这个数（`EXPLAIN` 里有 `TEMP B-TREE FOR ORDER BY`）。
  ⚠ 那个"35383/38294 行的联调库"在 2026-11-04 随 `build/build_rk3588_linux/` 一起没了
  （见 `plan/代码审查_2026-11-04_协议歧义与冲突复核.md` §〇），
  这几个数从此**复现不了**，只能当作建索引时的历史依据；现在盘上最大的一份是 3432 行。
- **`acknowledgeAllPending()`**：一条 `UPDATE … WHERE status NOT IN ('rectified','disposed')`。
  原来「全部确认」在 RAM 里最多收集 1000 个 id 再逐条 UPDATE，而库里 pending 有 27858 条 ——
  按钮点了等于没点。现在返回受影响行数，`test_database` 断言它等于点之前的 pending 数。
  排除 `disposed` 是因为它是 v1 的 `DISPOSED` 终态：让"全部确认"把已处置的行退回复通，
  等于本机自己违反 §3.7「终态不能回退成 PENDING」。

