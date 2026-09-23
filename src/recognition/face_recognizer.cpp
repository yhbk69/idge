/* 蔡超添加
调用exe 直接提取图片包含的人脸特征。 
*/
// ============================================================================
// face_recognizer.cpp - 实现要点（代码审查视角）
// ============================================================================
// 交互协议：与本类强耦合的是 exe 的**命令行位置约定**：
//   <exe> <det_model> <rec_model> <image> <out_json> <det_th> <nms_th>
// 以及 out_json 的 JSON 结构（"image"/"image_size"/"num_faces"/"faces"…）。
// 两侧任一格式变更都必须同步，否则表现为运行时静默错结果而非编译错误。
// 失败语义：所有失败路径（exe 非 0 退出、文件缺失、JSON 语法错误）统一
// 返回默认构造的空结果，调用方以 faces.empty() 判断，无错误码区分。
// ============================================================================
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

// 默认阈值：det 0.6 明显高于通用检测惯例（YOLO 侧 0.25）——点名/考勤场景
// 误检代价高（错认人），宁可漏检；NMS 0.4 与工程内其它检测器保持一致。
// 阈值只是透传给 exe 的命令行参数，本进程内不做任何过滤。
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

// ============================================================================
// detectAndExtract - 拉起外部 exe 并解析其 JSON 输出
// ============================================================================
// 审查注记：
//   - 命令为**空格分隔的位置参数拼接**，未做 shell 转义：路径含空格或
//     ;、&、$() 等元字符不仅会破坏解析，还构成命令注入面——输入路径
//     必须来自可信配置（当前调用方传入的是程序内部生成的抓拍文件路径）；
//   - 输出文件名 = image_path + "_faces.json"（临时文件约定由 exe 写出）：
//     1) 同一图片路径并发识别会互相覆写结果文件（读到的可能是另一路结果）；
//     2) unlink 被注释掉（见下），解析后临时 JSON 残留在磁盘，长期运行
//     会在抓拍目录累积小文件——清理责任事实上转给了上层目录管理；
//   - executeCommand 失败即返回空结果，不再尝试读 JSON（exe 未写或写了
//     旧内容均无法区分，属协议固有的幂等缺陷）。
// ============================================================================
FaceDetectionResult FaceRecognitionWrapper::detectAndExtract(const std::string& image_path) {
    FaceDetectionResult result;
    
    // 生成临时输出JSON文件名
    std::string output_json = image_path + "_faces.json";
    
    // 构建命令（7 个位置参数，顺序即与 exe 的接口契约）
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
    // 注：刻意注释保留——现场调试可查看 exe 原始输出；代价是文件残留（见上）
    // unlink(output_json.c_str());
    
    return result;
}

// ============================================================================
// executeCommand - system() 包装
// ============================================================================
// 返回值语义（易错点）：system() 返回的是 wait(2) 风格状态字，
// ret == 0 才表示"子进程正常退出且 exit code 为 0"；非 0 可能是
// 非零退出码（ret>>8）或被信号杀死（ret&0x7f），本函数不区分原因。
// 另两点约束：
//   - 同步阻塞：调用线程会挂起直到 exe 结束（识别一张脸通常数百 ms 级），
//     禁止在 UI/解码关键路径上直接调用；
//   - exe 内部使用 NPU，与本进程常驻的 RKNN 推理任务竞争核心算力，
//     高并发点名时主检测帧率会受影响（部署层面的隐含约束）。
// ============================================================================
bool FaceRecognitionWrapper::executeCommand(const std::string& command) {
    int ret = system(command.c_str());
    return (ret == 0);
}

// ============================================================================
// parseJsonResult - 解析 exe 输出的 JSON
// ============================================================================
// 审查注记：
//   - 仅 try 块包住"读文件语法解析"；随后的必需字段用 j["image"]、
//     j["num_faces"]、j["faces"] 直接 operator[] 取值——nlohmann 对
//     非 const 对象缺键会插入 null 再 get<T>() 抛 type_error，该异常
//     **不被本函数捕获**，会向调用方（detectAndExtract→上层业务）穿透。
//     即 exe 输出结构缺字段/类型不符时行为是"抛异常"而非"返回空结果"，
//     与文件头所述失败语义不完全一致——扩展时应先补 contains 防御；
//   - 可选字段（feature_dim/feature_normalized/raw_l2_norm）用
//     contains/is_number/value() 带默认值读取，向后兼容旧版 exe 输出；
//   - result.num_faces 取自 JSON 自报数，faces 逐条解析时非法 bbox 会被
//     剔除（见下 isfinite 过滤），因此可能 num_faces > faces.size()。
// ============================================================================
FaceDetectionResult FaceRecognitionWrapper::parseJsonResult(const std::string& json_path) {
    FaceDetectionResult result;
    
    // 检查文件是否存在
    // （exe 退出码 0 但没写文件也会走到这里返回空——协议幂等缺陷的体现）
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
        // 兼容两种序列化形态：[[x,y],[x,y],...] 嵌套数组 或 [x0,y0,x1,y1,...]
        // 扁平数组（不同版本 exe 的输出差异），逐对装配为 Point2f。
        // 约定 5 点顺序：左眼/右眼/鼻尖/左嘴角/右嘴角（SCRFD 惯例）。
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

// ============================================================================
// calculateSimilarity - 特征比对（点名判决的核心算子）
// ============================================================================
// 数学依据：L2 归一化后 |f|=1，点积 f1·f2 = cos(夹角)，值域[-1,1]；
// 因此无需再除范数，单遍 O(D) 循环（D=512）即完成余弦相似度计算。
// 前提约束：仅当两侧特征均满足 feature_normalized=true 时结果才是余弦；
// 若 exe 版本改变不再归一化，本函数退化为内积、上层比对阈值全部失效。
// 边界：维度不等/为空返回 0.0f——调用方注意 0 与"特征正交"不可区分，
// 应先自行校验 feature.size() 再调用。
// 注：raw_l2_norm 未参与本计算，仅供上层做特征质量门限参考。
// ============================================================================
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
