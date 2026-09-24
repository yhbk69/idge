# docs

## 功能概述

项目知识文档目录。核心内容是 `knowledge/` 下按学习/开发阶段整理的 RK3588 平台技术笔记（多媒体硬解、NPU 推理与优化、RGA 加速、模型转换、部署方案等），以及若干图解 HTML 与专题子目录。本 README 只做**索引汇总**；`knowledge/4k/README.md` 与 `knowledge/artifacts/README.txt` 已有各自的详细索引。

## 文件/子目录清单

### docs/ 根

| 文件/目录 | 说明 |
|-----------|------|
| `knowledge/` | 技术文档主体（下表） |
| `ref.txt` | 参考资料/链接备忘 |
| `bug.txt` | 缺陷记录备忘 |

### knowledge/ 专题文档（Markdown）

| 文档 | 主题 |
|------|------|
| `第三阶段-设备树入门.md` | 设备树基础（UART/I2C/SPI 描述） |
| `第五阶段-MPP硬解.md` | MPP 硬件解码方案、FFmpeg 对接 rkmpp |
| `第五阶段-多媒体硬解完成记录.md` | 多媒体硬解阶段完成记录 |
| `第六阶段-YOLOv8模型转换为RKNN格式.md` | WSL2 上 ONNX→RKNN 转换记录 |
| `第六阶段-YOLOv8 NPU推理(板子端).md` | 板端 RKNN NPU 推理记录 |
| `第六阶段-YOLOv8摄像头实时推理.md` | 摄像头实时推理链路 |
| `RGA预处理加速方案.md` | RGA 硬件加速推理预处理（评估完成待实施） |
| `NPU官方demo测速对比记录.md` | rknn_model_zoo 官方 demo 耗时对比 |
| `YOLOv5s与YOLOv8s-NPU耗时对比测试记录.md` | 两代模型 NPU 耗时对比 |
| `YOLOv8s-RK3588推理性能优化路线.md` | 推理性能优化路线图 |
| `YOLOv8s-RK3588推理优化实验记录.md` | 按实验编号的优化实验记录（含失败实验） |
| `YOLOv8s-RK3588桌面六类数据集与微调方案.md` | 桌面六类数据集设计、标注与微调全流程规范 |
| `检测框消失问题排查记录.md` | 检测框消失问题排查 |
| `AlertGateway项目技术总结.md` | AlertGateway 桌面物品检测系统技术总结 |
| `baseline_deployment_verification_2026-07-11.md` | 基线部署验证报告 |
| `Docker打包与部署方案.md` | WSL 交叉编译 + 板端的 Docker 化改造规划 |
| `HDMI-Sunshine自动切换方案.md` | RK3588S HDMI/Sunshine 远程桌面切换 |
| `WebRTC远程桌面方案.md` | RK3588S WebRTC 远程桌面架构 |

### knowledge/ 图解 HTML（浏览器打开）

| 文件 | 主题 |
|------|------|
| `H264封装格式转换图解.html` | H.264 封装/流转格式图解 |
| `NV12渲染原理图解.html` | NV12 → EGL/OpenGL 渲染原理图解 |
| `optimization_storyboard.html` | 优化过程分镜汇总 |

### knowledge/ 子目录

| 目录 | 说明 |
|------|------|
| [`4k/`](knowledge/4k/README.md) | 4K 拉流专题：方案设计、帧率/码率验证、单双多路兼容计划等 7 个文件，**索引见其 README.md** |
| [`artifacts/`](knowledge/artifacts/README.txt) | YOLOv8s 结构实验工件（基线 YAML、通道重要性、剪枝/蒸馏方案 JSON/TSV），**分类说明见其 README.txt** |
| `architecture/` | `alertgateway_architecture.html/.svg` 系统架构图 |
| `NPU全局调度/` | `NPU全局调度设计.md`、`NPU全局调度开发计划.md`（2026-07-21） |

## 使用方法

```bash
# 索引入口（4k 与 artifacts 两个专题先读各自 README）
less docs/knowledge/4k/README.md
cat  docs/knowledge/artifacts/README.txt

# HTML 图解用浏览器打开（含图片/样式引用，勿单独移动）
xdg-open docs/knowledge/architecture/alertgateway_architecture.html
```

注意：`4k/README.md` 中部分链接指向仓库外的历史工作区路径（`../../runs/...`、`../../tools/...`），这些产物目录未随本仓库收录，链接仅作溯源记录。

## 依赖关系

- 文档之间互相引用（阶段系列、artifacts 被优化路线/实验记录引用）；
- 内容与代码目录对应：MPP/FFmpeg → `src/media/reader`，RGA → `src/media/rga`，NPU → `src/ai/yolo11`、`3rdparty/rknpu2`，模型实验 → `python/`；
- 本 README 为汇总层，不复制专题 README 的正文，避免双处维护。

## 注意事项

- 文档多为**阶段性记录**（含日期），性能数字基于当时固件/驱动/模型版本，复测前先看 `baseline_deployment_verification_*.md` 的环境说明。
- 文件名混合中英文与括号，脚本处理时用引号包裹路径。
- 新增文档请沿用"第N阶段-主题.md"或"主题_日期.md"命名，并回来更新本索引。
- `docs/` 不参与编译，构建系统（CMake GLOB 仅收集 `src/`、`res/`）不受这些文件影响。
