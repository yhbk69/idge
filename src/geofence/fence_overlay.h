#pragma once

// ============================================================================
// fence_overlay.h - 电子围栏绘制覆盖层
// ============================================================================
//
// 概述：
//   透明的 QWidget 覆盖在 GLVideoWidget 上方，用于：
//   1. 绘制已保存的围栏（红色半透明填充 + 边框）
//   2. 处理鼠标交互，支持绘制新围栏
//   3. 支持删除已有围栏
//
// 交互模式（DrawMode）：
//   NoMode:     空闲模式，不处理鼠标事件
//   RectDraw:   矩形绘制，按下拖动松开完成
//   PolyDraw:   多边形绘制，逐点点击，右键/双击闭合
//   DeleteMode: 删除模式，点击围栏删除
//
// 坐标空间：
//   所有坐标都是 overlay widget 的像素坐标。
//   保存时同时记录 widget 尺寸，检测时用于坐标映射。
//
// ============================================================================

#include <QWidget>
#include <QVector>
#include <QPoint>
#include "fence_shape.h"

// ============================================================================
// DrawMode - 绘制模式枚举
// ============================================================================
// 注意：不使用 enum class 避免与 X11 宏 None 冲突
// ============================================================================
enum class DrawMode {
    NoMode,      // 空闲
    RectDraw,    // 矩形绘制
    PolyDraw,    // 多边形绘制
    DeleteMode   // 删除模式
};

namespace geofence {

class FenceOverlay : public QWidget {
    Q_OBJECT
public:
    explicit FenceOverlay(QWidget *parent = nullptr);

    // 设置当前操作的通道号
    void setChannel(int channel);

    // 设置绘制模式
    void setDrawMode(DrawMode mode);
    DrawMode drawMode() const { return drawMode_; }

    // 从 FenceManager 加载围栏数据并刷新显示
    void loadFences();

    // 将当前围栏数据保存到 FenceManager
    void saveFences();

signals:
    void fenceUpdated();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    // 绘制一个已保存的围栏形状
    void drawFenceShape(QPainter &painter, const FenceShape &shape);

    // 绘制正在拖动的临时矩形
    void drawTempRect(QPainter &painter);

    // 绘制正在绘制的临时多边形
    void drawTempPolygon(QPainter &painter);

    int channel_ = 0;                // 当前操作的通道号
    DrawMode drawMode_ = DrawMode::NoMode;  // 当前绘制模式

    // 矩形绘制状态
    QPoint rectStart_;               // 鼠标按下点
    QPoint rectEnd_;                 // 鼠标当前位置/松开点
    bool rectDragging_ = false;      // 是否正在拖动

    // 多边形绘制状态
    QVector<QPoint> polyPoints_;     // 已点击的顶点列表

    ChannelFence channelFence_;      // 当前通道的围栏数据
};

} // namespace geofence
