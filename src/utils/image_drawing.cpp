#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "image_drawing.h"
#include "font.h"

// ============================================================================
// 工具宏定义
// ============================================================================
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) < (b)) ? (a) : (b))

// ============================================================================
// clamp - 将值限制在指定范围内
// ============================================================================
static int clamp(float val, int min, int max)
{
    return val > min ? (val < max ? val : max) : min;
}

// ============================================================================
// convert_color - ARGB8888颜色格式转换
// 将统一的ARGB8888颜色转换为不同像素格式所需的颜色表示
// ============================================================================
static unsigned int convert_color(unsigned int src_color, image_format_t dst_fmt)
{
    unsigned int dst_color = 0x0;
    unsigned char* p_src_color = (unsigned char*)&src_color;
    unsigned char* p_dst_color = (unsigned char*)&dst_color;
    char r = p_src_color[2];
    char g = p_src_color[1];
    char b = p_src_color[0];
    char a = p_src_color[3];

    switch (dst_fmt)
    {
    case IMAGE_FORMAT_GRAY8:
        // 灰度图只使用Alpha通道值
        p_dst_color[0] = a;
        break;
    case IMAGE_FORMAT_RGB888:
        // RGB格式：R-G-B顺序
        p_dst_color[0] = r;
        p_dst_color[1] = g;
        p_dst_color[2] = b;
        break;
    case IMAGE_FORMAT_RGBA8888:
        // RGBA格式：R-G-B-A顺序
        p_dst_color[0] = r;
        p_dst_color[1] = g;
        p_dst_color[2] = b;
        p_dst_color[3] = a;
        break;
    case IMAGE_FORMAT_YUV420SP_NV12:
        // NV12格式YUV转换：使用BT.601标准公式
        // Y = 0.299R + 0.587G + 0.114B
        // U = 128 - 0.14713R - 0.28886G + 0.436B
        // V = 128 + 0.615R - 0.51499G - 0.10001B
        p_dst_color[0] = (0.299 * r + 0.587 * g + 0.114 * b);
        p_dst_color[1] = clamp(128 + (-0.14713 * r - 0.28886 * g + 0.436 * b), 0, 255);
        p_dst_color[2] = clamp(128 + (0.615 * r - 0.51499 * g - 0.10001 * b), 0, 255);
        break;
    case IMAGE_FORMAT_YUV420SP_NV21:
        // NV21格式YUV转换
        p_dst_color[0] = 0.299 * r + 0.587 * g + 0.114 * b;
        p_dst_color[1] = 0.877 * (r - p_dst_color[0]);
        p_dst_color[2] = 0.492 * (b - p_dst_color[0]);
        break;
    default:
        break;
    }
    return dst_color;
}

// ============================================================================
// 矩形绘制实现（按通道数分别实现）
// C1=单通道(灰度), C2=双通道, C3=三通道(RGB), C4=四通道(RGBA)
// ============================================================================

// draw_rectangle_c1 - 单通道矩形绘制
// thickness=-1为填充模式，其他值为边框模式
static void draw_rectangle_c1(unsigned char* pixels, int w, int h, int rx, int ry, int rw, int rh, unsigned int color,
                              int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w;

    if (thickness == -1) {
        // 填充模式：填充整个矩形区域
        for (int y = ry; y < ry + rh; y++) {
            if (y < 0) continue;
            if (y >= h) break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx; x < rx + rw; x++) {
                if (x < 0) continue;
                if (x >= w) break;

                p[x] = pen_color[0];
            }
        }
        return;
    }

    // 边框模式：分别绘制四条边
    const int t0 = thickness / 2;
    const int t1 = thickness - t0;

    // 绘制上边
    for (int y = ry - t0; y < ry + t1; y++) {
        if (y < 0) continue;
        if (y >= h) break;
        unsigned char* p = pixels + stride * y;
        for (int x = rx - t0; x < rx + rw + t1; x++) {
            if (x < 0) continue;
            if (x >= w) break;
            p[x] = pen_color[0];
        }
    }

    // 绘制下边
    for (int y = ry + rh - t0; y < ry + rh + t1; y++) {
        if (y < 0) continue;
        if (y >= h) break;
        unsigned char* p = pixels + stride * y;
        for (int x = rx - t0; x < rx + rw + t1; x++) {
            if (x < 0) continue;
            if (x >= w) break;
            p[x] = pen_color[0];
        }
    }

    // 绘制左边（避开已绘制的上下角）
    for (int x = rx - t0; x < rx + t1; x++) {
        if (x < 0) continue;
        if (x >= w) break;
        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0) continue;
            if (y >= h) break;
            unsigned char* p = pixels + stride * y;
            p[x] = pen_color[0];
        }
    }

    // 绘制右边（避开已绘制的上下角）
    for (int x = rx + rw - t0; x < rx + rw + t1; x++) {
        if (x < 0) continue;
        if (x >= w) break;
        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0) continue;
            if (y >= h) break;
            unsigned char* p = pixels + stride * y;
            p[x] = pen_color[0];
        }
    }
}

