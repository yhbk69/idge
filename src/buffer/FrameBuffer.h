/**
 * @file FrameBuffer.h
 * @brief 帧缓冲区数据结构定义
 *
 * 作用：定义用于存储解码/处理后的视频帧数据的结构体 DmaFrameBuffer。
 *       包含帧的尺寸、格式、虚拟地址、DMA 缓冲区引用等元信息。
 *       注意：此文件中的 DmaFrameBuffer 是简单数据结构，与 DmaFrameBuffer.h 中的类不同。
 */

#pragma once
// RGA 头文件不能放在 extern "C" 块中，因为包含 C++ inline 函数
#include <rga/rga.h>
#include <rga/RgaApi.h>
#include <rga/im2d.h>
#include <rga/RgaUtils.h>
#include <mutex>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <QMetaType>

// FFmpeg C 头文件需要在 extern "C" 块中包含
extern "C" {
#include "libavutil/imgutils.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

}
#include <memory>
#include <drm_fourcc.h>

// ============================================================================
// DmaFrameBuffer 结构体 —— 帧缓冲区数据载体
// ============================================================================
// 作用：存储单帧视频数据的元信息，包括尺寸、格式、内存地址和 DMA 缓冲区引用。
//       作为帧队列中传递的数据单元使用。
// ============================================================================
class DmaFrameBuffer
{
public:

    // 如果 virt_address 是已经转换的数据，srcWidth 是转换前的 width 和 height
    int srcWidth;              // 原始帧宽度（转换前）
    int srcHeight;             // 原始帧高度（转换前）
    int width;                 // 当前帧宽度（可能经过缩放）
    int height;                // 当前帧高度（可能经过缩放）
    int width_stride;          // 行步长（字节），考虑内存对齐后的宽度
    int height_stride;         // 列步长（像素），考虑内存对齐后的高度
    image_format_t format;     // 像素格式（RGA 格式枚举）
    unsigned char* virt_addr;  // 帧数据的虚拟地址指针
    int size;                  // 帧数据大小（字节）
    int fd;                    // DMA 缓冲区文件描述符
    DmaBuffer* dmaBuffer;      // 底层 DMA 缓冲区指针（由 DmaBufferPool 管理）
    long time;                 // 帧时间戳（用于音视频同步或帧率控制）
}
    ;
