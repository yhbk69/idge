#ifndef BASE_TASK_H
#define BASE_TASK_H


#include "queue/priority_queue.h"
#include "DmaBufferPool.h"

struct DetectResult
{
    long time;
    std::vector<std::vector<float>> result;

    // Define operator< for priority_queue ordering
    // Returns true if this task has LOWER priority than other
    // (priority_queue puts the "largest" element at the top)
    bool operator<(const DetectResult &other) const
    {
        return time < other.time;
    }
};

struct DetectContext
{
    int streamId;
    uint64_t frameId;
    std::shared_ptr<PriorityQueue<DetectResult>> detectResultQueue;
    std::shared_ptr<DmaBufferPool>  dmaBufferPool;


};


class BaseTask
{
protected:
    /* data */
    int m_stream_id;

    //并行还是串行执行
    int m_type;

public:
    BaseTask(/* args */);
    void init();
    void run();
    ~BaseTask();
};



#endif //BASE_TASK_H