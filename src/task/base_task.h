#ifndef BASE_TASK_H
#define BASE_TASK_H

// ============================================================================
// base_task.h - 任务基类和数据结构定义
// ============================================================================
//
// 本文件定义了任务系统的基础数据结构和接口。
//
// 任务系统架构（现状，与文件尾 BaseTask 现状注记一致）：
//   - BaseTask: 任务基类——目前仅为占位声明，init/run 无定义、
//     析构非虚，尚未形成可用的抽象接口；
//   - PpeTask / HelmetTask: 实际推理任务，均**未继承** BaseTask，
//     只复用本文件定义的 DetectContext 等数据结构。
//
// 数据流：
//   解码线程 → TaskData(图像) → 任务线程 → DetectResult(结果) → 结果队列
//
// ============================================================================

#include "priority_queue.h"
#include "DmaBufferPool.h"

// ============================================================================
// DetectResult - 检测结果
// ============================================================================
// 存放一次目标检测的结果：
//   - time: 时间戳（用于排序，时间戳越大越新）
//   - result: 检测结果（当前无消费方填充，属早期接口遗留形态）
//
// 为什么需要 operator<？
//   PriorityQueue 使用 std::less 作为默认比较函数：
//   operator< 约定 "time 小者为'小'（旧）" ⇒ 堆顶恒为 time 最大（最新）元素；
//   队列满时新元素替换堆顶外的最小(最旧)元素，天然实现"只留最新 N 条"。
//
// 现状注记：主流水线实际使用的是 common.hpp 的 object_detect_result_list，
//   本结构体仅在 DetectContext 中引用、暂无生产读写点，属保留的旧结果形态。
// ============================================================================
struct DetectResult
{
    long time;                            // 时间戳
    std::vector<std::vector<float>> result;  // 检测结果

    // 定义比较操作符（用于 PriorityQueue 排序）
    // 返回 true 表示 this 比 other "更小"（time 更小 = 更旧 = 优先级更低）；
    // PriorityQueue 内部维护大顶堆，堆顶恒为"最大/最新"元素
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
//   模型实例**不在**本上下文中：HelmetTask 经 thread_local dpool::context
//   （ThreadPool.hpp）按 ID 取模型，属于"线程局部服务定位器"模式，
//   要求任务必须运行在 ThreadPool 工作线程上，否则取不到模型。
//
// 所有权约定：dmaBufferPool 为共享引用（不拥有缓冲区）；调用方把某个
//   DmaBuffer **借**给 run() 后，由被调方负责 release 归还（见 helmet_task.cpp
//   所有提前 return 路径均先 release 再返回）——这是本系统少数
//   "手工借还"资源的地方，新增 return 分支时务必保持归还。
// ============================================================================
struct DetectContext
{
    int streamId;           // 视频流 ID
    uint64_t frameId;       // 帧 ID
    std::shared_ptr<PriorityQueue<DetectResult>> detectResultQueue;  // 结果队列
    std::shared_ptr<DmaBufferPool>  dmaBufferPool;  // DMA 缓冲池


};


// ============================================================================
// BaseTask - 任务基类（现状：占位基类，尚未形成真正的抽象层）
// ============================================================================
// 审查注记（务必了解现状再用/扩展本类）：
//   - init()/run() 只是普通非虚成员函数声明，base_task.cpp 中仅实现了
//     构造/析构，init/run 无定义——一旦有代码调用即链接错误；
//   - 无纯虚函数、无虚析构（~BaseTask 非 virtual，经基类指针 delete
//     派生类是 UB，因此也不允许这样用）；
//   - 实际的推理任务 PpeTask、检测任务 HelmetTask 都**没有继承**本类
//     （仅 include 其头文件复用 DetectContext 等数据结构）；
//   - m_stream_id/m_type 未被任何代码读写。
//   结论：本类当前价值 = "任务系统公共数据结构与架构文档的载体"。若要
//   落地统一任务接口，需补 virtual ~BaseTask、纯虚 init/run 并让
//   PpeTask 等真正派生，属结构性改造，勿在无规划下随手派生。
// ============================================================================
class BaseTask
{
protected:
    int m_stream_id;   // 视频流 ID（预留，未使用）
    int m_type;        // 任务类型（0=并行, 1=串行；预留，未使用）

public:
    BaseTask(/* args */);
    void init();       // 声明占位：无实现，调用即链接错误（见上方现状注记）
    void run();        // 同上
    ~BaseTask();       // 非虚析构：本类不可作为多态删除的基类指针类型
};



#endif //BASE_TASK_H