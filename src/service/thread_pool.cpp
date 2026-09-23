#include "thread_pool.h"
#include <iostream>

ThreadPool::ThreadPool(size_t num_threads)
    : stop_(false), busy_threads_(0), pending_tasks_(0) {
    
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this] { this->worker(); });
    }
    
    std::cout << "ThreadPool created with " << num_threads << " threads" << std::endl;
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    
    condition_.notify_all();
    
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    
    std::cout << "ThreadPool destroyed" << std::endl;
}

void ThreadPool::worker() {
    while (true) {
        std::function<void()> task;
        
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            
            condition_.wait(lock, [this] {
                return stop_ || !tasks_.empty();
            });
            
            if (stop_ && tasks_.empty()) {
                return;
            }
            
            task = std::move(tasks_.front());
            tasks_.pop();
            ++busy_threads_;
        }
        
        // 执行任务（不持有锁）
        task();
        
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            --busy_threads_;
            --pending_tasks_;
        }
        
        wait_condition_.notify_all();
    }
}

void ThreadPool::wait() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    wait_condition_.wait(lock, [this] {
        return pending_tasks_ == 0 && busy_threads_ == 0;
    });
}