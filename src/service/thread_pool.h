#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>

class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads = 4);
    ~ThreadPool();

    // 提交任务，返回 future 用于获取结果
    template<class F, class... Args>
    auto submit(F&& f, Args&&... args) 
        -> std::future<typename std::result_of<F(Args...)>::type>;

    // 等待所有任务完成
    void wait();

private:
    void worker();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::condition_variable wait_condition_;
    
    bool stop_;
    size_t busy_threads_;
    size_t pending_tasks_;
};

// 模板实现必须在头文件
template<class F, class... Args>
auto ThreadPool::submit(F&& f, Args&&... args) 
    -> std::future<typename std::result_of<F(Args...)>::type> {
    
    using return_type = typename std::result_of<F(Args...)>::type;
    
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
    
    std::future<return_type> res = task->get_future();
    
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (stop_) {
            throw std::runtime_error("submit on stopped ThreadPool");
        }
        
        tasks_.emplace([task](){ (*task)(); });
        ++pending_tasks_;
    }
    
    condition_.notify_one();
    return res;
}

#endif // THREAD_POOL_H