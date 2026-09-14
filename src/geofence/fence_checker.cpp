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
//   3. 将脚部坐标从帧坐标系映射到 overlay widget 坐标系
//      （考虑 GLVideoWidget 的 letterbox 宽高比保持）
//   4. 判断脚部点是否在围栏内（围栏坐标本身就在 overlay widget 坐标系）
//
// 为什么用底边中点？
//   围栏判断的是"人站在哪里"，脚的位置最准确。
//   用中心点可能因为人举手等原因误判。
// ============================================================================
bool FenceChecker::isInsideFence(const object_detect_result &det,
                                 int srcW, int srcH,
                                 int modelW, int modelH,
                                 int overlayW, int overlayH,
                                 const FenceShape &fence) {
    // 步骤 1: 映射检测框到原始视频空间
    int left, top, right, bottom;
    mapBoxToOriginal(det.box, srcW, srcH, modelW, modelH,
                     left, top, right, bottom);

    // 步骤 2: 取底边中点（脚部位置）—— 帧坐标系
    int footX = (left + right) / 2;
    int footY = bottom;

    // 如果没有 overlay 尺寸信息，直接用原始坐标比较
    if (overlayW <= 0 || overlayH <= 0) {
        return pointInShape(footX, footY, fence);
    }

    // 步骤 3: 将脚部坐标从帧坐标系映射到 overlay widget 坐标系
    //   GLVideoWidget 保持宽高比渲染，视频内容可能不占满整个 widget（有黑边）
    //   需要考虑 letterbox 偏移，才能将帧坐标正确映射到 widget 坐标
    float videoAspect = static_cast<float>(srcW) / srcH;
    float viewAspect  = static_cast<float>(overlayW) / overlayH;

    float renderW, renderH, xOffset, yOffset;
    if (videoAspect > viewAspect) {
        // 视频更宽 → 左右满，上下黑边
        renderW = static_cast<float>(overlayW);
        renderH = overlayW * srcH / static_cast<float>(srcW);
        xOffset = 0.0f;
        yOffset = (overlayH - renderH) / 2.0f;
    } else {
        // 视频更高 → 上下满，左右黑边
        renderH = static_cast<float>(overlayH);
        renderW = overlayH * srcW / static_cast<float>(srcH);
        xOffset = (overlayW - renderW) / 2.0f;
        yOffset = 0.0f;
    }

    // 帧坐标 → widget 坐标
    float widgetX = footX * renderW / srcW + xOffset;
    float widgetY = footY * renderH / srcH + yOffset;

    // 步骤 4: 判断 widget 坐标是否在围栏内
    if (fence.type == ShapeType::Rectangle && fence.points.size() >= 2) {
        int fx1 = fence.points[0].x;
        int fy1 = fence.points[0].y;
        int fx2 = fence.points[1].x;
        int fy2 = fence.points[1].y;
        int rx1 = std::min(fx1, fx2);
        int ry1 = std::min(fy1, fy2);
        int rx2 = std::max(fx1, fx2);
        int ry2 = std::max(fy1, fy2);
        Rectangle rect;
        rect.left = rx1;
        rect.top = ry1;
        rect.right = rx2;
        rect.bottom = ry2;
        return pointInRect(static_cast<int>(widgetX), static_cast<int>(widgetY), rect);
    } else {
        // 多边形直接用 overlay 坐标比较（围栏坐标已在 widget 空间）
        return pointInPolygon(static_cast<int>(widgetX), static_cast<int>(widgetY), fence.points);
    }
}

// ============================================================================
// checkDetection - 综合判断检测目标是否违反围栏规则
// ============================================================================
// 遍历该通道所有围栏形状，只要目标在任一围栏内即判定为 inside。
//
// alarmInside:
//   true  → 目标在围栏内需要报警（默认模式，检测人员闯入禁区）
//   false → 目标在围栏外需要报警（检测人员离开安全区域）
//
// 返回 true 表示该检测结果应该触发报警。
// ============================================================================
bool FenceChecker::checkDetection(const object_detect_result &det,
                                  int srcW, int srcH,
                                  int modelW, int modelH,
                                  int overlayW, int overlayH,
                                  const ChannelFence &channelFence,
                                  bool alarmInside) {
    // 围栏未启用或没有形状，不做围栏过滤
    if (!channelFence.enabled || channelFence.shapes.empty()) {
        return false;
    }

    // 检查目标是否在任一围栏内
    bool insideAny = false;
    for (const auto &shape : channelFence.shapes) {
        if (isInsideFence(det, srcW, srcH, modelW, modelH,
                          overlayW, overlayH, shape)) {
            insideAny = true;
            break;
        }
    }

    // 根据模式决定是否报警
    return alarmInside ? insideAny : !insideAny;
}

} // namespace geofence
