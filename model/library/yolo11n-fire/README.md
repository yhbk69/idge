# 明火/烟雾检测模型（已入库）

**用途**: 视频实时明火与烟雾检测

**类别**: `fire`、`smoke`（labels.txt 行序 = cls_id）

**对应 plan**: plan/roadmap.md §二 P0-3 / §九 - 增强AI能力

**状态**: **已入库**（2026-11-10，2026-11-12 更新权重）。onnx 源权重在 `model/onnx/fire.onnx`，
板端 rknn-toolkit2 2.3.0 转 INT8 落本目录；转换命令与校准注意事项见 `model/onnx/README.md`。

---

## 目录结构

```
yolo11n-fire/
├── model.rknn      # YOLO11n INT8 明火/烟雾检测模型（640×640 输入）
├── labels.txt      # fire / smoke
├── meta.json       # 元数据（含 sha256 与量化备注）
└── README.md
```

## 备注

- 设备盘点链路（`frmMain::initEquipmentService()`）按 id 含 "fire" 自动探测，入库即生效。
- 视频通道要用它，需在 `video.channels[i].models` 里加上 `yolo11n-fire`，
  报警类别走 `video.models["yolo11n-fire"].alarm_class`（填 `fire` 或 `smoke`）。
- ⚠ 当前 i8 校准用的是通用占位图（非明火/烟雾域内图），量化精度未验证；
  现场验收前拿含明火/烟雾的帧重转一次（`DATASET=` 覆盖校准清单）。