#ifndef SCRFD_FACE_DETECTOR_H
#define SCRFD_FACE_DETECTOR_H

// ============================================================================
// ScrfdFaceDetector —— SCRFD 人脸检测器（人员点名链路的第一级）
// ============================================================================
// 职责：加载 SCRFD rknn 模型（640 输入、9 输出：3 个 stride 层级 ×
//   {score, bbox, kps} 三分支布局，kps 通道当前未消费），对调用方给出的
//   RGBA8888 帧做同步推理并返回原图坐标系下的人脸框。
// 定位：只做人脸"检出"；1:1 特征比对由 recognition/FaceRecognitionWrapper
//   （外部进程）完成，两者共同支撑点名/考勤流程。
// 线程安全：内部单一 rknn_context 非线程安全，约定仅在解码线程内使用
//   （CameraPreviewDecoder 以 unique_ptr 独占持有，同步调用）。
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>
#include <opencv2/core.hpp>
#include <rknn_api.h>

// 一张检出的人脸：rect 为源图像素坐标（已 clamp 到画面内），
// score 为检测置信度（sigmoid 前的原始分数已由导出端归一为 0~1）
struct ScrfdFaceBox {
    cv::Rect rect;
    float score = 0.0f;
};

class ScrfdFaceDetector final {
public:
    ScrfdFaceDetector() = default;
    ~ScrfdFaceDetector();

    // 禁止拷贝：rknn_context 是裸句柄，浅拷贝会导致双重 rknn_destroy
    ScrfdFaceDetector(const ScrfdFaceDetector&) = delete;
    ScrfdFaceDetector& operator=(const ScrfdFaceDetector&) = delete;

    // 读入 .rknn 文件并初始化 NPU 上下文；core_mask 指定绑定的 NPU 核
    // （RK3588 三核，与级联 PpeTask 错核可并行）。失败返回 false 且不留半成品上下文。
    bool init(const std::string& model_path, rknn_core_mask core_mask);
    void release();
    bool isInitialized() const { return context_ != 0; }

    // pixels is an RGBA8888 frame. Results are returned in the source frame coordinates.
    // 预处理为"左上对齐 letterbox"：等比缩放后右下角补 0（黑边），与 YOLO
    // 主流水线的居中灰边不同，因此坐标逆变换只需除以 scale、无平移项。
    // score_threshold=0.5 为点名场景经验值（宁缺勿误报）；nms_threshold=0.4
    // 与 SCRFD 官方推理配置一致。返回 false 表示推理链路失败（非"没人"）。
    bool detectRgba(const uint8_t* pixels, int width, int height, int stride,
                   std::vector<ScrfdFaceBox>& faces,
                   float score_threshold = 0.5f,
                   float nms_threshold = 0.4f);

private:
    rknn_context context_ = 0;   // RKNN 上下文句柄；0 表示未初始化
};

#endif