// draw_rectangle_c2 - 双通道矩形绘制
static void draw_rectangle_c2(unsigned char* pixels, int w, int h, int rx, int ry, int rw, int rh, unsigned int color,
                              int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 2;

    if (thickness == -1) {
        // 填充模式
        for (int y = ry; y < ry + rh; y++) {
            if (y < 0) continue;
            if (y >= h) break;
            unsigned char* p = pixels + stride * y;
            for (int x = rx; x < rx + rw; x++) {
                if (x < 0) continue;
                if (x >= w) break;
                p[x * 2 + 0] = pen_color[0];
                p[x * 2 + 1] = pen_color[1];
            }
        }
        return;
    }

    // 边框模式：四条边
    const int t0 = thickness / 2;
    const int t1 = thickness - t0;

    // 上边
    for (int y = ry - t0; y < ry + t1; y++) {
        if (y < 0) continue;
        if (y >= h) break;
        unsigned char* p = pixels + stride * y;
        for (int x = rx - t0; x < rx + rw + t1; x++) {
            if (x < 0) continue;
            if (x >= w) break;
            p[x * 2 + 0] = pen_color[0];
            p[x * 2 + 1] = pen_color[1];
        }
    }

    // 下边
    for (int y = ry + rh - t0; y < ry + rh + t1; y++) {
        if (y < 0) continue;
        if (y >= h) break;
        unsigned char* p = pixels + stride * y;
        for (int x = rx - t0; x < rx + rw + t1; x++) {
            if (x < 0) continue;
            if (x >= w) break;
            p[x * 2 + 0] = pen_color[0];
            p[x * 2 + 1] = pen_color[1];
        }
    }

    // 左边
    for (int x = rx - t0; x < rx + t1; x++) {
        if (x < 0) continue;
        if (x >= w) break;
        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0) continue;
            if (y >= h) break;
            unsigned char* p = pixels + stride * y;
            p[x * 2 + 0] = pen_color[0];
            p[x * 2 + 1] = pen_color[1];
        }
    }

    // 右边
    for (int x = rx + rw - t0; x < rx + rw + t1; x++) {
        if (x < 0) continue;
        if (x >= w) break;
        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0) continue;
            if (y >= h) break;
            unsigned char* p = pixels + stride * y;
            p[x * 2 + 0] = pen_color[0];
            p[x * 2 + 1] = pen_color[1];
        }
    }
}

// draw_rectangle_c3 - 三通道(RGB)矩形绘制
static void draw_rectangle_c3(unsigned char* pixels, int w, int h, int rx, int ry, int rw, int rh, unsigned int color,
                              int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 3;

    if (thickness == -1) {
        // 填充模式
        for (int y = ry; y < ry + rh; y++) {
            if (y < 0) continue;
            if (y >= h) break;
            unsigned char* p = pixels + stride * y;
            for (int x = rx; x < rx + rw; x++) {
                if (x < 0) continue;
                if (x >= w) break;
                p[x * 3 + 0] = pen_color[0];
                p[x * 3 + 1] = pen_color[1];
                p[x * 3 + 2] = pen_color[2];
            }
        }
        return;
    }

    // 边框模式
    const int t0 = thickness / 2;
    const int t1 = thickness - t0;

    // 上边
    for (int y = ry - t0; y < ry + t1; y++) {
        if (y < 0) continue;
        if (y >= h) break;
        unsigned char* p = pixels + stride * y;
        for (int x = rx - t0; x < rx + rw + t1; x++) {
            if (x < 0) continue;
            if (x >= w) break;
            p[x * 3 + 0] = pen_color[0];
            p[x * 3 + 1] = pen_color[1];
            p[x * 3 + 2] = pen_color[2];
        }
    }

    // 下边
    for (int y = ry + rh - t0; y < ry + rh + t1; y++) {
        if (y < 0) continue;
        if (y >= h) break;
        unsigned char* p = pixels + stride * y;
        for (int x = rx - t0; x < rx + rw + t1; x++) {
            if (x < 0) continue;
            if (x >= w) break;
            p[x * 3 + 0] = pen_color[0];
            p[x * 3 + 1] = pen_color[1];
            p[x * 3 + 2] = pen_color[2];
        }
    }

    // 左边
    for (int x = rx - t0; x < rx + t1; x++) {
        if (x < 0) continue;
        if (x >= w) break;
        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0) continue;
            if (y >= h) break;
            unsigned char* p = pixels + stride * y;
            p[x * 3 + 0] = pen_color[0];
            p[x * 3 + 1] = pen_color[1];
            p[x * 3 + 2] = pen_color[2];
        }
    }

    // 右边
    for (int x = rx + rw - t0; x < rx + rw + t1; x++) {
        if (x < 0) continue;
        if (x >= w) break;
        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0) continue;
            if (y >= h) break;
            unsigned char* p = pixels + stride * y;
            p[x * 3 + 0] = pen_color[0];
            p[x * 3 + 1] = pen_color[1];
            p[x * 3 + 2] = pen_color[2];
        }
    }
}

// draw_rectangle_c4 - 四通道(RGBA)矩形绘制
static void draw_rectangle_c4(unsigned char* pixels, int w, int h, int rx, int ry, int rw, int rh, unsigned int color,
                              int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 4;

    if (thickness == -1) {
        // 填充模式
        for (int y = ry; y < ry + rh; y++) {
            if (y < 0) continue;
            if (y >= h) break;
            unsigned char* p = pixels + stride * y;
            for (int x = rx; x < rx + rw; x++) {
                if (x < 0) continue;
                if (x >= w) break;
                p[x * 4 + 0] = pen_color[0];
                p[x * 4 + 1] = pen_color[1];
                p[x * 4 + 2] = pen_color[2];
                p[x * 4 + 3] = pen_color[3];
            }
        }
        return;
    }

    // 边框模式
    const int t0 = thickness / 2;
    const int t1 = thickness - t0;

    // 上边
    for (int y = ry - t0; y < ry + t1; y++) {
        if (y < 0) continue;
        if (y >= h) break;
        unsigned char* p = pixels + stride * y;
        for (int x = rx - t0; x < rx + rw + t1; x++) {
            if (x < 0) continue;
            if (x >= w) break;
            p[x * 4 + 0] = pen_color[0];
            p[x * 4 + 1] = pen_color[1];
            p[x * 4 + 2] = pen_color[2];
            p[x * 4 + 3] = pen_color[3];
        }
    }

    // 下边
    for (int y = ry + rh - t0; y < ry + rh + t1; y++) {
        if (y < 0) continue;
        if (y >= h) break;
        unsigned char* p = pixels + stride * y;
        for (int x = rx - t0; x < rx + rw + t1; x++) {
            if (x < 0) continue;
            if (x >= w) break;
            p[x * 4 + 0] = pen_color[0];
            p[x * 4 + 1] = pen_color[1];
            p[x * 4 + 2] = pen_color[2];
            p[x * 4 + 3] = pen_color[3];
        }
    }

    // 左边
    for (int x = rx - t0; x < rx + t1; x++) {
        if (x < 0) continue;
        if (x >= w) break;
        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0) continue;
            if (y >= h) break;
            unsigned char* p = pixels + stride * y;
            p[x * 4 + 0] = pen_color[0];
            p[x * 4 + 1] = pen_color[1];
            p[x * 4 + 2] = pen_color[2];
            p[x * 4 + 3] = pen_color[3];
        }
    }

    // 右边
    for (int x = rx + rw - t0; x < rx + rw + t1; x++) {
        if (x < 0) continue;
        if (x >= w) break;
        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0) continue;
            if (y >= h) break;
            unsigned char* p = pixels + stride * y;
            p[x * 4 + 0] = pen_color[0];
            p[x * 4 + 1] = pen_color[1];
            p[x * 4 + 2] = pen_color[2];
            p[x * 4 + 3] = pen_color[3];
        }
    }
}

