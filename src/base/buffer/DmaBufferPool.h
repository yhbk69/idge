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
// 【位运算含义】(val + align - 1) 先加"align-1"保证跨过一个对齐边界，
//   再与 ~(align-1) 相与：align 必须是 2 的幂，此时 (align-1) 的低
//   log2(align) 位全为 1，取反后高位次 1，与运算即把低位清零 = 向下
//   取整到边界，整体效果 = 向上取整。例：val=1920, align=32 → 1920
//   （已对齐）；val=1288 → (1288+31)&~31 = 1319&0xFFFFFFE0 = 1312。
//   默认 align=32 是 RK3588 RGA 对行步长(wstride)的安全字节对齐值，
//   NV12 色度平面还隐含要求宽度为偶数（4:2:0 两像素共享一个 UV）。
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
    // fd 缺省必须是 -1：dma_buf_alloc 失败时不会写回 fd，
    // 若缺省为 0 会被 `fd >= 0` 误判为分配成功，后续清理路径
    // close(0) 将关掉进程 stdin，产生极难定位的连锁故障。
    int fd = -1;                // DMA 缓冲区的文件描述符
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
// 【为什么用池化而非按需 dma_buf_alloc】
//   dma_buf_alloc 每次要走 open+ioctl+mmap 三次系统调用，4K 帧下
//   每帧毫秒级开销且造成物理内存反复 fragmentation；池化把开销
//   一次性摊销到构造期，运行期 acquire/release 均为 O(1)
//   （队列 push/pop + 一次条件变量唤醒），且 RGA importbuffer_fd
//   得到的句柄也可复用，避免每帧重复导入。
//
// 设计特点：
//   - 预分配所有缓冲区，避免运行时分配延迟
//   - 支持多种像素格式（NV12、NV21、I420、RGB888、RGBA8888）
//   - 支持智能指针管理（tryAcquireSharedPtr），自动归还缓冲区
//   - 继承 enable_shared_from_this，支持安全的 shared_ptr 回调
//
// ⚠【生命周期总警示】本池管理的 DmaBuffer 由池 new、由池 delete：
//   任何方式取出的裸指针，其有效期都终止于析构。池析构会释放所有
//   已分配的缓冲区（包括仍在外部借出的），但外部持有的指针会变成悬垂。
//   最佳实践：先确保所有 acquire 都 release，再销毁池。
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
     * 作用：遍历所有已分配的缓冲区，释放仍持有的（包括未归还的）
     * 注意：未归还的缓冲区会被强制释放并打印警告
     */
    ~DmaBufferPool() {
        std::lock_guard<std::mutex> lock(mutex_);
        // 释放所有已分配的缓冲区（包括仍在外部借出的）
        for (auto buf : all_buffers_) {
            if (buf) {
                free_dma_buffer(buf);
                delete buf;
            }
        }
        all_buffers_.clear();
        // 清空可用队列（缓冲区已释放）
        while (!available_buffers.empty()) {
            available_buffers.pop();
        }
    }

    // ========================================================================
    // acquire —— 获取一个缓冲区（阻塞等待）
    // ========================================================================
    DmaBuffer* acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !available_buffers.empty(); });
        auto buf = available_buffers.front();
        available_buffers.pop();
        return buf;
    }

    // ========================================================================
    // tryAcquireFor —— 获取缓冲区（带超时）
    // ========================================================================
    // @param timeout_ms 超时毫秒数
    // @return 成功返回缓冲区指针，超时返回 nullptr
    // ========================================================================
    DmaBuffer* tryAcquireFor(int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                         [this] { return !available_buffers.empty(); })) {
            auto buf = available_buffers.front();
            available_buffers.pop();
            return buf;
        }
        return nullptr;
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
    // 【删除器的所有权设计】shared_ptr 的"删除"被重定向为 release 归还，
    //   因此这个 shared_ptr 绝不能 reset()/置换为别的 DmaBuffer，
    //   否则归还的是错的缓冲、原缓冲永久丢失；跨线程传递时引用计数
    //   本身是线程安全的，但指向的像素数据不是（多方同时写会撕裂）。
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
        // 【单位教训（2026-09-24 板端实测）】get_bpp_from_format 返回的是
        // **字节/像素**（BGR_888→3），不是位！曾按"位"理解加 /8 换算，导致
        // 分配尺寸缩为 1/8、importbuffer 全失败、池为空、检测停摆——勿再
        // "顺手除以8"。此处乘得的就是缓冲字节数，与构造函数的 size_ 一致。
        size_t size = static_cast<size_t>(width_stride * height *
                                          get_bpp_from_format(format_));
        
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
                    printf("[DmaBufferPool] importbuffer_fd failed idx=%d size=%zu\n",
                           i, size);
                    // RGA 导入失败，释放已分配的 DMA 内存和 DmaBuffer 对象
                    dma_buf_free(size, &(buf->fd), buf->va);
                    delete buf;
                }else
                {
                    available_buffers.push(buf);  // 成功，加入可用队列
                    all_buffers_.push_back(buf);  // 记录到全量列表
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
     * 作用：先释放 RGA 句柄，再用真实的 size/fd/va 释放 DMA 内存，最后重置元信息
     * 【顺序说明】必须先 dma_buf_free 再 reset：此前实现先 reset 把
     *   fd/va/size 清零，随后 dma_buf_free(0, &fd=-1, nullptr) 实际是
     *   munmap(nullptr,0)+close(-1) 双双失败静默吞掉——DMA 内存与映射
     *   直到进程退出才被内核回收。reset 只应作为释放完成后的收尾。
     */
    void free_dma_buffer(DmaBuffer* buf) {
        if (buf->rga_handle != 0) {
            releasebuffer_handle(buf->rga_handle);  // 释放 RGA 硬件句柄
            buf->rga_handle = 0;
        }
        dma_buf_free(buf->size, &(buf->fd), buf->va);  // 释放 DMA 内存
        buf->reset();                                  // 重置元信息
    }

    int capacity_;                  // 缓冲池容量
    int width_, height_, format_, align_;  // 图像参数
    int wstride_;                   // 对齐后的行步长
    size_t size_;                   // 单个缓冲区大小
    int allocated_count_ = 0;       // 已分配计数

    std::queue<DmaBuffer*> available_buffers;  // 可用缓冲区队列
    std::vector<DmaBuffer*> all_buffers_;      // 所有已分配的缓冲区（用于析构时完整释放）
    std::mutex mutex_;                          // 互斥锁
    std::condition_variable cv_;                // 条件变量（用于阻塞等待）
};

#endif
