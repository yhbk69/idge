#ifndef TASK_DATA_H
#define TASK_DATA_H

// ============================================================================
// task_data.h - 任务数据结构
// ============================================================================
//
// TaskData 是在线程间传递的数据单元，包含：
//   - 图像数据（待检测的帧）
//   - 时间戳（用于排序）
//   - 结果队列（检测结果写入这里）
//
// 数据流：
//   解码线程 → 创建 TaskData → 放入任务队列 → 推理线程取出 → 执行检测 → 结果写入 resultQueue
//
// 生命周期管理：
//   - 使用 shared_ptr 管理 TaskData 的生命周期
//   - 多个线程可以安全地共享同一个 TaskData
//   - 当所有引用都释放时，TaskData 自动销毁
//
// ============================================================================

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

// ============================================================================
// TaskData - 任务数据
// ============================================================================
// 在解码线程和推理线程之间传递的数据单元
//
// 使用场景：
//   解码线程调用 tasks_[k]->put(td) 时创建 TaskData
//   推理线程从任务队列取出 TaskData 并执行检测
//   检测结果写入 resultQueue，解码线程从 resultQueue 取出结果并画框
//
// ============================================================================
class TaskData
{
public:

    // ============================================================================
    // 构造函数
    // ============================================================================
    // 参数：
    //   - t: 时间戳（用于结果排序）
    //   - image: 图像数据（shared_ptr，多个任务共享）
    //   - resultQueue: 结果队列（检测结果写入这里）
    //
    // ============================================================================
    explicit TaskData(long t, std::shared_ptr<image_buffer_t> image, std::shared_ptr<PriorityQueue<object_detect_result_list>> resultQueue)
        :time(t), image(image), resultQueue(resultQueue)
    {
        
    }

    long time;                                          // 时间戳
    std::shared_ptr<image_buffer_t> image;              // 图像数据
    std::shared_ptr<PriorityQueue<object_detect_result_list>> resultQueue;  // 结果队列

    ~TaskData()
    {

    }

};


#endif