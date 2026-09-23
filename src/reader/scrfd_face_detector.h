#ifndef SCRFD_FACE_DETECTOR_H
#define SCRFD_FACE_DETECTOR_H

#include <cstdint>
#include <string>
#include <vector>
#include <opencv2/core.hpp>
#include <rknn_api.h>

struct ScrfdFaceBox {
    cv::Rect rect;
    float score = 0.0f;
};

class ScrfdFaceDetector final {
public:
    ScrfdFaceDetector() = default;
    ~ScrfdFaceDetector();

    ScrfdFaceDetector(const ScrfdFaceDetector&) = delete;
    ScrfdFaceDetector& operator=(const ScrfdFaceDetector&) = delete;

    bool init(const std::string& model_path, rknn_core_mask core_mask);
    void release();
    bool isInitialized() const { return context_ != 0; }

    // pixels is an RGBA8888 frame. Results are returned in the source frame coordinates.
    bool detectRgba(const uint8_t* pixels, int width, int height, int stride,
                   std::vector<ScrfdFaceBox>& faces,
                   float score_threshold = 0.5f,
                   float nms_threshold = 0.4f);

private:
    rknn_context context_ = 0;
};

#endif
