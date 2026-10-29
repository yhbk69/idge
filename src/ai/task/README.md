# src/ai/task

> 所属域：**ai 推理与模型应用域**（目录重组 S4c 迁入）

## 功能概述

推理任务层：把"一帧图像 + 结果去向"封装成可跨线程传递的任务单元，并以独立线程运行 RKNN 推理任务（PpeTask），供 `src/media/reader` 解码流水线投递。另含占位的任务基类 `BaseTask`（未接线，仅作结构预留）。
（原先的无状态 `HelmetTask` 与空壳 `TaskPool` 全工程零调用，已随死代码清理删除。）

## 文件清单

| 文件 | 职责 |
|---|---|
| `base_task.h/.cpp` | 数据结构载体：`DetectResult`（大顶堆结果，旧形态）、`DetectContext`（流/帧 ID + 结果队列 + DMA 池）；`BaseTask` 为**占位基类**（init/run 声明无定义、非虚析构） |
| `ppe_task.hpp/.cpp` | PPE 检测推理任务：自有线程 + BlockingQueue（容量 2，push_latest）+ 独占 YOLO11Model；结果回填 time/id 后入结果队列 |
| `task_config.hpp` | `TaskConfig`：模型/标签路径 + `core_mask`（**位掩码**枚举）+ `result_id` 结果路由 ID |
| `task_data.h` | `TaskData`：跨线程任务单元（采集时间戳 + shared_ptr 图像 + shared_ptr 结果队列） |

## 核心类与数据流

```
解码线程（reader）                     PpeTask 推理线程（每任务一个）
  new TaskData(t, image, resultQueue)
  tasks_[k]->put(taskData)  ────────→  taskQueue_(容量2,满丢最旧) pop(200ms超时)
                                        model_->detect(image, &od_results)
                                        od_results.time = taskData->time  (epoch纳秒!)
                                        od_results.id   = config.result_id(多路复用路由)
              resultQueue  ←──────────  taskData->resultQueue.push(od_results)
  解码线程 tryPop 取最新结果 → 画框/告警

退出路径：requestStop(running_=false+队列close) → join 或 stopBestEffort
（超时 detach——曾 detach 的实例上层"只停不删"，防 UAF，见 ppe_task.hpp 注记）
```

## 使用方法

```cpp
#include "ppe_task.hpp"

TaskConfig cfg;
cfg.modelPath = "/app/models/yolo11_ppe.rknn";
cfg.labelPath = "/app/models/labels.txt";
cfg.core_mask = (rknn_core_mask)(RKNN_NPU_CORE_0 + (idx % 3)); // 三核轮转
cfg.result_id = 0;                                             // 预览多检测器区分槽位
PpeTask task(cfg);
task.start();                                   // 内部 init()+拉起推理线程

auto td = std::make_shared<TaskData>(
        now_epoch_ns, image, resultQueue);      // 队列随任务传递
task.put(td);                                   // start() 之前不可调用

task.stop();                                    // 阻塞退出（干净）
task.stopBestEffort(1500);                      // UI 友好：超时 detach（此后禁 delete）
```

## 依赖关系

- 依赖：`src/ai/yolo11`（YOLO11Model/common.hpp）、`src/base/queue`（PriorityQueue、BlockingQueue）、`src/base/buffer`（DmaBufferPool，帧由 reader 借出并归还）、OpenCV；
- 被依赖：`src/media/reader/ffmpeg_video_decoder`、`src/media/reader/camera_preview_decoder`（创建并驱动 PpeTask）。

## 注意事项

- **时间单位隐患**：`TaskData::time` → `od_results.time` 回填链携带的是 **epoch 纳秒**，与 `common.hpp` 毫秒文档、`src/biz/alarm` 纳秒限流常量三方不一致；本层只是"搬运"，勿在任务层顺手换算（详见 `src/ai/yolo11/common.hpp` ⚠ 注释）。
- `BaseTask` 是占位基类：调用其 init/run 即链接错误，禁止经基类指针多态删除；PpeTask 未继承它。
- `PpeTask` 生命周期：构造后未 start 即 put → 解引用空 taskQueue_；stopBestEffort detach 后析构 delete 队列存在在途线程 UAF 风险——上层策略是只停不删。
- `core_mask` 是位掩码（CORE_0=1、CORE_1=2、CORE_2=4），`+idx%3` 轮转依赖位值连续这一 rknn_api 细节，勿按序号理解。
- 本目录**没有全局任务调度器**：线程模型是“一个模型一个 PpeTask 线程”，由解码器按通道创建与销毁。