// ============================================================================
// draw_rectangle_yuv420sp - YUV420SP格式矩形绘制
// 分别对Y分量和UV分量绘制矩形
// YUV420SP：Y分量全分辨率，UV分量1/2分辨率
// ============================================================================
static void draw_rectangle_yuv420sp(unsigned char* yuv420sp, int w, int h, int rx, int ry, int rw, int rh,
                                    unsigned int color, int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;

    // 分离Y和UV颜色分量
    unsigned int v_y;
    unsigned int v_uv;
    unsigned char* pen_color_y = (unsigned char*)&v_y;
    unsigned char* pen_color_uv = (unsigned char*)&v_uv;
    pen_color_y[0] = pen_color[0];     // Y分量
    pen_color_uv[0] = pen_color[1];    // U分量
    pen_color_uv[1] = pen_color[2];    // V分量

    // 在Y分量上绘制矩形
    unsigned char* Y = yuv420sp;
    draw_rectangle_c1(Y, w, h, rx, ry, rw, rh, v_y, thickness);

    // 在UV分量上绘制矩形（分辨率减半）
    unsigned char* UV = yuv420sp + w * h;
    int thickness_uv = thickness == -1 ? thickness : max(thickness / 2, 1);
    draw_rectangle_c2(UV, w / 2, h / 2, rx / 2, ry / 2, rw / 2, rh / 2, v_uv, thickness_uv);
}

// ============================================================================
// 距离计算辅助函数
// 用于圆形和线段的像素级绘制
// ============================================================================

// distance_lessequal - 判断点到圆心距离是否小于等于半径
static inline int distance_lessequal(int x0, int y0, int x1, int y1, float r)
{
    int dx = x0 - x1;
    int dy = y0 - y1;
    int q = dx * dx + dy * dy;
    return q <= r * r;
}

// distance_inrange - 判断点到圆心距离是否在指定范围内（用于绘制空心圆）
static inline int distance_inrange(int x0, int y0, int x1, int y1, float r0, float r1)
{
    int dx = x0 - x1;
    int dy = y0 - y1;
    int q = dx * dx + dy * dy;
    return q >= r0 * r0 && q < r1 * r1;
}

// ============================================================================
// 圆形绘制实现（按通道数分别实现）
// ============================================================================

// draw_circle_c1 - 单通道圆形绘制
static void draw_circle_c1(unsigned char* pixels, int w, int h, int cx, int cy, int radius, unsigned int color,
                           int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w;

    if (thickness == -1) {
        // 填充模式：遍历圆的外接矩形，判断像素是否在圆内
        for (int y = cy - (radius - 1); y < cy + radius; y++) {
            if (y < 0) continue;
            if (y >= h) break;
            unsigned char* p = pixels + stride * y;
            for (int x = cx - (radius - 1); x < cx + radius; x++) {
                if (x < 0) continue;
                if (x >= w) break;
                if (distance_lessequal(x, y, cx, cy, radius)) {
                    p[x] = pen_color[0];
                }
            }
        }
        return;
    }

    // 边框模式：绘制两个同心圆之间的环形区域
    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    for (int y = cy - (radius - 1) - t0; y < cy + radius + t1; y++) {
        if (y < 0) continue;
        if (y >= h) break;
        unsigned char* p = pixels + stride * y;
        for (int x = cx - (radius - 1) - t0; x < cx + radius + t1; x++) {
            if (x < 0) continue;
            if (x >= w) break;
            if (distance_inrange(x, y, cx, cy, radius - t0, radius + t1)) {
                p[x] = pen_color[0];
            }
        }
    }
}

// draw_circle_c2 - 双通道圆形绘制
static void draw_circle_c2(unsigned char* pixels, int w, int h, int cx, int cy, int radius, unsigned int color,
                           int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 2;

    if (thickness == -1) {
        // 填充模式
        for (int y = cy - (radius - 1); y < cy + radius; y++) {
            if (y < 0) continue;
            if (y >= h) break;
            unsigned char* p = pixels + stride * y;
            for (int x = cx - (radius - 1); x < cx + radius; x++) {
                if (x < 0) continue;
                if (x >= w) break;
                if (distance_lessequal(x, y, cx, cy, radius)) {
                    p[x * 2 + 0] = pen_color[0];
                    p[x * 2 + 1] = pen_color[1];
                }
            }
        }
        return;
    }

    // 边框模式
    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    for (int y = cy - radius - t0; y < cy + radius + t1; y++) {
        if (y < 0) continue;
        if (y >= h) break;
        unsigned char* p = pixels + stride * y;
        for (int x = cx - radius - t0; x < cx + radius + t1; x++) {
            if (x < 0) continue;
            if (x >= w) break;
            if (distance_inrange(x, y, cx, cy, radius - t0, radius + t1)) {
                p[x * 2 + 0] = pen_color[0];
                p[x * 2 + 1] = pen_color[1];
            }
        }
    }
}

