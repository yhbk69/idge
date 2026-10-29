#ifndef _RKNN_MODEL_ZOO_IMAGE_UTILS_H_
#define _RKNN_MODEL_ZOO_IMAGE_UTILS_H_

// ============================================================================
// 图像工具函数头文件
// 提供图像读写、格式转换、缩放裁剪等功能
// 支持JPEG/PNG/BMP等常见图像格式
// 支持RGB888/RGBA8888/YUV420SP/GRAY8等多种像素格式
// ============================================================================

#include "common.hpp"

/**
 * @brief LetterBox 参数结构体
 * 用于目标检测模型输入前的图像预处理
 * 保持宽高比缩放图像，并在短边填充指定颜色
 */
typedef struct {
    int x_pad;      // 水平方向填充像素数（居中填充）
    int y_pad;      // 垂直方向填充像素数（居中填充）
    float scale;    // 缩放比例因子
} letterbox_t;

/**
 * @brief 读取图像文件（支持PNG/JPEG/BMP）
 * 
 * @param path [in] 图像文件路径
 * @param image [out] 读取的图像数据
 * @return int 0: 成功; -1: 错误
 */
int read_image(const char* path, image_buffer_t* image);

/**
 * @brief 写入图像文件（支持PNG/JPEG）
 * 
 * @param path [in] 输出图像路径
 * @param image [in] 要写入的图像数据（仅支持IMAGE_FORMAT_RGB888格式）
 * @return int 0: 成功; -1: 错误
 */
int write_image(const char* path, image_buffer_t* image);

/**
 * @brief 图像格式转换与缩放
 * 支持像素格式转换和区域缩放裁剪
 * 
 * @param src_image [in] 源图像
 * @param dst_image [out] 目标图像
 * @param src_box [in] 源图像上的裁剪矩形
 * @param dst_box [in] 目标图像上的裁剪矩形
 * @param color [in] 目标图像填充颜色（当dst_box无法填满目标时）
 * @return int 
 */
int convert_image(image_buffer_t* src_image, image_buffer_t* dst_image, image_rect_t* src_box, image_rect_t* dst_box, char color);

/**
 * @brief 带LetterBox的图像转换
 * 保持宽高比缩放图像，并居中填充指定颜色
 * 适用于目标检测模型输入预处理
 * 
 * @param src_image [in] 源图像
 * @param dst_image [out] 目标图像
 * @param letterbox [out] LetterBox参数（用于后处理还原坐标）
 * @param color [in] 填充颜色
 * @return int 
 */
int convert_image_with_letterbox(image_buffer_t* src_image, image_buffer_t* dst_image, letterbox_t* letterbox, char color);

/**
 * @brief 获取图像数据大小（字节）
 * 
 * @param image [in] 图像
 * @return int 图像数据大小
 */
int get_image_size(const image_buffer_t* image);

/**
 * @brief 计算LetterBox参数（不执行转换）
 * 仅计算缩放比例和填充偏移量
 * 
 * @param srcW [in] 源图像宽度
 * @param srcH [in] 源图像高度
 * @param dstW [in] 目标图像宽度
 * @param dstH [in] 目标图像高度
 * @param letterbox [out] LetterBox参数
 */
void getLetter(int srcW, int srcH, int dstW, int dstH, letterbox_t* letterbox);

#endif // _RKNN_MODEL_ZOO_IMAGE_UTILS_H_
