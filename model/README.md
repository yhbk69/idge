# model

## 功能概述

运行时模型与标签资源目录（不参与编译，随程序部署）。2026-09-24 起采用**模型库结构**：
RKNN 模型收编进 `library/<id>/`，一个模型一个目录；测试素材移至仓库根 `assets/test/`。
2026-10-28 起再把**训练侧源材料**（`.pt` 权重、数据集 yaml、量化校准清单）单独收进
`train/`，与运行时彻底分开——程序不读 `train/`，只在 x86 PC 上转换时用。
库索引由 `src/ai/model_repo/ModelRegistry` 启动时扫描 `library/` 自动生成（无需手工维护清单），
规划详见 `plan/model_management.md`。

## 目录结构

```
model/
├── library/                      # 视频级联检测模型库（运行时唯一事实源）
│   ├── yolo11n-coco/             # YOLO11n INT8，默认模型（COCO 80 类）
│   │   ├── model.rknn            # 固定文件名
│   │   ├── labels.txt            # 专属类别表（行序 = cls_id）
│   │   └── meta.json             # 元数据（首次扫描自动生成，可手工修订）
│   ├── yolo11s-coco/             # YOLO11s（精度优先备选）
│   ├── yolo11m-coco/             # YOLO11m（对照实验用）
│   ├── yolo11n-ppe/              # PPE安全装备检测（5类：un_helmet/helmet/people/vest/workWear）
│   ├── yolo11n-cigarette/        # 吸烟检测（1类：cigarette，2026-10-28 转好 rknn，尚未挂通道）
│   ├── yolo11n-behavior/         # [预留] 行为分析检测（跌倒/打电话/入侵/动火）
│   ├── yolo11n-fire/             # [预留] 明火/烟雾检测
│   ├── pp-liteseg-seg/           # [预留] 语义分割（区域入侵精确判定）
│   └── ppocrv4-ocr/              # [已部署] OCR文字识别（det.rknn+rec.rknn+labels.txt，不走 model.rknn 约定；执行体在 3rdparty/ppocrv4）
├── train/                        # 【训练/转换侧】.pt 权重、数据集 yaml、量化校准清单，程序不读
│   ├── README.md                 # 转换链路与"为什么程序不读这里"
│   ├── cigarette/yolo11n-cigarette.pt
│   ├── ppe/ppe.yaml
│   └── calib/{dataset.txt,dataset_test.txt}
├── face/                         # 人脸相关模型（照片处理，非实时；路径被代码写死，勿改名）
│   ├── detection/                # SCRFD 人脸检测
│   │   ├── model.rknn
│   │   ├── labels.txt
│   │   └── meta.json
│   └── recognition/              # 人脸特征提取（512维）
│       ├── model.rknn
│       ├── labels.txt
│       └── meta.json
├── tools/                        # [预留] 工器具检测模型（照片处理，非实时）
└── audio/                        # [预留] 音频检测模型（声音事件检测）
```

- 换库/分发：zip 整个 `library/` 覆盖即可；程序启动重扫。
- 新模型入库：放入 `library/<id>/`（model.rknn + labels.txt）重启即被识别，
  或调用 `ModelRegistry::importModel()`（设置页「模型管理」在用）。
- `rescan()` **只登记存在 `model.rknn` 的目录**（`model_registry.cpp:143`），且**文件名必须正好是
  `model.rknn`**——所以先建 labels/meta 的占位目录不会出现在 UI 里，也不会被任何通道引用；
  反过来，产物叫别的名字（如 `cigarette.rknn`）也会被静默忽略。当前注册表可见 5 个：
  `yolo11n-coco` / `yolo11s-coco` / `yolo11m-coco` / `yolo11n-ppe` / `yolo11n-cigarette`。
  `ppocrv4-ocr`（det.rknn + rec.rknn 两件套）与 `model/face/` 两件套按约定**不进注册表**，
  由各自模块直接加载。

## 预留目录说明

按 `plan/roadmap.md` §二 P0-3 与 §九 的需求缺口，预留以下模型目录（各含 README.md 详细说明）：

| 目录 | 用途 | 对应 plan | 状态 |
|------|------|----------|------|
| `library/yolo11n-cigarette/` | 吸烟（手持烟支）检测 | 方向2 - 行为分析 / P0-3 场景⑤ | **已入库**（2026-10-28 转好 rknn），差 config 挂接：`alarm_class=cigarette` + 加进通道 |
| `library/yolo11n-behavior/` | 行为分析（跌倒/打电话/动火） | 方向2 - 行为分析 | 待模型选型（吸烟已单独立项，见上） |
| `library/yolo11n-fire/` | 明火/烟雾检测 | 方向5 - 增强AI | 待模型训练 |
| `library/pp-liteseg-seg/` | 语义分割（区域入侵精确判定） | 方向5 - 增强AI | 待模型转换 |
| `library/ppocrv4-ocr/` | OCR文字识别（车牌/安全标识） | 方向5 - 增强AI | 已部署（两阶段模型，`SmartRecognitionWidget` 经 `3rdparty/ppocrv4/bin` 的外部可执行调用，不被 ModelRegistry 收录） |
| `tools/` | 工器具检测（照片处理） | 方向5 - 增强AI | 待模型训练 |
| `audio/` | 音频检测（声音事件） | 方向7 - 音频能力 | 待模型选型 |

