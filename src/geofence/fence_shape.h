#pragma once

// ============================================================================
// fence_shape.h - 电子围栏形状定义
// ============================================================================
//
// 定义围栏的基本数据结构：
//   - Point:          二维坐标点
//   - Rectangle:      矩形（左上角 + 右下角）
//   - FenceShape:     围栏形状（矩形或多边形，包含一组顶点）
//   - ChannelFence:   单通道的围栏配置（是否启用 + 形状列表）
//
// 坐标空间：
//   围栏坐标以 overlay widget 的像素为单位。
//   在做点命中判断时，需要先将围栏坐标映射到原始视频分辨率空间。
//
// ============================================================================

#include <vector>
#include <utility>

namespace geofence {

// ============================================================================
// Point - 二维坐标点
// ============================================================================
struct Point {
    int x = 0;
    int y = 0;
    Point() = default;
    Point(int x, int y) : x(x), y(y) {}
};

// ============================================================================
// Rectangle - 矩形（用两个对角点表示）
// ============================================================================
// left/top: 左上角坐标
// right/bottom: 右下角坐标
// ============================================================================
struct Rectangle {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    bool empty() const { return left == right && top == bottom; }
};

// ============================================================================
// ShapeType - 围栏形状类型
// ============================================================================
enum class ShapeType {
    Rectangle,  // 矩形：只需 2 个顶点（对角点）
    Polygon     // 多边形：3 个及以上顶点
};

// ============================================================================
// FenceShape - 围栏形状
// ============================================================================
// type:    形状类型（矩形或多边形）
// points:  顶点列表
//   - 矩形：2 个点，分别是左上角和右下角
//   - 多边形：按顺序排列的顶点，首尾自动闭合
//
// toRect(): 将任意形状的顶点列表计算为外接矩形（用于矩形快速判断）
// ============================================================================
struct FenceShape {
    ShapeType type = ShapeType::Rectangle;
    std::vector<Point> points;

    bool empty() const { return points.empty(); }
    void clear() { points.clear(); }

    // 计算所有顶点的外接矩形
    Rectangle toRect() const {
        if (points.size() < 2) return {};
        int minX = points[0].x, minY = points[0].y;
        int maxX = points[0].x, maxY = points[0].y;
        for (size_t i = 1; i < points.size(); ++i) {
            if (points[i].x < minX) minX = points[i].x;
            if (points[i].y < minY) minY = points[i].y;
            if (points[i].x > maxX) maxX = points[i].x;
            if (points[i].y > maxY) maxY = points[i].y;
        }
        return {minX, minY, maxX, maxY};
    }
};

// ============================================================================
// ChannelFence - 单通道围栏配置
// ============================================================================
// enabled: 该通道是否启用围栏检测
// shapes:  该通道的所有围栏形状（可以有多个）
// ============================================================================
struct ChannelFence {
    bool enabled = false;
    std::vector<FenceShape> shapes;
};

} // namespace geofence
