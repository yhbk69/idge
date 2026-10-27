# res

## 功能概述

主程序的 Qt 资源目录：界面导航图标 PNG、图标字体 TTF、GL 着色器参考副本，经 `main.qrc` 编译进可执行文件，运行时以 `:/image/...`、`:/font/...`、`:/gl/...` 访问。注意：**拍照快门音效 `shutter.wav` 不在本目录**，而在 `sounds/`（由 `sounds/resources.qrc` 注册为 `qrc:/shutter.wav`，`src/ui/form/photo_selection_widget.cpp` 的 QSoundEffect 使用）。

## 文件/子目录清单

| 文件/目录 | 资源路径 | 说明 |
|-----------|----------|------|
| `main.qrc` | — | 资源清单，注册下列全部文件（prefix=`/`） |
| `image/main_*.png`（9 个） | `:/image/main_*.png` | 顶部导航按钮图标。**文件名是模板遗留语义，与当前标签不一一对应**，实际配对见下表 |
| `image/1.png` | `:/image/1.png` | 已注册但当前 src 内未发现引用（备用/历史素材，390KB 全在可执行文件里） |
| `font/iconfont.ttf` | `:/font/iconfont.ttf` | 阿里巴巴图标字体（IconHelper 使用） |
| `font/fontawesome-webfont.ttf` | `:/font/fontawesome-webfont.ttf` | FontAwesome 图标字体（IconHelper 使用） |
| `gl/vertex.vsh`、`gl/fragment.fsh` | `:/gl/...` | **参考副本**：`#version 300 es` 的 YUV 三采样器（tex_y/tex_u/tex_v）着色器。运行时 `src/ui/widgets/gl_video_widget.cpp` 内联的 `VS_SRC/FS_SRC`（GL ES 2.0 风格、单 sampler2D 直接采样 DMA-BUF 导入纹理）才是真正编译的那份，两套代码内容也不同 |

### 导航图标实际配对（`frmmain.cpp` 运行时 `setIcon` + `.ui` 的 btnSmart）

| 资源 | 图形 | 按钮 | 标签 |
| --- | --- | --- | --- |
| `main_main.png` | 盾牌 | `btnMain` | 视频监控 |
| `main_help.png` | 分栏面板 | `btnRoll` | 数据看板 |
| `main_data.png` | 放大镜 | `btnEquipCheck` | 报警数据 |
| `main_person.png` | 人形 | `btnData` | 人员点名 |
| `main_company.png` | 楼宇 | `btnConfig` | 设备盘点 |
| `main_smart.png` | 扫码框 | `btnSmart`（`.ui` 直接引用） | 智能识别 |
| `main_config.png` | 齿轮 | `btnHelp` | 调试设置 |
| `main_exit.png` | 出口 | `btnExit` | 退出 |
| `main_about.png` | — | 未使用（模板遗留） | — |

`btnData`/`btnConfig` 在 `initNewPages()` 里先 `hide()`（收敛占位按钮），再由
`initBusinessPages()` 换标签换图标 `show()` 回来——改导航时两处都要看，只改一处会出现
"图标对不上标签"或按钮消失。

## 使用方法

`main.qrc` 由根 `CMakeLists.txt` 的 GLOB 自动收编（第 310~317 行 `file(GLOB PROJECT_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/res/*.qrc ...)` 进入 `PROJECT_SOURCES`，配合 `CMAKE_AUTORCC ON` 自动 rcc）。日常只需：

1. 把新图片放入 `res/image/`，在 `main.qrc` 增加 `<file>image/xxx.png</file>`；
2. 重新执行 `./build-linux.sh -t rk3588 -b Release` 即可。

UI 中的引用写法（已核实示例）：`frmmain.ui` iconset 使用 `:/image/main_main.png`；Qt Designer 属性面板可直接输入该路径。

## 依赖关系

- 被 `src/ui/form/frmmain.ui`（图标）、`src/ui/core_helper/iconhelper.cpp`（`:/font/iconfont.ttf`、`:/font/fontawesome-webfont.ttf`）间接依赖；
- 与 `sounds/resources.qrc`（快门音）、`src/ui/core_qss/qss.qrc`（皮肤）并列为工程的三个资源入口；
- `gl/*.vsh/.fsh` 为 OpenGL ES 3.0 语法（`#version 300 es`），当前运行时着色器为 ES 2.0 语法内联版，仅作 YUV 渲染方案留档。

## 注意事项

- **`image/*.png` 必须在库里**：根 `.gitignore` 全局忽略 `*.png`，`res/image/*.png` 是显式
  豁免（`!res/image/*.png`）。删掉这条豁免或漏提交任一图标，`rcc` 会以
  `RCC: Error in 'main.qrc': Cannot find file 'image/xxx.png'` **直接退出**，新克隆构建即失败
  （不是"图标显示不出来"这种软失败）。新增导航图标时同步确认这条规则仍然覆盖到它。
- **字体别名重复注册**：`:/font/iconfont.ttf` 与 `:/font/fontawesome-webfont.ttf` 同时存在于 `res/main.qrc` 和 `src/ui/core_helper/qrc/font.qrc`，Qt 资源系统对重复 alias 只保留先注册者（rcc 阶段可能出 duplicate 提示）。更新字体时**两处必须同步替换**，否则可能加载到旧副本。
- 修改 `main.qrc` 后若 IDE 未感知，需重新 configure（GLOB 带 CONFIGURE_DEPENDS，通常自动重跑；qmake 场景无此保证）。
- `res/gl/*.vsh/.fsh` 改动**不影响渲染**（运行时从不按路径加载它们）；调整渲染逻辑请直接改 `gl_video_widget.cpp` 内联 `VS_SRC/FS_SRC`，改完可顺手同步留档副本。
- 资源全部内嵌二进制，大图会显著增大可执行文件与启动内存；新增资源前考虑是否适合放文件系统。
- 文件路径大小写敏感（Linux 目标机），qrc 中登记的路径必须与磁盘完全一致。
