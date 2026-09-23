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
// ⚠【命名冲突警示】本类与 DmaFrameBuffer.h 中的同名 class DmaFrameBuffer
//   是完全不同的类型：二者同名不同构，任何同时包含两个头文件的编译单元
//   都会触发重定义编译错误（这也是二者至今未同 TU 共存的原因）。
//   检索/引用该类时必须先确认包含路径。历史上本文件是数据结构版，
//   DmaFrameBuffer.h 是 RAII 生命周期管理版。
// ⚠ 成员均无默认初始化值：栈上构造后 fd/dmaBuffer/virt_addr 为垃圾值，
//   使用前必须整体清零（memset 或 {} 聚合初始化）或逐字段赋值，
//   否则判空/if(fd) 逻辑不可靠。fd 不拥有所有权（指向 DmaBufferPool
//   借出的底层缓冲），拷贝本结构体只是别名一份指针，勿对其 close()。
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
