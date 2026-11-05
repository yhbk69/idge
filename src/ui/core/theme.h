#ifndef THEME_H
#define THEME_H

#include <QString>

class QComboBox;

/**
 * @file theme.h
 * @brief 全站设计令牌（色板 + 字阶 + 常用样式构造器）—— 单一事实源
 *
 * 约定（2026-09-24 UI 统一批次确立）：
 *   - 新增代码禁止再手写内联色值/字号，一律引用本文件常量或 text() 构造器；
 *   - 存量 ~160 处内联 setStyleSheet 逐批迁移，本批：frmmain / dashboard_widget；
 *   - 令牌与 src/ui/core/qss/blacksoft.css 的注释色板保持一致，改主题两处同步。
 *
 * 用法：
 *   label->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_HINT));
 *   panel->setStyleSheet(QString("background:%1;border:1px solid %2;border-radius:8px;")
 *                            .arg(theme::PANEL, theme::BORDER));
 */

namespace theme
{

// ========== 色板（与 blacksoft.css 主题注释一致） ==========
constexpr const char *BG            = "#1e1e2e";  ///< 页面背景
constexpr const char *PANEL         = "#2d2d3d";  ///< 面板/卡片底
constexpr const char *FIELD         = "#262636";  ///< 输入控件底
constexpr const char *BORDER        = "#3d3d4d";  ///< 边框/悬停底
constexpr const char *ALT           = "#343444";  ///< 交替行底
constexpr const char *HOVER         = "#45455c";  ///< 深悬停底
constexpr const char *ACCENT        = "#4fc3f7";  ///< 强调色（选中/高亮/进度）
constexpr const char *TEXT          = "#E5E7EB";  ///< 主文字
constexpr const char *TEXT_MUTED    = "#9CA3AF";  ///< 次要文字/提示
constexpr const char *DANGER        = "#f44336";  ///< 危险/报警
constexpr const char *SUCCESS       = "#4caf50";  ///< 成功/正常
constexpr const char *WARNING       = "#ff9800";  ///< 警告
constexpr const char *INFO          = "#2196f3";  ///< 已办结（报警「已处置」）：区别于 SUCCESS 的"已确认＝看过"
constexpr const char *BTN_SECONDARY = "#4a6fa5";  ///< 次级按钮底

// ========== 交互态（hover/pressed，来自点名/盘点批次并全站收敛） ==========
constexpr const char *BTN_SECONDARY_HOVER = "#5a8fc5";  ///< 次级按钮悬停
constexpr const char *SKY                 = "#0EA5E9";  ///< 亮蓝按钮悬停
constexpr const char *SKY_PRESSED         = "#0284C7";  ///< 亮蓝按钮按下
constexpr const char *DANGER_PRESSED      = "#B91C1C";  ///< 危险按钮按下
constexpr const char *SUCCESS_HOVER       = "#34D399";  ///< 成功按钮悬停（亮）

// ========== 字阶（px，ARM 上 QtHelper 不再强制放大） ==========
constexpr int FS_BADGE = 12;  ///< 角标
constexpr int FS_HINT  = 13;  ///< 提示文字
constexpr int FS_NAV   = 14;  ///< 顶部/左侧导航按钮标题
constexpr int FS_BODY  = 16;  ///< 正文基础
constexpr int FS_CARD  = 18;  ///< 卡片/分组标题
constexpr int FS_PAGE  = 22;  ///< 页面标题
constexpr int FS_KPI   = 30;  ///< KPI 大数字

// ========== 触屏命中区档（2026-10-29 触屏批次） ==========
// 目标硬件实测：DSI-1 1920x1200、goodix-ts 十寸面板、≈226PPI（1mm ≈ 8.9 逻辑 px），
// 且工程未启用 HiDPI 缩放 → 这里的数字就是毫米换算的直接输入。
// Qt QSS 的 min-height 不含 padding 与 border：总高 = min-height + 2*padding_v + 2*border。
// 本轮原则：字号一律不动（锚在 blacksoft.css 末尾的全局 16px），只长命中区 —— 这是不截字的前提。
constexpr int TOUCH_MAIN_MIN_H  = 54;  ///< T1 主操作/导航：54 + 2*8 + 2*1 = 72px ≈ 8.1mm
constexpr int TOUCH_FIELD_MIN_H = 42;  ///< T2 输入件：42 + 2*6 + 2*1 = 56px ≈ 6.3mm
constexpr int TOUCH_ROW_MIN_H   = 30;  ///< T3 表格内次级钮：30 + 2*8 + 2*1 = 48px ≈ 5.4mm
constexpr int TOUCH_ICON_BTN    = 48;  ///< T4 纯图标钮边长（代码 setFixedSize 直接用）
constexpr int TOUCH_TABLE_ROW   = 56;  ///< 表格行高：CSS 的 ::item padding 管不到，只能 setDefaultSectionSize
// TOUCH_FIELD_H 是 T2 的**总高**，给代码里 setMinimumHeight 用（皮肤对 QCheckBox 没有高度档，
// indicator 就是全部命中区，只能自己抬）。与 TOUCH_TABLE_ROW 数值相同但语义不同：
// 一个是"输入件要多高"，一个是"表格一行要多高"，改任意一个都不该连带另一个。
constexpr int TOUCH_FIELD_H     = 56;  ///< T2 总高（setMinimumHeight 直接可用）
constexpr int TOUCH_MAIN_H      = 72;  ///< T1 总高（弹窗关闭钮这类代码里定尺寸的场合）

// ========== 顶部导航字号（去 icon 后唯一被调大的一档） ==========
// 导航钮从"图上字下"变"只有字"，横向不再被 22px 图标占位，才敢从 FS_NAV 抬到 18。
// 只给导航行用，别处不要引用 —— 其余字阶本轮刻意不动。
constexpr int FS_NAV_TOUCH = 18;

// ========== 常用样式构造器 ==========

/**
 * @brief 文字样式：颜色 + 字号（可选加粗）
 * 形如 "color:#9CA3AF;font-size:13px;font-weight:bold;"
 */
QString text(const char *color, int px, bool bold = false);

/**
 * @brief 文字 + 背景 + 圆角胶囊（徽标/小按钮常用）
 */
QString chip(const char *fg, const char *bg, int px, int radius = 4);

/**
 * @brief 输入框微样式：主文字 + 指定底色 + 标准边框圆角（默认 FIELD 底）
 */
QString field(const char *bg = FIELD);

/**
 * @brief 分组框容器的底：与 field() 同配色，但**只描自己、不带 min-height/padding**。
 *
 * ⚠ 容器一律用它，不要用 field()：field() 是裸声明（无选择器），Qt 会把它顺着父子链
 *   漏给组里的每一个子件。而 QSS 的 min-height 是**赋值**不是"与现有下限取大"
 *   （QStyleSheetStyle::setGeometry 只在规则声明了 min-height 时才写 minimumSize），
 *   于是漏到 QTableWidget 上就把代码里的 setMinimumHeight(328) 改成 56，整组被压到
 *   65px、两行表单叠成一行。2026-11-02 板端实测：设置页三张带 field() 的分组框全部
 *   塌成 65px，同一页没带 field() 的 WebSocket 分组框高度正常（193px=自身 sizeHint）。
 */
QString panel(const char *bg = PANEL);

/**
 * @brief 内联小按钮微样式（清空/浏览等）：主文字 + 指定底色
 */
QString miniButton(const char *bg);

/**
 * @brief 页级主操作钮（T1）：与 miniButton 同配色，只是命中区从 48 抬到 72。
 *
 * 判据只有一条：**这个按钮不在表格行里**。表格行高 56，行内钮必须走 T3 才不被行边界裁掉；
 * 而页面上独立的一级动作（保存/连接/导入/删除选中）用 T3 实测只有 5.4mm，低于 6mm 下限。
 * 2026-11-01 板端实测把这两档混用的代价照了出来：设置页 10 处页级钮全是 88x48。
 */
QString mainButton(const char *bg);

/**
 * @brief 把 QComboBox **弹出列表**的行高抬到触屏档（默认 T5 表格行 56px）。
 *
 * ⚠ 为什么不是 QSS：`QComboBox QAbstractItemView::item { min-height }` 写在 combo 自己的
 *   样式表里、还是写在 `combo->view()` 的样式表里，**都不生效**（2026-10-31 用独立探针程序
 *   实测：两种写法弹层行高都停在字体算出的 40px，只有换 item delegate 才变成 56px）。
 *   弹层行高由 QComboBoxListView::sizeHintForRow 决定，它只认 delegate 给的尺寸。
 *   后果是真实的：20 项 × 32px 的下拉在十寸屏上既按不准、首项还会被顶到屏幕外。
 *
 * 顺带把**滚轮**吃掉：下拉在滚动区里被滚一下就会改当前项并触发 activated，
 * 而 activated 的槽是"写配置 + 落盘"，等于一次无意的手势改了配置。
 */
void makeComboTouch(QComboBox *combo, int rowH = TOUCH_TABLE_ROW);

}  // namespace theme

#endif  // THEME_H
