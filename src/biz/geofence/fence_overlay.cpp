// ============================================================================
// fence_overlay.cpp - 电子围栏显示覆盖层实现（只读）
// ============================================================================

#include "fence_overlay.h"
#include "fence_manager.h"
#include "fence_palette.h"

#include <QPainter>
#include <QPainterPath>
#include <QPen>

namespace geofence {

FenceOverlay::FenceOverlay(QWidget *parent)
    : QWidget(parent)
{
    // 视频页只显示：鼠标事件穿透到下面的视频控件
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);

    // 围栏库刷新（管理页应用/停用、Pad HTTP 下发、登录切换）后自动重读快照。
    // 必须 QueuedConnection：FenceManager 持锁 emit，直连会在 loadFences →
    // channelFence 里再抢同一把不可重入锁，当场死锁。
    connect(&FenceManager::instance(), &FenceManager::fenceChanged,
            this, [this](int channel) {
                if (channel == channel_)
                    loadFences();
            }, Qt::QueuedConnection);
}

void FenceOverlay::setChannel(int channel)
{
    channel_ = channel;
    loadFences();
}

void FenceOverlay::setContentRect(const QRect &contentRect)
{
    if (contentRect_ == contentRect)
        return;
    contentRect_ = contentRect;
    update();
}

void FenceOverlay::loadFences()
{
    channelFence_ = FenceManager::instance().channelFence(channel_);
    update();
}

QPoint FenceOverlay::toWidget(const Point &p) const
{
    const QRect canvas = contentRect_.isValid() ? contentRect_ : rect();
    return QPoint(pixelFromNorm(p.x, canvas.left(), canvas.width()),
                  pixelFromNorm(p.y, canvas.top(), canvas.height()));
}

void FenceOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    for (const FenceShape &shape : channelFence_.shapes)
        drawFenceShape(painter, shape);
}

void FenceOverlay::drawFenceShape(QPainter &painter, const FenceShape &shape)
{
    if (shape.points.empty())
        return;

    const QColor penColor = fenceLineColor(shape.role);
    const QColor fillColor = fenceFillColor(shape.role);

    if (shape.type == ShapeType::Rectangle && shape.points.size() >= 2) {
        QRect rect(toWidget(shape.points[0]), toWidget(shape.points[1]));
        rect = rect.normalized();
        painter.fillRect(rect, fillColor);
        painter.setPen(QPen(penColor, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect);
    } else {
        QPainterPath path;
        const QPoint first = toWidget(shape.points[0]);
        path.moveTo(first);
        for (int i = 1; i < shape.points.size(); ++i)
            path.lineTo(toWidget(shape.points[i]));
        path.closeSubpath();

        painter.fillPath(path, fillColor);
        painter.setPen(QPen(penColor, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }
}

}  // namespace geofence
