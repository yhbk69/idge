 #ifndef RGA_CONVERTER_H
#define RGA_CONVERTER_H

#include <rga/im2d.h>
#include <rga/im2d.hpp>
#include <drm_fourcc.h>
#include <cstdio>
#include <cstring>
#include <rga/RgaUtils.h>

class RgaConverter
{
public:
    /**
     * NV12 DMA-BUF → RGBA DMA-BUF（零拷贝，fd → fd）
     */
    static bool nv12_to_rgba(int src_fd, int src_w, int src_h, int src_stride,
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

        IM_STATUS status;

        if (src_w == dst_w && src_h == dst_h)
        {
            // 同尺寸：纯色彩转换
            status = imcvtcolor(src, dst, src.format, dst.format, IM_SYNC);
        }
        else
        {
            // 不同尺寸：缩放 + 色彩转换
            im_rect src_rect = {0, 0, src_w, src_h};
            im_rect dst_rect = {0, 0, dst_w, dst_h};
            status = improcess(src, dst, {}, src_rect, dst_rect, {}, IM_SYNC);
        }

        if (status != IM_STATUS_SUCCESS)
        {
            printf("[RGA] nv12_to_rgba failed: %s\n", imStrError(status));
            return false;
        }
        return true;
    }

    static bool convertNV12ToRGBAbyRGA(int src_dma_fd, int dst_dma_fd, int width, int height,
                                      int src_hor_stride, int src_ver_stride)
    {
        int ret = 0;
        int src_width = width;
        int src_height = height;
        int dst_widt = width, dst_height = height;
        int src_format = RK_FORMAT_YCbCr_420_SP;
        int dst_format = RK_FORMAT_RGBA_8888;
        int src_buf_size, dst_buf_size;

        rga_buffer_t src_img, dst_img;
        rga_buffer_handle_t src_handle, dst_handle;

        memset(&src_img, 0, sizeof(src_img));
        memset(&dst_img, 0, sizeof(dst_img));

        src_buf_size = src_hor_stride * src_ver_stride * get_bpp_from_format(src_format);

        dst_buf_size = dst_widt * dst_height * get_bpp_from_format(dst_format);

        src_handle = importbuffer_fd(src_dma_fd, src_buf_size);
        dst_handle = importbuffer_fd(dst_dma_fd, dst_buf_size);

        if (src_handle == 0 || dst_handle == 0)
        {
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

        if (IM_STATUS_NOERROR != ret)
        {
            printf("%d check error %s\n", __LINE__, imStrError((IM_STATUS)ret));
            goto release_buffer;
        }

        ret = imcvtcolor(src_img, dst_img, src_format, dst_format);

        if (ret != IM_STATUS_SUCCESS)
        {
            printf("imcvtcolor error %s\n", imStrError((IM_STATUS)ret));
        }

    release_buffer:
        if (src_handle)
        {
            releasebuffer_handle(src_handle);
        }
        if (dst_handle)
        {
            releasebuffer_handle(dst_handle);
        }

        // // 创建 QImage - RGB888 每像素3字节
        // int bytesPerLine = dstWStride * 3;
        // // 关键改变：直接包装内存而不是深拷贝 (去掉 .copy() ！！！)
        // // 对象池保证了这部分内存至少在接下来的 4 帧期间不会被覆盖，能够完全覆盖 UI 的消费时间。
        // QImage image = QImage(currentVirAddr, m_outWidth, m_outHeight, bytesPerLine, QImage::Format_RGB888);

        return true;
    }

    static bool convertNV12ToRGBbyRGA(int src_dma_fd, int dst_dma_fd, int width, int height,
                                      int src_hor_stride, int src_ver_stride)
    {
        int ret = 0;
        int src_width = width;
        int src_height = height;
        int dst_widt = width, dst_height = height;
        int src_format = RK_FORMAT_YCbCr_420_SP;
        int dst_format = RK_FORMAT_RGB_888;
        int src_buf_size, dst_buf_size;

        rga_buffer_t src_img, dst_img;
        rga_buffer_handle_t src_handle, dst_handle;

        memset(&src_img, 0, sizeof(src_img));
        memset(&dst_img, 0, sizeof(dst_img));

        src_buf_size = src_hor_stride * src_ver_stride * get_bpp_from_format(src_format);

        dst_buf_size = dst_widt * dst_height * get_bpp_from_format(dst_format);

        src_handle = importbuffer_fd(src_dma_fd, src_buf_size);
        dst_handle = importbuffer_fd(dst_dma_fd, dst_buf_size);

        if (src_handle == 0 || dst_handle == 0)
        {
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

        if (IM_STATUS_NOERROR != ret)
        {
            printf("%d check error %s\n", __LINE__, imStrError((IM_STATUS)ret));
            goto release_buffer;
        }

        ret = imcvtcolor(src_img, dst_img, src_format, dst_format);

        if (ret != IM_STATUS_SUCCESS)
        {
            printf("imcvtcolor error %s\n", imStrError((IM_STATUS)ret));
        }

    release_buffer:
        if (src_handle)
        {
            releasebuffer_handle(src_handle);
        }
        if (dst_handle)
        {
            releasebuffer_handle(dst_handle);
        }

        // // 创建 QImage - RGB888 每像素3字节
        // int bytesPerLine = dstWStride * 3;
        // // 关键改变：直接包装内存而不是深拷贝 (去掉 .copy() ！！！)
        // // 对象池保证了这部分内存至少在接下来的 4 帧期间不会被覆盖，能够完全覆盖 UI 的消费时间。
        // QImage image = QImage(currentVirAddr, m_outWidth, m_outHeight, bytesPerLine, QImage::Format_RGB888);

        return true;
    }
    /**
     * NV12 DMA-BUF → XRGB DMA-BUF
     */
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
        dst.format = RK_FORMAT_RGBX_8888;

        IM_STATUS status = imcvtcolor(src, dst, src.format, dst.format, IM_SYNC);
        return status == IM_STATUS_SUCCESS;
    }

    /**
     * 异步转换（高帧率场景）
     */
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

    /**
     * NV12 → RGB + Resize（一步完成，零拷贝）
     * 用于 YOLOv8 预处理
     *
     * @param src_fd      源 NV12 DMA-BUF fd（MPP 解码输出）
     * @param src_w       源宽度
     * @param src_h       源高度
     * @param src_stride  源 stride
     * @param dst_fd      目标 RGB DMA-BUF fd（RKNN 输入）
     * @param dst_w       目标宽度（如 640）
     * @param dst_h       目标高度（如 640）
     * @param dst_stride  目标 stride
     * @param letterbox   是否使用 letterbox（保持宽高比）
     * @return 0 成功
     */
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
            IM_STATUS status = imcvtcolor(src, dst,
                                           src.format, dst.format, IM_SYNC);
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

    /**
     * NV12 → RGB + Resize（一步完成，零拷贝）
     * 用于 YOLOv8 预处理
     *
     * @param src_fd      源 rgba DMA-BUF fd（MPP 解码输出）
     * @param src_w       源宽度
     * @param src_h       源高度
     * @param src_stride  源 stride
     * @param dst_fd      目标 RGB DMA-BUF fd（RKNN 输入）
     * @param dst_w       目标宽度（如 640）
     * @param dst_h       目标高度（如 640）
     * @param dst_stride  目标 stride
     * @param letterbox   是否使用 letterbox（保持宽高比）
     * @return 0 成功
     */
    static int rgba_to_rgb_resize(int src_fd, int src_w, int src_h, int src_stride,
                                   int dst_fd, int dst_w, int dst_h, int dst_stride,
                                   bool letterbox = true)
    {

        imconfig(IM_CONFIG_SCHEDULER_CORE, IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1);
        
        int src_buf_size = src_stride * src_h;//get_bpp_from_format(RK_FORMAT_RGBA_8888);

        int dst_buf_size = dst_stride * dst_h;//get_bpp_from_format(RK_FORMAT_RGB_888);

        rga_buffer_t src_img, dst_img;
        rga_buffer_handle_t src_handle, dst_handle;

        memset(&src_img, 0, sizeof(src_img));
        memset(&dst_img, 0, sizeof(dst_img));

        int ret = -1;

        src_handle = importbuffer_fd(src_fd, src_w, src_h, RK_FORMAT_RGBA_8888);
        dst_handle = importbuffer_fd(dst_fd, dst_w, dst_h, RK_FORMAT_RGB_888);


        if (src_handle == 0 || dst_handle == 0)
        {
            printf("importbuffer error\n");
            goto release_buffer;
        }

        src_img = wrapbuffer_handle(src_handle, src_w, src_h, RK_FORMAT_RGBA_8888);
        dst_img = wrapbuffer_handle(dst_handle, dst_w, dst_h, RK_FORMAT_RGB_888);

        if (src_handle == 0 || dst_handle == 0)
        {
            printf("wrapbuffer_handle error\n");
            goto release_buffer;
        }

        ret = imcheck(src_img, dst_img, (im_rect){}, (im_rect){});

        if (IM_STATUS_NOERROR != ret)
        {
            printf("%d check error %s\n", __LINE__, imStrError((IM_STATUS)ret));
            goto release_buffer;
        }


        if (!letterbox) {
            // 直接缩放（不保持宽高比）
            IM_STATUS status = imcvtcolor(src_img, dst_img,
                                           src_img.format, dst_img.format, IM_SYNC);
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
            //imfill(dst_img, fill_rect, 0x727272);  // 灰色

            // 缩放 + 转换到目标位置
            im_rect src_rect = {0, 0, src_w, src_h};
            im_rect dst_rect = {offset_x, offset_y, new_w, new_h};

            IM_STATUS status = improcess(src_img, dst_img, {}, src_rect, dst_rect, {}, IM_SYNC);
            if (status != IM_STATUS_SUCCESS) {
                printf("[RGA] letterbox resize failed: %s\n", imStrError(status));
                return -1;
            }
        }

    release_buffer:
        if (src_handle)
        {
            releasebuffer_handle(src_handle);
        }
        if (dst_handle)
        {
            releasebuffer_handle(dst_handle);
        }

        return 0;
    }
    
    
    /**
     * 纯 Resize（同格式）
     */
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
    static void sync()
    {
        // imsync();
    }
};

#endif // RGA_CONVERTER_H
