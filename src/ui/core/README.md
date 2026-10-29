# src/ui/core

> 所属域：**ui 界面域**。原 `src/ui/core_helper`（移植的 Qt 基础组件）与 `src/ui/core_qss`（blacksoft 皮肤包）
> 是一个东西的两半——代码负责"读哪份样式、怎么换肤"，资源负责"样式本身"，拆成两个目录只会让两边 README
> 互相指。2026-10-29 合并为本目录：`.cpp` 位于 `src/*/*/*.cpp`，仍在根 CMake GLOB 的覆盖深度内。

## 功能概述

从 blacksoft（feiyangqingyun）框架移植的 Qt 基础组件 + 全局深色皮肤，为主界面提供：

- **无边框窗体与全局拖动**：`QtHelper::setFramelessForm` + `AppInit` 全局事件过滤器（依赖窗体属性 `canMove`）；
- **图标字体**：`IconHelper` 统一管理 iconfont / FontAwesome 等字体图标，四态切换 + 导航栏整体换肤 `IconHelper::setStyle`；
- **QSS 换肤**：`QtHelper::getStyle/setStyle` 读取 `:/qss/blacksoft.css` 并应用（由本目录 `qss/` 提供）；
- **通用工具**：`QtHelper`（屏幕/DPI、居中、字体、编码、OpenGL 后端、消息框按钮本地化等）；`singleton.h`（双重检查锁单例宏，仅 `appinit` 用）。

## 文件/子目录清单

| 文件/目录 | 说明 |
|-----------|------|
| `appinit.h/.cpp` | 单例初始化类，`start()` 向 qApp 安装全局事件过滤器，实现 `canMove` 窗体鼠标拖动（仅 `main.cpp` 用） |
| `iconhelper.h/.cpp` | 图标字体核心：懒加载 4 套字体、Unicode 码点自动路由、`setIcon/getPixmap/setStyle` 导航栏换肤（仅 `frmmain` 用） |
| `qthelper.h/.cpp` | 通用静态工具类（屏幕信息/居中/字体/编码/OpenGL/QSS/进程/对话框/校验/格式化等） |
| `theme.h/.cpp` | `theme::` 命名空间的配色/尺寸取值，form×10 + widgets×4 共 300 余处调用——本目录被依赖最广的一件 |
| `singleton.h` | `SINGLETON_DECL/IMPL` 宏：QScopedPointer + QMutex 双重检查锁单例 |
| `qss.qrc` | 皮肤资源脚本，prefix=`/`，注册 `qss/blacksoft.css` 与 `qss/blacksoft/` 下 24 个 PNG |
| `qss/blacksoft.css` | 主样式表（约 15.7 KB）。首行 `QPalette{background:#444444;}…` 为约定格式，代码用 `qss.mid(20,7)` 提取背景色 |
| `qss/blacksoft/*.png` | 复选框/单选框/步进器箭头/下拉箭头/树分支/日历翻页/菜单选中标记等状态图（14~19px） |
| `qrc/font.qrc` + `qrc/font/` | 注册 `:/font/iconfont.ttf`、`:/font/fontawesome-webfont.ttf`、`:/font/pe-icon-set-weather.ttf`、`:/font/DroidSansFallback.ttf` |
| `qrc/image.qrc` + `qrc/image/` | `:/image/bg_novideo.png`（**当前无代码引用**，素材保留） |
| `qrc/qm.qrc` + `qrc/qm/` | Qt 自带控件中文翻译（`:/qm/qt_zh_CN.qm` 由 `main.cpp` 加载；`widgets.qm` 只被零调用的 `QtHelper::initAll` 引用） |
| `qrc/wasm.qrc` | 与 `font.qrc` 同一份 `DroidSansFallback.ttf` 的第二个注册入口（`qthelper.cpp` 按 `:/font/DroidSansFallback.ttf` 取用） |

已随死代码清理删除（零 include、零调用）：`appdata.*`、`customstyle.*`、`delegate.*`（`DelegateComboBox`）、
`base64helper.*`。注意旧 README 把 `frmMain::initStyle()` 当成 `CustomStyle` 的使用者，那是错的——它是 frmMain 自己的私有函数。

## 使用方法

