# res

## 功能概述

主程序的 Qt 资源目录：界面导航图标 PNG、图标字体 TTF、GL 着色器参考副本，经 `main.qrc` 编译进可执行文件，运行时以 `:/image/...`、`:/font/...`、`:/gl/...` 访问。注意：**拍照快门音效 `shutter.wav` 不在本目录**，而在 `sounds/`（由 `sounds/resources.qrc` 注册为 `qrc:/shutter.wav`，`src/form/photo_selection_widget.cpp` 的 QSoundEffect 使用）。

## 文件/子目录清单

| 文件/目录 | 资源路径 | 说明 |
|-----------|----------|------|
| `main.qrc` | — | 资源清单，注册下列全部文件（prefix=`/`） |
| `image/main_main.png` 等 8 个 `main_*.png` | `:/image/main_*.png` | 顶部导航按钮图标（视频监控/人员/单位/数据/设置/退出/关于/帮助），被 `src/form/frmmain.ui` 的 iconset 引用 |
| `image/1.png` | `:/image/1.png` | 已注册但当前 src 内未发现引用（备用/历史素材） |
| `font/iconfont.ttf` | `:/font/iconfont.ttf` | 阿里巴巴图标字体（IconHelper 使用） |
| `font/fontawesome-webfont.ttf` | `:/font/fontawesome-webfont.ttf` | FontAwesome 图标字体（IconHelper 使用） |
| `gl/vertex.vsh`、`gl/fragment.fsh` | `:/gl/...` | **参考副本**：`#version 300 es` 的 YUV 三采样器（tex_y/tex_u/tex_v）着色器。运行时 `src/ui/gl_video_widget.cpp:162-163` 实际编译的是同文件 55/78 行的**内联字符串** `VS_SRC/FS_SRC`（GL ES 2.0 风格、单 sampler2D 直接采样 DMA-BUF 导入纹理），并不读取该资源路径，两套代码内容也不同 |

## 使用方法

`main.qrc` 由根 `CMakeLists.txt` 的 GLOB 自动收编（第 310~317 行 `file(GLOB PROJECT_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/res/*.qrc ...)` 进入 `PROJECT_SOURCES`，配合 `CMAKE_AUTORCC ON` 自动 rcc）。日常只需：

1. 把新图片放入 `res/image/`，在 `main.qrc` 增加 `<file>image/xxx.png</file>`；
2. 重新执行 `./build-linux.sh -t rk3588 -b Release` 即可。

UI 中的引用写法（已核实示例）：`frmmain.ui` iconset 使用 `:/image/main_main.png`；Qt Designer 属性面板可直接输入该路径。

## 依赖关系

- 被 `src/form/frmmain.ui`（图标）、`src/core_helper/iconhelper.cpp`（`:/font/iconfont.ttf`、`:/font/fontawesome-webfont.ttf`）间接依赖；
- 与 `sounds/resources.qrc`（快门音）、`src/core_qss/qss.qrc`（皮肤）并列为工程的三个资源入口；
- `gl/*.vsh/.fsh` 为 OpenGL ES 3.0 语法（`#version 300 es`），当前运行时着色器为 ES 2.0 语法内联版，仅作 YUV 渲染方案留档。

## 注意事项

- **字体别名重复注册**：`:/font/iconfont.ttf` 与 `:/font/fontawesome-webfont.ttf` 同时存在于 `res/main.qrc` 和 `src/core_helper/qrc/font.qrc`，Qt 资源系统对重复 alias 只保留先注册者（rcc 阶段可能出 duplicate 提示）。更新字体时**两处必须同步替换**，否则可能加载到旧副本。
- 修改 `main.qrc` 后若 IDE 未感知，需重新 configure（GLOB 带 CONFIGURE_DEPENDS，通常自动重跑；qmake 场景无此保证）。
- `res/gl/*.vsh/.fsh` 改动**不影响渲染**（运行时从不按路径加载它们）；调整渲染逻辑请直接改 `gl_video_widget.cpp` 内联 `VS_SRC/FS_SRC`，改完可顺手同步留档副本。
- 资源全部内嵌二进制，大图会显著增大可执行文件与启动内存；新增资源前考虑是否适合放文件系统。
- 文件路径大小写敏感（Linux 目标机），qrc 中登记的路径必须与磁盘完全一致。
