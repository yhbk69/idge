/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 */

#ifndef FRAME_QUEUE_H   // 头文件保护宏：防止重复包含
#define FRAME_QUEUE_H   // 定义宏，表示该头文件已被包含

#include <condition_variable>    // 包含条件变量，用于线程同步
#include <mutex>                 // 包含互斥锁，用于保护共享数据
#include <queue>                 // 包含队列容器，用于存储帧数据
#include <cstddef>
#include "common.hpp"
#include "DmaBufferPool.h"

// ============================================================================
// FrameQueue 类 —— 线程安全的帧数据队列
// ============================================================================
// 作用：生产者-消费者模型的阻塞队列，用于在视频采集/解码线程与显示线程之间
//       传递 image_buffer_t 帧数据。支持有界队列（满时阻塞生产者）、
//       优雅关闭（shutdown）、以及 pushAndReplace（满时替换最旧帧）。
//
// 线程安全机制：
//   - mutex_ 保护 queue_ 的并发访问
//   - cond_not_full_  生产者等待队列非满
//   - cond_not_empty_ 消费者等待队列非空
// ============================================================================
class FrameQueue 
{
public:
    // 默认构造函数，最大容量为0
    FrameQueue() : max_size(0) {}

    /**
     * @brief 带参数的构造函数
     * @param max_data_size 队列最大容量
     * @param pool DMA 缓冲池，用于 pushAndReplace 时释放旧帧
     * 作用：初始化队列容量、DMA 缓冲池引用，并设置关闭标志为 false
     */
    explicit FrameQueue(int max_data_size, std::shared_ptr<DmaBufferPool> pool) : max_size(max_data_size), dmaBufferPool(pool), shutdown_(false) {}

    // ========================================================================
    // push（右值引用版本）—— 移动入队
    // ========================================================================
    // 作用：将临时对象（右值）通过移动语义高效地放入队列，避免不必要的拷贝。
    //       队列满时阻塞等待，直到有空间或收到关闭信号。
    // @param temp 待入队的帧数据（右值引用）
    // @return 成功返回 true，已关闭返回 false
    // ========================================================================
    bool push(image_buffer_t &&temp)
    {
        std::unique_lock<std::mutex> lock(mtx_);   // 获取互斥锁（自动管理锁的生命周期）
        // 等待条件变量，直到队列未满或收到关闭信号
        cond_not_full_.wait(lock, [this] { return queue_.size() < max_size || shutdown_; });
        if (shutdown_)                              // 如果已关闭，则不再push
            return false;
        queue_.emplace(std::move(temp));            // 将temp移动构造到队列末尾
        cond_not_empty_.notify_one();               // 通知一个等待"非空"条件的线程
        return true;                                // 成功push返回true
    }

    // ========================================================================
    // push（左值引用版本）—— 拷贝入队
    // ========================================================================
    // 作用：将左值帧数据通过拷贝放入队列。如果 T 是左值，将进行拷贝操作。
    //       队列满时阻塞等待，直到有空间或收到关闭信号。
    // @param temp 待入队的帧数据（常量左值引用）
    // @return 成功返回 true，已关闭返回 false
    // ========================================================================
    bool push(const image_buffer_t &temp)
    {
        std::unique_lock<std::mutex> lock(mtx_);   // 获取互斥锁
        // 等待队列未满或关闭
        cond_not_full_.wait(lock, [this] { return queue_.size() < max_size || shutdown_; });
        if (shutdown_)                              // 已关闭则放弃push
            return false;
        queue_.emplace(temp);                       // 拷贝构造元素到队列末尾
        cond_not_empty_.notify_one();               // 唤醒等待非空的线程
        return true;
    }

    // ========================================================================
    // pushAndReplace —— 满时替换最旧帧的入队
    // ========================================================================
    // 作用：与普通 push 不同，当队列已满时不会阻塞，而是替换队列中最旧的帧。
    //       替换时会通过 DMA 缓冲池释放旧帧占用的物理内存。
    //       适用于实时视频场景，确保始终保留最新的帧数据。
    // @param temp 待入队的帧数据
    // @return 成功返回 true，已关闭返回 false
    // ========================================================================
    bool pushAndReplace(const image_buffer_t &temp)
    {
        std::unique_lock<std::mutex> lock(mtx_);   // 获取互斥锁
        
        if (shutdown_)                              // 已关闭则放弃push
            return false;
        // 如果队列已满，释放最旧帧（队尾）的 DMA 缓冲区
        if (queue_.size() == max_size)
        {
            image_buffer_t last = queue_.back();    // 取出最旧帧
            if (last.dmaBuffer)
            {
                dmaBufferPool->release(last.dmaBuffer);  // 归还 DMA 缓冲区到缓冲池
            }
        }
        queue_.emplace(temp);                       // 拷贝构造元素到队列末尾
        cond_not_empty_.notify_one();               // 唤醒等待非空的线程
        return true;
    }