// draw_circle_c3 - 三通道(RGB)圆形绘制
static void draw_circle_c3(unsigned char* pixels, int w, int h, int cx, int cy, int radius, unsigned int color,
                           int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 3;

    if (thickness == -1) {
        // 填充模式
        for (int y = cy - (radius - 1); y < cy + radius; y++) {
            if (y < 0) continue;
            if (y >= h) break;
            unsigned char* p = pixels + stride * y;
            for (int x = cx - (radius - 1); x < cx + radius; x++) {
                if (x < 0) continue;
                if (x >= w) break;
                if (distance_lessequal(x, y, cx, cy, radius)) {
                    p[x * 3 + 0] = pen_color[0];
                    p[x * 3 + 1] = pen_color[1];
                    p[x * 3 + 2] = pen_color[2];
                }
            }
        }
        return;
    }

    // 边框模式
    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    for (int y = cy - radius - t0; y < cy + radius + t1; y++) {
        if (y < 0) continue;
        if (y >= h) break;
        unsigned char* p = pixels + stride * y;
        for (int x = cx - radius - t0; x < cx + radius + t1; x++) {
            if (x < 0) continue;
            if (x >= w) break;
            if (distance_inrange(x, y, cx, cy, radius - t0, radius + t1)) {
                p[x * 3 + 0] = pen_color[0];
                p[x * 3 + 1] = pen_color[1];
                p[x * 3 + 2] = pen_color[2];
            }
        }
    }
}

// draw_circle_c4 - 四通道(RGBA)圆形绘制
static void draw_circle_c4(unsigned char* pixels, int w, int h, int cx, int cy, int radius, unsigned int color,
                           int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 4;

    if (thickness == -1) {
        // 填充模式
        for (int y = cy - (radius - 1); y < cy + radius; y++) {
            if (y < 0) continue;
            if (y >= h) break;
            unsigned char* p = pixels + stride * y;
            for (int x = cx - (radius - 1); x < cx + radius; x++) {
                if (x < 0) continue;
                if (x >= w) break;
                if (distance_lessequal(x, y, cx, cy, radius)) {
                    p[x * 4 + 0] = pen_color[0];
                    p[x * 4 + 1] = pen_color[1];
                    p[x * 4 + 2] = pen_color[2];
                    p[x * 4 + 3] = pen_color[3];
                }
            }
        }
        return;
    }

    // 边框模式
    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    for (int y = cy - (radius - 1) - t0; y < cy + radius + t1; y++) {
        if (y < 0) continue;
        if (y >= h) break;
        unsigned char* p = pixels + stride * y;
        for (int x = cx - (radius - 1) - t0; x < cx + radius + t1; x++) {
            if (x < 0) continue;
            if (x >= w) break;
            if (distance_inrange(x, y, cx, cy, radius - t0, radius + t1)) {
                p[x * 4 + 0] = pen_color[0];
                p[x * 4 + 1] = pen_color[1];
                p[x * 4 + 2] = pen_color[2];
                p[x * 4 + 3] = pen_color[3];
            }
        }
    }
}

// ============================================================================
// draw_circle_yuv420sp - YUV420SP格式圆形绘制
// 分别对Y和UV分量绘制圆形
// ============================================================================
static void draw_circle_yuv420sp(unsigned char* yuv420sp, int w, int h, int cx, int cy, int radius, unsigned int color,
                                 int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;

    unsigned int v_y;
    unsigned int v_uv;
    unsigned char* pen_color_y = (unsigned char*)&v_y;
    unsigned char* pen_color_uv = (unsigned char*)&v_uv;
    pen_color_y[0] = pen_color[0];
    pen_color_uv[0] = pen_color[1];
    pen_color_uv[1] = pen_color[2];

    // 在Y分量上绘制圆形
    unsigned char* Y = yuv420sp;
    draw_circle_c1(Y, w, h, cx, cy, radius, v_y, thickness);

    // 在UV分量上绘制圆形（分辨率减半）
    unsigned char* UV = yuv420sp + w * h;
    int thickness_uv = thickness == -1 ? thickness : max(thickness / 2, 1);
    draw_circle_c2(UV, w / 2, h / 2, cx / 2, cy / 2, radius / 2, v_uv, thickness_uv);
}

// ============================================================================
// distance_lessthan - 计算点到线段的最短距离（用于线段绘制）
// 使用向量投影法：将点投影到线段所在直线上，判断是否在线段范围内
// ============================================================================
static inline int distance_lessthan(int x, int y, int x0, int y0, int x1, int y1, float t)
{
    int dx01 = x1 - x0;
    int dy01 = y1 - y0;
    int dx0 = x - x0;
    int dy0 = y - y0;

    // 计算投影参数 r = (P-A)·(B-A) / |B-A|²
    float r = (float)(dx0 * dx01 + dy0 * dy01) / (dx01 * dx01 + dy01 * dy01);

    // r不在[0,1]范围内，说明垂足在线段延长线上
    if (r < 0 || r > 1)
        return 0;

    // 计算垂足坐标
    float px = x0 + dx01 * r;
    float py = y0 + dy01 * r;
    float dx = x - px;
    float dy = y - py;
    float p = dx * dx + dy * dy;
    return p < t;  // 判断距离是否小于阈值
}

// ============================================================================
// 直线绘制实现（按通道数分别实现）
// 使用距离场方法：遍历线段外接矩形，判断像素到线段的距离
// ============================================================================

// draw_line_c1 - 单通道直线绘制
static void draw_line_c1(unsigned char* pixels, int w, int h, int x0, int y0, int x1, int y1, unsigned int color,
                         int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w;

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    int x_min = min(x0, x1);
    int x_max = max(x0, x1);
    int y_min = min(y0, y1);
    int y_max = max(y0, y1);

    // 遍历线段外接矩形区域
    for (int y = y_min - t0; y < y_max + t1; y++) {
        if (y < 0) continue;
        if (y >= h) break;
        unsigned char* p = pixels + stride * y;
        for (int x = x_min - t0; x < x_max + t1; x++) {
            if (x < 0) continue;
            if (x >= w) break;
            // 判断像素到线段的距离是否在粗细范围内
            if (distance_lessthan(x, y, x0, y0, x1, y1, t1)) {
                p[x] = pen_color[0];
            }
        }
    }
}

