// ============================================================================
// theme.cpp - 设计令牌构造器实现（令牌定义见 theme.h）
// ============================================================================

#include "theme.h"
#include <QString>

namespace theme
{

QString text(const char *color, int px, bool bold)
{
    return QString("color:%1;font-size:%2px;%3")
        .arg(color)
        .arg(px)
        .arg(bold ? QStringLiteral("font-weight:bold;") : QString());
}

QString chip(const char *fg, const char *bg, int px, int radius)
{
    // 徽标只把 padding 从 2x8 抬到 3x9，**不设 min-height**：
    // chip 常贴在按钮/卡片内部，撑高会把宿主顶变形。
    return QString("color:%1;background:%2;font-size:%3px;border-radius:%4px;padding:3px 9px;")
        .arg(fg, bg)
        .arg(px)
        .arg(radius);
}

QString field(const char *bg)
{
    // T2：min-height 不含 padding/border → 总高 42 + 2*6 + 2*1 = 56px ≈ 6.3mm。
    // 不写这一句，blacksoft.css 的输入件档位会被每一处内联 field() 打穿。
    return QString("color:%1;background:%2;border:1px solid %3;border-radius:4px;"
                   "min-height:%4px;padding:6px 10px;")
        .arg(TEXT, bg, BORDER)
        .arg(TOUCH_FIELD_MIN_H);
}

QString miniButton(const char *bg)
{
    // T3：30 + 2*8 + 2*1 = 48px ≈ 5.4mm（原来是 3px 上下 padding ≈ 24px，只有 2.7mm）。
    return QString("color:%1;background:%2;border-radius:4px;"
                   "min-height:%3px;padding:8px 14px;")
        .arg(TEXT, bg)
        .arg(TOUCH_ROW_MIN_H);
}

}  // namespace theme
