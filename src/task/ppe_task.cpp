// ============================================================================
// PpeTask - 推理任务（独立线程运行）
// ============================================================================
//
// 【作用】
//   在独立线程中执行 RKNN NPU 推理，与解码线程并行工作。
//
// 【线程模型】
//   - 解码线程：读取视频帧 → RGA 转换 → 放入任务队列
//   - 推理线程(PpeTask)：从队列取出帧 → RKNN 推理 → 结果放入结果队列
//   - 解码线程：从结果队列取出结果 → 画框 → 渲染
//
//   这种生产者-消费者模型实现了流水线并行：
//   解码、推理、渲染可以同时进行，提高整体吞吐量。
//
// 【任务队列】
//   - 容量为 2（只保留最新 2 帧）
//   - 如果队列满，旧帧会被丢弃（push_latest 语义）
//   - 这保证了低延迟：始终处理最新的帧
//
// 【NPU 核心分配】
//   每个 PpeTask 绑定到特定的 NPU 核心（通过 core_mask）
//   多个 PpeTask 可以并行运行在不同核心上
//
// 【代码审查视角的补充注记】
//   - 时间戳回填链：od_results.time = taskData->time（解码线程采集帧时
//     刻，epoch 纳秒）。common.hpp 文档称该字段为"毫秒"、而下游
//     alarm 限流常量按纳秒比较——三方不一致，详见 common.hpp 的
//     ⚠ 单位隐患长注释，改动任何一侧前务必先读。
//   - 停止路径有两套：stop()（阻塞 join，保证干净）与
//     stopBestEffort()（超时 detach，保 UI 响应）。后者留下
//     "线程在途 + 对象析构"窗口，与本文件析构函数 delete taskQueue_
//     组合存在 use-after-free 隐患 → 上层对曾 detach 的实例只停不删。
//   - 与 ffmpeg_video_decoder 的 tasks_ "故意不 delete" 策略互证。
// ============================================================================

#include <math.h>
#include <chrono>
#include <functional>
#include "ppe_task.hpp"
#include "ThreadPool.hpp"
#include "DmaBufferPool.h"
#include "easy_timer.h"
#include <QDebug>

// 构造仅浅拷贝配置（modelPath/labelPath/core_mask/result_id）；
// 模型加载与队列创建延迟到 init()：构造函数保持轻量、可安全提前构造，
// 耗时的 NPU 初始化推迟到 start() 显式触发（构造函数抛异常难以处理）。
PpeTask::PpeTask(TaskConfig config)
{
    this->config = config;

}

// ============================================================================
// 初始化推理任务
// ============================================================================
// 创建 YOLO11Model 实例和任务队列
//
// 注意：模型初始化比较耗时（加载 .rknn 文件 + 初始化 NPU）
//       所以放在 start() 中，而不是构造函数中
// ============================================================================
void PpeTask::init() 
{
    model_ = std::make_shared<YOLO11Model>(
            config.modelPath,
            config.labelPath,
            config.core_mask);  // 指定 NPU 核心
    
    // 任务队列容量为 2：只保留最新 2 帧
    // 如果队列满，push_latest 会丢弃最旧的帧
    taskQueue_ = new BlockingQueue<std::shared_ptr<TaskData>>(2);

}

// ============================================================================
// 启动推理线程
// ============================================================================
// 初始化模型后，启动独立线程运行 run()
// 线程会持续从任务队列中取帧进行推理
// ============================================================================
void PpeTask::start() 
{
    init();  // 初始化模型（可能耗时几秒）
    
    running_ = true;

    // 启动推理线程
    // 每个 PpeTask 有自己独立的线程，可以并行推理
    thread_ = std::thread(&PpeTask::run, this);


}

