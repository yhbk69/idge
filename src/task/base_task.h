#ifndef BASE_TASK_H
#define BASE_TASK_H

// ============================================================================
// base_task.h - 任务基类和数据结构定义
// ============================================================================
//
// 本文件定义了任务系统的基础数据结构和接口。
//
// 任务系统架构：
//   - BaseTask: 任务基类（抽象接口）
//   - PpeTask: 推理任务（继承 BaseTask）
//   - HelmetTask: 安全帽检测任务（继承 BaseTask）
//
// 数据流：
//   解码线程 → TaskData(图像) → 任务线程 → DetectResult(结果) → 结果队列
//
// ============================================================================

#include "queue/priority_queue.h"
#include "DmaBufferPool.h"

// ============================================================================
// DetectResult - 检测结果
// ============================================================================
// 存放一次目标检测的结果：
//   - time: 时间戳（用于排序，时间戳越大越新）
//   - result: 检测结果列表
//
// 为什么需要 operator<？
//   PriorityQueue 使用 std::less 作为默认比较函数
//   operator< 定义了"哪个更新"的语义：time 越大越新，优先级越高
//
// ============================================================================
struct DetectResult
{
    long time;                            // 时间戳
    std::vector<std::vector<float>> result;  // 检测结果

    // 定义比较操作符（用于 PriorityQueue 排序）
    // 返回 true 表示 this 的优先级低于 other（即 this 更新）
    // PriorityQueue 将"最大"元素放在堆顶
    bool operator<(const DetectResult &other) const
    {
        return time < other.time;
    }
};

// ============================================================================
// DetectContext - 检测上下文
// ============================================================================
// 在检测过程中传递的上下文信息：
//   - streamId: 视频流 ID（区分多个摄像头）
//   - frameId: 帧 ID（区分同一摄像头的不同帧）
//   - detectResultQueue: 结果队列（检测结果写入这里）
//   - dmaBufferPool: DMA 缓冲池（用于分配检测用的缓冲区）
//
// ============================================================================
struct DetectContext
{
    int streamId;           // 视频流 ID
    uint64_t frameId;       // 帧 ID
    std::shared_ptr<PriorityQueue<DetectResult>> detectResultQueue;  // 结果队列
    std::shared_ptr<DmaBufferPool>  dmaBufferPool;  // DMA 缓冲池


};


// ============================================================================
// BaseTask - 任务基类
// ============================================================================
// 所有任务的抽象基类，定义了任务的基本接口。
//
// 子类需要实现：
//   - init(): 初始化任务
//   - run(): 执行任务
//
// 成员变量：
//   - m_stream_id: 视频流 ID
//   - m_type: 任务类型（并行/串行）
//
// ============================================================================
class BaseTask
{
protected:
    int m_stream_id;   // 视频流 ID
    int m_type;        // 任务类型（0=并行, 1=串行）

public:
    BaseTask(/* args */);
    void init();
    void run();
    ~BaseTask();
};



#endif //BASE_TASK_H