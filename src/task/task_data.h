#ifndef TASK_DATA_H
#define TASK_DATA_H

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

class TaskData
{
public:

    explicit TaskData(long t, std::shared_ptr<image_buffer_t> image, std::shared_ptr<PriorityQueue<object_detect_result_list>> resultQueue)
        :time(t), image(image), resultQueue(resultQueue)
    {
        
    }

    long time;
    std::shared_ptr<image_buffer_t> image;
    std::shared_ptr<PriorityQueue<object_detect_result_list>> resultQueue;

    ~TaskData()
    {

    }

};


#endif