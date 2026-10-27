# src/base/queue

> 所属域：**base 基础设施域**（目录重组 S4a 迁入，依赖方向 base ← media/ai/biz/ui）

## 功能概述（在流水线中的位置）
各工作线程之间的帧/任务传递通道：解码线程 → 推理线程 → 编码/推流线程 → MQTT/告警线程。队列即背压阀门——容量选择直接决定丢帧策略与端到端延迟。

## 文件清单
| 文件 | 职责 |
|---|---|
| BlockingQueue.hpp | 通用有界阻塞队列：push/pop 支持阻塞/超时/非阻塞三态，push_latest 保最新，close 优雅收尾 |
| frame_queue.h | FrameQueue：专传 image_buffer_t 帧，满时可联动 DmaBufferPool 归还旧帧缓冲 |
| priority_queue.h | PriorityQueue：固定大小 Top-N 优先级堆（满时自动淘汰最低优先级） |
| latest_frame_slot.h | LatestFrameSlot：单槽"最新值"广播邮箱，按 generation 跳帧，永不排队/积压（**当前仅自检覆盖，业务链路未接入**） |

## 核心类与数据流
- `BlockingQueue<T>`：双条件变量（not_full_/not_empty_）分离生产/消费等待集；所有 wait 带谓词，杜绝虚假唤醒与丢通知。容量策略（来自工程注释）：enc_queue=1 背压、infer_queue=2 非阻塞丢帧、stream_queue=2 平滑抖动、mqtt_queue=16 隔离网络延迟。
- `push_latest`：满则 pop 队首（最旧）再入队——"保最新帧"语义，推理链路专用；与 `FrameQueue::pushAndReplace`（实现有误，见注意事项）形成对照。
- `FrameQueue`：生产者（采集/解码）阻塞入队，消费者（显示/编码）`wait_and_pop`；`shutdown()` 用 notify_all 唤醒双侧后按"关闭且队列空"退出。
- `PriorityQueue<T,Compare>`：vector 堆，push 满时替换最小者；tryPop/waitAndPop 只读堆顶不删除（top 语义）。
- `LatestFrameSlot<T>`：与上面三个的区别是**它不是队列**——槽里永远只有一帧，`publish()` 覆盖并递增 `generation_`，`waitNext(previous, running)` 按调用方自己的游标返回"比上次新的那一帧"，落后的消费者自动跳过中间帧。用于"只要最新画面、旧帧丢了才对"的下游（预览/取帧抓拍/推流），比 `push_latest` 更进一步：不但不积压，也不会把同一帧重复消费。

## 使用方法
```cpp
#include "BlockingQueue.hpp"
BlockingQueue<FramePtr> infer_q(2);
infer_q.close();                       // 停机：唤醒所有阻塞者
FramePtr f;
while (infer_q.pop(f, /*timeout_ms=*/200)) { /* 超时后检查 running_ */ }
infer_q.push_latest(frame);            // 满则丢最旧，保证处理最新画面

#include "priority_queue.h"
PriorityQueue<Result> top5(5);         // 只保留优先级最高的 5 条结果
Result r; if (top5.tryPop(r)) { /* 注意：未删除，消费需再 pop() */ }

#include "latest_frame_slot.h"
LatestFrameSlot<Frame> slot;           // 单槽，生产者覆盖、消费者各持自己的游标
slot.publish(std::make_shared<Frame>(f));      // 抢不到锁或已 close → 丢帧返回 false
uint64_t myCursor = 0;
std::atomic<bool> running{true};
auto frame = slot.waitNext(myCursor, running); // 只在新帧上返回；close/停 → 空指针
slot.close();                          // 唤醒所有等待者；reset() 后游标须一起归零
```

## 依赖关系
- frame_queue.h 依赖 `common.hpp`（image_buffer_t）与 `buffer/DmaBufferPool.h`（归还旧帧）；BlockingQueue/priority_queue 为纯 STL 模板，可独立复用。
- latest_frame_slot.h 同为纯 STL（mutex/condition_variable/atomic/memory），无 Qt 依赖；由 `tests/test_latest_frame_slot.cpp`（根 CMake 同名 target）自检，业务侧暂无接入点。
- 下游：推理/编码/推流/MQTT 线程主循环；上游：reader/解码器。

## 注意事项
- 帧元素是浅拷贝载体：队列里的 image_buffer_t/RenderFrame 与生产者共享底层 DMA 指针与 fd；所有权归池与 fd 签发协议，队列不管理资源。
- **pushAndReplace 隐患（上轮审查确认）**：FrameQueue 满时取的是 `back()`（最新帧）而非最旧帧、只 release 不 pop（队列可越过 max_size 增长）、被归还的帧仍留在队列中被消费者取出——悬垂/撕裂风险；注释已警示，勿依赖该接口。
- FrameQueue 默认构造 shutdown_ 未初始化且 max_size=0，push 可能永久阻塞；必须用带池构造。
- priority_queue **waitAndPop 出队不移除元素**：waitAndPop 反复返回同一堆顶，需成对手动 pop() 才等价消费（tryPop 已修复为 pop_heap+pop_back 取出即删除）；且 push 从不 notify 条件变量，waitAndPop 的阻塞唤醒链不完整，停机也无 shutdown 通道——仅适合与轮询 tryPop 搭配。
- BlockingQueue close 单向不可复用；notify_one 假设单消费者/单生产者，多消费者共享需重新评估。
- **LatestFrameSlot 的丢弃是设计而非缺陷**：`publish()` 用 `try_lock`，抢不到锁立刻返回 false 丢掉这帧，绝不为了"入队"而阻塞生产者——代价是 `closed_` 的读取必须依赖同一把锁，不能改成裸 atomic 标志位。另外**每个消费者必须各传自己的 `previous` 游标**：游标是"我看到过第几代"，共用一个变量时先返回的一方会把它推走，另一方就漏掉这一帧。
