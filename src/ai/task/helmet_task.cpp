// ============================================================================
// helmet_task.cpp - 安全帽检测任务实现
// ============================================================================
//
// 功能：
//   执行目标检测推理，调用 YOLO11Model 进行检测。
//
// 工作流程：
//   1. 从 thread_local dpool::context（服务定位器，见 ThreadPool.hpp）
//      按字符串 ID 获取模型实例——注意这不是 src/model/ModelPool，
//      而是 ThreadPool 为每个工作线程挂载的执行上下文
//   2. 调用 model->detect() 执行推理（RKNN NPU）
//   3. 推理完成后释放 DMA 缓冲区
//
// 现状注记（代码审查视角）：
//   - 本文件目前只有**单级**检测：results1 推理后既未写入
//     context.detectResultQueue、也未通过出参回传，即"算完即弃"，
//     检测链路在此分支上是未闭合的（与 ppe_task 的完整闭环对照）；
//   - "级联模型支持"仅为设计意图，当前代码未实现任何级联调用；
//   - 每个 return 路径都先 context.dmaBufferPool->release(image.dmaBuffer)
//     再返回——DMA 缓冲为手工借还，新增 return 分支必须保持该配对。
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
//   - image: 输入图像（NV12 格式，包含 DMA-BUF fd）。其 dmaBuffer 由调用方
//            借出，所有权约定见 base_task.h DetectContext 注释：被调方归还
//   - context: 检测上下文（按值拷贝，仅复制 shared_ptr）
//
// 流程：
//   1. 判空 dpool::context（thread_local 服务定位器）：为空说明当前线程
//      不是 ThreadPool 工作线程，无法取模型——**先归还 DMA 再返回**
//   2. 经 dpool::context->getModel("1") 取 YOLO11Model 实例
//      （"1" 只是注册表字符串键，与 NPU 核心编号无关；取不到同样先归还 DMA）
//   3. model->detect() 执行推理（RKNN NPU），want_float=true 走浮点反量化路径
//   4. 释放 DMA 缓冲区（避免泄漏）
//
// 审查注记：
//   - results1 是局部变量：detect 写入后即随函数返回销毁，未进任何队列、
//     未回传调用方——本任务链路的"最后一公里"尚未接上（现状如实记录）；
//   - t1 计时变量取完即弃，未参与任何统计输出（残留的调试代码痕迹）；
//   - detect 内部对同一模型实例非线程安全：若多线程共用 dpool 中同一
//     shared_ptr<YOLO11Model>，需要上层保证串行调用（见 yolo11_model.hpp）。
// ============================================================================
void HelmetTask::run(image_buffer_t& image, DetectContext context) 
{
    object_detect_result_list results1;

    // 服务定位器判空：dpool::context 仅 ThreadPool 工作线程上有值
    if (!dpool::context) {
        std::cerr << "[HelmetTask] 错误: context 为空，当前线程不是工作线程" << std::endl;
        context.dmaBufferPool->release(image.dmaBuffer);   // 提前返回也必须归还 DMA
        return;
    }

    std::shared_ptr<YOLO11Model> model = dpool::context->getModel("1");
    if (!model) {
        std::cerr << "[HelmetTask] 错误: 模型 '1' 未加载" << std::endl;
        context.dmaBufferPool->release(image.dmaBuffer);   // 同上，借还配对
        return;
    }
    
    auto t1 = chrono::system_clock::now();   // 计时起点（残留调试代码，未使用）
    model->detect(&image, &results1, true);  // 执行推理（RKNN NPU）

    // 释放 DMA 缓冲区（避免内存泄漏）——正常出口，与两处提前 return 对称
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


