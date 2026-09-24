# src/biz/service

> 所属域：**biz 业务域**（目录重组 S4d 迁入）

## 功能概述

本目录实现 IDGE 系统两大业务服务：**人员点名（人脸识别注册/注销）服务** 与 **设备盘点（YOLO11 目标检测）服务**，以及点名服务使用的**固定大小线程池**。两个服务共享同一个 SQLite 注册库（通过 `BusinessDBManager` 注入），任务记录登记在同一张 `tasks` 表中，用 `type` 字段区分（点名/注册任务为 `registration`，盘点任务为 `equipment_registration`；注销不是新任务，而是对既有注册任务的比对与标记）。推理方式两条链路不同：点名的人脸检测+识别通过**子进程调用外部 `face_recognition` 可执行程序**完成；设备盘点的 YOLO11 检测为**进程内推理**（`src/ai/yolo11/YOLO11Model` + `model/library/` 库模型权重，与实时预览同一链路）。本目录代码负责任务编排、结果解析、去重匹配与落库。

## 文件清单

| 文件 | 职责 |
| --- | --- |
| `roll_call_service.h/.cpp` | 点名服务：人脸注册、点名（重复检测）、注销三大流程；调用外部 `face_recognition` 程序做 SCRFD 检测 + 512 维特征提取；贪心一对一余弦相似度匹配 |
| `equipment_inventory_service.h/.cpp` | 设备盘点服务：多模型（如 coco + 明火）进程内 YOLO11Model 检测、检出转换与计数、检测框绘制回写、按 phase 登记/注销落库 |
| `thread_pool.h/.cpp` | 固定线程数工作池（默认 4 线程），用于并行处理多张照片的"检测+特征提取"子进程调用；独立于全局 `src/base/threadpool` |

## 核心类与流程

### RollCallService —— 点名三阶段调用链

```
initialize(exe, det_rknn, rec_rknn, db, storage)
   │  创建工作区目录(0755)、打开 SQLite、设置检测阈值(置信度0.6/NMS 0.4)
   ├─ 注册: createRegistrationTask(name)          // 建任务文件夹 + tasks 表插入，库失败回滚删夹
   │        processPhotos(task_id, photos)        // 阶段1 并行检测(线程池) → 阶段2 序列去重匹配 → 阶段3 返回待确认结果
   │        saveTaskResult(result)                // 用户确认后写 face_records 表(删旧插新，幂等覆盖)
   ├─ 点名: 同注册流程，匹配到已注册人脸即标记"重复"
   └─ 注销: matchCancellation(task_id, photos)   // 只比对不落库：与注册库贪心匹配，status=1匹配/0未注销/2未登记
            confirmCancellation(...)             // 用户确认后落库：覆盖式写 cancellation_matches，任务置 is_cancelled=1 + cancelled_count

内部匹配核心: findSimilarFaceVectorized / matchCancellation
   特征向量 → normalizeFeature(L2归一化) → 点积即余弦相似度
   贪心一对一: 逐张人脸取注册库中分数最高者(阈值0.8起扫)，used[] 占用列保证一人只配一次
```

关键点：`processPhotos` 采用**两阶段流水线**——每张照片的"检测+特征提取"（外部子进程，CPU/NPU 密集）用线程池并行；去重与匹配（纯内存向量运算）串行执行。外部程序输出文件路径为 `<图片路径>_faces.json`，按图片天然隔离，故并行安全（但同一图片路径不可重复提交）。

### EquipmentInventoryService —— 盘点流程

```
initialize(std::vector<EquipmentModelConfig>)     // 每模型 = {rknn权重, 标签列表txt}；校验通过后构造常驻 YOLO11Model（models[i]→NPU 核 i，核0/1）
createEquipmentTask(name)                         // 复用 RollCallService 的 BusinessDBManager
processPhotos(task_id, photos, phase)             // phase: 0=登记 1=注销
   └─ 每照片×每模型: runDetector → read_image 解码 + 进程内 detector.detect()（坐标已是原图像素空间）
        按标签计数 + drawAndSave(检测框+标签绘制后另存)   // 任一模型失败则该照片整体失败(保守策略)
saveResult(result)                                // 确认后落库：photo_id 数组下标 → DB rowid 重映射
```

`initialize` 为每个模型一次性加载权重到 NPU 并常驻（避免每张照片重复 init 的秒级开销）；构造失败抛 `std::runtime_error`，被捕获后回收已加载实例、整体保持未就绪。模型清单由 `frmMain::initEquipmentService` 经 `ModelRegistry` 解析（当前主力 `yolo11n-coco`；明火模型入库后自动加入）。

### 线程模型

`ThreadPool(num_threads=4)`：RK3588 上 4 路并发子进程推理基本打满 NPU/大核，更多线程只增开销。语义要点：

- 无界任务队列；`shutdown()` 后再 `submit` 抛 `std::runtime_error`；
- 析构函数**排空**队列（不丢任务）后再停线程；
- `wait()` 严禁在 worker 线程内调用（自死锁）；
- 服务成员声明顺序保证安全析构：`thread_pool_` 在 `recognizer_`/`db_` **之后**声明 → **先**析构 → worker 全部 join 后依赖对象才释放，杜绝悬垂 `this`。

## 使用方法

