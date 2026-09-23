# src/model

## 功能概述

推理模型注册表。`ModelPool` 以字符串 ID 登记 `shared_ptr<YOLO11Model>` 实例，供无状态任务（如 HelmetTask 所在链路）按 ID 取用模型。**现状为只读查表容器**——早期"借出-归还"池化设计已退化（详见文件头注释），真正的多核并行调度由 `src/threadpool/ThreadPool.hpp`（`ExecuteContext` + `dpool::context`）承担。

## 文件清单

| 文件 | 职责 |
|---|---|
| `ModelPool.hpp` | 唯一文件：`m_models` 映射表 + `addModel`/`getModel`/`getClassNames` 三个接口；头文件内含"借还语义现状澄清"长注释 |

## 核心类与数据流

```
启动期:  上层构造 YOLO11Model → ModelPool::addModel("1", model)   // 同 ID 覆盖
运行期:  任务线程 → getModel("1") → shared_ptr 副本（非独占借用，无需归还）
             → model->detect(...)   // 并发约束靠调用纪律，而非池
取用链路注记: HelmetTask 实际经 thread_local dpool::context->getModel(id)
取模型（ThreadPool 侧同名接口），本类为并行的另一注册表路径，二者独立。
```

## 使用方法

```cpp
#include "ModelPool.hpp"

ModelPool pool;
pool.addModel("1", std::make_shared<YOLO11Model>(
        "/app/models/yolo11.rknn", "/app/models/labels.txt", RKNN_NPU_CORE_0));

std::shared_ptr<YOLO11Model> m = pool.getModel("1");  // 未命中返回 nullptr
if (m) {
    const std::vector<std::string> names = pool.getClassNames("1"); // 值拷贝
}
```

## 依赖关系

- 依赖：`src/yolo11/yolo11_model.hpp`（被管理的模型类型）；
- 被依赖：历史上由任务层使用；当前生产取模路径是 `ThreadPool.hpp` 的 `ExecuteContext`/`dpool::context`，本类保留作备用注册表。

## 注意事项

- **不是池**：`getModel` 返回共享指针副本，无独占借出/归还/排队/容量语义；`<queue>` 为旧设计遗留 include。多任务共用同一模型实例时，串行化约束在调用方（YOLO11Model::detect 非线程安全）。
- `m_models` 无锁：约定"启动期 addModel、运行期只读"，运行期并发写是数据竞争；`getModel`/`getClassNames` 均已判空/判存（未命中分别返回 nullptr / 空表），无隐式插入。
- 析构不等待任何在途推理，销毁池前须确保无任务仍持有模型副本。
