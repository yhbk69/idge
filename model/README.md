# model

## 功能概述

运行时模型与标签资源目录（不参与编译，随程序部署）。2026-09-24 起采用**模型库结构**：
RKNN 模型收编进 `library/<id>/`，一个模型一个目录；测试素材移至仓库根 `assets/test/`。
2026-10-28 起再把**训练侧源材料**（`.pt` 权重、数据集 yaml、量化校准清单）单独收进
`train/`，与运行时彻底分开——程序不读 `train/`，只在转换时用。
2026-11-10 起新增 `onnx/`：**已导出待转换**的 onnx 权重收在这里（ladder/fire），
与 `train/` 的分工见 `onnx/README.md`；两者都不被运行期加载。
库索引由 `src/ai/model_repo/ModelRegistry` 启动时扫描 `library/` 自动生成（无需手工维护清单），
规划详见 `plan/model_management.md`。

## 目录结构

```
model/
├── library/                      # 视频级联检测模型库（运行时唯一事实源）
│   ├── yolo11n-ppe/              # PPE安全装备检测（5类：un_helmet/helmet/person/vest/workWear），默认模型
│   │   ├── model.rknn            # 固定文件名
│   │   ├── labels.txt            # 专属类别表（行序 = cls_id）
│   │   ├── label_zh.txt          # [可选] 逐行对齐的中文显示名，仅本机 UI（叠加/报警展示），不影响推理与协议
│   │   └── meta.json             # 元数据（首次扫描自动生成，可手工修订）
│   ├── yolo11n-cigarette/        # 吸烟检测（1类：cigarette，2026-10-28 转好 rknn，尚未挂通道）
│   ├── yolo11n-ladder/           # 梯子检测（1类：ladder，2026-11-10 入库；登高守护触发源）
│   ├── yolo11n-fire/             # 明火/烟雾检测（2类：fire/smoke，2026-11-10 入库）
│   ├── yolo11n-tools/            # 工器具检测（24类，2026-11-11 入库；盘点自动探测）
│   ├── yolo11n-behavior/         # [预留] 行为分析检测（打电话/入侵；登高守护已走规则引擎，见其 README）
│   ├── pp-liteseg-seg/           # [预留] 语义分割（区域入侵精确判定）
│   └── ppocrv4-ocr/              # [已部署] OCR文字识别（det.rknn+rec.rknn+labels.txt，不走 model.rknn 约定；执行体在 3rdparty/ppocrv4）
├── onnx/                         # 【转换输入】已导出待转换的 onnx 权重（ladder.onnx/fire.onnx），程序不读
│   └── README.md                 # 来源、规格、sha256、转换命令与精度注意事项
├── train/                        # 【训练/转换侧】.pt 权重、数据集 yaml、量化校准清单，程序不读
│   ├── README.md                 # 转换链路与"为什么程序不读这里"
│   ├── cigarette/yolo11n-cigarette.pt
│   ├── ppe/ppe.yaml
│   └── calib/{dataset.txt,dataset_test.txt,dataset_onnx_generic.txt}
├── face/                         # 人脸相关模型（照片处理，非实时；路径被代码写死，勿改名）
│   ├── detection/                # SCRFD 人脸检测
│   │   ├── model.rknn
│   │   ├── labels.txt
│   │   └── meta.json
│   └── recognition/              # 人脸特征提取（512维）
│       ├── model.rknn
│       ├── labels.txt
│       └── meta.json
├── tools/                        # [预留] 安全装备检测（安全带/绝缘手套；工器具检测已入库 library/yolo11n-tools）
└── audio/                        # [预留] 音频检测模型（声音事件检测）
```

- 换库/分发：zip 整个 `library/` 覆盖即可；程序启动重扫。
- 新模型入库：放入 `library/<id>/`（model.rknn + labels.txt）重启即被识别，
  或调用 `ModelRegistry::importModel()`（设置页「模型管理」在用）。
- `rescan()` **只登记存在 `model.rknn` 的目录**（`model_registry.cpp:143`），且**文件名必须正好是
  `model.rknn`**——所以先建 labels/meta 的占位目录不会出现在 UI 里，也不会被任何通道引用；
  反过来，产物叫别的名字（如 `cigarette.rknn`）也会被静默忽略。当前注册表可见 5 个：
  `yolo11n-ppe` / `yolo11n-cigarette` / `yolo11n-ladder` / `yolo11n-fire` / `yolo11n-tools`。
  `ppocrv4-ocr`（det.rknn + rec.rknn 两件套）与 `model/face/` 两件套按约定**不进注册表**，
  由各自模块直接加载。

## 预留目录说明

按 `plan/roadmap.md` §二 P0-3 与 §九 的需求缺口，预留以下模型目录（各含 README.md 详细说明）：

