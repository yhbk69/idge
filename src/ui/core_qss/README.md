# src/ui/core_qss

> 所属域：**ui 界面域**（目录重组 S4e 迁入）

## 功能概述

blacksoft 深色（黑灰色）QSS 皮肤包：一个全局样式表 `blacksoft.css` + 一组配套小图标 PNG（复选框、单选框、树分支箭头、日历翻页、下拉箭头等）。整个目录通过 `qss.qrc` 编译进可执行文件，运行时以 `:/qss/...` 资源路径访问，为 IDGE 全部 Qt 控件（按钮、输入框、表格、滚动条、菜单、日期控件等）提供统一的暗色外观。

## 文件/子目录清单

| 文件/目录 | 说明 |
|-----------|------|
| `qss.qrc` | 资源脚本，prefix=`/`，注册 `qss/blacksoft.css` 与 `qss/blacksoft/` 下全部 PNG（24 个） |
| `qss/blacksoft.css` | 主样式表（约 15.7 KB，1 行 1 规则、无 BOM）。首行 `QPalette{background:#444444;}*{outline:0px;color:#DCDCDC;}` 为约定格式，被代码用 `qss.mid(20,7)` 提取背景色 |
| `qss/blacksoft/checkbox_*.png` | 复选框 选中/未选中/半选/禁用 四态图 |
| `qss/blacksoft/radiobutton_*.png` | 单选框 选中/未选中/禁用 图 |
| `qss/blacksoft/add_*.png` | 步进器（QSpinBox 等）上下加减速箭头 |
| `qss/blacksoft/arrow_*.png` | 下拉框/组合框四个方向箭头 |
| `qss/blacksoft/branch_open.png` / `branch_close.png` | 树形控件展开/收起分支符号 |
| `qss/blacksoft/calendar_nextmonth.png` / `calendar_prevmonth.png` | 日期编辑框翻页月份按钮 |
| `qss/blacksoft/menu_checked.png` | 菜单项选中标记 |

## 使用方法

皮肤生效链路（均为已核实的真实代码位置）：

1. **编译打包**：根 `CMakeLists.txt` 第 331~333 行
   `qt5_add_big_resources(CORE_QRC ${CMAKE_SOURCE_DIR}/src/ui/core_qss/qss.qrc)`，随主程序 `idge` 一起链接。
2. **运行时加载**：`frmMain::initStyle()`（`src/ui/form/frmmain.cpp:314`）：

   ```cpp
   QString qss = QtHelper::getStyle(":/qss/blacksoft.css");   // 读取 qrc 资源
   if (!qss.isEmpty()) {
       QString paletteColor = qss.mid(20, 7);                 // 取首行背景色 #444444
       qApp->setPalette(QPalette(QColor(paletteColor)));      // 同步全局调色板
       qApp->setStyleSheet(qss);                              // 应用全局样式
   }
   ```

3. 随后 `getQssColor(qss, ...)` 从样式表中解析文字/面板/边框等主题色，保存到 `frmMain` 成员，供左侧导航栏 `IconHelper::setStyle` 复用，保证控件配色与皮肤一致。

临时预览修改效果：直接编辑 `qss/blacksoft.css` 后重新构建（qrc 内容变化会触发 rcc 重新生成）。

## 依赖关系

- 被 `src/ui/form/frmmain.cpp` 通过资源路径 `:/qss/blacksoft.css` 加载（全工程唯一引用点）；
- `blacksoft.css` 内部用 `url(:/qss/blacksoft/xxx.png)` 引用同目录 PNG 小图；
- 选择器依赖 `src/ui/core_helper` 设置的窗体动态属性：`QtHelper::setFramelessForm` 会设置 `form=true`，`AppInit` 依赖 `canMove` 属性；导航容器需带 `nav="left"/"top"`、`flag` 属性（frmmain 中设置）；
- 与 `CustomStyle::initStyle`（core_helper）互补：后者只做字号/滑块等局部覆盖。

## 注意事项

- **不要改动 `blacksoft.css` 首行格式**：`qss.mid(20, 7)` 依赖前 20 个字符恰为 `QPalette{background:`；破坏该约定会导致全局调色板被设置成错误颜色（frmmain 与 QtHelper::setStyle 两处都有同样的硬编码）。
- css 末尾的 `/*TextColor:#...*/ /*PanelColor:#...*/ /*BorderColor:#...*/ /*NormalColorStart/End:*/ /*DarkColorStart/End:*/ /*HighColor:*/` 标记注释（blacksoft.css 第 687 行起）会被 `frmMain::getQssColor`（按标记字符串 indexOf + mid 取 7 字符）解析，供导航栏配色使用；换主题色时这些标记注释必须保留。
- 全局 `setStyleSheet` 作用于所有窗体，新增页面若使用内联样式需注意优先级（内联 > 全局）。
- `QtHelper::getStyle` 按 `stream >> line` 逐词读取，css 中不要依赖一行的列对齐（读取会压缩空白）。
- PNG 均为小尺寸状态图（14~19px），替换图片时保持尺寸，否则控件指示器会错位。
