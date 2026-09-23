#ifndef EQUIPMENT_INVENTORY_SERVICE_H
#define EQUIPMENT_INVENTORY_SERVICE_H

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <opencv2/opencv.hpp>

#include "../db/business_db_manager.h"
#include "../service/roll_call_service.h"

struct EquipmentDetection {
    int class_index = 0;
    float confidence = 0.0f;
    std::string label;
    cv::Rect rect;
};

struct EquipmentPhotoResult {
    std::string original_path;
    std::string processed_path;
    std::vector<EquipmentDetection> detections;
    std::map<std::string, int> counts;
    bool success = false;
    std::string error_message;
};

struct EquipmentTaskResult {
    int task_id = 0;
    int phase = 0; // 0: registration, 1: cancellation
    std::vector<EquipmentPhotoResult> photos;
    std::map<std::string, int> total_counts;
    bool success = false;
    std::string error_message;
};

struct EquipmentModelConfig {
    std::string executable_path;
    std::string model_path;
    std::string labels_path;
};

class EquipmentInventoryService {
public:
    explicit EquipmentInventoryService(std::shared_ptr<RollCallService> roll_call_service);

    bool initialize(const std::vector<EquipmentModelConfig>& models);
    bool initialize(const std::string& executable_path,
                    const std::string& model_path,
                    const std::string& labels_path);
    bool isReady() const {
        return ready_;
    }
    std::vector<EquipmentModelConfig> modelConfigs() const { return models_; }

    int createEquipmentTask(const std::string& task_name);
    std::vector<Task> getEquipmentTasks() const;
    Task getTaskInfo(int task_id) const;
    bool deleteTask(int task_id);

    EquipmentTaskResult processPhotos(int task_id,
                                      const std::vector<std::string>& photo_paths,
                                      int phase);
    bool saveResult(const EquipmentTaskResult& result);

    std::vector<EquipmentPhotoRecord> getPhotos(int task_id, int phase) const;
    std::vector<EquipmentDetectionRecord> getDetections(int photo_id) const;

private:
    bool runDetector(const std::string& image_path,
                     const std::string& result_path,
                     const EquipmentModelConfig& model,
                     int label_count,
                     std::vector<EquipmentDetection>& detections) const;
    bool parseResultFile(const std::string& result_path,
                         std::vector<EquipmentDetection>& detections) const;
    EquipmentPhotoResult processSinglePhoto(const std::string& photo_path,
                                            const std::string& task_folder) const;
    std::string drawAndSave(const EquipmentPhotoResult& result,
                            const std::string& task_folder) const;
    std::shared_ptr<RollCallService> roll_call_service_;
    std::string executable_path_;
    std::string model_path_;
    std::string labels_path_;
    int label_count_ = 0;
    std::vector<EquipmentModelConfig> models_;
    std::vector<int> model_label_counts_;
    bool ready_ = false;
};

#endif
