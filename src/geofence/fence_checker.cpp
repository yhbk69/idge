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
// mapBoxToOriginal - 检测框坐标映射
// ============================================================================
// 将检测框从模型输入空间 (modelW x modelH) 线性映射到原始视频空间 (srcW x srcH)
//
// 例如：模型输入 640x640，视频 1920x1080
//   scaleX = 1920/640 = 3.0
//   scaleY = 1080/640 = 1.6875
//   模型坐标 (100, 100, 200, 200) → 视频坐标 (300, 168, 600, 337)
// ============================================================================
void FenceChecker::mapBoxToOriginal(const image_rect_t &box,
                                    int srcW, int srcH,
                                    int modelW, int modelH,
                                    int &outLeft, int &outTop,
                                    int &outRight, int &outBottom) {
    float scaleX = static_cast<float>(srcW) / modelW;
    float scaleY = static_cast<float>(srcH) / modelH;

    outLeft   = static_cast<int>(box.left * scaleX);
    outTop    = static_cast<int>(box.top * scaleY);
    outRight  = static_cast<int>(box.right * scaleX);
    outBottom = static_cast<int>(box.bottom * scaleY);
}

// ============================================================================
// isInsideFence - 检测目标是否在围栏内
// ============================================================================
// 步骤：
//   1. 将检测框从模型空间映射到原始视频空间
//   2. 取检测框底边中点（代表"脚"的位置）
//   3. 将围栏坐标从 overlay 空间映射到原始视频空间
//   4. 判断脚部点是否在映射后的围栏内
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

    // 步骤 2: 取底边中点（脚部位置）
    int footX = (left + right) / 2;
    int footY = bottom;

    // 如果没有 overlay 尺寸信息，直接用原始坐标比较
    if (overlayW <= 0 || overlayH <= 0) {
        return pointInShape(footX, footY, fence);
    }

    // 步骤 3: 将围栏坐标从 overlay 空间映射到原始视频空间
    //   用户在 overlay widget 上绘制围栏，坐标是 widget 像素
    //   需要按比例缩放到原始视频分辨率
    float fenceScaleX = static_cast<float>(srcW) / overlayW;
    float fenceScaleY = static_cast<float>(srcH) / overlayH;

    if (fence.type == ShapeType::Rectangle && fence.points.size() >= 2) {
        // 矩形：映射两个对角点
        int fx1 = static_cast<int>(fence.points[0].x * fenceScaleX);
        int fy1 = static_cast<int>(fence.points[0].y * fenceScaleY);
        int fx2 = static_cast<int>(fence.points[1].x * fenceScaleX);
        int fy2 = static_cast<int>(fence.points[1].y * fenceScaleY);
        int rx1 = std::min(fx1, fx2);
        int ry1 = std::min(fy1, fy2);
        int rx2 = std::max(fx1, fx2);
        int ry2 = std::max(fy1, fy2);
        Rectangle rect;
        rect.left = rx1;
        rect.top = ry1;
        rect.right = rx2;
        rect.bottom = ry2;
        return pointInRect(footX, footY, rect);
    } else {
        // 多边形：逐点映射
        std::vector<Point> scaled;
        scaled.reserve(fence.points.size());
        for (const auto &p : fence.points) {
            scaled.push_back(Point(static_cast<int>(p.x * fenceScaleX),
                                   static_cast<int>(p.y * fenceScaleY)));
        }
        return pointInPolygon(footX, footY, scaled);
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
