// ============================================================================
// fence_overlay.cpp - 电子围栏绘制覆盖层实现
// ============================================================================

#include "fence_overlay.h"
#include "fence_manager.h"
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QPen>
#include <QBrush>
#include <QColor>

namespace geofence {

// ============================================================================
// 构造函数
// ============================================================================
// 设置透明背景和鼠标跟踪
// ============================================================================
FenceOverlay::FenceOverlay(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
}

// ============================================================================
// setChannel - 设置当前操作的通道
// ============================================================================
void FenceOverlay::setChannel(int channel)
{
    channel_ = channel;
    loadFences();
}

// ============================================================================
// setDrawMode - 设置绘制模式
// ============================================================================
// 切换模式时清除临时绘制状态
// ============================================================================
void FenceOverlay::setDrawMode(DrawMode mode)
{
    drawMode_ = mode;
    polyPoints_.clear();
    rectDragging_ = false;
    setCursor((mode == DrawMode::NoMode) ? Qt::ArrowCursor : Qt::CrossCursor);
    update();
}

// ============================================================================
// loadFences - 从 FenceManager 加载围栏数据
// ============================================================================
void FenceOverlay::loadFences()
{
    channelFence_ = FenceManager::instance().channelFence(channel_);
    update();
}

// ============================================================================
// saveFences - 保存围栏数据到 FenceManager
// ============================================================================
// 先记录当前 widget 尺寸再落数据：围栏顶点是 widget 像素坐标，检测端要把帧坐标
// 反算回同一 widget 空间，必须知道这组坐标"生成时"的画布大小，二者配对才有意义。
// 若在窗口缩放后未重新保存，围栏坐标与 overlay_w/h 会失配，导致命中判断整体偏移。
// 顺序：setOverlaySize → setChannelFence（触发 fenceChanged 刷新）→ saveToConfig 落盘。
// ============================================================================
void FenceOverlay::saveFences()
{
    FenceManager::instance().setOverlaySize(channel_, width(), height());
    FenceManager::instance().setChannelFence(channel_, channelFence_);
    FenceManager::instance().saveToConfig();
    emit fenceUpdated();
}

// ============================================================================
// paintEvent - 绘制所有围栏和临时图形
// ============================================================================
void FenceOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // 绘制已保存的围栏形状
    for (const auto &shape : channelFence_.shapes) {
        drawFenceShape(painter, shape);
    }

    // 绘制正在绘制的临时图形
    if (drawMode_ == DrawMode::RectDraw && rectDragging_) {
        drawTempRect(painter);
    } else if (drawMode_ == DrawMode::PolyDraw && !polyPoints_.isEmpty()) {
        drawTempPolygon(painter);
    }
}

