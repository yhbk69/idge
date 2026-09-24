#ifndef EQUIPMENT_INVENTORY_SERVICE_H
#define EQUIPMENT_INVENTORY_SERVICE_H

/*
 * 设备盘点服务：对批量照片运行 YOLO11(RKNN) 目标检测，统计各品类数量。
 * 检测在主进程内完成（YOLO11Model + model/library 下的库模型权重，
 * 与实时预览 PpeTask 同一推理链路）；模型实例在 initialize 时一次性
 * 加载并常驻，绑定约定 models[i]->NPU 核 i（核0/1，主流水线留核2）。
 * 任务与结果落库共用 RollCallService 持有的同一个 BusinessDBManager 连接，
 * 因此本服务以 shared_ptr<RollCallService> 为依赖注入而非独立建库。
 * phase 语义：0=进场盘点（registration），1=退场/复查（cancellation），
 * 同一任务两阶段的照片与检测明细分开存档。
 */

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <opencv2/opencv.hpp>

#include "business_db_manager.h"
#include "roll_call_service.h"

// 前置声明即可：完整头（含 rknn 依赖）只在 .cpp 引入；
// 成员析构因此移到 .cpp（unique_ptr 要求完整类型）
class YOLO11Model;

// 单个设备检出：class_index 为模型标签表行号，label 为标签名，rect 为原图像素坐标框
struct EquipmentDetection {
    int class_index = 0;
    float confidence = 0.0f;   // 置信度（模型侧已按阈值过滤，此处直接采信）
    std::string label;         // 类别名（来自 labels 文件，参与 counts 统计键）
    cv::Rect rect;             // x1,y1 起点 + 宽高（由 x2-x1/y2-y1 求得，负宽高已被钳 0）
};

// 单张照片的盘点结果：detections 汇总所有模型的输出，counts 按标签计数
struct EquipmentPhotoResult {
    std::string original_path;     // 复制进任务目录后的“归档原图”路径（非用户选择的原始路径）
    std::string processed_path;    // 画框回显图路径（失败为空 -> success=false）
    std::vector<EquipmentDetection> detections;
    std::map<std::string, int> counts;  // 标签 -> 数量（std::map 有序，UI 表格展示稳定）
    bool success = false;
    std::string error_message;     // 首个失败原因（中文，直接面向 UI 弹窗）
};

// 一个盘点任务某阶段的全量结果
struct EquipmentTaskResult {
    int task_id = 0;
    int phase = 0; // 0: registration, 1: cancellation
    std::vector<EquipmentPhotoResult> photos;
    std::map<std::string, int> total_counts;  // 各照片 counts 的按标签累加 = 阶段盘点总数
    bool success = false;                     // 全部照片成功才为 true（一票失败即 false）
    std::string error_message;                // 记录第一张失败照片的错误
};

// 盘点模型配置：rknn 权重 + 标签表（每行一个类别名）。
// executable_path 为外部 demo 时代的遗留字段，已废弃，恒可留空
struct EquipmentModelConfig {
    std::string executable_path;   // 废弃：保留仅为兼容既有构造点聚合初始化
    std::string model_path;
    std::string labels_path;
};
// 清单变更比较用（热更新时判断"模型库变了但盘点清单没变"）；废弃字段不参与
inline bool operator==(const EquipmentModelConfig& a, const EquipmentModelConfig& b) {
    return a.model_path == b.model_path && a.labels_path == b.labels_path;
}

class EquipmentInventoryService {
public:
    // roll_call_service 以 shared_ptr 注入：设备任务复用其 SQLite 连接与存储根目录；
    // 允许为空（未初始化点名服务时所有库操作静默失败返回默认值，不崩溃）
    explicit EquipmentInventoryService(std::shared_ptr<RollCallService> roll_call_service);
    // 定义在 .cpp：detectors_ 的 unique_ptr<YOLO11Model> 析构需要完整类型
    ~EquipmentInventoryService();

    // 多模型初始化（原子）：逐个校验标签可读非空、模型文件存在，并为每个模型构造
    // 常驻 YOLO11Model（加载权重到 NPU，首模型核0、其余核1）。任一项失败则直接
    // 返回 false 且**旧配置与旧模型实例保持不动**（热更新失败仍可继续用老模型）。
    // 标签行数即模型类别数 numClasses，决定输出解码宽度。
    bool initialize(const std::vector<EquipmentModelConfig>& models);
    // 单模型便捷重载：转调 vector 版本
    bool initialize(const std::string& model_path, const std::string& labels_path);
    bool isReady() const {
        return ready_;
    }
    std::vector<EquipmentModelConfig> modelConfigs() const { return models_; }

    // 任务生命周期：与点名任务同表（tasks），以 type="equipment_registration" 区分
    int createEquipmentTask(const std::string& task_name);  // 建夹+写库，库失败回滚删夹
    std::vector<Task> getEquipmentTasks() const;
    Task getTaskInfo(int task_id) const;
    bool deleteTask(int task_id);   // 直接委托 RollCallService（文件夹+库记录同删）

    // 盘点主流程：对 photo_paths 逐张（串行）检测计数；phase 决定结果归档到哪个阶段
    EquipmentTaskResult processPhotos(int task_id,
                                      const std::vector<std::string>& photo_paths,
                                      int phase);
    bool saveResult(const EquipmentTaskResult& result);  // 确认后落库并更新任务计数

    // 查询已落库的阶段明细（UI 历史任务回显）
    std::vector<EquipmentPhotoRecord> getPhotos(int task_id, int phase) const;
    std::vector<EquipmentDetectionRecord> getDetections(int photo_id) const;

private:
    // 进程内跑一次检测：read_image 解码 -> detectors_[model_index] 推理 ->
    // 转 EquipmentDetection（坐标已是原图像素空间）；解码/推理失败返回 false
    bool runDetector(const std::string& image_path,
                     size_t model_index,
                     std::vector<EquipmentDetection>& detections);
    EquipmentPhotoResult processSinglePhoto(const std::string& photo_path,
                                             const std::string& task_folder);
    // 解码原图(带Qt兜底)->画框->写 processed_equipment_*.png；返回空串视为该照失败
    std::string drawAndSave(const EquipmentPhotoResult& result,
                            const std::string& task_folder) const;
    std::shared_ptr<RollCallService> roll_call_service_;
    // 以下 4 个成员是 models_[0] 的便捷副本（历史单模型接口保留，逻辑以 models_ 为准）
    std::string executable_path_;   // 废弃：外部 demo 时代遗留
    std::string model_path_;
    std::string labels_path_;
    int label_count_ = 0;
    std::vector<EquipmentModelConfig> models_;          // 全部启用的检测模型（多品类各一）
    std::vector<int> model_label_counts_;               // 与 models_ 一一对应的类别数
    // 与 models_ 同序的常驻推理实例（initialize 构造、析构释放 NPU）；
    // 仅盘点工作线程串行使用（detect 对同实例非线程安全，单线程独占即可）
    std::vector<std::unique_ptr<YOLO11Model>> detectors_;
    bool ready_ = false;                                // initialize 全部校验通过才可跑
};

#endif
