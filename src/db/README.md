# src/db

## 功能概述
人员点名 / 设备盘点业务数据库模块。用**原生 sqlite3 C API**（非 Qt SQL）管理人脸登记/注销、
装备盘点两阶段的持久化。与检测报警库相互独立，通过 `open(db_path)` 打开专用业务库文件。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `business_db_manager.h` | 6 个记录结构体 + `BusinessDBManager` 类声明 |
| `business_db_manager.cpp` | 建表、任务/人脸/注销/装备 CRUD、两个原子替换事务 |

## 核心类与数据流
`BusinessDBManager` 持有单个 `sqlite3* db_` 与 `initialized_` 标志；所有接口先检查
`initialized_`，未初始化则返回空/-1。查询用预编译语句成对 `finalize`，文本 bind 全用
`SQLITE_TRANSIENT` 避免悬垂。

### 表结构概览（6 张表）
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

## 使用方法
```cpp
BusinessDBManager db;
db.open("roll_call.db");
int tid = db.createTask("一班点名", "registration", "/data/roll_call/t1");

// 装备：detections[].photo_id 填的是 photos 数组下标，函数内自动重映射为真实 rowid
std::vector<EquipmentPhotoRecord> photos = {...};
std::vector<EquipmentDetectionRecord> dets = {{0, /*photo_id=下标*/0, ...}};
db.replaceEquipmentData(tid, /*phase=0登记*/0, photos, dets);
```

## 依赖关系
- `sqlite3`（3rdparty/sqlite）：原生 C API。
- 上层点名/盘点服务（`src/service` 内 roll_call / equipment_inventory）为调用方。
- 无 Qt 依赖。

## 注意事项
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
