# tools/face_recognition — 人脸识别命令行工具

SCRFD 人脸检测 + ArcFace 特征提取的独立可执行工具。人员点名（roll_call）链
2026-09 已进程内化（`src/ai/recognition/in_process_face_recognizer.cpp` 逐段移植
本工具推理逻辑），主程序不再调起本 exe；保留作 CLI 手工验证工具与
`tests/test_rollcall.cpp` 对照回归的移植前基准（经 `FaceRecognitionWrapper`
fork+execv 调起）。原目录名 `cc_face`，2026-09 迁入 tools/ 并接入根 CMake 主构建。

## 构建

```bash
cd build/build_rk3588_linux && cmake . && make -j8 face_recognition
```

产物固定输出到 `model/face/face_recognition`（`test_rollcall` 对照回归按此路径寻找基准工具，勿改）。

## 命令行接口

```
face_recognition <det_model.rknn> <rec_model.rknn> <image.jpg> [output.json] [score_thresh=0.5] [nms_thresh=0.4]
```

输出 JSON：人脸框 + 5 关键点 + 512 维特征向量（另有 `<image>_result.jpg` 画框回显）。
点名生产链阈值口径为 0.6/0.4（进程内识别器与旧 wrapper 同值）。

## 模型权重（需手工放置，仓库不收录 .rknn）

```
model/face/
├── detection.rknn       # SCRFD 检测模型（外部获取；点名进程内链路与本工具共用）
├── recognition.rknn     # ArcFace 识别模型（外部获取；同上）
└── face_recognition     # 本工具构建产物（仅 CLI 验证/对照回归需要）
```

点名功能只依赖两件 .rknn 权重（进程内推理）；缺失时主程序初始化失败并弹窗提示。

## 文件

- `main.cpp` — 本工具源码；同时是 `src/ai/recognition/in_process_face_recognizer.cpp` 的移植源，**推理逻辑修改须两侧同步**（否则 `test_rollcall` 对照回归首先失败）
- `face_align.h` — 对齐实验件，当前未被引用，保留备查
- `experimental/` — 历史实验件：精度对比、关键点调试、ONNX 转换与对齐测试脚本，不参与构建
