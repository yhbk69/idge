# model

## 功能概述

运行时模型与标签资源目录（不参与编译，随程序部署）。存放 YOLO11 系列 RKNN 模型（NPU 推理）、COCO/专项类别标签文件及少量检测验证用测试图片/视频。当前工作区为**扁平结构**（12 个文件，无子目录）；注意人员点名/设备盘点模块还引用了 `model/face/`、`model/coco/`、`model/fire/` 三个**尚未随仓库提供**的子目录（见下文），部署这些功能时需自行补齐。

## 文件/子目录清单

### 实际存在的文件（以 ls 为准）

| 文件 | 大小约 | 说明 |
|------|--------|------|
| `yolo11n.rknn` | 4.2 MB | YOLO11n INT8 模型，**默认模型**：`config.json` 的 `model.path` 与 `cascade.models[].path`、`ConfigManager` 缺省值、CLI `-m` 示例均指向它 |
| `yolo11s.rknn` | 11 MB | YOLO11s 模型（精度优先备选） |
| `yolo11m.rknn` | 23 MB | YOLO11m 模型（对照实验用） |
| `coco_80_labels_list.txt` | 621 B | COCO 80 类标签，与 yolo11*.rknn 配套（`main.cpp` CLI 固定读取此路径） |
| `person_labels.txt` / `helmet_labels.txt` / `vest_labels.txt` | <20 B | 行人/安全帽/反光背心单类专项模型标签（对应 ThreadPool 注释中的 person/helmet/vest 级联方案，模型文件本身未入库） |
| `dataset.txt` | 7 B | 单行内容 `bus.jpg`，量化/推理小样本清单 |
| `bus.jpg`、`test.jpg` | — | 检测验证测试图（python/yolo11.py 的 `--img_folder ../model` 默认扫这些图） |
| `1.mp4` | 1.7 MB | 短视频测试样本（视频通道调试用） |

### 代码引用但仓库中不存在的子目录（需部署时补建）

| 路径 | 引用位置 |
|------|----------|
| `model/face/face_recognition`、`model/face/detection.rknn`、`model/face/recognition.rknn` | `src/form/frmmain.cpp:510-512`（RollCallService 初始化） |
| `model/coco/rknn_yolo11_demo`、`model/coco/model/yolo11.rknn`、`model/coco/model/coco_80_labels_list.txt` | `frmmain.cpp:525-527`（设备盘点 coco 模型） |
| `model/fire/rknn_yolo11_demo`、`model/fire/model/yolo11.rknn`、`model/fire/model/coco_80_labels_list.txt` | `frmmain.cpp:530-532`（设备盘点 fire 模型） |

## 使用方法

路径均相对于程序工作目录（主界面 `frmMain` 使用 `workspace_`，CLI 模式在仓库根目录运行）。典型用法：

```bash
# CLI 检测（main.cpp 头部注释的正式用法；labels 固定读 model/coco_80_labels_list.txt）
./idge -m model/yolo11n.rknn -i 192.mp4

# GUI：config.json 的 model.path / cascade.models[].path 填本目录相对或绝对路径
#   "model": { "path": "model/yolo11n.rknn", "label": "model/coco_80_labels_list.txt", ... }

# python 侧验证（见 python/README.md）
python3 yolo11.py --model_path ../model/yolo11n.rknn --target rk3588 --img_folder ../model

# 模型转换输出目标也在本目录
python3 convert.py yolo11n.onnx rk3588 i8 ../model/yolo11n.rknn
```

## 依赖关系

- `.rknn` 由 `python/convert.py`（rknn-toolkit2）在 x86 PC 上生成，平台绑定 rk3588；
- 加载方：`src/yolo11/`（YOLO11Model，经 `3rdparty/rknpu2` librknnrt 运行时推理）、`src/service/`（点名/盘点走外部 demo 二进制 + 模型子目录）、`src/threadpool/`（级联多模型方案）；
- 标签文件行序必须与模型输出 `cls_id` 一致（COCO 80 类顺序）；
- 预处理约定 640×640、/255 归一化，与 `config.json detect` 段（conf 0.25 / nms 0.45）配套。

## 注意事项

- **模型与驱动版本强绑定**：librknnrt.so 过旧会报 RKNN_ERR_MODEL_INVALID，换板/换固件后先跑 CLI 冒烟。
- `model/face|coco|fire/` 子目录缺失时，人员点名/设备盘点初始化会失败（`initRollCallService/initEquipmentService` 返回 false），但视频监控主流程不受影响——属预期降级，勿误判为程序 bug。
- RKNN 模型文件较大（合计约 38 MB），仓库若做 LFS/打包部署，注意 `.gitignore` 与 install 脚本是否排除本目录。
- 标签 txt 每行一个类名、不要带引号或逗号；新增类别后需重新导出模型保持 cls_id 对齐。
- 测试图/视频（bus.jpg、test.jpg、1.mp4）仅为验证素材，生产部署镜像可剔除。