// draw_line_c2 - 双通道直线绘制
static void draw_line_c2(unsigned char* pixels, int w, int h, int x0, int y0, int x1, int y1, unsigned int color,
                         int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 2;

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    int x_min = min(x0, x1);
    int x_max = max(x0, x1);
    int y_min = min(y0, y1);
    int y_max = max(y0, y1);

    for (int y = y_min - t0; y < y_max + t1; y++) {
        if (y < 0) continue;
        if (y >= h) break;
        unsigned char* p = pixels + stride * y;
        for (int x = x_min - t0; x < x_max + t1; x++) {
            if (x < 0) continue;
            if (x >= w) break;
            if (distance_lessthan(x, y, x0, y0, x1, y1, t1)) {
                p[x * 2 + 0] = pen_color[0];
                p[x * 2 + 1] = pen_color[1];
            }
        }
    }
}

// draw_line_c3 - 三通道(RGB)直线绘制
static void draw_line_c3(unsigned char* pixels, int w, int h, int x0, int y0, int x1, int y1, unsigned int color,
                         int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 3;

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    int x_min = min(x0, x1);
    int x_max = max(x0, x1);
    int y_min = min(y0, y1);
    int y_max = max(y0, y1);

    for (int y = y_min - t0; y < y_max + t1; y++) {
        if (y < 0) continue;
        if (y >= h) break;
        unsigned char* p = pixels + stride * y;
        for (int x = x_min - t0; x < x_max + t1; x++) {
            if (x < 0) continue;
            if (x >= w) break;
            if (distance_lessthan(x, y, x0, y0, x1, y1, t1)) {
                p[x * 3 + 0] = pen_color[0];
                p[x * 3 + 1] = pen_color[1];
                p[x * 3 + 2] = pen_color[2];
            }
        }
    }
}

// draw_line_c4 - 四通道(RGBA)直线绘制
static void draw_line_c4(unsigned char* pixels, int w, int h, int x0, int y0, int x1, int y1, unsigned int color,
                         int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 4;

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    int x_min = min(x0, x1);
    int x_max = max(x0, x1);
    int y_min = min(y0, y1);
    int y_max = max(y0, y1);

    for (int y = y_min - t0; y < y_max + t1; y++) {
        if (y < 0) continue;
        if (y >= h) break;
        unsigned char* p = pixels + stride * y;
        for (int x = x_min - t0; x < x_max + t1; x++) {
            if (x < 0) continue;
            if (x >= w) break;
            if (distance_lessthan(x, y, x0, y0, x1, y1, t1)) {
                p[x * 4 + 0] = pen_color[0];
                p[x * 4 + 1] = pen_color[1];
                p[x * 4 + 2] = pen_color[2];
                p[x * 4 + 3] = pen_color[3];
            }
        }
    }
}

// ============================================================================
// draw_line_yuv420sp - YUV420SP格式直线绘制
// 分别对Y和UV分量绘制直线
// ============================================================================
static void draw_line_yuv420sp(unsigned char* yuv420sp, int w, int h, int x0, int y0, int x1, int y1,
                               unsigned int color, int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;

    unsigned int v_y;
    unsigned int v_uv;
    unsigned char* pen_color_y = (unsigned char*)&v_y;
    unsigned char* pen_color_uv = (unsigned char*)&v_uv;
    pen_color_y[0] = pen_color[0];
    pen_color_uv[0] = pen_color[1];
    pen_color_uv[1] = pen_color[2];

    // 在Y分量上绘制直线
    unsigned char* Y = yuv420sp;
    draw_line_c1(Y, w, h, x0, y0, x1, y1, v_y, thickness);

    // 在UV分量上绘制直线（分辨率减半）
    unsigned char* UV = yuv420sp + w * h;
    int thickness_uv = thickness == -1 ? thickness : max(thickness / 2, 1);
    draw_line_c2(UV, w / 2, h / 2, x0 / 2, y0 / 2, x1 / 2, y1 / 2, v_uv, thickness_uv);
}

// ============================================================================
// 文本绘制相关函数
// ============================================================================

// get_text_drawing_size - 计算文本绘制所需的像素尺寸
static void get_text_drawing_size(const char* text, int fontpixelsize, int* w, int* h)
{
    *w = 0;
    *h = 0;

    const int n = strlen(text);
    int line_w = 0;

    for (int i = 0; i < n; i++) {
        char ch = text[i];
        if (ch == '\n') {
            // 遇到换行符：更新最大宽度，行高加倍
            *w = max(*w, line_w);
            *h += fontpixelsize * 2;
            line_w = 0;
        }
        if (isprint(ch) != 0) {
            line_w += fontpixelsize;
        }
    }

    *w = max(*w, line_w);
    *h += fontpixelsize * 2;
}

// ============================================================================
// resize_bilinear_c1 - 双线性插值缩放单通道图像
// 用于将内置字体位图缩放到指定字号
// ============================================================================
static int resize_bilinear_c1(const unsigned char* src_pixels, int w, int h, unsigned char* dst_pixels, int w2, int h2)
{
    int A, B, C, D, x, y, index, gray;
    float x_ratio = ((float)(w - 1)) / w2;
    float y_ratio = ((float)(h - 1)) / h2;
    float x_diff, y_diff, ya, yb;
    int offset = 0;

    for (int i = 0; i < h2; i++) {
        for (int j = 0; j < w2; j++) {
            x = (int)(x_ratio * j);
            y = (int)(y_ratio * i);
            x_diff = (x_ratio * j) - x;
            y_diff = (y_ratio * i) - y;
            index = y * w + x;

            // 获取四个相邻像素
            A = src_pixels[index] & 0xff;
            B = src_pixels[index + 1] & 0xff;
            C = src_pixels[index + w] & 0xff;
            D = src_pixels[index + w + 1] & 0xff;

            // 双线性插值公式
            gray = (int)(A * (1 - x_diff) * (1 - y_diff) + B * (x_diff) * (1 - y_diff) + C * (y_diff) * (1 - x_diff) +
                         D * (x_diff * y_diff));

            dst_pixels[offset++] = gray;
        }
    }
    return 0;
}