**使用方式**：模型文件放入对应目录后，重启应用即可被 `ModelRegistry` 自动识别；
再在 `config.json` 的 `video.channels[i].models` 里按顺序写上**库目录名**，该模型才会真正参与推理。

## 配置引用

config.json（schema V2）里**不出现任何模型/标签路径**，只按**库目录名**引用：

```json
"video": {
  "models":   { "yolo11n-coco": { "alarm_class": "", "note": "COCO通用检测" } },
  "channels": [ { "channel": 0, "source": "192.mp4", "models": ["yolo11n-ppe", "yolo11n-coco"] } ]
}
```

- 槽位解析单一出口 `ModelRegistry::resolveChannelSlots(ch)`：路径固定取
  `library/<id>/model.rknn`，标签优先级 = `video.models[<id>].label`（逃生口，一般留空）
  \> 库内 `labels.txt`；报警类取 `video.models[<id>].alarm_class`。
- 目录名即 id，**改名等于换模型**（config 里的引用会一起断掉，启动打告警并跳过该槽位）。
- 老配置（`model.path` + 全局 5 槽 `cascade.models[]`，含绝对路径与平铺 `model/yolo11n.rknn`）
  在 `ConfigManager::load()` 时一次性自动迁移并落盘；未收编的外部模型路径由迁移反查库 id，
  反查不到会 `qWarning` 打印原始路径，需要人工把它收编进 `library/`。

## 代码引用但仓库中不存在的子目录（需部署时补建）

| 路径 | 引用位置 |
|------|----------|
| `model/face/detection/model.rknn`、`model/face/recognition/model.rknn` | `src/ui/form/frmmain.cpp`（RollCallService 初始化，进程内 `InProcessFaceRecognizer` 加载） |
| `model/face/face_recognition` | 仅 CLI 手工验证与 `tests/test_rollcall` 对照回归需要（点名生产链 2026-09 已进程内化，主程序不再调起） |

设备盘点不再依赖外部 demo 与 `model/coco|fire/` 目录（2026-09-24 内化改造）：
`frmMain::initEquipmentService()` 经 ModelRegistry 从 `library/` 解析权重
（coco 槽固定 `yolo11n-coco`；明火按 id 含 "fire" 自动探测，入库即生效）。

## 使用方法

```bash
# CLI 检测（类别表取"模型同目录 labels.txt"，取不到才退回 library/yolo11n-coco/labels.txt）
./idge -m model/library/yolo11n-coco/model.rknn -i 192.mp4

# python 侧验证（见 python/README.md）
python3 yolo11.py --model_path ../model/library/yolo11n-coco/model.rknn --target rk3588 --img_folder ../assets/test

# 模型转换：.pt 一步到库目录（x86 PC）
./python/pt2rknn.sh model/train/cigarette/yolo11n-cigarette.pt yolo11n-cigarette
# 或已有 onnx 时手工转（输出路径必填，直接落库）
python3 convert.py yolo11n.onnx rk3588 i8 ../model/library/yolo11n-coco/model.rknn
```

## 依赖关系

- `.rknn` 由 `python/convert.py`（rknn-toolkit2）在 x86 PC 上生成，平台绑定 rk3588；
- 加载方：`src/ai/yolo11/`（YOLO11Model，经 `3rdparty/rknpu2` librknnrt 运行时推理）、
  `src/ai/model_repo/`（ModelRegistry 库扫描/导入/槽位解析）、`src/ai/recognition/`
  （点名的 `InProcessFaceRecognizer` 直接加载 `face/` 两件套权重，进程内推理）、
  `src/base/threadpool/`（级联多模型方案）；
- 标签文件行序必须与模型输出 `cls_id` 一致；预处理约定 640×640、/255 归一化，
  与 `config.json detect` 段（conf 0.25 / nms 0.45）配套。

## 注意事项

- **模型与驱动版本强绑定**：librknnrt.so 过旧会报 RKNN_ERR_MODEL_INVALID，换板/换固件后先跑 CLI 冒烟。
- 二进制模型/权重不进 git：`.gitignore` 排除 `*.rknn`、`*.pt`、`*.onnx`。部署包需自行携带
  `library/`；`train/` 下只有 README 与 yaml 入库，`.pt` 靠训练方自存（丢失即无法重转，只能重新训练）。
- `meta.json` 的 `sha256` 用于导入去重与完整性核对；删除后下次启动重扫会重新生成。
- `model/face/` 的**两件套权重**（detection/model.rknn 和 recognition/model.rknn）缺失时，人员点名初始化会失败
  （`initRollCallService` 返回 false；`face_recognition` exe 已降级为 CLI 对照工具，缺失不影响点名运行）；
  盘点权重改从 `library/` 解析，库中既无 `yolo11n-coco` 也无明火模型时 `initEquipmentService`
  返回 false——均为弹警告降级，视频监控主流程不受影响。
- 标签 txt 每行一个类名、不要带引号或逗号；新增类别后需重新导出模型保持 cls_id 对齐。
