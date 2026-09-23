#ifndef BUSINESS_DB_MANAGER_H
#define BUSINESS_DB_MANAGER_H

#include <string>
#include <vector>
#include <cstdint>
#include <sqlite3.h>

struct Task {
    int id;
    std::string name;
    std::string type;  // "registration", "cancellation" or "equipment_registration"
    std::string create_time;
    std::string folder_path;
    int registered_count;
    int cancelled_count;
    int is_cancelled;  // 0: 未注销, 1: 已注销
    int parent_task_id;  // 注销任务关联的登记任务ID
};

struct FaceRecord {
    int id;
    int task_id;
    std::vector<float> feature;
    std::string original_photo_path;
    std::string processed_photo_path;
    std::string face_photo_path;
    int face_index;  // 该图中第几个人脸
    int is_duplicate;  // 0: 唯一, 1: 重复
    int similar_to_id;  // 重复时指向的原始人脸ID
};

struct CancellationPhotoRecord {
    int id = 0;
    int task_id = 0;
    std::string original_photo_path;
    std::string processed_photo_path;
};

struct CancellationMatchRecord {
    int id = 0;
    int task_id = 0;
    int registration_record_id = 0;
    std::string registration_image;
    std::string cancellation_image;
    float similarity = 0.0f;
    int status = 0;
};

struct EquipmentPhotoRecord {
    int id = 0;
    int task_id = 0;
    int phase = 0; // 0: registration, 1: cancellation
    std::string original_photo_path;
    std::string processed_photo_path;
};

struct EquipmentDetectionRecord {
    int id = 0;
    int photo_id = 0;
    int class_index = 0;
    std::string label;
    float confidence = 0.0f;
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
};

class BusinessDBManager {
public:
    BusinessDBManager();
    ~BusinessDBManager();
    
    bool open(const std::string& db_path);
    void close();
    
    // 任务操作
    int createTask(const std::string& name, const std::string& type, const std::string& folder_path);
    bool updateTask(int task_id, int registered_count, int cancelled_count, int is_cancelled);
    bool deleteTask(int task_id);
    Task getTask(int task_id);
    std::vector<Task> getAllTasks();
    std::vector<Task> getRegistrationTasks();  // 获取所有登记任务
    
    // 人脸记录操作
    int insertFaceRecord(const FaceRecord& record);
    bool deleteFaceRecordsByTask(int task_id);
    std::vector<FaceRecord> getFaceRecordsByTask(int task_id);
    std::vector<FaceRecord> getUniqueFacesByTask(int task_id);  // 获取去重后的人脸
    bool updateFaceRecord(int record_id, const FaceRecord& record);

    bool replaceCancellationData(
        int task_id,
        const std::vector<CancellationPhotoRecord>& photos,
        const std::vector<CancellationMatchRecord>& matches);
    bool deleteCancellationDataByTask(int task_id);
    std::vector<CancellationPhotoRecord> getCancellationPhotosByTask(int task_id);
    std::vector<CancellationMatchRecord> getCancellationMatchesByTask(int task_id);

    std::vector<Task> getTasksByType(const std::string& type);
    bool replaceEquipmentData(int task_id, int phase,
                              const std::vector<EquipmentPhotoRecord>& photos,
                              const std::vector<EquipmentDetectionRecord>& detections);
    std::vector<EquipmentPhotoRecord> getEquipmentPhotosByTask(int task_id, int phase);
    std::vector<EquipmentDetectionRecord> getEquipmentDetectionsByPhoto(int photo_id);
    bool deleteEquipmentDataByTask(int task_id);
private:
    sqlite3* db_;
    bool initialized_;
    
    bool createTables();
    std::vector<float> blobToFeature(const void* blob, int size);
    std::vector<uint8_t> featureToBlob(const std::vector<float>& feature);
};

#endif // BUSINESS_DB_MANAGER_H
