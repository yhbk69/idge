#ifndef DPOOL_THREADPOOL_H
#define DPOOL_THREADPOOL_H

// ============================================================================
// ThreadPool - 线程池实现
// ============================================================================
//
// 作用：
//   管理一组工作线程，用于并发执行任务。
//   避免频繁创建/销毁线程的开销。
//
// 使用场景：
//   - 并发推理：多个视频通道同时检测
//   - 并发编码：多个视频流同时编码
//   - 并发写盘：多个截图同时保存
//
// 设计特点：
//   - 动态扩缩容：根据任务量自动调整线程数
//   - 空闲回收：空闲线程超时后自动退出
//   - 单例模式：全局唯一实例
//   - 每线程独立上下文：每个线程有自己的 RKNN 模型实例
//
// ============================================================================

#include <cassert>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include "yolo11_model.hpp"
#include "rknn_api.h"
#include "SharedTypes.hpp"

// NPU 核心掩码数组（循环分配：0 → 1 → 2 → 0 → 1）
static const rknn_core_mask NPU_CORES[] = {
    RKNN_NPU_CORE_0,
    RKNN_NPU_CORE_1,
    RKNN_NPU_CORE_2,
    RKNN_NPU_CORE_0,
    RKNN_NPU_CORE_1
};

// 模型配置结构体（用于从外部传入配置）
struct ModelConfig {
    std::string modelPath;   // RKNN 模型文件路径
    std::string labelPath;   // 标签文件路径
    std::string note;        // 模型备注
};

// ============================================================================
// ExecuteContext - 执行上下文
// ============================================================================
// 每个工作线程拥有独立的 ExecuteContext，包含：
//   - 模型实例（YOLO11Model）
//   - 配置信息
//
// 为什么每个线程需要独立上下文？：
//   - RKNN 模型不是线程安全的
//   - 多个线程共享一个模型会导致竞争
//   - 每个线程有自己的模型实例，避免竞争
//
// ============================================================================
// ============================================================================
// ExecuteContext - 执行上下文（管理模型实例）
// ============================================================================
// 每个线程有自己的 ExecuteContext，包含多个 YOLO11Model 实例。
// 通过 thread_local 确保线程隔离。
//
// 级联模型配置（从 config.json 读取）：
//   "cascade": {
//     "models": [
//       {"path": "model/person.rknn", "label": "model/person_labels.txt", "note": "行人检测"},
//       {"path": "model/helmet.rknn", "label": "model/helmet_labels.txt", "note": "安全帽检测"},
//       {"path": "model/vest.rknn",   "label": "model/vest_labels.txt",   "note": "背心检测"}
//     ]
//   }
// ============================================================================
class ExecuteContext
{
private:
    // 模型实例表（按 modelId 索引）
    std::unordered_map<std::string, std::shared_ptr<YOLO11Model>> m_models;

public:
    // 初始化：从配置列表加载所有模型
    void init(AppConfig config, const std::vector<ModelConfig> &modelConfigs = {})
    {
        // 如果没有传入配置，使用默认配置
        std::vector<ModelConfig> configs = modelConfigs;
        if (configs.empty()) {
            configs = {
                {"model/yolo11n.rknn", "model/coco_80_labels_list.txt", "默认模型"}
            };
        }

        for (int i = 0; i < configs.size() && i < 5; ++i) {
            const auto &cfg = configs[i];

            // 跳过未配置的模型（路径为空）
            if (cfg.modelPath.empty()) {
                continue;
            }

            // 循环分配 NPU 核心（0, 1, 2, 0, 1）
            rknn_core_mask coreMask = NPU_CORES[i % 5];

            std::string modelId = std::to_string(i + 1);

            std::cout << "[ExecuteContext] 加载模型 " << modelId
                      << ": path=" << cfg.modelPath
                      << ", label=" << cfg.labelPath
                      << ", core=" << (i % 3)
                      << ", note=" << cfg.note
                      << std::endl;

            // 创建 YOLO11Model 实例（构造时自动加载标签文件）
            auto model = std::make_shared<YOLO11Model>(
                cfg.modelPath,
                cfg.labelPath,
                coreMask);

            m_models[modelId] = model;
        }

        std::cout << "[ExecuteContext] 共加载 " << m_models.size() << " 个模型" << std::endl;
    }

