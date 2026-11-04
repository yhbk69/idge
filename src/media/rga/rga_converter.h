/**
 * @file rga_converter.h
 * @brief media/rga 域 —— RGA2/RGA3 硬件 2D 加速的静态封装（零拷贝三条路径都在这里）
 */
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
#include <QElapsedTimer>     // 可选耗时分段出参用
#include <rga/RgaUtils.h>    // RGA 工具函数（get_bpp_from_format 等）

/**
 * @brief RGA 硬件作业的一层薄封装：全静态、无成员、无状态，因此本类自身没有
 *        线程安全约束——约束全部落在**入参口径**和**驱动全局状态**上。
 *
 * 两条等价但开销不同的 API 风格（选哪条取决于缓冲是不是常驻）：
 *   - 轻量版（直接把 fd 塞进 rga_buffer_t.fd）：nv12_to_rgba / nv12_to_xrgb /
 *     nv12_to_rgba_async / nv12_to_rgb_resize / resize。每次作业都由驱动现场
 *     pin 一次 fd，且**不显式 release**，注册会累积到进程退出才由内核兜底
 *     （dmesg "Destroy handle[...] when the user exits"）。
 *   - 句柄版（importbuffer_fd → wrapbuffer_handle → im* → releasebuffer_handle）：
 *     convertNV12ToRGBAbyRGA / convertNV12ToRGBbyRGA / rgba_to_nv12 /
 *     rgba_to_rgb_resize。驱动缓存 fd→物理地址表，适合逐帧反复使用的同一块缓冲。
 *
 * 三条必须记住的口径（都有板端实测依据，详见各函数与 rga/README.md）：
 *   1. **wstride 一律是"像素/行"，不是字节 pitch**——内核按
 *      wstride*hstride*bpp/8 计算所需字节再 map dma_buf，误传字节 pitch 时
 *      报的是 "required ... byte but only got ..."，用户态只看到笼统的
 *      "Failed to call RockChipRga interface"。
 *   2. NV12 的宽高必须是**偶数**（4:2:0 色度 2×2 共享一个 UV），
 *      且 format 必须与缓冲的实际内存排布一致，否则是按字节错读而不是报错。
 *   3. 轻量版把 hstride 硬写成 height，因此**不认 vstride > height 的 MPP 帧**
 *      （MPP 会把 UV 平面按对齐向上取整）；这类帧要走句柄版并显式传 ver_stride。
 *
 * ⚠ 本类的 rgba_to_rgb_resize 会调 imconfig()，那是**进程级全局**调度器状态，
 *   一次生效于后续所有 im* 调用，多线程下后写者覆盖前者——不是本函数私有。
 */
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
    /**
     * @brief NV12 DMA-BUF → RGBA DMA-BUF（轻量 fd 直填版，同步返回）
     * @param src_fd/src_w/src_h 源 NV12 缓冲；宽高**必须为偶数**（4:2:0 色度 2×2 共享），
     *             奇数尺寸在驱动侧是参数非法，不是"少一列"
     * @param src_stride 源行跨度，**像素/行口径**（NV12 一字节一像素时才与字节 pitch 同值）；
     *             本函数直接塞进 wstride，不做任何换算，传字节 pitch 会得到
     *             "required N byte but only got M" 这种难以归因的内核错误
     * @param dst_fd/dst_w/dst_h/dst_stride 目标 RGBA 缓冲；dst_stride 同样按像素口径
     * @return true = 作业成功（IM_SYNC，返回时像素已落盘）；false = 驱动失败，
     *         目标缓冲内容未定义（可能是上一帧残留，调用方不可"部分使用"）
     * @note hstride 被写死成 height：不认 vstride>height 的帧，MPP 输出请用
     *       convertNV12ToRGBAbyRGA()。
     * @note 目标格式固定 RGBA_8888，不做格式选择；且本函数**不调 imsetColorSpace**，
     *       色彩矩阵走驱动默认，而句柄版显式设成 BT.709 limited → RGB full。
     *       两条路径混用同一段帧会看到轻微色偏——本仓监控主链路只走句柄版，
     *       本函数当前无调用方（grep `RgaConverter::nv12_to_rgba` 零命中）。
     */
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
    // 【返回值语义（本轮已修复）】此前末尾无条件 return true，import/imcheck/
    //   转换失败全部报成功，调用方判断完全失效；现各失败路径均返回 false。
    // 【t_import_ns / t_cvt_ns】可选出参，用于把"导入句柄"和"真正转换"拆开计量：
    //   本函数每帧都 import+release，而目标缓冲其实全程只有两个、fd 不变，
    //   所以要先知道 import 占比，才能判断缓存句柄值不值（见调用侧探针）。
    // ============================================================================
    /**
     * @brief NV12 DMA-BUF → RGBA DMA-BUF（句柄版，同步；监控主链路每帧走这一条）
     * @param src_dma_fd 源 NV12 的 DMA-BUF fd（AVFrame->buf[0] 派生出来的那个，
     *             **归 FFmpeg 所有，禁止在本函数之外或之内 close**）
     * @param dst_dma_fd 目标 RGBA 的 DMA-BUF fd（通常是 DmaFrameBuffer 的双缓冲之一）
     * @param width/height 目标尺寸，同时被当源尺寸用 —— **本函数不做缩放**，
     *             src/dst 尺寸不同要走 nv12_to_rgb_resize 那类带 rect 的接口
     * @param src_hor_stride 源 Y 平面行跨度（像素口径；NV12 数值上等于字节 pitch）
     * @param src_ver_stride 源垂直跨度（行数），同时进 hstride 与导入字节数。
     *             正口径是 MPP 的真实 vstride（planes[1].offset-planes[0].offset
     *             再除 pitch，见 camera_preview_decoder 的做法）；现网
     *             ffmpeg_video_decoder.cpp 传的是 frame->height，隐含"MPP 输出无垂直
     *             填充"的假设，该风险已在调用侧注释登记（有填充时表现为偏色/横条纹）。
     * @param source_rga_format 源格式，默认 NV12(RK_FORMAT_YCbCr_420_SP)；
     *             只允许与缓冲实际排布一致的 FourCC，传错是错读像素而非报错
     * @param t_import_ns/t_cvt_ns 可选出参（可为 nullptr）：分别回填"两次 importbuffer_fd"
     *             与"imcvtcolor 本体"的耗时纳秒，两者互斥地只在至少传了一个时才起表
     * @return true = 全链成功；false = import 失败 / imcheck 参数非法 / 转换失败，
     *         三种都只在 stdout 打一行，**不区分原因**；无论走到哪一步失败，
     *         已 import 的两个句柄都由 release_buffer 标签统一归还（无句柄泄漏）
     * @note 目标导入尺寸按 `width*height*4` 紧凑算，**没有** dst stride 入参——
     *       目标缓冲必须是紧凑 RGBA；若将来要支持有 stride 的 dst，得加参数而不是改常量。
     * @note 每次调用都 import+release 一对句柄：常驻双缓冲其实只有两块、fd 不变，
     *       这是刻意的保守做法还是该缓存句柄，由 t_import_ns 的数据决定（见上面的探针说明）。
     */
    static bool convertNV12ToRGBAbyRGA(int src_dma_fd, int dst_dma_fd, int width, int height,
                                      int src_hor_stride, int src_ver_stride,
                                      int source_rga_format = RK_FORMAT_YCbCr_420_SP,
                                      long long *t_import_ns = nullptr,
                                      long long *t_cvt_ns = nullptr)
    {
        int ret = 0;
        int src_width = width;
        int src_height = height;
        int dst_widt = width, dst_height = height;
        int src_format = source_rga_format;  // NV12(默认) 或 NV21 等半平面 YUV
        int dst_format = RK_FORMAT_RGBA_8888;      // RGBA
        int src_buf_size, dst_buf_size;
        bool ok = false;

        rga_buffer_t src_img, dst_img;
        rga_buffer_handle_t src_handle = 0, dst_handle = 0;
        // 计时器声明在所有 goto 之前：goto 跳过带构造函数的对象初始化是非法的
        QElapsedTimer impClock, cvtClock;

        memset(&src_img, 0, sizeof(src_img));
        memset(&dst_img, 0, sizeof(dst_img));

        // 计算缓冲区大小（行跨度 × 高度 × 每像素字节数）
        // 【实测】get_bpp_from_format 返回字节/像素（BGR_888→3、NV12→1.5），
        // 并非位数；勿再"顺手 /8"（2026-09-24 曾误改导致导入失败）。
        src_buf_size = src_hor_stride * src_ver_stride * get_bpp_from_format(src_format);
        dst_buf_size = dst_widt * dst_height * get_bpp_from_format(dst_format);

        // 通过 DMA-BUF fd 导入缓冲区到 RGA
        if (t_import_ns || t_cvt_ns) impClock.start();
        src_handle = importbuffer_fd(src_dma_fd, src_buf_size);
        dst_handle = importbuffer_fd(dst_dma_fd, dst_buf_size);
        if (t_import_ns) *t_import_ns = impClock.nsecsElapsed();


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
        if (t_cvt_ns) cvtClock.start();
        ret = imcvtcolor(src_img, dst_img, src_format, dst_format);
        if (t_cvt_ns) *t_cvt_ns = cvtClock.nsecsElapsed();
        if (ret != IM_STATUS_SUCCESS) {
            printf("imcvtcolor error %s\n", imStrError((IM_STATUS)ret));
            goto release_buffer;
        }

        ok = true;  // 仅全部步骤成功才置真

    release_buffer:
        // 释放 RGA 缓冲区句柄
        if (src_handle) {
            releasebuffer_handle(src_handle);
        }
        if (dst_handle) {
            releasebuffer_handle(dst_handle);
        }

        return ok;
    }

    // ============================================================================
    // convertNV12ToRGBbyRGA - NV12 → RGB 转换（3 字节/像素）
    // ============================================================================
    // 与 convertNV12ToRGBAbyRGA 类似，但输出 RGB888 格式（3字节/像素）
    // 适用于需要更小内存占用的场景
    // 【返回值语义（本轮已修复）】与 RGBA 版同步整改：失败路径返回 false。
    // ⚠ RGB888 行需按像素三元组排布，RGA 对 wstride 有额外对齐要求，
    //   输出缓冲若按 width*3 紧凑分配可能触发 imcheck 失败（现在调用方
    //   能通过返回 false 感知）。
    // ============================================================================
    /**
     * @brief NV12 DMA-BUF → RGB888 DMA-BUF（句柄版；与 RGBA 版同一条链，只是目标 3 字节/像素）
     * @param src_dma_fd/dst_dma_fd/width/height/src_hor_stride/src_ver_stride
     *             口径与 convertNV12ToRGBAbyRGA() **完全一致**（那份注释里的约束同样适用），
     *             区别是本函数没有 source_rga_format 入参 —— 源格式**写死 NV12**，
     *             NV21 之类的流走这里会静默错读色度顺序，需要时得先加参数。
     * @return true = 成功；false = import/imcheck/转换任一失败（句柄仍已归还）
     * @note 本函数当前无调用方（推理输入实际由 rgba_to_rgb_resize 从已画框的 RGBA 产出）。
     *       RGB888 的 dst 导入尺寸按 width*height*3 紧凑算，没有 stride 入参，
     *       所以目标缓冲必须满足 RGA 对 3 字节行的对齐要求，否则只在 imcheck 处失败。
     */
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
        bool ok = false;

        rga_buffer_t src_img, dst_img;
        rga_buffer_handle_t src_handle = 0, dst_handle = 0;

        memset(&src_img, 0, sizeof(src_img));
        memset(&dst_img, 0, sizeof(dst_img));

        // 缓冲区字节数 = 跨度 × 高度 × 每像素字节数（get_bpp 返回字节，非位）
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
            goto release_buffer;
        }

        ok = true;  // 仅全部步骤成功才置真

    release_buffer:
        if (src_handle) {
            releasebuffer_handle(src_handle);
        }
        if (dst_handle) {
            releasebuffer_handle(dst_handle);
        }

        return ok;
    }

    // ============================================================================
    // rgba_to_nv12 - RGBA → NV12 色彩转换（零拷贝，硬编前处理）
    // ============================================================================
    // 用途：显示侧已把检测框画在 RGBA DMA-BUF 上，编码器（h264/hevc_rkmpp）
    //   要 NV12；用 RGA 做 RGBA→NV12 可让像素全程不过 CPU（替代 sws_scale）。
    //
    // 参数（⚠ 单位是"像素/行"，不是字节——见下方实测结论）：
    //   - src_wstride_px: RGBA 行跨度（像素）。32bpp 缓冲 = 字节pitch / 4，
    //     DmaFrameBuffer::stride() 返回的是字节 pitch，必须除以 4 再传入。
    //   - dst_wstride_px/dst_hstride_lines: NV12 Y 平面行跨度（像素）与垂直
    //     跨度（行数）。NV12 的 Y 平面是 1 字节/像素，所以 AVDRM 的
    //     planes[0].pitch（字节）在数值上就等于像素跨度，可直接传；
    //     vstride 用 planes[1].offset - planes[0].offset 再除 pitch 反推，
    //     不要用 height（MPP 会把 UV 平面按对齐向上取整）。
    //
    // 【实测（2026-09-29，板端 /tmp/rga_probe）】RGA 内核按
    //   required_bytes = wstride * hstride * bpp/8 计算并 map dma_buf，
    //   因此 wstride 一律按像素解释。误传字节 pitch 时 dmesg 报
    //   "Only get buffer 921600 byte ... but current image required 3686400
    //   byte / memory param: w = 2560"（2560 即 640px×4），用户态只会看到
    //   笼统的 "Failed to call RockChipRga interface"。
    //   同类口径的本类轻量 fd 直填函数（nv12_to_rgba / nv12_to_xrgb /
    //   nv12_to_rgba_async / resize）当前无调用方；若将来启用，注意它们
    //   把调用方给的 stride 原样塞进 wstride，同样要求像素口径。
    //
    // 色彩范围：RGB full(0-255) → YUV BT.709 limited(16-235)，与本类反方向
    //   转换（convertNV12ToRGBAbyRGA）保持同一约定，避免来回转换后色阶漂移。
    //
    // 【为什么走 importbuffer_fd + releasebuffer_handle，而不是直填 fd】
    //   板端实测（2026-09-29，4 路 720p 常驻录制 12s）：直填 fd 时内核日志刷出
    //   "rga_mm: Destroy handle[...] when the user exits" 512 条，而同场景关闭
    //   录制只有 16 条——每个 fd 型任务都会在驱动里注册一个 buffer，不显式
    //   release 就要等进程退出由内核兜底回收，等于把常驻编码的整条链路都泄漏
    //   在驱动里。import 拿到 handle、用完 release 与 convertNV12ToRGBAbyRGA
    //   的既有口径一致，且 handle 让驱动缓存 fd→物理地址表，省掉重复 pin/unpin。
    // ============================================================================
    /**
     * @brief 已画框的 RGBA DMA-BUF → NV12 DMA-BUF（硬编前处理，同步返回）
     * @param src_fd/src_w/src_h 源 RGBA 缓冲（DmaFrameBuffer，内容是"这一帧 + 检测框"）
     * @param src_wstride_px RGBA 行跨度，**像素/行**：调用方给的是字节 pitch，
     *             必须 /4 之后再传（32bpp）。见 VideoRecorder::encodeFrame() 的换算。
     * @param dst_fd/dst_w/dst_h 目标 NV12 缓冲（rkmpp 编码器帧池里的那一个）
     * @param dst_wstride_px NV12 Y 平面行跨度（像素）。Y 平面 1 字节/像素，
     *             所以 AVDRM 的 planes[0].pitch（字节）数值上可直接传。
     * @param dst_hstride_lines NV12 垂直跨度（行数），用 planes[1].offset-planes[0].offset
     *             反推，**不要传 height**：MPP 会把 UV 平面按对齐向上取整，
     *             传小了会让导入字节数不足、内核 map 失败。
     * @return true = 转换成功（IM_SYNC，返回即像素可用）；false = 入参不合理 /
     *         import 失败 / 驱动作业失败。三种都只 printf 一行，不区分。
     *         返回 false 时目标缓冲内容未定义 —— 调用方（VideoRecorder）按
     *         "丢这一帧并计入 droppedFrameCount()"处理，绝不重试也不阻塞解码线程。
     * @note 入口的三条区间检查（src_wstride_px<src_w 等）就是字节/像素口径搞混的
     *       防呆：字节 pitch 误传进来时 wstride 恰是宽度的 4 倍，只会在内核里留下
     *       一条 "map 失败" 日志，现场无法归因，所以在用户态先拦。
     *       检查只兜住"传大了"，**传小了（比如按 height 传了 vstride）拦不住**，
     *       那种情况仍要靠上面的 dst_hstride_lines 口径。
     * @note 4 路常驻时 RGA 作业会排队，本函数偶发返回 false（实测 12s 内约 1 次量级），
     *       属预期噪声，不要在这里加循环重试（见 rga/README.md 最后一条）。
     */
    static bool rgba_to_nv12(int src_fd, int src_w, int src_h, int src_wstride_px,
                             int dst_fd, int dst_w, int dst_h,
                             int dst_wstride_px, int dst_hstride_lines)
    {
        // 像素口径自检：字节 pitch 误传进来时 wstride 会是宽度的 4 倍，
        // 这里直接拦下，避免只留下一条无法定位的内核 map 失败日志。
        if (src_wstride_px < src_w || dst_wstride_px < dst_w ||
            dst_hstride_lines < dst_h) {
            printf("[RGA] rgba_to_nv12 bad stride(px): src=%d/%d dst=%d/%d\n",
                   src_wstride_px, src_w, dst_wstride_px, dst_hstride_lines);
            return false;
        }

        // 导入尺寸按各格式的实际字节数算：RGBA 4 字节/像素，NV12 1.5 字节/像素
        // （wstride 是像素、hstride 是行，故 NV12 用 *3/2 而非 bpp 查表）。
        int src_handle = importbuffer_fd(src_fd, src_wstride_px * src_h * 4);
        int dst_handle = importbuffer_fd(dst_fd, dst_wstride_px * dst_hstride_lines * 3 / 2);
        if (src_handle == 0 || dst_handle == 0) {
            printf("[RGA] rgba_to_nv12 importbuffer failed: src=%d dst=%d\n",
                   src_handle, dst_handle);
            if (src_handle)
                releasebuffer_handle(src_handle);
            if (dst_handle)
                releasebuffer_handle(dst_handle);
            return false;
        }

        rga_buffer_t src = wrapbuffer_handle(src_handle, src_w, src_h,
                                             RK_FORMAT_RGBA_8888, src_wstride_px, src_h);
        rga_buffer_t dst = wrapbuffer_handle(dst_handle, dst_w, dst_h,
                                             RK_FORMAT_YCbCr_420_SP,  // NV12
                                             dst_wstride_px, dst_hstride_lines);

        imsetColorSpace(&src, IM_RGB_FULL);
        imsetColorSpace(&dst, IM_YUV_BT709_LIMIT_RANGE);

        IM_STATUS status = imcvtcolor(src, dst, src.format, dst.format, IM_SYNC);

        releasebuffer_handle(src_handle);
        releasebuffer_handle(dst_handle);

        if (status != IM_STATUS_SUCCESS) {
            printf("[RGA] rgba_to_nv12 failed: %s\n", imStrError(status));
            return false;
        }
        return true;
    }

    // ============================================================================
    // nv12_to_xrgb - NV12 → XRGB 色彩转换
    // ============================================================================
    // XRGB = 32 位，最高字节未使用（X = padding）
    // ============================================================================
    /**
     * @brief NV12 → XRGB(RGBX_8888) 转换，轻量 fd 直填
     * @param 参数口径同 nv12_to_rgba()（stride 为像素口径、hstride 写死 height）
     * @return status == IM_STATUS_SUCCESS；失败时目标缓冲内容未定义
     * @note 与 nv12_to_rgba 的唯一区别是目标 FourCC（RK_FORMAT_RGBX_8888 vs RGBA_8888），
     *       两者在 RGA 侧都是 32bpp、通道排布不同。选错**不会报错**，只会按字节
     *       错读通道（颜色不对），与本类"format 必须与内存排布一致"的总口径同一条。
     * @note 全工程零调用方；直填 fd 会累积驱动注册（见类头"轻量版"那条）。
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
    /**
     * @brief NV12 → RGBA 的**异步入队**版本
     * @param 参数口径同 nv12_to_rgba()
     * @return true 只代表"作业已进驱动队列"，**不代表转换完成**；
     *         false = 连入队都失败（参数非法/驱动拒绝）
     * @note ⚠ 返回 true 后立刻读 dst_fd 会拿到半成品像素。必须自行 imsync()，
     *       本类的 sync() 是空实现（imsync 被注释），拿它当栅栏等于没有栅栏。
     * @note 全工程零调用方。留着的唯一价值是"将来要做 RGA 流水线"时的入口；
     *       真要启用，同步点得连调用方一起设计，不能只改这里。
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
    /**
     * @brief NV12 → RGB888 + 缩放（一步完成，零拷贝），YOLO 模型输入的预处理入口之一
     * @param src_fd/src_w/src_h/src_stride 源 NV12；stride 像素口径，hstride 写死 height
     * @param dst_fd/dst_w/dst_h/dst_stride 目标 RGB888（现网是 640×640、stride=640*3）
     *             【640 的来源】YOLO11 导出时固定的输入 attr（.rknn 的 width×height），
     *             不是可调参数；改它要连同 .rknn 一起重新转换，否则 NPU 侧直接拒。
     * @param letterbox true（默认）= 等比缩放 + 居中补 0x727272；false = 走 imcvtcolor，
     *             见上方 ⚠：那条分支其实**不缩放**，别当成"拉伸模式"用。
     * @return 0 = 成功；-1 = 驱动作业失败（打印 imStrError）。**不返回 IM_STATUS 原值**，
     *         调用方只能判"成/败"，想知道原因得看 stdout。
     * @note 目标缓冲不会被本函数清零：letterbox 分支先 imfill 整幅补灰再写 dst_rect，
     *       所以无边残留；但 fd 直填路径每次都在驱动里注册缓冲，逐帧用会累积
     *       （模型输入这条路径现网走的是 rgba_to_rgb_resize，见 ffmpeg_video_decoder 4.7 节）。
     * @note (int) 截断 new_w/new_h 可能比理论值小 1px，居中偏移随之有 1px 抖动；
     *       叠加 NV12 要求偶数宽高，奇数源尺寸时会有 1px 级别的坐标失配 —— 
     *       坐标还原靠 letterbox_t{x_pad,y_pad,scale}，别按"整数对齐"去反推。
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
    // 【返回值语义（本轮已修复）】失败路径统一 goto release_buffer 归还
    //   RGA 句柄后再返回 -1（此前 import/imcheck 失败走标签尾返回旧值、
    //   转换失败直接 return -1 跳过句柄释放，两处均已纠正）；
    //   成功返回 0。调用方可以且应当检查返回值。
    // ⚠ 第二处 `if (src_handle == 0 || dst_handle == 0)`（wrapbuffer 后）
    //   是死代码：handle 非零已在第一处 goto 前判过，此处恒为假。
    // ⚠ src_buf_size/dst_buf_size 计算后未使用（importbuffer_fd 走的是
    //   宽高+格式重载），保留自旧版本，勿据此推断缓冲尺寸校验存在。
    // ============================================================================
    /**
     * @brief RGBA → RGB888 + letterbox 缩放（一步完成），**监控主链路每帧走这一条**
     * @param src_fd/src_w/src_h 源 RGBA（已画框的显示缓冲，或抓拍用的原始显示帧）
     * @param src_stride 名义上是源行跨度，但本函数把它只用于那条"算完没用的
     *             src_buf_size"（importbuffer_fd 走的是 (fd,w,h,format) 重载），
     *             所以现网两个调用点分别传字节 pitch 与 w*4 都"没出事"。
     *             NOTE: 这是运气而不是契约——一旦改用带 stride 的重载，两处口径必须
     *             同时换成像素口径，否则就是本文件第一条实测结论里那个 map 失败。
     * @param dst_fd/dst_w/dst_h 目标 RGB888（640×640，来自 DmaBufferPool，模型输入）
     * @param dst_stride 同上，当前未参与导入尺寸计算，但被拿去做那条无用乘法。
     * @param letterbox true=等比缩放居中（**不补边**，理由见上方"不可用 imfill"）；
     *             false=imcvtcolor 纯转换不缩放，SnapWriter 用这一档做同尺寸 RGBA→RGB。
     * @return 0=成功；-1=任一失败（import/imcheck/作业）。两种失败都经
     *         release_buffer 标签归还句柄，调用方可以且应当判返回值。
     * @note 目标格式在本函数里**写死 RK_FORMAT_RGB_888**，而两个调用点的池是按
     *       RK_FORMAT_BGR_888 建的（只用于算 size，3 字节/像素与 RGB 相同，
     *       所以分配不会出错）。也就是说缓冲池元信息里的 format 与实际像素排布
     *       不一致，下游一律按 image_buffer_t::format = IMAGE_FORMAT_RGB888 读；
     *       要改成真 BGR 得同时改这里的 wrapbuffer 与 RKNN 输入约定，别只改池参数。
     * @note letterbox=true 时 dst 的边条残留的是池内**上一帧**像素（没有 imfill），
     *       属已接受的轻微噪声；若哪天要求干净补边，得先解决 imfill 对
     *       importbuffer_fd 句柄缓冲返回 EINVAL 那个问题。
     */
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
                goto release_buffer;  // ret 保持 -1，但句柄必须归还
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

            // 【不可用 imfill】RGA 的 COLORFILL 对 importbuffer_fd 导入的
            // RGB888 handle 缓冲返回 EINVAL（无效的参数），每帧刷屏
            // "RgaCollorFill fail"。参照 caichao 去掉填充：improcess 会把
            // 等比缩放后的画面写进 dst_rect，letterbox 边残留的池内上一帧
            // 像素是模型输入的轻微噪声，远好过每帧失败 ioctl+刷日志。
            //im_rect fill_rect = {0, 0, dst_w, dst_h};
            //imfill(dst_img, fill_rect, 0x727272);  // 灰色

            im_rect src_rect = {0, 0, src_w, src_h};
            im_rect dst_rect = {offset_x, offset_y, new_w, new_h};

            IM_STATUS status = improcess(src_img, dst_img, {}, src_rect, dst_rect, {}, IM_SYNC);
            if (status != IM_STATUS_SUCCESS) {
                printf("[RGA] letterbox resize failed: %s\n", imStrError(status));
                goto release_buffer;  // ret 保持 -1，但句柄必须归还
            }
        }

        ret = 0;  // 转换成功

    release_buffer:
        if (src_handle) {
            releasebuffer_handle(src_handle);
        }
        if (dst_handle) {
            releasebuffer_handle(dst_handle);
        }

        return ret;
    }
    
    // ============================================================================
    // resize - 纯缩放（同格式）
    // ============================================================================
    // 参数：
    //   - src_fmt/dst_fmt: 源/目标格式（如 RK_FORMAT_RGB_888）
    // ============================================================================
    /**
     * @brief 纯缩放（imresize），格式由调用方给
     * @param src_fd/src_w/src_h/src_stride/src_fmt 源缓冲与其几何、FourCC
     * @param dst_fd/dst_w/dst_h/dst_stride/dst_fmt 目标缓冲与其几何、FourCC
     * @return 0 = 成功；-1 = 驱动失败（注意：本函数**不打印** imStrError，
     *         失败时现场什么都看不到，只有返回值）
     * @note src_fmt/dst_fmt 必须与各自缓冲的真实排布一致：imresize 不做色彩转换，
     *       格式写错是按字节错读（画面花），不会返回错误。
     *       stride 口径同本类其它轻量版（像素/行），hstride 写死 height。全工程零调用方。
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

    // ============================================================================
    // sync - 同步等待 RGA 硬件完成（当前为空实现）
    // ============================================================================
    /**
     * @brief 占位：当前是**空函数**，imsync() 被注释掉了
     * @note ⚠ 不要把它当同步栅栏用——调它等于什么都不做。
     *       本类现有被调用的转换全部带 IM_SYNC（同步语义由驱动保证），所以现状是安全的；
     *       一旦有人启用 nv12_to_rgba_async()，必须在这里恢复 imsync() 或让调用方自调，
     *       否则会读到半成品像素而不是报错（这类 bug 在现场表现为"偶发半帧画面"）。
     *       刻意留着注释而非删掉本函数：它是"异步路径欠一个栅栏"的可见提醒。
     */
    static void sync()
    {
        // imsync();
    }
};

#endif // RGA_CONVERTER_H
