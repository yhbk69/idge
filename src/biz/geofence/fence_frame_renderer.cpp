// ============================================================================
// fence_frame_renderer.cpp — 围栏版本的合成画面 PNG（§5.2 的 frameUrl 指向它）
// ============================================================================
// 为什么单独一个文件而不是塞进 pad_fence_store.cpp：那一边是 SQL 与状态机，
// 这一边是 QPainter 与 QtGui；混在一起会让"围栏库"莫名其妙依赖起 GUI 栈。
// ============================================================================
#include "fence_frame_renderer.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPen>
#include <QPolygonF>

namespace geofence {
namespace {

/// 两种 ruleType 的画法。颜色只是"一眼分清这条是闯入还是越界"，
/// 规约没规定配色，所以这里用与本机界面同一族的两档（禁区红、越界琥珀）。
QColor ruleColor(const QString &ruleType)
{
    return ruleType == QLatin1String("CROSS") ? QColor(0xFB, 0x8C, 0x00)
                                              : QColor(0xE5, 0x39, 0x35);
}

/// 一个区域的标注点：顶点的算术平均（多边形内部一点，够写序号用）。
/// 不求精确几何重心——图上那个圆点是给人认"这是第几个区域"的，不是量面积用的。
QPointF labelAnchor(const QPolygonF &polygon)
{
    if (polygon.isEmpty())
        return QPointF();
    double x = 0.0;
    double y = 0.0;
    for (const QPointF &point : polygon) {
        x += point.x();
        y += point.y();
    }
    return QPointF(x / polygon.size(), y / polygon.size());
}

}  // namespace

bool renderFenceFrame(const QString &fenceId, int version, const QVector<PadRegion> &regions,
                      const QString &mediaRootDir, const QString &subDir,
                      QString *absPathOut, QString *relPathOut, QString *err)
{
    if (fenceId.isEmpty() || mediaRootDir.isEmpty() || version <= 0) {
        if (err)
            *err = QString::fromUtf8("渲染围栏画面缺参数（fenceId/媒体根/版本号）");
        return false;
    }
    const QString dirName = subDir.isEmpty() ? QStringLiteral("fence-frames") : subDir;
    const QString relativeDir = QStringLiteral("%1/%2").arg(dirName, fenceId);
    const QString relative = QStringLiteral("%1/v%2.png").arg(relativeDir).arg(version);
    const QString absolute = QDir(mediaRootDir).filePath(relative);

    if (!QDir().mkpath(QFileInfo(absolute).absolutePath())) {
        if (err)
            *err = QString::fromUtf8("建不出围栏画面目录：%1").arg(QFileInfo(absolute).absolutePath());
        return false;
    }

    QImage image(kFrameWidth, kFrameHeight, QImage::Format_RGB32);
    image.fill(QColor(16, 20, 28));   // 深底：非主题色板成员，理由同视频预览留边用的纯黑

    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        // 十等分网格：给现场一个"这条围栏大概在画面中间偏右"的比例参照。
        // 没有视频帧可叠的时候（见头文件那段），这是唯一能估位置的东西。
        QPen grid(QColor(255, 255, 255, 26));
        grid.setWidth(1);
        painter.setPen(grid);
        for (int i = 1; i < 10; ++i) {
            const int x = kFrameWidth * i / 10;
            const int y = kFrameHeight * i / 10;
            painter.drawLine(QPoint(x, 0), QPoint(x, kFrameHeight));
            painter.drawLine(QPoint(0, y), QPoint(kFrameWidth, y));
        }

        QFont labelFont = painter.font();
        labelFont.setPixelSize(34);
        labelFont.setBold(true);
        painter.setFont(labelFont);

        // 顺序即界面上的"围栏 1..N"（§5.2 明写数组顺序就是编号），所以序号用数组下标 +1，
        // 而不是从 regionId 里解析什么——regionId 是 Pad 生成的任意串。
        for (int index = 0; index < regions.size(); ++index) {
            const PadRegion &region = regions.at(index);
            QPolygonF polygon;
            polygon.reserve(region.points.size());
            for (const QPointF &point : region.points) {
                polygon << QPointF(point.x() * kFrameWidth, point.y() * kFrameHeight);
            }
            if (polygon.size() < 2)
                continue;   // validateRegions 已经把点数判过，这里是防御性跳过而不是新规则
            const QColor color = ruleColor(region.ruleType);
            QColor fill = color;
            fill.setAlpha(72);              // 半透明：多个区域叠在一起时能看见彼此的边
            painter.setBrush(fill);
            QPen outline(color);
            outline.setWidth(3);
            painter.setPen(outline);
            painter.drawPolygon(polygon);

            const QPointF anchor = labelAnchor(polygon);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 190));
            painter.drawEllipse(anchor, 24, 24);
            painter.setPen(Qt::white);
            painter.drawText(QRectF(anchor.x() - 24, anchor.y() - 24, 48, 48),
                             Qt::AlignCenter, QString::number(index + 1));
        }
        painter.end();
    }

    if (!image.save(absolute, "PNG")) {
        if (err)
            *err = QString::fromUtf8("围栏画面写盘失败：%1").arg(absolute);
        return false;
    }
    // save() 返回 true 之后再确认一次文件真在：媒体根在只读挂载/满盘上
    // 会出现"报了成功但没落下去"的情况，而 apply 的响应里已经带上这个地址了。
    if (!QFileInfo::exists(absolute)) {
        if (err)
            *err = QString::fromUtf8("围栏画面写完但文件不在：%1").arg(absolute);
        return false;
    }
    if (absPathOut)
        *absPathOut = absolute;
    if (relPathOut)
        *relPathOut = relative;
    return true;
}

}  // namespace geofence