    // 获取指定 ID 的模型实例
    std::shared_ptr<YOLO11Model> getModel(std::string modelId)
    {
        if (modelId.empty()) {
            return NULL;
        }
        return m_models[modelId];
    }

    // 获取指定 ID 模型的类别名称列表
    std::vector<std::string> getClassNames(const std::string &modelId)
    {
        if (m_models.count(modelId)) {
            return m_models[modelId]->getClassNames();
        }
        return {};
    }

    // 获取已加载的模型数量
    int getModelCount() const
    {
        return m_models.size();
    }

    void putResult()
    {
        
    }

};

namespace dpool
{

    // 线程本地存储：每个线程有自己的 ExecuteContext
    // 使用 thread_local 确保每个线程独立初始化
    inline thread_local shared_ptr<ExecuteContext> context;

    class ThreadPool
    {
    public:
        using MutexGuard = std::lock_guard<std::mutex>;
        using UniqueLock = std::unique_lock<std::mutex>;
        using Thread = std::thread;
        using ThreadID = std::thread::id;
        using Task = std::function<void()>;

        // 默认构造：使用硬件并发数（CPU 核心数）
        ThreadPool()
            : ThreadPool(Thread::hardware_concurrency())
        {
        }

        // 指定最大线程数
        explicit ThreadPool(size_t maxThreads)
            : quit_(false),
              currentThreads_(0),
              idleThreads_(0),
              maxThreads_(maxThreads)
        {
        }

        // 获取单例实例的静态方法
        // C++11 保证了静态局部变量的线程安全性
        static ThreadPool &getInstance()
        {
            static ThreadPool instance; // C++11 保证了静态局部变量的线程安全性
            
            return instance;
        }

        inline void setMaxThreads(size_t maxThreads)
        {
            this->maxThreads_ = maxThreads;
        }

        // 设置模型配置列表（在启动线程前调用）
        inline void setModelConfigs(const std::vector<ModelConfig> &configs)
        {
            modelConfigs_ = configs;
        }

        // 禁用拷贝操作
        ThreadPool(const ThreadPool &) = delete;
        ThreadPool &operator=(const ThreadPool &) = delete;

        // 析构函数：停止所有工作线程
        ~ThreadPool()
        {
            {
                MutexGuard guard(mutex_);
                quit_ = true;  // 通知所有线程退出
            }
            cv_.notify_all();  // 唤醒所有等待的线程

            // 等待所有线程退出
            for (auto &elem : threads_)
            {
                assert(elem.second.joinable());
                elem.second.join();
            }
        }

        // ============================================================================
        // submit: 提交任务到线程池
        // ============================================================================
        // 参数：
        //   - func: 要执行的函数
        //   - params: 函数参数
        //
        // 返回：std::future，可以等待任务完成并获取结果
        //
        // 工作流程：
        //   1. 将函数和参数打包成 packaged_task
        //   2. 将 task 放入任务队列
        //   3. 如果有空闲线程，唤醒一个
        //   4. 如果没有空闲线程且未达上限，创建新线程
        //   5. 返回 future，调用者可以等待结果
        //
        // ============================================================================
        template <typename Func, typename... Ts>
        auto submit(Func &&func, Ts &&...params)
            -> std::future<typename std::result_of<Func(Ts...)>::type>
        {
            // 将函数和参数绑定
            auto execute = std::bind(std::forward<Func>(func), std::forward<Ts>(params)...);

            using ReturnType = typename std::result_of<Func(Ts...)>::type;
            using PackagedTask = std::packaged_task<ReturnType()>;

            // 创建 packaged_task，用于获取异步结果
            auto task = std::make_shared<PackagedTask>(std::move(execute));
            auto result = task->get_future();

            MutexGuard guard(mutex_);
            assert(!quit_);

            // 将 task 放入任务队列
            tasks_.emplace([task]()
                           { (*task)(); });
            
            // 调度策略：
            //   1. 如果有空闲线程，唤醒一个
            //   2. 如果没有空闲线程但未达上限，创建新线程
            //   3. 否则等待有线程变为空闲
            if (idleThreads_ > 0)
            {
                cv_.notify_one();
            }
            else if (currentThreads_ < maxThreads_)
            {
                // 创建新工作线程
                Thread t(&ThreadPool::worker, this);
                assert(threads_.find(t.get_id()) == threads_.end());
                threads_[t.get_id()] = std::move(t);
                ++currentThreads_;
            }

            return result;
        }

