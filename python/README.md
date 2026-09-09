# Python 工具使用说明

## 文件说明

| 文件 | 功能 |
|------|------|
| `convert.py` | ONNX 模型转换为 RKNN 格式（在 PC 上运行） |
| `yolo11.py` | YOLO11 推理测试和 COCO mAP 评估 |

---

## 1. 环境安装

### 在 x86 PC 上安装（用于转换）

```bash
# 创建虚拟环境（推荐）
conda create -n rknn python=3.10
conda activate rknn

# 安装 rknn_toolkit（转换工具）
pip install rknn_toolkit

# 安装其他依赖
pip install numpy opencv-python
```

### 在 RK3588 上安装（用于推理）

```bash
# RK3588 开发板上已预装 Python 和依赖
# 如果需要重新安装：
pip install numpy opencv-python
```

---

## 2. 模型转换（convert.py）

### 功能

将 YOLO11/YOLOv8 的 ONNX 模型转换为 Rockchip RKNN 格式，用于 NPU 硬件加速。

### 使用方法

```bash
python3 convert.py <onnx_model_path> <platform> [dtype] [output_rknn_path]
```

### 参数说明

| 参数 | 必填 | 说明 |
|------|------|------|
| `onnx_model_path` | 是 | ONNX 模型路径（如 `yolo11n.onnx`） |
| `platform` | 是 | 目标平台：`rk3562` / `rk3566` / `rk3568` / `rk3588` / `rk3576` |
| `dtype` | 否 | 量化类型：`i8`（INT8 量化，默认）/ `fp`（FP16 保留） |
| `output_rknn_path` | 否 | 输出路径（默认 `../model/yolo11.rknn`） |

### 示例

```bash
# INT8 量化（推荐，模型小、速度快）
python3 convert.py yolo11n.onnx rk3588 i8 ../model/yolo11n.rknn

# FP16 保留（精度更高，但模型大）
python3 convert.py yolo11n.onnx rk3588 fp ../model/yolo11n_fp16.rknn

# 使用默认参数
python3 convert.py yolo11n.onnx rk3588
```

### 量化说明

| 类型 | 模型大小 | 推理速度 | 精度 |
|------|----------|----------|------|
| `i8` (INT8) | 原始的 1/4 | 更快 | 略有损失 |
| `fp` (FP16) | 原始的 1/2 | 较快 | 无损失 |

**注意**：INT8 量化需要校准数据集，项目使用 `coco_subset_20.txt`（20 张 COCO 图片路径）。

---

## 3. 推理测试（yolo11.py）

### 功能

加载模型对图片进行推理，支持 RKNN/ONNX/PyTorch 三种格式。

### 使用方法

```bash
python3 yolo11.py --model_path <模型路径> [其他参数]
```

### 参数说明

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--model_path` | 模型路径（必填） | - |
| `--target` | 目标平台 | `rk3566` |
| `--device_id` | 设备 ID | `None` |
| `--img_show` | 显示检测结果 | `False` |
| `--img_save` | 保存检测结果 | `False` |
| `--img_folder` | 图片文件夹路径 | `../model` |
| `--coco_map_test` | 启用 COCO mAP 评估 | `False` |

### 示例

```bash
# 在 RK3588 上测试 RKNN 模型
python3 yolo11.py --model_path ../model/yolo11n.rknn --target rk3588 --img_folder ../model

# 在 PC 上测试 ONNX 模型
python3 yolo11.py --model_path yolo11n.onnx --img_folder ../model

# 显示并保存结果
python3 yolo11.py --model_path ../model/yolo11n.rknn --target rk3588 --img_show --img_save

# COCO mAP 评估
python3 yolo11.py --model_path ../model/yolo11n.rknn --target rk3588 --coco_map_test
```

---

## 4. 完整工作流程

### 步骤 1：下载预训练模型

```bash
# 从 Ultralytics 下载 YOLO11 ONNX 模型
pip install ultralytics
yolo export model=yolo11n.pt format=onnx
```

### 步骤 2：转换为 RKNN 格式（在 PC 上）

```bash
python3 convert.py yolo11n.onnx rk3588 i8 ../model/yolo11n.rknn
```

### 步骤 3：在 RK3588 上测试

```bash
python3 yolo11.py --model_path ../model/yolo11n.rknn --target rk3588 --img_show
```

### 步骤 4：部署到 idge 系统

将生成的 `.rknn` 文件拷贝到 `model/` 目录，idge 程序会自动加载。

---

## 5. 常见问题

### Q: 转换必须在 RK3588 上运行吗？

**不需要**。转换在 x86 PC 上完成，生成的 `.rknn` 文件拷贝到 RK3588 上运行。

### Q: 量化后精度下降怎么办？

- 尝试 `fp`（FP16）模式，不量化
- 增加校准数据集数量（修改 `DATASET_PATH`）
- 使用更大的模型（如 `yolo11s` 替代 `yolo11n`）

### Q: 如何获取 ONNX 模型？

```bash
# 方法 1：从 Ultralytics 导出
yolo export model=yolo11n.pt format=onnx

# 方法 2：从 GitHub Release 下载
# https://github.com/airockchip/rknn_model_zoo
```

### Q: 支持哪些 YOLO 版本？

- YOLOv8 / YOLOv11（推荐）
- YOLOv5 / YOLOv7（需要修改后处理代码）

---

## 6. 目录结构

```
python/
├── convert.py          # 模型转换脚本
├── yolo11.py           # 推理测试脚本
├── README.md           # 本说明文件
└── result/             # 检测结果输出目录（自动创建）
```

---

## 7. 相关资源

- [RKNN Toolkit 文档](https://github.com/airockchip/rknn-toolkit2)
- [RKNN Model Zoo](https://github.com/airockchip/rknn_model_zoo)
- [Ultralytics YOLO11](https://docs.ultralytics.com/models/yolo11/)
