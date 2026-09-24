# model

## 功能概述

运行时模型与标签资源目录（不参与编译，随程序部署）。2026-09-24 起采用**模型库结构**：
RKNN 模型收编进 `library/<id>/`，一个模型一个目录；专项标签 txt 保留在根目录备用；
测试素材移至仓库根 `assets/test/`。库索引由 `src/ai/model_repo/ModelRegistry` 启动时扫描
`library/` 自动生成（无需手工维护清单），规划详见 `plan/model_management.md`。

## 目录结构

```
model/
├── library/                      # 模型库：一模型一目录，目录名即 id
│   ├── yolo11n-coco/             # YOLO11n INT8，默认模型（COCO 80 类）
│   │   ├── model.rknn            # 固定文件名
│   │   ├── labels.txt            # 专属类别表（行序 = cls_id）
│   │   └── meta.json             # 元数据（首次扫描自动生成，可手工修订）
│   ├── yolo11s-coco/             # YOLO11s（精度优先备选）
│   └── yolo11m-coco/             # YOLO11m（对照实验用）
├── coco_80_labels_list.txt       # COCO 标签母本（CLI 模式固定读取）
├── person_labels.txt / helmet_labels.txt / vest_labels.txt   # 专项标签（模型文件未入库）
└── dataset.txt                   # 量化小样本清单（内容指向 assets/test/bus.jpg）
```

- 换库/分发：zip 整个 `library/` 覆盖即可；程序启动重扫。
- 新模型入库：放入 `library/<id>/`（model.rknn + labels.txt）重启即被识别，
  或调用 `ModelRegistry::importModel()`（Phase 2 管理 UI 使用）。

## 配置引用

`config.json` 的 `model.path` / `cascade.models[].path` 填**仓库相对路径**：

```json
"model": { "path": "model/library/yolo11n-coco/model.rknn",
           "label": "model/library/yolo11n-coco/labels.txt", ... }
```

- 级联每槽位标签独立解析：`cascade.models[].label` 显式值 > 库内 `labels.txt` > 全局 `model.label`
  （`ModelRegistry::resolveCascadeSlots()` 单一事实源）。
- 旧配置（绝对路径、平铺 `model/yolo11n.rknn`）在 `ConfigManager::load()` 时一次性自动迁移并落盘。
- 未收编的外部模型（如 `/opt/xxx.rknn` 绝对路径）仍可直填，只是标签需显式配置或走全局兜底。

## 代码引用但仓库中不存在的子目录（需部署时补建）

| 路径 | 引用位置 |
|------|----------|
| `model/face/face_recognition`、`model/face/detection.rknn`、`model/face/recognition.rknn` | `src/ui/form/frmmain.cpp`（RollCallService 初始化） |

设备盘点不再依赖外部 demo 与 `model/coco|fire/` 目录（2026-09-24 内化改造）：
`frmMain::initEquipmentService()` 经 ModelRegistry 从 `library/` 解析权重
（coco 槽固定 `yolo11n-coco`；明火按 id 含 "fire" 自动探测，入库即生效）。

## 使用方法

```bash
# CLI 检测（labels 固定读 model/coco_80_labels_list.txt）
./idge -m model/library/yolo11n-coco/model.rknn -i 192.mp4

# python 侧验证（见 python/README.md）
python3 yolo11.py --model_path ../model/library/yolo11n-coco/model.rknn --target rk3588 --img_folder ../assets/test

# 模型转换输出：直接输出到库目录
python3 convert.py yolo11n.onnx rk3588 i8 ../model/library/yolo11n-coco/model.rknn
```

## 依赖关系

- `.rknn` 由 `python/convert.py`（rknn-toolkit2）在 x86 PC 上生成，平台绑定 rk3588；
- 加载方：`src/ai/yolo11/`（YOLO11Model，经 `3rdparty/rknpu2` librknnrt 运行时推理）、
  `src/ai/model_repo/`（ModelRegistry 库扫描/导入/槽位解析）、`src/biz/service/`（点名走外部
  face_recognition 二进制；盘点进程内 YOLO11Model，权重取自本库）、
  `src/base/threadpool/`（级联多模型方案）；
- 标签文件行序必须与模型输出 `cls_id` 一致；预处理约定 640×640、/255 归一化，
  与 `config.json detect` 段（conf 0.25 / nms 0.45）配套。

## 注意事项

- **模型与驱动版本强绑定**：librknnrt.so 过旧会报 RKNN_ERR_MODEL_INVALID，换板/换固件后先跑 CLI 冒烟。
- rknn 文件不进 git（`.gitignore` 排除），部署包需自行携带 `library/`。
- `meta.json` 的 `sha256` 用于导入去重与完整性核对；删除后下次启动重扫会重新生成。
- `model/face/` 缺失时，人员点名初始化会失败（`initRollCallService` 返回 false）；
  盘点权重改从 `library/` 解析，库中既无 `yolo11n-coco` 也无明火模型时 `initEquipmentService`
  返回 false——均为弹警告降级，视频监控主流程不受影响。
- 标签 txt 每行一个类名、不要带引号或逗号；新增类别后需重新导出模型保持 cls_id 对齐。
