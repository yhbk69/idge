#ifndef ROLL_CALL_SERVICE_H
#define ROLL_CALL_SERVICE_H

#include <string>
#include <vector>
#include <memory>
#include "../recognition/face_recognizer.h"
#include "../db/business_db_manager.h"
#include <opencv2/opencv.hpp>
#include "thread_pool.h"  // +++++ 引入线程池
// ProcessedFace 
struct ProcessedFace {
    cv::Rect rect;
    std::vector<float> feature;
    bool is_duplicate;
    int similar_to_index;
    int face_index;
    float score;  // 
};

// 
struct PhotoProcessResult {
    std::string original_path;
    std::string processed_path;
    std::vector<ProcessedFace> faces;
    int unique_count;
    std::vector<std::string> face_image_paths;
};

struct TaskProcessResult {
    int task_id;
    std::string task_folder;
    std::vector<PhotoProcessResult> photos;
    int total_unique_count;
    std::vector<std::string> face_image_paths;
};

struct CancellationMatch {
    int registration_record_id = 0;
    int cancellation_record_id = 0;
    std::string registration_image;
    std::string cancellation_image;
    float similarity = 0.0f;
    int status = 0; // match status
};

struct CancellationPhotoResult {
    std::string original_path;
    std::string processed_path;
};

struct CancellationProcessResult {
    std::vector<CancellationPhotoResult> photos;
    std::vector<CancellationMatch> matches;
};

class RollCallService {
public:
    RollCallService();
    ~RollCallService();
    
    // 初始化
    bool initialize(const std::string& exe_path,
                   const std::string& det_model_path,
                   const std::string& rec_model_path,
                   const std::string& db_path,
                   const std::string& base_storage_path);
    
    // 任务管理
    int createRegistrationTask(const std::string& task_name);
    bool deleteTask(int task_id);
    bool cancelTask(int task_id);
    Task getTaskInfo(int task_id);
    std::vector<Task> getAllTasks();

    //登记流程
    TaskProcessResult processPhotos(int task_id, const std::vector<std::string>& photo_paths);
    bool saveTaskResult(const TaskProcessResult& result);
    //注销流程
    CancellationProcessResult matchCancellation(
        int task_id, const std::vector<std::string>& photo_paths);
    bool confirmCancellation(
        int task_id, int cancelled_count,
        const CancellationProcessResult& result);
    
    
    
    BusinessDBManager* getDatabase() { return db_.get(); }
    const std::string& getStoragePath() const { return base_storage_path_; }
    const std::string& getDetectionModelPath() const { return detection_model_path_; }
    void setSimilarityThreshold(float threshold) { similarity_threshold_ = threshold; }
    
private:
    std::unique_ptr<FaceRecognitionWrapper> recognizer_;  
    std::unique_ptr<BusinessDBManager> db_;
    std::unique_ptr<ThreadPool> thread_pool_;  // +++++ 线程池
    std::string base_storage_path_;
    std::string detection_model_path_;
    float similarity_threshold_;
    
     // +++++ 修改：单张图片处理改为独立函数，可在线程池中调用
    PhotoProcessResult processSinglePhotoParallel(const std::string& photo_path,
                                                  const std::string& task_folder);
    
    // +++++ 新增：合并多张图片的检测结果，做去重
    TaskProcessResult mergeAndDeduplicateResults(
        int task_id,
        const std::string& task_folder,
        std::vector<PhotoProcessResult>& photo_results);
    
        
    std::string drawAndSaveProcessedImage(const cv::Mat& image,
                                         const std::vector<ProcessedFace>& faces,
                                         const std::string& task_folder,
                                         const std::string& original_filename);
    
    std::string saveDetectedFace(const cv::Mat& image,
                                 const cv::Rect& face_rect,
                                 const std::string& task_folder,
                                 int face_id,
                                 const std::string& name_prefix = "face");

    // +++++ 新增：矩阵版本的相似度查找
    int findSimilarFaceVectorized(const std::vector<float>& feature,
                                   const std::vector<std::vector<float>>& unique_features);

    // +++++ 新增：归一化辅助函数
    void normalizeFeature(std::vector<float>& feature);

};

#endif // ROLL_CALL_SERVICE_H
