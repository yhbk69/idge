/**
 * @file DmaFrameBuffer.h
 * @brief DMA 帧缓冲区管理类
 *
 * 作用：封装 DMA（Direct Memory Access）帧缓冲区的分配、释放和管理。
 *       支持通过 DRM（Direct Rendering Manager）和 DMA Heap 分配物理连续内存，
 *       用于视频解码输出和 RGA 硬件加速处理。
 *       该类实现了移动语义，支持高效的所有权转移，避免不必要的内存拷贝。
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

// 全局互斥锁，保护 DRM 操作的线程安全
static std::mutex dma_mutex;

// ============================================================================
// DmaFrameBuffer 类 —— DMA 帧缓冲区
// ============================================================================
// 作用：管理单个 DMA 帧缓冲区的生命周期，包括：
//       - 通过 DMA Heap 或 DRM 分配物理连续内存
//       - 关联 FFmpeg AVFrame 用于硬件解码输出
//       - 提供文件描述符（fd）和虚拟地址（ptr）访问
//       - 支持移动语义实现高效的所有权转移
// ============================================================================
class DmaFrameBuffer
{
private:
    int m_drm_fd = -1;         // DRM 设备文件描述符
    int m_fd = -1;             // DMA 缓冲区文件描述符
    uint8_t* m_ptr;            // DMA 缓冲区的虚拟地址映射
    size_t m_size;             // 缓冲区大小（字节）
    AVFrame* m_frame;          // 关联的 FFmpeg 帧对象
    int m_width;               // 帧宽度
    int m_height;              // 帧高度
    int m_format;              // 像素格式（如 NV12、RGBA 等）
    int m_stride = 0;          // 行步长（字节），考虑对齐后的宽度
    uint32_t m_handle;         // DRM 缓冲区句柄

public:
    // 默认构造函数
    DmaFrameBuffer() = default;

    /**
     * @brief 带参数的构造函数
     * @param width  帧宽度
     * @param height 帧高度
     * @param format 像素格式
     * 作用：初始化帧缓冲区的尺寸和格式参数
     */
    DmaFrameBuffer(int width, int height, int format);
        
    /**
     * @brief 分配 DMA 缓冲区内存
     * @return 成功返回 0，失败返回负数
     * 作用：调用 DMA Heap 分配指定大小的物理连续内存
     */
    int alloc();
    
    /**
     * @brief 析构函数
     * 作用：释放 DMA 缓冲区和 DRM 资源
     */
    ~DmaFrameBuffer();
    
    /**
     * @brief 释放缓冲区资源
     * 作用：释放 DMA 映射和文件描述符，重置内部状态
     */
    void release ();
    
    // 拷贝构造和赋值（使用默认实现，浅拷贝）
    DmaFrameBuffer(const DmaFrameBuffer&) = default;
    DmaFrameBuffer& operator=(const DmaFrameBuffer&) = default;

    /**
     * @brief 移动构造函数
     * 作用：接管另一个 DmaFrameBuffer 的资源所有权，源对象置为无效状态
     *       实现零拷贝的所有权转移
     */
    DmaFrameBuffer(DmaFrameBuffer&& other) noexcept
        :m_fd(other.m_fd), m_ptr(other.m_ptr)
    {
        other.m_fd = -1;       // 源对象的 fd 置为无效
        other.m_ptr = nullptr; // 源对象的指针置空
    }

    /**
     * @brief 移动赋值运算符
     * 作用：接管另一个 DmaFrameBuffer 的资源所有权，释放当前资源
     *       避免自赋值，确保资源正确转移
     */
    DmaFrameBuffer& operator=(DmaFrameBuffer&& other) noexcept
    {
        if (this != &other)
        {
            release();                // 释放当前持有的资源
            m_fd = other.m_fd;        // 接管源对象的 fd
            m_ptr = other.m_ptr;      // 接管源对象的指针
            other.m_fd = -1;          // 源对象置为无效
            other.m_ptr = nullptr;
        }
        return *this;
    }

    /**
     * @brief 通过 DRM 分配缓冲区
     * @return 成功返回 0，失败返回负数
     * 作用：使用 DRM ioctls 分配 GEM 对象并映射到用户空间
     */
    int alloc_drm_buffer() ;

    /**
     * @brief 释放 DMA 缓冲区
     * 作用：解除虚拟地址映射并关闭 DMA 文件描述符
     */
    void free_dma_buffer();

    /**
     * @brief 释放 DRM 缓冲区
     * 作用：关闭 DRM 设备文件描述符
     */
    void free_drm_buffer() ;

    // ========== Getter/Setter 方法 ==========

    /** @brief 获取 DMA 缓冲区的文件描述符 */
    int fd() const ;
    /** @brief 获取 DMA 缓冲区的虚拟地址指针 */
    uint8_t* ptr() const ;
    /** @brief 获取缓冲区大小（字节） */
    size_t size() const ;
    /** @brief 获取帧宽度 */
    int width() const ;
    /** @brief 设置帧宽度 */
    void setWidth(int width) ;
    /** @brief 获取帧高度 */
    int height() const ;
    /** @brief 设置帧高度 */
    void setHeight(int height) ;
    /** @brief 获取像素格式 */
    int format() const;
    /** @brief 设置像素格式 */
    void setFormat(int format);
    /** @brief 关联 FFmpeg AVFrame 对象 */
    void setFrame(AVFrame* frame) ;
    /** @brief 获取 DRM 缓冲区句柄 */
    uint32_t handle();
    /** @brief 获取行步长（字节） */
    int stride() const; 
    
};

// 向 Qt 元对象系统注册类型，使 DmaFrameBuffer 可在信号槽中传递
Q_DECLARE_METATYPE(DmaFrameBuffer);
Q_DECLARE_METATYPE(std::shared_ptr<DmaFrameBuffer>);