// ============================================================================
// 文本绘制实现（按通道数分别实现）
// 使用内置位图字体，支持抗锯齿渲染
// ============================================================================

// draw_text_c1 - 单通道文本绘制
static void draw_text_c1(unsigned char* pixels, int w, int h, const char* text, int x, int y, int fontpixelsize,
                         unsigned int color)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w;

    // 分配缩放后的字体位图缓冲区
    unsigned char* resized_font_bitmap = (unsigned char*)malloc(fontpixelsize * fontpixelsize * 2);

    const int n = strlen(text);
    int cursor_x = x;
    int cursor_y = y;

    for (int i = 0; i < n; i++) {
        char ch = text[i];

        if (ch == '\n') {
            // 换行处理
            cursor_x = x;
            cursor_y += fontpixelsize * 2;
        }

        if (isprint(ch) != 0) {
            // 获取字符的位图数据（来自font.h内置字体）
            const unsigned char* font_bitmap = mono_font_data[ch - ' '];

            // 将20x40的字体位图缩放到指定字号
            resize_bilinear_c1(font_bitmap, 20, 40, resized_font_bitmap, fontpixelsize, fontpixelsize * 2);

            // 将缩放后的字体绘制到图像上（使用Alpha混合）
            for (int j = cursor_y; j < cursor_y + fontpixelsize * 2; j++) {
                if (j < 0) continue;
                if (j >= h) break;

                const unsigned char* palpha = resized_font_bitmap + (j - cursor_y) * fontpixelsize;
                unsigned char* p = pixels + stride * j;

                for (int k = cursor_x; k < cursor_x + fontpixelsize; k++) {
                    if (k < 0) continue;
                    if (k >= w) break;

                    unsigned char alpha = palpha[k - cursor_x];
                    // Alpha混合公式：dst = src * (1-alpha) + color * alpha
                    p[k] = (p[k] * (255 - alpha) + pen_color[0] * alpha) / 255;
                }
            }

            cursor_x += fontpixelsize;
        }
    }

    free(resized_font_bitmap);
}

// draw_text_c2 - 双通道文本绘制
static void draw_text_c2(unsigned char* pixels, int w, int h, const char* text, int x, int y, int fontpixelsize,
                         unsigned int color)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 2;

    unsigned char* resized_font_bitmap = (unsigned char*)malloc(fontpixelsize * fontpixelsize * 2);
    const int n = strlen(text);
    int cursor_x = x;
    int cursor_y = y;

    for (int i = 0; i < n; i++) {
        char ch = text[i];
        if (ch == '\n') {
            cursor_x = x;
            cursor_y += fontpixelsize * 2;
        }

        if (isprint(ch) != 0) {
            int font_bitmap_index = ch - ' ';
            const unsigned char* font_bitmap = mono_font_data[font_bitmap_index];
            resize_bilinear_c1(font_bitmap, 20, 40, resized_font_bitmap, fontpixelsize, fontpixelsize * 2);

            for (int j = cursor_y; j < cursor_y + fontpixelsize * 2; j++) {
                if (j < 0) continue;
                if (j >= h) break;
                const unsigned char* palpha = resized_font_bitmap + (j - cursor_y) * fontpixelsize;
                unsigned char* p = pixels + stride * j;
                for (int k = cursor_x; k < cursor_x + fontpixelsize; k++) {
                    if (k < 0) continue;
                    if (k >= w) break;
                    unsigned char alpha = palpha[k - cursor_x];
                    p[k * 2 + 0] = (p[k * 2 + 0] * (255 - alpha) + pen_color[0] * alpha) / 255;
                    p[k * 2 + 1] = (p[k * 2 + 1] * (255 - alpha) + pen_color[1] * alpha) / 255;
                }
            }
            cursor_x += fontpixelsize;
        }
    }
    free(resized_font_bitmap);
}

// draw_text_c3 - 三通道(RGB)文本绘制
static void draw_text_c3(unsigned char* pixels, int w, int h, const char* text, int x, int y, int fontpixelsize,
                         unsigned int color)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 3;

    unsigned char* resized_font_bitmap = (unsigned char*)malloc(fontpixelsize * fontpixelsize * 2);
    const int n = strlen(text);
    int cursor_x = x;
    int cursor_y = y;

    for (int i = 0; i < n; i++) {
        char ch = text[i];
        if (ch == '\n') {
            cursor_x = x;
            cursor_y += fontpixelsize * 2;
        }

        if (isprint(ch) != 0) {
            int font_bitmap_index = ch - ' ';
            const unsigned char* font_bitmap = mono_font_data[font_bitmap_index];
            resize_bilinear_c1(font_bitmap, 20, 40, resized_font_bitmap, fontpixelsize, fontpixelsize * 2);

            for (int j = cursor_y; j < cursor_y + fontpixelsize * 2; j++) {
                if (j < 0) continue;
                if (j >= h) break;
                const unsigned char* palpha = resized_font_bitmap + (j - cursor_y) * fontpixelsize;
                unsigned char* p = pixels + stride * j;
                for (int k = cursor_x; k < cursor_x + fontpixelsize; k++) {
                    if (k < 0) continue;
                    if (k >= w) break;
                    unsigned char alpha = palpha[k - cursor_x];
                    p[k * 3 + 0] = (p[k * 3 + 0] * (255 - alpha) + pen_color[0] * alpha) / 255;
                    p[k * 3 + 1] = (p[k * 3 + 1] * (255 - alpha) + pen_color[1] * alpha) / 255;
                    p[k * 3 + 2] = (p[k * 3 + 2] * (255 - alpha) + pen_color[2] * alpha) / 255;
                }
            }
            cursor_x += fontpixelsize;
        }
    }
    free(resized_font_bitmap);
}

