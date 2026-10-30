// ============================================================================
// fence_checker.cpp - 电子围栏检测判断实现
// ============================================================================

#include "fence_checker.h"
#include <algorithm>

namespace geofence {

// ============================================================================
// pointInRect - 点是否在矩形内
// ============================================================================
// 直接比较点坐标与矩形边界
// ============================================================================
bool FenceChecker::pointInRect(int x, int y, const Rectangle &rect) {
    return x >= rect.left && x <= rect.right &&
           y >= rect.top && y <= rect.bottom;
}

// ============================================================================
// pointInPolygon - 点是否在多边形内（射线法）
// ============================================================================
// 算法步骤：
//   1. 从测试点向右发射一条水平射线
//   2. 统计射线与多边形各边的交点数量
//   3. 交点为奇数 → 点在内部；偶数 → 点在外部
//
// 判断射线与边相交的条件：
//   - 边的两个端点在射线两侧（一个在上，一个在下）
//   - 交点的 x 坐标大于测试点的 x 坐标（交点在射线方向上）
// ============================================================================
bool FenceChecker::pointInPolygon(int x, int y, const std::vector<Point> &polygon) {
    int n = static_cast<int>(polygon.size());
    if (n < 3) return false;

    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        int xi = polygon[i].x, yi = polygon[i].y;
        int xj = polygon[j].x, yj = polygon[j].y;

        // 判断射线是否与边 (i→j) 相交
        if (((yi > y) != (yj > y)) &&
            (x < (xj - xi) * (y - yi) / (double)(yj - yi) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

// ============================================================================
// pointInShape - 点是否在形状内（自动分派）
// ============================================================================
bool FenceChecker::pointInShape(int x, int y, const FenceShape &shape) {
    if (shape.empty()) return false;

    if (shape.type == ShapeType::Rectangle) {
        Rectangle rect = shape.toRect();
        return pointInRect(x, y, rect);
    } else {
        return pointInPolygon(x, y, shape.points);
    }
}

// ============================================================================
// mapBoxToOriginal - 检测框坐标映射（兼容旧接口）
// ============================================================================
// 检测结果的 box 坐标已经在原始视频空间（RKNN 后处理已做逆 letterbox 变换）。
// 此函数直接返回原始坐标，不做额外缩放。
// ============================================================================
void FenceChecker::mapBoxToOriginal(const image_rect_t &box,
                                    int srcW, int srcH,
                                    int modelW, int modelH,
                                    int &outLeft, int &outTop,
                                    int &outRight, int &outBottom) {
    (void)srcW; (void)srcH; (void)modelW; (void)modelH;
    outLeft   = box.left;
    outTop    = box.top;
    outRight  = box.right;
    outBottom = box.bottom;
}

// ============================================================================
// isInsideFence - 检测目标是否在围栏内
// ============================================================================
// 步骤：
//   1. 将检测框从模型空间映射到原始视频空间 (srcW x srcH)
//   2. 取检测框底边中点（代表"脚"的位置）—— 在帧坐标系中
//   3. 把脚点归一化到 0..kFenceNormBase（横轴除 srcW、纵轴除 srcH）
//   4. 与同样是归一化坐标的围栏顶点比 —— 纯整数比较，不做任何投影
//
// 为什么用底边中点？
//   围栏判断的是"人站在哪里"，脚的位置最准确。
//   用中心点可能因为人举手等原因误判。
//
// 为什么被变换的是脚点而不是围栏？
//   点在形状内的判定在仿射变换下保持不变，所以只要把两边放进同一个空间就行。
//   选"归一化的视频内容空间"作公共空间，判定端就**完全不需要知道 widget 几何**
//   （不需要 overlay 尺寸、不需要 letterbox 偏移），于是 2x2/单路放大/全屏/拖窗口
//   都不会改变围栏语义 —— 这正是旧实现（围栏存 widget 像素）失配的地方。
// ============================================================================
bool FenceChecker::isInsideFence(const object_detect_result &det,
                                 int srcW, int srcH,
                                 int modelW, int modelH,
                                 const FenceShape &fence) {
    // 步骤 1: 映射检测框到原始视频空间
    int left, top, right, bottom;
    mapBoxToOriginal(det.box, srcW, srcH, modelW, modelH,
                     left, top, right, bottom);

    // 步骤 2+3: 脚点（帧坐标）→ 归一化。srcW/srcH <=0 时 normFromPixel 给 0，
    // 等价于"这一帧没有可信的画面尺寸，判定不参与"，不会误报成命中。
    const int footX = (left + right) / 2;
    const int footY = bottom;
    const int normX = normFromPixel(footX, 0, srcW);
    const int normY = normFromPixel(footY, 0, srcH);

    // 步骤 4: 矩形/多边形各自的分派仍走 pointInShape（内部 toRect/射线法，一行未改）
    return pointInShape(normX, normY, fence);
}

// ============================================================================
// checkDetection - 综合判断检测目标违反了哪种围栏规则
// ============================================================================
// 逐形状判定，按角色分两条归并（语义与裁决顺序见 fence_checker.h 的注释）：
//   禁区(Forbidden) —— 脚点在任一个之内即闯入：OR 归并，取下标最小的命中者
//   岗区域(Workzone) —— 脚点在所有岗区域之外才离岗：AND 归并，无责任形状
// 两条规则不是互补关系，所以必须分开扫；旧实现把整张表 OR 成一个 insideAny
// 再按全局 mode 取反，于是"这一路既要禁进 A 区又要守在 B 区"根本表达不出来。
//
// 禁区优先：命中禁区就直接返回，不再判岗区域。一人一帧只出一条报警。
// ============================================================================
FenceViolation FenceChecker::checkDetection(const object_detect_result &det,
                                            int srcW, int srcH,
                                            int modelW, int modelH,
                                            const ChannelFence &channelFence) {
    FenceViolation out;

    // 围栏未启用或没有形状，不做围栏判定
    if (!channelFence.enabled || channelFence.shapes.empty()) {
        return out;   // verdict == None
    }

    bool hasWorkzone = false;       // 本通道到底有没有岗区域（决定离岗规则是否成立）
    bool insideAnyWorkzone = false;

    for (size_t i = 0; i < channelFence.shapes.size(); ++i) {
        const FenceShape &shape = channelFence.shapes[i];
        const bool inside = isInsideFence(det, srcW, srcH, modelW, modelH, shape);
        if (shape.role == FenceRole::Forbidden) {
            if (inside) {
                out.verdict = FenceVerdict::Intrusion;
                out.role = FenceRole::Forbidden;
                out.shapeIndex = static_cast<int>(i);
                return out;         // 禁区优先，岗区域不再看
            }
        } else {
            hasWorkzone = true;
            if (inside) insideAnyWorkzone = true;
        }
    }

    // 只在"确实画了岗区域"时才谈离岗：一条纯禁区通道会把每个检到的人都判成
    // 离岗（hasWorkzone=false 时 !insideAnyWorkzone 恒成立），那是最糟的误报。
    if (hasWorkzone && !insideAnyWorkzone) {
        out.verdict = FenceVerdict::Absence;
        out.role = FenceRole::Workzone;
        out.shapeIndex = -1;
    }
    return out;
}

} // namespace geofence
