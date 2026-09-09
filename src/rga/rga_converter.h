// ============================================================================
// RGA Converter - Rockchip RGA 硬件加速色彩转换和缩放
// ============================================================================
//
// 功能：
//   使用 Rockchip RGA（2D 加速引擎）进行色彩空间转换和图像缩放。
//   所有操作都在 DMA-BUF fd 上执行，无需 CPU 参与像素拷贝（零拷贝）。
//
// RGA 硬件特性：
//   - 支持 NV12/RGB/RGBA/XRGB 等多种格式转换
//   - 支持图像缩放、旋转、裁剪
//   - 支持同步和异步模式
//   - 使用 DMA-BUF fd 作为输入输出（零拷贝）
//
// 关键 API：
//   - imcvtcolor(): 纯色彩转换
//   - improcess(): 缩放 + 色彩转换
//   - imresize(): 纯缩放
//   - imfill(): 填充矩形区域
//
// ============================================================================

#ifndef RGA_CONVERTER_H
#define RGA_CONVERTER_H

#include <rga/im2d.h>        // RGA 图像处理 API
#include <rga/im2d.hpp>      // RGA C++ 封装
#include <drm_fourcc.h>      // DRM 四字符码（像素格式定义）
#include <cstdio>
#include <cstring>
#include <rga/RgaUtils.h>    // RGA 工具函数（get_bpp_from_format 等）

class RgaConverter
{
public:
    // ============================================================================
    // nv12_to_rgba - NV12 → RGBA 色彩转换（零拷贝）
    // ============================================================================
    // 参数：
    //   - src_fd: 源 NV12 DMA-BUF fd
    //   - dst_fd: 目标 RGBA DMA-BUF fd
    //   - src_w/src_h: 源图像尺寸
    //   - dst_w/dst_h: 目标图像尺寸
    //   - src_stride/dst_stride: 行跨度（字节）
    //
    // 流程：
    //   1. 构造源/目标 rga_buffer_t（绑定 fd）
    //   2. 同尺寸：调用 imcvtcolor()（纯色彩转换）
    //   3. 不同尺寸：调用 improcess()（缩放 + 色彩转换）
    //
    // ============================================================================
    static bool nv12_to_rgba(int src_fd, int src_w, int src_h, int src_stride,
                             int dst_fd, int dst_w, int dst_h, int dst_stride)
    {
        // 构造源缓冲区（NV12 格式）
        rga_buffer_t src = {};
        src.fd = src_fd;
        src.width = src_w;
        src.height = src_h;
        src.wstride = src_stride;
        src.hstride = src_h;
        src.format = RK_FORMAT_YCbCr_420_SP;  // NV12 = YUV420 semi-planar

        // 构造目标缓冲区（RGBA 格式）
        rga_buffer_t dst = {};
        dst.fd = dst_fd;
        dst.width = dst_w;
        dst.height = dst_h;
        dst.wstride = dst_stride;
        dst.hstride = dst_h;
        dst.format = RK_FORMAT_RGBA_8888;

        IM_STATUS status;

        if (src_w == dst_w && src_h == dst_h) {
            // 同尺寸：纯色彩转换（NV12 → RGBA）
            status = imcvtcolor(src, dst, src.format, dst.format, IM_SYNC);
        } else {
            // 不同尺寸：缩放 + 色彩转换
            im_rect src_rect = {0, 0, src_w, src_h};
            im_rect dst_rect = {0, 0, dst_w, dst_h};
            status = improcess(src, dst, {}, src_rect, dst_rect, {}, IM_SYNC);
        }

        if (status != IM_STATUS_SUCCESS) {
            printf("[RGA] nv12_to_rgba failed: %s\n", imStrError(status));
            return false;
        }
        return true;
    }

