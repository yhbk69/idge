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
// 覆盖九条契约：
//   1) 禁区优先：同一个人既落在禁区里、又落在岗区域外时，只出 Intrusion
//   2) 没有岗区域形状时永不判离岗（否则纯禁区通道会把每个检到的人都判成离岗）
//   3) 人在任一岗区域内就不算离岗，即使他不在第二个岗区域内
//   4) 只有岗区域、人在外 → Absence 且 shapeIndex=-1（"在所有岗区域之外"没有责任形状）
//   5) 多个禁区时 shapeIndex 指向真正命中的那一个（事件表与日志都靠它定位）
//   6) 通道未启用 / 一个形状都没有 → None（围栏不参与过滤）
//   7) 凹多边形命中逐位不变：归一化换算与射线法没被这次改动带坏
//   8) roleFromString 对缺键 / 空串 / 拼错一律回落 Forbidden，且与 roleToString 往返一致
//      （这条是老配置迁移的唯一兜底：认不出就当禁区，宁可多报不可漏报）
//   9) 判定与视图尺寸无关：同一组归一化顶点，在小画布和大画布上往返换算后
//      落在同一归一化位置（换 2x2/单路放大/全屏都不动围栏语义的唯一离线证据）
//
// 仓库根运行：./build/build_rk3588_linux/test_geofence_role，rc 0=PASS 1=FAIL
// ============================================================================

#include "fence_checker.h"

#include <cstdlib>
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

// 帧尺寸 1920x1080。围栏顶点是**归一化内容坐标**（0..kFenceNormBase），横轴按帧宽、
// 纵轴按帧高各除各的，所以本文件里出现的坐标一律是"百分比 × 100"：
//   2000 = 20%，落在 1920 宽上是第 384 个像素、落在 960 宽上是第 192 个。
// 判定端不再收 overlay/widget 尺寸（旧实现收，且按它反算 letterbox，换视图即失配），
// 所以这里没有 kOvW/kOvH —— 少两个参数就是这次改动本身。
static constexpr int kSrcW = 1920, kSrcH = 1080;
static constexpr int kModel = 640;   // 传进传出都不参与计算（框已是原始空间坐标）

static void check(bool ok, const char *message)
{
    if (!ok) {
        throw std::runtime_error(message);
    }
}

/// 一个"站立的框"：脚点 = ((left+right)/2, bottom)。宽 100 高 200（帧像素）。
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

/// 让脚点落在**归一化**坐标 (normX, normY)：先还原成帧像素，再按框尺寸摆。
/// 帧像素 = norm * src / 10000，所以同一组入参在 1920x1080 与 640x480 的帧里
/// 指的是同一个相对位置 —— 这正是归一化要买的东西。
static object_detect_result footAtNorm(int normX, int normY)
{
    const int footFrameX = geofence::pixelFromNorm(normX, 0, kSrcW);
    const int footFrameY = geofence::pixelFromNorm(normY, 0, kSrcH);
    return makeBox(footFrameX - 50, footFrameY - 200, footFrameX + 50, footFrameY);
}

/// 归一化空间里的禁区/岗区域矩形
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
    return FenceChecker::checkDetection(det, kSrcW, kSrcH, kModel, kModel, cf);
}

