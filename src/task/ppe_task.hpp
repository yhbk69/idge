#ifndef PPE_TASK_H
#define PPE_TASK_H

#include "base_task.h"
#include <iostream>
#include <atomic>
#include <memory>
#include "opencv2/opencv.hpp"
#include "DmaBufferPool.h"
#include "easy_timer.h"
#include "common.hpp"
#include "yolo11_model.hpp"
#include "frame_queue.h"
#include <thread>
#include "task_config.hpp"
#include "BlockingQueue.hpp"
#include "task_data.h"

class PpeTask
{
private:
    std::shared_ptr<YOLO11Model> model_;
    BlockingQueue<std::shared_ptr<TaskData>>* taskQueue_;
    std::thread thread_;
    std::atomic<bool> running_;
    TaskConfig config;

public:
    PpeTask(TaskConfig config);
    void init();
    void start();
    void put(std::shared_ptr<TaskData> task);
    void stop();
private:
    void run();
};

#endif //PPE_TASK_H
