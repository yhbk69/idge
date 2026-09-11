/**
 * @file DmaBufferPool.h
 * @brief DMA 缓冲池管理
 *
 * 作用：提供 DMA 缓冲区的池化管理，预分配一组物理连续的 DMA 缓冲区，
 *       支持高效的获取/归还操作，避免频繁的系统调用开销。
 *       主要用于视频处理流水线中，为解码器、RGA 缩放等模块提供零拷贝的帧缓冲。
 */

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

// ============================================================================
// RGA 内存对齐宏
// ============================================================================
// 作用：将值 val 向上对齐到 align 的整数倍。
//       RGA 硬件要求图像的行步长按特定字节（通常 32 或 64）对齐。
// ============================================================================
#define RGA_ALIGN(val, align) (((val) + (align) - 1) & ~((align) - 1))

// ============================================================================
// DmaBuffer 结构体 —— 单个 DMA 缓冲区的描述信息
// ============================================================================
// 作用：描述一个 DMA 缓冲区的所有属性，包括内存地址、尺寸、格式、
//       RGA 句柄等。作为缓冲池中的基本单元。
// ============================================================================
struct DmaBuffer {
    int index = 0;              // 在缓冲池中的索引编号
    int ref_count = 0;          // 引用计数（当前使用次数）
    int frame_id = 0;           // 帧编号（用于跟踪帧序）
    void *va = nullptr;         // DMA 缓冲区的虚拟地址
    int fd = 0;                 // DMA 缓冲区的文件描述符
    size_t size = 0;            // 缓冲区大小（字节）
    int width = 0;              // 图像宽度
    int height = 0;             // 图像高度
    int width_stride = 0;       // 行步长（字节，考虑对齐）
    int height_stride = 0;      // 列步长（像素）
    int format = 0;             // 像素格式（RK_FORMAT_* 枚举）
    rga_buffer_handle_t rga_handle{};  // RGA 硬件句柄（用于 RGA 图像处理）
    
    /**
     * @brief 重置缓冲区元信息
     * 作用：将文件描述符、虚拟地址和大小重置为初始值。
     *       注意：不会释放实际的 DMA 内存。
     */
    void reset() {
        fd = -1;
        va = nullptr;
        size = 0;
    }
};

// ============================================================================
// DmaBufferPool 类 —— DMA 缓冲池
// ============================================================================
// 作用：预分配并管理一组 DMA 缓冲区，提供线程安全的获取/归还接口。
//       使用条件变量实现阻塞等待，当所有缓冲区都被占用时，
//       acquire() 会阻塞直到有缓冲区被归还。
//
// 设计特点：
//   - 预分配所有缓冲区，避免运行时分配延迟
//   - 支持多种像素格式（NV12、NV21、I420、RGB888、RGBA8888）
//   - 支持智能指针管理（tryAcquireSharedPtr），自动归还缓冲区
//   - 继承 enable_shared_from_this，支持安全的 shared_ptr 回调
// ============================================================================
class DmaBufferPool : public std::enable_shared_from_this<DmaBufferPool>{
public:
    /**
     * @brief 构造函数 —— 初始化并预分配缓冲池
     * @param capacity 缓冲池容量（预分配的缓冲区数量）
     * @param width    图像宽度
     * @param height   图像高度
     * @param format   像素格式（RK_FORMAT_* 枚举值）
     * @param align    内存对齐字节数（默认 32）
     * 作用：根据格式计算对齐后的内存大小，然后调用 init_dma_buffer_pool 预分配所有缓冲区
     */
    DmaBufferPool(int capacity, int width, int height, int format, int align = 32)
        : capacity_(capacity), width_(width), height_(height), format_(format), align_(align) {
        
        // 1. 计算对齐后的 Stride 和总内存大小
        wstride_ = RGA_ALIGN(width, align_);
        
        // 根据像素格式计算缓冲区总大小
        if (format == RK_FORMAT_YCbCr_420_SP || format == RK_FORMAT_YCrCb_420_SP) {
            // NV12/NV21: Y平面 + UV交错平面（UV stride 与 Y 相同，高度为一半）
            size_ = wstride_ * height + wstride_ * (height / 2);
        } else if (format == RK_FORMAT_YCbCr_420_P) {
            // I420: Y平面 + U平面 + V平面
            int uv_stride = RGA_ALIGN(width / 2, align_);
            size_ = wstride_ * height + uv_stride * (height / 2) * 2;
        } else if (format == RK_FORMAT_RGB_888 || format == RK_FORMAT_BGR_888) {
            // RGB888/BGR888: 3字节每像素
            size_ = wstride_ * height * 3;
        } else if (format == RK_FORMAT_RGBA_8888 || format == RK_FORMAT_BGRA_8888) {
            // RGBA8888/BGRA8888: 4字节每像素
            size_ = wstride_ * height * 4;
        } else {
            throw std::invalid_argument("Unsupported RGA format for this pool");
        }

        // 2. 预分配所有 DMA 缓冲区
        init_dma_buffer_pool(width, height, wstride_, height);
        
    }

