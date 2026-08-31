#ifndef DMA_BUFFER_POOL_H
#define DMA_BUFFER_POOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <rga/im2d.hpp>
#include <rga/rga.h>
#include <rga/RgaUtils.h>
#include "dma_alloc.h"

// RK 平台 RGA 常用的内存对齐宏 (通常对齐到 32 或 64 字节)
#define RGA_ALIGN(val, align) (((val) + (align) - 1) & ~((align) - 1))

// 定义 RGA Buffer 结构
struct DmaBuffer {
    int index = 0;
    int ref_count = 0;
    int frame_id = 0;
    void *va = nullptr; //dma buffer virtual address
    int fd = 0;
    size_t size = 0;
    int width = 0;
    int height = 0;
    int width_stride = 0;
    int height_stride = 0;
    int format = 0;
    rga_buffer_handle_t rga_handle{};
    
    // 清理函数
    void reset() {
        fd = -1;
        va = nullptr;
        size = 0;
    }
};

class DmaBufferPool : public std::enable_shared_from_this<DmaBufferPool>{
public:
    DmaBufferPool(int capacity, int width, int height, int format, int align = 32)
        : capacity_(capacity), width_(width), height_(height), format_(format), align_(align) {
        
        //imconfig(IM_CONFIG_SCHEDULER_CORE, IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1);  
        // 1. 计算对齐后的 Stride 和总内存大小
        wstride_ = RGA_ALIGN(width, align_);
        
        if (format == RK_FORMAT_YCbCr_420_SP || format == RK_FORMAT_YCrCb_420_SP) {
            // NV12/NV21: Y平面 + UV交错平面 (UV stride 与 Y 相同，高度为一半)
            size_ = wstride_ * height + wstride_ * (height / 2);
        } else if (format == RK_FORMAT_YCbCr_420_P) {
            // I420: Y平面 + U平面 + V平面
            int uv_stride = RGA_ALIGN(width / 2, align_);
            size_ = wstride_ * height + uv_stride * (height / 2) * 2;
        } else if (format == RK_FORMAT_RGB_888 || format == RK_FORMAT_BGR_888) {
            size_ = wstride_ * height * 3;
        } else if (format == RK_FORMAT_RGBA_8888 || format == RK_FORMAT_BGRA_8888) {
            size_ = wstride_ * height * 4;
        } else {
            throw std::invalid_argument("Unsupported RGA format for this pool");
        }

        // 2. 预分配 Buffer
        init_dma_buffer_pool(width, height, wstride_, height);
        
    }

    ~DmaBufferPool() {
        // 释放所有物理内存
        std::lock_guard<std::mutex> lock(mutex_);
        while (!available_buffers.empty()) {
            auto buf = available_buffers.front();
            available_buffers.pop();
            free_dma_buffer(buf);
        }
    }

    // 获取 Buffer (阻塞等待)
    DmaBuffer* acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !available_buffers.empty(); });
        
        auto buf = available_buffers.front();
        available_buffers.pop();
        return buf;
    }

    // 获取 Buffer (阻塞等待)
    DmaBuffer* tryAcquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        
        if (available_buffers.empty())
        {
            return nullptr;
        }
        
        auto buf = available_buffers.front();
        available_buffers.pop();
        return buf;
    }

    // 获取 Buffer (阻塞等待)
    std::shared_ptr<DmaBuffer> tryAcquireSharedPtr() {
        std::unique_lock<std::mutex> lock(mutex_);
        
        if (available_buffers.empty())
        {
            return nullptr;
        }
        
        auto buf = available_buffers.front();
        available_buffers.pop();
        std::weak_ptr<DmaBufferPool> weak_pool = shared_from_this();
        std::shared_ptr<DmaBuffer> shared_buf(buf,
            [weak_pool](DmaBuffer* returned) {
                if(auto pool = weak_pool.lock()) {
                    pool->release(returned);
                }else {
                    delete returned;
                }
            });
        return std::move(shared_buf);
    }

    // 归还 Buffer
    void release(DmaBuffer* buf) {
        if (!buf) return;
        std::lock_guard<std::mutex> lock(mutex_);
        available_buffers.push(buf);
        cv_.notify_one(); // 唤醒等待 acquire 的线程
    }

    int get_wstride() const { return wstride_; }
    size_t get_size() const { return size_; }

private:

    void init_dma_buffer_pool(int width, int height, int width_stride, int height_stride) 
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t size = width_stride * height * get_bpp_from_format(format_);
        
        for (int i = 0; i < capacity_; ++i)
        {
            DmaBuffer* buf = new DmaBuffer();
            buf->index = i;
            buf->ref_count = 0;
            buf->format = format_;
            buf->width = width;
            buf->height = height;
            buf->width_stride = width_stride;
            buf->height_stride = height_stride;
            buf->size = size;
            dma_buf_alloc(DMA_HEAP_UNCACHE_PATH, size, &(buf->fd), &(buf->va));


            if (buf->fd >= 0)
            {
                im_handle_param_t infer_param = {0};
                infer_param.width = width_stride;
                infer_param.height = height;
                infer_param.format = format_;
                buf->rga_handle = importbuffer_fd(buf->fd, &infer_param);
                if (buf->rga_handle == 0)
                {
                    dma_buf_free(size, &(buf->fd), buf->va);
                }else
                {
                    available_buffers.push(buf);
                }
                

            }
        }

    }

    void free_dma_buffer(DmaBuffer* buf) {
        if (buf->rga_handle != 0) {
            releasebuffer_handle(buf->rga_handle);
            buf->reset();
        }
        dma_buf_free(buf->size, &(buf->fd), buf->va);
    }

    int capacity_;
    int width_, height_, format_, align_;
    int wstride_;
    size_t size_;
    int allocated_count_ = 0;

    std::queue<DmaBuffer*> available_buffers;
    std::mutex mutex_;
    std::condition_variable cv_;
};

//使用有bug，需修改
class DmaBufferGuard {
public:
    DmaBufferGuard(DmaBufferPool& pool) 
        : pool_(pool) 
    {
        this->buf_ = pool_.acquire();
    }
    
    ~DmaBufferGuard() {
        if (buf_)
        {
            pool_.release(buf_); // 离开作用域自动归还
        }
        
    }

    DmaBuffer* get() { return buf_; }

    // 禁用拷贝
    DmaBufferGuard(const DmaBufferGuard&) = delete;
    DmaBufferGuard& operator=(const DmaBufferGuard&) = delete;

    DmaBufferGuard(DmaBufferGuard&& other) noexcept : pool_(other.pool_), buf_(other.buf_)
    {
        other.buf_ = nullptr;
    }

    DmaBufferGuard& operator=(DmaBufferGuard&& other) noexcept
    {
        if (this != &other)
        {
            if (buf_)
            {
                pool_.release(buf_);
            }
            buf_ = other.buf_;
            other.buf_ = nullptr;
        }
        return *this;
    }

    bool valid() const {return buf_ != nullptr;}
    DmaBuffer* get() const { return buf_; }
    DmaBuffer* operator->() const { return buf_; }
    DmaBuffer& operator*() const {return *buf_; }

    DmaBuffer* detach()
    {
        DmaBuffer* buf = buf_;
        buf_ = nullptr;
        return buf;
    }

private:
    DmaBuffer* buf_;
    DmaBufferPool& pool_;
};

#endif