// ============================================================================
// fence_canvas.cpp - 围栏草稿编辑画布实现
// ============================================================================
// 坐标与交互的总口径见 fence_canvas.h 的文件头。这里只写实现里几处
// 容易踩的点：
//   - 所有几何都在 **widget 像素空间**做（命中、拖动、绘制），只有落进
//     PadRegion 那一刻才换算成 0~1 —— 像素空间的命中精度是用户能直接感知的，
//     归一化浮点的舍入误差不该掺进来。
//   - 拖动过程只 update() 不发信号，松开才发 regionsEdited()：管理页的
//     「已修改」标记与落库都该在动作结束时发生。
// ============================================================================

#include "fence_canvas.h"

#include "fence_palette.h"   // 与视频叠加层/截图同一套配色

#include <QBrush>
#include <QColor>
#include <QCursor>
#include <QFont>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QResizeEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QtMath>

namespace geofence {
namespace {

// 锚点命中半径（widget 像素）。12px 是手指能稳定点中、又不至于"点旁边就动"的折中。
constexpr double kAnchorHitRadius = 12.0;
// 矩形绘制的最小尺寸（widget 像素）。防误触：轻点不该落一个零面积区域。
constexpr double kMinRectPixels = 3.0;
// "单击"与"拖动"的分界（widget 像素）：Edit 态空白处按下到松手位移不超过它
// 才算一次点击（给多边形追加顶点），超过就什么都不做——触屏上没有右键，
// 误拖不该留下一个用户没点过的顶点。
constexpr double kTapSlop = 4.0;

}  // namespace

FenceCanvas::FenceCanvas(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);   // Esc/Backspace 走 keyPressEvent，点画布即拿焦点
    setMinimumSize(320, 240);
    refreshTimer_.setInterval(1000);
    connect(&refreshTimer_, &QTimer::timeout, this, &FenceCanvas::frameRefreshRequested);
}

// ============================================================================
// 数据与状态设置
// ============================================================================

void FenceCanvas::setRegions(const QVector<PadRegion> &regions)
{
    regions_ = regions;
    dragRegion_ = -1;
    dragVertex_ = -1;
    appendPending_ = false;
    polyPoints_.clear();
    rectDragging_ = false;
    update();
}

void FenceCanvas::setMode(CanvasMode mode)
{
    mode_ = mode;
    clearInProgressDrawing();
    updateCursor();
    update();
    emit modeChanged(mode_);
}

void FenceCanvas::setCreateRule(const QString &ruleType)
{
    createRule_ = ruleType;
}

void FenceCanvas::setBackground(const QImage &image)
{
    if (background_ == image)
        return;
    background_ = image;
    update();
}

void FenceCanvas::clearInProgressDrawing()
{
    polyPoints_.clear();
    rectDragging_ = false;
    dragRegion_ = -1;
    dragVertex_ = -1;
    appendPending_ = false;
}

void FenceCanvas::updateCursor()
{
    switch (mode_) {
    case CanvasMode::RectDraw:
    case CanvasMode::PolyDraw:
        setCursor(Qt::CrossCursor);
        break;
    case CanvasMode::DeleteMode:
        setCursor(Qt::PointingHandCursor);
        break;
    case CanvasMode::Edit:
        break;
    }
}

// ============================================================================
// 坐标换算
// ============================================================================

QRect FenceCanvas::contentRect() const
{
    const QRect full = rect();
    if (background_.isNull() || full.isEmpty())
        return full;
    // 等比居中（letterbox）：与视频页 FenceOverlay 的 contentRect 同口径，
    // 编辑时看到的画面比例与监控画面一致，形状不会在两个界面间变形。
    const QSize scaled = background_.size().scaled(full.size(), Qt::KeepAspectRatio);
    return QRect(full.left() + (full.width() - scaled.width()) / 2,
                 full.top() + (full.height() - scaled.height()) / 2,
                 scaled.width(), scaled.height());
}

QPointF FenceCanvas::clampToContent(const QPointF &point) const
{
    const QRect canvas = contentRect();
    return QPointF(qBound(static_cast<double>(canvas.left()), point.x(),
                          static_cast<double>(canvas.left() + canvas.width())),
                   qBound(static_cast<double>(canvas.top()), point.y(),
                          static_cast<double>(canvas.top() + canvas.height())));
}