本目录通过根 `CMakeLists.txt` 编入主程序：头文件路径在 `include_directories(${CMAKE_SOURCE_DIR}/src/ui/core)`，
资源经两个 `qt5_add_big_resources` 块打包——`CORE_HELPER_SOURCES_QRC`（本目录 `qrc/` 四个 qrc）
与 `CORE_QRC`（本目录 `qss.qrc`），随 `idge` 一起链接。

**资源别名由「qrc 文件所在目录 + qrc 内相对路径 + prefix」决定，改目录名/挪 qss 位置会直接破坏 `:/qss/…` 字符串**：
`qss.qrc` 必须与 `qss/` 同级，`qrc/*.qrc` 必须与 `qrc/<子目录>` 同级。

皮肤生效链路（`src/ui/form/frmmain.cpp` 的 `initStyle()`）：

```cpp
QString qss = QtHelper::getStyle(":/qss/blacksoft.css");   // 全工程唯一加载点
if (!qss.isEmpty()) {
    QString paletteColor = qss.mid(20, 7);                 // 取首行背景色 #444444
    qApp->setPalette(QPalette(QColor(paletteColor)));      // 同步全局调色板
    qApp->setStyleSheet(qss);                              // 应用全局样式
}
```

随后 `getQssColor(qss, ...)` 从样式表尾部的 `/*TextColor:#…*/` 等标记注释解析主题色，供导航栏 `IconHelper::setStyle` 复用。

窗体侧真实用法（`frmmain.cpp`）：`QtHelper::setFramelessForm(this)` →
`IconHelper::setIcon(ui->btnMenu_Min, 0xf068)` → `IconHelper::setStyle(widgetLeftMain, btns, icons, styleColor)`。
应用级初始化在 `src/main.cpp`：`QtHelper::initMain()` → `QtHelper::initOpenGL(2,…)`（强制 OpenGLES，RK3588）
→ `AppInit::Instance()->start()` → `QtHelper::setFont()/setCode()/setTranslator()` → `QtHelper::setFormInCenter(&w)`。

## 依赖关系

- 依赖 `src/head.h`（聚合 QtCore/QtGui/QtWidgets）与 Qt5（Core/Gui/Widgets）；图标全部走 qrc 路径，不读文件系统；
- 被 `src/main.cpp`、`src/ui/form/*`（theme 10 处、qthelper 6 处、iconhelper 1 处）、`src/ui/widgets/*`（theme 4 处、qthelper 2 处）引用；
  **本目录不反向依赖 form/widgets**；
- 与仓库根 `res/main.qrc` 并列的三个资源入口：`res/*.qrc` 由 GLOB 收入，注册 `:/image/main_*.png`、`:/gl/*.fsh|.vsh`
  以及**同名**的 `:/font/iconfont.ttf`、`:/font/fontawesome-webfont.ttf`（与 `qrc/font.qrc` 重复注册，rcc 出警告、先到者生效；
  两份文件内容一致，故暂无副作用——要清掉就先确认 rcc 顺序，属另一件事）。

## 注意事项

- 组件为移植通用代码，**不要随意修改**，否则影响全部窗体外观；业务改动应放在 `src/ui/form` 层。
- `QtHelper::setStyle(qssFile)` 与 `frmMain::initStyle()` 都硬编码依赖 QSS 首行 `QPalette{background:#RRGGBB…`
  （`qss.mid(20, 7)`）；css 尾部 `/*TextColor:*/ … /*HighColor:*/` 标记注释同样被 `getQssColor` 解析，换色时必须保留。
- `AppInit::eventFilter` 的拖动状态是 `static` 全局共享：同一时刻只能拖一个窗体，且只在 GUI 线程回调。
- `IconHelper::initFont()` 用裸 `new` 且无锁，只应在主线程首次显示界面前调用（现有调用链满足）。
- `qthelper.cpp` 里有一批**零调用的移植遗留**（`initAll`、`setSystemDateTime`、`checkRun`（仅 Windows）、`runWithSystem` 等），
  保留是为了将来接平台配置时对照，不等于可用；`checkRun` 在本 Linux 目标上直接返回 true，别拿它当单实例保护。
- `getStyle` 按 `stream >> line` 逐词读取，css 里不要依赖一行的列对齐（读取会压缩空白）。
- PNG 均为 14~19px 状态图，替换时保持尺寸，否则控件指示器错位；改 css 后重新构建即可（qrc 内容变化会触发 rcc 重新生成）。
