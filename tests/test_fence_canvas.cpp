// ============================================================================
// test_fence_canvas.cpp — 围栏草稿编辑画布 + 管理页的离屏自检
// ----------------------------------------------------------------------------
// 钉的是这次新增的两块交互逻辑（都不开摄像头、不碰 RKNN）：
//   A. FenceCanvas 的几何：画矩形/多边形（第 3 点自动闭合）、**拖锚点改形状**、
//      拖整体平移、点删区域、撤销顶点。全部经由真的鼠标事件路径提交（合成 QMouseEvent），
//      断言落出来的 PadRegion 归一化坐标（0~1）与预期一致——这正是用户要求的
//      "编辑围栏时要可以拖动锚点编辑"那条能力的可执行判据。
//   B. FenceManageWidget 在 无围栏/草稿/生效/已停用/复制 各状态下构造 + refresh
//      不崩溃（buildUi / loadFenceForChannel / applyEditorState / refreshFramePreview
//      整条装载链都走到）。页面状态机本身在 PadFenceStore，已由 test_pad_http_api
//      钉过，这里只验页面读状态、装画布、读合成画面 PNG 这一段。
//   A2. 背景 letterbox：画面比例不拉伸，归一化换算以 contentRect 为基准。
//   C. 应用/停用后 FenceOverlay 经 fenceChanged（queued）自动重读快照——
//      "点应用后围栏没上视频监控界面"那条修复的可执行判据。
//   F. RECT 出口点序（左上→右上→右下→左下）、刷新按钮同步外部写入、
//      草稿黑底预览、真实帧底图渲染、通道下拉显示「通道N」。
//   G. 合成画面换算基准：非 16:9 底图 letterbox 后围栏相对**内容区**的
//      归一化位置 == 存储值（"示意图与现场围栏坐标不对应"那条修复的判据）。
//   H. 多边形自动闭合（2026-11-17 用户裁定"第 3 点自动闭合"）：第 3 点自动落区域并
//      回 Edit、modeChanged 通知工具栏、右击/Esc/双击取消、Backspace 撤点、
//      管理页「画多边形」钮与提交后「编辑」钮自动高亮。
//   I. 整体平移跟手 + 编辑态空白追加顶点（2026-11-17 用户报"不跟手/贴边/乱跳"与
//      "在其他位置加点，每次加点按照 1-2-3-4-…-1 闭合"）：总位移叠原始顶点、
//      钳边用原始包围盒、拖出再拖回零漂移；空白单击按点序追加顶点、超 slop 不追加、
//      锚点优先、无多边形不追加；触屏口径：右键不删除、操作提示条已移除。
//
// 运行（cwd 任意，库与媒体都建在 QDir::temp() 下的独立目录）：
//   ./build/build_rk3588_linux/test_fence_canvas      rc 0=PASS 1=FAIL
// ============================================================================

#include "fence_canvas.h"
#include "fence_manage_widget.h"
#include "fence_overlay.h"
#include "pad_fence_store.h"
#include "fence_frame_renderer.h"
#include "fence_manager.h"
#include "construction_session.h"
#include "database_manager.h"
#include "ConfigManager.h"
#include "runtime_paths.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPointF>
#include <QPushButton>
#include <QTemporaryDir>
#include <QVector>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

using geofence::CanvasMode;
using geofence::FenceCanvas;
using geofence::PadFence;
using geofence::PadFenceStore;
using geofence::PadFenceVersion;
using geofence::PadRegion;

namespace {

const QString kTask = QStringLiteral("TASK-CANVAS");

void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}

bool near(double a, double b, double eps = 1e-3)
{
    return std::fabs(a - b) <= eps;
}

// 一个区域的外接边界（归一化 0~1）
void bounds(const PadRegion &r, double *minX, double *maxX, double *minY, double *maxY)
{
    *minX = 1.0; *maxX = 0.0; *minY = 1.0; *maxY = 0.0;
    for (const QPointF &p : r.points) {
        *minX = std::min(*minX, p.x());
        *maxX = std::max(*maxX, p.x());
        *minY = std::min(*minY, p.y());
        *maxY = std::max(*maxY, p.y());
    }
}

