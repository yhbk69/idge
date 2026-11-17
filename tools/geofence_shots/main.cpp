// ============================================================================
// geofence_shots.cpp — 电子围栏图文说明用的离屏出图工具
// ----------------------------------------------------------------------------
// 产物是**生产代码自己画出来的**，不是手工拼的示意图：
//   - 底图：板端真实抓拍帧（data/alarms/... 里的报警快照，检测框已烧在画面上）
//   - 围栏：真 FenceCanvas，顶点由合成鼠标事件走真交互路径提交（第 3 点自动闭合；
//     >3 点走 Edit 态空白处单击按点序追加），再经 FenceManager 交给
//     只读 FenceOverlay 显示（绘制入口迁到管理页后 overlay 不再收鼠标）
//   - 结论：真 FenceChecker::checkDetection 对同一帧的脚点裁决
//   - 落库：真 FenceManager → NativeFenceStore → 临时 idge.db
//   - Pad 合成画面：真 renderFenceFrame
// 库与配置都指向临时目录，不碰板端的 data/idge.db 和 config.json。
//
// 仓库根运行：
//   QT_QPA_PLATFORM=offscreen ./build/build_rk3588_linux/geofence_shots \
//       <出图目录> <底图jpg>
// ============================================================================

#include "ConfigManager.h"
#include "construction_session.h"
#include "database_manager.h"
#include "fence_canvas.h"
#include "fence_checker.h"
#include "fence_frame_renderer.h"
#include "fence_manager.h"
#include "fence_overlay.h"
#include "native_fence_store.h"
#include "pad_fence_store.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QTemporaryDir>
#include <QVector>

#include <iostream>
#include <vector>

using geofence::CanvasMode;
using geofence::ChannelFence;
using geofence::FenceCanvas;
using geofence::FenceChecker;
using geofence::FenceOverlay;
using geofence::FenceRole;
using geofence::FenceShape;
using geofence::FenceVerdict;
using geofence::FenceViolation;
using geofence::PadRegion;
using geofence::Point;
using geofence::ShapeType;

