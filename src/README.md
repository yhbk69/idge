# src/ — 源码功能域结构

2026-09 目录重组后，`src/` 按功能域分为五组（模块化原则：**每个域单一职责、依赖单向**）。

```
src/
├── main.cpp  head.h          # 入口：CLI/GUI 双模式、EGL 初始化、data/ 旧布局迁移
├── base/     基础设施域       # buffer queue config imaging runtime_paths.h perf_probe.h
├── media/    视频管线域       # reader(FFmpeg硬解/硬编环形录像) rga
├── ai/       推理与模型应用域  # yolo11 model_repo recognition task
├── biz/      业务域           # alarm geofence service db(报警库+业务库) sip(GB28181) ws(Pad↔Aibox)
└── ui/       界面域           # form widgets(原src/ui) core(主题/Qt助手 + qss与字体图片资源)
```

## 依赖方向（禁止反向）

```
base ← media ← ai ← biz ← ui
```

- **base**：零业务依赖。队列/DMA 缓冲/配置解析/图像读写与绘制（`imaging/`）/运行时路径与采样开关。
- **media**：解码、色彩转换、硬件编码环形录像。产出帧与检测结果，不含业务判定。
- **ai**：RKNN 推理、模型库注册、检测任务、人脸桥接（调 `tools/face_recognition` 产物）。
- **biz**：报警/围栏/点名/盘点业务逻辑（含点名/盘点的任务目录组织 `service/task_manager`）、
  两套持久层（QtSQL `data/idge.db` + sqlite3 `roll_call.db`）、
  GB28181 设备侧推流（`sip/`，纯 UDP/TCP socket + FFmpeg 复用/打包）、Pad↔Aibox WebSocket 客户端（`ws/`）。
- **ui**：窗口、控件、渲染，以及主题/Qt 助手/皮肤与字体图片资源（`core/`）。仅被 main.cpp 装配。

同域内模块互引允许；跨域只允许右侧引左侧。

## include 约定

一律**平铺**（`#include "xxx.h"`），由根 `CMakeLists.txt` 的 per-module `include_directories` 解析；
禁止 `../` 相对与模块前缀写法（重组中已全部清除，新增代码请保持）。

## 运行时数据

所有运行时产物路径的唯一事实源是 `base/runtime_paths.h`（`data/` 约定 + 旧布局自动迁移），
业务代码不得写路径字面量。⚠ `data/` 是**相对当前工作目录**的：`run.sh` 会先 `cd` 进
`build/build_rk3588_linux`，所以经它启动时真正读写的是构建目录下的那份 `data/`，
不是源码根的 `data/`（`model/`、`3rdparty/` 由 CMake 在构建目录挂了软链回源码根，不受影响）。
模型仓库 `model/` 遵循外部约定，不在 data/ 内。

各模块细节见各级 `README.md`；根 [README](../README.md) 第四节有全量索引。