    // ============================================================================
    // convertNV12ToRGBAbyRGA - NV12 → RGBA 转换（使用 buffer handle）
    // ============================================================================
    // 与 nv12_to_rgba 类似，但使用 importbuffer_fd + wrapbuffer_handle 流程
    // 适用于需要更精细控制缓冲区的场景
    // ============================================================================
    static bool convertNV12ToRGBAbyRGA(int src_dma_fd, int dst_dma_fd, int width, int height,
                                      int src_hor_stride, int src_ver_stride)
    {
        int ret = 0;
        int src_width = width;
        int src_height = height;
        int dst_widt = width, dst_height = height;
        int src_format = RK_FORMAT_YCbCr_420_SP;  // NV12
        int dst_format = RK_FORMAT_RGBA_8888;      // RGBA
        int src_buf_size, dst_buf_size;

        rga_buffer_t src_img, dst_img;
        rga_buffer_handle_t src_handle, dst_handle;

        memset(&src_img, 0, sizeof(src_img));
        memset(&dst_img, 0, sizeof(dst_img));

        // 计算缓冲区大小（行跨度 × 高度 × 每像素字节数）
        src_buf_size = src_hor_stride * src_ver_stride * get_bpp_from_format(src_format);
        dst_buf_size = dst_widt * dst_height * get_bpp_from_format(dst_format);

        // 通过 DMA-BUF fd 导入缓冲区到 RGA
        src_handle = importbuffer_fd(src_dma_fd, src_buf_size);
        dst_handle = importbuffer_fd(dst_dma_fd, dst_buf_size);

        if (src_handle == 0 || dst_handle == 0) {
            printf("importbuffer failed\n");
            goto release_buffer;
        }

        // 将缓冲区句柄包装为 rga_buffer_t
        src_img = wrapbuffer_handle(src_handle, src_width, src_height, src_format);
        dst_img = wrapbuffer_handle(dst_handle, dst_widt, dst_height, dst_format);

        src_img.wstride = src_hor_stride;
        src_img.hstride = src_ver_stride;

        // 设置色彩空间（BT.709 有限范围 → RGB 全范围）
        imsetColorSpace(&src_img, IM_YUV_BT709_LIMIT_RANGE);
        imsetColorSpace(&dst_img, IM_RGB_FULL);

        // 检查参数有效性
        ret = imcheck(src_img, dst_img, (im_rect){}, (im_rect){});
        if (IM_STATUS_NOERROR != ret) {
            printf("%d check error %s\n", __LINE__, imStrError((IM_STATUS)ret));
            goto release_buffer;
        }

        // 执行色彩转换
        ret = imcvtcolor(src_img, dst_img, src_format, dst_format);
        if (ret != IM_STATUS_SUCCESS) {
            printf("imcvtcolor error %s\n", imStrError((IM_STATUS)ret));
        }

    release_buffer:
        // 释放 RGA 缓冲区句柄
        if (src_handle) {
            releasebuffer_handle(src_handle);
        }
        if (dst_handle) {
            releasebuffer_handle(dst_handle);
        }

        return true;
    }

