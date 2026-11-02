#pragma once

// ============================================================================
// fence_overlay.h - 电子围栏绘制覆盖层
// ============================================================================
//
// 概述：
//   透明的 QWidget 覆盖在 GLVideoWidget 上方，用于：
//   1. 绘制已保存的围栏（按角色配色：禁区红、岗区域绿）
//   2. 处理鼠标交互，支持绘制新围栏
//   3. 支持删除已有围栏
//
// 交互模式（DrawMode）：
//   NoMode:     空闲模式，不处理鼠标事件
//   RectDraw:   矩形绘制，按下拖动松开完成
//   PolyDraw:   多边形绘制，逐点点击；闭合有三个入口 —— 工具栏「闭合」钮
//               （触屏唯一可靠的那条）、右键、双击
//   DeleteMode: 删除模式，点击围栏删除
//
// 角色（FenceRole）与模式是**两个正交的维度**：模式决定"怎么画"，
//   角色决定"画出来的这个形状触发哪种报警"。绘制前用工具栏选角色，落盘的
//   每个形状自带 role。角色创建后不可改（删除按 vector 下标命中，没有稳定 id），
//   要改就删了重画。
//
// 坐标空间：
//   围栏顶点存的是**归一化的视频内容坐标**（0..kFenceNormBase，见 fence_shape.h）。
//   本 widget 只负责"把归一化坐标画成像素 / 把像素点击换算回归一化"，换算基准是
//   set 进来的 contentRect（GLVideoWidget 里真正有画面的那块矩形，不含黑边）。
//   所以换视图模式、全屏、拖窗口都只改 contentRect，不改围栏语义。
//   临时图形（拖动中的矩形、画到一半的顶点）仍然用 widget 像素存 —— 它们不落盘，
//   只在落盘那一刻统一 toNorm。
//
// ============================================================================

#include <QWidget>
#include <QVector>
#include <QPoint>
#include <QRect>
#include <QColor>
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

    // 设置"下一个画出来的形状"属于哪种角色（禁区 / 岗区域）。
    // 与 setDrawMode 不同，这里**不清空**进行中的顶点：切角色不该把用户
    // 已经点多边形点好的顶点静默丢掉。
    void setRole(FenceRole role);
    FenceRole role() const { return role_; }

    // 角色配色（边框色）：禁区红、岗区域绿。放成 public 是因为工具栏的角色按钮
    // 要用同一个颜色画色块 —— 颜色只能有这一个出处，否则按钮上的方块和画布上
    // 的围栏会各说各话。
    static QColor roleColor(FenceRole role);

    // 视频内容矩形（GLVideoWidget::contentRect()，widget 像素坐标，已扣掉黑边）。
    // 这是"归一化坐标 ↔ 屏幕像素"唯一的换算基准，由 PlayerWidget 在 resize 与
    // 首帧到达时推过来。尺寸 <=0 时换算退化成直接用 widget 像素（首帧前的兜底，
    // 宁可画得不准，也不能让 overlay 因为拿不到内容区而整块不显示围栏）。
    void setContentRect(const QRect &contentRect);

    // 从 FenceManager 加载围栏数据并刷新显示
    void loadFences();

    // 将当前围栏数据保存到 FenceManager
    void saveFences();

    // 多边形的手势闭合入口（工具栏「闭合」「撤销」按钮调这两个）。
    // public 的理由是**触屏没有右键**，而闭合原本只挂在右键与双击上；
    // 双击在电容屏上还极易被识别成拖动。右键/双击两条老路径保留，
    // 但都改成调用 closePolygon()，闭合语义只有一份。
    // 不足 3 点：closePolygon 静默返回（不落盘、不弹框），undoLastPoint 空操作。
    void closePolygon();
    void undoLastPoint();

signals:
    void fenceUpdated();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    // ========================================================================
    // 归一化坐标 <-> widget 像素坐标
    // ========================================================================
    // 画围栏（norm → 像素）与收输入（像素 → norm）必须走同一对函数、同一个
    // contentRect_，否则"看到的围栏"和"报警判的围栏"会分家。
    QPoint toWidget(const Point &p) const;     // 围栏顶点 → 屏幕像素
    Point  toNorm(const QPoint &pt) const;     // 鼠标点 → 归一化顶点

    // 绘制一个已保存的围栏形状
    void drawFenceShape(QPainter &painter, const FenceShape &shape);

    // 绘制正在拖动的临时矩形
    void drawTempRect(QPainter &painter);

    // 绘制正在绘制的临时多边形
    void drawTempPolygon(QPainter &painter);

    // 角色配色：三种绘制路径（已存形状 / 临时矩形 / 临时多边形）共用一套，
    // 否则"选的是绿、画出来是红"，用户会按错的图去配规则。
    // 边框色 roleColor 已提到 public（工具栏色块同源），这里只留填充色。
    static QColor roleFillColor(FenceRole role);   // 半透明填充色

    int channel_ = 0;                // 当前操作的通道号
    DrawMode drawMode_ = DrawMode::NoMode;  // 当前绘制模式
    FenceRole role_ = FenceRole::Forbidden; // 下一个形状的角色（默认禁区）

    // 矩形绘制状态
    QPoint rectStart_;               // 鼠标按下点
    QPoint rectEnd_;                 // 鼠标当前位置/松开点
    bool rectDragging_ = false;      // 是否正在拖动

    // 多边形绘制状态
    QVector<QPoint> polyPoints_;     // 已点击的顶点列表

    ChannelFence channelFence_;      // 当前通道的围栏数据（顶点为归一化坐标）

    QRect contentRect_;              // 视频内容区（widget 像素），换算基准
};

} // namespace geofence