    /**
     * @brief 析构函数 —— 释放所有预分配的 DMA 缓冲区
     * 作用：遍历可用缓冲区队列，逐个释放 DMA 缓冲区和 RGA 句柄
     */
    ~DmaBufferPool() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!available_buffers.empty()) {
            auto buf = available_buffers.front();
            available_buffers.pop();
            free_dma_buffer(buf);
        }
    }

    // ========================================================================
    // acquire —— 获取一个缓冲区（阻塞等待）
    // ========================================================================
    // 作用：从缓冲池中获取一个可用的 DMA 缓冲区。如果所有缓冲区都被占用，
    //       则阻塞等待直到有缓冲区被归还。
    // @return 可用的 DmaBuffer 指针
    // ========================================================================
    DmaBuffer* acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        // 阻塞等待直到缓冲池非空
        cv_.wait(lock, [this] { return !available_buffers.empty(); });
        
        auto buf = available_buffers.front();   // 取出队首缓冲区
        available_buffers.pop();                // 从可用队列中移除
        return buf;
    }

    // ========================================================================
    // tryAcquire —— 尝试获取缓冲区（非阻塞）
    // ========================================================================
    // 作用：尝试从缓冲池中获取一个可用的 DMA 缓冲区。
    //       如果没有可用缓冲区，立即返回 nullptr，不会阻塞。
    // @return 可用的 DmaBuffer 指针，无可用则返回 nullptr
    // ========================================================================
    DmaBuffer* tryAcquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        
        if (available_buffers.empty())
        {
            return nullptr;  // 无可用缓冲区，立即返回
        }
        
        auto buf = available_buffers.front();
        available_buffers.pop();
        return buf;
    }

    // ========================================================================
    // tryAcquireSharedPtr —— 尝试获取缓冲区（智能指针版本）
    // ========================================================================
    // 作用：获取缓冲区并包装为 shared_ptr，通过自定义删除器实现自动归还。
    //       当 shared_ptr 引用计数归零时，缓冲区会自动归还到缓冲池。
    //       使用 weak_ptr 避免循环引用导致的内存泄漏。
    // @return shared_ptr 包装的 DmaBuffer，无可用则返回 nullptr
    // ========================================================================
    std::shared_ptr<DmaBuffer> tryAcquireSharedPtr() {
        std::unique_lock<std::mutex> lock(mutex_);
        
        if (available_buffers.empty())
        {
            return nullptr;
        }
        
        auto buf = available_buffers.front();
        available_buffers.pop();
        // 创建 weak_ptr 用于回调时安全访问缓冲池
        std::weak_ptr<DmaBufferPool> weak_pool = shared_from_this();
        // 使用自定义删除器：当 shared_ptr 析构时自动归还缓冲区
        std::shared_ptr<DmaBuffer> shared_buf(buf,
            [weak_pool](DmaBuffer* returned) {
                if(auto pool = weak_pool.lock()) {
                    pool->release(returned);  // 缓冲池仍存活，归还缓冲区
                }else {
                    delete returned;          // 缓冲池已销毁，直接释放内存
                }
            });
        return std::move(shared_buf);
    }

    // ========================================================================
    // release —— 归还缓冲区
    // ========================================================================
    // 作用：将使用完毕的 DMA 缓冲区归还到缓冲池，并唤醒一个等待获取的线程。
    // @param buf 待归还的 DmaBuffer 指针
    // ========================================================================
    void release(DmaBuffer* buf) {
        if (!buf) return;
        std::lock_guard<std::mutex> lock(mutex_);
        available_buffers.push(buf);    // 放回可用队列
        cv_.notify_one();               // 唤醒一个等待 acquire 的线程
    }

    /** @brief 获取对齐后的行步长 */
    int get_wstride() const { return wstride_; }
    /** @brief 获取单个缓冲区的大小 */
    size_t get_size() const { return size_; }