```cpp
// 1. 点名服务初始化（工作区约定：模型放 model/face/，数据放 roll_call_data/）
auto rollCallService_ = std::make_shared<RollCallService>();
rollCallService_->initialize(
    ws + "/model/face/face_recognition",   // 外部推理可执行程序
    ws + "/model/face/detection.rknn",     // SCRFD 人脸检测模型
    ws + "/model/face/recognition.rknn",   // ArcFace 风格识别模型(512维)
    ws + "/roll_call_data/roll_call.db",   // SQLite 注册库
    ws + "/roll_call_data");               // 任务产物根目录

// 2. 注册/点名
int task_id = rollCallService_->createRegistrationTask("第一批入场");
auto result = rollCallService_->processPhotos(task_id, photoPaths);  // UI 展示确认页
rollCallService_->saveTaskResult(result);                            // 用户确认后落库

// 3. 注销
auto cancelResult = rollCallService_->matchCancellation(task_id, shotPaths);
int cancelledCount = /* 用户在确认页微调的注销人数 */;   // 见 cancellation_result_dialog
rollCallService_->confirmCancellation(task_id, cancelledCount, cancelResult);

// 4. 设备盘点（共享同一 db 连接，经 rollCallService_ 注入；路径来自模型库 ModelRegistry）
auto equipmentService_ = std::make_shared<EquipmentInventoryService>(rollCallService_);
equipmentService_->initialize({
    {ws+"/model/library/yolo11n-coco/model.rknn",  ws+"/model/library/yolo11n-coco/labels.txt"},   // 明火等后续模型按同格式追加
});
auto inv = equipmentService_->processPhotos(equipmentService_->createEquipmentTask("塔吊核查"), photos, /*phase=*/0);
equipmentService_->saveResult(inv);
```

界面层参考 `src/ui/form/frmmain.cpp` 中的服务初始化与 `EquipmentInventoryWidget`/点名窗体的调用。

## 依赖关系

- **推理链路（两种）**：点名调外部 `face_recognition` 可执行程序（子进程，SCRFD 检测 + 512 维识别）；盘点直接链接 `src/ai/yolo11/YOLO11Model` 进程内推理，权重 `.rknn` 由本进程加载到 NPU；
- **BusinessDBManager**（`src/biz/db`）：SQLite 注册库唯一入口，点名与盘点共用同一连接（`getDatabase()` 暴露）；
- **Qt5 Core**：QProcess（点名子进程）、QImage（绘制回写，绕开板端 cv::imwrite JPEG 编码器崩溃问题）；
- **OpenCV**：图片读取、人脸裁剪与画框；
- **std::vector 手写点积匹配**：当前余弦相似度匹配为逐对手工计算（贪心内按需算分），**未使用 Eigen 矩阵库**——注册库规模小（百人级），O(R·C·d) 足够，且避免引入矩阵构建与内存拷贝开销；
- **src/base/utils**：`task_manager`（任务目录/唯一文件名）、`draw_utils`（绿实线/黄虚线框）、`qt_image_utils`（安全解码）。

## 注意事项

1. **阈值调优**：相似度阈值默认 0.8（`setSimilarityThreshold` 可调）。调大→合并更少（漏判重复），调小→合并更多（误认他人）。同一识别模型不同批次光照/角度下分布漂移明显，建议先在目标场景用已知同/异人对校准。检测端固定 0.6 置信度 / 0.4 NMS IoU。
2. **注册库与识别模型必须配套**：特征维度和语义由 recognition.rknn 决定（代码按 512 维描述）。更换识别模型后旧注册特征全部失效，且特征长度不一致会导致匹配越界/截断（见 `findSimilarFaceVectorized` 注释），必须清库重录。
3. **贪心匹配局限**：逐行取最大 + `used[]` 占用，属近似最优而非匈牙利算法的全局最优；匹配结果对注册库顺序（注册先后）敏感，"先到先得"符合业务语义，且有人工确认页兜底，故未上 KM。极端相似人群（如双胞胎、同角度证件照）可能出现次优配对。
4. **并发约束**：两个服务均为"一流程一批照片"设计——`processPhotos` 内部自带线程池并行，**外层不要再并发驱动同一服务实例**（匹配与落库阶段为串行共享状态）。同一图片路径不要在一次批量中重复出现（`_faces.json` 输出文件会互相覆盖）。
5. **进程风险差异**：盘点为进程内推理，单张 detect 内部 RKNN 调用若挂死会卡住整个应用（不再是子进程级隔离；模型加载只发生在 `initialize`——启动期与模型库热更新时，运行期风险集中在 rknn_run）；点名侧外部程序启动失败按单照片失败处理，不阻断整批。
6. **磁盘约定**：任务 = `roll_call_data/<任务名>/` 文件夹 + `tasks` 表行，删除任务先删文件夹后删库；注销为软删除（置标记），数据可追溯。
7. **盘点模型热更新**：`EquipmentInventoryService::initialize` 为原子操作——新清单在临时容器上全部加载成功才 swap 生效，任一模型失败则成员状态与 `ready_` 完全不动（旧模型继续工作）。`frmMain` 在模型导入 / zip 导入 / 删除 / 手动刷新四处挂点调用 `reloadEquipmentService()`（清单未变且已就绪则跳过），明火等模型入库后免重启生效。并发安全依据：盘点选图/识别对话框均为模态 `exec()`，批处理运行期无法操作模型管理页；预览 `PpeTask` 持独立模型实例不受影响。离线验证见 `tests/test_equipment.cpp` 热更新原子性用例。
