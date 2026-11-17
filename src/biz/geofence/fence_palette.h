#pragma once

// ============================================================================
// fence_palette.h - 电子围栏配色的唯一出处
// ============================================================================
// 「电子围栏」管理页按 Pad 的 ruleType 上色，视频页/报警截图按本机 FenceRole
// 上色；两边曾经各写一套常量，导致同一条围栏在管理页是琥珀、在视频页是绿色。
// 现在统一到这里：
//   INTRUSION（禁区）   → #E53935
//   CROSS    （岗区域） → #FB8C00
// 边框 RGB 与填充透明度都只在这里定义，新增绘制路径不要再直接写颜色值。
// ============================================================================

#include <QColor>
#include <QString>

#include "fence_shape.h"

namespace geofence {

/// 填充透明度：屏幕预览/直播/报警截图统一到此值。
inline int fenceFillAlpha()
{
    return 60;
}

/// 边框/填充基色（与管理页「闯入红 / 越界琥珀」一致）。
inline QColor fenceLineColor(FenceRole role)
{
    return role == FenceRole::Workzone ? QColor(0xFB, 0x8C, 0x00)
                                       : QColor(0xE5, 0x39, 0x35);
}

/// 半透明填充色；alpha 默认取统一值，导出图等场景可显式覆盖。
inline QColor fenceFillColor(FenceRole role, int alpha = fenceFillAlpha())
{
    QColor color = fenceLineColor(role);
    color.setAlpha(alpha);
    return color;
}

/// image_drawing 的 ARGB8888 口径（0xAARRGGBB）。
inline unsigned int fenceArgb(FenceRole role)
{
    const QColor color = fenceLineColor(role);
    return 0xFF000000u | ((unsigned int)color.red() << 16)
           | ((unsigned int)color.green() << 8) | (unsigned int)color.blue();
}

/// Pad ruleType → 本机角色。与 FenceManager::regionsToShapes 的换算保持同一口径。
inline FenceRole roleFromRuleType(const QString &ruleType)
{
    return ruleType.compare(QLatin1String("CROSS"), Qt::CaseInsensitive) == 0
               ? FenceRole::Workzone
               : FenceRole::Forbidden;
}

}  // namespace geofence
