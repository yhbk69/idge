// ============================================================================
// test_geofence_role.cpp — 电子围栏"每形状角色"判定的离线自检
// ----------------------------------------------------------------------------
// 只链 Qt5::Core + fence_checker.cpp：不碰 GUI、不碰 FenceManager/ConfigManager
// （那两头要 QSettings 级别的真实 config.json，属于板端的事），也不需要 RKNN
// —— fence_checker 只用 object_detect_result 的三个整数字段，不调任何 OpenCV 符号。
//
// 为什么值得单独钉：禁区与岗区域的违规条件**不是互补关系**，一条写错的归并
// 会静默产出要么永不报警、要么每帧报警的行为，板端肉眼看不出来。
//
// 覆盖八条契约：
//   1) 禁区优先：同一个人既落在禁区里、又落在岗区域外时，只出 Intrusion
//   2) 没有岗区域形状时永不判离岗（否则纯禁区通道会把每个检到的人都判成离岗）
//   3) 人在任一岗区域内就不算离岗，即使他不在第二个岗区域内
//   4) 只有岗区域、人在外 → Absence 且 shapeIndex=-1（"在所有岗区域之外"没有责任形状）
//   5) 多个禁区时 shapeIndex 指向真正命中的那一个（事件表与日志都靠它定位）
//   6) 通道未启用 / 一个形状都没有 → None（围栏不参与过滤）
//   7) 凹多边形命中逐位不变：letterbox 映射与射线法没被这次改动带坏
//   8) roleFromString 对缺键 / 空串 / 拼错一律回落 Forbidden，且与 roleToString 往返一致
//      （这条是老配置迁移的唯一兜底：认不出就当禁区，宁可多报不可漏报）
//
// 仓库根运行：./build/build_rk3588_linux/test_geofence_role，rc 0=PASS 1=FAIL
// ============================================================================

#include "fence_checker.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using geofence::ChannelFence;
using geofence::FenceChecker;
using geofence::FenceRole;
using geofence::FenceVerdict;
using geofence::FenceViolation;
using geofence::Point;
using geofence::ShapeType;

// 帧 → widget 的换算在本测试里刻意取整比：源 1920x1080、overlay 960x540，
// 两者宽高比相同 → letterbox 无黑边、缩放恰为 0.5，于是"widget 坐标 = 帧坐标 / 2"
// 可以手算，围栏顶点填的就是肉眼可读的一半尺寸。
static constexpr int kSrcW = 1920, kSrcH = 1080;
static constexpr int kOvW = 960, kOvH = 540;
static constexpr int kModel = 640;   // 传进传出都不参与计算（框已是原始空间坐标）

static void check(bool ok, const char *message)
{
    if (!ok) {
        throw std::runtime_error(message);
    }
}

/// 一个"站立的框"：脚点 = ((left+right)/2, bottom)。宽 100 高 200，
/// 于是脚点在帧空间是 (footX+50, footY+200)，映射到 widget 是 ((footX+50)/2, (footY+200)/2)。
/// 传进来的 x/y 想表达"脚点的 widget 坐标"时，用 footBoxAt() 反算最省事。
static object_detect_result makeBox(int left, int top, int right, int bottom)
{
    object_detect_result det{};
    det.box.left = left;
    det.box.top = top;
    det.box.right = right;
    det.box.bottom = bottom;
    det.prop = 0.9f;
    det.cls_id = 0;
    return det;
}

/// 让脚点落在 widget 坐标 (widgetX, widgetY)：帧坐标 = widget 坐标 × 2
static object_detect_result footAt(int widgetX, int widgetY)
{
    const int footFrameX = widgetX * 2;
    const int footFrameY = widgetY * 2;
    return makeBox(footFrameX - 50, footFrameY - 200, footFrameX + 50, footFrameY);
}

/// widget 空间里的禁区矩形（role 显式给 Forbidden）
static geofence::FenceShape rect(FenceRole role, int x1, int y1, int x2, int y2)
{
    geofence::FenceShape shape;
    shape.type = ShapeType::Rectangle;
    shape.role = role;
    shape.points.push_back(Point(x1, y1));
    shape.points.push_back(Point(x2, y2));
    return shape;
}

static geofence::FenceShape poly(FenceRole role, std::vector<Point> pts)
{
    geofence::FenceShape shape;
    shape.type = ShapeType::Polygon;
    shape.role = role;
    for (const auto &p : pts) shape.points.push_back(p);
    return shape;
}

static ChannelFence fence(std::vector<geofence::FenceShape> shapes, bool enabled = true)
{
    ChannelFence cf;
    cf.enabled = enabled;
    cf.shapes = shapes;
    return cf;
}