namespace {

// ── 底图里那根蓝色检测框 ────────────────────────────────────────────────────
struct PixelBox {
    int left = 0, top = 0, right = 0, bottom = 0;
    bool found = false;
};

// 报警快照上的框是纯蓝描边，直接量出来比手填坐标可靠：脚点必须是真脚点。
// 只认"长线"：竖边框是某一列上连续几百个蓝像素，横边框是某一行上几百个。
// 按整体包围盒量会被画面右上角那枚蓝色水印带偏（实测右边界冲到 2386）。
PixelBox measureBlueBox(const QImage &image)
{
    const int w = image.width(), h = image.height();
    auto isBlue = [&image](int x, int y) {
        const QRgb rgb = image.pixel(x, y);
        return qBlue(rgb) > 150 && qRed(rgb) < 120 && qGreen(rgb) < 120;
    };

    int left = -1, right = -1;
    const int colMin = h * 2 / 5;
    for (int x = 0; x < w; ++x) {
        int count = 0;
        for (int y = 0; y < h; ++y)
            if (isBlue(x, y))
                ++count;
        if (count >= colMin) {
            if (left < 0)
                left = x;
            right = x;
        }
    }
    if (left < 0 || right - left < 8)
        return PixelBox();

    int top = -1, bottom = -1;
    const int rowMin = (right - left) * 3 / 5;
    for (int y = 0; y < h; ++y) {
        int count = 0;
        for (int x = left; x <= right; ++x)
            if (isBlue(x, y))
                ++count;
        if (count >= rowMin) {
            if (top < 0)
                top = y;
            bottom = y;
        }
    }
    if (top < 0 || bottom - top < 8)
        return PixelBox();

    PixelBox out;
    out.found = true;
    out.left = left;
    out.top = top;
    out.right = right;
    out.bottom = bottom;
    return out;
}

// ── 合成鼠标事件：让围栏经由真的绘制路径提交（FenceCanvas，管理页同款画布）──
void sendMouse(FenceCanvas *canvas, QEvent::Type type, const QPoint &pos,
               Qt::MouseButton button)
{
    const Qt::MouseButtons buttons =
        (type == QEvent::MouseButtonRelease) ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, QPointF(pos), QPointF(pos), QPointF(pos), button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(canvas, &event);
}

void click(FenceCanvas *canvas, const QPoint &pos)
{
    sendMouse(canvas, QEvent::MouseButtonPress, pos, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseButtonRelease, pos, Qt::LeftButton);
}

void drag(FenceCanvas *canvas, const QPoint &a, const QPoint &b)
{
    sendMouse(canvas, QEvent::MouseButtonPress, a, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseMove, b, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseButtonRelease, b, Qt::LeftButton);
}

void dragRect(FenceCanvas *canvas, const QPoint &a, const QPoint &b)
{
    canvas->setMode(CanvasMode::RectDraw);
    drag(canvas, a, b);
}

// 多边形：前 3 点走 PolyDraw（第 3 点落地自动闭合提交，2026-11-17 用户裁定）；
// 第 4 点起画布已自动回 Edit，空白处单击按点序追加顶点（1-2-…-n-1 闭合）。
// 不直接 setRegions：出图承诺的是真交互路径。
void clickPolygon(FenceCanvas *canvas, const QVector<QPoint> &points)
{
    canvas->setMode(CanvasMode::PolyDraw);
    for (const QPoint &pt : points)
        click(canvas, pt);
}

// 画布草稿（PadRegion 0~1）→ 视频页围栏（FenceShape 0..kFenceNormBase）。
// 规则配色同源：INTRUSION↔Forbidden 红、CROSS↔Workzone 琥珀。
ChannelFence fenceFromCanvas(const FenceCanvas *canvas)
{
    ChannelFence fence;
    fence.enabled = true;
    for (const PadRegion &region : canvas->regions()) {
        FenceShape shape;
        shape.role = region.ruleType == QLatin1String("CROSS") ? FenceRole::Workzone
                                                               : FenceRole::Forbidden;
        if (region.shapeType == QLatin1String("RECT")) {
            shape.type = ShapeType::Rectangle;
            double minX = 1.0, maxX = 0.0, minY = 1.0, maxY = 0.0;
            for (const QPointF &p : region.points) {
                minX = qMin(minX, p.x()); maxX = qMax(maxX, p.x());
                minY = qMin(minY, p.y()); maxY = qMax(maxY, p.y());
            }
            shape.points.push_back(Point(qRound(minX * geofence::kFenceNormBase),
                                         qRound(minY * geofence::kFenceNormBase)));
            shape.points.push_back(Point(qRound(maxX * geofence::kFenceNormBase),
                                         qRound(maxY * geofence::kFenceNormBase)));
        } else {
            shape.type = ShapeType::Polygon;
            for (const QPointF &p : region.points) {
                shape.points.push_back(Point(qRound(p.x() * geofence::kFenceNormBase),
                                             qRound(p.y() * geofence::kFenceNormBase)));
            }
        }
        fence.shapes.push_back(shape);
    }
    return fence;
}

// 画布出图：背景帧由画布自己 letterbox（与视频页同口径），直接 render 整块画布。
// 用于"画到一半"的临时图形——那种状态只存在于画布，不在只读 overlay 里。
bool captureCanvas(FenceCanvas *canvas, const QSize &widgetSize, const QString &outPath,
                   QString *err)
{
    canvas->resize(widgetSize);
    QImage shot(widgetSize, QImage::Format_ARGB32_Premultiplied);
    shot.fill(Qt::black);
    canvas->render(&shot);
    if (!shot.save(outPath)) {
        *err = QStringLiteral("写不出 %1").arg(outPath);
        return false;
    }
    std::cout << "[shot] " << outPath.toStdString() << "  widget=" << widgetSize.width() << "x"
              << widgetSize.height() << std::endl;
    return true;
}

// ── 出图：底图按 letterbox 铺进 widget，overlay 叠在上面 ─────────────────────
// contentRect 的算法与 GLVideoWidget 一致（等比居中、余下是黑边），
// 这样 overlay 拿到的换算基准和现场一样。
QRect contentRectFor(const QSize &widget, const QSize &frame)
{
    const double scale =
        qMin(double(widget.width()) / frame.width(), double(widget.height()) / frame.height());
    const QSize scaled(int(frame.width() * scale), int(frame.height() * scale));
    return QRect((widget.width() - scaled.width()) / 2, (widget.height() - scaled.height()) / 2,
                 scaled.width(), scaled.height());
}

bool capture(FenceOverlay *overlay, const QImage &frame, const QSize &widgetSize,
             const QString &outPath, QString *err)
{
    const QRect content = contentRectFor(widgetSize, frame.size());
    overlay->resize(widgetSize);
    overlay->setContentRect(content);

    QImage canvas(widgetSize, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::black);
    {
        QPainter painter(&canvas);
        painter.drawImage(content.topLeft(),
                          frame.scaled(content.size(), Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation));
    }

    QImage layer(widgetSize, QImage::Format_ARGB32_Premultiplied);
    layer.fill(Qt::transparent);
    overlay->render(&layer);
    {
        QPainter painter(&canvas);
        painter.drawImage(0, 0, layer);
    }
    if (!canvas.save(outPath)) {
        *err = QStringLiteral("写不出 %1").arg(outPath);
        return false;
    }
    std::cout << "[shot] " << outPath.toStdString() << "  widget=" << widgetSize.width() << "x"
              << widgetSize.height() << " content=" << content.x() << "," << content.y() << ","
              << content.width() << "x" << content.height() << std::endl;
    return true;
}

// ── 判定：真 FenceChecker 对真脚点出结论 ────────────────────────────────────
object_detect_result detectionFromBox(const PixelBox &box)
{
    object_detect_result det{};
    det.box.left = box.left;
    det.box.top = box.top;
    det.box.right = box.right;
    det.box.bottom = box.bottom;
    det.prop = 0.67f;
    det.cls_id = 0;   // person
    return det;
}

std::string verdictText(const FenceViolation &v)
{
    switch (v.verdict) {
    case FenceVerdict::None:
        return "None（合规，不报）";
    case FenceVerdict::Intrusion:
        return "Intrusion（闯入禁区，shapeIndex=" + std::to_string(v.shapeIndex) + "）";
    case FenceVerdict::Absence:
        return "Absence（离岗，人在所有岗区域之外，shapeIndex=-1）";
    }
    return "?";
}

// 把某通道当前围栏的归一化顶点打出来：文档里的数字直接抄这里，不手填。
void dumpShapes(int channel)
{
    const ChannelFence fence = geofence::FenceManager::instance().channelFence(channel);
    std::cout << "  通道" << channel << " enabled=" << (fence.enabled ? "true" : "false")
              << " 形状数=" << fence.shapes.size() << std::endl;
    for (size_t i = 0; i < fence.shapes.size(); ++i) {
        const FenceShape &shape = fence.shapes[i];
        std::cout << "    [" << i << "] "
                  << (shape.type == ShapeType::Rectangle ? "rectangle" : "polygon") << " "
                  << geofence::roleToString(shape.role) << " points=[";
        for (size_t k = 0; k < shape.points.size(); ++k) {
            std::cout << shape.points[k].x << "," << shape.points[k].y
                      << (k + 1 < shape.points.size() ? " " : "");
        }
        std::cout << "]" << std::endl;
    }
}

FenceShape rectShape(FenceRole role, int x1, int y1, int x2, int y2)
{
    FenceShape shape;
    shape.type = ShapeType::Rectangle;
    shape.role = role;
    shape.points.push_back(Point(x1, y1));
    shape.points.push_back(Point(x2, y2));
    return shape;
}

FenceShape polygonShape(FenceRole role, const std::vector<Point> &points)
{
    FenceShape shape;
    shape.type = ShapeType::Polygon;
    shape.role = role;
    shape.points = points;
    return shape;
}

// 直接写进 FenceManager（等价于工具栏点完之后的落库结果），
// 用于"同一批顶点在不同画布尺寸下"的那张对比图。
void putFence(int channel, const ChannelFence &fence)
{
    geofence::FenceManager::instance().setChannelFence(channel, fence);
}

}   // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    if (argc < 3) {
        std::cerr << "用法: geofence_shots <出图目录> <底图jpg> [底图2jpg ...]" << std::endl;
        return 2;
    }
    const QString outDir = QString::fromLocal8Bit(argv[1]);
    const QString framePath = QString::fromLocal8Bit(argv[2]);
    QDir().mkpath(outDir);

    QImage frame(framePath);
    if (frame.isNull()) {
        std::cerr << "底图读不出来: " << framePath.toStdString() << std::endl;
        return 1;
    }
    frame = frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QSize kSrc = frame.size();
    const PixelBox box = measureBlueBox(frame);
    if (!box.found) {
        std::cerr << "底图里没量到蓝色检测框，脚点无从谈起" << std::endl;
        return 1;
    }
    const int footX = (box.left + box.right) / 2;
    const int footY = box.bottom;
    std::cout << "=== 底图 " << framePath.toStdString() << " ===" << std::endl;
    std::cout << "帧尺寸 " << kSrc.width() << "x" << kSrc.height() << "；量到检测框 left/top/right/bottom = "
              << box.left << "/" << box.top << "/" << box.right << "/" << box.bottom << std::endl;
    std::cout << "脚点(帧像素) = " << footX << "," << footY << " → 归一化 = "
              << geofence::normFromPixel(footX, 0, kSrc.width()) << ","
              << geofence::normFromPixel(footY, 0, kSrc.height()) << std::endl;

    // ── 临时库 + 临时配置 + 假登录（不落板端的库，也不写板端的 config）────────
    QTemporaryDir workspace;
    if (!workspace.isValid())
        return 1;
    const QString dbPath = workspace.path() + QStringLiteral("/geofence_shots.db");
    {
        QFile config(QDir(workspace.path()).filePath(QStringLiteral("config.json")));
        config.open(QIODevice::WriteOnly);
        config.write("{}");
        config.close();
        ConfigManager::instance().load(QFileInfo(config).absoluteFilePath());
    }
    if (!DatabaseManager::instance().initialize(dbPath)) {
        std::cerr << "临时库初始化失败: " << dbPath.toStdString() << std::endl;
        return 1;
    }
    session::ConstructionInfo info;
    info.personId = "SHOT";
    info.personName = "出图工具";
    info.taskId = "DEMO-TASK";
    info.endtime = "23:00";
    info.totalPersonCount = 1;
    session::ConstructionSession::instance().startLogin(info);
    std::cout << "登录态 taskId=" << session::ConstructionSession::instance().snapshot().taskId
              .toStdString() << "（未登录时围栏根本不让写）" << std::endl;

    FenceOverlay overlay;
    overlay.show();

    const QSize kW(1280, 720);
    // 先把画布立起来再合成鼠标事件：overlay 拿不到 contentRect 时会退回用 widget
    // 自身尺寸换算，默认构造的 640x480 会把顶点算成 1.3 万这种越界归一化值。
    overlay.resize(kW);
    overlay.setContentRect(contentRectFor(kW, kSrc));

    // 绘制画布铺同一张底图：它的 contentRect 与 overlay 的换算基准同口径
    // （等比居中），合成鼠标坐标在两层里才是一致的。
    FenceCanvas canvas;
    canvas.resize(kW);
    canvas.setBackground(frame);
    QString err;
    const object_detect_result det = detectionFromBox(box);

    // ── 图 1：禁区盖住人 → 闯入 ──────────────────────────────────────────────
    std::cout << "\n=== 图1 禁区盖住人 ===" << std::endl;
    canvas.setRegions({});
    canvas.setCreateRule(QStringLiteral("INTRUSION"));
    dragRect(&canvas, QPoint(box.left - 45, box.top - 25), QPoint(box.right + 45, box.bottom + 25));
    geofence::FenceManager::instance().setChannelFence(1, fenceFromCanvas(&canvas));
    overlay.setChannel(1);
    overlay.loadFences();
    dumpShapes(1);
    {
        const FenceViolation v =
            FenceChecker::checkDetection(det, kSrc.width(), kSrc.height(), 640, 640,
                                         geofence::FenceManager::instance().channelFence(1));
        std::cout << "  判定: " << verdictText(v) << std::endl;
    }
    capture(&overlay, frame, kW, outDir + "/geofence_01_forbidden_intrusion.png", &err);

    // ── 图 2：禁区只圈右侧脚手架 → 合规不报 ─────────────────────────────────
    std::cout << "\n=== 图2 禁区在右侧，人不在里面 ===" << std::endl;
    canvas.setRegions({});
    canvas.setCreateRule(QStringLiteral("INTRUSION"));
    dragRect(&canvas, QPoint(860, 60), QPoint(1240, 690));
    geofence::FenceManager::instance().setChannelFence(2, fenceFromCanvas(&canvas));
    overlay.setChannel(2);
    overlay.loadFences();
    dumpShapes(2);
    {
        const FenceViolation v =
            FenceChecker::checkDetection(det, kSrc.width(), kSrc.height(), 640, 640,
                                         geofence::FenceManager::instance().channelFence(2));
        std::cout << "  判定: " << verdictText(v) << std::endl;
    }
    capture(&overlay, frame, kW, outDir + "/geofence_02_forbidden_clear.png", &err);

    // ── 图 3：同一通道混用岗区域 + 禁区，人守着岗 → 合规 ────────────────────
    std::cout << "\n=== 图3 岗区域圈住人 + 右侧禁区（混用）===" << std::endl;
    canvas.setRegions({});
    canvas.setCreateRule(QStringLiteral("CROSS"));
    clickPolygon(&canvas, {QPoint(545, 130), QPoint(845, 130), QPoint(860, 400), QPoint(800, 700),
                           QPoint(560, 700), QPoint(500, 400)});
    canvas.setCreateRule(QStringLiteral("INTRUSION"));
    dragRect(&canvas, QPoint(860, 60), QPoint(1240, 690));
    geofence::FenceManager::instance().setChannelFence(3, fenceFromCanvas(&canvas));
    overlay.setChannel(3);
    overlay.loadFences();
    dumpShapes(3);
    {
        const FenceViolation v =
            FenceChecker::checkDetection(det, kSrc.width(), kSrc.height(), 640, 640,
                                         geofence::FenceManager::instance().channelFence(3));
        std::cout << "  判定: " << verdictText(v) << std::endl;
    }
    capture(&overlay, frame, kW, outDir + "/geofence_03_mixed_workzone_ok.png", &err);

    // ── 图 4：岗区域画在左侧空处，人在所有岗区域之外 → 离岗 ─────────────────
    std::cout << "\n=== 图4 岗区域在左侧，人不在里面 ===" << std::endl;
    canvas.setRegions({});
    canvas.setCreateRule(QStringLiteral("CROSS"));
    clickPolygon(&canvas, {QPoint(60, 120), QPoint(430, 120), QPoint(430, 700), QPoint(60, 700)});
    geofence::FenceManager::instance().setChannelFence(4, fenceFromCanvas(&canvas));
    overlay.setChannel(4);
    overlay.loadFences();
    dumpShapes(4);
    {
        const FenceViolation v =
            FenceChecker::checkDetection(det, kSrc.width(), kSrc.height(), 640, 640,
                                         geofence::FenceManager::instance().channelFence(4));
        std::cout << "  判定: " << verdictText(v) << std::endl;
    }
    capture(&overlay, frame, kW, outDir + "/geofence_04_workzone_absence.png", &err);

    // ── 图 5：多边形画到一半（2 点 + 光标预览闭合三角形：虚线 + 顶点序号）─────
    std::cout << "\n=== 图5 多边形画到一半 ===" << std::endl;
    canvas.setRegions({});
    canvas.setCreateRule(QStringLiteral("CROSS"));
    canvas.setMode(CanvasMode::PolyDraw);
    click(&canvas, QPoint(545, 130));
    click(&canvas, QPoint(845, 130));
    sendMouse(&canvas, QEvent::MouseMove, QPoint(700, 620), Qt::LeftButton);
    std::cout << "  （第 3 点没落地，形状没提交，通道5 形状数应为 0）" << std::endl;
    dumpShapes(5);
    captureCanvas(&canvas, kW, outDir + "/geofence_05_polygon_in_progress.png", &err);

    // ── 图 6：矩形拖动中（临时矩形，虚线）───────────────────────────────────
    std::cout << "\n=== 图6 矩形拖动中 ===" << std::endl;
    canvas.setRegions({});
    canvas.setCreateRule(QStringLiteral("INTRUSION"));
    canvas.setMode(CanvasMode::RectDraw);
    sendMouse(&canvas, QEvent::MouseButtonPress, QPoint(810, 60), Qt::LeftButton);
    sendMouse(&canvas, QEvent::MouseMove, QPoint(1150, 640), Qt::LeftButton);
    dumpShapes(6);
    captureCanvas(&canvas, kW, outDir + "/geofence_06_rect_dragging.png", &err);

    // ── 图 7：同一批归一化顶点，三种画布（单路 / 2x2 / 带黑边）───────────────
    std::cout << "\n=== 图7 换视图不漂 ===" << std::endl;
    putFence(7, geofence::FenceManager::instance().channelFence(3));
    dumpShapes(7);
    {
        const struct Panel {
            QSize widget;
            const char *name;
        } panels[] = {
            {QSize(1280, 720), "单路放大 1280x720"},
            {QSize(640, 360), "2x2 分屏 640x360"},
            {QSize(960, 720), "竖一点的视频窗 960x720（上下黑边）"},
        };
        QImage strip(QSize(1280 + 640 + 960 + 20, 720), QImage::Format_ARGB32_Premultiplied);
        strip.fill(QColor(12, 14, 18));
        int x = 0;
        overlay.setChannel(7);
        overlay.loadFences();
        for (const auto &panel : panels) {
            const QRect content = contentRectFor(panel.widget, kSrc);
            overlay.resize(panel.widget);
            overlay.setContentRect(content);
            QImage tile(panel.widget, QImage::Format_ARGB32_Premultiplied);
            tile.fill(Qt::black);
            {
                QPainter painter(&tile);
                painter.drawImage(content.topLeft(),
                                  frame.scaled(content.size(), Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation));
            }
            QImage layer(panel.widget, QImage::Format_ARGB32_Premultiplied);
            layer.fill(Qt::transparent);
            overlay.render(&layer);
            {
                QPainter painter(&tile);
                painter.drawImage(0, 0, layer);
            }
            QPainter(&strip).drawImage(x, 0, tile);
            std::cout << "  " << panel.name << " content=" << content.x() << "," << content.y()
                      << "," << content.width() << "x" << content.height() << std::endl;
            x += panel.widget.width() + 10;
        }
        strip.save(outDir + "/geofence_07_view_invariance.png");
        std::cout << "[shot] " << (outDir + "/geofence_07_view_invariance.png").toStdString() << std::endl;
    }

    // ── 图 8：Pad v1 §5.3 的"合成生效画面"，真 renderFenceFrame ──────────────
    std::cout << "\n=== 图8 Pad v1 合成生效画面 ===" << std::endl;
    {
        const QString mediaRoot = workspace.path() + QStringLiteral("/media");
        QDir().mkpath(mediaRoot);
        QVector<PadRegion> regions;
        PadRegion intrusion;
        intrusion.regionId = "RG-pad-1";
        intrusion.shapeType = "RECT";
        intrusion.ruleType = "INTRUSION";
        intrusion.points = {QPointF(0.63, 0.08), QPointF(0.96, 0.08), QPointF(0.96, 0.96),
                            QPointF(0.63, 0.96)};
        regions.push_back(intrusion);
        PadRegion cross;
        cross.regionId = "RG-pad-2";
        cross.shapeType = "POLYGON";
        cross.ruleType = "CROSS";
        cross.points = {QPointF(0.42, 0.18), QPointF(0.66, 0.18), QPointF(0.67, 0.55),
                        QPointF(0.62, 0.97), QPointF(0.43, 0.97), QPointF(0.39, 0.55)};
        regions.push_back(cross);

        QString absPath, relPath, renderErr;
        if (geofence::renderFenceFrame("NFENCE-DEMO", 1, regions, mediaRoot, "fence-frames",
                                       &absPath, &relPath, &renderErr)) {
            std::cout << "  落盘 rel=" << relPath.toStdString() << std::endl;
            QFile::copy(absPath, outDir + "/geofence_08_pad_fence_frame.png");
            std::cout << "[shot] " << (outDir + "/geofence_08_pad_fence_frame.png").toStdString()
                      << std::endl;
        } else {
            std::cout << "  渲染失败: " << renderErr.toStdString() << std::endl;
        }
    }

    // ── 落库回读：本机围栏在 DB 里的真实形状 ────────────────────────────────
    // 注意：走 FenceManager 的保存路径时 stream_id 传的是默认构造 QString()，
    // 绑进 SQL 是 NULL，撞 native_fences.stream_id 的 NOT NULL，插入整条失败
    // （上面每条 "insert fence failed" 就是它）。内存里的围栏还在，所以现场
    // 看着生效，重启后全丢。这里改用显式 streamId 直写一次，把"本该长什么样"读回来。
    std::cout << "\n=== native_fences 回读（taskId=DEMO-TASK）===" << std::endl;
    geofence::NativeFenceStore store;
    {
        const ChannelFence cf = geofence::FenceManager::instance().channelFence(3);
        QVector<PadRegion> regions;
        for (const FenceShape &shape : cf.shapes) {
            PadRegion region;
            region.regionId = QStringLiteral("RG-DEMO-%1").arg(regions.size() + 1, 2, 10,
                                                                QLatin1Char('0'));
            region.shapeType = shape.type == ShapeType::Rectangle ? QStringLiteral("RECT")
                                                                  : QStringLiteral("POLYGON");
            region.ruleType = geofence::roleToString(shape.role) == "workzone"
                                  ? QStringLiteral("CROSS")
                                  : QStringLiteral("INTRUSION");
            for (const Point &p : shape.points)
                region.points.push_back(QPointF(p.x / 10000.0, p.y / 10000.0));
            regions.push_back(region);
        }
        const QString fenceId = store.saveActiveVersion("DEMO-TASK", "CH3", "CH3",
                                                        QStringLiteral("通道3围栏"), regions);
        std::cout << "  显式带 stream_id 直写：" << (fenceId.isEmpty() ? "失败" : fenceId.toStdString())
                  << std::endl;
    }
    for (const auto &fence : store.fencesForTask("DEMO-TASK")) {
        for (const auto &version : fence.versions) {
            std::cout << "  " << fence.fenceId.toStdString() << " " << fence.cameraId.toStdString()
                      << " v" << version.version << " " << geofence::padStatusName(version.status).toStdString()
                      << " 区域数=" << version.regions.size() << std::endl;
            for (const auto &region : version.regions) {
                std::cout << "    " << region.regionId.toStdString() << " "
                          << region.shapeType.toStdString() << " " << region.ruleType.toStdString()
                          << " 点数=" << region.points.size() << std::endl;
            }
        }
    }

    std::cout << "\n出图目录: " << outDir.toStdString() << std::endl;
    return err.isEmpty() ? 0 : 1;
}
