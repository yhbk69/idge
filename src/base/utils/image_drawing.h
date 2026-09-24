#ifndef _RKNN_MODEL_ZOO_IMAGE_DRAWING_H_
#define _RKNN_MODEL_ZOO_IMAGE_DRAWING_H_

// ============================================================================
// 图像绘制函数头文件
// 提供在图像上绘制几何图形、文本等功能
// 支持RGB888/RGBA8888/YUV420SP等多种像素格式
// 颜色格式统一使用ARGB8888（Alpha-Red-Green-Blue）
// ============================================================================

#include "common.hpp"

// ============================================================================
// 颜色常量定义（ARGB8888格式）
// 每个颜色占4字节：Alpha(透明度) + Red + Green + Blue
// Alpha=0xFF表示完全不透明
// ============================================================================
#define COLOR_GREEN     0xFF00FF00    // 绿色
#define COLOR_BLUE      0xFF0000FF    // 蓝色
#define COLOR_RED       0xFFFF0000    // 红色
#define COLOR_YELLOW    0xFFFFFF00    // 黄色
#define COLOR_ORANGE    0xFFFF4500    // 橙色
#define COLOR_BLACK     0xFF000000    // 黑色
#define COLOR_WHITE     0xFFFFFFFF    // 白色

/**
 * @brief 绘制矩形框
 * 支持空心（指定线宽）和实心（thickness=-1）两种模式
 * 常用于目标检测框绘制
 * 
 * @param image [in] 目标图像缓冲区
 * @param rx [in] 矩形左上角X坐标
 * @param ry [in] 矩形左上角Y坐标
 * @param rw [in] 矩形宽度
 * @param rh [in] 矩形高度
 * @param color [in] 矩形颜色（ARGB8888格式）
 * @param thickness [in] 线条粗细，-1表示填充
 */
void draw_rectangle(image_buffer_t* image, int rx, int ry, int rw, int rh, unsigned int color,
                      int thickness);

/**
 * @brief 绘制旋转矩形（OBB定向边界框）
 * 用于旋转目标检测结果的可视化
 * 
 * @param image [in] 目标图像缓冲区
 * @param rx [in] 矩形中心X坐标
 * @param ry [in] 矩形中心Y坐标
 * @param rw [in] 矩形宽度
 * @param rh [in] 矩形高度
 * @param angle [in] 旋转角度（弧度）
 * @param color [in] 矩形颜色（ARGB8888格式）
 * @param thickness [in] 线条粗细
 */
void draw_obb_rectangle(image_buffer_t *image, int rx, int ry, int rw, int rh, float angle, unsigned int color,
                        int thickness);

/**
 * @brief 绘制直线
 * 
 * @param image [in] 目标图像缓冲区
 * @param x0 [in] 起点X坐标
 * @param y0 [in] 起点Y坐标
 * @param x1 [in] 终点X坐标
 * @param y1 [in] 终点Y坐标
 * @param color [in] 线条颜色（ARGB8888格式）
 * @param thickness [in] 线条粗细
 */
void draw_line(image_buffer_t* image, int x0, int y0, int x1, int y1, unsigned int color,
                 int thickness);

/**
 * @brief 绘制文本（仅支持ASCII字符）
 * 使用内置位图字体渲染，支持可变字号
 * 
 * @param image [in] 目标图像缓冲区
 * @param text [in] 要绘制的文本字符串
 * @param x [in] 文本起始X坐标
 * @param y [in] 文本起始Y坐标
 * @param color [in] 文本颜色（ARGB8888格式）
 * @param fontsize [in] 字体大小（像素）
 */
void draw_text(image_buffer_t* image, const char* text, int x, int y, unsigned int color,
                 int fontsize);

/**
 * @brief 绘制圆形
 * 支持空心（指定线宽）和实心（thickness=-1）两种模式
 * 
 * @param image [in] 目标图像缓冲区
 * @param cx [in] 圆心X坐标
 * @param cy [in] 圆心Y坐标
 * @param radius [in] 圆的半径
 * @param color [in] 圆的颜色（ARGB8888格式）
 * @param thickness [in] 线条粗细，-1表示填充
 */
void draw_circle(image_buffer_t* image, int cx, int cy, int radius, unsigned int color,
                 int thickness);

/**
 * @brief 绘制图像（图像叠加）
 * 将一张图像绘制到另一张图像的指定位置
 * 
 * @param image [in] 目标图像缓冲区
 * @param draw_img [in] 要绘制的源图像数据
 * @param x [in] 绘制位置X坐标
 * @param y [in] 绘制位置Y坐标
 * @param rw [in] 源图像宽度
 * @param rh [in] 源图像高度
 */
void draw_image(image_buffer_t* image, unsigned char* draw_img, int x, int y, int rw, int rh);

#endif  // _RKNN_MODEL_ZOO_IMAGE_DRAWING_H_
