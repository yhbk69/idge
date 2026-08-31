#pragma once
// RGA 头文件不能放在 extern "C" 块中,因为包含 C++ inline 函数
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

static std::mutex dma_mutex;   // 全局互斥锁，保护 DRM 操作
class DmaFrameBuffer
{
private:
    int m_drm_fd = -1;
    int m_fd=-1;
    uint8_t* m_ptr;
    size_t m_size;
    AVFrame* m_frame;
    int m_width;
    int m_height;
    int m_format;
    int m_stride = 0;
    uint32_t m_handle;

public:
    DmaFrameBuffer() = default;

    DmaFrameBuffer(int width, int height, int format);
        
    int alloc();
    

    ~DmaFrameBuffer();
    

    void release ();
    

    DmaFrameBuffer(const DmaFrameBuffer&) = default;
    DmaFrameBuffer& operator=(const DmaFrameBuffer&) = default;

    DmaFrameBuffer(DmaFrameBuffer&& other) noexcept
        :m_fd(other.m_fd), m_ptr(other.m_ptr)
    {
        other.m_fd = -1;
        other.m_ptr = nullptr;
    
    }

    DmaFrameBuffer& operator=(DmaFrameBuffer&& other) noexcept
    {
        if (this!= &other)
        {
            release();
            m_fd = other.m_fd;
            m_ptr = other.m_ptr;
            other.m_fd = -1;
            other.m_ptr = nullptr;
        }
        return *this;
    
    }

    


    int alloc_drm_buffer() ;

    void free_dma_buffer();

    void free_drm_buffer() ;

    int fd() const ;
    uint8_t* ptr() const ;
    size_t size() const ;
    int width() const ;
    void setWidth(int width) ;
    int height() const ;
    void setHeight(int height) ;
    int format() const;
    void setFormat(int format);
    void setFrame(AVFrame* frame) ;
    uint32_t handle();
    int stride() const; 
    
};

Q_DECLARE_METATYPE(DmaFrameBuffer);
Q_DECLARE_METATYPE(std::shared_ptr<DmaFrameBuffer>);