// draw_text_c4 - 四通道(RGBA)文本绘制
static void draw_text_c4(unsigned char* pixels, int w, int h, const char* text, int x, int y, int fontpixelsize,
                         unsigned int color)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 4;

    unsigned char* resized_font_bitmap = (unsigned char*)malloc(fontpixelsize * fontpixelsize * 2);
    const int n = strlen(text);
    int cursor_x = x;
    int cursor_y = y;

    for (int i = 0; i < n; i++) {
        char ch = text[i];
        if (ch == '\n') {
            cursor_x = x;
            cursor_y += fontpixelsize * 2;
        }

        if (isprint(ch) != 0) {
            const unsigned char* font_bitmap = mono_font_data[ch - ' '];
            resize_bilinear_c1(font_bitmap, 20, 40, resized_font_bitmap, fontpixelsize, fontpixelsize * 2);

            for (int j = cursor_y; j < cursor_y + fontpixelsize * 2; j++) {
                if (j < 0) continue;
                if (j >= h) break;
                const unsigned char* palpha = resized_font_bitmap + (j - cursor_y) * fontpixelsize;
                unsigned char* p = pixels + stride * j;
                for (int k = cursor_x; k < cursor_x + fontpixelsize; k++) {
                    if (k < 0) continue;
                    if (k >= w) break;
                    unsigned char alpha = palpha[k - cursor_x];
                    p[k * 4 + 0] = (p[k * 4 + 0] * (255 - alpha) + pen_color[0] * alpha) / 255;
                    p[k * 4 + 1] = (p[k * 4 + 1] * (255 - alpha) + pen_color[1] * alpha) / 255;
                    p[k * 4 + 2] = (p[k * 4 + 2] * (255 - alpha) + pen_color[2] * alpha) / 255;
                    p[k * 4 + 3] = (p[k * 4 + 3] * (255 - alpha) + pen_color[3] * alpha) / 255;
                }
            }
            cursor_x += fontpixelsize;
        }
    }
    free(resized_font_bitmap);
}

// ============================================================================
// draw_text_yuv420sp - YUV420SP格式文本绘制
// 分别对Y和UV分量绘制文本
// ============================================================================
static void draw_text_yuv420sp(unsigned char* yuv420sp, int w, int h, const char* text, int x, int y, int fontpixelsize,
                               unsigned int color)
{
    const unsigned char* pen_color = (const unsigned char*)&color;

    unsigned int v_y;
    unsigned int v_uv;
    unsigned char* pen_color_y = (unsigned char*)&v_y;
    unsigned char* pen_color_uv = (unsigned char*)&v_uv;
    pen_color_y[0] = pen_color[0];
    pen_color_uv[0] = pen_color[1];
    pen_color_uv[1] = pen_color[2];

    // 在Y分量上绘制文本
    unsigned char* Y = yuv420sp;
    draw_text_c1(Y, w, h, text, x, y, fontpixelsize, v_y);

    // 在UV分量上绘制文本（分辨率减半，字号也减半）
    unsigned char* UV = yuv420sp + w * h;
    draw_text_c2(UV, w / 2, h / 2, text, x / 2, y / 2, max(fontpixelsize / 2, 1), v_uv);
}

// ============================================================================
// 图像叠加绘制实现（按通道数分别实现）
// ============================================================================

// draw_image_c1 - 单通道图像叠加
static void draw_image_c1(unsigned char* pixels, int w, int h, unsigned char* draw_img, int x, int y, int rw, int rh)
{
    for (int i = 0; i < rh; i++) {
        memcpy(pixels + (y + i) * w + x,  draw_img + i * rw,  rw);
    }
}

// draw_image_c2 - 双通道图像叠加
static void draw_image_c2(unsigned char* pixels, int w, int h, unsigned char* draw_img, int x, int y, int rw, int rh)
{
    for (int i = 0; i < rh; i++) {
        memcpy(pixels + ((y + i) * w + x) * 2,  draw_img + i * rw * 2,  rw * 2);
    }
}

// draw_image_c3 - 三通道(RGB)图像叠加
static void draw_image_c3(unsigned char* pixels, int w, int h, unsigned char* draw_img, int x, int y, int rw, int rh)
{
    printf("draw_image_c3 pixels=%p wxh=%dx%d draw_img=%p pos=(%d %d) rwxrh=%dx%d\n", pixels, w, h, draw_img, x, y, rw, rh);
    for (int i = 0; i < rh; i++) {
        memcpy(pixels + ((y + i) * w + x) * 3,  draw_img + i * rw * 3,  rw * 3);
    }
}

// draw_image_c4 - 四通道(RGBA)图像叠加
static void draw_image_c4(unsigned char* pixels, int w, int h, unsigned char* draw_img, int x, int y, int rw, int rh)
{
    for (int i = 0; i < rh; i++) {
        memcpy(pixels + ((y + i) * w + x) * 4,  draw_img + i * rw * 4,  rw * 4);
    }
}

// draw_image_yuv420sp - YUV420SP格式图像叠加
static void draw_image_yuv420sp(unsigned char* pixels, int w, int h, unsigned char* draw_img, int x, int y, int rw, int rh)
{
    // 分别叠加Y和UV分量
    draw_image_c1(pixels, w, h, draw_img, x, y, rw, rh);
    draw_image_c2(pixels, w, h / 2, draw_img + rw * rh, x, y/2, rw, rh/2);
}

// ============================================================================
// 对外接口实现
// ============================================================================

// draw_rectangle - 绘制矩形（根据图像格式选择对应实现）
void draw_rectangle(image_buffer_t* image, int rx, int ry, int rw, int rh, unsigned int color,
                      int thickness)
{
    image_format_t format = image->format;
    unsigned char* pixels = image->virt_addr;
    int w = image->width;
    int h = image->height;

    // 将ARGB8888颜色转换为目标格式的颜色表示
    unsigned int draw_color = convert_color(color, format);

    switch (format)
    {
    case IMAGE_FORMAT_RGB888:
        draw_rectangle_c3(pixels, w, h, rx, ry, rw, rh, draw_color, thickness);
        break;
    case IMAGE_FORMAT_RGBA8888:
        draw_rectangle_c4(pixels, w, h, rx, ry, rw, rh, draw_color, thickness);
        break;
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        draw_rectangle_yuv420sp(pixels, w, h, rx, ry, rw, rh, draw_color, thickness);
        break;
    default:
        printf("no support format %d", format);
        break;
    }
}

