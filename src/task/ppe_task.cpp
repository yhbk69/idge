#include <math.h>
#include <chrono>
#include <functional>
#include "ppe_task.hpp"
#include "ThreadPool.hpp"
#include "DmaBufferPool.h"
#include "easy_timer.h"

PpeTask::PpeTask(TaskConfig config)
{
    this->config = config;

}

void PpeTask::init() 
{
    model_ = std::make_shared<YOLO11Model>(
            config.modelPath,
            config.labelPath,
            config.core_mask);
    taskQueue_ = new BlockingQueue<std::shared_ptr<TaskData>>(2);

}

void PpeTask::start() 
{
    init();
    
    running_ = true;

    thread_ = std::thread(&PpeTask::run, this);


}

void PpeTask::run()
{
    finished_.store(false);
    TIMER t;
    while (running_)
    {
        std::shared_ptr<TaskData> taskData;
        if (!taskQueue_->pop(taskData, 200))
        {
            continue;
        }

        object_detect_result_list od_results;
        if (!running_)
        {
            break;
        }

        model_->detect(taskData->image, &od_results, true);
        od_results.time = taskData->time;
        taskData->resultQueue->push(od_results);
        if (!running_)
        {
            break;
        }
    }
    // 标记线程已自然退出（stopBestEffort 轮询这个标志）
    finished_.store(true);
}

void PpeTask::put(std::shared_ptr<TaskData> task)
{
    taskQueue_->push_latest(task);
}

void PpeTask::requestStop()
{
    running_ = false;
    if (taskQueue_) {
        taskQueue_->close();  // 唤醒阻塞在 pop() 上的线程
    }
}

void PpeTask::join()
{
    if (thread_.joinable()) {
        thread_.join();
    }
}

void PpeTask::stop()
{
    requestStop();
    join();
}

// 有界停止：避免在 UI 线程里 join 一个卡在 rknn 推理上的线程导致界面假死
// - requestStop 后最多等 waitMs 毫秒（线程如果在做一次推理，通常几十~几百ms就退出）
// - 超时仍不退（NPU 忙/异常）就 detach，不再等待；任务对象本身不释放，安全
void PpeTask::stopBestEffort(int waitMs)
{
    requestStop();
    if (!thread_.joinable()) return;

    for (int waited = 0; waited < waitMs; waited += 20) {
        if (finished_.load()) {
            thread_.join();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // 超时：脱离线程，绝不阻塞调用方
    if (thread_.joinable()) {
        thread_.detach();
    }
}

PpeTask::~PpeTask()
{
    if (thread_.joinable()) {
        stop();
    }
}