QPointF FenceCanvas::toWidget(const QPointF &padPoint, const QRect &canvas)
{
    return QPointF(canvas.left() + padPoint.x() * canvas.width(),
                   canvas.top() + padPoint.y() * canvas.height());
}

QPointF FenceCanvas::toPad(const QPointF &widgetPoint, const QRect &canvas)
{
    if (canvas.width() <= 0 || canvas.height() <= 0)
        return QPointF();   // 还没布局好，不猜（同 fence_shape.h 的口径）
    return QPointF((widgetPoint.x() - canvas.left()) / canvas.width(),
                   (widgetPoint.y() - canvas.top()) / canvas.height());
}

QVector<QPointF> FenceCanvas::regionWidgetPoints(const PadRegion &region, const QRect &canvas)
{
    QVector<QPointF> points;
    points.reserve(region.points.size());
    for (const QPointF &point : region.points) {
        points << toWidget(point, canvas);
    }
    return points;
}

QRect FenceCanvas::regionWidgetRect(const PadRegion &region, const QRect &canvas)
{
    const QVector<QPointF> points = regionWidgetPoints(region, canvas);
    if (points.isEmpty())
        return {};
    int minX = static_cast<int>(points.at(0).x());
    int maxX = minX;
    int minY = static_cast<int>(points.at(0).y());
    int maxY = minY;
    for (const QPointF &point : points) {
        const int x = static_cast<int>(point.x());
        const int y = static_cast<int>(point.y());
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
    }
    return QRect(minX, minY, maxX - minX, maxY - minY);
}

bool FenceCanvas::pointInPolygon(const QPointF &point, const QVector<QPointF> &polygon)
{
    // 射线法。n<3 无内部可言，直接判否（与 FenceChecker 的口径一致）。
    const int n = polygon.size();
    if (n < 3)
        return false;
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const QPointF &pi = polygon.at(i);
        const QPointF &pj = polygon.at(j);
        const bool straddles = ((pj.y() > point.y()) != (pi.y() > point.y()));
        if (!straddles)
            continue;
        const double xCross = (pj.x() - pi.x()) * (point.y() - pi.y()) / (pj.y() - pi.y()) + pi.x();
        if (point.x() < xCross)
            inside = !inside;
    }
    return inside;
}

// ============================================================================
// 命中测试
// ============================================================================

bool FenceCanvas::hitAnchor(const QPointF &point, int *regionIndex, int *vertexIndex) const
{
    const QRect canvas = contentRect();
    // 从后往前：区域是后画的在上，锚点命中优先最上层。
    for (int r = regions_.size() - 1; r >= 0; --r) {
        const QVector<QPointF> points = regionWidgetPoints(regions_.at(r), canvas);
        for (int v = 0; v < points.size(); ++v) {
            const double dx = point.x() - points.at(v).x();
            const double dy = point.y() - points.at(v).y();
            if (qSqrt(dx * dx + dy * dy) <= kAnchorHitRadius) {
                *regionIndex = r;
                *vertexIndex = v;
                return true;
            }
        }
    }
    return false;
}

int FenceCanvas::hitRegionBody(const QPointF &point) const
{
    const QRect canvas = contentRect();
    for (int r = regions_.size() - 1; r >= 0; --r) {
        const PadRegion &region = regions_.at(r);
        if (region.shapeType == QLatin1String("RECT")) {
            // RECT 的 4 点存储顺序不保证，先归一化成外接矩形再判，与顶点顺序无关。
            if (regionWidgetRect(region, canvas).contains(point.toPoint()))
                return r;
        } else if (pointInPolygon(point, regionWidgetPoints(region, canvas))) {
            return r;
        }
    }
    return -1;
}

// ============================================================================
// 拖动编辑
// ============================================================================

void FenceCanvas::beginDrag(int regionIndex, int vertexIndex, const QPointF &pressPoint)
{
    dragRegion_ = regionIndex;
    dragVertex_ = vertexIndex;
    dragPress_ = pressPoint;
    dragOriginPoints_ = regionWidgetPoints(regions_.at(regionIndex), contentRect());
}

