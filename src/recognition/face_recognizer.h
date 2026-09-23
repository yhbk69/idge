#ifndef FACE_RECOGNITION_WRAPPER_H
#define FACE_RECOGNITION_WRAPPER_H

// ============================================================================
// face_recognizer.h - 人脸识别外部工具的进程级包装
// ============================================================================
//
// 职责：
//   本类不做任何推理，只是"外部识别 exe + JSON 文件 IPC"的薄包装：
//   detectAndExtract() 拼命令行 → system() 拉起子进程（exe 内部完成
//   人脸检测/对齐/512维特征提取，RKNN 工具链独立开发）→ 解析 exe 写出的
//   JSON 文件为结构化结果。选择在子进程里跑而非 C++ 内嵌 RKNN，是为了
//   复用一个已独立验证过的识别程序（含检测+识别两级模型），避免与主
//   检测流水线共享进程内 NPU 上下文/线程约束，代价是每次调用有进程
//   创建与文件 IO 开销（同步阻塞）。
//
// 线程/并发警示：
//   - detectAndExtract() 内部 system() 阻塞直到底层命令结束，同一实例
//     不保证线程安全（成员只是配置，无锁）；
//   - 输出 JSON 文件名由 image_path 派生（见 .cpp），**并发识别同一张
//     图片路径会互相覆写**结果文件；调用方须自行错峰或保证路径唯一。
// ============================================================================
#include <string>
#include <vector>
#include <opencv2/opencv.hpp>

// 人脸信息结构
// 单个检测到的人脸的完整描述（exe 输出 JSON "faces" 数组一项的映射）
struct DetectedFace {
    int face_id;                    // exe 分配的人脸序号（同一图内唯一，非跨图身份ID）
    float score;                    // 检测置信度（> det_threshold 才会出现在结果里）
    cv::Rect bbox;                  // x1, y1, width, height
                                    // 像素坐标、原图坐标系（exe 侧已完成 letterbox 逆变换）
    std::vector<cv::Point2f> landmarks;  // 5个关键点
                                    // SCRFD 惯例：左眼/右眼/鼻尖/左嘴角/右嘴角；
                                    // 对齐(affine)由 exe 内部完成，此处仅随结果携带
    float raw_l2_norm;              // 特征向量归一化**前**的原始 L2 范数
                                    // 可作特征质量/置信参考（范数过小的特征区分度差）
    std::vector<float> feature;     // 512维已归一化特征
                                    // feature_normalized=true 时为单位向量，
                                    // 两特征点积即余弦相似度（见 calculateSimilarity）
};

// 检测结果
// 一张图片一次识别调用的全部输出（JSON 顶层字段的映射）
struct FaceDetectionResult {
    std::string image_path;         // 回显的输入图路径（来自 JSON "image" 字段）
    cv::Size image_size;            // 原图宽高（bbox 坐标系的参照尺寸）
    int feature_dim;                // 特征维度（当前模型为 512；JSON 缺字段时为 0）
    bool feature_normalized;        // 特征是否已 L2 归一化——消费方仅当 true 时
                                    // 才能把点积当余弦比较；false 时阈值语义失效
    int num_faces;                  // JSON 声称的人脸数；注意与 faces.size()
                                    // 可能不等（解析中非法 bbox 被剔除，见 .cpp）
    std::vector<DetectedFace> faces;
};

class FaceRecognitionWrapper {
public:
    FaceRecognitionWrapper();
    ~FaceRecognitionWrapper();
    
    // 设置exe和模型路径
    // 均为 exe 命令行位置参数，路径含空格/引号会破坏命令拼接（见 .cpp 注记）
    void setExecutablePath(const std::string& exe_path);
    void setDetectionModel(const std::string& det_model);
    void setRecognitionModel(const std::string& rec_model);
    // 检测置信度/NMS 阈值，作为命令行参数透传给 exe（本进程不做二次过滤）
    void setThresholds(float det_threshold, float nms_threshold);
    
    // 检测并提取特征
    // 同步阻塞调用；失败（exe 退出码非 0 / JSON 缺失或不可解析）时返回
    // 默认构造的空结果（faces 为空），不会抛异常穿透到此层——但 JSON
    // 解析内部对必需字段用 operator[]，畸形 JSON 可能抛异常（见 .cpp）
    FaceDetectionResult detectAndExtract(const std::string& image_path);
    
    // 计算两个特征向量的余弦相似度
    // 前提：两特征均为 L2 归一化单位向量（feature_normalized=true），
    // 此时点积=cos夹角，值域[-1,1]，越大越像；O(D) 单遍线性扫描。
    // 维度不等或空向量返回 0.0f（注意 0 也可能表示"正交"，调用方应先判
    // feature.empty() 再调用以区分两种含义）
    static float calculateSimilarity(const std::vector<float>& feat1, const std::vector<float>& feat2);
    
private:
    std::string exe_path_;
    std::string det_model_;
    std::string rec_model_;
    float det_threshold_;   // 默认 0.6（点名场景宁缺毋滥，高于通用检测阈值惯例）
    float nms_threshold_;   // 默认 0.4
    
    // 解析JSON结果
    FaceDetectionResult parseJsonResult(const std::string& json_path);
    
    // 执行外部命令
    bool executeCommand(const std::string& command);
};

#endif // FACE_RECOGNITION_WRAPPER_H