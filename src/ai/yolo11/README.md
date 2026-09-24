# src/ai/yolo11

> 所属域：**ai 推理与模型应用域**（目录重组 S4c 迁入）

## 功能概述

YOLO11 目标检测的 RKNN NPU 推理与后处理实现（anchor-free + DFL）。覆盖：模型加载与零拷贝 io-mem 绑定、量化/反量化、3 分支（stride 8/16/32）输出解码（score_sum 粗筛 → 整数域 argmax → 存活格反量化 + DFL 期望框回归）、按类别 NMS、letterbox 坐标还原。

## 文件清单

| 文件 | 职责 |
|---|---|
| `common.hpp` | 公共类型：`image_buffer_t`、`object_detect_result(_list)`（含 ⚠ time 单位隐患警示）、rknn app 上下文、阈值/尺寸宏、`OBJ_NUMB_MAX_SIZE` 定长设计 |
| `yolo_base_detector.hpp` | 检测器基类：CPU 参考实现 `letterBox`/`scaleCoords`（与生产 RGA 路径公式互为对照）、`loadClassNames`、`PreprocessType` |
| `yolo11_model.hpp` | 核心实现：`init_yolo11_model`（UINT8 输入 + `rknn_create_mem/set_io_mem` 零拷贝）、`infer`、`post_process`、`process_i8/process_u8`、`compute_dfl`、`nms`/`quick_sort_indice_inverse`、量化四件套、`detect` |

## 核心类与数据流

```
YOLO11Model : YoloBaseDetector
  构造: loadClassNames + init_yolo11_model(.rknn, core_mask)   // 失败 throw
  detect(img, results, want_float)
    → infer(): 输入字节流直接写入 input_mems[0]（RGA 已产出 letterbox 640x640
       RGB888；UINT8 输入使归一化/量化在 NPU 片上融合）→ rknn_run
       → 输出取 outputs[i].buf（want_float=true 时驱动侧反量化为 float）
    → post_process(): 每层级 index∈{0=box,1=score,2=score_sum}
       score_sum 量化域粗筛 → 80 类整数域 argmax → 存活格 dequant + DFL
       单趟 softmax 期望 Σ(i·e^t_i)/Σe^t_i → 框 = (grid±dfl)·stride
       → clamp(640 空间) → /scale 还原原图坐标 → 按类别 nms → 截断 128
  复杂度: O(ΣG²×C)=8400 格×80 类粗筛 + 存活格精算 + NMS O(V²)/类
```

结果以 `object_detect_result_list` **值拷贝**返回；`id`/`time` 由调用方回填（见 `src/task/ppe_task.cpp`）。

## 使用方法

```cpp
#include "yolo11_model.hpp"

auto model = std::make_shared<YOLO11Model>(
        "/app/models/yolo11_ppe.rknn",   // 模型（.rknn, UINT8 输入）
        "/app/models/labels.txt",        // 每行一类名，行号=类别 id
        RKNN_NPU_CORE_1,                 // 位掩码核绑定；0=AUTO
        80, 16);                         // numClasses / dflLen（可默认）

image_buffer_t img{ ... };               // RGA letterbox 后的 640x640 RGB888
object_detect_result_list results;
model->detect(&img, &results, true);     // want_float=true：反量化浮点输出
for (int i = 0; i < results.count; ++i) {
    auto& r = results.results[i];
    // r.box.left/top/right/bottom 已还原为原图坐标
}
```

## 依赖关系

- 被依赖：`src/task/ppe_task`（主要消费者）、`src/model/ModelPool`（登记共享指针）、`src/reader`（间接）；
- 依赖：`rknn_api`（librknnrt）、`src/utils/image_utils.h`（`letterbox_t`）、OpenCV（基类参考实现）、`common.hpp` 类型。

## 注意事项

- **模型输入约定**：输入 attr 被强制为 UINT8 NHWC 640x640x3——`.rknn` 必须按"归一化在 NPU 内做/量化感知训练导出"生成，普通 mean/std 归一化浮点模型不适用。
- **time 单位隐患**：`object_detect_result_list::time` 文档写"毫秒"，生产实际回填 epoch **纳秒**（TaskData::time），而 `src/alarm/alarm_manager.h` 的 `kAlarmThrottleNs=2e9` 按纳秒比较——三方不一致，修改任一小时前先读 `common.hpp` 的 ⚠ 长注释。
- **输出内存所有权**：`infer` 已改用官方 `rknn_outputs_release` 归还 runtime 分配的输出缓冲。板端实测（librknnrt 2.3.2，2026-09-24）：旧写法 `free(outputs[i].buf)` 与 release 行为完全一致（buf 即 runtime malloc 堆指针，800 轮 RSS 平稳、destroy 正常）——历史上并非 bug，但 release 才是 API 契约。
- **线程纪律**：单实例 `detect` 非线程安全（共享 `app_ctx` 输出缓冲），一个 `YOLO11Model` 只允许一个线程串行调用；`ModelPool::getModel` 返回共享副本不等于并发推理许可。
- **泄漏登记**：类无析构函数，`release_yolo11_model`（`rknn_destroy`）无调用点，长驻进程模型加载即泄漏 NPU 上下文。
- 阈值以 `detect()` 形参默认值（conf 0.25 / nms 0.45）为准，宏仅为文档性默认。
