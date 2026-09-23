#ifndef FACE_RECOGNITION_WRAPPER_H
#define FACE_RECOGNITION_WRAPPER_H

#include <string>
#include <vector>
#include <opencv2/opencv.hpp>

// 人脸信息结构
struct DetectedFace {
    int face_id;
    float score;
    cv::Rect bbox;  // x1, y1, width, height
    std::vector<cv::Point2f> landmarks;  // 5个关键点
    float raw_l2_norm;
    std::vector<float> feature;  // 512维已归一化特征
};

// 检测结果
struct FaceDetectionResult {
    std::string image_path;
    cv::Size image_size;
    int feature_dim;
    bool feature_normalized;
    int num_faces;
    std::vector<DetectedFace> faces;
};

class FaceRecognitionWrapper {
public:
    FaceRecognitionWrapper();
    ~FaceRecognitionWrapper();
    
    // 设置exe和模型路径
    void setExecutablePath(const std::string& exe_path);
    void setDetectionModel(const std::string& det_model);
    void setRecognitionModel(const std::string& rec_model);
    void setThresholds(float det_threshold, float nms_threshold);
    
    // 检测并提取特征
    FaceDetectionResult detectAndExtract(const std::string& image_path);
    
    // 计算两个特征向量的余弦相似度
    static float calculateSimilarity(const std::vector<float>& feat1, const std::vector<float>& feat2);
    
private:
    std::string exe_path_;
    std::string det_model_;
    std::string rec_model_;
    float det_threshold_;
    float nms_threshold_;
    
    // 解析JSON结果
    FaceDetectionResult parseJsonResult(const std::string& json_path);
    
    // 执行外部命令
    bool executeCommand(const std::string& command);
};

#endif // FACE_RECOGNITION_WRAPPER_H