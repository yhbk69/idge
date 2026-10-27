# 3rdparty/ppocrv4

## 功能概述

PP-OCRv4 文字识别链路的**第三方外部可执行**（rknn_model_zoo 官方 demo）。工程内不把这些
C++ 推理逻辑编进 `idge`，而是由 `src/ui/form/smart_recognition_widget.cpp` 用 `QProcess`
起进程、按行解析 stdout。模型权重与字符字典不在本目录，见"依赖关系"。

## 文件/子目录清单

| 路径 | 说明 |
|---|---|
| `bin/rknn_ppocr_det_demo` | 文字检测（DB）。CLI：`<det.rknn> <image>`，stdout 每行 `[i]: [(x,y)x4] score` |
| `bin/rknn_ppocr_system_demo` | 检测+识别一体。CLI：`<det.rknn> <rec.rknn> <image>`，stdout 每行 `regconize result: <文本>, score=<置信度>`（官方源码里就少个 c，勿改） |
| `lib/librknnrt.so` | **软链** → `../rknpu2/Linux/aarch64/librknnrt.so`（2.3.2）。两个可执行的 RUNPATH 是 `$ORIGIN/../lib`，故必须有这一层；用软链而非副本，保证与主程序链接的是同一份运行时 |

## 版本与构建配置

- 上游：`rknn_model_zoo-v2.3.0/examples/PPOCR/{PPOCR-Det,PPOCR-System}/cpp`，本机 aarch64 构建。
- 必须带的 cmake 参数，否则产物会把构建目录的绝对路径写进 RUNPATH、换目录即无法运行：

```bash
cmake -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON '-DCMAKE_INSTALL_RPATH=$ORIGIN/../lib' ..
```

- 运行时版本按 `strings lib/librknnrt.so` 实测为 `librknnrt version: 2.3.2`，与板端 NPU 驱动需配套，
  不匹配报 `RKNN_ERR_DRIVER_*`。

## 依赖关系

- 模型与字典：`model/library/ppocrv4-ocr/{det.rknn,rec.rknn,labels.txt}`（调用方拼的是
  `<workspace>/model/library/ppocrv4-ocr` 与 `<workspace>/3rdparty/ppocrv4/bin` 两个前缀）。
- 运行期入口：根 `CMakeLists.txt` 在 configure 阶段于构建目录里建 `3rdparty`/`model` 软链，
  使"启动目录即工作区根"的约定同时覆盖这两棵子树（`run.sh` 会 `cd` 到构建目录）。

## 注意事项

- `bin/`、`lib/` 已在 `.gitignore` 中排除（与其余 `3rdparty/*/lib` 一致），**新克隆需自行构建**后放回本目录。
  缺失时 `SmartRecognitionWidget::ocrModelsReady()` 返回 false，OCR 卡片自动降级为不可用，不影响其它功能。
- 两个可执行都会在**当前工作目录**写一张 `out.jpg` 可视化结果；调用方已把 `QProcess` 的工作目录
  设到系统临时目录，避免污染仓库。手工在命令行跑 demo 时注意别在源码根目录跑。
- 识别字典同时编译进了 `PPOCR-Rec` 的 `dict.h`（6625 项），`labels.txt` 只用于人工核对，
  demo 实际不使用它。