int main()
try {
    // ========================================================================
    // 1) 禁区优先：两种条件同时成立时只出闯入
    // ========================================================================
    {
        // 禁区 (20%,18%)-(40%,37%)；岗区域 (62%,18%)-(72%,37%)
        // 脚点 (30%,27%) → 在禁区里、在岗区域外，两个条件都满足
        const ChannelFence cf = fence({
            rect(FenceRole::Forbidden, 2000, 1800, 4000, 3700),
            rect(FenceRole::Workzone, 6200, 1800, 7200, 3700),
        });
        const FenceViolation v = verdictOf(footAtNorm(3000, 2700), cf);
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
            rect(FenceRole::Forbidden, 2000, 1800, 4000, 3700),
        });
        const FenceViolation v = verdictOf(footAtNorm(8300, 7400), onlyForbidden);
        check(v.verdict == FenceVerdict::None,
              "只有禁区、人在两处之外时必须是 None，绝不能判离岗");

        // 一个禁区都没有、只有岗区域时，人在区域内也必须是 None
        const ChannelFence onlyWorkzone = fence({
            rect(FenceRole::Workzone, 1000, 1800, 4000, 7400),
        });
        check(verdictOf(footAtNorm(2500, 4600), onlyWorkzone).verdict == FenceVerdict::None,
              "人在岗区域内必须是 None");
    }

    // ========================================================================
    // 3) 人在任一岗区域内就不算离岗（"在所有岗区域之外"，非逐区域判定）
    // ========================================================================
    {
        const ChannelFence cf = fence({
            rect(FenceRole::Workzone, 1000, 1800, 2000, 3700),   // 岗区域 A：人在这里
            rect(FenceRole::Workzone, 6200, 1800, 7200, 3700),   // 岗区域 B：人不在这里
        });
        const FenceViolation v = verdictOf(footAtNorm(1500, 2700), cf);
        check(v.verdict == FenceVerdict::None,
              "人站在 A 岗区域里，仅因为不在 B 里就判离岗 —— 归并方向写反了");
    }

    // ========================================================================
    // 4) 只有岗区域、人在外 → Absence 且 shapeIndex=-1
    // ========================================================================
    {
        const ChannelFence cf = fence({
            rect(FenceRole::Workzone, 1000, 1800, 2000, 3700),
            rect(FenceRole::Workzone, 3100, 1800, 4100, 3700),
        });
        const FenceViolation v = verdictOf(footAtNorm(8300, 7400), cf);
        check(v.verdict == FenceVerdict::Absence, "人在两个岗区域之外应判离岗");
        check(v.role == FenceRole::Workzone, "离岗的 role 必须是 Workzone");
        check(v.shapeIndex == -1,
              "离岗是对整张形状表的负向推断，没有单一责任形状，必须是 -1");

        // 禁区 + 岗区域混合，人两处都不在 → 离岗（禁区的 shapeIndex 不该被带出来）
        const ChannelFence mixed = fence({
            rect(FenceRole::Forbidden, 1000, 1800, 2000, 3700),
            rect(FenceRole::Workzone, 3100, 1800, 4100, 3700),
        });
        const FenceViolation mv = verdictOf(footAtNorm(8300, 7400), mixed);
        check(mv.verdict == FenceVerdict::Absence && mv.shapeIndex == -1,
              "混合角色下人在禁区外且在岗区域外，应判离岗且无责任形状");
    }

    // ========================================================================
    // 5) 多个禁区时 shapeIndex 指向真正命中的那个
    // ========================================================================
    {
        const ChannelFence cf = fence({
            rect(FenceRole::Forbidden, 1000, 1800, 2000, 3700),
            rect(FenceRole::Forbidden, 3100, 1800, 4100, 3700),
            rect(FenceRole::Forbidden, 5200, 1800, 6200, 3700),
        });
        const FenceViolation v = verdictOf(footAtNorm(5700, 2700), cf);
        check(v.verdict == FenceVerdict::Intrusion && v.shapeIndex == 2,
              "shapeIndex 必须是实际命中的第 3 个禁区，不是 0 也不是 -1");
    }

    // ========================================================================
    // 6) 通道未启用 / 无形状 → None
    // ========================================================================
    {
        const ChannelFence off = fence({
            rect(FenceRole::Forbidden, 0, 0, 9400, 9300),   // 覆盖全画面，启用时必报
        }, /*enabled=*/false);
        check(verdictOf(footAtNorm(4700, 4700), off).verdict == FenceVerdict::None,
              "通道 enabled=false 时一个形状都不该参与判定");

        const ChannelFence emptyShapes = fence({});
        check(verdictOf(footAtNorm(4700, 4700), emptyShapes).verdict == FenceVerdict::None,
              "没有任何形状时必须是 None（尤其不能判离岗）");
    }

    // ========================================================================
    // 7) 凹多边形命中逐位不变（射线法 + 归一化换算的回归保护）
    // ========================================================================
    // U 形岗区域：归一化 (1000,900)-(4000,2800) 挖掉中间 (2000,900)-(3000,2300) 的凹槽。
    // 顶点顺序：外框左下起逆时针，凹槽在中间向上开口。
    {
        const std::vector<Point> uShape = {
            {1000, 2800}, {1000, 900}, {2000, 900}, {2000, 2300},
            {3000, 2300}, {3000, 900}, {4000, 900}, {4000, 2800}
        };
        const ChannelFence cf = fence({ poly(FenceRole::Workzone, uShape) });

        // 凹槽正中 (2500,1400)：射线法判为外部 → 人在"所有岗区域之外" → 离岗
        check(verdictOf(footAtNorm(2500, 1400), cf).verdict == FenceVerdict::Absence,
              "U 形凹槽里不该算在岗区域内（凹多边形被当成凸包就会漏报离岗）");
        // 左臂内 (1500,1900)：在岗区域内 → None
        check(verdictOf(footAtNorm(1500, 1900), cf).verdict == FenceVerdict::None,
              "U 形左臂内是在岗");
        // 右臂内 (3500,1900)：同上
        check(verdictOf(footAtNorm(3500, 1900), cf).verdict == FenceVerdict::None,
              "U 形右臂内是在岗");
        // 外接矩形右下角之外 (4500,3000)：离岗（确认没有用 toRect 兜底多边形）
        check(verdictOf(footAtNorm(4500, 3000), cf).verdict == FenceVerdict::Absence,
              "多边形不能用外接矩形判定");

        // 同一形状换成禁区角色：凹槽外、臂内 → 闯入，且顶点数足够走 polygon 支路
        const ChannelFence cfForbidden = fence({ poly(FenceRole::Forbidden, uShape) });
        const FenceViolation v = verdictOf(footAtNorm(1500, 1900), cfForbidden);
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

    // ========================================================================
    // 9) 视图尺寸无关性：归一化换算与画布大小无关（N1 的回归锁）
    // ========================================================================
    // 旧实现把围栏顶点存成 overlay widget 像素，判定端按"绘制时记录的 overlay_w/h"
    // 反算 letterbox —— 于是 2x2↔单路放大、窗口缩放、全屏都会让围栏改语义。
    // 现在顶点是归一化值，本组钉三条：
    //   a) 逐像素扫描：像素→归一化→像素必须回到同一像素（画出来的就是判出来的）
    //   b) 代表值往返：归一化→像素→归一化的偏差不得超过"一个像素值多少归一化单位"，
    //      并且这块画布换成另一块尺寸的画布，偏差同阶 ⇒ 换视图只是换落点，不改语义
    //   c) 换算必须带内容矩形原点偏移（黑边里的围栏要对得上判定端）
    // 三块画布取板端真实出现过的尺寸：2x2 的一格 786x521、全屏单路 1864x1114，
    // 再加一块横纵比相反的，钉住"两轴各除各的、与宽高比无关"。
    {
        struct Canvas { const char *name; int w; int h; };
        const Canvas canvases[] = {
            {"2x2 格 786x521", 786, 521},
            {"全屏单路 1864x1114", 1864, 1114},
            {"竖屏 480x640（横纵比相反）", 480, 640},
        };
        // 一组肉眼可读的归一化值（含两端边界，以及画到画布外的负值/超界值）
        const int probes[] = {0, 1500, 2700, 5000, 9999, geofence::kFenceNormBase, -800, 11200};

        for (const auto &cv : canvases) {
            // a) 逐像素：extent 内每个像素都必须原样回来。这条是精确判据，
            //    四舍五入了半个单位、少加了 origin，都会在这里被抓到。
            for (int px = 0; px < cv.w; ++px) {
                const int norm = geofence::normFromPixel(px, 0, cv.w);
                if (geofence::pixelFromNorm(norm, 0, cv.w) != px) {
                    throw std::runtime_error(
                        std::string("像素→归一化→像素在画布 ") + cv.name +
                        " 的横轴 x=" + std::to_string(px) + " 上不闭环（画出来≠判出来）");
                }
            }
            for (int py = 0; py < cv.h; ++py) {
                const int norm = geofence::normFromPixel(py, 0, cv.h);
                if (geofence::pixelFromNorm(norm, 0, cv.h) != py) {
                    throw std::runtime_error(
                        std::string("像素→归一化→像素在画布 ") + cv.name +
                        " 的纵轴 y=" + std::to_string(py) + " 上不闭环（画出来≠判出来）");
                }
            }

            // b) 反方向：归一化值先落到像素再取回，只能差一个像素以内的量化误差。
            //    注意容差是"这个画布的一个像素"，不是固定的 1 个归一化单位 ——
            //    1920 宽画布上一个像素值 5.2 单位、786 宽值 12.7 单位，写死 1 会误报。
            const int tolX = geofence::kFenceNormBase / cv.w + 1;
            const int tolY = geofence::kFenceNormBase / cv.h + 1;
            for (int norm : probes) {
                const int backX = geofence::normFromPixel(
                    geofence::pixelFromNorm(norm, 0, cv.w), 0, cv.w);
                const int backY = geofence::normFromPixel(
                    geofence::pixelFromNorm(norm, 0, cv.h), 0, cv.h);
                if (std::abs(backX - norm) > tolX || std::abs(backY - norm) > tolY) {
                    throw std::runtime_error(
                        std::string("归一化→像素→归一化在画布 ") + cv.name +
                        " 上偏差超过一个像素，换视图必然导致围栏错位");
                }
            }
        }

        // c) 内容矩形有原点偏移：1864x1114 的 widget 装 16:9 内容 → 内容块约
        //    1864x1048、上下各留 ~33px 黑边。围栏画在黑边里也要自洽（所见=所判）。
        const int originY = 33, extentY = 1048;
        const int mid = geofence::pixelFromNorm(5000, originY, extentY);
        check(mid == originY + extentY / 2,
              "pixelFromNorm 没按内容矩形原点偏移：围栏会整体偏向 widget 左上");
        check(geofence::normFromPixel(mid, originY, extentY) == 5000,
              "带偏移的往返换算不自洽（黑边里的围栏会对不上判定端）");

        // 脚点归一化与判定同口径：同一个帧坐标，checker 判命中 = 该点的归一化值落在围栏里
        const ChannelFence cf = fence({
            rect(FenceRole::Forbidden, 4000, 4000, 6000, 6000),
        });
        check(verdictOf(footAtNorm(5000, 5000), cf).verdict == FenceVerdict::Intrusion,
              "帧→归一化的口径与围栏不同源：画面正中央的目标判不进正中央的禁区");
        // 帧尺寸换一半（720p 流的语义等价物），同一个相对位置仍命中：
        // 脚点落在帧的 (450,270)/960x540 = 归一化 (4688,5000)，仍在 (4000..6000)² 里
        {
            object_detect_result det = makeBox(400, 70, 500, 270);
            const FenceViolation v = FenceChecker::checkDetection(
                det, 960, 540, kModel, kModel, cf);
            check(v.verdict == FenceVerdict::Intrusion,
                  "换帧尺寸后同一相对位置必须同样命中（判定只依赖帧尺寸，不依赖 widget）");
        }

        // extent<=0 是"画布还没布局好"的退化输入，必须给 0 而不是除零/乱值
        check(geofence::normFromPixel(123, 0, 0) == 0 &&
              geofence::normFromPixel(123, 0, -5) == 0,
              "画布尺寸为 0 时换算不该猜值或崩溃");
    }

    std::cout << "fence role contracts passed: forbidden-over-absence / no-workzone-never-absent"
                 " / inside-any-workzone / absence-index-minus-one / shapeIndex targeting"
                 " / disabled-and-empty / concave polygon / role string round-trip"
                 " / view-size-independent\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