void FenceCanvas::applyDragDelta(const QPointF &deltaWidget)
{
    if (dragRegion_ < 0)
        return;
    const QRect canvas = contentRect();
    PadRegion &region = regions_[dragRegion_];

    if (dragVertex_ >= 0) {
        // 锚点拖动：只动一个顶点
        const QPointF dragged = dragOriginPoints_.at(dragVertex_) + deltaWidget;
        if (region.shapeType == QLatin1String("RECT")) {
            // 对角固定不动，用两角算出新的外接矩形。对角必须在**原始矩形**里找
            // （离被拖顶点最远的那个 = 对角线端点）；若按拖动后的位置找，大幅内拖
            // 时邻角会比对角更远，矩形会翻到错误的角上。
            // 不依赖存储顺序：RECT 是轴对齐的，min/max 扫描即得新边界。
            const QPointF origin = dragOriginPoints_.at(dragVertex_);
            QPointF fixed = dragOriginPoints_.at(0);
            double bestDist = -1.0;
            for (int i = 0; i < dragOriginPoints_.size(); ++i) {
                if (i == dragVertex_)
                    continue;
                const double dx = dragOriginPoints_.at(i).x() - origin.x();
                const double dy = dragOriginPoints_.at(i).y() - origin.y();
                const double dist = qSqrt(dx * dx + dy * dy);
                if (dist > bestDist) {
                    bestDist = dist;
                    fixed = dragOriginPoints_.at(i);
                }
            }
            QPointF newCorner = dragged;
            newCorner.setX(qBound(static_cast<double>(canvas.left()), newCorner.x(),
                                  static_cast<double>(canvas.left() + canvas.width())));
            newCorner.setY(qBound(static_cast<double>(canvas.top()), newCorner.y(),
                                  static_cast<double>(canvas.top() + canvas.height())));
            const double xMin = qMin(fixed.x(), newCorner.x());
            const double xMax = qMax(fixed.x(), newCorner.x());
            const double yMin = qMin(fixed.y(), newCorner.y());
            const double yMax = qMax(fixed.y(), newCorner.y());
            region.points.clear();
            region.points << toPad(QPointF(xMin, yMin), canvas)
                          << toPad(QPointF(xMax, yMin), canvas)
                          << toPad(QPointF(xMax, yMax), canvas)
                          << toPad(QPointF(xMin, yMax), canvas);
        } else {
            // 多边形：只挪这一个顶点，钳进内容矩形
            QPointF vertex = dragOriginPoints_.at(dragVertex_) + deltaWidget;
            vertex = clampToContent(vertex);
            region.points[dragVertex_] = toPad(vertex, canvas);
        }
    } else {
        // 整体平移：delta 是**从按下起的总位移**，必须叠在原始顶点上——
        // 累加到已移动的点上会越拖越快（用户报的"乱跳"）；钳制边界也必须用
        // **原始**包围盒，用移动后的边界会随拖动不断收缩，表现为"自动贴边"。
        // 位移没有原点，只除以内容矩形尺寸，不能减 left/top。
        QPointF deltaPad(deltaWidget.x() / canvas.width(), deltaWidget.y() / canvas.height());
        double minX = 1.0, maxX = 0.0, minY = 1.0, maxY = 0.0;
        for (const QPointF &origin : dragOriginPoints_) {
            const QPointF pad = toPad(origin, canvas);
            minX = qMin(minX, pad.x());
            maxX = qMax(maxX, pad.x());
            minY = qMin(minY, pad.y());
            maxY = qMax(maxY, pad.y());
        }
        deltaPad.setX(qBound(-minX, deltaPad.x(), 1.0 - maxX));
        deltaPad.setY(qBound(-minY, deltaPad.y(), 1.0 - maxY));
        for (int i = 0; i < region.points.size() && i < dragOriginPoints_.size(); ++i) {
            region.points[i] = toPad(dragOriginPoints_.at(i), canvas) + deltaPad;
        }
    }
}

// ============================================================================
// 新区域落笔
// ============================================================================

void FenceCanvas::finishRect(const QPointF &start, const QPointF &end)
{
    const QRect canvas = contentRect();
    const double x1 = qMin(start.x(), end.x());
    const double y1 = qMin(start.y(), end.y());
    const double x2 = qMax(start.x(), end.x());
    const double y2 = qMax(start.y(), end.y());
    if (x2 - x1 < kMinRectPixels || y2 - y1 < kMinRectPixels)
        return;   // 轻点/误触，不落区域

    PadRegion region;
    region.shapeType = QStringLiteral("RECT");
    region.ruleType = createRule_;
    // 四点顺序 左上→右上→右下→左下（2026-11-17 用户裁定，传 Pad 与落库同序）。
    // PadRegion::toJson 出口还会再归一一次，旧数据/对端乱序也兜得住。
    region.points << toPad(QPointF(x1, y1), canvas)
                  << toPad(QPointF(x2, y1), canvas)
                  << toPad(QPointF(x2, y2), canvas)
                  << toPad(QPointF(x1, y2), canvas);
    regions_.append(region);
    emit regionsEdited();
}

