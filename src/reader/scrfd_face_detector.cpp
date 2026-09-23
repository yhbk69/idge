#include "scrfd_face_detector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>
#include <opencv2/imgproc.hpp>

namespace {
constexpr int kInputSize = 640;

struct Candidate {
    float x1, y1, x2, y2, score;
};

float iou(const Candidate& a, const Candidate& b)
{
    const float x1 = std::max(a.x1, b.x1);
    const float y1 = std::max(a.y1, b.y1);
    const float x2 = std::min(a.x2, b.x2);
    const float y2 = std::min(a.y2, b.y2);
    const float intersection = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    const float area_a = std::max(0.0f, a.x2 - a.x1) * std::max(0.0f, a.y2 - a.y1);
    const float area_b = std::max(0.0f, b.x2 - b.x1) * std::max(0.0f, b.y2 - b.y1);
    const float denominator = area_a + area_b - intersection;
    return denominator > 0.0f ? intersection / denominator : 0.0f;
}

std::vector<Candidate> nms(std::vector<Candidate> candidates, float threshold)
{
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    std::vector<Candidate> result;
    std::vector<bool> suppressed(candidates.size(), false);
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (suppressed[i]) continue;
        result.push_back(candidates[i]);
        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (!suppressed[j] && iou(candidates[i], candidates[j]) > threshold)
                suppressed[j] = true;
        }
    }
    return result;
}
} // namespace

ScrfdFaceDetector::~ScrfdFaceDetector()
{
    release();
}

bool ScrfdFaceDetector::init(const std::string& model_path, rknn_core_mask core_mask)
{
    release();
    std::ifstream file(model_path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamsize size = file.tellg();
    if (size <= 0) return false;
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> model(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(model.data()), size)) return false;
    if (rknn_init(&context_, model.data(), static_cast<uint32_t>(model.size()), 0, nullptr) < 0)
        return false;
    if (rknn_set_core_mask(context_, core_mask) < 0) {
        release();
        return false;
    }
    return true;
}

void ScrfdFaceDetector::release()
{
    if (context_ != 0) {
        rknn_destroy(context_);
        context_ = 0;
    }
}

bool ScrfdFaceDetector::detectRgba(const uint8_t* pixels, int width, int height, int stride,
                                   std::vector<ScrfdFaceBox>& faces,
                                   float score_threshold, float nms_threshold)
{
    faces.clear();
    if (!isInitialized() || !pixels || width <= 0 || height <= 0 || stride < width * 4)
        return false;

    cv::Mat rgba(height, width, CV_8UC4, const_cast<uint8_t*>(pixels), stride);
    cv::Mat bgr;
    cv::cvtColor(rgba, bgr, cv::COLOR_RGBA2BGR);

    const float scale = std::min(kInputSize / static_cast<float>(width),
                                 kInputSize / static_cast<float>(height));
    const int resized_width = std::max(1, std::min(kInputSize, static_cast<int>(std::round(width * scale))));
    const int resized_height = std::max(1, std::min(kInputSize, static_cast<int>(std::round(height * scale))));
    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(resized_width, resized_height), 0, 0, cv::INTER_LINEAR);
    cv::Mat canvas = cv::Mat::zeros(kInputSize, kInputSize, CV_8UC3);
    resized.copyTo(canvas(cv::Rect(0, 0, resized_width, resized_height)));
    cv::Mat rgb;
    cv::cvtColor(canvas, rgb, cv::COLOR_BGR2RGB);

    std::vector<float> input_data(static_cast<size_t>(kInputSize) * kInputSize * 3);
    for (size_t i = 0; i < input_data.size(); ++i)
        input_data[i] = (static_cast<float>(rgb.data[i]) - 127.5f) / 128.0f;

    rknn_input input{};
    input.index = 0;
    input.type = RKNN_TENSOR_FLOAT32;
    input.fmt = RKNN_TENSOR_NHWC;
    input.size = static_cast<uint32_t>(input_data.size() * sizeof(float));
    input.buf = input_data.data();
    if (rknn_inputs_set(context_, 1, &input) < 0 || rknn_run(context_, nullptr) < 0)
        return false;

    rknn_output outputs[9]{};
    for (auto& output : outputs) output.want_float = 1;
    if (rknn_outputs_get(context_, 9, outputs, nullptr) < 0)
        return false;

    struct Scale { int stride; int grid; int score; int bbox; };
    constexpr Scale scales[] = {{8, 80, 0, 3}, {16, 40, 1, 4}, {32, 20, 2, 5}};
    std::vector<Candidate> candidates;
    const float inverse_scale = 1.0f / scale;
    for (const auto& current : scales) {
        const float* score_data = static_cast<const float*>(outputs[current.score].buf);
        const float* bbox_data = static_cast<const float*>(outputs[current.bbox].buf);
        for (int y = 0; y < current.grid; ++y) {
            for (int x = 0; x < current.grid; ++x) {
                for (int anchor = 0; anchor < 2; ++anchor) {
                    const int index = (y * current.grid + x) * 2 + anchor;
                    const float score = score_data[index];
                    if (score < score_threshold) continue;
                    const float cx = static_cast<float>(x * current.stride);
                    const float cy = static_cast<float>(y * current.stride);
                    Candidate candidate{
                        (cx - bbox_data[index * 4 + 0] * current.stride) * inverse_scale,
                        (cy - bbox_data[index * 4 + 1] * current.stride) * inverse_scale,
                        (cx + bbox_data[index * 4 + 2] * current.stride) * inverse_scale,
                        (cy + bbox_data[index * 4 + 3] * current.stride) * inverse_scale,
                        score};
                    candidate.x1 = std::clamp(candidate.x1, 0.0f, static_cast<float>(width - 1));
                    candidate.y1 = std::clamp(candidate.y1, 0.0f, static_cast<float>(height - 1));
                    candidate.x2 = std::clamp(candidate.x2, 0.0f, static_cast<float>(width - 1));
                    candidate.y2 = std::clamp(candidate.y2, 0.0f, static_cast<float>(height - 1));
                    if (candidate.x2 > candidate.x1 && candidate.y2 > candidate.y1)
                        candidates.push_back(candidate);
                }
            }
        }
    }
    rknn_outputs_release(context_, 9, outputs);

    for (const Candidate& candidate : nms(std::move(candidates), nms_threshold)) {
        faces.push_back({cv::Rect(static_cast<int>(candidate.x1), static_cast<int>(candidate.y1),
                                  static_cast<int>(candidate.x2 - candidate.x1),
                                  static_cast<int>(candidate.y2 - candidate.y1)),
                         candidate.score});
    }
    return true;
}
