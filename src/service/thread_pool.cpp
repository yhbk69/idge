// 线程池实现：worker 主循环、停止与排空、wait 屏障
#include "thread_pool.h"
#include <iostream>

// 构造即拉起全部 worker：线程数固定后不再伸缩（批处理场景无需动态扩缩）。
// worker 捕获 this 回调成员函数——因此 ThreadPool 不可拷贝（本身含 mutex/thread 也不可拷贝）
ThreadPool::ThreadPool(size_t num_threads)
    : stop_(false), busy_threads_(0), pending_tasks_(0) {
    
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this] { this->worker(); });
    }
    
    std::cout << "ThreadPool created with " << num_threads << " threads" << std::endl;
}

// 优雅停止：stop_=true -> 唤醒所有 worker -> 逐个 join。
// 关键语义：worker 在“stop_ 且队列已排空”时才退出，即析构会执行完所有已提交任务，
// 保证调用方此前拿到的每个 future 最终都有值（或异常），不会 get() 永久阻塞。
// 死锁规避：不要在任务函数里同步等待析构、也不要在持有任何应用层锁时析构本池
ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    
    // notify_all（而非 notify_one）：停止信号必须唤醒每一个等待中的 worker，
    // 否则未被唤醒的线程永远等不到条件，join 挂死
    condition_.notify_all();
    
    // join 全部线程：此处是析构的主要耗时点（等待在跑任务收尾+队列排空）
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    
    std::cout << "ThreadPool destroyed" << std::endl;
}

// 单个 worker 的生命周期循环：等待任务 -> 出队 -> 脱锁执行 -> 归还计数
void ThreadPool::worker() {
    while (true) {
        std::function<void()> task;
        
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            
            // 带谓词的 wait：防虚假唤醒；条件=“有活干”或“该下班了（stop_）”
            condition_.wait(lock, [this] {
                return stop_ || !tasks_.empty();
            });
            
            // 退出条件必须两个都满足：已请求停止 且 队列已排空（不丢弃已提交任务）
            if (stop_ && tasks_.empty()) {
                return;
            }
            
            // 持锁出队并登记忙碌：busy 计数与出队在同一次临界区内完成，
            // 保证 wait() 的“pending==0 && busy==0”判定不存在任务掉缝
            task = std::move(tasks_.front());
            tasks_.pop();
            ++busy_threads_;
        }
        
        // 执行任务（不持有锁）：任务体可能长时间跑 NPU/子进程，持锁会导致
        // 其余 worker 全部无法取任务、submit 端被阻塞——必须脱锁执行。
        // 任务抛出的异常由 packaged_task 捕获存入共享状态，这里不会击穿 worker
        task();
        
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            // busy-- 与 pending-- 一起归还：pending 在“执行完”而非“出队时”递减，
            // 因此 pending==0 蕴含“无未完成工作”，wait() 屏障语义完整
            --busy_threads_;
            --pending_tasks_;
        }
        
        // 通知 wait() 的等待者检查归零条件（持锁外发通知，减少临界区竞争）
        wait_condition_.notify_all();
    }
}

// 批次屏障：阻塞直到此刻已提交的任务全部执行完。
// 注意两点边界：
//   1) 谓词只覆盖“调用时刻已提交”的任务——等待期间新 submit 会继续拉长等待；
//   2) 不可在 worker 线程内调用（自己处于 busy 计数中，永远等不到归零，自死锁）
void ThreadPool::wait() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    wait_condition_.wait(lock, [this] {
        return pending_tasks_ == 0 && busy_threads_ == 0;
    });
}