    // ========================================================================
    // wait_and_pop —— 阻塞弹出
    // ========================================================================
    // 作用：从队列头部取出一个帧数据。如果队列为空，则阻塞等待直到有新帧入队或
    //       收到关闭信号。取出后通知可能等待"非满"的生产者线程。
    // @param temp 输出参数，存放取出的帧数据
    // @return 成功返回 true，已关闭且队列为空返回 false
    // ========================================================================
    bool wait_and_pop(image_buffer_t &temp)
    {
        std::unique_lock<std::mutex> lock(mtx_);   // 获取互斥锁
        // 等待队列非空或收到关闭信号
        cond_not_empty_.wait(lock, [this] { return !queue_.empty() || shutdown_; });
        if (shutdown_ && queue_.empty())            // 如果已关闭且队列为空，则pop失败
            return false;
        temp = queue_.front();                      // 取出队首元素
        queue_.pop();                               // 弹出队首元素
        cond_not_full_.notify_one();                // 通知可能等待"非满"的线程（因为有空间了）
        return true;                                // 成功pop返回true
    }

    // ========================================================================
    // shutdown —— 关闭队列
    // ========================================================================
    // 作用：设置关闭标志并唤醒所有等待的线程（包括生产者和消费者），
    //       使它们检测到关闭状态后退出阻塞，实现优雅关闭。
    // ========================================================================
    void shutdown()
    {
        std::lock_guard<std::mutex> lock(mtx_);     // 加锁（自动管理锁）
        shutdown_ = true;                           // 设置关闭标志
        cond_not_empty_.notify_all();               // 唤醒所有等待非空的线程
        cond_not_full_.notify_all();                // 唤醒所有等待非满的线程
    }

    // ========================================================================
    // size —— 获取当前队列大小
    // ========================================================================
    // 作用：线程安全地返回当前队列中的帧数量
    // ========================================================================
    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mtx_);     // 加锁以保证读取安全
        return queue_.size();                       // 返回队列大小
    }

    // ========================================================================
    // clear —— 清空队列
    // ========================================================================
    // 作用：清空队列中的所有帧数据，并唤醒等待"非满"的线程
    //       注意：仅交换空队列，不会显式释放帧中的 DMA 缓冲区
    // ========================================================================
    void clear()
    {
        std::lock_guard<std::mutex> lock(mtx_);     // 加锁
        std::queue<image_buffer_t> tmp;             // 创建一个临时空队列
        std::swap(queue_, tmp);                     // 交换，原队列被清空
        cond_not_full_.notify_all();                // 通知所有等待非满的线程（现在队列空了，有空间）
    }

    // ========================================================================
    // empty —— 判断队列是否为空
    // ========================================================================
    // 作用：线程安全地检查队列是否没有帧数据
    // ========================================================================
    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mtx_);     // 加锁
        return queue_.empty();                      // 返回是否为空
    }

    // ========================================================================
    // reset —— 重置队列
    // ========================================================================
    // 作用：设置新的最大容量，清空队列，并重置关闭标志。
    //       用于动态调整队列大小或重新初始化队列状态。
    // @param new_max_size 新的最大容量
    // ========================================================================
    void reset(int new_max_size)
    {
        std::lock_guard<std::mutex> lock(mtx_);     // 加锁
        max_size = new_max_size;                    // 更新最大容量
        queue_ = std::queue<image_buffer_t>();      // 清空队列（赋值空队列）
        shutdown_ = false;                          // 重置关闭标志
        cond_not_empty_.notify_all();               // 唤醒所有等待非空的线程（虽然队列已空，但通知有助于状态更新）
    }

private:
    mutable std::mutex mtx_;                        // 互斥锁，用于保护共享数据；mutable允许在const方法中加锁
    std::condition_variable cond_not_empty_;        // 条件变量，用于等待队列非空
    std::condition_variable cond_not_full_;         // 条件变量，用于等待队列非满
    size_t max_size;                                // 队列允许的最大元素个数
    bool shutdown_;                                 // 关闭标志，为true时所有阻塞操作应立即返回
    std::queue<image_buffer_t> queue_;              // 底层队列容器，实际存储帧数据
    std::shared_ptr<DmaBufferPool> dmaBufferPool;   // DMA 缓冲池，用于 pushAndReplace 时释放旧帧
};

#endif /* PROJECT2_FRAME_QUEUE_H */                 // 结束头文件保护宏