    // ============================================================================
    // convertNV12ToRGBbyRGA - NV12 → RGB 转换（3 字节/像素）
    // ============================================================================
    // 与 convertNV12ToRGBAbyRGA 类似，但输出 RGB888 格式（3 字节/像素）
    // 适用于需要更小内存占用的场景
    // ============================================================================
    static bool convertNV12ToRGBbyRGA(int src_dma_fd, int dst_dma_fd, int width, int height,
                                      int src_hor_stride, int src_ver_stride)
    {
        int ret = 0;
        int src_width = width;
        int src_height = height;
        int dst_widt = width, dst_height = height;
        int src_format = RK_FORMAT_YCbCr_420_SP;  // NV12
        int dst_format = RK_FORMAT_RGB_888;        // RGB888（3字节/像素）
        int src_buf_size, dst_buf_size;

        rga_buffer_t src_img, dst_img;
        rga_buffer_handle_t src_handle, dst_handle;

        memset(&src_img, 0, sizeof(src_img));
        memset(&dst_img, 0, sizeof(dst_img));

        src_buf_size = src_hor_stride * src_ver_stride * get_bpp_from_format(src_format);
        dst_buf_size = dst_widt * dst_height * get_bpp_from_format(dst_format);

        src_handle = importbuffer_fd(src_dma_fd, src_buf_size);
        dst_handle = importbuffer_fd(dst_dma_fd, dst_buf_size);

        if (src_handle == 0 || dst_handle == 0) {
            printf("importbuffer failed\n");
            goto release_buffer;
        }

        src_img = wrapbuffer_handle(src_handle, src_width, src_height, src_format);
        dst_img = wrapbuffer_handle(dst_handle, dst_widt, dst_height, dst_format);

        src_img.wstride = src_hor_stride;
        src_img.hstride = src_ver_stride;

        imsetColorSpace(&src_img, IM_YUV_BT709_LIMIT_RANGE);
        imsetColorSpace(&dst_img, IM_RGB_FULL);

        ret = imcheck(src_img, dst_img, (im_rect){}, (im_rect){});
        if (IM_STATUS_NOERROR != ret) {
            printf("%d check error %s\n", __LINE__, imStrError((IM_STATUS)ret));
            goto release_buffer;
        }

        ret = imcvtcolor(src_img, dst_img, src_format, dst_format);
        if (ret != IM_STATUS_SUCCESS) {
            printf("imcvtcolor error %s\n", imStrError((IM_STATUS)ret));
        }

    release_buffer:
        if (src_handle) {
            releasebuffer_handle(src_handle);
        }
        if (dst_handle) {
            releasebuffer_handle(dst_handle);
        }

        return true;
    }

    // ============================================================================
    // nv12_to_xrgb - NV12 → XRGB 色彩转换
    // ============================================================================
    // XRGB = 32 位，最高字节未使用（X = padding）
    // ============================================================================
    static bool nv12_to_xrgb(int src_fd, int src_w, int src_h, int src_stride,
                             int dst_fd, int dst_w, int dst_h, int dst_stride)
    {
        rga_buffer_t src = {};
        src.fd = src_fd;
        src.width = src_w;
        src.height = src_h;
        src.wstride = src_stride;
        src.hstride = src_h;
        src.format = RK_FORMAT_YCbCr_420_SP;

        rga_buffer_t dst = {};
        dst.fd = dst_fd;
        dst.width = dst_w;
        dst.height = dst_h;
        dst.wstride = dst_stride;
        dst.hstride = dst_h;
        dst.format = RK_FORMAT_RGBX_8888;  // XRGB = RGBX（字节序）

        IM_STATUS status = imcvtcolor(src, dst, src.format, dst.format, IM_SYNC);
        return status == IM_STATUS_SUCCESS;
    }

    // ============================================================================
    // nv12_to_rgba_async - NV12 → RGBA 异步转换
    // ============================================================================
    // 使用 IM_ASYNC 模式，RGA 硬件在后台执行转换
    // 适用于高帧率场景（如 60fps 视频），可以流水线化
    // ============================================================================
    static bool nv12_to_rgba_async(int src_fd, int src_w, int src_h, int src_stride,
                                   int dst_fd, int dst_w, int dst_h, int dst_stride)
    {
        rga_buffer_t src = {};
        src.fd = src_fd;
        src.width = src_w;
        src.height = src_h;
        src.wstride = src_stride;
        src.hstride = src_h;
        src.format = RK_FORMAT_YCbCr_420_SP;

        rga_buffer_t dst = {};
        dst.fd = dst_fd;
        dst.width = dst_w;
        dst.height = dst_h;
        dst.wstride = dst_stride;
        dst.hstride = dst_h;
        dst.format = RK_FORMAT_RGBA_8888;

        IM_STATUS status = imcvtcolor(src, dst, src.format, dst.format, IM_ASYNC);
        return status == IM_STATUS_SUCCESS;
    }

