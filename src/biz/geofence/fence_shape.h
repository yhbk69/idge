#pragma once

// ============================================================================
// fence_shape.h - 电子围栏形状定义
// ============================================================================
//
// 定义围栏的基本数据结构：
//   - Point:          二维坐标点（归一化到视频内容空间，见下方坐标空间约定）
//   - kFenceNormBase: 归一化基准（10000），配 normFromPixel/pixelFromNorm 两个互转
//   - Rectangle:      矩形（左上角 + 右下角）
//   - FenceRole:      围栏形状的角色（禁区 / 岗区域）
//   - FenceShape:     围栏形状（矩形或多边形，包含一组顶点 + 一个角色）
//   - ChannelFence:   单通道的围栏配置（是否启用 + 形状列表）
//
// 坐标空间约定（2026-10-30 起，替代此前的"overlay widget 像素"口径）：
//   围栏顶点存的是**视频内容空间的归一化坐标**，范围 0..kFenceNormBase（=10000，即 0.01%）。
//   横轴按视频帧宽归一、纵轴按帧高归一，两轴各除各的 ⇒ 与宽高比无关。
//
//   判定端（fence_checker）只需要帧尺寸：脚点 norm = footX * 10000 / srcW。
//   绘制端（fence_overlay）只需要内容矩形（GLVideoWidget 按 letterbox 算出的那块）：
//   像素 = contentRect.origin + norm / 10000 * contentRect.size。
//   两侧不再共享任何"画布尺寸"，所以换 2x2/单路放大、换窗口大小、进全屏都不动围栏语义。
//
//   为什么基准是"视频内容"而不是"widget"：widget 里视频上下（或左右）有黑边，黑边不属于
//   画面，围栏画在黑边里没有物理意义；以内容矩形为基准，用户看到的围栏始终贴在画面上，
//   判定也始终落在画面内的目标上。
//
//   允许出现 0..10000 之外的值（用户把围栏拖进黑边）：刻意不 clamp，
//   画出来什么样，判的就是什么样。
//
//   ⚠ 老配置（config.json 里没有 "coord":"norm" 的通道）顶点仍是 widget 像素，
//   由 FenceManager::loadFromConfig 就近换算一次并落盘；那条路径是**近似**，
//   详见 fence_manager.cpp 的迁移注释。
//
// ============================================================================

#include <vector>
#include <utility>
#include <string>

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
// kFenceNormBase / normFromPixel / pixelFromNorm - 归一化坐标与本层画布像素的互转
// ============================================================================
// 围栏顶点存归一化值（见文件头"坐标空间约定"），而"归一化到哪个矩形"只有握着
// GLVideoWidget 内容矩形的绘制端知道，所以这里只给纯算术、不引 Qt：
//   normFromPixel(像素, 该轴内容起点, 该轴内容长度)  → 归一化
//   pixelFromNorm(归一化, 该轴内容起点, 该轴内容长度) → 像素
// 两者互为逆运算（四舍五入误差 ≤1 个单位 = 1920 宽下 0.19px）。
//
// 用 long long 中间量：1920 像素 * 10000 = 1.92e7 还在 int 里，但归一化值可达
// 数万（围栏画到黑边外），乘上 extent 就溢出 int 了。
// ============================================================================
constexpr int kFenceNormBase = 10000;

// 四舍五入的整除（den 必须 > 0）；C++ 的 / 对负数向零截断，直接用会让所有点系统性
// 向原点偏半个单位。
inline int normDivRound(long long num, long long den) {
    if (num >= 0) return static_cast<int>((num + den / 2) / den);
    return -static_cast<int>((-num + den / 2) / den);
}

inline int normFromPixel(int pixel, int origin, int extent) {
    if (extent <= 0) return 0;   // 画布还没布局好（首帧前/尺寸为 0），不猜
    return normDivRound(static_cast<long long>(pixel - origin) * kFenceNormBase, extent);
}

