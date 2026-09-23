# src/threadpool

## 功能概述（在流水线中的位置）
多路视频并发推理的执行引擎。每个工作线程持有独立的 RKNN 模型上下文（线程局部 ExecuteContext），把"一帧送 NPU 跑 YOLO11"的任务从各通道解码线程中卸载出来，实现通道级并行 + 动态扩缩容。

## 文件清单
| 文件 | 职责 |
|---|---|
| ThreadPool.hpp | ExecuteContext（每线程多模型实例、NPU 核循环绑定）+ dpool::ThreadPool（单例、惰性建线程、空闲 2s 回收） |

## 核心类与数据流
- `submit(func, args...)`：bind 打包 → `packaged_task`（shared_ptr 承载）→ 无界 FIFO 任务队列 → 有空闲线程 notify_one，否则未达上限就新建线程；返回 `std::future`。任务 Lambda 捕获的 `shared_ptr<DmaBuffer>` 在任务销毁时自动归还缓冲池（依赖 buffer 模块删除器）。
- `worker()`：首次运行懒加载 ExecuteContext（逐模型绑 `NPU_CORES[i%5]`，核 0→1→2→0→1 轮转）；循环体在锁内 `++idleThreads_ → wait_for(2s, quit||有任务) → --idleThreads_ → 取任务`，锁外执行任务；超时线程把自身 id 交给 `joinFinishedThreads()` 由同伴 join+erase。
- 单例 `getInstance()` 默认线程数 = `hardware_concurrency()`。

## 使用方法
```cpp
#include "ThreadPool.hpp"
auto& pool = dpool::ThreadPool::getInstance();
pool.setMaxThreads(6);                 // 仅允许在首个 submit 前调用
pool.setModelConfigs({{"model/person.rknn","model/person_labels.txt","行人"}});
auto fut = pool.submit([&buf]{
    auto r = ctx_model->infer(buf->va, buf->width_stride);  // buf 随 Lambda 结束自动归还
    return r;
});
// fut.get() 等待结果；注意勿在任务内再 submit（自死锁风险）
```

## 依赖关系
- 上游：各通道解码/抽帧线程（投推理任务）；下游：检测结果回传 UI/告警链路。
- 依赖 `yolo11_model.hpp`（RKNN 推理）、`rknn_api.h`（core mask）、`SharedTypes.hpp`（AppConfig）。

## 注意事项
- **无背压**：tasks_ 无界，生产快于 NPU 吞吐时队列与其中捕获的帧缓冲无限增长——上游必须限流。
- 懒加载代价：线程首任务承担模型加载（百 ms~s 级），预热可消除首帧尖刺；但每线程各持一份模型权重，线程数 × 模型数的内存成本必须预算。
- `setMaxThreads` 不加锁（data race），仅限启动期调用。
- 停机契约：quit_ 后队列中未执行任务被丢弃，其 `future` 永不 ready——阻塞等待 get() 的调用方会挂死；submit 在 quit 后触发 assert。
- 线程退出靠"自报 + 他人代为 join"，最后一个超时线程的 join 顺延到析构统一处理；析构对全部记录线程 join，任务持有池引用过久会拖慢退出。
- 同核多模型为时分复用，NPU_CORES 轮转不保证真并行。
