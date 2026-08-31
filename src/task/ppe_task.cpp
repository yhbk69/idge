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
    

    
}
void PpeTask::put(std::shared_ptr<TaskData> task)
{
    taskQueue_->push_latest(task);

}

void PpeTask::stop()
{
    running_ = false;
    thread_.join();
}