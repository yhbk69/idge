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
| `qss/blacksoft.css` | 主样式表（约 21.6 KB）。首行 `QPalette{background:#RRGGBB;}` 为约定格式，代码用 `qss.mid(20,7)` 提取背景色；**尺寸声明的写法受"注意事项"里 `getStyle` 拆行一条约束** |
| `qss/blacksoft/*.png` | 复选框/单选框/步进器箭头/下拉箭头/树分支/日历翻页/菜单选中标记等状态图（14~19px） |
| `qrc/font.qrc` + `qrc/font/` | 注册 `:/font/iconfont.ttf`、`:/font/fontawesome-webfont.ttf`、`:/font/pe-icon-set-weather.ttf`、`:/font/DroidSansFallback.ttf` |
| `qrc/image.qrc` + `qrc/image/` | `:/image/bg_novideo.png`（**当前无代码引用**，素材保留） |
| `qrc/qm.qrc` + `qrc/qm/` | Qt 自带控件中文翻译（`:/qm/qt_zh_CN.qm` 由 `main.cpp` 加载；`widgets.qm` 只被零调用的 `QtHelper::initAll` 引用） |
| `qrc/wasm.qrc` | 与 `font.qrc` 同一份 `DroidSansFallback.ttf` 的第二个注册入口（`qthelper.cpp` 按 `:/font/DroidSansFallback.ttf` 取用） |

已随死代码清理删除（零 include、零调用）：`appdata.*`、`customstyle.*`、`delegate.*`（`DelegateComboBox`）、
`base64helper.*`。注意旧 README 把 `frmMain::initStyle()` 当成 `CustomStyle` 的使用者，那是错的——它是 frmMain 自己的私有函数。

## 使用方法

本目录通过根 `CMakeLists.txt` 编入主程序：头文件路径在 `include_directories(${CMAKE_SOURCE_DIR}/src/ui/core)`，
资源经两个块打包——`qt5_add_big_resources` 的 `CORE_HELPER_SOURCES_QRC`（本目录 `qrc/` 四个 qrc，字体/图片体积大）
与 `qt5_add_resources` 的 `CORE_QRC`（本目录 `qss.qrc`），随 `idge` 一起链接。

**`qss.qrc` 刻意不走 big_resources**：big_resources 生成的是 `rcc --pass 1 / --pass 2` 两段式规则，
Makefile 里那条 `qrc_qss.o:` **前置依赖为空**，改 `blacksoft.css` 后 rcc 根本不会重跑，
皮肤改动静默不进二进制（2026-10-29 触屏批次实测白折腾一轮）。普通 `qt5_add_resources` 会把
`.qrc` 里列出的每个文件都挂成前置依赖（对照 `qrc_resources.cpp` 规则：`resources.qrc` + `shutter.wav` + `.depends`）。
皮肤只有 1 个 21KB css + 23 张小图标，本来也不需要 big 模式。

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

## 触屏命中区设计令牌（2026-10-29 批次）

目标硬件是**板载 10 寸电容触摸屏**，分辨率实测而非推测：`xrandr` 报 `DSI-1 connected 1920x1200+0+0`
（HDMI-1 上那台 1920x1080 是外接调试屏，不是目标），`/sys/class/input/event3` 名为 `goodix-ts`
确认电容触摸控制器。面板按 10.1" 16:10 推算 ≈217x136mm → **≈226 PPI，1mm ≈ 8.9 逻辑 px**；
工程里 `AA_EnableHighDpiScaling`/`QT_SCALE_FACTOR` 零命中，所以**逻辑 px == 物理 px**，
样式里的数字就是毫米换算的直接输入。触摸目标换算：6mm ≈ 53px（下限）、8mm ≈ 71px（舒适）。

令牌定义在 `theme.h`，css 里的数字与之一一对应（Qt QSS 的 `min-height` **不含** padding 与 border，
故 总高 = min-height + 2×padding_v + 2×border）：