// ============================================================================
// drawFenceShape - 绘制一个已保存的围栏形状
// ============================================================================
// 矩形：用 QRect + drawRect 绘制四条边
// 多边形：用 QPainterPath 依次连接各顶点
// 填充：红色半透明 (255,0,0,50)
// 边框：红色实线，宽度 2
// ============================================================================
void FenceOverlay::drawFenceShape(QPainter &painter, const FenceShape &shape)
{
    if (shape.points.empty()) return;

    if (shape.type == ShapeType::Rectangle && shape.points.size() >= 2) {
        // 矩形：用两个对角点构造 QRect
        int x1 = shape.points[0].x, y1 = shape.points[0].y;
        int x2 = shape.points[1].x, y2 = shape.points[1].y;
        QRect rect(QPoint(x1, y1), QPoint(x2, y2));
        painter.fillRect(rect, QColor(255, 0, 0, 50));
        painter.setPen(QPen(QColor(255, 0, 0), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect);
    } else {
        // 多边形：依次连接各顶点，首尾闭合
        QPainterPath path;
        path.moveTo(shape.points[0].x, shape.points[0].y);
        for (int i = 1; i < shape.points.size(); ++i) {
            path.lineTo(shape.points[i].x, shape.points[i].y);
        }
        path.closeSubpath();

        painter.fillPath(path, QColor(255, 0, 0, 50));
        painter.setPen(QPen(QColor(255, 0, 0), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }
}

// ============================================================================
// drawTempRect - 绘制正在拖动的临时矩形
// ============================================================================
// 虚线边框 + 半透明填充，表示尚未确认的矩形
// ============================================================================
void FenceOverlay::drawTempRect(QPainter &painter)
{
    QRect rect(rectStart_, rectEnd_);
    painter.setPen(QPen(QColor(255, 0, 0, 180), 2, Qt::DashLine));
    painter.setBrush(QColor(255, 0, 0, 30));
    painter.drawRect(rect);
}

// ============================================================================
// drawTempPolygon - 绘制正在绘制的临时多边形
// ============================================================================
// 已点击的顶点用小圆点标记，顶点之间用虚线连接
// ============================================================================
void FenceOverlay::drawTempPolygon(QPainter &painter)
{
    QPen pen(QColor(255, 0, 0, 180), 2, Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(QColor(255, 0, 0, 30));

    // 绘制已有点之间的连线
    QPainterPath path;
    path.moveTo(polyPoints_[0]);
    for (int i = 1; i < polyPoints_.size(); ++i) {
        path.lineTo(polyPoints_[i]);
    }
    painter.drawPath(path);

    // 绘制顶点圆点
    painter.setBrush(QColor(255, 0, 0, 150));
    for (const auto &pt : polyPoints_) {
        painter.drawEllipse(pt, 4, 4);
    }
}

// ============================================================================
// mousePressEvent - 鼠标按下事件
// ============================================================================
// 矩形模式：左键按下开始拖动
// 多边形模式：左键添加顶点，右键闭合完成
// 删除模式：点击围栏形状删除
// ============================================================================
void FenceOverlay::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton)
        return;

    QPoint pos = event->pos();

    if (drawMode_ == DrawMode::RectDraw) {
        if (event->button() == Qt::LeftButton) {
            rectStart_ = pos;
            rectEnd_ = pos;
            rectDragging_ = true;
        }
    } else if (drawMode_ == DrawMode::PolyDraw) {
        if (event->button() == Qt::LeftButton) {
            // 左键点击添加顶点
            polyPoints_.append(pos);
            update();
        } else if (event->button() == Qt::RightButton) {
            // 右键闭合多边形（至少 3 个点）
            if (polyPoints_.size() >= 3) {
                FenceShape shape;
                shape.type = ShapeType::Polygon;
                for (const auto &pt : polyPoints_) {
                    shape.points.push_back(Point(pt.x(), pt.y()));
                }
                channelFence_.shapes.push_back(shape);
                channelFence_.enabled = true;
                saveFences();
            }
            polyPoints_.clear();
            update();
        }
    } else if (drawMode_ == DrawMode::DeleteMode) {
        // 从后往前遍历，优先删除最上层的围栏
        for (int i = channelFence_.shapes.size() - 1; i >= 0; --i) {
            const auto &shape = channelFence_.shapes[i];
            bool hit = false;
            if (shape.type == ShapeType::Rectangle && shape.points.size() >= 2) {
                // 矩形：判断点击点是否在矩形范围内
                int x1 = std::min(shape.points[0].x, shape.points[1].x);
                int y1 = std::min(shape.points[0].y, shape.points[1].y);
                int x2 = std::max(shape.points[0].x, shape.points[1].x);
                int y2 = std::max(shape.points[0].y, shape.points[1].y);
                hit = (pos.x() >= x1 && pos.x() <= x2 && pos.y() >= y1 && pos.y() <= y2);
            } else {
                // 多边形：用 QPainterPath::contains 判断
                QPainterPath path;
                path.moveTo(shape.points[0].x, shape.points[0].y);
                for (int j = 1; j < shape.points.size(); ++j) {
                    path.lineTo(shape.points[j].x, shape.points[j].y);
                }
                path.closeSubpath();
                hit = path.contains(pos);
            }
            if (hit) {
                channelFence_.shapes.erase(channelFence_.shapes.begin() + i);
                saveFences();
                update();
                return;
            }
        }
    }
}

// ============================================================================
// mouseMoveEvent - 鼠标移动事件
// ============================================================================
// 矩形拖动时实时更新临时矩形
// ============================================================================
void FenceOverlay::mouseMoveEvent(QMouseEvent *event)
{
    if (drawMode_ == DrawMode::RectDraw && rectDragging_) {
        rectEnd_ = event->pos();
        update();
    }
}

// ============================================================================
// mouseReleaseEvent - 鼠标松开事件
// ============================================================================
// 矩形模式：松开完成矩形绘制
// 最小尺寸 5x5 像素，太小则忽略（防止误触）
// ============================================================================
void FenceOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (drawMode_ == DrawMode::RectDraw && event->button() == Qt::LeftButton && rectDragging_) {
        rectDragging_ = false;
        rectEnd_ = event->pos();

        int x1 = qMin(rectStart_.x(), rectEnd_.x());
        int y1 = qMin(rectStart_.y(), rectEnd_.y());
        int x2 = qMax(rectStart_.x(), rectEnd_.x());
        int y2 = qMax(rectStart_.y(), rectEnd_.y());

        // 最小尺寸检查
        if ((x2 - x1) > 5 && (y2 - y1) > 5) {
            FenceShape shape;
            shape.type = ShapeType::Rectangle;
            shape.points.push_back(Point(x1, y1));
            shape.points.push_back(Point(x2, y2));
            channelFence_.shapes.push_back(shape);
            channelFence_.enabled = true;
            saveFences();
        }
        update();
    }
}

// ============================================================================
// mouseDoubleClickEvent - 鼠标双击事件
// ============================================================================
// 多边形模式：双击闭合多边形（等同于右键闭合）
// ============================================================================
void FenceOverlay::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (drawMode_ == DrawMode::PolyDraw && event->button() == Qt::LeftButton) {
        if (polyPoints_.size() >= 3) {
            FenceShape shape;
            shape.type = ShapeType::Polygon;
            for (const auto &pt : polyPoints_) {
                shape.points.push_back(Point(pt.x(), pt.y()));
            }
            channelFence_.shapes.push_back(shape);
            channelFence_.enabled = true;
            saveFences();
        }
        polyPoints_.clear();
        update();
    }
}

} // namespace geofence
