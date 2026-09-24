#ifndef THEME_H
#define THEME_H

#include <QString>

/**
 * @file theme.h
 * @brief 全站设计令牌（色板 + 字阶 + 常用样式构造器）—— 单一事实源
 *
 * 约定（2026-09-24 UI 统一批次确立）：
 *   - 新增代码禁止再手写内联色值/字号，一律引用本文件常量或 text() 构造器；
 *   - 存量 ~160 处内联 setStyleSheet 逐批迁移，本批：frmmain / dashboard_widget；
 *   - 令牌与 src/core_qss/qss/blacksoft.css 的注释色板保持一致，改主题两处同步。
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
constexpr int FS_BODY  = 16;  ///< 正文基础
constexpr int FS_CARD  = 18;  ///< 卡片/分组标题
constexpr int FS_PAGE  = 22;  ///< 页面标题
constexpr int FS_KPI   = 30;  ///< KPI 大数字

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
 * @brief 内联小按钮微样式（清空/浏览等）：主文字 + 指定底色
 */
QString miniButton(const char *bg);

}  // namespace theme

#endif  // THEME_H
