# 工器具检测模型（已入库）

**用途**: 施工工器具盘点（拍照识别计数），也可挂视频级联画框

**类别**: 24 类（Adjustable Wrench / Allen Key / Bolt / Claw Hammer / Tape Measure /
Vernier Calipers / Voltage Tester 等，全表见 labels.txt，行序 = cls_id，训练方随 onnx 提供）

**对应 plan**: plan/roadmap.md §二 P1-6 工器具清点

**状态**: **已入库**（2026-11-11）。onnx 源权重在 `model/onnx/tools.onnx`，
板端 rknn-toolkit2 2.3.0 转 INT8 落本目录；转换命令与校准注意事项见 `model/onnx/README.md`。

---

## 目录结构

```
yolo11n-tools/
├── model.rknn      # YOLO11n INT8 工器具检测模型（640×640 输入，split 头 cls 24ch）
├── labels.txt      # 24 类工器具
├── meta.json       # 元数据（含 sha256 与量化备注）
└── README.md
```

## 使用方式

- **设备盘点（主用途，零配置）**：`frmMain::equipmentModelConfigsFromLibrary()` 按
  id 含 "tools" 自动探测，入库即进盘点清单（coco 主力 + fire + tools 多模型合并检出），
  模型库导入/删除后热更新，免重启。
- **视频级联（可选）**：在 `video.channels[i].models` 里加上 `yolo11n-tools`；
  `alarm_class` 留空 = 只画框不报警（工器具没有"见到就违规"的语义，计数走盘点）。

## 备注

- ⚠ 当前 i8 校准用的是通用占位图（非工器具域内图），量化精度未验证；
  现场验收前拿含工具的照片重转一次（`DATASET=` 覆盖校准清单）。
- 类名含空格（如 `Adjustable Wrench`），labels.txt 逐行原样保留，勿改成下划线——
  行序与 cls_id 对齐是硬约定，改名不改序可以，改序必须重转模型。