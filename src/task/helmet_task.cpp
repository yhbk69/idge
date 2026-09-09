// ============================================================================
// helmet_task.cpp - 安全帽检测任务实现
// ============================================================================
//
// 功能：
//   执行目标检测推理，调用 YOLO11Model 进行检测。
//
// 工作流程：
//   1. 从 ModelPool 获取模型实例（按模型 ID）
//   2. 调用 model->detect() 执行推理（RKNN NPU）
//   3. 推理完成后释放 DMA 缓冲区
//
// 级联模型支持：
//   可以依次调用多个模型进行级联检测，
//   每个模型的检测结果合并后输出。
//
// ============================================================================

#include <math.h>
#include <chrono>
#include <functional>
#include "helmet_task.h"
#include "ThreadPool.hpp"
#include "DmaBufferPool.h"
#include "easy_timer.h"

// ============================================================================
// run - 执行安全帽检测（使用 image_buffer_t）
// ============================================================================
// 参数：
//   - image: 输入图像（NV12 格式，包含 DMA-BUF fd）
//   - context: 检测上下文（包含模型池、DMA 缓冲池等）
//
// 流程：
//   1. 从模型池获取模型 "1"（YOLO11Model）
//   2. 调用 detect() 执行推理
//   3. 释放 DMA 缓冲区
//
// ============================================================================
void HelmetTask::run(image_buffer_t& image, DetectContext context) 
{
    object_detect_result_list results1;

    // 从模型池获取模型 "1"（对应 NPU 核心 0）
    std::shared_ptr<YOLO11Model> model = dpool::context->getModel("1");
    
    auto t1 = chrono::system_clock::now();
    model->detect(&image, &results1, true);  // 执行推理（RKNN NPU）

    // 释放 DMA 缓冲区（避免内存泄漏）
    context.dmaBufferPool->release(image.dmaBuffer);
}

// ============================================================================
// runWithDma - 使用 DMA-BUF 直接推理（零拷贝）
// ============================================================================
// 参数：
//   - detectFrame: DMA-BUF 帧（直接传给 RKNN 推理）
//   - context: 检测上下文
//
// 注意：此函数当前为未实现状态（代码被注释）
// 预期流程：
//   1. model->setInputDmaBuf(detectFrame): 设置 RKNN 输入
//   2. model->run(): 执行推理
//   3. model->getResult(): 获取输出结果
//   4. context.detectResultQueue->push(r): 推送到结果队列
//
// ============================================================================
void HelmetTask::runWithDma(DmaBuffer* detectFrame, DetectContext& context)
{
    // 当前为未实现状态，代码被注释
}


