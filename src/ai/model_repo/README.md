# src/ai/model_repo

> 所属域：**ai 推理与模型应用域**（目录重组 S4c 迁入）

## 功能概述

模型库注册表（`ModelRegistry`，单例）：扫描 `model/library/` 建立 id→模型/标签/元数据 索引，
为级联多模型提供"每槽位用什么模型、用哪张类别表"的**单一事实源**。
设计规划见 `plan/model_management.md`。

## 文件清单

| 文件 | 说明 |
|------|------|
| `model_registry.h` | `SlotModel`（槽位解析结果）、`ModelMeta`（库内元数据）、`ModelRegistry` API |
| `model_registry.cpp` | 库扫描/meta 合成/sha256 去重导入/`resolveCascadeSlots()` 实现 |

## 核心 API

```cpp
ModelRegistry::instance().rescan();            // 重扫 model/library/（首次调用自动执行）
ModelRegistry::resolveCascadeSlots();          // 5 槽位 → {modelPath, labelPath, modelId}
ModelRegistry::instance().importModel(...);    // 外部 rknn+labels 入库（sha256 去重）
ModelRegistry::instance().findIdByModelFile(p);// 模型文件反查库内 id
```

标签解析优先级（`resolveCascadeSlots`）：
`cascade.models[].label` 显式值 > 库内 `labels.txt`（模型文件命中库条目时） > 全局 `model.label`。

## 依赖关系 / 使用方

- `src/media/reader/ffmpeg_video_decoder.cpp`：`buildCascadeTasks()` 按槽建任务，
  `taskConfig.result_id = 槽下标`，结果经 `od.id` 路由到 `AlarmManager` 的按槽类名表；
- `src/base/config/ConfigManager.cpp`：旧配置路径迁移（绝对路径/平铺路径 → `library/` 相对路径）；
- Phase 2 模型管理 UI（规划中）：下拉框数据源、导入/删除。

## 注意事项

- `rescan()` 会清空重建索引；`ensureScanned()` 只保证首次自动扫描，改库后需显式 `rescan()`。
- `meta.json` 缺失时按目录名 + `labels.txt` 合成并回写；`classes` 以库内 `labels.txt` 实际内容为准。
- 线程安全：`entries_` 由内部 `std::mutex` 保护；`rescan` 期间并发查询只可能读到新旧混合快照，
  约定在启动与配置页操作时调用，避免与高频推理路径同瞬时重建。
- `slotClassNames_`（decoder）与 `AlarmManager::slotClassNames_` 同下标同内容，
  下标即 `result_id`；新增槽位机制时两边都要维护。
