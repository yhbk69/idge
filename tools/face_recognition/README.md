# tools/face_recognition — 人脸识别命令行工具

SCRFD 人脸检测 + ArcFace 特征提取的独立可执行工具，由主程序
`src/ai/recognition/face_recognizer.cpp`（FaceRecognitionWrapper）经 fork+execv 调起，
服务于人员点名（roll_call）功能。原目录名 `cc_face`，2026-09 迁入 tools/ 并接入根 CMake 主构建。

## 构建

```bash
cd build/build_rk3588_linux && cmake . && make -j8 face_recognition
```

产物自动输出到 `model/face/face_recognition`（主程序约定路径，勿改）。

## 命令行接口

```
face_recognition <det_model.rknn> <rec_model.rknn> <image.jpg> [output.json] [score_thresh=0.5] [nms_thresh=0.4]
```

输出 JSON：人脸框 + 5 关键点 + 512 维特征向量。

## 模型权重（需手工放置，仓库不收录 .rknn）

```
model/face/
├── face_recognition     # 本工具构建产物
├── detection.rknn       # SCRFD 检测模型（外部获取）
└── recognition.rknn     # ArcFace 识别模型（外部获取）
```

三件套齐备后点名功能才可用；缺失时主程序会回退提示。

## 文件

- `main.cpp` / `face_align.h` — 生产源码（后者当前未被引用，保留备查）
- `experimental/` — 历史实验件：精度对比、关键点调试、ONNX 转换与对齐测试脚本，不参与构建