| 目录 | 用途 | 对应 plan | 状态 |
|------|------|----------|------|
| `library/yolo11n-cigarette/` | 吸烟（手持烟支）检测 | 方向2 - 行为分析 / P0-3 场景⑤ | **已入库**（2026-10-28 转好 rknn），差 config 挂接：`alarm_class=cigarette` + 加进通道 |
| `library/yolo11n-ladder/` | 梯子检测（登高守护触发源） | 登高守护实现方案（docs/ladderguard/） | **已入库**（2026-11-10）；与人类模型同挂一路即激活守护（`src/biz/ladderguard`），报警类型 `ladder_no_spotter` |
| `library/yolo11n-behavior/` | 行为分析（打电话） | 方向2 - 行为分析 | 待模型选型（吸烟已单独立项；登高守护不等行为模型，已用 ladder+人模型规则引擎落地） |
| `library/yolo11n-fire/` | 明火/烟雾检测 | 方向5 - 增强AI | **已入库**（2026-11-10，fire/smoke 两类，2026-11-12 更新权重）；盘点链路按 id 含 "fire" 自动探测，入库即生效 |
| `library/yolo11n-tools/` | 工器具检测（24 类） | P1-6 工器具清点 | **已入库**（2026-11-11）；盘点链路按 id 含 "tools" 自动探测，入库即生效；也可挂视频级联只画框 |
| `library/pp-liteseg-seg/` | 语义分割（区域入侵精确判定） | 方向5 - 增强AI | 待模型转换 |
| `library/ppocrv4-ocr/` | OCR文字识别（车牌/安全标识） | 方向5 - 增强AI | 已部署（两阶段模型，`SmartRecognitionWidget` 经 `3rdparty/ppocrv4/bin` 的外部可执行调用，不被 ModelRegistry 收录） |
| `tools/` | 安全装备检测（安全带/绝缘手套） | 方向5 - 增强AI | 待模型训练（工器具检测已入库 `library/yolo11n-tools`） |
| `audio/` | 音频检测（声音事件） | 方向7 - 音频能力 | 待模型选型 |

**使用方式**：模型文件放入对应目录后，重启应用即可被 `ModelRegistry` 自动识别；
再在 `config.json` 的 `video.channels[i].models` 里按顺序写上**库目录名**，该模型才会真正参与推理。

## 配置引用

config.json（schema V2）里**不出现任何模型/标签路径**，只按**库目录名**引用：

```json
"video": {
  "sources":  [ { "id": "demo-mp4", "type": "file", "url": "assets/test/192.mp4" } ],
  "models":   { "yolo11n-ppe": { "alarm_class": "un_helmet", "note": "PPE安全装备检测" } },
  "channels": [ { "channel": 0, "source_id": "demo-mp4", "models": ["yolo11n-ppe"] } ]
}
```

通道引用视频流走 `source_id`（`video.sources[]` 的 `id`）；老写法直接内联
`"source": "assets/test/192.mp4"` 仍然可用，见 `src/base/config/README.md` 的三层兜底。

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
（基础槽固定 `yolo11n-ppe`；明火按 id 含 "fire"、工器具按 id 含 "tools" 自动探测，入库即生效）。

## 使用方法

```bash
# python 侧验证（见 python/README.md）
python3 yolo11.py --model_path ../model/library/yolo11n-ppe/model.rknn --target rk3588 --img_folder ../assets/test

# 模型转换：.pt 一步到库目录（x86 PC 或板端均可，toolkit2 2.3.0 起 aarch64 可用）
./python/pt2rknn.sh model/train/cigarette/yolo11n-cigarette.pt yolo11n-cigarette
# 或已有 onnx 时手工转（输出路径必填，直接落库；onnx 源权重收在 model/onnx/）
python3 convert.py yolo11n.onnx rk3588 i8 ../model/library/yolo11n-ppe/model.rknn
# ladder/fire 的实际转换命令与校准注意事项见 model/onnx/README.md
```

## 依赖关系

- `.rknn` 由 `python/convert.py`（rknn-toolkit2）生成，平台绑定 rk3588；
  2.3.0 起板端 aarch64 也能转（2026-11-10 本机实测，见 `onnx/README.md`）；
- 加载方：`src/ai/yolo11/`（YOLO11Model，经 `3rdparty/rknpu2` librknnrt 运行时推理）、
  `src/ai/model_repo/`（ModelRegistry 库扫描/导入/槽位解析）、`src/ai/recognition/`
  （点名的 `InProcessFaceRecognizer` 直接加载 `face/` 两件套权重，进程内推理）、
  `src/ai/task/ppe_task`（级联多模型：一个模型一个推理线程）。
- 标签文件行序必须与模型输出 `cls_id` 一致；预处理约定 640×640、/255 归一化，
  与 `config.json detect` 段（conf 0.25 / nms 0.45）配套。

## 注意事项

- **模型与驱动版本强绑定**：librknnrt.so 过旧会报 RKNN_ERR_MODEL_INVALID，换板/换固件后先在设置页挂一路做冒烟验证。
- 二进制模型/权重不进 git：`.gitignore` 排除 `*.rknn`、`*.pt`、`*.onnx`。部署包需自行携带
  `library/`；`train/` 下只有 README 与 yaml 入库，`.pt` 靠训练方自存（丢失即无法重转，只能重新训练）。
- `meta.json` 的 `sha256` 用于导入去重与完整性核对；删除后下次启动重扫会重新生成。
- `model/face/` 的**两件套权重**（detection/model.rknn 和 recognition/model.rknn）缺失时，人员点名初始化会失败
  （`initRollCallService` 返回 false；`face_recognition` exe 已降级为 CLI 对照工具，缺失不影响点名运行）；
  盘点权重改从 `library/` 解析，库中无任何可用盘点模型（ppe/明火/工器具都未入库）时 `initEquipmentService`
  返回 false——均为弹警告降级，视频监控主流程不受影响。
- 标签 txt 每行一个类名、不要带引号或逗号；新增类别后需重新导出模型保持 cls_id 对齐。