// ============================================================================
// 推理线程主循环
// ============================================================================
// 持续从任务队列中取帧进行推理：
//   1. 从队列取出一帧（阻塞等待，超时 200ms）
//   2. 调用 model_->detect() 执行 RKNN 推理
//   3. 将检测结果放入结果队列
//
// 注意：
//   - running_ 为 false 时退出循环
//   - taskQueue_->pop() 超时 200ms，避免无限阻塞
//     （为什么用带超时的 pop 而不是永久阻塞：退出条件靠轮询 running_
//       实现，200ms 是"停止请求响应延迟"与"空转开销"的折中；
//       requestStop 中 close() 也会立即唤醒 pop）
//   - 推理过程中会检查 running_，及时响应停止请求
//     （pop 成功后、detect 前后各查一次，最迟在下一轮循环头退出）
// ============================================================================
void PpeTask::run()
{
    finished_.store(false);
    TIMER t;
    while (running_)
    {
        std::shared_ptr<TaskData> taskData;
        // 从任务队列取帧（超时 200ms）
        // 如果队列为空，会阻塞等待 200ms
        if (!taskQueue_->pop(taskData, 200))
        {
            continue;  // 超时，继续循环
        }

        object_detect_result_list od_results;
        if (!running_)
        {
            break;  // 收到停止请求，退出
        }

        // ===== 执行 RKNN 推理 =====
        // detect() 会：
        //   1. 将 RGB 图像送入 NPU 输入内存
        //   2. 调用 rknn_run() 执行推理
        //   3. 从 NPU 输出内存读取结果
        //   4. 后处理：解析检测框、类别、置信度
        // 注意：detect 对同一 model_ 实例非线程安全，但本任务模型独占，
        //       仅本线程调用，满足"单实例单线程"纪律。
        model_->detect(taskData->image, &od_results, true);
        
        // 添加时间戳（用于结果排序）
        // taskData->time 由解码线程在采集时刻写入（epoch 纳秒）；
        // ⚠ common.hpp 文档称该字段为毫秒、alarm 限流按纳秒比较，
        //   单位三方不一致，详见 common.hpp 隐患注释，勿在此"顺手换算"。
        od_results.time = taskData->time;
        // 结果路由ID（摄像头预览多检测器区分来源；主流水线默认0）
        // 消费者（camera_preview_decoder）按 id 选择对应显示槽位，
        // 这是"一条结果队列多路复用多个检测器"的关键标记。
        od_results.id = config.result_id;
        
        // 将结果放入结果队列
        // 解码线程会从这个队列中取出结果并画框
        // resultQueue 随 TaskData 传入：任务线程无需知道队列归属，
        // 一图多任务时各任务共享同一队列对象（shared_ptr 语义）。
        taskData->resultQueue->push(od_results);
        
        if (!running_)
        {
            break;
        }
    }
    // 标记线程已自然退出（stopBestEffort 轮询这个标志）
    // 注意 finished_ 只在本线程出口写 true，外层据此判断 join 安全性
    finished_.store(true);
}

// ============================================================================
// 提交任务到队列
// ============================================================================
// 使用 push_latest 语义：如果队列满，丢弃最旧的任务
// 这保证了始终处理最新的帧，避免延迟累积
// ============================================================================
void PpeTask::put(std::shared_ptr<TaskData> task)
{
    taskQueue_->push_latest(task);
}

// ============================================================================
// 请求停止（非阻塞）
// ============================================================================
// 设置 running_ = false 并关闭队列
// 关闭队列会唤醒阻塞在 pop() 上的线程
// ============================================================================
void PpeTask::requestStop()
{
    running_ = false;
    if (taskQueue_) {
        taskQueue_->close();  // 唤醒阻塞在 pop() 上的线程
    }
}

// ============================================================================
// 等待线程退出（阻塞）
// ============================================================================
// 注意：如果 NPU 正在推理，可能需要等待较长时间
// 不要在 UI 线程中调用此函数，会导致界面卡死
// ============================================================================
void PpeTask::join()
{
    if (thread_.joinable()) {
        thread_.join();
    }
}

// ============================================================================
// 停止任务（阻塞）
// ============================================================================
// 先请求停止，再等待线程退出
// ============================================================================
void PpeTask::stop()
{
    requestStop();
    join();
}

