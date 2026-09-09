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
#include <atomic>
#include "task_config.hpp"
#include "BlockingQueue.hpp"
#include "task_data.h"

class PpeTask
{
private:
    std::shared_ptr<YOLO11Model> model_;
    BlockingQueue<std::shared_ptr<TaskData>>* taskQueue_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};   // 线程 run() 是否已退出
    TaskConfig config;

public:
    PpeTask(TaskConfig config);
    ~PpeTask();
    void init();
    void start();
    void put(std::shared_ptr<TaskData> task);
    // 仅请求停止（关队列唤醒线程），不阻塞
    void requestStop();
    // 等待线程退出（阻塞，可能很久——不要在 UI 线程直接调用）
    void join();
    void stop();
    // 有界停止：最多等 waitMs 毫秒，仍不退就 detach，保证 UI 不卡死
    void stopBestEffort(int waitMs = 1500);
private:
    void run();
};

#endif //PPE_TASK_H