void FenceCanvas::commitPolygon()
{
    const QRect canvas = contentRect();
    PadRegion region;
    region.shapeType = QStringLiteral("POLYGON");
    region.ruleType = createRule_;
    for (const QPointF &point : polyPoints_) {
        region.points << toPad(point, canvas);
    }
    regions_.append(region);
    polyPoints_.clear();
    // 闭合后回 Edit：形状已成型，下一次点击不该又起一个新形状；
    // 要接着画由用户再点工具钮，误触成本最低。
    setMode(CanvasMode::Edit);
    emit regionsEdited();
}

void FenceCanvas::cancelPolygon()
{
    // 不足 3 点的进行中绘制直接作废：不弹框、不落区域，
    // 误点不会多出一个用户没见过的形状。
    polyPoints_.clear();
    update();
}

void FenceCanvas::undoLastPolyPoint()
{
    if (polyPoints_.isEmpty())
        return;
    polyPoints_.removeLast();
    update();
}

// ============================================================================
// 鼠标事件
// ============================================================================

void FenceCanvas::mousePressEvent(QMouseEvent *event)
{
    const QPointF pos = event->pos();
    cursorPos_ = pos;

    if (mode_ == CanvasMode::Edit) {
        if (event->button() == Qt::LeftButton) {
            int regionIndex = -1;
            int vertexIndex = -1;
            if (hitAnchor(pos, &regionIndex, &vertexIndex)) {
                beginDrag(regionIndex, vertexIndex, pos);   // 拖锚点
            } else if (hitRegionBody(pos) >= 0) {
                beginDrag(hitRegionBody(pos), -1, pos);     // 拖整体
            } else {
                // 空白处：候选"追加顶点"。松手位移没超过 slop 才真追加到
                // 当前多边形末尾（按 1-2-…-n-1 点序闭合）；触屏没有右键，
                // 删除走 DeleteMode 工具，这里不收右键。
                appendPending_ = true;
                appendPos_ = clampToContent(pos);
            }
            update();
        }
    } else if (mode_ == CanvasMode::RectDraw) {
        if (event->button() == Qt::LeftButton) {
            rectStart_ = clampToContent(pos);   // 黑边上的落笔钳进画面，不出 0~1 坐标
            rectEnd_ = rectStart_;
            rectDragging_ = true;
            update();
        }
    } else if (mode_ == CanvasMode::PolyDraw) {
        if (event->button() == Qt::LeftButton) {
            polyPoints_.append(clampToContent(pos));
            if (polyPoints_.size() >= 3)
                commitPolygon();   // 第 3 点落地自动闭合（2026-11-17 用户裁定）
            update();
        }
    } else if (mode_ == CanvasMode::DeleteMode) {
        if (event->button() == Qt::LeftButton) {
            const int regionIndex = hitRegionBody(pos);
            if (regionIndex >= 0) {
                regions_.remove(regionIndex);
                emit regionsEdited();
                update();
            }
        }
    }
}

void FenceCanvas::mouseMoveEvent(QMouseEvent *event)
{
    cursorPos_ = event->pos();
    if (dragRegion_ >= 0) {
        applyDragDelta(cursorPos_ - dragPress_);
        update();
    } else if (rectDragging_) {
        rectEnd_ = clampToContent(cursorPos_);
        update();
    } else if (mode_ == CanvasMode::PolyDraw && !polyPoints_.isEmpty()) {
        update();   // 虚线预览跟着光标动
    }
}

void FenceCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (dragRegion_ >= 0) {
        dragRegion_ = -1;
        dragVertex_ = -1;
        emit regionsEdited();   // 拖动结束才算一次完整编辑
        update();
    } else if (rectDragging_) {
        rectDragging_ = false;
        finishRect(rectStart_, rectEnd_);
        update();
    } else if (appendPending_) {
        appendPending_ = false;
        const QPointF pos = event->pos();
        const double dx = pos.x() - appendPos_.x();
        const double dy = pos.y() - appendPos_.y();
        if (qSqrt(dx * dx + dy * dy) > kTapSlop)
            return;   // 是拖不是点：不留顶点
        // 追加到**最后一个多边形**末尾：点序即 1-2-…-n，画闭合多边形时
        // 自动 n→1 收口（2026-11-17 用户裁定"在其他位置加点，按点序闭合"）。
        // 只有 RECT 时不追加：矩形加第 5 点会破坏矩形语义。
        for (int r = regions_.size() - 1; r >= 0; --r) {
            if (regions_[r].shapeType != QLatin1String("POLYGON"))
                continue;
            regions_[r].points.append(toPad(clampToContent(pos), contentRect()));
            emit regionsEdited();
            update();
            break;
        }
    }
}

void FenceCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    // 双击的第二下 press 已先落了点，这里统一按"取消重画"处理：
    // 手抖双击不该提交一个带重复顶点的形状。
    if (mode_ == CanvasMode::PolyDraw && event->button() == Qt::LeftButton) {
        cancelPolygon();
    }
}

void FenceCanvas::keyPressEvent(QKeyEvent *event)
{
    if (mode_ == CanvasMode::PolyDraw) {
        if (event->key() == Qt::Key_Escape) {
            cancelPolygon();
            return;
        }
        if (event->key() == Qt::Key_Backspace) {
            undoLastPolyPoint();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

void FenceCanvas::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    update();   // 背景拉伸尺寸变了，重绘
}

void FenceCanvas::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refreshTimer_.start();   // 可见才需要新帧，隐藏时停掉，不空转
}

void FenceCanvas::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    refreshTimer_.stop();
}

// ============================================================================
// 绘制
// ============================================================================

QColor FenceCanvas::ruleColor(const QString &ruleType)
{
    // 配色唯一出处：fence_palette.h（INTRUSION 红 #E53935、CROSS 琥珀 #FB8C00）。
    return geofence::fenceLineColor(geofence::roleFromRuleType(ruleType));
}

void FenceCanvas::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRect canvas = contentRect();

    // 黑边打底，背景等比居中：画面比例与监控页一致，换算基准是 contentRect。
    painter.fillRect(rect(), QColor(16, 20, 28));
    if (!background_.isNull()) {
        painter.drawImage(canvas, background_.scaled(canvas.size(), Qt::KeepAspectRatio,
                                                     Qt::SmoothTransformation));
    } else {
        painter.setPen(QColor(255, 255, 255, 120));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("等待视频画面…"));
    }

    // 提示条已按用户要求移除（2026-11-17）：触屏界面不叠操作说明文字。
    for (int index = 0; index < regions_.size(); ++index) {
        drawRegion(painter, regions_.at(index), index);
    }

    if (mode_ == CanvasMode::Edit) {
        drawAnchors(painter);
    }

    if (rectDragging_) {
        drawTempRect(painter);
    }
    if (!polyPoints_.isEmpty()) {
        drawTempPolygon(painter);
    }
}

void FenceCanvas::drawRegion(QPainter &painter, const PadRegion &region, int index)
{
    const QRect canvas = contentRect();
    const QColor color = ruleColor(region.ruleType);
    QColor fill = color;
    fill.setAlpha(60);

    if (region.shapeType == QLatin1String("RECT")) {
        const QRect regionRect = regionWidgetRect(region, canvas);
        if (regionRect.width() > 0 && regionRect.height() > 0) {
            painter.fillRect(regionRect, fill);
            painter.setPen(QPen(color, 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(regionRect);
        }
    } else {
        const QVector<QPointF> points = regionWidgetPoints(region, canvas);
        if (points.size() < 2)
            return;
        QPolygonF polygon;
        polygon.reserve(points.size());
        for (const QPointF &point : points) {
            polygon << point;
        }
        painter.setBrush(fill);
        painter.setPen(QPen(color, 2));
        painter.drawPolygon(polygon);
    }

    // 区域序号：数组下标 +1（§5.2 明写数组顺序就是编号），与
    // fence_frame_renderer 的合成画面同款标注，用户对照的是同一套编号。
    const QVector<QPointF> widgetPoints = regionWidgetPoints(region, canvas);
    if (widgetPoints.isEmpty())
        return;
    QPointF centroid;
    for (const QPointF &point : widgetPoints) {
        centroid += point;
    }
    centroid /= widgetPoints.size();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 190));
    painter.drawEllipse(centroid, 7, 7);
    QFont labelFont = painter.font();
    labelFont.setPixelSize(11);
    labelFont.setBold(true);
    painter.setFont(labelFont);
    painter.setPen(color.lighter(170));
    painter.drawText(QRectF(centroid.x() - 7, centroid.y() - 7, 14, 14),
                     Qt::AlignCenter, QString::number(index + 1));
}

