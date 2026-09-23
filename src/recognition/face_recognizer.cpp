/* 蔡超添加
调用exe 直接提取图片包含的人脸特征。 
*/
#include "face_recognizer.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

// 使用第三方JSON库（推荐jsoncpp或nlohmann/json）
// 这里使用 nlohmann/json，需要包含 json.hpp
#include "nlohmann/json.hpp"
using json = nlohmann::json;

FaceRecognitionWrapper::FaceRecognitionWrapper()
    : det_threshold_(0.6f),
      nms_threshold_(0.4f) {
}

FaceRecognitionWrapper::~FaceRecognitionWrapper() = default;

void FaceRecognitionWrapper::setExecutablePath(const std::string& exe_path) {
    exe_path_ = exe_path;
}

void FaceRecognitionWrapper::setDetectionModel(const std::string& det_model) {
    det_model_ = det_model;
}

void FaceRecognitionWrapper::setRecognitionModel(const std::string& rec_model) {
    rec_model_ = rec_model;
}

void FaceRecognitionWrapper::setThresholds(float det_threshold, float nms_threshold) {
    det_threshold_ = det_threshold;
    nms_threshold_ = nms_threshold;
}

FaceDetectionResult FaceRecognitionWrapper::detectAndExtract(const std::string& image_path) {
    FaceDetectionResult result;
    
    // 生成临时输出JSON文件名
    std::string output_json = image_path + "_faces.json";
    
    // 构建命令
    std::ostringstream cmd;
    cmd << exe_path_ << " "
        << det_model_ << " "
        << rec_model_ << " "
        << image_path << " "
        << output_json << " "
        << det_threshold_ << " "
        << nms_threshold_;
    
    std::string command = cmd.str();
    std::cout << "Executing: " << command << std::endl;
    
    // 执行命令
    if (!executeCommand(command)) {
        std::cerr << "Failed to execute face detection command" << std::endl;
        return result;
    }
    
    // 解析JSON结果
    result = parseJsonResult(output_json);
    
    // 删除临时JSON文件（可选）
    // unlink(output_json.c_str());
    
    return result;
}

bool FaceRecognitionWrapper::executeCommand(const std::string& command) {
    int ret = system(command.c_str());
    return (ret == 0);
}

FaceDetectionResult FaceRecognitionWrapper::parseJsonResult(const std::string& json_path) {
    FaceDetectionResult result;
    
    // 检查文件是否存在
    struct stat buffer;
    if (stat(json_path.c_str(), &buffer) != 0) {
        std::cerr << "JSON result file not found: " << json_path << std::endl;
        return result;
    }
    
    // 读取JSON文件
    std::ifstream file(json_path);
    if (!file.is_open()) {
        std::cerr << "Failed to open JSON file: " << json_path << std::endl;
        return result;
    }
    
    json j;
    try {
        file >> j;
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse JSON: " << e.what() << std::endl;
        return result;
    }
    
    // 解析基本信息
    result.image_path = j["image"].get<std::string>();
    auto img_size = j["image_size"];
    result.image_size = cv::Size(img_size[0].get<int>(), img_size[1].get<int>());
    result.feature_dim = j.contains("feature_dim") && j["feature_dim"].is_number()
        ? j["feature_dim"].get<int>() : 0;
    result.feature_normalized = j.contains("feature_normalized") && j["feature_normalized"].is_boolean()
        ? j["feature_normalized"].get<bool>() : false;
    result.num_faces = j["num_faces"].get<int>();
    
    // 解析每个人脸
    for (const auto& face_json : j["faces"]) {
        DetectedFace face;
        
        face.face_id = face_json["face_id"].get<int>();
        face.score = face_json["score"].get<float>();
        
        // Accept both legacy bbox array and the current x1/y1/x2/y2 fields.
        float x1, y1, x2, y2;
        if (face_json.contains("bbox") && face_json["bbox"].is_array()) {
            const auto& bbox = face_json["bbox"];
            x1 = bbox[0].get<float>(); y1 = bbox[1].get<float>();
            x2 = bbox[2].get<float>(); y2 = bbox[3].get<float>();
        } else {
            x1 = face_json.value("x1", 0.0f); y1 = face_json.value("y1", 0.0f);
            x2 = face_json.value("x2", x1); y2 = face_json.value("y2", y1);
        }
        // Reject malformed boxes before constructing cv::Rect.  A negative
        // width/height can trigger assertions or native crashes downstream.
        if (!std::isfinite(x1) || !std::isfinite(y1) ||
            !std::isfinite(x2) || !std::isfinite(y2) || x2 <= x1 || y2 <= y1) {
            std::cerr << "Skipping invalid face bbox" << std::endl;
            continue;
        }
        face.bbox = cv::Rect(static_cast<int>(x1), static_cast<int>(y1),
                             static_cast<int>(x2 - x1), static_cast<int>(y2 - y1));
        
        // 解析landmarks
        face.landmarks.clear();
        if (face_json.contains("landmarks") && face_json["landmarks"].is_array()) {
            const auto& landmarks = face_json["landmarks"];
            if (!landmarks.empty() && landmarks[0].is_array()) {
                for (const auto& lm : landmarks)
                    face.landmarks.push_back(cv::Point2f(lm[0].get<float>(), lm[1].get<float>()));
            } else {
                for (size_t i = 0; i + 1 < landmarks.size(); i += 2)
                    face.landmarks.push_back(cv::Point2f(landmarks[i].get<float>(), landmarks[i + 1].get<float>()));
            }
        }
        
        // 解析raw_l2_norm
        face.raw_l2_norm = face_json.value("raw_l2_norm", face_json.value("l2_norm", 0.0f));
        
        // 解析feature
        face.feature.clear();
        if (face_json.contains("feature") && face_json["feature"].is_array()) {
            for (const auto& val : face_json["feature"]) {
                if (val.is_number()) face.feature.push_back(val.get<float>());
            }
        }
        
        result.faces.push_back(face);
    }
    
    std::cout << "Parsed " << result.num_faces << " faces from " << json_path << std::endl;
    
    return result;
}

float FaceRecognitionWrapper::calculateSimilarity(const std::vector<float>& feat1, const std::vector<float>& feat2) {
    if (feat1.size() != feat2.size() || feat1.empty()) {
        return 0.0f;
    }
    
    // 计算余弦相似度（特征已归一化，直接点积）
    float dot_product = 0.0f;
    for (size_t i = 0; i < feat1.size(); ++i) {
        dot_product += feat1[i] * feat2[i];
    }
    
    return dot_product;
}