static FenceViolation verdictOf(const object_detect_result &det, const ChannelFence &cf)
{
    return FenceChecker::checkDetection(det, kSrcW, kSrcH, kModel, kModel, kOvW, kOvH, cf);
}

int main()
try {
    // ========================================================================
    // 1) 禁区优先：两种条件同时成立时只出闯入
    // ========================================================================
    {
        // 禁区 widget (100,100)-(200,200)；岗区域 widget (600,100)-(700,200)
        // 脚点 (150,150) → 在禁区里、在岗区域外，两个条件都满足
        const ChannelFence cf = fence({
            rect(FenceRole::Forbidden, 100, 100, 200, 200),
            rect(FenceRole::Workzone, 600, 100, 700, 200),
        });
        const FenceViolation v = verdictOf(footAt(150, 150), cf);
        check(v.verdict == FenceVerdict::Intrusion,
              "禁区内 + 岗区域外应判闯入（禁区优先），实际不是");
        check(v.role == FenceRole::Forbidden, "闯入的 role 必须是 Forbidden");
        check(v.shapeIndex == 0, "闯入的 shapeIndex 必须指向命中的那个禁区形状");
    }

    // ========================================================================
    // 2) 没有岗区域形状 ⇒ 永不离岗
    // ========================================================================
    // 这是最容易被写坏的一条：若实现漏掉 hasWorkzone 判据，"不在任何岗区域内"
    // 对空集合恒为真，于是纯禁区通道会把每一个检到的人（哪怕他离围栏十万八千里）
    // 每帧判一次离岗。
    {
        const ChannelFence onlyForbidden = fence({
            rect(FenceRole::Forbidden, 100, 100, 200, 200),
        });
        const FenceViolation v = verdictOf(footAt(800, 400), onlyForbidden);
        check(v.verdict == FenceVerdict::None,
              "只有禁区、人在两处之外时必须是 None，绝不能判离岗");

        // 一个禁区都没有、只有岗区域时，人在区域内也必须是 None
        const ChannelFence onlyWorkzone = fence({
            rect(FenceRole::Workzone, 100, 100, 400, 400),
        });
        check(verdictOf(footAt(250, 250), onlyWorkzone).verdict == FenceVerdict::None,
              "人在岗区域内必须是 None");
    }

    // ========================================================================
    // 3) 人在任一岗区域内就不算离岗（"在所有岗区域之外"，非逐区域判定）
    // ========================================================================
    {
        const ChannelFence cf = fence({
            rect(FenceRole::Workzone, 100, 100, 200, 200),   // 岗区域 A：人在这里
            rect(FenceRole::Workzone, 600, 100, 700, 200),   // 岗区域 B：人不在这里
        });
        const FenceViolation v = verdictOf(footAt(150, 150), cf);
        check(v.verdict == FenceVerdict::None,
              "人站在 A 岗区域里，仅因为不在 B 里就判离岗 —— 归并方向写反了");
    }

    // ========================================================================
    // 4) 只有岗区域、人在外 → Absence 且 shapeIndex=-1
    // ========================================================================
    {
        const ChannelFence cf = fence({
            rect(FenceRole::Workzone, 100, 100, 200, 200),
            rect(FenceRole::Workzone, 300, 100, 400, 200),
        });
        const FenceViolation v = verdictOf(footAt(800, 400), cf);
        check(v.verdict == FenceVerdict::Absence, "人在两个岗区域之外应判离岗");
        check(v.role == FenceRole::Workzone, "离岗的 role 必须是 Workzone");
        check(v.shapeIndex == -1,
              "离岗是对整张形状表的负向推断，没有单一责任形状，必须是 -1");

        // 禁区 + 岗区域混合，人两处都不在 → 离岗（禁区的 shapeIndex 不该被带出来）
        const ChannelFence mixed = fence({
            rect(FenceRole::Forbidden, 100, 100, 200, 200),
            rect(FenceRole::Workzone, 300, 100, 400, 200),
        });
        const FenceViolation mv = verdictOf(footAt(800, 400), mixed);
        check(mv.verdict == FenceVerdict::Absence && mv.shapeIndex == -1,
              "混合角色下人在禁区外且在岗区域外，应判离岗且无责任形状");
    }

    // ========================================================================
    // 5) 多个禁区时 shapeIndex 指向真正命中的那个
    // ========================================================================
    {
        const ChannelFence cf = fence({
            rect(FenceRole::Forbidden, 100, 100, 200, 200),
            rect(FenceRole::Forbidden, 300, 100, 400, 200),
            rect(FenceRole::Forbidden, 500, 100, 600, 200),
        });
        const FenceViolation v = verdictOf(footAt(550, 150), cf);
        check(v.verdict == FenceVerdict::Intrusion && v.shapeIndex == 2,
              "shapeIndex 必须是实际命中的第 3 个禁区，不是 0 也不是 -1");
    }

    // ========================================================================
    // 6) 通道未启用 / 无形状 → None
    // ========================================================================
    {
        const ChannelFence off = fence({
            rect(FenceRole::Forbidden, 0, 0, 900, 500),   // 覆盖全画面，启用时必报
        }, /*enabled=*/false);
        check(verdictOf(footAt(450, 250), off).verdict == FenceVerdict::None,
              "通道 enabled=false 时一个形状都不该参与判定");

        const ChannelFence emptyShapes = fence({});
        check(verdictOf(footAt(450, 250), emptyShapes).verdict == FenceVerdict::None,
              "没有任何形状时必须是 None（尤其不能判离岗）");
    }

    // ========================================================================
    // 7) 凹多边形命中逐位不变（射线法 + letterbox 的回归保护）
    // ========================================================================
    // U 形岗区域：widget 空间 (100,100)-(400,300) 挖掉中间 (200,100)-(300,250) 的凹槽。
    // 顶点顺序：外框左下起逆时针，凹槽在中间向上开口。
    {
        const std::vector<Point> uShape = {
            {100, 300}, {100, 100}, {200, 100}, {200, 250},
            {300, 250}, {300, 100}, {400, 100}, {400, 300}
        };
        const ChannelFence cf = fence({ poly(FenceRole::Workzone, uShape) });

        // 凹槽正中 (250,150)：射线法判为外部 → 人在"所有岗区域之外" → 离岗
        check(verdictOf(footAt(250, 150), cf).verdict == FenceVerdict::Absence,
              "U 形凹槽里不该算在岗区域内（凹多边形被当成凸包就会漏报离岗）");
        // 左臂内 (150,200)：在岗区域内 → None
        check(verdictOf(footAt(150, 200), cf).verdict == FenceVerdict::None,
              "U 形左臂内是在岗");
        // 右臂内 (350,200)：同上
        check(verdictOf(footAt(350, 200), cf).verdict == FenceVerdict::None,
              "U 形右臂内是在岗");
        // 外接矩形右下角之外 (450,320)：离岗（确认没有用 toRect 兜底多边形）
        check(verdictOf(footAt(450, 320), cf).verdict == FenceVerdict::Absence,
              "多边形不能用外接矩形判定");

        // 同一形状换成禁区角色：凹槽外、臂内 → 闯入，且顶点数足够走 polygon 支路
        const ChannelFence cfForbidden = fence({ poly(FenceRole::Forbidden, uShape) });
        const FenceViolation v = verdictOf(footAt(150, 200), cfForbidden);
        check(v.verdict == FenceVerdict::Intrusion && v.shapeIndex == 0,
              "同一凹多边形挂禁区角色时臂内应判闯入");
    }

    // ========================================================================
    // 8) 角色字符串：迁移的唯一兜底路径
    // ========================================================================
    {
        check(geofence::roleFromString("") == FenceRole::Forbidden,
              "空 role 必须回落禁区（缺键的老配置就走这条路）");
        check(geofence::roleFromString("garbage") == FenceRole::Forbidden,
              "认不出的 role 必须回落禁区：宁可多报不可漏报");
        check(geofence::roleFromString("Forbidden") == FenceRole::Forbidden,
              "大小写不属于协议，未知值一律回落禁区");
        check(geofence::roleFromString("workzone") == FenceRole::Workzone,
              "workzone 必须认");
        check(geofence::roleToString(FenceRole::Workzone) == "workzone" &&
              geofence::roleToString(FenceRole::Forbidden) == "forbidden",
              "roleToString 的字面量是 config.json 的协议，改动等于改 schema");
        // 往返：落盘再读回必须同值，否则迁移写回的 role 会在下次加载漂移
        for (const auto r : {FenceRole::Forbidden, FenceRole::Workzone}) {
            check(geofence::roleFromString(geofence::roleToString(r)) == r,
                  "role 往返不一致，重存一次就会改角色");
        }
    }

    std::cout << "fence role contracts passed: forbidden-over-absence / no-workzone-never-absent"
                 " / inside-any-workzone / absence-index-minus-one / shapeIndex targeting"
                 " / disabled-and-empty / concave polygon / role string round-trip\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