    // ============================================================================
    // nv12_to_rgb_resize - NV12 → RGB + 缩放（一步完成，零拷贝）
    // ============================================================================
    // 用于 YOLOv8/YOLO11 预处理：
    //   - 输入：MPP 解码输出的 NV12 DMA-BUF
    //   - 输出：RKNN 输入的 RGB DMA-BUF（640×640）
    //
    // Letterbox 模式：
    //   保持原始宽高比，居中放置，灰色背景填充
    //   这是 YOLO 系列模型的标准预处理方式
    //
    // 参数：
    //   - letterbox: 是否使用 letterbox（默认 true）
    //     true:  保持宽高比，灰色背景填充（YOLO 标准）
    //     false: 直接拉伸到目标尺寸
    // ============================================================================
    static int nv12_to_rgb_resize(int src_fd, int src_w, int src_h, int src_stride,
                                   int dst_fd, int dst_w, int dst_h, int dst_stride,
                                   bool letterbox = true)
    {
        rga_buffer_t src = {};
        src.fd = src_fd;
        src.width = src_w;
        src.height = src_h;
        src.wstride = src_stride;
        src.hstride = src_h;
        src.format = RK_FORMAT_YCbCr_420_SP;

        rga_buffer_t dst = {};
        dst.fd = dst_fd;
        dst.width = dst_w;
        dst.height = dst_h;
        dst.wstride = dst_stride;
        dst.hstride = dst_h;
        dst.format = RK_FORMAT_RGB_888;

        if (!letterbox) {
            // 直接缩放（不保持宽高比）
            IM_STATUS status = imcvtcolor(src, dst, src.format, dst.format, IM_SYNC);
            if (status != IM_STATUS_SUCCESS) {
                printf("[RGA] cvtcolor failed: %s\n", imStrError(status));
                return -1;
            }
        } else {
            // Letterbox：保持宽高比，居中放置
            float scale_x = (float)dst_w / src_w;
            float scale_y = (float)dst_h / src_h;
            float scale = (scale_x < scale_y) ? scale_x : scale_y;

            int new_w = (int)(src_w * scale);
            int new_h = (int)(src_h * scale);
            int offset_x = (dst_w - new_w) / 2;
            int offset_y = (dst_h - new_h) / 2;

            // 先填充灰色背景 (114, 114, 114)
            im_rect fill_rect = {0, 0, dst_w, dst_h};
            imfill(dst, fill_rect, 0x727272);  // 灰色

            // 缩放 + 转换到目标位置
            im_rect src_rect = {0, 0, src_w, src_h};
            im_rect dst_rect = {offset_x, offset_y, new_w, new_h};

            IM_STATUS status = improcess(src, dst, {}, src_rect, dst_rect, {}, IM_SYNC);
            if (status != IM_STATUS_SUCCESS) {
                printf("[RGA] letterbox resize failed: %s\n", imStrError(status));
                return -1;
            }
        }

        return 0;
    }

