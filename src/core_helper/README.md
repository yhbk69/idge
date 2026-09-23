# src/core_helper

## 功能概述

从 blacksoft（feiyangqingyun）框架移植的 Qt 通用基础组件集合，为 IDGE 主界面提供：

- **无边框窗体与全局拖动**：`QtHelper::setFramelessForm` + `AppInit` 全局事件过滤器（依赖窗体属性 `canMove`）；
- **图标字体（图形化按钮）**：`IconHelper` 统一管理阿里巴巴 iconfont / FontAwesome 等字体图标，支持正常/悬停/按下/选中四态切换与导航栏整体换肤 `IconHelper::setStyle`；
- **QSS 换肤辅助**：`QtHelper::getStyle/setStyle` 读取并应用样式表（配合 `src/core_qss`）；
- **全局配置与初始化**：`AppData`（分辨率自适应参数）、`AppInit`（启动初始化）、`CustomStyle`（QSS 全局字号/滑块样式）；
- **通用工具**：`QtHelper`（屏幕/DPI、居中、字体、编码、OpenGL 后端、消息日志、文件对话框、表格初始化等）、`Base64Helper`（图片/文本 Base64 互转）、`DelegateComboBox`（表格内下拉编辑）、`singleton.h`（双重检查锁单例宏）、`Logger.hpp`（异步彩色日志组件，当前工程内**尚无引用点**，见注意事项）。

## 文件/子目录清单

| 文件/目录 | 说明 |
|-----------|------|
| `appdata.h/.cpp` | 全局界面参数静态类（行高、窗体尺寸），`checkRatio()` 按屏幕宽度≥1440 修正下限 |
| `appinit.h/.cpp` | 单例初始化类，`start()` 向 qApp 安装全局事件过滤器，实现 `canMove` 窗体鼠标拖动 |
| `base64helper.h/.cpp` | QImage/QByteArray 与 Base64 互转（JPEG 编码路线） |
| `customstyle.h/.cpp` | 生成全局 QSS：字号、单选/复选指示器尺寸、QSlider 四态样式（颜色取自系统 QPalette） |
| `delegate.h/.cpp` | `DelegateComboBox`：QTableView 单元格内嵌 QComboBox 编辑器委托 |
| `iconhelper.h/.cpp` | 图标字体核心：懒加载 4 套字体、Unicode 码点自动路由、`setIcon/getPixmap/setStyle` 导航栏换肤 |
| `qthelper.h/.cpp` | 通用静态工具类（屏幕信息/居中/字体/编码/OpenGL/QSS/进程/对话框/校验/格式化等） |
| `singleton.h` | `SINGLETON_DECL/IMPL` 宏：QScopedPointer + QMutex 双重检查锁单例 |
| `Logger.hpp` | 异步日志：调用线程格式化入队，后台线程批量写 stderr；`LOG_DEBUG/INFO/WARN/ERROR/ALARM` 宏 |
| `core_helper.pri` / `core_util.pri` | qmake 工程引用文件（CMake 构建未使用，保留兼容） |
| `qrc/font.qrc` | 注册 `:/font/iconfont.ttf`、`:/font/fontawesome-webfont.ttf`、`:/font/pe-icon-set-weather.ttf` |
| `qrc/image.qrc` | 图片资源（`bg_novideo.png`） |
| `qrc/qm.qrc` | Qt 自带控件中文翻译（qt_zh_CN.qm、widgets.qm） |
| `qrc/wasm.qrc` | WebAssembly 平台资源（本项目未启用） |
| `qrc/font/`、`qrc/image/`、`qrc/qm/` | 上述 qrc 引用的实际文件 |

## 使用方法

本目录通过根 `CMakeLists.txt` 编入主程序：头文件路径加入 `include_directories(${CMAKE_SOURCE_DIR}/src/core_helper)`，qrc 经 `qt5_add_big_resources(CORE_HELPER_SOURCES_QRC ...)` 打包（CMakeLists.txt 第 324~329 行）。

在 `frmmain` 中的真实用法（`src/form/frmmain.cpp`）：

```cpp
// 1. 无边框窗体（同时设置 form/canMove 属性，拖动由 AppInit 全局过滤器实现）
QtHelper::setFramelessForm(this);                    // frmmain.cpp:233

// 2. 图标字体按钮：0xf000+ 为 FontAwesome 码点
IconHelper::setIcon(ui->labIco, 0xf073, 30);         // frmmain.cpp:236（日历）
IconHelper::setIcon(ui->btnMenu_Min, 0xf068);        // 最小化
IconHelper::setIcon(ui->btnMenu_Max, 0xf067);        // 最大化
IconHelper::setIcon(ui->btnMenu_Close, 0xf00d);      // 关闭

// 3. 左侧导航栏整体换肤（StyleColor 配置四态颜色 + 图标码点列表）
IconHelper::StyleColor styleColor;
/* ...设置颜色... */
IconHelper::setStyle(ui->widgetLeftMain, btnsMain, iconsMain, styleColor);  // frmmain.cpp:608

// 4. QSS 换肤：读取资源文件并应用
QString qss = QtHelper::getStyle(":/qss/blacksoft.css");   // frmmain.cpp:316
```

应用级初始化在 `src/main.cpp`：`QtHelper::initMain()` → `QtHelper::initOpenGL(2,...)`（强制 OpenGLES，RK3588 平台）→ `AppInit::Instance()->start()` → `QtHelper::setFont()/setCode()` → `QtHelper::setFormInCenter(&w)`。

## 依赖关系

- 依赖 `src/head.h`（聚合 QtCore/QtGui/QtWidgets）；
- 依赖 Qt5（Core/Gui/Widgets；`setTranslator`/`qm.qrc` 为可选国际化支持）；
- 被 `src/form/frmmain.cpp`、`src/main.cpp` 及多个 UI 窗体引用；`Logger.hpp` 目前未被任何源文件 include（现行日志走 `src/utils/logging.h` 的 NN_LOG_* 宏与 qDebug），保留待接入；
- 与 `src/core_qss` 配套：`QtHelper::getStyle` 读取的 `:/qss/blacksoft.css` 由 core_qss 提供。

## 注意事项

- 组件为移植通用代码，**不要随意修改**，否则影响全部窗体外观；业务改动应放在 `src/form` 层。
- `QtHelper::setStyle(qssFile)` 与 `frmMain::initStyle()` 均依赖硬编码约定：QSS 首行必须为 `QPalette{background:#RRGGBB...` 格式（`qss.mid(20, 7)` 取背景色），改动 blacksoft.css 首行会破坏全局调色板。
- `AppInit::eventFilter` 中的拖动状态是 `static` 全局共享的：同一时刻仅一个窗体可拖动；仅在 GUI 线程回调，无线程竞争，但不可跨线程复用。
- `IconHelper::initFont()` 使用裸 `new` 且无锁保护，只应在主线程首次显示界面前调用（现有调用链满足此约束）。
- `Logger.hpp` 的 `AsyncLogger` 为 Meyers 单例：析构发生在进程退出时，会 join 后台线程并 flush，日志写在 **stderr**（非文件）；重定向日志需 `2>&1`。
- `qthelper.cpp` 中 `checkRun()`（单实例）仅 Windows 实现；`setSystemDateTime`、`runWithSystem` 等函数在本 Linux 目标上仅作保留，不推荐使用。
- `qrc/image.qrc`、`qrc/qm.qrc`、`qrc/wasm.qrc` 是否打包由 `core_helper.pri` 的 `no_qrc_*` 宏控制，但 CMake 构建固定打包了全部 4 个 qrc（见 CMakeLists.txt），以 CMake 行为为准。