// draw_line - 绘制直线（根据图像格式选择对应实现）
void draw_line(image_buffer_t* image, int x0, int y0, int x1, int y1, unsigned int color,
                 int thickness)
{
    image_format_t format = image->format;
    unsigned char* pixels = image->virt_addr;
    int w = image->width;
    int h = image->height;

    unsigned draw_color = convert_color(color, format);

    switch (format)
    {
    case IMAGE_FORMAT_RGB888:
        draw_line_c3(pixels, w, h, x0, y0, x1, y1, draw_color, thickness);
        break;
    case IMAGE_FORMAT_RGBA8888:
        draw_line_c4(pixels, w, h, x0, y0, x1, y1, draw_color, thickness);
        break;
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        draw_line_yuv420sp(pixels, w, h, x0, y0, x1, y1, draw_color, thickness);
        break;
    default:
        printf("no support format %d", format);
        break;
    }
}

// ============================================================================
// rbbox_to_corners - 将旋转边界框(OBB)转换为四个角点坐标
// 输入：中心点+宽高+旋转角度
// 输出：顺时针排列的四个角点坐标
// ============================================================================
void rbbox_to_corners(const float *in_rbbox, int *out_rbbox) {
    // 计算中心点
    float cx = in_rbbox[0] + in_rbbox[2] / 2;
    float cy = in_rbbox[1] + in_rbbox[3] / 2;
    float x_d = in_rbbox[2];   // 宽度
    float y_d = in_rbbox[3];   // 高度
    float angle = in_rbbox[4]; // 旋转角度（弧度）

    float a_cos = cos(angle);
    float a_sin = sin(angle);

    // 相对中心点的四个角点（未旋转）
    float corners_x[4] = {-x_d / 2, -x_d / 2, x_d / 2, x_d / 2};
    float corners_y[4] = {-y_d / 2, y_d / 2, y_d / 2, -y_d / 2};

    // 应用旋转矩阵并平移到绝对坐标
    for (int i = 0; i < 4; ++i) {
        out_rbbox[2 * i] = (int)(a_cos * corners_x[i] - a_sin * corners_y[i] + cx);
        out_rbbox[2 * i + 1] = (int)(a_sin * corners_x[i] + a_cos * corners_y[i] + cy);
    }
}

// ============================================================================
// draw_obb_rectangle - 绘制旋转矩形（OBB定向边界框）
// 通过计算旋转后的四个角点，用直线连接绘制
// ============================================================================
void draw_obb_rectangle(image_buffer_t *image, int rx, int ry, int rw, int rh, float angle, unsigned int color,
                        int thickness) {
    float in_bbox[5] = {(float)(rx), (float)(ry), (float)(rw), (float)(rh), angle};
    int out_box_corners[8];

    // 将OBB转换为四个角点
    rbbox_to_corners(in_bbox, out_box_corners);

    // 顺时针连接四个角点绘制四条边
    for(int i = 0 ; i < 4; i++) {
        int index1 = i;
        int index2 = (i + 1) % 4;
        draw_line(image, out_box_corners[index1 * 2], out_box_corners[index1 * 2 + 1],
                  out_box_corners[index2 * 2], out_box_corners[index2 * 2 + 1], color, thickness);
    }
}

// draw_text - 绘制文本（根据图像格式选择对应实现）
void draw_text(image_buffer_t* image, const char* text, int x, int y, unsigned int color,
                 int fontsize)
{
    image_format_t format = image->format;
    unsigned char* pixels = image->virt_addr;
    int w = image->width;
    int h = image->height;
    unsigned int draw_color = convert_color(color, format);

    switch (format)
    {
    case IMAGE_FORMAT_RGB888:
        draw_text_c3(pixels, w, h, text, x, y, fontsize, draw_color);
        break;
    case IMAGE_FORMAT_RGBA8888:
        draw_text_c4(pixels, w, h, text, x, y, fontsize, draw_color);
        break;
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        draw_text_yuv420sp(pixels, w, h, text, x, y, fontsize, draw_color);
        break;
    default:
        printf("no support format %d", format);
        break;
    }
}

// draw_circle - 绘制圆形（根据图像格式选择对应实现）
void draw_circle(image_buffer_t* image, int cx, int cy, int radius, unsigned int color,
                 int thickness)
{
    image_format_t format = image->format;
    unsigned char* pixels = image->virt_addr;
    int w = image->width;
    int h = image->height;
    unsigned draw_color = convert_color(color, format);

    switch (format)
    {
    case IMAGE_FORMAT_RGB888:
        draw_circle_c3(pixels, w, h, cx, cy, radius, draw_color, thickness);
        break;
    case IMAGE_FORMAT_RGBA8888:
        draw_circle_c4(pixels, w, h, cx, cy, radius, draw_color, thickness);
        break;
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        draw_circle_yuv420sp(pixels, w, h, cx, cy, radius, draw_color, thickness);
        break;
    default:
        printf("no support format %d", format);
        break;
    }
}

// draw_image - 叠加图像（根据图像格式选择对应实现）
void draw_image(image_buffer_t* image, unsigned char* draw_img, int x, int y, int rw, int rh)
{
    image_format_t format = image->format;
    unsigned char* pixels = image->virt_addr;
    int w = image->width;
    int h = image->height;

    switch (format)
    {
    case IMAGE_FORMAT_RGB888:
        draw_image_c3(pixels, w, h, draw_img, x, y, rw, rh);
        break;
    case IMAGE_FORMAT_RGBA8888:
        draw_image_c4(pixels, w, h, draw_img, x, y, rw, rh);
        break;
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        draw_image_yuv420sp(pixels, w, h, draw_img, x, y, rw, rh);
        break;
    default:
        printf("no support format %d", format);
        break;
    }
}
