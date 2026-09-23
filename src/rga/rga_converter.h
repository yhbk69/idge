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
    //
    // 【import 后再 wrap 的原因】importbuffer_fd 把 DMA-BUF fd 注册进 RGA
    //   驱动得到 handle（驱动内部做了 fd→物理地址表缓存，之后每次任务
    //   免重复 pin/unpin），wrapbuffer_handle 才生成带几何信息的 rga_buffer_t。
    //   与 nv12_to_rgba 直接填 fd 的轻量路径相比，适合同一缓冲反复使用的场景。
    //
    // ⚠⚠【返回值恒为真陷阱（上轮审查确认，仅警示不改代码）】
    //   本函数末尾无条件 `return true`：importbuffer 失败、imcheck 失败、
    //   甚至 imcvtcolor 失败（ret!=SUCCESS 只打印不改变返回）全部返回 true。
    //   调用方以此判断转换成败的逻辑完全失效——失败时输出缓冲保持旧内容/
    //   未定义内容，画面表现为"显示上一帧"或花屏，且无错误可查。
    //   （convertNV12ToRGBbyRGA 同病。）使用方切勿依赖返回值。
    // ⚠ src_buf_size 用 hor_stride*ver_stride*bpp 计算，NV12 的 bpp=12
    //   （位/像素）时 RgaUtils 返回的是"位宽"，importbuffer_fd(fd, size)
    //   重载内部按字节处理，两处约定不同勿混用。
    // ============================================================================
    static bool convertNV12ToRGBAbyRGA(int src_dma_fd, int dst_dma_fd, int width, int height,
                                      int src_hor_stride, int src_ver_stride,
                                      int source_rga_format = RK_FORMAT_YCbCr_420_SP)
    {
        int ret = 0;
        int src_width = width;
        int src_height = height;
        int dst_widt = width, dst_height = height;
        int src_format = source_rga_format;  // NV12(默认) 或 NV21 等半平面 YUV
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
    // 与 convertNV12ToRGBAbyRGA 类似，但输出 RGB888 格式（3字节/像素）
    // 适用于需要更小内存占用的场景
    // ⚠ 继承同一缺陷：所有失败路径同样 `return true`（恒真返回值），
    //   且 RGB888 行需按像素三元组排布，RGA 对 wstride 有额外对齐要求，
    //   输出缓冲若按 width*3 紧凑分配可能触发 imcheck 失败（但没人看得见）。
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
    //
    // ⚠【异步语义陷阱】IM_ASYNC 下 imcvtcolor 返回 SUCCESS 仅表示
    //   "任务已入队"，不代表转换完成；返回 true 后立即读取 dst_fd
    //   会读到半成品像素。调用方必须先 imsync() 等待硬件栅栏。
    //   而本类的 sync() 是空实现（imsync 被注释掉），配合
    //   nv12_to_rgba（IM_SYNC）使用的同步语义由驱动隐式保证——
    //   任何使用本异步函数的代码都必须自行显式调用 imsync()。
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
    //
    // 【魔法数字】imfill 的 0x727272：0x72=114，即 Ultralytics YOLO
    //   预处理标准灰 (114,114,114)；RGB888 三字节同值，故按字填充。
    //   scale 取 min(dst_w/src_w, dst_h/src_h)，new_w/new_h 用 (int) 截断，
    //   可能比理论值小 1 像素；NV12 源要求宽高为偶数（4:2:0 色度
    //   2x2 共享），奇数尺寸输入时裁剪/填充坐标可能失配，产生 1px 偏移。
    // ⚠ 非 letterbox 分支实际调用的是 imcvtcolor（纯色彩转换，不缩放！）：
    //   src/dst 尺寸不同时结果取决于驱动行为，"直接拉伸"预期不成立；
    //   需要拉伸应走 improcess（同函数 letterbox 分支的做法）。
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
    //
    // 【imconfig 是进程全局】双核调度配置一次生效于后续所有 im* 调用，
    //   并非本函数私有——多线程下后写者覆盖前者，注意跨模块干扰。
    //
    // ⚠⚠【错误路径句柄泄漏（上轮审查确认，仅警示不改代码）】
    //   letterbox / 非 letterbox 两分支中 improcess / imcvtcolor 失败时
    //   直接 `return -1`，跳过了函数尾的 release_buffer 标签——
    //   此前 importbuffer_fd 得到的 src_handle/dst_handle 未释放，
    //   RGA 驱动内的 fd 注册表与内核资源随之泄漏（泄漏累积后
    //   importbuffer 会开始失败）。正确路径应 goto release_buffer。
    // ⚠ 反之，走 release_buffer 收尾的路径末尾无条件 `return 0`：
    //   import 失败/imcheck 失败也报"成功"——返回值仅"转换执行失败"
    //   一种错误可信，调用方不能以返回值判断转换质量。
    // ⚠ 第二处 `if (src_handle == 0 || dst_handle == 0)`（wrapbuffer 后）
    //   是死代码：handle 非零已在第一处 goto 前判过，此处恒为假。
    // ⚠ src_buf_size/dst_buf_size 计算后未使用（importbuffer_fd 走的是
    //   宽高+格式重载），保留自旧版本，勿据此推断缓冲尺寸校验存在。
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
