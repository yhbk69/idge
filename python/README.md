# python

## 功能概述

PC 侧（x86 Ubuntu/WSL）模型工具链脚本，来自 rknn_model_zoo 的 YOLO11 示例并做了本项目适配：

- `convert.py`：将 YOLO11/YOLOv8 导出的 **ONNX 模型转换为 RKNN 格式**（配置归一化、目标平台、INT8/FP16 量化、导出 .rknn）；
- `yolo11.py`：RKNN/ONNX 模型图片推理验证 + 可选 **COCO mAP 评估**（LetterBox 预处理、DFL 解码、NMS 后处理），用于核对转换前后精度。

模型在板上由主程序 `idge`（`src/yolo11/`、`3rdparty/rknpu2` 运行时）加载推理。

## 文件/子目录清单

| 文件 | 说明 |
|------|------|
| `convert.py` | ONNX→RKNN 转换脚本；默认量化数据集 `DATASET_PATH='../../../datasets/COCO/coco_subset_20.txt'`，默认输出 `../model/yolo11.rknn`，默认 INT8 量化 |
| `yolo11.py` | 推理验证脚本；置信度阈值 0.25、NMS 0.45、输入 640×640（与 `config.json` 的 detect 段一致） |
| `result/` | `--img_save` 时自动创建的检测结果输出目录（脚本内 `os.mkdir('./result')`） |
| `README.md` | 本说明 |

## 使用方法

环境：Python 3.8~3.10 + `rknn-toolkit2`（提供 `from rknn.api import RKNN`），以及 `numpy`、`opencv-python`；`yolo11.py` 还需 `rknn_model_zoo` 仓库布局（脚本按路径回溯 `rknn_model_zoo` 目录并 import `py_utils.coco_utils`）。

```bash
# 1) ONNX → RKNN（INT8 量化，4 参数按位置传入）
#    用法: python3 convert.py <onnx路径> <平台> [i8|u8|fp] [输出.rknn路径]
python3 convert.py yolo11n.onnx rk3588 i8 ../model/yolo11n.rknn

# FP16（不量化）
python3 convert.py yolo11n.onnx rk3588 fp ../model/yolo11n_fp16.rknn

# 省略后两个参数时：默认 i8 量化、输出 ../model/yolo11.rknn
python3 convert.py yolo11n.onnx rk3588
# 平台可选：rk3562 / rk3566 / rk3568 / rk3588 / rk3576

# 2) 推理验证（板端或 PC；argparse 命名参数）
python3 yolo11.py --model_path ../model/yolo11n.rknn --target rk3588 --img_folder ../model
python3 yolo11.py --model_path ../model/yolo11n.rknn --target rk3588 --img_show --img_save
python3 yolo11.py --model_path yolo11n.onnx            # PC 上跑 ONNX 对照
python3 yolo11.py --model_path ../model/yolo11n.rknn --target rk3588 --coco_map_test
```

`yolo11.py` 全部参数：`--model_path`(必填) `--target`(默认 rk3566) `--device_id` `--img_show` `--img_save` `--anno_json`(默认 `../../../datasets/COCO/annotations/instances_val2017.json`) `--img_folder`(默认 `../model`) `--coco_map_test`。

## 依赖关系

- 上游产物：Ultralytics 导出 `yolo export model=yolo11n.pt format=onnx`；
- `convert.py` → rknn-toolkit2（仅 PC 用，板端不需要）；
- 转换输出放入根目录 `model/`，由 `config.json` 的 `model.path` / `cascade.models[].path` 引用（默认 `model/yolo11n.rknn` + `model/coco_80_labels_list.txt`）；
- 量化校准数据集与 COCO 标注均指向 rknn_model_zoo 的 `../../../datasets/COCO/` 相对路径。

## 注意事项

- **INT8 量化必须提供校准集**：`rknn.build(do_quantization=True, dataset=DATASET_PATH)`，DATASET_PATH 是相对脚本运行目录的路径，脱离 rknn_model_zoo 目录布局单独运行会因找不到 `coco_subset_20.txt` 而失败——需先改 `DATASET_PATH`。
- 归一化在转换期配置为 `mean_values=[[0,0,0]] / std_values=[[255,255,255]]`，即 NPU 输入前做 /255；C++ 端预处理必须与此一致，否则精度异常。
- 转换只在 x86 PC 完成，`.rknn` 与目标平台绑定（rk3588 生成的模型不能跑在 rk3568 上）。
- `yolo11.py` 依赖同级 rknn_model_zoo 代码（`py_utils/`、模型 zoo 路径回溯），本仓库单独拷贝 python/ 目录到任意位置运行会 ImportError。
- `--img_save` 结果写入**当前工作目录**下的 `result/`（非 python/result），注意先 cd 到期望位置。
- 若量化后掉点明显：改用 `fp`、增大校准集、或换更大模型（n→s→m，`model/` 内已备好三档 .rknn）。