| 档 | 总高 | mm | 常量 | 用在 |
| --- | --- | --- | --- | --- |
| T1 主操作 | 72 | 8.1 | `TOUCH_MAIN_MIN_H=54`（+2×8 padding +2×1 border） | 全局 `.QPushButton/.QToolButton`、顶部导航钮、围栏工具栏钮 |
| T2 输入件 | 56 | 6.3 | `TOUCH_FIELD_MIN_H=42` | `QLineEdit/QComboBox/QSpinBox/QDoubleSpinBox` 及 `theme::field()` |
| T3 表格内 | 48 | 5.4 | `TOUCH_ROW_MIN_H=30` | `详情`/`查看`/`注销`/`移除` 等行内次级钮、`theme::miniButton()` |
| T4 图标钮 | 48x48 | 5.4 | `TOUCH_ICON_BTN` | 画面放大钮、标题栏 min/max/close、照片删除 × |
| 指示器 | 40x40 | 4.5 | — | `QCheckBox/QRadioButton::indicator`（Qt 里 indicator 尺寸**就是**命中区，只能真放大） |
| 步进/下拉 | 宽 44 | — | — | `::up-button/::down-button`、`::drop-down`（原 10px/15px 指肚按不到） |
| 滚动条 | 厚 26 | 2.9 | — | `QScrollBar`（原 12px 且无翻页钮） |

三条容易踩的口径，都是这一轮量出来的：

1. **字号基本不动**。全局正文仍是 `font-size:16px`（css 尾部），唯一上调的一档是导航 `FS_NAV_TOUCH=18`——
   因为导航钮去掉了 icon，横向纵向都腾出了空间。"按钮小"的解法是长命中区，不是长字号；
   动字号必然截字，本轮用探针验过 5 个页面 626 个可见控件 0 截字才收工。
2. **表格行高 CSS 管不到**。`QTableView::item{padding}` 不改行高，行高由 `verticalHeader()` 的
   default section size 决定，只能在代码里 `setDefaultSectionSize(theme::TOUCH_TABLE_ROW)`（56px）。
3. **皮肤层会被内联样式打穿**。`theme::field()/miniButton()/chip()` 是全仓 300 余处内联 QSS 的集中生成器，
   只改 css 不改它们等于没改；反过来，凡是固定宽 `setFixedSize` 的按钮在新 padding 下会截字，
   该轮把角色钮等处的固定宽换成了 `setMinimumHeight` + `setMinimumSize`。

顶部导航已从"图标+文字（`ToolButtonTextUnderIcon`）"改为**纯文字**，`IconHelper` 只留给标题栏三钮的 glyph；
`labIco`（日历 glyph）隐藏。顺带删掉了"双击标题栏切最大化"——触屏上是误触源，且窗口尺寸一变，
围栏 overlay 的像素坐标就跟着漂，直接变成漏报/误报。

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
- **`getStyle` 会把带空格的值拆行，进而吃掉整条规则的后半段**（本目录最坑的一条，2026-10-29 才定位）：
  `qthelper.cpp` 的 `getStyle()` 用 `stream >> line` 逐 **token** 读取再 `join("\n")`，
  于是 `border:1px solid #3d3d4d;` 在交给 Qt 前就被拆成 `border:1px` / `solid` / `#3d3d4d;` 三行。
  `QCssParser::parseDeclaration` 碰到 `solid` 这种"缺冒号的属性名"报错，`parseStyleRule` 随即
  `skipTo(RightBrace)` —— **该条规则里剩下的声明全部作废**。所以 css 现状是"半死"的，
  肉眼看着写了却不起作用的属性多半是这个原因。
  **写法规约：尺寸类声明一律单 token（`padding-top/left/bottom/right` 分开写）并排在规则最前面**，
  多 token 行（`border:`、`background:qlineargradient(...)`）留在后面别动。
  不去改 `getStyle()` 是有意取舍：把 reader 修好会一次性点亮全仓几百条已死的声明，
  等于给 74 个对话框和每个页面同时换肤，不属于任何一轮改动的射程。
  代码里 `setStyleSheet()` 的内联 QSS 不经 `getStyle()`，多 token 值正常，不受本条约束。
- PNG 均为 14~19px 状态图，替换时保持尺寸，否则控件指示器错位；改 css 后重新构建即可
  （`qss.qrc` 已改走 `qt5_add_resources`，`blacksoft.css` 与 23 张 PNG 都是真实前置依赖）。
- **皮肤 PNG 必须入库**：根 `.gitignore` 全局忽略 `*.png`，本目录靠三条显式豁免放行
  （`!src/ui/core/qss/*.png`、`!src/ui/core/qss/*/*.png`、`!src/ui/core/qrc/image/*.png`，共 24 个、约 31KB）。
  漏掉任何一条，新克隆会在 autorcc 阶段以
  `RCC: Error in 'qss.qrc': Cannot find file 'qss/blacksoft/xxx.png'` **直接构建失败**（不是"皮肤显示不出来"的软失败）；
  新增皮肤图或换 qss 包时，先 `git status --porcelain` 确认文件真的出现在未跟踪列表里而不是被忽略。
