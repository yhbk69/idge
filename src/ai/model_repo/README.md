# src/ai/model_repo

> 所属域：**ai 推理与模型应用域**（目录重组 S4c 迁入）

## 功能概述

模型库注册表（`ModelRegistry`，单例）：扫描 `model/library/` 建立 id→模型/标签/元数据 索引，
为**按通道**级联多模型提供"这个模型是哪份权重、用哪张类别表、触发哪一类报警"的**单一事实源**。
config 里从此不出现任何 `.rknn`/`labels.txt` 路径字符串，只用**库目录名**引用模型。
设计规划见 `plan/model_management.md`。

## 文件清单

| 文件 | 说明 |
|------|------|
| `model_registry.h` | `ResolvedSlot`（单槽位解析结果）、`ModelMeta`（库内元数据）、`ModelRegistry` API |
| `model_registry.cpp` | 库扫描/meta 合成/sha256 去重导入/`resolveChannelSlots()` 实现 |

## 核心 API

```cpp
ModelRegistry::instance().rescan();             // 重扫 model/library/（首次调用自动执行）
ModelRegistry::resolveChannelSlots(ch);         // 通道 ch(0-based) → QList<ResolvedSlot>
ModelRegistry::instance().referenceCount(id);   // 该库 id 被多少个通道的模型列表引用
ModelRegistry::instance().importModel(...);     // 外部 rknn+labels 入库（sha256 去重）
ModelRegistry::instance().deleteModel(id, err); // 被引用则拒绝，否则移入 model/.trash/
ModelRegistry::instance().findIdByModelFile(p); // 模型文件反查库内 id（迁移/排障用）
```

`ResolvedSlot{modelPath, labelPath, libId, alarmClass}` 的字段来源：
`modelPath`/`labelPath` 取自库目录，`alarmClass` 取自 `video.models[<libId>].alarm_class`。
列表**下标即紧凑槽位号（= `result_id`，画面上的 M1/M2/…）**；库里找不到的 id 被跳过并打告警，
不占下标——所以"某个模型没入库"只会让后面的槽位前移，不会留下永久空槽。

标签解析优先级（`resolveChannelSlots`）：
`video.models[<id>].label` 显式值（库外标签文件的逃生口，读不到会回落库内默认）> 库内 `labels.txt`。

## 依赖关系 / 使用方

- `src/media/reader/ffmpeg_video_decoder.cpp`：`buildCascadeTasks()` 在 `start()` 里按
  **本通道**解析槽位建任务，`taskConfig.result_id = 槽下标`；每槽的类别名与报警类留在解码器
  （`slotClassNames_` / `slotAlarmClasses_`），随结果组成 `AlarmManager::SlotContext` 传入 `ingest()`；
- `src/ui/form/frmmain.cpp`：`initEquipmentService()` 设备盘点模型清单的单一来源
  （coco 槽固定取 `yolo11n-coco`；明火按 id 含 "fire" 探测，入库即自动生效）；
  设置页"模型参数表/通道配置区"以 `ids()` + `resolveChannelSlots()` 为数据源；
- `src/base/config/ConfigManager.cpp`：`migrateSchemaV2()` 把旧 V1 配置（`model.path` +
  `cascade.models[]`）翻译成新的"按通道库 id 列表 + 每模型报警类"。
  ⚠ 迁移**只使用纯字符串**的 `pathToLibraryId()`，不得回调本类（见下"线程安全"）。
- 模型管理 UI：下拉框数据源、导入/删除。

## 注意事项

- `rescan()` 会清空重建索引；`ensureScanned()` 只保证首次自动扫描，改库后需显式 `rescan()`。
  每次扫描打一行 `已收录 N 个模型: <id 列表>`（现场问"现在有几个模型"就看这行）。
- **只登记存在 `model.rknn` 的目录，且文件名必须一字不差**：把转换产物命名成 `cigarette.rknn`
  这类目录会被忽略。现在会额外打 `qWarning` 点名（判据收窄到"有 labels.txt 且恰好一个 .rknn"，
  所以 `ppocrv4-ocr` 那种 det/rec 多权重目录不会天天报假警告）。
- `meta.json` 缺失时按目录名 + `labels.txt` 合成并回写；`classes` 以库内 `labels.txt` 实际内容为准。
- 线程安全：`entries_` 由内部 `std::mutex` 保护；`rescan` 期间并发查询只可能读到新旧混合快照，
  约定在启动与配置页操作时调用，避免与高频推理路径同瞬时重建。
  `resolveChannelSlots()` 是**静态**方法且会取 `ConfigManager` 的锁
  （`channelModelIds` / `modelParam` / `modelParamUsers`），而 `ConfigManager::load()` 持锁期间
  做 schema 迁移——`std::mutex` 不可重入，故 ConfigManager 内部**绝不能**回调本类，
  迁移里的路径→id 反查另有一份不依赖注册表的纯字符串实现。
- 槽位类别名表由解码器持有、按通道独立，`AlarmManager` 不再维护全局槽表；
  跨通道改动槽位配置不需要通知报警模块。