    // ============================================================================
    // rgba_to_rgb_resize - RGBA → RGB + 缩放（一步完成）
    // ============================================================================
    // 用于 RGBA 格式图像的预处理（与 nv12_to_rgb_resize 类似）
    // 使用 RGA3 双核心加速（IM_SCHEDULER_RGA3_CORE0 | CORE1）
    // ============================================================================
    static int rgba_to_rgb_resize(int src_fd, int src_w, int src_h, int src_stride,
                                   int dst_fd, int dst_w, int dst_h, int dst_stride,
                                   bool letterbox = true)
    {
        // 配置 RGA3 双核心调度器（提升性能）
        imconfig(IM_CONFIG_SCHEDULER_CORE, IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1);
        
        int src_buf_size = src_stride * src_h;
        int dst_buf_size = dst_stride * dst_h;

        rga_buffer_t src_img, dst_img;
        rga_buffer_handle_t src_handle, dst_handle;

        memset(&src_img, 0, sizeof(src_img));
        memset(&dst_img, 0, sizeof(dst_img));

        int ret = -1;

        // 通过 DMA-BUF fd 导入缓冲区
        src_handle = importbuffer_fd(src_fd, src_w, src_h, RK_FORMAT_RGBA_8888);
        dst_handle = importbuffer_fd(dst_fd, dst_w, dst_h, RK_FORMAT_RGB_888);

        if (src_handle == 0 || dst_handle == 0) {
            printf("importbuffer error\n");
            goto release_buffer;
        }

        src_img = wrapbuffer_handle(src_handle, src_w, src_h, RK_FORMAT_RGBA_8888);
        dst_img = wrapbuffer_handle(dst_handle, dst_w, dst_h, RK_FORMAT_RGB_888);

        if (src_handle == 0 || dst_handle == 0) {
            printf("wrapbuffer_handle error\n");
            goto release_buffer;
        }

        ret = imcheck(src_img, dst_img, (im_rect){}, (im_rect){});
        if (IM_STATUS_NOERROR != ret) {
            printf("%d check error %s\n", __LINE__, imStrError((IM_STATUS)ret));
            goto release_buffer;
        }

        if (!letterbox) {
            // 直接缩放
            IM_STATUS status = imcvtcolor(src_img, dst_img, src_img.format, dst_img.format, IM_SYNC);
            if (status != IM_STATUS_SUCCESS) {
                printf("[RGA] cvtcolor failed: %s\n", imStrError(status));
                return -1;
            }
        } else {
            // Letterbox：保持宽高比，居中放置
            float scale_x = (float)dst_w / src_w;
            float scale_y = (float)dst_h / src_h;
            float scale = (scale_x < scale_y) ? scale_x : scale_y;

            int new_w = (int)(src_w * scale);
            int new_h = (int)(src_h * scale);
            int offset_x = (dst_w - new_w) / 2;
            int offset_y = (dst_h - new_h) / 2;

            im_rect src_rect = {0, 0, src_w, src_h};
            im_rect dst_rect = {offset_x, offset_y, new_w, new_h};

            IM_STATUS status = improcess(src_img, dst_img, {}, src_rect, dst_rect, {}, IM_SYNC);
            if (status != IM_STATUS_SUCCESS) {
                printf("[RGA] letterbox resize failed: %s\n", imStrError(status));
                return -1;
            }
        }

    release_buffer:
        if (src_handle) {
            releasebuffer_handle(src_handle);
        }
        if (dst_handle) {
            releasebuffer_handle(dst_handle);
        }

        return 0;
    }
    
    // ============================================================================
    // resize - 纯缩放（同格式）
    // ============================================================================
    // 参数：
    //   - src_fmt/dst_fmt: 源/目标格式（如 RK_FORMAT_RGB_888）
    // ============================================================================
    static int resize(int src_fd, int src_w, int src_h, int src_stride, int src_fmt,
                      int dst_fd, int dst_w, int dst_h, int dst_stride, int dst_fmt)
    {
        rga_buffer_t src = {};
        src.fd = src_fd;
        src.width = src_w;
        src.height = src_h;
        src.wstride = src_stride;
        src.hstride = src_h;
        src.format = src_fmt;

        rga_buffer_t dst = {};
        dst.fd = dst_fd;
        dst.width = dst_w;
        dst.height = dst_h;
        dst.wstride = dst_stride;
        dst.hstride = dst_h;
        dst.format = dst_fmt;

        IM_STATUS status = imresize(src, dst);
        return (status == IM_STATUS_SUCCESS) ? 0 : -1;
    }

    // ============================================================================
    // sync - 同步等待 RGA 硬件完成（当前为空实现）
    // ============================================================================
    static void sync()
    {
        // imsync();
    }
};

#endif // RGA_CONVERTER_H
