#ifndef HELMET_TASK_H
#define HELMET_TASK_H

// =====================================================================
// 头文件引用
// =====================================================================
#include "base_task.h"          // 复用其数据结构 DetectContext/DetectResult（注：本类并不继承 BaseTask）
#include <iostream>             // 标准输入输出流
#include "opencv2/opencv.hpp"   // OpenCV图像处理库
#include "DmaFrameBuffer.h"     // DMA帧缓冲区，用于零拷贝图像传输
#include "DmaBufferPool.h"      // DMA缓冲池，管理DMA内存分配与释放
#include "easy_timer.h"         // 计时器工具，用于性能测量
#include "common.hpp"           // 通用类型定义（DetectContext等）

// =====================================================================
// HelmetTask类 - 头盔检测任务
// =====================================================================
// 作用：执行头盔检测推理任务，支持多种输入方式
// 通过NPU加速进行目标检测，支持DMA零拷贝和常规图像缓冲区
//
// 审查注记（使用前提与现状）：
//   - 本类不继承 BaseTask，仅复用其头文件中的 DetectContext/image_buffer_t；
//     全部方法为 static 无状态，可多线程各自调用；
//   - 但模型取自 thread_local dpool::context（见 ThreadPool.hpp），
//     因此 run() **必须**运行在 ThreadPool 工作线程上，否则判空直接返回；
//   - run() 的推理结果目前未写回任何队列/上下文（链路未完成，见 .cpp）；
//   - runWithDma 为空实现占位。
// =====================================================================
class HelmetTask
{

public:
    // =================================================================
    // runWithDma - DMA零拷贝推理（预留接口）
    // =================================================================
    // 作用：设计意图是直接使用 DMA 缓冲区指针喂给 RKNN 做零拷贝推理；
    //       当前为**空实现**（函数体无任何逻辑），调用无效果。
    // 参数：
    //   detectFrame - DMA缓冲区指针，指向待检测的图像数据
    //                 DMA零拷贝避免CPU内存拷贝，提升性能（设计目标）
    //   context - 检测上下文，包含检测结果和配置信息
    // =================================================================
    //static void run(cv::Mat& img, DetectContext context);
    static void runWithDma(DmaBuffer* detectFrame, DetectContext& context);

    // =================================================================
    // run - 常规图像推理
    // =================================================================
    // 作用：使用常规图像缓冲区进行头盔检测推理
    // 参数：
    //   image - 图像缓冲区引用，包含待检测图像；其 dmaBuffer 以
    //           "手工借出"方式传入，本函数负责所有出口路径 release 归还
    //   context - 检测上下文（**按值传递**：DetectContext 是 POD，
    //             拷贝仅复制两个 shared_ptr，成本低且避免悬挂引用）
    // 返回：无——结果对象 results 在函数内丢弃，属现状缺陷（见 .cpp）
    // =================================================================
    static void run(image_buffer_t& image, DetectContext context);
};

#endif //HELMET_TASK_H
