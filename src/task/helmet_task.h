#ifndef HELMET_TASK_H
#define HELMET_TASK_H

#include "base_task.h"
#include <iostream>
#include "opencv2/opencv.hpp"
#include "DmaFrameBuffer.h"
#include "DmaBufferPool.h"
#include "easy_timer.h"
#include "common.hpp"

class HelmetTask
{

public:
    //static void run(cv::Mat& img, DetectContext context);
    static void runWithDma(DmaBuffer* detectFrame, DetectContext& context);
    static void run(image_buffer_t& image, DetectContext context);
};

#endif //HELMET_TASK_H