// ============================================================================
// 有界停止（避免 UI 卡死）
// ============================================================================
// 最多等待 waitMs 毫秒，超时就 detach 线程
//
// 为什么需要这个？：
//   - NPU 忙时，推理线程可能无法及时响应 stop 请求
//   - 如果无限等待，主线程会卡死
//   - detach 虽然不优雅（线程继续运行直到自然退出），但保证了系统响应性
//
// 流程：
//   1. requestStop(): 设置 running_=false，关闭队列
//   2. 轮询 finished_ 标志（每 20ms 检查一次）
//   3. 如果线程在做推理，通常几十~几百ms 就会退出
//   4. 超时（默认 1.5s）就 detach，不再等待
//
// ⚠ detach 后的所有权隐患（代码审查重点）：
//   detach 仅意味着"没人再等它"，run() 线程仍在访问 this 的
//   taskQueue_/model_/config。若调用方随后销毁本对象，析构函数会
//   delete taskQueue_、释放 model_ 与 config，在途线程继续读写即
//   use-after-free。规避方式是上层保证"曾 detach 的实例不删除"
//   （ffmpeg_video_decoder 对 tasks_ 正是如此），而不是在这里补偿。
// ============================================================================
void PpeTask::stopBestEffort(int waitMs)
{
    requestStop();
    if (!thread_.joinable()) return;

    // 轮询等待线程退出
    for (int waited = 0; waited < waitMs; waited += 20) {
        if (finished_.load()) {
            // 线程已退出，安全 join
            thread_.join();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // 超时：脱离线程，绝不阻塞调用方
    // 线程会在下次检查 running_ 时自然退出
    if (thread_.joinable()) {
        thread_.detach();
    }
}

// ============================================================================
// 析构函数
// ============================================================================
// 清理顺序与前提：
//   1. 若线程仍 joinable（未经 stopBestEffort detach），先 stop() 阻塞等待
//      run() 退出——保证 delete taskQueue_ 时无线程再碰它；
//   2. delete taskQueue_（裸指针手工释放，init() 未调用时可能为未定义值？
//      不会：类内声明未初始化，但 put/requestStop 均有判空/时序约定，
//      未 init 即析构属用法错误路径）；
//   3. model_/thread_ 成员随析构自动释放（shared_ptr 引用计数归零；
//      但 YOLO11Model 自身无析构函数，NPU 上下文泄漏问题见其头文件注记）。
// ⚠ 若此前某线程已被 detach（stopBestEffort 超时路径），第 1 步的
//   joinable() 为 false 直接跳过，第 2 步 delete 时在途线程可能仍在
//   pop/访问队列 → use-after-free。上层必须遵守"detach 过就不销毁"
//   的配套纪律（见 stopBestEffort 注释与 ffmpeg_video_decoder 策略）。
// ============================================================================
PpeTask::~PpeTask()
{
    if (thread_.joinable()) {
        stop();
    }
    delete taskQueue_;
    taskQueue_ = nullptr;
}

// ============================================================================
// restartThread: 复位队列并重启推理线程
// ============================================================================
// 前提：调用方已确认旧线程退出（finished_），否则旧线程复活后与新线程
// 双消费同一队列，且形成对 model_ 的并发读写。
// requestStop() 已把 taskQueue_ close，close 复位必须经 reopen()——
// 这正是旧实现换模型后该槽位永久丢帧（push_latest 恒 false、pop 恒空转）的根因。
// ============================================================================
void PpeTask::restartThread()
{
    taskQueue_->clear();
    taskQueue_->reopen();
    running_ = true;
    finished_ = false;
    thread_ = std::thread(&PpeTask::run, this);
}

// ============================================================================
// reloadModel: 热更新模型
// ============================================================================
// 安全地替换推理模型，流程：
//   1. 有界停止推理线程，并确认其已退出（finished_）
//   2. 创建新模型（失败则用旧模型原样重启）
//   3. 替换模型 + 复位队列 + 重启推理线程
//
// UAF 防线（修复旧实现的三连隐患）：
//   旧实现 stopBestEffort 超时 detach 后**照常**替换 model_ 并重启线程——
//   在途线程仍引用旧 model_/队列，属 use-after-free + 双消费者；且重启后
//   队列保持 close 状态，该槽位永久丢帧。现在：线程未确认退出就直接放弃
//   本次热更新（本槽位保持停止，上层可稍后重试——旧线程 soon 会因
//   running_==false 自行退出并置 finished_）。
// ============================================================================
bool PpeTask::reloadModel(const std::string &newPath,
                           const std::string &newLabelPath,
                           rknn_core_mask coreMask)
{
    qInfo() << "PpeTask: Reloading model from" << QString::fromStdString(newPath);

    // 1. 停止推理线程（有界等待）并确认已退出
    stopBestEffort(2000);
    if (!finished_.load()) {
        qWarning() << "PpeTask: reload aborted - inference thread still in flight"
                   << "(model unchanged for this slot; retry after it exits)";
        return false;
    }
    // finished_ 已置位但线程对象仍 joinable（超时边界上刚好退出的竞态）：回收之
    if (thread_.joinable()) {
        thread_.join();
    }
    running_ = false;

    // 2. 尝试创建新模型（如果失败，旧模型原样重启）
    std::shared_ptr<YOLO11Model> newModel;
    try {
        newModel = std::make_shared<YOLO11Model>(newPath, newLabelPath, coreMask);
    } catch (const std::exception &e) {
        qWarning() << "PpeTask: Failed to load new model:" << e.what();
        restartThread();  // 旧线程已确认退出，重启安全
        return false;
    }

    // 3. 替换模型与配置（旧模型在 shared_ptr 析构时自动释放 NPU 资源；
    //    此刻已无任何线程引用旧模型——第 1 步的 finished_ 门槛保证）
    model_ = newModel;
    config.modelPath = newPath;
    config.labelPath = newLabelPath;
    config.core_mask = coreMask;

    // 4. 清空残留旧帧 + 复位队列 + 重启推理线程
    restartThread();

    qInfo() << "PpeTask: Model reloaded successfully";
    return true;
}