        size_t threadsNum() const
        {
            MutexGuard guard(mutex_);
            return currentThreads_;
        }

    private:
        // ============================================================================
        // worker: 工作线程主函数
        // ============================================================================
        // 每个工作线程执行此函数，循环等待并执行任务。
        //
        // 生命周期：
        //   1. 首次执行时初始化 ExecuteContext（加载 RKNN 模型）
        //   2. 循环等待任务
        //   3. 执行任务
        //   4. 超时或收到退出信号时退出
        //
        // 超时退出机制：
        //   - 空闲线程等待 2 秒后自动退出
        //   - 避免空闲线程占用系统资源
        //   - 下次有任务时会创建新线程
        //
        // ============================================================================
        void worker()
        {
            // 首次执行时初始化执行上下文（加载 RKNN 模型）
            if (context.get() == nullptr)
            {
                //std::cout << "thread Id:" << std::this_thread::get_id() << " context init" << std::endl;
                context = std::make_shared<ExecuteContext>();
                context->init(config, modelConfigs_);
            }
            
            while (true)
            {
                Task task;
                {
                    UniqueLock uniqueLock(mutex_);
                    ++idleThreads_;  // 标记为闲
                    
                    // 等待任务或退出信号
                    // 超时时间：WAIT_SECONDS 秒
                    auto hasTimedout = !cv_.wait_for(uniqueLock,
                                                     std::chrono::seconds(WAIT_SECONDS),
                                                     [this]()
                                                     {
                                                         return quit_ || !tasks_.empty();
                                                     });
                    --idleThreads_;  // 不再闲
                    
                    if (tasks_.empty())
                    {
                        if (quit_)
                        {
                            // 收到退出信号
                            --currentThreads_;
                            return;
                        }
                        if (hasTimedout)
                        {
                            // 超时退出（空闲线程自动回收）
                            --currentThreads_;
                            joinFinishedThreads();
                            finishedThreadIDs_.emplace(std::this_thread::get_id());
                            return;
                        }
                    }
                    // 取出任务
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                // 执行任务（在锁外执行，允许并发）
                task();
            }
        }

        // 回收已退出的线程
        void joinFinishedThreads()
        {
            while (!finishedThreadIDs_.empty())
            {
                auto id = std::move(finishedThreadIDs_.front());
                finishedThreadIDs_.pop();
                auto iter = threads_.find(id);

                assert(iter != threads_.end());
                assert(iter->second.joinable());

                iter->second.join();  // 等待线程退出
                threads_.erase(iter); // 从 map 中移除
            }
        }

        static constexpr size_t WAIT_SECONDS = 2;  // 空闲线程超时时间（秒）

        bool quit_;                          // 是否正在退出
        size_t currentThreads_;              // 当前线程数
        size_t idleThreads_;                 // 空闲线程数
        size_t maxThreads_;                  // 最大线程数

        mutable std::mutex mutex_;           // 互斥锁
        std::condition_variable cv_;         // 条件变量
        std::queue<Task> tasks_;             // 任务队列
        std::queue<ThreadID> finishedThreadIDs_;  // 已退出线程的 ID
        std::unordered_map<ThreadID, Thread> threads_;  // 线程表
        AppConfig config;                    // 配置信息
        std::vector<ModelConfig> modelConfigs_;  // 模型配置列表
    };

    constexpr size_t ThreadPool::WAIT_SECONDS;

} // namespace dpool

#endif /* DPOOL_THREADPOOL_H */