inline int pixelFromNorm(int norm, int origin, int extent) {
    return origin + normDivRound(static_cast<long long>(norm) * extent, kFenceNormBase);
}

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
// Rectangle: 矩形，仅需 2 个顶点，且这两个点是"任意两对角点"——
//            存储顺序不固定（用户可能从右下往左上拖），故命中判断前需归一化。
// Polygon:   多边形，需 3 个及以上顶点；顶点数 < 3 时射线法直接判为"不在内部"
//            （见 FenceChecker::pointInPolygon 的 n<3 提前返回）。
// ============================================================================
enum class ShapeType {
    Rectangle,  // 矩形：只需 2 个顶点（对角点）
    Polygon     // 多边形：3 个及以上顶点
};

// ============================================================================
// FenceRole - 围栏形状的角色
// ============================================================================
// 角色是"形状"级属性而不是"通道"级：同一条通道可以同时有一个禁区和一个岗区域，
// 两条规则对同一张形状表做**不同**的归并（禁区 = 在任一个之内即违规，
// 岗区域 = 在所有岗区域之外才违规），所以必须由每个形状自带，不能由全局开关代管。
// （替代此前那个一按不定、只被解码器读过一次的 geofence.mode 字符串。）
//
// Forbidden: 禁区 —— 目标脚点落在形状内 → 闯入报警。
// Workzone:  岗区域 —— 目标脚点落在所有岗区域之外 → 离岗报警。
//            语义是"人出现在区域外"，不是"区域里没人"（缺人检测本轮不做）。
//
// 默认 Forbidden：与旧 inside_alarm 行为一致，且"配置读不出来就当要报警"
// 是更安全的失效方向（漏报闯入比误报离岗更难被发现）。
// ============================================================================
enum class FenceRole {
    Forbidden,  // 禁区：进去就报
    Workzone    // 岗区域：待在外面就报
};

// ============================================================================
// roleToString / roleFromString - 角色与 config.json 里 "role" 字段的互转
// ============================================================================
// 刻意只用 std::string（不引 Qt）：迁移判定与离线自测都要能脱离 QJson 调用它们。
// fromString 对空串/未知串一律回落 Forbidden —— 缺键、拼错、老配置都得走这条路。
// ============================================================================
inline std::string roleToString(FenceRole role) {
    return role == FenceRole::Workzone ? "workzone" : "forbidden";
}

inline FenceRole roleFromString(const std::string &text) {
    return text == "workzone" ? FenceRole::Workzone : FenceRole::Forbidden;
}

// ============================================================================
// FenceShape - 围栏形状
// ============================================================================
// type:    形状类型（矩形或多边形）—— 决定几何怎么算
// role:    形状角色（禁区 / 岗区域）—— 决定几何结果怎么变成报警，见 FenceRole
// points:  顶点列表
//   - 矩形：2 个点，分别是左上角和右下角
//   - 多边形：按顺序排列的顶点，首尾自动闭合
//
// 角色在形状落盘后**不可改**：删除模式按 vector 下标命中（fence_overlay.cpp 的
// DeleteMode），没有稳定 id，改角色只能删了重画。绘制端用颜色区分（禁区红、
// 岗区域绿），所以"看颜色以为能点着改"是这条约束最容易踩的地方。
//
// toRect():  将任意形状的顶点列表归一化为外接矩形（min/max 扫描，O(n)）。
//            矩形命中判断前先经 toRect() 归一化，从而与顶点的存储顺序无关，
//            即用户从任意方向拖出的对角点都能得到正确的 [left,top,right,bottom]。
// ============================================================================
struct FenceShape {
    ShapeType type = ShapeType::Rectangle;
    FenceRole role = FenceRole::Forbidden;
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
// enabled: 该通道是否启用围栏检测（通道级，混用两种角色时无法只关其中一种）
// shapes:  该通道的所有围栏形状，可混合禁区与岗区域
// ============================================================================
struct ChannelFence {
    bool enabled = false;
    std::vector<FenceShape> shapes;
};

} // namespace geofence
