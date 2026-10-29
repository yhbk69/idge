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
    return QString("color:%1;background:%2;font-size:%3px;border-radius:%4px;padding:2px 8px;")
        .arg(fg, bg)
        .arg(px)
        .arg(radius);
}

QString field(const char *bg)
{
    return QString("color:%1;background:%2;border:1px solid %3;border-radius:4px;padding:2px 6px;")
        .arg(TEXT, bg, BORDER);
}

QString miniButton(const char *bg)
{
    return QString("color:%1;background:%2;border-radius:4px;padding:3px 8px;")
        .arg(TEXT, bg);
}

}  // namespace theme
