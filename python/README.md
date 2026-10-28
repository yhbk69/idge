# python

## 功能概述

PC 侧（x86 Ubuntu/WSL）模型工具链脚本，来自 rknn_model_zoo 的 YOLO11 示例并做了本项目适配：

- `pt2rknn.sh`：**从 `.pt` 一步做到 `model/library/<id>/model.rknn`**（内部串起 ultralytics 导出 ONNX 与 `convert.py`）；
- `convert.py`：将 YOLO11/YOLOv8 导出的 **ONNX 模型转换为 RKNN 格式**（配置归一化、目标平台、INT8/FP16 量化、导出 .rknn）；
- `yolo11.py`：RKNN/ONNX 模型图片推理验证 + 可选 **COCO mAP 评估**（LetterBox 预处理、DFL 解码、NMS 后处理），用于核对转换前后精度。

模型在板上由主程序 `idge`（`src/ai/yolo11/`、`3rdparty/rknpu2` 运行时）加载推理。

## 文件/子目录清单

| 文件 | 说明 |
|------|------|
| `pt2rknn.sh` | `.pt → onnx → rknn` 一步脚本，产物直接落 `model/library/<库id>/model.rknn`，并在结束时打印入库后续步骤；**只能在 x86_64 Linux/WSL 上跑** |
| `convert.py` | ONNX→RKNN 转换脚本；量化清单取环境变量 `DATASET`（默认 `../model/train/calib/dataset_test.txt`，仓库里那份是占位图）；**输出路径必填**（模型库约定 `library/<id>/model.rknn`，已去掉 `../model/yolo11.rknn` 这种默认输出）；默认 INT8 量化 |
| `yolo11.py` | 推理验证脚本；置信度阈值 0.25、NMS 0.45、输入 640×640（与 `config.json` 的 detect 段一致） |
| `result/` | `--img_save` 时自动创建的检测结果输出目录（脚本内 `os.mkdir('./result')`） |
| `README.md` | 本说明 |

## 使用方法

环境：Python 3.8~3.10 + `rknn-toolkit2`（提供 `from rknn.api import RKNN`），以及 `numpy`、`opencv-python`；`pt2rknn.sh` 另需 `ultralytics`；`yolo11.py` 还需 `rknn_model_zoo` 仓库布局（脚本按路径回溯 `rknn_model_zoo` 目录并 import `py_utils.coco_utils`）。

```bash
# 0) 一步到位（推荐，仓库根目录下执行）：.pt → model/library/<id>/model.rknn
#    用法: ./python/pt2rknn.sh <model.pt> <库id> [platform=rk3588] [dtype=i8|u8|fp] [校准清单]
./python/pt2rknn.sh model/train/cigarette/yolo11n-cigarette.pt yolo11n-cigarette
DATASET=~/calib/smoke.txt ./python/pt2rknn.sh best.pt yolo11n-cigarette rk3588 i8

# 1) 手工分步 —— ONNX 导出（imgsz 必须 640，与 C++ 端 letterbox 640x640 同口径）
yolo export model=yolo11n.pt format=onnx imgsz=640 simplify=True opset=12

# 2) 手工分步 —— ONNX → RKNN（cd 进 python/ 再跑；输出路径必填，直接落库）
#    用法: python3 convert.py <onnx路径> <平台> [i8|u8|fp] <输出.rknn路径>
python3 convert.py yolo11n.onnx rk3588 i8 ../model/library/yolo11n-coco/model.rknn
python3 convert.py yolo11n.onnx rk3588 fp  ../model/library/yolo11n-coco/model.rknn   # 不量化
# 平台可选：rk3562 / rk3566 / rk3568 / rk3588 / rk3576
# 校准清单：DATASET=~/calib/xxx.txt python3 convert.py ...（默认用仓库内占位清单）

# 3) 推理验证（板端或 PC；argparse 命名参数）
python3 yolo11.py --model_path ../model/library/yolo11n-coco/model.rknn --target rk3588 --img_folder ../assets/test
python3 yolo11.py --model_path ../model/library/yolo11n-coco/model.rknn --target rk3588 --img_show --img_save
python3 yolo11.py --model_path yolo11n.onnx            # PC 上跑 ONNX 对照
python3 yolo11.py --model_path ../model/library/yolo11n-coco/model.rknn --target rk3588 --coco_map_test
```

`yolo11.py` 全部参数：`--model_path`(必填) `--target`(默认 rk3566) `--device_id` `--img_show` `--img_save` `--anno_json`(默认 `../../../datasets/COCO/annotations/instances_val2017.json`) `--img_folder`(默认 `../model`) `--coco_map_test`。

## 依赖关系

- 上游产物：Ultralytics 的 `.pt`（放在 `model/train/<任务>/`，见 `model/train/README.md`），
  导出 ONNX 用 `yolo export model=yolo11n.pt format=onnx imgsz=640`（或直接 `pt2rknn.sh`）；
- `convert.py` → rknn-toolkit2（仅 PC 用，板端 aarch64 装不上也不需要）；
- 转换产物（`.rknn` + 类别表）**收编进 `model/library/<库id>/`**（文件名固定 `model.rknn` / `labels.txt`），
  config.json 只在 `video.channels[].models` 里按**库目录名**引用它，不写路径（schema V2）；
  也可走设置页「模型管理 → 导入模型」。老配置里平铺在 `model/` 根的 `.rknn` 路径
  由 `ConfigManager::migrateSchemaV2()` 反查库 id 后改写（**只改配置引用，不搬文件**，
  文件本身要人工放进入库目录）；
- 量化校准清单默认在仓库内（`model/train/calib/`）；`yolo11.py` 的 COCO 标注
  `--anno_json` 仍指向 rknn_model_zoo 的 `../../../datasets/COCO/` 相对路径（只有 mAP 评估用到）。

## 注意事项

- **INT8 量化必须提供校准集**：`rknn.build(do_quantization=True, dataset=DATASET_PATH)`。
  路径相对脚本运行目录（惯例 `cd python/`），默认 `../model/train/calib/dataset_test.txt`
  只是跑通流程的占位图（`assets/test` 里的通用图），**换场景模型必须用自己的校准集**：
  `DATASET=~/calib/xxx.txt python3 convert.py ...`，不要去改脚本里的默认值。
- 归一化在转换期配置为 `mean_values=[[0,0,0]] / std_values=[[255,255,255]]`，即 NPU 输入前做 /255；C++ 端预处理必须与此一致，否则精度异常。
- 转换只在 x86 PC 完成，`.rknn` 与目标平台绑定（rk3588 生成的模型不能跑在 rk3568 上）。
- `yolo11.py` 依赖同级 rknn_model_zoo 代码（`py_utils/`、模型 zoo 路径回溯），本仓库单独拷贝 python/ 目录到任意位置运行会 ImportError。
- `--img_save` 结果写入**当前工作目录**下的 `result/`（非 python/result），注意先 cd 到期望位置。
- 若量化后掉点明显：改用 `fp`、增大校准集、或换更大模型（n→s→m，`model/library/` 已备好 coco 三档）。
