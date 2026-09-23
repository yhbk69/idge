#ifndef EQUIPMENT_INVENTORY_SERVICE_H
#define EQUIPMENT_INVENTORY_SERVICE_H

/*
 * 设备盘点服务：对批量照片运行 YOLO11(RKNN) 目标检测，统计各品类数量。
 * 检测不在本进程内做，而是复用外部 rknn_yolo11_demo 可执行程序（子进程，
 * 参数：模型 图片 结果txt 类别数），逐行解析其输出的 JSON-like 检测结果；
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

#include "../db/business_db_manager.h"
#include "../service/roll_call_service.h"

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

// 外部检测程序的三件套配置：宿主 exe + rknn 模型 + 标签表（每行一个类别名）
struct EquipmentModelConfig {
    std::string executable_path;
    std::string model_path;
    std::string labels_path;
};

class EquipmentInventoryService {
public:
    // roll_call_service 以 shared_ptr 注入：设备任务复用其 SQLite 连接与存储根目录；
    // 允许为空（未初始化点名服务时所有库操作静默失败返回默认值，不崩溃）
    explicit EquipmentInventoryService(std::shared_ptr<RollCallService> roll_call_service);

    // 多模型初始化：逐个校验标签文件可读非空、exe 与模型文件存在，任一失败则整体不就绪。
    // 单标签行计数即模型类别数 label_count，作为参数传给外部检测程序（决定输出解码宽度）
    bool initialize(const std::vector<EquipmentModelConfig>& models);
    // 单模型便捷重载：转调 vector 版本
    bool initialize(const std::string& executable_path,
                    const std::string& model_path,
                    const std::string& labels_path);
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
    // 以子进程方式跑一次检测：参数约定 <model> <image> <result.txt> <label_count>，
    // 结果写文本文件回传（避免解析 stdout 噪声）；超时/非零退出/异常退出都算失败
    bool runDetector(const std::string& image_path,
                     const std::string& result_path,
                     const EquipmentModelConfig& model,
                     int label_count,
                     std::vector<EquipmentDetection>& detections) const;
    // 解析结果文件：每行一条 {index,confidence,label,x1,y1,x2,y2}，不匹配的行静默跳过
    bool parseResultFile(const std::string& result_path,
                         std::vector<EquipmentDetection>& detections) const;
    EquipmentPhotoResult processSinglePhoto(const std::string& photo_path,
                                             const std::string& task_folder) const;
    // 解码原图(带Qt兜底)->画框->写 processed_equipment_*.png；返回空串视为该照失败
    std::string drawAndSave(const EquipmentPhotoResult& result,
                            const std::string& task_folder) const;
    std::shared_ptr<RollCallService> roll_call_service_;
    // 以下 4 个成员是 models_[0] 的便捷副本（历史单模型接口保留，逻辑以 models_ 为准）
    std::string executable_path_;
    std::string model_path_;
    std::string labels_path_;
    int label_count_ = 0;
    std::vector<EquipmentModelConfig> models_;          // 全部启用的检测模型（多品类各一）
    std::vector<int> model_label_counts_;               // 与 models_ 一一对应的类别数
    bool ready_ = false;                                // initialize 全部校验通过才可跑
};

#endif
