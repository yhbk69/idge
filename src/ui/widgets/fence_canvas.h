#pragma once

// ============================================================================
// fence_canvas.h - 围栏草稿编辑画布（新 Tab 用，§5 的 DRAFT 编辑）
// ============================================================================
//
// 定位：与 FenceOverlay（叠在四宫格视频上的透明层）同族，但分工不同——
//   - FenceOverlay 改的是本机手绘围栏（FenceShape，0..kFenceNormBase 归一化），
//     画完即落库，判定立刻生效。
//   - 本画布改的是 Pad 围栏草稿（PadRegion，§13.4.3 的 0~1 归一化），
//     **只负责几何与交互，不碰库**：任何改动发 regionsChanged()，
//     由管理页决定何时 save-draft（草稿语义：改多少遍都只在点保存时落一次库）。
//
// 背景画面：摄像头当前帧的截图，由外部抓帧后 setBackground() 推进来。
//   抓帧走 frmVideoWindow::playerWidget(ch)->decoder() + captureFrameAsImage()，
//   不另开解码器。背景按 **KeepAspectRatio 居中（letterbox）** 绘制，不拉伸——
//   编辑时看到的画面比例必须与监控画面一致，否则画出来的形状在视频页会变形。
//
// 坐标空间：PadRegion 的点是 0~1 归一化，换算基准是 **contentRect()**（背景
//   实际占据的居中等比矩形），不是 widget 尺寸本身——与 FenceOverlay 的
//   contentRect 同一口径。无背景时 contentRect() 退化为 rect()。
//   画围栏（norm → 像素）与收输入（像素 → norm）必须走同一对函数、同一个
//   contentRect，否则"看到的围栏"与"存出去的 0~1"会分家。
//
// 交互（对应用户定的两条生命周期规则的编辑侧）：
//   Edit      拖锚点（顶点）改形状；拖形状内部整体平移；右键删除
//   RectDraw  按下拖动画矩形（松开落一个 RECT 区域）
//   PolyDraw  逐点画多边形；闭合有三个入口：工具栏「闭合」钮、右键、双击
//   DeleteMode 点击区域删除
//   —— 锚点拖动是本画布相对 FenceOverlay 新增的能力（FenceOverlay 画完即定，
//   顶点不可再动；草稿要反复改，不能每改一次就删了重画）。
//
// 任何几何改动只在**松开/闭合/删除那一刻**发一次 regionsChanged()，
// 拖动过程只重绘不发信号——管理页的「已修改」标记与落库都该在动作结束时发生，
// 鼠标每挪一像素就触发一次是无意义的抖动。
//
// ============================================================================

#include <QImage>
#include <QPointF>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include "pad_fence_store.h"   // PadRegion：画布的数据类型（§13.4.3）

namespace geofence {

// 画布模式。命名与 FenceOverlay 的 DrawMode 对应，但多了 Edit（锚点/平移）
// —— 草稿编辑的主力操作。
enum class CanvasMode {
    Edit,        // 拖锚点改形状、拖内部平移、右键删除区域
    RectDraw,    // 画矩形区域
    PolyDraw,    // 画多边形区域
    DeleteMode   // 点区域删除
};

class FenceCanvas : public QWidget {
    Q_OBJECT
public:
    explicit FenceCanvas(QWidget *parent = nullptr);

    // 载入一批区域（从草稿版本读出来）。只替换数据，不发信号——这是"载入"不是"编辑"。
    void setRegions(const QVector<PadRegion> &regions);
    QVector<PadRegion> regions() const { return regions_; }

    // 当前模式。切模式清掉进行中的绘制（与 FenceOverlay::setDrawMode 同口径：
    // 换工具意味着上一个图形作废）。
    void setMode(CanvasMode mode);
    CanvasMode mode() const { return mode_; }

    // 下一个新画区域的规则类型（INTRUSION/CROSS）。与模式正交：模式决定怎么画，
    // 规则决定画出来触发哪种报警。
    void setCreateRule(const QString &ruleType);
    QString createRule() const { return createRule_; }

    // 背景画面帧。空图 = 清背景（回到等待态）。
    void setBackground(const QImage &image);

    // 归一化坐标 <-> 像素的唯一换算基准：背景等比居中后的内容矩形。
    // 无背景时等于 rect()。
    QRect contentRect() const;

    // 多边形的手势闭合/撤销入口。公开的理由与 FenceOverlay 相同：
    // 触屏没有可靠的右键与双击，闭合必须能由可见按钮触发，且闭合语义只有一份。
    void closePolygon();
    void undoLastPolyPoint();

