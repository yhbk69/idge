// ============================================================================
// test_fence_canvas.cpp — 围栏草稿编辑画布 + 管理页的离屏自检
// ----------------------------------------------------------------------------
// 钉的是这次新增的两块交互逻辑（都不开摄像头、不碰 RKNN）：
//   A. FenceCanvas 的几何：画矩形/多边形、**拖锚点改形状**、拖整体平移、
//      点删区域、撤销顶点。全部经由真的鼠标事件路径提交（合成 QMouseEvent），
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

    // ---- 4) 画多边形：三点 + 闭合 ----
    canvas.setMode(CanvasMode::PolyDraw);
    click(&canvas, QPoint(300, 40));
    click(&canvas, QPoint(360, 40));
    click(&canvas, QPoint(330, 100));
    canvas.closePolygon();
    check(canvas.regions().size() == 2, "4) 闭合后应新增一个多边形");
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

    // ---- 6) 撤销顶点 + 不足 3 点闭合不落区域 ----
    canvas.setMode(CanvasMode::PolyDraw);
    click(&canvas, QPoint(50, 250));
    click(&canvas, QPoint(90, 250));
    click(&canvas, QPoint(70, 280));
    canvas.undoLastPolyPoint();        // 撤到 2 点
    canvas.closePolygon();             // <3 点，静默作废
    check(canvas.regions().size() == 1, "6) 不足 3 点闭合不应落区域");
    click(&canvas, QPoint(70, 280));   // 补回第 3 点
    canvas.closePolygon();
    check(canvas.regions().size() == 2, "6) 补满 3 点闭合应落区域");

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
        rc = 0;
        std::cout << "PASS: test_fence_canvas\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << std::endl;
    }

    session::ConstructionSession::instance().logout();
    DatabaseManager::instance().close();
    return rc;
}
