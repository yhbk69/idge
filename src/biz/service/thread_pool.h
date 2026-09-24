#ifndef THREAD_POOL_H
#define THREAD_POOL_H

/*
 * 服务层专用固定大小线程池（RollCallService 内部使用）
 * 模型：任务队列（std::queue + mutex + 条件变量）+ N 个常驻 worker；
 * submit() 把可调用对象包装成 packaged_task 入队并返回 future，
 * worker 串行取任务执行，任务异常经 packaged_task 捕获、在 future.get() 处重放。
 * 与全局 src/threadpool（视频流水线用）相互独立：本池只管“照片检测”这类批任务。
 */

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
    // num_threads 默认 4：与 RK3588 上人脸检测子进程可并行的 NPU/大核数匹配，
    // 更多线程只会加剧 NPU 争抢而非提升吞吐
    explicit ThreadPool(size_t num_threads = 4);
    // 析构 = 请求停止 + 等待收尾：置 stop_ 后唤醒全部 worker；
    // worker 会先把队列中已提交的任务跑完才退出（stop_ && 队列空才 return），
    // 因此析构可能耗时较长但不会丢弃任务、不会让 future 永远悬空。
    // 风险：禁止在任务函数体内触发本对象析构（worker 等自己 join 会死锁）；
    // 析构后不得再 submit（见下，会抛异常）
    ~ThreadPool();

    // 提交任务，返回 future 用于获取结果。
    // 线程安全：可被任意线程调用，仅短暂持锁入队；队列无容量上限，提交端永不阻塞，
    // 故“等待提交”与“等待执行”不会互为死锁。
    // 池已停止（析构中）时抛 std::runtime_error 而不是静默丢弃——调用方必须处理该异常，
    // 否则 future.get() 永远不会到来将导致上层逻辑挂死。
    // 生命周期：以成员函数指针+this 提交时（RollCallService 用法），必须保证任务
    // 执行完之前对象仍存活——RollCallService 将 thread_pool_ 声明在 recognizer_/db_
    // 之后，靠成员逆序析构保证先 join 完 worker，再释放依赖对象，规避悬垂 this。
    template<class F, class... Args>
    auto submit(F&& f, Args&&... args) 
        -> std::future<typename std::result_of<F(Args...)>::type>;

    // 等待当前已提交任务全部执行完（不阻止后续继续提交）。
    // 死锁警示：绝不能在 worker 线程（即任务函数内部）调用——自己等自己退出 busy 态；
    // 也不能与“持锁后 wait”混用。推荐用法仅限外部线程做批次屏障
    // （RollCallService 实际靠逐个 future.get() 收束，未用本接口）
    void wait();

private:
    void worker();

    std::vector<std::thread> workers_;           // 常驻工作线程，构造时全部拉起
    std::queue<std::function<void()>> tasks_;    // 待执行任务队列（FIFO，无界）
    
    std::mutex queue_mutex_;                     // 保护 tasks_/stop_/busy_threads_/pending_tasks_
    std::condition_variable condition_;          // 生产者->worker：有新任务或请求停止
    std::condition_variable wait_condition_;     // worker->wait() 调用方：busy/pending 归零
    
    bool stop_;             // 停止标志：true 后禁止提交，worker 清空队列即退出
    size_t busy_threads_;   // 正在执行任务、尚未归还的线程数
    size_t pending_tasks_;  // 已入队未完成的总数（含执行中）：submit 时 +1，任务跑完 -1
};

// 模板实现必须在头文件
// 注：std::result_of 在 C++17 起弃用（等价于 std::invoke_result_t），
// 本工程构建标准下仍可用；迁移 C++17+ 时可无痛替换，接口行为不变
template<class F, class... Args>
auto ThreadPool::submit(F&& f, Args&&... args) 
    -> std::future<typename std::result_of<F(Args...)>::type> {
    
    using return_type = typename std::result_of<F(Args...)>::type;
    
    // packaged_task 用 shared_ptr 托管：任务闭包可安全入队，即使调用方丢弃 future，
    // 任务本体仍由队列持有直至执行（结果留在共享状态里，不会悬垂）
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
    
    // future 先行取出：packaged_task 只能 get_future() 一次；调用方经 future.get()
    // 阻塞收结果或重放任务内抛出的异常
    std::future<return_type> res = task->get_future();
    
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (stop_) {
            throw std::runtime_error("submit on stopped ThreadPool");
        }
        
        // 入队仅捕获 task 的 shared_ptr 副本；执行 (*task)() 会把返回值/异常写入共享状态
        tasks_.emplace([task](){ (*task)(); });
        ++pending_tasks_;
    }
    
    // 出锁后再 notify：避免被唤醒的 worker 立刻回抢 mutex（惊群+优先级反转放大）
    condition_.notify_one();
    return res;
}

#endif // THREAD_POOL_H