    // 画布可见时每秒发出一次，外部抓一帧新背景推回来。
    // 由画布自己定时是为了避免管理页写定时器——"什么时候需要新帧"是画布知道的事
    // （只有可见且开着时才需要）。

signals:
    // 几何被改动（拖动结束/新区域/删区域）。管理页据此刷新区域列表与「已修改」态。
    void regionsEdited();
    // 请求外部抓一帧新背景
    void frameRefreshRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    // ---- 坐标换算：PadRegion 0~1 <-> widget 像素（基准 = contentRect()）----
    // 画围栏（norm → 像素）与收输入（像素 → norm）必须走同一对函数、同一个
    // 内容矩形，否则"看到的"和"存出去的"会分家。
    static QPointF toWidget(const QPointF &padPoint, const QRect &canvas);
    static QPointF toPad(const QPointF &widgetPoint, const QRect &canvas);
    /// 把输入点钳进内容矩形（letterbox 黑边上的落笔/拖动不出 0~1 坐标）
    QPointF clampToContent(const QPointF &point) const;
    /// 一个区域的全部顶点换算成 widget 像素
    static QVector<QPointF> regionWidgetPoints(const PadRegion &region, const QRect &canvas);
    /// RECT 的外接矩形（widget 像素）。RECT 的 4 点存储顺序不保证，命中判断前先归一化。
    static QRect regionWidgetRect(const PadRegion &region, const QRect &canvas);
    /// 点是否在多边形内（射线法，widget 像素空间）。n<3 直接判否。
    static bool pointInPolygon(const QPointF &point, const QVector<QPointF> &polygon);

    // ---- 命中测试（Edit 模式用）----
    /// 命中锚点：返回区域下标与顶点下标；没命中返回 false。从后往前找，优先最上层。
    bool hitAnchor(const QPointF &point, int *regionIndex, int *vertexIndex) const;
    /// 命中区域本体：返回区域下标（从后往前，最上层优先）；没命中返回 -1。
    int hitRegionBody(const QPointF &point) const;

    // ---- 动作 ----
    /// 矩形绘制完成：两个对角点 → 规范四点 RECT 区域，追加进 regions_
    void finishRect(const QPointF &start, const QPointF &end);

    /// 拖动进行中：把 delta 应用到被拖区域。delta 是 widget 像素，换算回 0~1 后逐顶点应用。
    void applyDragDelta(const QPointF &deltaWidget);
    /// 拖动开始：记录被拖区域的原始顶点（widget 像素）与被拖顶点下标
    void beginDrag(int regionIndex, int vertexIndex, const QPointF &pressPoint);

    // 模式切换时清进行中的绘制状态（同 FenceOverlay::setDrawMode）
    void clearInProgressDrawing();

    void updateCursor();

    // ---- 绘制 ----
    void drawRegion(QPainter &painter, const PadRegion &region, int index);
    void drawAnchors(QPainter &painter);
    void drawTempRect(QPainter &painter);
    void drawTempPolygon(QPainter &painter);
    /// ruleType 配色（INTRUSION 红 / CROSS 琥珀），与 fence_frame_renderer 同色——
    /// 画布上的围栏与 apply 后出给 Pad 的合成画面必须是同一个颜色语言。
    static QColor ruleColor(const QString &ruleType);

    QVector<PadRegion> regions_;      // 当前编辑的区域（0~1 归一化）
    CanvasMode mode_ = CanvasMode::Edit;
    QString createRule_ = QStringLiteral("INTRUSION");  // 新区域的规则类型
    QImage background_;               // 摄像头当前帧截图

    // 矩形绘制中
    QPointF rectStart_;
    QPointF rectEnd_;
    bool rectDragging_ = false;

    // 多边形绘制中（widget 像素，落区域那一刻才换算成 0~1）
    QVector<QPointF> polyPoints_;

    // 拖动编辑中
    int dragRegion_ = -1;             // 被拖区域下标
    int dragVertex_ = -1;             // 被拖顶点下标；-1 = 拖整体
    QPointF dragPress_;               // 按下点（widget 像素）
    QVector<QPointF> dragOriginPoints_;  // 按下时该区域的顶点（widget 像素）

    QPointF cursorPos_;               // 最近一次鼠标位置（画多边形虚线用）

    QTimer refreshTimer_;              // 可见时每秒请求新背景帧
};

} // namespace geofence
