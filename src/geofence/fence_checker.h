#pragma once

// ============================================================================
// fence_checker.h - 电子围栏检测判断
// ============================================================================
//
// 核心功能：
//   1. 判断一个点是否在围栏形状内（射线法）
//   2. 将检测框从模型空间（640x640）映射到原始视频空间
//   3. 将检测框脚点（帧坐标）映射到 overlay 空间，与围栏坐标（widget 像素系）比较
//   4. 综合判断检测目标是否违反围栏规则
//
// 坐标空间说明：
//   模型空间:     YOLO 推理输出的坐标，固定 640x640
//   原始视频空间: 解码后的视频帧分辨率（如 1920x1080）
//   overlay 空间: FenceOverlay widget 的像素坐标（用户绘制围栏时使用）
//
// 判断逻辑：
//   取检测框的"底边中点"（脚部位置），判断该点是否在围栏内。
//   - inside_alarm 模式: 目标在围栏内 → 报警
//   - outside_alarm 模式: 目标在围栏外 → 报警
//
// ============================================================================

#include "fence_shape.h"
#include "../yolo11/common.hpp"

namespace geofence {

class FenceChecker {
public:
    // ========================================================================
    // 点是否在矩形内
    // ========================================================================
    static bool pointInRect(int x, int y, const Rectangle &rect);

    // ========================================================================
    // 点是否在多边形内（射线法 / crossing number）
    // 原理：从测试点向右发射水平射线，统计与多边形各边的穿越次数，
    //       奇数=内部，偶数=外部。时间复杂度 O(n)，n 为顶点数。
    // 特性：天然支持凹多边形（凹口处穿越次数自动抵消，不会误判）。
    // 边界：射线判定条件用 (yi>y)!=(yj>y) 的半开区间写法规避顶点重复计数；
    //       落在边/顶点上的点属于退化情形，结果不保证严格一致（对像素级围栏可接受）。
    // ========================================================================
    static bool pointInPolygon(int x, int y, const std::vector<Point> &polygon);

    // ========================================================================
    // 点是否在形状内（自动判断矩形/多边形）
    // ========================================================================
    static bool pointInShape(int x, int y, const FenceShape &shape);

    // ========================================================================
    // 将检测框从模型空间映射到原始视频空间
    // box:        模型输出的检测框（640x640）
    // srcW/srcH:  原始视频分辨率
    // modelW/H:   模型输入尺寸（通常 640x640）
    // ========================================================================
    static void mapBoxToOriginal(const image_rect_t &box,
                                 int srcW, int srcH,
                                 int modelW, int modelH,
                                 int &outLeft, int &outTop,
                                 int &outRight, int &outBottom);

    // ========================================================================
    // 判断单个检测目标是否在围栏内
    // 算法：取检测框"底边中点"作为脚点（帧坐标），按 letterbox 规则映射到
    //       overlay/widget 空间，再与本身就是 widget 坐标的围栏比较。
    //       注意变换方向是"帧→widget"，不是把围栏变换到视频空间。
    //
    // overlayW/H: overlay widget 尺寸，用于确定帧坐标到 widget 坐标的缩放比例
    //             与黑边（letterbox）偏移；<=0 时退化为直接用帧坐标与围栏比较。
    // ========================================================================
    static bool isInsideFence(const object_detect_result &det,
                              int srcW, int srcH,
                              int modelW, int modelH,
                              int overlayW, int overlayH,
                              const FenceShape &fence);

    // ========================================================================
    // 综合判断：检测目标是否违反围栏规则
    // alarmInside=true:  目标在围栏内 → 返回 true（需要报警）
    // alarmInside=false: 目标在围栏外 → 返回 true（需要报警）
    // ========================================================================
    static bool checkDetection(const object_detect_result &det,
                               int srcW, int srcH,
                               int modelW, int modelH,
                               int overlayW, int overlayH,
                               const ChannelFence &channelFence,
                               bool alarmInside);
};

} // namespace geofence
