# src/ — 源码功能域结构

2026-09 目录重组后，`src/` 按功能域分为五组（模块化原则：**每个域单一职责、依赖单向**）。

```
src/
├── main.cpp  head.h          # 入口：CLI/GUI 双模式、EGL 初始化、data/ 旧布局迁移
├── base/     基础设施域       # buffer queue threadpool config utils runtime_paths.h
├── media/    视频管线域       # reader(FFmpeg解码/录像) rga model(ModelPool)
├── ai/       推理与模型应用域  # yolo11 model_repo recognition task
├── biz/      业务域           # alarm geofence service db(报警库+业务库)
└── ui/       界面域           # form widgets(原src/ui) core_helper core_qss
```

## 依赖方向（禁止反向）

```
base ← media ← ai ← biz ← ui
```

- **base**：零业务依赖。队列/DMA 缓冲/线程池/配置解析/通用工具/运行时路径。
- **media**：解码、色彩转换、帧模型池。产出帧与检测结果，不含业务判定。
- **ai**：RKNN 推理、模型库注册、检测任务、人脸桥接（调 `tools/face_recognition` 产物）。
- **biz**：报警/围栏/点名/盘点业务逻辑与两套持久层（QtSQL `data/idge.db` + sqlite3 `roll_call.db`）。
- **ui**：窗口、控件、渲染。仅被 main.cpp 装配。

同域内模块互引允许；跨域只允许右侧引左侧。

## include 约定

一律**平铺**（`#include "xxx.h"`），由根 `CMakeLists.txt` 的 per-module `include_directories` 解析；
禁止 `../` 相对与模块前缀写法（重组中已全部清除，新增代码请保持）。

## 运行时数据

所有运行时产物路径的唯一事实源是 `base/runtime_paths.h`（`data/` 约定 + 旧布局自动迁移），
业务代码不得写路径字面量。模型仓库 `model/` 遵循外部约定，不在 data/ 内。

各模块细节见各级 `README.md`；根 [README](../README.md) 第四节有全量索引。