void FenceCanvas::drawAnchors(QPainter &painter)
{
    const QRect canvas = contentRect();
    QFont numFont = painter.font();
    numFont.setPixelSize(10);
    numFont.setBold(true);
    for (int r = 0; r < regions_.size(); ++r) {
        const QColor color = ruleColor(regions_.at(r).ruleType);
        const QVector<QPointF> points = regionWidgetPoints(regions_.at(r), canvas);
        for (int v = 0; v < points.size(); ++v) {
            const bool hot = (dragRegion_ == r && dragVertex_ == v);
            painter.setPen(QPen(color, hot ? 2 : 1));
            painter.setBrush(hot ? color : Qt::white);
            painter.drawEllipse(points.at(v), hot ? 6 : 4.5, hot ? 6 : 4.5);
            // 顶点序号 = 存储点序（RECT 为 左上1→右上2→右下3→左下4），
            // 拖锚点时能对上号；黑底白字小圆盘，与区域编号同款。
            const QPointF label = points.at(v) + QPointF(10, -10);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 190));
            painter.drawEllipse(label, 7, 7);
            painter.setFont(numFont);
            painter.setPen(Qt::white);
            painter.drawText(QRectF(label.x() - 7, label.y() - 7, 14, 14),
                             Qt::AlignCenter, QString::number(v + 1));
        }
    }
}

void FenceCanvas::drawTempRect(QPainter &painter)
{
    const QRect rectBounds(QPoint(qMin(rectStart_.x(), rectEnd_.x()), qMin(rectStart_.y(), rectEnd_.y())),
                           QPoint(qMax(rectStart_.x(), rectEnd_.x()), qMax(rectStart_.y(), rectEnd_.y())));
    const QColor color = ruleColor(createRule_);
    QColor fill = color;
    fill.setAlpha(50);
    painter.setPen(QPen(color.lighter(120), 2, Qt::DashLine));
    painter.setBrush(fill);
    painter.drawRect(rectBounds);
}

void FenceCanvas::drawTempPolygon(QPainter &painter)
{
    const QColor color = ruleColor(createRule_);
    QColor fill = color;
    fill.setAlpha(45);

    if (polyPoints_.size() >= 2) {
        // 已有 2 点：光标就是第 3 点，直接预览**闭合后的多边形**——
        // 用户落第 3 点前看到的就是将要提交的形状。
        QPolygonF preview;
        preview << polyPoints_.at(0) << polyPoints_.at(1) << clampToContent(cursorPos_);
        painter.setPen(QPen(color.lighter(120), 2, Qt::DashLine));
        painter.setBrush(fill);
        painter.drawPolygon(preview);
    } else {
        // 只有 1 点：到光标的虚线
        painter.setPen(QPen(color.lighter(120), 1, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(polyPoints_.last(), clampToContent(cursorPos_));
    }

    // 顶点圆点加大到 6px 并描白边：暗背景上 4px 纯色点几乎看不见。
    // 每个点带序号（落笔顺序 = 存储点序），与区域编号圆盘同款黑底白字。
    QFont numFont = painter.font();
    numFont.setPixelSize(11);
    numFont.setBold(true);
    for (int i = 0; i < polyPoints_.size(); ++i) {
        const QPointF &point = polyPoints_.at(i);
        painter.setPen(QPen(Qt::white, 1));
        painter.setBrush(QColor(color.red(), color.green(), color.blue(), 200));
        painter.drawEllipse(point, 6, 6);
        const QPointF label = point + QPointF(11, -11);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 190));
        painter.drawEllipse(label, 8, 8);
        painter.setFont(numFont);
        painter.setPen(Qt::white);
        painter.drawText(QRectF(label.x() - 8, label.y() - 8, 16, 16),
                         Qt::AlignCenter, QString::number(i + 1));
    }
}

} // namespace geofence
