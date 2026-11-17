#pragma once

// ============================================================================
// fence_overlay.h - 电子围栏显示覆盖层（只读）
// ============================================================================
//
// 概述：
//   透明的 QWidget 覆盖在 GLVideoWidget 上方，只负责把 FenceManager 里的
//   围栏画出来；绘制/删除入口已经移到标题栏的「电子围栏」页，这里不再收鼠标。
//
// 配色：
//   与「电子围栏」管理页同源（fence_palette.h）：
//     禁区 Forbidden → #E53935
//     岗区域 Workzone → #FB8C00
//
// 坐标空间：
//   围栏顶点存的是归一化的视频内容坐标（0..kFenceNormBase，见 fence_shape.h）。
//   换算基准是 setContentRect 推进来的 contentRect（不含黑边），换视图模式、
//   全屏、拖窗口都只改 contentRect，不改围栏语义。
//
// ============================================================================

#include <QWidget>
#include <QRect>

#include "fence_shape.h"

namespace geofence {

class FenceOverlay : public QWidget {
    Q_OBJECT
public:
    explicit FenceOverlay(QWidget *parent = nullptr);

    /// 设置当前显示的通道号
    void setChannel(int channel);

    /// 视频内容矩形（GLVideoWidget::contentRect()，widget 像素坐标，已扣掉黑边）。
    /// 这是"归一化坐标 ↔ 屏幕像素"唯一的换算基准，由 PlayerWidget 在 resize 与
    /// 首帧到达时推过来。尺寸 <=0 时换算退化成直接用 widget 像素。
    void setContentRect(const QRect &contentRect);

    /// 从 FenceManager 加载围栏数据并刷新显示
    void loadFences();

    /// 当前显示的围栏快照（只读；自检用它断言"库刷新后 overlay 真的跟着重读了"）
    const ChannelFence &channelFence() const { return channelFence_; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QPoint toWidget(const Point &p) const;     // 围栏顶点 → 屏幕像素
    void drawFenceShape(QPainter &painter, const FenceShape &shape);

    int channel_ = 0;
    ChannelFence channelFence_;      // 当前通道的围栏数据（顶点为归一化坐标）
    QRect contentRect_;              // 视频内容区（widget 像素），换算基准
};

}  // namespace geofence