// ── 合成鼠标事件：让几何经由真的绘制/拖动路径提交 ──────────────────────────
void sendMouse(FenceCanvas *canvas, QEvent::Type type, const QPoint &pos,
               Qt::MouseButton button = Qt::LeftButton)
{
    const Qt::MouseButtons buttons =
        (type == QEvent::MouseButtonRelease) ? Qt::NoButton : button;
    QMouseEvent event(type, QPointF(pos), QPointF(pos), QPointF(pos), button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(canvas, &event);
}

void drag(FenceCanvas *canvas, const QPoint &a, const QPoint &b)
{
    sendMouse(canvas, QEvent::MouseButtonPress, a);
    sendMouse(canvas, QEvent::MouseMove, b);
    sendMouse(canvas, QEvent::MouseButtonRelease, b);
}

void click(FenceCanvas *canvas, const QPoint &p)
{
    sendMouse(canvas, QEvent::MouseButtonPress, p);
    sendMouse(canvas, QEvent::MouseButtonRelease, p);
}

PadRegion makeRect(const QString &id)
{
    PadRegion r;
    r.regionId = id;
    r.shapeType = QStringLiteral("RECT");
    r.ruleType = QStringLiteral("INTRUSION");
    r.points = {QPointF(0.1, 0.1), QPointF(0.4, 0.4), QPointF(0.1, 0.4), QPointF(0.4, 0.1)};
    return r;
}

// ============================================================================
// A. 画布几何
// ============================================================================
void testCanvasGeometry()
{
    FenceCanvas canvas;
    canvas.resize(400, 300);
    canvas.show();
    QApplication::processEvents();

    // ---- 1) 画矩形：拖 (40,30)→(200,150)，落 RECT 四点，边界 0.1..0.5 ----
    canvas.setMode(CanvasMode::RectDraw);
    drag(&canvas, QPoint(40, 30), QPoint(200, 150));
    check(canvas.regions().size() == 1, "1) 画矩形应落一个区域");
    PadRegion r = canvas.regions().at(0);
    check(r.shapeType == QLatin1String("RECT"), "1) 形状应是 RECT");
    check(r.points.size() == 4, "1) RECT 应恰好 4 点");
    double minX, maxX, minY, maxY;
    bounds(r, &minX, &maxX, &minY, &maxY);
    check(near(minX, 0.1) && near(maxX, 0.5) && near(minY, 0.1) && near(maxY, 0.5),
          "1) 矩形边界应换算成 0.1..0.5");
    // 点序 左上→右上→右下→左下（2026-11-17 用户裁定，传 Pad 与落库同序）
    check(near(r.points.at(0).x(), 0.1) && near(r.points.at(0).y(), 0.1)
              && near(r.points.at(1).x(), 0.5) && near(r.points.at(1).y(), 0.1)
              && near(r.points.at(2).x(), 0.5) && near(r.points.at(2).y(), 0.5)
              && near(r.points.at(3).x(), 0.1) && near(r.points.at(3).y(), 0.5),
          "1) RECT 点序应是 左上→右上→右下→左下");

    // ---- 2) 拖锚点：把右下角 (200,150) 拖到 (240,180)，对角固定 ----
    canvas.setMode(CanvasMode::Edit);
    drag(&canvas, QPoint(200, 150), QPoint(240, 180));
    r = canvas.regions().at(0);
    bounds(r, &minX, &maxX, &minY, &maxY);
    check(near(minX, 0.1) && near(minY, 0.1), "2) 拖锚点时对角的左上应不动");
    check(near(maxX, 0.6) && near(maxY, 0.6), "2) 被拖角应移到 0.6/0.6");
    check(r.points.size() == 4, "2) 拖锚点后仍是 4 点 RECT");
    check(near(r.points.at(0).x(), 0.1) && near(r.points.at(0).y(), 0.1)
              && near(r.points.at(1).x(), 0.6) && near(r.points.at(1).y(), 0.1)
              && near(r.points.at(2).x(), 0.6) && near(r.points.at(2).y(), 0.6)
              && near(r.points.at(3).x(), 0.1) && near(r.points.at(3).y(), 0.6),
          "2) 拖锚点后点序仍应是 左上→右上→右下→左下");

    // ---- 3) 拖整体：内部点 (120,90) 平移 (+40,+30) ----
    drag(&canvas, QPoint(120, 90), QPoint(160, 120));
    r = canvas.regions().at(0);
    bounds(r, &minX, &maxX, &minY, &maxY);
    check(near(minX, 0.2) && near(maxX, 0.7) && near(minY, 0.2) && near(maxY, 0.7),
          "3) 整体平移后边界应是 0.2..0.7");

    // ---- 4) 画多边形：第 3 点落地自动闭合，不需要任何闭合操作 ----
    canvas.setMode(CanvasMode::PolyDraw);
    click(&canvas, QPoint(300, 40));
    click(&canvas, QPoint(360, 40));
    check(canvas.regions().size() == 1, "4) 2 点时不应落区域");
    click(&canvas, QPoint(330, 100));
    check(canvas.regions().size() == 2, "4) 第 3 点应自动提交多边形");
    check(canvas.mode() == CanvasMode::Edit, "4) 提交后应自动回 Edit 模式");
    const PadRegion poly = canvas.regions().at(1);
    check(poly.shapeType == QLatin1String("POLYGON"), "4) 第二个应是 POLYGON");
    check(poly.points.size() == 3, "4) 多边形应 3 点");
    check(near(poly.points.at(0).x(), 0.75), "4) 多边形首点 x 应是 300/400=0.75");

    // ---- 5) 删除：点第一个矩形内部，删掉它，留多边形 ----
    canvas.setMode(CanvasMode::DeleteMode);
    click(&canvas, QPoint(100, 80));   // 落在矩形 [80,280]x[60,210] 内
    check(canvas.regions().size() == 1, "5) 删除后应只剩一个区域");
    check(canvas.regions().at(0).shapeType == QLatin1String("POLYGON"),
          "5) 剩下的应是多边形（删的是矩形）");

    // ---- 6) 撤销顶点 + 取消：不足 3 点不落区域 ----
    canvas.setMode(CanvasMode::PolyDraw);
    click(&canvas, QPoint(50, 250));
    click(&canvas, QPoint(90, 250));
    canvas.undoLastPolyPoint();        // 撤到 1 点
    canvas.cancelPolygon();            // 取消，进行中的点全部作废
    check(canvas.regions().size() == 1, "6) 取消后不应落区域");
    check(canvas.mode() == CanvasMode::PolyDraw, "6) 取消后应留在绘制模式");
    click(&canvas, QPoint(50, 250));
    click(&canvas, QPoint(90, 250));
    click(&canvas, QPoint(70, 280));   // 第 3 点自动提交
    check(canvas.regions().size() == 2, "6) 点满 3 点应落区域");

    std::cout << "  [A] 画布几何 6 组断言通过\n";
}

// ============================================================================
// A2. 画面比例：背景 letterbox 后，换算基准必须是 contentRect 而不是 widget
// ============================================================================
void testCanvasAspectRatio()
{
    FenceCanvas canvas;
    canvas.resize(400, 300);
    canvas.show();
    QImage frame(640, 360, QImage::Format_RGB32);   // 16:9 背景
    frame.fill(Qt::black);
    canvas.setBackground(frame);
    QApplication::processEvents();

    // 400x300 里放 16:9 → 内容 400x225，上下黑边各 (300-225)/2
    const QRect content = canvas.contentRect();
    check(content.width() == 400 && content.height() == 225,
          "A2) 16:9 背景在 400x300 里内容区应是 400x225（不拉伸）");
    check(content.top() == (300 - 225) / 2 && content.left() == 0,
          "A2) 内容区应垂直居中");

    // 在内容区里画矩形：y 换算必须扣掉黑边、除以 225 而不是 300
    canvas.setMode(CanvasMode::RectDraw);
    drag(&canvas, QPoint(40, content.top() + 22), QPoint(200, content.top() + 112));
    check(canvas.regions().size() == 1, "A2) 应落一个区域");
    double minX, maxX, minY, maxY;
    bounds(canvas.regions().at(0), &minX, &maxX, &minY, &maxY);
    check(near(minX, 0.1) && near(maxX, 0.5), "A2) x 仍按内容宽换算 0.1..0.5");
    check(near(minY, 22.0 / 225) && near(maxY, 112.0 / 225),
          "A2) y 必须按内容高换算（扣黑边 /225），不能按 widget 高 /300");

    // 黑边上的落笔钳进画面：从左上黑边拖到右下 widget 外 → 落 0..1 满幅矩形
    drag(&canvas, QPoint(-10, 0), QPoint(500, 400));
    check(canvas.regions().size() == 2, "A2) 黑边落笔应钳进内容区而不是作废");
    bounds(canvas.regions().at(1), &minX, &maxX, &minY, &maxY);
    check(near(minX, 0.0) && near(maxX, 1.0) && near(minY, 0.0) && near(maxY, 1.0),
          "A2) 钳位后应是 0..1 满幅，不出归一化范围");

    // 锚点拖动同样以 contentRect 为基准：把满幅矩形右下角拖回内容区中点
    canvas.setMode(CanvasMode::Edit);
    drag(&canvas, QPoint(content.right() + 1, content.bottom() + 1),
         QPoint(200, content.top() + 112));
    bounds(canvas.regions().at(1), &minX, &maxX, &minY, &maxY);
    check(near(minX, 0.0) && near(minY, 0.0), "A2) 拖锚点对角不动");
    check(near(maxX, 0.5) && near(maxY, 112.0 / 225), "A2) 锚点拖动按内容区换算");

    std::cout << "  [A2] 画面比例（letterbox 换算）4 组断言通过\n";
}

// ============================================================================
// B. 管理页在各状态下构造 + refresh 不崩溃
// ============================================================================
void testManageWidgetStates()
{
    PadFenceStore store;
    const QString cameraId = QStringLiteral("CH1");   // 页面下拉默认 CH1

    // 无围栏态：直接构造 + refresh
    {
        FenceManageWidget page;
        page.refresh();
        QApplication::processEvents();
    }

    // 建草稿
    PadFenceStore::DraftInput input;
    input.taskId = kTask;
    input.cameraId = cameraId;
    input.streamId = cameraId;
    input.name = QStringLiteral("自检围栏");
    input.regions = {makeRect(QStringLiteral("RG-TEST0001"))};
    input.plan = QJsonObject{{QStringLiteral("taskId"), kTask}};
    input.owner = QJsonObject{{QStringLiteral("id"), QStringLiteral("CANVAS")},
                              {QStringLiteral("name"), QStringLiteral("自检")}};
    QString fenceId;
    int version = 0;
    QString detail;
    auto outcome = store.saveDraft(input, &fenceId, &version, &detail);
    check(outcome == PadFenceStore::DraftOutcome::Created, "B) 建草稿应 Created");

    // 草稿态
    {
        FenceManageWidget page;
        page.refresh();
        QApplication::processEvents();
    }

    // 生效态：渲染画面 → apply → 写帧路径 → 刷内存（与页面 onApply 同序）
    PadFence fence;
    check(store.loadFence(fenceId, &fence), "B) 应能读回围栏");
    QString absPath, relPath, renderErr;
    check(geofence::renderFenceFrame(fenceId, version, fence.versions.at(0).regions,
                                     RuntimePaths::alarmsDir(), QStringLiteral("fence-frames"),
                                     &absPath, &relPath, &renderErr),
          "B) 合成画面应渲染成功");
    QString appliedAt;
    check(store.applyDraft(fenceId, version, &appliedAt, &detail)
              == PadFenceStore::ApplyOutcome::Applied, "B) apply 应 Applied");
    check(store.setFramePath(fenceId, version, relPath), "B) 帧路径应落库");
    geofence::FenceManager::instance().refreshFromStores();
    {
        FenceManageWidget page;   // 生效态：refreshFramePreview 会读那张 PNG
        page.refresh();
        QApplication::processEvents();
    }

    // 另存为新方案（从 ACTIVE 复制出新草稿）
    input.fenceId = fenceId;
    input.sourceVersion = version;
    input.hasSourceVersion = true;
    int copiedVersion = 0;
    outcome = store.saveDraft(input, &fenceId, &copiedVersion, &detail);
    check(outcome == PadFenceStore::DraftOutcome::Copied, "B) 复制应 Copied");
    check(copiedVersion == version + 1, "B) 新草稿版本号应是 max+1");
    {
        FenceManageWidget page;   // 草稿态（复制出来的）
        page.refresh();
        QApplication::processEvents();
    }

    // 停用 ACTIVE
    QString disabledAt;
    check(store.disableVersion(fenceId, version, &disabledAt, &detail)
              == PadFenceStore::DisableOutcome::Disabled, "B) 停用应 Disabled");
    geofence::FenceManager::instance().refreshFromStores();
    {
        FenceManageWidget page;   // 此时有 DISABLED + DRAFT，页面优先编辑草稿
        page.refresh();
        QApplication::processEvents();
    }

    // 删草稿
    check(store.deleteDraft(fenceId, copiedVersion, &detail)
              == PadFenceStore::DeleteOutcome::Deleted, "B) 删草稿应 Deleted");
    {
        FenceManageWidget page;   // 只剩 DISABLED
        page.refresh();
        QApplication::processEvents();
    }

    std::cout << "  [B] 管理页 6 种状态构造+refresh 通过\n";
}

// ============================================================================
// C. 应用/停用后，视频页的 FenceOverlay 自动跟着库刷新（问题 4 的可执行判据）
// ============================================================================
void testOverlayAutoRefresh()
{
    // 先放一块 CH2（通道 1）的空 overlay，再走真的 apply 链：
    // refreshFromStores 持锁发 fenceChanged，overlay 以 QueuedConnection 订阅，
    // processEvents 后快照必须已经换掉——不经过任何手动 loadFences。
    geofence::FenceOverlay overlay;
    overlay.setChannel(1);
    check(overlay.channelFence().shapes.empty(), "C) 初始应无围栏");

    PadFenceStore store;
    PadFenceStore::DraftInput input;
    input.taskId = kTask;
    input.cameraId = QStringLiteral("CH2");
    input.streamId = input.cameraId;
    input.name = QStringLiteral("同步自检");
    input.regions = {makeRect(QStringLiteral("RG-TEST0002"))};
    input.plan = QJsonObject{{QStringLiteral("taskId"), kTask}};
    input.owner = QJsonObject{{QStringLiteral("id"), QStringLiteral("CANVAS")},
                              {QStringLiteral("name"), QStringLiteral("自检")}};
    QString fenceId, detail;
    int version = 0;
    check(store.saveDraft(input, &fenceId, &version, &detail)
              == PadFenceStore::DraftOutcome::Created, "C) 建草稿应 Created");

    PadFence fence;
    check(store.loadFence(fenceId, &fence), "C) 应能读回围栏");
    QString absPath, relPath, renderErr;
    check(geofence::renderFenceFrame(fenceId, version, fence.versions.at(0).regions,
                                     RuntimePaths::alarmsDir(), QStringLiteral("fence-frames"),
                                     &absPath, &relPath, &renderErr), "C) 渲染应成功");
    QString appliedAt;
    check(store.applyDraft(fenceId, version, &appliedAt, &detail)
              == PadFenceStore::ApplyOutcome::Applied, "C) apply 应 Applied");
    check(store.setFramePath(fenceId, version, relPath), "C) 帧路径应落库");
    geofence::FenceManager::instance().refreshFromStores();
    QApplication::processEvents();   // queued 的 fenceChanged 在这里才进槽
    check(overlay.channelFence().shapes.size() == 1,
          "C) 应用后 overlay 应自动显示围栏（无需手动 loadFences）");

    QString disabledAt;
    check(store.disableVersion(fenceId, version, &disabledAt, &detail)
              == PadFenceStore::DisableOutcome::Disabled, "C) 停用应 Disabled");
    geofence::FenceManager::instance().refreshFromStores();
    QApplication::processEvents();
    check(overlay.channelFence().shapes.empty(), "C) 停用后 overlay 应自动清空");

    std::cout << "  [C] 应用/停用 → 视频页 overlay 自动同步通过\n";
}

// ============================================================================
// D. 「新建围栏」按钮：空画布也能建出草稿（§5 的 1–8 区域约束下的创建路径）
// ============================================================================
void testNewFenceButton()
{
    FenceManageWidget page;
    page.refresh();
    auto *combo = page.findChild<QComboBox *>();
    check(combo, "D) 页面应有通道下拉");
    combo->setCurrentIndex(3);   // CH4：前面几组都没碰过，必无围栏
    QApplication::processEvents();

    QPushButton *newBtn = nullptr;
    for (QPushButton *btn : page.findChildren<QPushButton *>()) {
        if (btn->text() == QStringLiteral("新建围栏"))
            newBtn = btn;
    }
    check(newBtn && newBtn->isEnabled(), "D) 无围栏通道上「新建围栏」应可用");
    newBtn->click();
    QApplication::processEvents();

    PadFenceStore store;
    bool found = false;
    for (const PadFence &fence : store.fencesForTask(kTask)) {
        if (fence.cameraId != QLatin1String("CH4"))
            continue;
        found = true;
        check(fence.draftVersion() == 1, "D) 新建应是草稿 v1");
        PadFenceVersion version;
        check(fence.findVersion(1, &version), "D) 应能读到 v1");
        check(version.regions.size() == 1, "D) 空画布新建应自带一个默认区域（§5 最少 1 个）");
        check(!version.regions.at(0).regionId.isEmpty(), "D) 默认区域应带 regionId");
    }
    check(found, "D) 点「新建围栏」应真的建出 CH4 的草稿");
    std::cout << "  [D] 新建围栏按钮（空画布 → 默认区域草稿）通过\n";
}

// ============================================================================
// E. 登录前建页 → 登录后 refresh：旧"先扫码登录"提示必须被盖掉；
//    只剩停用版本的围栏要显示最新版本而不是"无版本"（真机实测踩过的两条）
// ============================================================================
void testStaleLoginHint()
{
    // CH3 造一条"只剩 DISABLED"的围栏：草稿 → 应用 → 停用
    PadFenceStore store;
    PadFenceStore::DraftInput input;
    input.taskId = kTask;
    input.cameraId = QStringLiteral("CH3");
    input.streamId = input.cameraId;
    input.name = QStringLiteral("登录提示自检");
    input.regions = {makeRect(QStringLiteral("RG-TEST0003"))};
    input.plan = QJsonObject{{QStringLiteral("taskId"), kTask}};
    input.owner = QJsonObject{{QStringLiteral("id"), QStringLiteral("CANVAS")},
                              {QStringLiteral("name"), QStringLiteral("自检")}};
    QString fenceId, detail;
    int version = 0;
    check(store.saveDraft(input, &fenceId, &version, &detail)
              == PadFenceStore::DraftOutcome::Created, "E) 建草稿应 Created");
    PadFence fence;
    check(store.loadFence(fenceId, &fence), "E) 应能读回围栏");
    QString absPath, relPath, renderErr;
    check(geofence::renderFenceFrame(fenceId, version, fence.versions.at(0).regions,
                                     RuntimePaths::alarmsDir(), QStringLiteral("fence-frames"),
                                     &absPath, &relPath, &renderErr), "E) 渲染应成功");
    QString appliedAt, disabledAt;
    check(store.applyDraft(fenceId, version, &appliedAt, &detail)
              == PadFenceStore::ApplyOutcome::Applied, "E) apply 应 Applied");
    check(store.setFramePath(fenceId, version, relPath), "E) 帧路径应落库");
    check(store.disableVersion(fenceId, version, &disabledAt, &detail)
              == PadFenceStore::DisableOutcome::Disabled, "E) 停用应 Disabled");

    // 真机顺序：页面在登录前构造（frmMain 构造期），此刻提示是"先扫码登录"
    session::ConstructionSession::instance().logout();
    FenceManageWidget page;
    bool sawHint = false;
    for (QLabel *label : page.findChildren<QLabel *>())
        if (label->text() == QStringLiteral("先扫码登录，再管理围栏"))
            sawHint = true;
    check(sawHint, "E) 登录前构造应显示扫码提示");

    // 重新登录 + refresh（frmMain::onLoginSucceeded 的同款顺序）
    session::ConstructionInfo info;
    info.personId = "CANVAS";
    info.personName = "自检";
    info.taskId = kTask;
    info.endtime = "23:00";
    info.totalPersonCount = 1;
    check(session::ConstructionSession::instance().startLogin(info), "E) 重新登录应成功");
    page.refresh();
    QApplication::processEvents();

    bool staleHint = false, showsDisabled = false;
    for (QLabel *label : page.findChildren<QLabel *>()) {
        if (label->text() == QStringLiteral("先扫码登录，再管理围栏"))
            staleHint = true;
        if (label->text() == QStringLiteral("已停用 v1"))
            showsDisabled = true;
    }
    check(!staleHint, "E) 登录后旧扫码提示必须被盖掉");
    check(showsDisabled, "E) 只剩停用版本时应显示最新版本（而不是无版本）");

    QPushButton *copyBtn = nullptr;
    for (QPushButton *btn : page.findChildren<QPushButton *>())
        if (btn->text() == QStringLiteral("另存为新方案"))
            copyBtn = btn;
    check(copyBtn && copyBtn->isEnabled(), "E) 停用版本上「另存为新方案」应可用");

    std::cout << "  [E] 登录提示刷新 + 仅停用版本装载通过\n";
}

// ============================================================================
// F. 2026-11-17 一轮：RECT 出口点序 / 刷新按钮 / 草稿黑底预览 / 真实帧底图 / 通道显示
// ============================================================================
void testRectOrderAndRefresh()
{
    // ---- 1) toJson 出口归一：交叉序旧数据出来也是 左上→右上→右下→左下 ----
    PadRegion bowtie = makeRect(QStringLiteral("RG-ORDER001"));   // 故意交叉序
    const QJsonArray pts = bowtie.toJson().value(QStringLiteral("points")).toArray();
    check(pts.size() == 4, "F) RECT 出口应 4 点");
    const QPointF p0(pts.at(0).toArray().at(0).toDouble(), pts.at(0).toArray().at(1).toDouble());
    const QPointF p1(pts.at(1).toArray().at(0).toDouble(), pts.at(1).toArray().at(1).toDouble());
    const QPointF p2(pts.at(2).toArray().at(0).toDouble(), pts.at(2).toArray().at(1).toDouble());
    const QPointF p3(pts.at(3).toArray().at(0).toDouble(), pts.at(3).toArray().at(1).toDouble());
    check(near(p0.x(), 0.1) && near(p0.y(), 0.1)
              && near(p1.x(), 0.4) && near(p1.y(), 0.1)
              && near(p2.x(), 0.4) && near(p2.y(), 0.4)
              && near(p3.x(), 0.1) && near(p3.y(), 0.4),
          "F) toJson 应把 RECT 归一成 左上→右上→右下→左下");

    // ---- 2) 渲染底图两档：空图=深底网格，传帧=真实画面铺底 ----
    const QImage dark = geofence::renderFenceImage({});
    check(dark.pixelColor(10, 10) == QColor(16, 20, 28), "F) 无底图应是深色底");
    QImage redFrame(640, 360, QImage::Format_RGB32);
    redFrame.fill(Qt::red);
    const QImage framed = geofence::renderFenceImage({}, redFrame);
    check(framed.pixelColor(640, 360) == QColor(Qt::red), "F) 传帧时画面中心应是真实帧内容");

    // ---- 3) 帧提供者：注册后 frameForCamera 按 cameraId 取帧 ----
    geofence::setFrameProvider([](const QString &cameraId) -> QImage {
        if (cameraId != QLatin1String("CH1"))
            return {};
        QImage img(320, 180, QImage::Format_RGB32);
        img.fill(Qt::blue);
        return img;
    });
    check(!geofence::frameForCamera(QStringLiteral("CH1")).isNull(), "F) 提供者应能按 CH1 取帧");
    check(geofence::frameForCamera(QStringLiteral("CH2")).isNull(), "F) 其它通道无帧应返回空图");
    geofence::setFrameProvider(nullptr);

    // ---- 4) 通道下拉显示「通道N」，协议身份仍是 CHN ----
    FenceManageWidget page;
    page.refresh();
    auto *combo = page.findChild<QComboBox *>();
    check(combo && combo->itemText(0) == QStringLiteral("通道1")
              && combo->itemText(3) == QStringLiteral("通道4"),
          "F) 下拉显示应是 通道1..通道4");

    // ---- 5) 刷新按钮：外部（模拟 HTTP 下发）写入新草稿后，点刷新即同步 ----
    // CH1 在 B 组后只剩 DISABLED v1；页面先装载它，再从库外复制出一条新草稿。
    PadFenceStore store;
    QString fenceId;
    for (const PadFence &fence : store.fencesForTask(kTask))
        if (fence.cameraId == QLatin1String("CH1"))
            fenceId = fence.fenceId;
    check(!fenceId.isEmpty(), "F) CH1 应有 B 组留下的围栏");
    bool showsDisabled = false;
    for (QLabel *label : page.findChildren<QLabel *>())
        if (label->text() == QStringLiteral("已停用 v1"))
            showsDisabled = true;
    check(showsDisabled, "F) 刷新前应显示 已停用 v1");

    PadFenceStore::DraftInput input;
    input.fenceId = fenceId;
    input.taskId = kTask;
    input.cameraId = QStringLiteral("CH1");
    input.streamId = input.cameraId;
    input.name = QStringLiteral("HTTP 下发模拟");
    input.regions = {makeRect(QStringLiteral("RG-ORDER002"))};
    input.plan = QJsonObject{{QStringLiteral("taskId"), kTask}};
    input.owner = QJsonObject{{QStringLiteral("id"), QStringLiteral("CANVAS")},
                              {QStringLiteral("name"), QStringLiteral("自检")}};
    input.sourceVersion = 1;
    input.hasSourceVersion = true;
    QString detail;
    int newVersion = 0;
    check(store.saveDraft(input, &fenceId, &newVersion, &detail)
              == PadFenceStore::DraftOutcome::Copied,
          "F) 模拟 HTTP 复制下发应 Copied");

    QPushButton *refreshBtn = nullptr;
    for (QPushButton *btn : page.findChildren<QPushButton *>())
        if (btn->text() == QStringLiteral("刷新"))
            refreshBtn = btn;
    check(refreshBtn, "F) 页面应有「刷新」按钮");
    refreshBtn->click();
    QApplication::processEvents();

    bool showsDraft = false;
    for (QLabel *label : page.findChildren<QLabel *>())
        if (label->text() == QStringLiteral("草稿 v%1").arg(newVersion))
            showsDraft = true;
    check(showsDraft, "F) 点刷新后应同步出外部写入的新草稿");

    // ---- 6) 草稿预览：黑底区域图（pixmap 非空），不再是"草稿无画面"占位 ----
    bool draftPreview = false;
    for (QLabel *label : page.findChildren<QLabel *>()) {
        if (label->toolTip() == QStringLiteral("草稿预览（黑底，无生效画面）")
            && label->pixmap() && !label->pixmap()->isNull())
            draftPreview = true;
    }
    check(draftPreview, "F) 草稿应显示黑底区域预览图");

    std::cout << "  [F] RECT 点序/刷新按钮/草稿黑底预览/真实帧底图/通道显示 6 组断言通过\n";
}

// ============================================================================
// G. 合成画面换算基准：围栏相对**底图内容区**的位置 == 存储归一化值
//    "示意图与现场围栏坐标不对应"那条修复的可执行判据：非 16:9 底图 letterbox
//    留黑边后，区域必须按内容区换算（旧实现乘整幅画布，4:3 底图上横向拉宽
//    1.33 倍）。RECT/POLYGON 同判；无底图档内容区=满幅，口径不回退。
//    量法：扫边框纯色像素得围栏外接框，扫底图纯色得内容区，两者相除。
// ============================================================================
void testRenderContentBasis()
{
    const QColor outline(0xE5, 0x39, 0x35);   // INTRUSION 边框色（渲染器 ruleColor）
    const QColor baseFill(Qt::blue);          // 底图纯色，便于扫内容区

    auto boundsOf = [](const QImage &img, const QColor &color) {
        int x0 = img.width(), x1 = -1, y0 = img.height(), y1 = -1;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
                if (img.pixelColor(x, y) == color) {
                    x0 = qMin(x0, x); x1 = qMax(x1, x);
                    y0 = qMin(y0, y); y1 = qMax(y1, y);
                }
        return QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    };
    // 围栏外接框相对内容区的归一化边界（边框宽 3px，误差 < 2/内容边长）
    auto relBounds = [&](const QImage &img, const QRect &content,
                         double *x0, double *x1, double *y0, double *y1) {
        const QRect f = boundsOf(img, outline);
        *x0 = double(f.left() - content.left()) / content.width();
        *x1 = double(f.right() - content.left()) / content.width();
        *y0 = double(f.top() - content.top()) / content.height();
        *y1 = double(f.bottom() - content.top()) / content.height();
    };

    PadRegion rect;
    rect.regionId = QStringLiteral("RG-TEST000G");
    rect.shapeType = QStringLiteral("RECT");
    rect.ruleType = QStringLiteral("INTRUSION");
    rect.points = {QPointF(0.25, 0.25), QPointF(0.75, 0.25),
                   QPointF(0.75, 0.75), QPointF(0.25, 0.75)};
    PadRegion poly = rect;
    poly.shapeType = QStringLiteral("POLYGON");
    poly.points = {QPointF(0.2, 0.2), QPointF(0.6, 0.2), QPointF(0.4, 0.6)};

    double x0, x1, y0, y1;

    // ---- 1) 4:3 底图：内容区 (160,0,960,720)，RECT 相对内容区仍 0.25..0.75 ----
    QImage bg43(640, 480, QImage::Format_RGB32);
    bg43.fill(baseFill);
    QImage img = geofence::renderFenceImage({rect}, bg43);
    const QRect content = boundsOf(img, baseFill);
    check(content == QRect(160, 0, 960, 720), "G) 4:3 底图应 letterbox 成 (160,0,960,720)");
    relBounds(img, content, &x0, &x1, &y0, &y1);
    check(near(x0, 0.25, 1e-2) && near(x1, 0.75, 1e-2)
              && near(y0, 0.25, 1e-2) && near(y1, 0.75, 1e-2),
          "G) 4:3 底图上 RECT 相对内容区应是 0.25..0.75（旧实现为 0.166..0.833）");

    // ---- 2) 同底图 POLYGON：相对内容区 0.2..0.6 / 0.2..0.6 ----
    img = geofence::renderFenceImage({poly}, bg43);
    relBounds(img, content, &x0, &x1, &y0, &y1);
    check(near(x0, 0.2, 1e-2) && near(x1, 0.6, 1e-2)
              && near(y0, 0.2, 1e-2) && near(y1, 0.6, 1e-2),
          "G) 4:3 底图上 POLYGON 相对内容区应是 0.2..0.6");

    // ---- 3) 16:9 底图：满幅内容区，RECT 口径不变 ----
    QImage bg169(1280, 720, QImage::Format_RGB32);
    bg169.fill(baseFill);
    img = geofence::renderFenceImage({rect}, bg169);
    relBounds(img, boundsOf(img, baseFill), &x0, &x1, &y0, &y1);
    check(near(x0, 0.25, 1e-2) && near(x1, 0.75, 1e-2)
              && near(y0, 0.25, 1e-2) && near(y1, 0.75, 1e-2),
          "G) 16:9 底图上 RECT 相对内容区应是 0.25..0.75");

    // ---- 4) 无底图（草稿预览档）：内容区=满幅，相对满幅 0.25..0.75 ----
    img = geofence::renderFenceImage({rect});
    relBounds(img, QRect(0, 0, img.width(), img.height()), &x0, &x1, &y0, &y1);
    check(near(x0, 0.25, 1e-2) && near(x1, 0.75, 1e-2)
              && near(y0, 0.25, 1e-2) && near(y1, 0.75, 1e-2),
          "G) 无底图档 RECT 相对满幅应是 0.25..0.75");

    std::cout << "  [G] 合成画面换算基准（4:3/16:9/无底图 × RECT/POLYGON）4 组断言通过\n";
}

// ============================================================================
// H. 多边形自动闭合（2026-11-17 用户裁定"第 3 点自动闭合"）：
//    第 3 点自动提交并回 Edit、modeChanged 通知、右击/Esc/双击取消、
//    Backspace 撤点、管理页工具钮联动。取消/撤点是否真把点清掉，用
//    "随后 3 次点击落出的区域首点 == 新首点"来证明（旧点若残留会顶掉新点）。
// ============================================================================
void testPolygonAutoClose()
{
    FenceCanvas canvas;
    canvas.resize(400, 300);
    canvas.show();
    QApplication::processEvents();

    auto sendKey = [&canvas](int key) {
        QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    };

    // ---- 1) 第 3 点即提交 + 自动回 Edit + modeChanged 信号 + 规则类型 ----
    CanvasMode seen = CanvasMode::RectDraw;
    QObject::connect(&canvas, &FenceCanvas::modeChanged,
                     [&seen](CanvasMode mode) { seen = mode; });
    canvas.setCreateRule(QStringLiteral("CROSS"));
    canvas.setMode(CanvasMode::PolyDraw);
    click(&canvas, QPoint(40, 80));
    click(&canvas, QPoint(120, 80));
    check(canvas.regions().isEmpty(), "H1) 2 点不应提交");

    // 顶点序号是画出来的：渲染成图，点旁小圆盘内应有近白数字笔画。
    // 抗锯齿下文字像素不是精确 255（实测最亮 249），用 min 通道阈值判；
    // 绘制点放在 y=80，序号盘偏移 (11,-11) 落在 y=69，扫窗里只有数字笔画。
    auto hasNumberGlyph = [](const QImage &shot, const QPointF &labelCenter) {
        const QRect box(QPoint(int(labelCenter.x()) - 5, int(labelCenter.y()) - 5),
                        QSize(11, 11));
        for (int y = box.top(); y <= box.bottom(); ++y)
            for (int x = box.left(); x <= box.right(); ++x) {
                const QColor px = shot.pixelColor(x, y);
                if (qMin(qMin(px.red(), px.green()), px.blue()) >= 200)
                    return true;
            }
        return false;
    };
    QImage shot(canvas.size(), QImage::Format_RGB32);
    shot.fill(Qt::black);
    canvas.render(&shot);
    check(hasNumberGlyph(shot, QPointF(40, 80) + QPointF(11, -11)),
          "H1) 绘制中的第 1 点应带序号");
    check(hasNumberGlyph(shot, QPointF(120, 80) + QPointF(11, -11)),
          "H1) 绘制中的第 2 点应带序号");

    click(&canvas, QPoint(80, 160));
    check(canvas.regions().size() == 1, "H1) 第 3 点应自动提交");
    check(canvas.mode() == CanvasMode::Edit, "H1) 提交后应自动回 Edit");
    check(seen == CanvasMode::Edit, "H1) modeChanged 应通知到 Edit（工具栏高亮靠它）");
    const PadRegion tri = canvas.regions().at(0);
    check(tri.shapeType == QLatin1String("POLYGON") && tri.points.size() == 3,
          "H1) 提交的应是 3 点 POLYGON");
    check(tri.ruleType == QLatin1String("CROSS"), "H1) 规则类型应跟随当前选择");
    check(near(tri.points.at(0).x(), 0.1) && near(tri.points.at(0).y(), 80.0 / 300)
              && near(tri.points.at(2).x(), 0.2) && near(tri.points.at(2).y(), 160.0 / 300),
          "H1) 顶点应按点击序换算成 0~1");
    shot.fill(Qt::black);
    canvas.render(&shot);
    check(hasNumberGlyph(shot, QPointF(40, 80) + QPointF(10, -10)),
          "H1) Edit 模式锚点应带顶点序号");

    // ---- 2) 取消（触屏走工具栏「取消绘制」= cancelPolygon）：残留点作废 ----
    canvas.setMode(CanvasMode::PolyDraw);
    click(&canvas, QPoint(200, 200));
    click(&canvas, QPoint(240, 200));
    canvas.cancelPolygon();
    click(&canvas, QPoint(40, 200));
    click(&canvas, QPoint(80, 200));
    click(&canvas, QPoint(60, 260));
    check(canvas.regions().size() == 2, "H2) 取消后重新点满 3 点应只落 1 个新区域");
    check(near(canvas.regions().at(1).points.at(0).x(), 0.1)
              && near(canvas.regions().at(1).points.at(0).y(), 200.0 / 300),
          "H2) 新区域首点应是取消后的新首点（旧点没残留）");

    // ---- 3) Esc 取消 ----
    canvas.setMode(CanvasMode::PolyDraw);
    click(&canvas, QPoint(200, 40));
    click(&canvas, QPoint(240, 40));
    sendKey(Qt::Key_Escape);
    click(&canvas, QPoint(40, 40));
    click(&canvas, QPoint(80, 40));
    click(&canvas, QPoint(60, 100));
    check(canvas.regions().size() == 3, "H3) Esc 取消后重新点满 3 点应只落 1 个新区域");
    check(near(canvas.regions().at(2).points.at(0).x(), 0.1)
              && near(canvas.regions().at(2).points.at(0).y(), 40.0 / 300),
          "H3) 新区域首点应是 Esc 后的新首点");

    // ---- 4) Backspace 撤点：撤掉第 2 点后补 2 点，第 2 顶点应是补的第 1 点 ----
    canvas.setMode(CanvasMode::PolyDraw);
    click(&canvas, QPoint(100, 100));
    click(&canvas, QPoint(140, 100));
    sendKey(Qt::Key_Backspace);        // 撤掉 (140,100)，剩 (100,100)
    click(&canvas, QPoint(180, 100));
    click(&canvas, QPoint(140, 180));  // 第 3 点提交
    check(canvas.regions().size() == 4, "H4) 撤点后补满 3 点应落区域");
    check(near(canvas.regions().at(3).points.at(1).x(), 0.45),
          "H4) 第 2 顶点应是 180/400=0.45（Backspace 真撤掉了 140 那个点）");

    // ---- 5) 双击 = 取消重画，不提交带重复顶点的形状 ----
    canvas.setMode(CanvasMode::PolyDraw);
    click(&canvas, QPoint(300, 240));
    click(&canvas, QPoint(340, 240));
    sendMouse(&canvas, QEvent::MouseButtonDblClick, QPoint(340, 240));
    click(&canvas, QPoint(40, 240));
    click(&canvas, QPoint(80, 240));
    click(&canvas, QPoint(60, 280));
    check(canvas.regions().size() == 5, "H5) 双击取消后重新点满 3 点应只落 1 个新区域");
    check(near(canvas.regions().at(4).points.at(0).x(), 0.1)
              && near(canvas.regions().at(4).points.at(0).y(), 240.0 / 300),
          "H5) 新区域首点应是双击后的新首点");

    // ---- 6) 管理页联动：「画多边形」钮在、「闭合多边形」钮没了；
    //         画布提交后自动回 Edit，「编辑」钮跟着高亮 ----
    FenceManageWidget page;
    page.resize(1200, 800);
    page.show();
    page.refresh();                    // CH1 有 F 组留下的草稿，画布可编辑
    QApplication::processEvents();
    auto *pageCanvas = page.findChild<FenceCanvas *>();
    check(pageCanvas && pageCanvas->isEnabled(), "H6) CH1 草稿态画布应可编辑");
    QPushButton *polyBtn = nullptr;
    QPushButton *editBtn = nullptr;
    QPushButton *closeBtn = nullptr;
    QPushButton *cancelBtn = nullptr;
    for (QPushButton *btn : page.findChildren<QPushButton *>()) {
        if (btn->text() == QStringLiteral("画多边形")) polyBtn = btn;
        if (btn->text() == QStringLiteral("编辑")) editBtn = btn;
        if (btn->text() == QStringLiteral("闭合多边形")) closeBtn = btn;
        if (btn->text() == QStringLiteral("取消绘制")) cancelBtn = btn;
    }
    check(polyBtn && editBtn, "H6) 工具栏应有「画多边形」与「编辑」");
    check(!closeBtn, "H6) 「闭合多边形」钮应已移除（第 3 点即提交，无需闭合）");
    check(cancelBtn, "H6) 触屏取消入口「取消绘制」钮应在工具栏");
    polyBtn->click();
    QApplication::processEvents();
    check(pageCanvas->mode() == CanvasMode::PolyDraw, "H6) 点「画多边形」应进 PolyDraw");
    check(cancelBtn->isEnabled(), "H6) PolyDraw 态「取消绘制」应可用");
    const int before = pageCanvas->regions().size();
    click(pageCanvas, QPoint(60, 60));
    click(pageCanvas, QPoint(160, 60));
    click(pageCanvas, QPoint(110, 160));
    QApplication::processEvents();
    check(pageCanvas->regions().size() == before + 1, "H6) 画布上第 3 点应提交新区域");
    check(pageCanvas->mode() == CanvasMode::Edit, "H6) 提交后画布应回 Edit");
    check(editBtn->isChecked() && !polyBtn->isChecked(),
          "H6) 提交后「编辑」钮应自动高亮、「画多边形」取消高亮");
    check(!cancelBtn->isEnabled(), "H6) 回 Edit 后「取消绘制」应禁用（没有进行中的绘制）");

    std::cout << "  [H] 多边形自动闭合（取消/撤点/工具栏联动）6 组断言通过\n";
}

// ============================================================================
// I. 整体平移跟手 + 编辑态空白追加顶点（2026-11-17 用户报的两条）：
//    1) 平移是"原始顶点 + 从按下起的总位移"，逐步跟手、钳边不收缩、拖回零漂移；
//    2) Edit 空白处单击 = 给最后一个多边形按点序追加顶点（1-2-…-n-1 闭合），
//       超过 slop 的拖动不追加、锚点优先、只有 RECT 时不追加；
//    3) 触屏口径：右键不删除、画布不叠操作提示条。
// ============================================================================
void testDragFollowAndEdgeInsert()
{
    FenceCanvas canvas;
    canvas.resize(400, 300);
    canvas.show();
    QApplication::processEvents();

    int edited = 0;
    QObject::connect(&canvas, &FenceCanvas::regionsEdited, [&edited]() { ++edited; });

    double minX, maxX, minY, maxY;

    // ---- 1) 逐步跟手：总位移叠在原始顶点上，不是累加到已移动的点上 ----
    canvas.setRegions({makeRect(QStringLiteral("RG-DRAG0001"))});   // 0.1..0.4
    canvas.setMode(CanvasMode::Edit);
    sendMouse(&canvas, QEvent::MouseButtonPress, QPoint(100, 75));   // 矩形内部
    sendMouse(&canvas, QEvent::MouseMove, QPoint(110, 80));          // +10,+5
    bounds(canvas.regions().at(0), &minX, &maxX, &minY, &maxY);
    check(near(minX, 0.125) && near(minY, 0.1 + 5.0 / 300),
          "I1) 拖动中途应跟手到 原始+当前总位移（0.125/0.1167）");
    sendMouse(&canvas, QEvent::MouseMove, QPoint(130, 95));
    sendMouse(&canvas, QEvent::MouseMove, QPoint(140, 105));         // 总 +40,+30
    sendMouse(&canvas, QEvent::MouseButtonRelease, QPoint(140, 105));
    bounds(canvas.regions().at(0), &minX, &maxX, &minY, &maxY);
    check(near(minX, 0.2) && near(maxX, 0.5) && near(minY, 0.2) && near(maxY, 0.5),
          "I1) 多步拖动后应是 原始+总位移 0.2..0.5（累加 bug 会越拖越远）");
    check(edited == 1, "I1) 一次拖动只发一次 regionsEdited");

    // ---- 2) 钳边：拖出画布外，边界停在 0 且**尺寸不收缩** ----
    sendMouse(&canvas, QEvent::MouseButtonPress, QPoint(140, 105));
    sendMouse(&canvas, QEvent::MouseMove, QPoint(-500, -400));
    sendMouse(&canvas, QEvent::MouseButtonRelease, QPoint(-500, -400));
    bounds(canvas.regions().at(0), &minX, &maxX, &minY, &maxY);
    check(near(minX, 0.0) && near(minY, 0.0) && near(maxX, 0.3) && near(maxY, 0.3),
          "I2) 拖出界应钳到 0..0.3（原始包围盒钳制，尺寸保持 0.3 不贴边收缩）");

    // ---- 3) 拖出再拖回按下点：零漂移（旧实现累加后回不到原位） ----
    sendMouse(&canvas, QEvent::MouseButtonPress, QPoint(60, 45));
    sendMouse(&canvas, QEvent::MouseMove, QPoint(300, 200));
    sendMouse(&canvas, QEvent::MouseMove, QPoint(60, 45));
    sendMouse(&canvas, QEvent::MouseButtonRelease, QPoint(60, 45));
    bounds(canvas.regions().at(0), &minX, &maxX, &minY, &maxY);
    check(near(minX, 0.0) && near(maxX, 0.3) && near(minY, 0.0) && near(maxY, 0.3),
          "I3) 拖出再拖回应回到原位 0..0.3（无累积漂移）");

    // ---- 4) 空白处单击追加顶点：追加在末尾，前 3 点不动（点序 1-2-3-4）----
    PadRegion tri;
    tri.regionId = QStringLiteral("RG-EDGE0001");
    tri.shapeType = QStringLiteral("POLYGON");
    tri.ruleType = QStringLiteral("INTRUSION");
    tri.points = {QPointF(0.2, 0.2), QPointF(0.6, 0.2), QPointF(0.4, 0.6)};
    canvas.setRegions({tri});
    const int before = edited;
    click(&canvas, QPoint(300, 250));   // 三角形外的空白处
    check(canvas.regions().at(0).points.size() == 4, "I4) 空白单击应追加 1 个顶点（3→4）");
    const PadRegion quad = canvas.regions().at(0);
    check(near(quad.points.at(0).x(), 0.2) && near(quad.points.at(0).y(), 0.2)
              && near(quad.points.at(1).x(), 0.6) && near(quad.points.at(1).y(), 0.2)
              && near(quad.points.at(2).x(), 0.4) && near(quad.points.at(2).y(), 0.6)
              && near(quad.points.at(3).x(), 0.75) && near(quad.points.at(3).y(), 250.0 / 300),
          "I4) 新顶点应追加为第 4 点 (0.75,0.833)，原 3 点顺序不动");
    check(edited == before + 1, "I4) 加点完成发一次 regionsEdited");

    // ---- 5) 再点空白再追加：1-2-3-4-5 顺序闭合（5→1 收口由闭合绘制保证）----
    click(&canvas, QPoint(350, 100));
    check(canvas.regions().at(0).points.size() == 5, "I5) 第二次空白单击应再追加 1 点（4→5）");
    const PadRegion pent = canvas.regions().at(0);
    check(near(pent.points.at(3).x(), 0.75) && near(pent.points.at(3).y(), 250.0 / 300)
              && near(pent.points.at(4).x(), 0.875) && near(pent.points.at(4).y(), 100.0 / 300)
              && near(pent.points.at(0).x(), 0.2) && near(pent.points.at(0).y(), 0.2),
          "I5) 点序应是 1-2-3-4-5 依次追加，首点不动（按点序自动闭合）");

    // ---- 6) 空白按下但拖走（超 slop）= 不是点击：不追加顶点 ----
    sendMouse(&canvas, QEvent::MouseButtonPress, QPoint(60, 250));
    sendMouse(&canvas, QEvent::MouseMove, QPoint(120, 260));
    sendMouse(&canvas, QEvent::MouseButtonRelease, QPoint(120, 260));
    check(canvas.regions().at(0).points.size() == 5, "I6) 超 slop 的空白手势不应追加顶点");

    // ---- 7) 锚点优先：点在顶点上是拖锚点，不是追加 ----
    sendMouse(&canvas, QEvent::MouseButtonPress, QPoint(80, 60));   // 首点
    sendMouse(&canvas, QEvent::MouseMove, QPoint(90, 70));
    sendMouse(&canvas, QEvent::MouseButtonRelease, QPoint(90, 70));
    check(canvas.regions().at(0).points.size() == 5, "I7) 拖锚点不应改变顶点数");
    check(near(canvas.regions().at(0).points.at(0).x(), 0.225)
              && near(canvas.regions().at(0).points.at(0).y(), 70.0 / 300),
          "I7) 首点应被拖到 (0.225,0.2333)");

    // ---- 8) 只有 RECT 时空白单击不追加：矩形加第 5 点会破坏矩形语义 ----
    canvas.setRegions({makeRect(QStringLiteral("RG-EDGE0002"))});
    click(&canvas, QPoint(300, 250));
    check(canvas.regions().at(0).points.size() == 4, "I8) 无多边形时空白单击不应落点");
    bounds(canvas.regions().at(0), &minX, &maxX, &minY, &maxY);
    check(near(minX, 0.1) && near(maxX, 0.4) && near(minY, 0.1) && near(maxY, 0.4),
          "I8) RECT 坐标应不动");

    // ---- 9) 操作提示条已删除：Edit / PolyDraw 顶部都应是纯背景色 ----
    QImage shot(canvas.size(), QImage::Format_RGB32);
    shot.fill(Qt::black);
    canvas.render(&shot);
    check(shot.pixelColor(350, 20) == QColor(16, 20, 28),
          "I9) Edit 态顶部不应再叠操作提示条");
    canvas.setMode(CanvasMode::PolyDraw);
    click(&canvas, QPoint(200, 150));
    shot.fill(Qt::black);
    canvas.render(&shot);
    check(shot.pixelColor(350, 20) == QColor(16, 20, 28),
          "I9) PolyDraw 态顶部不应再叠操作提示条");

    // ---- 10) 触屏口径：右键不再删除区域（删除只走 DeleteMode 工具）----
    canvas.setMode(CanvasMode::Edit);
    sendMouse(&canvas, QEvent::MouseButtonPress, QPoint(100, 75), Qt::RightButton);
    sendMouse(&canvas, QEvent::MouseButtonRelease, QPoint(100, 75), Qt::RightButton);
    check(canvas.regions().size() == 1, "I10) Edit 态右键不应删除区域（触屏无右键）");

    std::cout << "  [I] 平移跟手/钳边/零漂移 + 空白追加顶点/无右键/无提示 10 组断言通过\n";
}

}  // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    QTemporaryDir workspace;
    if (!workspace.isValid())
        return 1;
    QDir::setCurrent(workspace.path());   // RuntimePaths 随 cwd：媒体/库都落临时目录
    {
        QFile config(QDir(workspace.path()).filePath(QStringLiteral("config.json")));
        config.open(QIODevice::WriteOnly);
        config.write("{}");
        config.close();
        ConfigManager::instance().load(QFileInfo(config).absoluteFilePath());
    }
    const QString dbPath = workspace.path() + QStringLiteral("/canvas.db");
    if (!DatabaseManager::instance().initialize(dbPath)) {
        std::cerr << "临时库初始化失败: " << dbPath.toStdString() << std::endl;
        return 1;
    }
    session::ConstructionInfo info;
    info.personId = "CANVAS";
    info.personName = "自检";
    info.taskId = kTask;
    info.endtime = "23:00";
    info.totalPersonCount = 1;
    if (!session::ConstructionSession::instance().startLogin(info)) {
        std::cerr << "假登录失败" << std::endl;
        return 1;
    }
    // 与真机登录路径同序：FenceManager 切到该 taskId 的作用域（C 组靠它读 pad_fences）
    geofence::FenceManager::instance().reloadForTask(kTask);

    int rc = 1;
    try {
        testCanvasGeometry();
        testCanvasAspectRatio();
        testManageWidgetStates();
        testOverlayAutoRefresh();
        testNewFenceButton();
        testStaleLoginHint();
        testRectOrderAndRefresh();
        testRenderContentBasis();
        testPolygonAutoClose();
        testDragFollowAndEdgeInsert();
        rc = 0;
        std::cout << "PASS: test_fence_canvas\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << std::endl;
    }

    session::ConstructionSession::instance().logout();
    DatabaseManager::instance().close();
    return rc;
}