private:

    /**
     * @brief 初始化 DMA 缓冲池（预分配所有缓冲区）
     * 作用：循环创建 DmaBuffer 对象，分配 DMA 内存，导入 RGA 句柄，
     *       成功的缓冲区加入可用队列。
     */
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
            // 通过 DMA Heap 分配物理连续内存
            dma_buf_alloc(DMA_HEAP_UNCACHE_PATH, size, &(buf->fd), &(buf->va));

            if (buf->fd >= 0)
            {
                // 导入 DMA 缓冲区到 RGA 硬件
                im_handle_param_t infer_param = {0};
                infer_param.width = width_stride;
                infer_param.height = height;
                infer_param.format = format_;
                buf->rga_handle = importbuffer_fd(buf->fd, &infer_param);
                if (buf->rga_handle == 0)
                {
                    // RGA 导入失败，释放已分配的 DMA 内存和 DmaBuffer 对象
                    dma_buf_free(size, &(buf->fd), buf->va);
                    delete buf;
                }else
                {
                    available_buffers.push(buf);  // 成功，加入可用队列
                }
            }
            else
            {
                // DMA 分配失败，释放 DmaBuffer 对象
                delete buf;
            }
        }
    }

    /**
     * @brief 释放单个 DMA 缓冲区
     * 作用：先释放 RGA 句柄，再释放 DMA 内存和重置元信息
     */
    void free_dma_buffer(DmaBuffer* buf) {
        if (buf->rga_handle != 0) {
            releasebuffer_handle(buf->rga_handle);  // 释放 RGA 硬件句柄
            buf->reset();                            // 重置元信息
        }
        dma_buf_free(buf->size, &(buf->fd), buf->va);  // 释放 DMA 内存
    }

    int capacity_;                  // 缓冲池容量
    int width_, height_, format_, align_;  // 图像参数
    int wstride_;                   // 对齐后的行步长
    size_t size_;                   // 单个缓冲区大小
    int allocated_count_ = 0;       // 已分配计数

    std::queue<DmaBuffer*> available_buffers;  // 可用缓冲区队列
    std::mutex mutex_;                          // 互斥锁
    std::condition_variable cv_;                // 条件变量（用于阻塞等待）
};

// ============================================================================
// DmaBufferGuard 类 —— DMA 缓冲区 RAII 守卫
// ============================================================================
// 作用：通过 RAII 机制管理 DmaBuffer 的生命周期。
//       构造时从缓冲池获取缓冲区，析构时自动归还。
//       支持移动语义，禁用拷贝，确保缓冲区不会被重复释放。
//
// 注意：当前实现存在 bug（类注释中标注），需要修复
// ============================================================================
class DmaBufferGuard {
public:
    /**
     * @brief 构造函数 —— 从缓冲池获取缓冲区
     * @param pool DMA 缓冲池引用
     * 作用：调用 pool.acquire() 获取一个可用缓冲区
     */
    DmaBufferGuard(DmaBufferPool& pool) 
        : pool_(pool) 
    {
        this->buf_ = pool_.acquire();  // 阻塞获取
    }
    
    /**
     * @brief 析构函数 —— 自动归还缓冲区
     * 作用：如果持有有效的缓冲区，则自动归还到缓冲池
     */
    ~DmaBufferGuard() {
        if (buf_)
        {
            pool_.release(buf_); // 离开作用域自动归还
        }
    }

    /** @brief 获取内部 DmaBuffer 指针 */
    DmaBuffer* get() { return buf_; }

    // 禁用拷贝，防止缓冲区被多次释放
    DmaBufferGuard(const DmaBufferGuard&) = delete;
    DmaBufferGuard& operator=(const DmaBufferGuard&) = delete;

    /**
     * @brief 移动构造函数
     * 作用：接管源对象的缓冲区所有权
     */
    DmaBufferGuard(DmaBufferGuard&& other) noexcept : pool_(other.pool_), buf_(other.buf_)
    {
        other.buf_ = nullptr;  // 源对象不再持有缓冲区
    }

    /**
     * @brief 移动赋值运算符
     * 作用：先归还当前持有的缓冲区，再接管源对象的缓冲区
     */
    DmaBufferGuard& operator=(DmaBufferGuard&& other) noexcept
    {
        if (this != &other)
        {
            if (buf_)
            {
                pool_.release(buf_);  // 归还当前缓冲区
            }
            buf_ = other.buf_;        // 接管源对象的缓冲区
            other.buf_ = nullptr;
        }
        return *this;
    }

    /** @brief 检查是否持有有效的缓冲区 */
    bool valid() const {return buf_ != nullptr;}
    /** @brief 获取内部 DmaBuffer 指针（常量版本） */
    DmaBuffer* get() const { return buf_; }
    /** @brief 箭头运算符，方便访问 DmaBuffer 成员 */
    DmaBuffer* operator->() const { return buf_; }
    /** @brief 解引用运算符 */
    DmaBuffer& operator*() const {return *buf_; }

    /**
     * @brief 分离所有权
     * 作用：放弃缓冲区的所有权并返回指针，调用者需自行管理内存。
     *       用于需要将缓冲区传递给不支持 RAII 的接口时。
     */
    DmaBuffer* detach()
    {
        DmaBuffer* buf = buf_;
        buf_ = nullptr;  // 不再持有缓冲区
        return buf;
    }

private:
    DmaBuffer* buf_;            // 持有的 DMA 缓冲区指针
    DmaBufferPool& pool_;       // 缓冲池引用
};

#endif
