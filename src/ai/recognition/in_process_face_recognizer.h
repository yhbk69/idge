#ifndef IN_PROCESS_FACE_RECOGNIZER_H
#define IN_PROCESS_FACE_RECOGNIZER_H

// ============================================================================
// InProcessFaceRecognizer —— 进程内人脸检测+识别（点名链路的推理宿主）
// ============================================================================
// 职责：常驻加载 SCRFD 检测与 ArcFace 识别两个 rknn 上下文，对图片路径完成
//   "检测(640 letterbox) → NMS → 五点仿射对齐(112) → 512 维特征 + L2 归一化"
//   全链路，输出 FaceDetectionResult（与外部 exe 时代的结构体完全一致，
//   下游去重/匹配/落库无感）。推理链逐段移植自 tools/face_recognition/main.cpp，
//   以替代原 fork+execv 子进程方案——外部 exe 每次调用都重新 rknn_init 加载
//   两个模型（10MB+85MB），常驻实例把该秒级开销摊薄为一次。
// 线程模型：单个 rknn_context 非线程安全，本类约定由 RollCallService 的
//   调用线程串行驱动（照片级并行已随内化移除，见 roll_call_service.h）。
// 失败语义：与旧 FaceRecognitionWrapper 对齐——init 失败返回 false；
//   detectAndExtract 整体失败（未初始化/读图失败/检测推理出错）返回空结果，
//   单脸对齐/特征提取失败仅剔除该脸；任何路径都不抛异常。
// ============================================================================

#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <rknn_api.h>

#include "face_recognizer.h"  // 复用 DetectedFace / FaceDetectionResult 结构体

// SCRFD 原始候选框（含五点关键点，坐标已逆变换回原图像素系）；
// 仅供类内推理链中间传递，不随结果对外暴露
struct InProcessFaceBox {
    float x1, y1, x2, y2;
    float score;
    float landmarks[5][2];
};

class InProcessFaceRecognizer {
public:
    InProcessFaceRecognizer() = default;
    ~InProcessFaceRecognizer();

    // rknn_context 是裸句柄，禁拷贝（同 ScrfdFaceDetector 约定）
    InProcessFaceRecognizer(const InProcessFaceRecognizer&) = delete;
    InProcessFaceRecognizer& operator=(const InProcessFaceRecognizer&) = delete;

    // 加载检测+识别两个模型并绑定 NPU 核。先 release() 支持换模型重复调用；
    // 任一模型失败则两个上下文全部回收，保证"返回 false ⇒ 无半成品状态"。
    bool init(const std::string& det_model_path,
              const std::string& rec_model_path,
              rknn_core_mask core_mask);
    void release();
    bool isInitialized() const { return det_ctx_ != 0 && rec_ctx_ != 0; }

    // 检测置信度 / NMS IoU 阈值（点名场景默认 0.6/0.4，与旧 exe 调用参数一致）
    void setThresholds(float det_threshold, float nms_threshold) {
        det_threshold_ = det_threshold;
        nms_threshold_ = nms_threshold;
    }

    // 单图全链识别：cv::imread 解码 → 检测 → 逐脸对齐+特征提取。
    // 同步阻塞（单张数十 ms 级）；读图/推理失败返回空结果（faces 为空）；
    // 单脸对齐或特征提取失败仅丢弃该脸（同旧 JSON 解析层剔除非法 bbox 的粒度）。
    FaceDetectionResult detectAndExtract(const std::string& image_path);

private:
    bool detectFaces(const cv::Mat& bgr, std::vector<InProcessFaceBox>& faces);
    bool extractFeature(const cv::Mat& bgr, const InProcessFaceBox& face,
                        std::vector<float>& feature, float& raw_l2_norm);

    rknn_context det_ctx_ = 0;   // SCRFD 检测上下文；0=未初始化
    rknn_context rec_ctx_ = 0;   // ArcFace 识别上下文；0=未初始化
    float det_threshold_ = 0.6f; // 点名场景宁缺毋滥，高于通用检测阈值惯例
    float nms_threshold_ = 0.4f;
};

#endif  // IN_PROCESS_FACE_RECOGNIZER_H
