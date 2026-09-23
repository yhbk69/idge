#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include <opencv2/opencv.hpp>
#include <fstream>
#include <cmath>
#include <string>
#include "rknn_api.h"

static const int DET_SIZE = 640;
static const int REC_SIZE = 112;
static const int FEATURE_DIM = 512;

// ArcFace 112x112 标准五点模板
static const float ARCFACE_DST[5][2] = {
    {38.2946f, 51.6963f},
    {73.5318f, 51.5014f},
    {56.0252f, 71.7366f},
    {41.5493f, 92.3655f},
    {70.7299f, 92.2041f}
};

struct FaceBox {
    float x1, y1, x2, y2;
    float score;
    float landmarks[5][2];
};

struct DetPreprocessResult {
    cv::Mat input_rgb;
    float scale;
    int resized_w;
    int resized_h;
};

static bool load_rknn_model(const char* model_path, rknn_context* ctx) {
    FILE* fp = fopen(model_path, "rb");
    if (!fp) return false;

    fseek(fp, 0, SEEK_END);
    long model_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (model_size <= 0) {
        fclose(fp);
        return false;
    }

    void* model_data = malloc((size_t)model_size);
    if (!model_data) {
        fclose(fp);
        return false;
    }

    size_t read_size = fread(model_data, 1, (size_t)model_size, fp);
    fclose(fp);

    if (read_size != (size_t)model_size) {
        free(model_data);
        return false;
    }

    int ret = rknn_init(ctx, model_data, (uint32_t)model_size, 0, nullptr);
    free(model_data);
    return ret >= 0;
}

static float iou(const FaceBox& a, const FaceBox& b) {
    float inter_x1 = std::max(a.x1, b.x1);
    float inter_y1 = std::max(a.y1, b.y1);
    float inter_x2 = std::min(a.x2, b.x2);
    float inter_y2 = std::min(a.y2, b.y2);

    float inter_w = std::max(0.0f, inter_x2 - inter_x1);
    float inter_h = std::max(0.0f, inter_y2 - inter_y1);
    float inter_area = inter_w * inter_h;

    float area_a = std::max(0.0f, a.x2 - a.x1) * std::max(0.0f, a.y2 - a.y1);
    float area_b = std::max(0.0f, b.x2 - b.x1) * std::max(0.0f, b.y2 - b.y1);
    float denom = area_a + area_b - inter_area;

    return denom > 0.0f ? inter_area / denom : 0.0f;
}

static std::vector<FaceBox> nms(std::vector<FaceBox> boxes, float threshold) {
    std::sort(boxes.begin(), boxes.end(), [](const FaceBox& a, const FaceBox& b) {
        return a.score > b.score;
    });

    std::vector<FaceBox> result;
    std::vector<bool> suppressed(boxes.size(), false);

    for (size_t i = 0; i < boxes.size(); ++i) {
        if (suppressed[i]) continue;
        result.push_back(boxes[i]);

        for (size_t j = i + 1; j < boxes.size(); ++j) {
            if (suppressed[j]) continue;
            if (iou(boxes[i], boxes[j]) > threshold) {
                suppressed[j] = true;
            }
        }
    }

    return result;
}

static void l2_normalize(float* feat, int dim) {
    double sum = 0.0;
    for (int i = 0; i < dim; ++i) {
        sum += (double)feat[i] * (double)feat[i];
    }

    float norm = (float)std::sqrt(sum);
    if (norm > 1e-12f) {
        for (int i = 0; i < dim; ++i) {
            feat[i] /= norm;
        }
    }
}

// 保持原图比例缩放；不拉伸。缩放结果放在 640x640 左上角，其余区域补 0。
// 与前面已经验证通过的 SCRFD 输入方式保持一致。
static DetPreprocessResult preprocess_detection(const cv::Mat& bgr) {
    DetPreprocessResult result;

    float sx = (float)DET_SIZE / (float)bgr.cols;
    float sy = (float)DET_SIZE / (float)bgr.rows;
    result.scale = std::min(sx, sy);

    result.resized_w = std::max(1, std::min(DET_SIZE, (int)std::round(bgr.cols * result.scale)));
    result.resized_h = std::max(1, std::min(DET_SIZE, (int)std::round(bgr.rows * result.scale)));

    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(result.resized_w, result.resized_h), 0, 0, cv::INTER_LINEAR);

    cv::Mat canvas = cv::Mat::zeros(DET_SIZE, DET_SIZE, CV_8UC3);
    resized.copyTo(canvas(cv::Rect(0, 0, result.resized_w, result.resized_h)));

    cv::cvtColor(canvas, result.input_rgb, cv::COLOR_BGR2RGB);
    return result;
}

// 已验证的 SCRFD 9 输出解析：
// score: output 0/1/2
// bbox : output 3/4/5
// kps  : output 6/7/8
// 每个 grid cell 2 anchors；anchor 点为 (j*stride, i*stride)。
// kps 10 值布局为 [x0,y0,x1,y1,x2,y2,x3,y3,x4,y4]。
static std::vector<FaceBox> parse_scrfd_outputs(
    rknn_output* outputs,
    int orig_width,
    int orig_height,
    float det_scale,
    float score_threshold) {

    std::vector<FaceBox> boxes;

    struct ScaleInfo {
        int stride;
        int grid_h;
        int grid_w;
        int num_anchors;
        int score_idx;
        int bbox_idx;
        int kps_idx;
    };

    const ScaleInfo scales[3] = {
        {8,  80, 80, 2, 0, 3, 6},
        {16, 40, 40, 2, 1, 4, 7},
        {32, 20, 20, 2, 2, 5, 8}
    };

    const float inv_scale = 1.0f / det_scale;

    for (const auto& s : scales) {
        const float* score_data = (const float*)outputs[s.score_idx].buf;
        const float* bbox_data  = (const float*)outputs[s.bbox_idx].buf;
        const float* kps_data   = (const float*)outputs[s.kps_idx].buf;

        for (int i = 0; i < s.grid_h; ++i) {
            for (int j = 0; j < s.grid_w; ++j) {
                for (int a = 0; a < s.num_anchors; ++a) {
                    int idx = (i * s.grid_w + j) * s.num_anchors + a;
                    float score = score_data[idx];
                    if (score < score_threshold) continue;

                    // 注意：这里不是 (j + 0.5) * stride。
                    float cx = (float)(j * s.stride);
                    float cy = (float)(i * s.stride);

                    float left   = bbox_data[idx * 4 + 0] * s.stride;
                    float top    = bbox_data[idx * 4 + 1] * s.stride;
                    float right  = bbox_data[idx * 4 + 2] * s.stride;
                    float bottom = bbox_data[idx * 4 + 3] * s.stride;

                    FaceBox box{};
                    box.score = score;

                    box.x1 = (cx - left)   * inv_scale;
                    box.y1 = (cy - top)    * inv_scale;
                    box.x2 = (cx + right)  * inv_scale;
                    box.y2 = (cy + bottom) * inv_scale;

                    box.x1 = std::max(0.0f, std::min(box.x1, (float)(orig_width  - 1)));
                    box.y1 = std::max(0.0f, std::min(box.y1, (float)(orig_height - 1)));
                    box.x2 = std::max(0.0f, std::min(box.x2, (float)(orig_width  - 1)));
                    box.y2 = std::max(0.0f, std::min(box.y2, (float)(orig_height - 1)));

                    for (int k = 0; k < 5; ++k) {
                        float dx = kps_data[idx * 10 + k * 2 + 0] * s.stride;
                        float dy = kps_data[idx * 10 + k * 2 + 1] * s.stride;

                        box.landmarks[k][0] = (cx + dx) * inv_scale;
                        box.landmarks[k][1] = (cy + dy) * inv_scale;
                    }

                    boxes.push_back(box);
                }
            }
        }
    }

    return boxes;
}

// 五点相似变换。
// 这里的符号方向与前面验证出的 InsightFace/ArcFace 对齐结果一致：
// M = [ a -b tx ; b a ty ]
static bool similarity_transform_5pts(const float src[5][2], cv::Mat& M) {
    double src_mean_x = 0.0, src_mean_y = 0.0;
    double dst_mean_x = 0.0, dst_mean_y = 0.0;

    for (int i = 0; i < 5; ++i) {
        src_mean_x += src[i][0];
        src_mean_y += src[i][1];
        dst_mean_x += ARCFACE_DST[i][0];
        dst_mean_y += ARCFACE_DST[i][1];
    }

    src_mean_x /= 5.0;
    src_mean_y /= 5.0;
    dst_mean_x /= 5.0;
    dst_mean_y /= 5.0;

    double sxx = 0.0;
    double sxy = 0.0;
    double src_norm2 = 0.0;

    for (int i = 0; i < 5; ++i) {
        double sx = src[i][0] - src_mean_x;
        double sy = src[i][1] - src_mean_y;
        double dx = ARCFACE_DST[i][0] - dst_mean_x;
        double dy = ARCFACE_DST[i][1] - dst_mean_y;

        sxx += dx * sx + dy * sy;
        // 关键：使用这个符号，得到 [a,-b; b,a] 的正确旋转方向
        sxy += dy * sx - dx * sy;
        src_norm2 += sx * sx + sy * sy;
    }

    if (src_norm2 <= 1e-12) return false;

    double a = sxx / src_norm2;
    double b = sxy / src_norm2;

    double tx = dst_mean_x - (a * src_mean_x - b * src_mean_y);
    double ty = dst_mean_y - (b * src_mean_x + a * src_mean_y);

    M = (cv::Mat_<double>(2, 3) <<
        a, -b, tx,
        b,  a, ty);

    return true;
}
static bool estimate_similarity_umeyama(
        const float src_pts[5][2],
        cv::Mat& M,
        double* out_scale = nullptr,
        double* out_angle_deg = nullptr,
        double* out_rms = nullptr) {

    static const double dst_pts[5][2] = {
        {38.2946, 51.6963},
        {73.5318, 51.5014},
        {56.0252, 71.7366},
        {41.5493, 92.3655},
        {70.7299, 92.2041}
    };

    const int N = 5;
    const int D = 2;

    cv::Mat src(N, D, CV_64F);
    cv::Mat dst(N, D, CV_64F);

    for (int i = 0; i < N; ++i) {
        src.at<double>(i, 0) = src_pts[i][0];
        src.at<double>(i, 1) = src_pts[i][1];

        dst.at<double>(i, 0) = dst_pts[i][0];
        dst.at<double>(i, 1) = dst_pts[i][1];
    }

    // 均值
    cv::Mat src_mean;
    cv::Mat dst_mean;

    cv::reduce(src, src_mean, 0, cv::REDUCE_AVG, CV_64F);
    cv::reduce(dst, dst_mean, 0, cv::REDUCE_AVG, CV_64F);

    cv::Mat src_demean = src.clone();
    cv::Mat dst_demean = dst.clone();

    for (int i = 0; i < N; ++i) {
        src_demean.at<double>(i, 0) -= src_mean.at<double>(0, 0);
        src_demean.at<double>(i, 1) -= src_mean.at<double>(0, 1);

        dst_demean.at<double>(i, 0) -= dst_mean.at<double>(0, 0);
        dst_demean.at<double>(i, 1) -= dst_mean.at<double>(0, 1);
    }

    // A = dst_demean^T * src_demean / N
    cv::Mat A =
        (dst_demean.t() * src_demean) /
        static_cast<double>(N);

    // src variance = sum(var of each dimension)
    double src_var = 0.0;

    for (int i = 0; i < N; ++i) {
        double x = src_demean.at<double>(i, 0);
        double y = src_demean.at<double>(i, 1);

        src_var += x * x + y * y;
    }

    src_var /= static_cast<double>(N);

    if (src_var <= std::numeric_limits<double>::epsilon()) {
        printf("ERROR: Umeyama source variance is zero.\n");
        return false;
    }

    cv::SVD svd(
        A,
        cv::SVD::FULL_UV);

    cv::Mat U = svd.u;
    cv::Mat Vt = svd.vt;
    cv::Mat S = svd.w;

    // skimage _umeyama:
    // d = ones(dim)
    // if det(A) < 0: d[dim-1] = -1
    double d0 = 1.0;
    double d1 = 1.0;

    if (cv::determinant(A) < 0.0) {
        d1 = -1.0;
    }

    cv::Mat Dm =
        (cv::Mat_<double>(2, 2) <<
            d0, 0.0,
            0.0, d1);

    // R = U * diag(d) * Vt
    cv::Mat R =
        U * Dm * Vt;

    // scale = dot(S, d) / src_var
    double s0 = S.at<double>(0, 0);
    double s1 = S.at<double>(1, 0);

    double scale =
        (s0 * d0 + s1 * d1) / src_var;

    // t = dst_mean - scale * R * src_mean
    cv::Mat src_mean_col =
        (cv::Mat_<double>(2, 1) <<
            src_mean.at<double>(0, 0),
            src_mean.at<double>(0, 1));

    cv::Mat dst_mean_col =
        (cv::Mat_<double>(2, 1) <<
            dst_mean.at<double>(0, 0),
            dst_mean.at<double>(0, 1));

    cv::Mat t =
        dst_mean_col -
        scale * R * src_mean_col;

    M = cv::Mat::zeros(2, 3, CV_64F);

    M.at<double>(0, 0) = scale * R.at<double>(0, 0);
    M.at<double>(0, 1) = scale * R.at<double>(0, 1);
    M.at<double>(1, 0) = scale * R.at<double>(1, 0);
    M.at<double>(1, 1) = scale * R.at<double>(1, 1);

    M.at<double>(0, 2) = t.at<double>(0, 0);
    M.at<double>(1, 2) = t.at<double>(1, 0);

    // 旋转角
    double angle_deg =
        std::atan2(
            R.at<double>(1, 0),
            R.at<double>(0, 0)) *
        180.0 / CV_PI;

    // 计算 5 点 RMS
    double sqerr = 0.0;

    for (int i = 0; i < N; ++i) {
        double x = src_pts[i][0];
        double y = src_pts[i][1];

        double xp =
            M.at<double>(0, 0) * x +
            M.at<double>(0, 1) * y +
            M.at<double>(0, 2);

        double yp =
            M.at<double>(1, 0) * x +
            M.at<double>(1, 1) * y +
            M.at<double>(1, 2);

        double dx = xp - dst_pts[i][0];
        double dy = yp - dst_pts[i][1];

        sqerr += dx * dx + dy * dy;
    }

    double rms =
        std::sqrt(sqerr / static_cast<double>(N));

    if (out_scale)
        *out_scale = scale;

    if (out_angle_deg)
        *out_angle_deg = angle_deg;

    if (out_rms)
        *out_rms = rms;

    return true;
}
// 替换这个函数
static bool align_face(const cv::Mat& img, const float landmarks[5][2], cv::Mat& aligned) {
    cv::Mat M;
    if (!estimate_similarity_umeyama(landmarks, M)) return false;  // 使用 Umeyama
    
    cv::warpAffine(
        img, aligned, M,
        cv::Size(REC_SIZE, REC_SIZE),
        cv::INTER_LINEAR,
        cv::BORDER_CONSTANT,
        cv::Scalar(0, 0, 0));
    
    return !aligned.empty();
}

static bool detect_faces(
    rknn_context det_ctx,
    const cv::Mat& img,
    float score_threshold,
    float nms_threshold,
    std::vector<FaceBox>& faces) {

    DetPreprocessResult prep = preprocess_detection(img);

    std::vector<float> input_f32(DET_SIZE * DET_SIZE * 3);
    const float norm_scale = 1.0f / 128.0f;

    const size_t total = prep.input_rgb.total() * prep.input_rgb.channels();
    for (size_t i = 0; i < total; ++i) {
        input_f32[i] = ((float)prep.input_rgb.data[i] - 127.5f) * norm_scale;
    }

    rknn_input input{};
    input.index = 0;
    input.type = RKNN_TENSOR_FLOAT32;
    input.size = (uint32_t)(input_f32.size() * sizeof(float));
    input.fmt = RKNN_TENSOR_NHWC;
    input.buf = input_f32.data();
    input.pass_through = 0;

    int ret = rknn_inputs_set(det_ctx, 1, &input);
    if (ret < 0) return false;

    ret = rknn_run(det_ctx, nullptr);
    if (ret < 0) return false;

    rknn_output outputs[9];
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < 9; ++i) outputs[i].want_float = 1;

    ret = rknn_outputs_get(det_ctx, 9, outputs, nullptr);
    if (ret < 0) return false;

    std::vector<FaceBox> raw = parse_scrfd_outputs(
        outputs,
        img.cols,
        img.rows,
        prep.scale,
        score_threshold);

    rknn_outputs_release(det_ctx, 9, outputs);

    faces = nms(raw, nms_threshold);
    return true;
}

static bool extract_feature(
    rknn_context rec_ctx,
    const cv::Mat& img,
    const FaceBox& face,
    std::vector<float>& feature_vec,
    float& raw_l2_norm) {

    cv::Mat aligned;
    if (!align_face(img, face.landmarks, aligned)) return false;

    cv::Mat rgb;
    cv::cvtColor(aligned, rgb, cv::COLOR_BGR2RGB);

    std::vector<float> input_f32(REC_SIZE * REC_SIZE * 3);
    const float norm_scale = 1.0f / 127.5f;

    const size_t total = rgb.total() * rgb.channels();
    for (size_t i = 0; i < total; ++i) {
        input_f32[i] = ((float)rgb.data[i] - 127.5f) * norm_scale;
    }

    rknn_input input{};
    input.index = 0;
    input.type = RKNN_TENSOR_FLOAT32;
    input.size = (uint32_t)(input_f32.size() * sizeof(float));
    input.fmt = RKNN_TENSOR_NHWC;
    input.buf = input_f32.data();
    input.pass_through = 0;

    int ret = rknn_inputs_set(rec_ctx, 1, &input);
    if (ret < 0) return false;

    ret = rknn_run(rec_ctx, nullptr);
    if (ret < 0) return false;

    rknn_output output{};
    output.want_float = 1;

    ret = rknn_outputs_get(rec_ctx, 1, &output, nullptr);
    if (ret < 0) return false;

    const float* feature = (const float*)output.buf;
    feature_vec.assign(feature, feature + FEATURE_DIM);

    double sum = 0.0;
    for (int i = 0; i < FEATURE_DIM; ++i) {
        sum += (double)feature[i] * (double)feature[i];
    }
    raw_l2_norm = (float)std::sqrt(sum);

    l2_normalize(feature_vec.data(), FEATURE_DIM);
    rknn_outputs_release(rec_ctx, 1, &output);

    return true;
}

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);

    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

static void write_float_array(std::ofstream& out, const std::vector<float>& values) {
    out << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << ", ";
        out << values[i];
    }
    out << "]";
}

static bool save_json(
    const char* output_json,
    const char* image_path,
    const cv::Mat& img,
    const std::vector<FaceBox>& faces,
    const std::vector<std::vector<float>>& features,
    const std::vector<float>& raw_l2_norms) {

    std::ofstream out(output_json);
    if (!out.is_open()) return false;

    out.setf(std::ios::fixed);
    out.precision(8);

    out << "{\n";
    out << "  \"image\": \"" << json_escape(image_path) << "\",\n";
    out << "  \"image_size\": [" << img.cols << ", " << img.rows << "],\n";
    out << "  \"feature_dim\": " << FEATURE_DIM << ",\n";
    out << "  \"feature_normalized\": true,\n";
    out << "  \"num_faces\": " << faces.size() << ",\n";
    out << "  \"faces\": [\n";

    for (size_t i = 0; i < faces.size(); ++i) {
        const FaceBox& f = faces[i];

        out << "    {\n";
        out << "      \"face_id\": " << i << ",\n";
        out << "      \"score\": " << f.score << ",\n";
        out << "      \"bbox\": [" << f.x1 << ", " << f.y1 << ", " << f.x2 << ", " << f.y2 << "],\n";
        out << "      \"landmarks\": [";
        for (int k = 0; k < 5; ++k) {
            if (k) out << ", ";
            out << "[" << f.landmarks[k][0] << ", " << f.landmarks[k][1] << "]";
        }
        out << "],\n";
        out << "      \"raw_l2_norm\": " << raw_l2_norms[i] << ",\n";
        out << "      \"feature\": ";
        write_float_array(out, features[i]);
        out << "\n";
        out << "    }";
        if (i + 1 < faces.size()) out << ",";
        out << "\n";
    }

    out << "  ]\n";
    out << "}\n";
    return true;
}

static bool save_visualization(
    const cv::Mat& img,
    const std::vector<FaceBox>& faces,
    const std::string& vis_path) {

    cv::Mat vis = img.clone();

    for (size_t i = 0; i < faces.size(); ++i) {
        const FaceBox& f = faces[i];

        cv::rectangle(
            vis,
            cv::Point((int)std::round(f.x1), (int)std::round(f.y1)),
            cv::Point((int)std::round(f.x2), (int)std::round(f.y2)),
            cv::Scalar(0, 255, 0), 2);

        static const cv::Scalar colors[5] = {
            cv::Scalar(0, 0, 255),
            cv::Scalar(0, 255, 0),
            cv::Scalar(255, 0, 0),
            cv::Scalar(255, 0, 255),
            cv::Scalar(0, 255, 255)
        };

        for (int k = 0; k < 5; ++k) {
            cv::circle(
                vis,
                cv::Point((int)std::round(f.landmarks[k][0]), (int)std::round(f.landmarks[k][1])),
                3, colors[k], -1);
        }

        char label[64];
        snprintf(label, sizeof(label), "id=%zu score=%.3f", i, f.score);
        int text_y = std::max(18, (int)std::round(f.y1) - 6);
        cv::putText(
            vis,
            label,
            cv::Point((int)std::round(f.x1), text_y),
            cv::FONT_HERSHEY_SIMPLEX,
            0.55,
            cv::Scalar(0, 255, 0),
            1,
            cv::LINE_AA);
    }

    return cv::imwrite(vis_path, vis);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("Usage: %s <det_model.rknn> <rec_model.rknn> <image.jpg> [output.json] [score_threshold] [nms_threshold]\n", argv[0]);
        return 1;
    }

    const char* det_model_path = argv[1];
    const char* rec_model_path = argv[2];
    const char* image_path = argv[3];
    const char* output_json = (argc > 4) ? argv[4] : "faces.json";
    float score_threshold = (argc > 5) ? (float)atof(argv[5]) : 0.5f;
    float nms_threshold = (argc > 6) ? (float)atof(argv[6]) : 0.4f;

    rknn_context det_ctx = 0;
    rknn_context rec_ctx = 0;

    if (!load_rknn_model(det_model_path, &det_ctx)) {
        fprintf(stderr, "Failed to load detection model: %s\n", det_model_path);
        return 1;
    }

    if (!load_rknn_model(rec_model_path, &rec_ctx)) {
        fprintf(stderr, "Failed to load recognition model: %s\n", rec_model_path);
        rknn_destroy(det_ctx);
        return 1;
    }

    cv::Mat img = cv::imread(image_path);
    if (img.empty()) {
        fprintf(stderr, "Failed to read image: %s\n", image_path);
        rknn_destroy(det_ctx);
        rknn_destroy(rec_ctx);
        return 1;
    }

    std::vector<FaceBox> faces;
    if (!detect_faces(det_ctx, img, score_threshold, nms_threshold, faces)) {
        fprintf(stderr, "Face detection failed.\n");
        rknn_destroy(det_ctx);
        rknn_destroy(rec_ctx);
        return 1;
    }

    std::vector<std::vector<float>> features;
    std::vector<float> raw_l2_norms;
    features.reserve(faces.size());
    raw_l2_norms.reserve(faces.size());

    bool feature_ok = true;
    for (size_t i = 0; i < faces.size(); ++i) {
        std::vector<float> feature;
        float raw_l2 = 0.0f;

        if (!extract_feature(rec_ctx, img, faces[i], feature, raw_l2)) {
            fprintf(stderr, "Feature extraction failed for face %zu.\n", i);
            feature_ok = false;
            break;
        }

        features.push_back(std::move(feature));
        raw_l2_norms.push_back(raw_l2);
    }

    if (!feature_ok) {
        rknn_destroy(det_ctx);
        rknn_destroy(rec_ctx);
        return 1;
    }

    if (!save_json(output_json, image_path, img, faces, features, raw_l2_norms)) {
        fprintf(stderr, "Failed to save JSON: %s\n", output_json);
        rknn_destroy(det_ctx);
        rknn_destroy(rec_ctx);
        return 1;
    }

    std::string vis_path = std::string(image_path) + "_result.jpg";
    save_visualization(img, faces, vis_path);

    rknn_destroy(det_ctx);
    rknn_destroy(rec_ctx);

    printf("Detected faces: %zu\n", faces.size());
    printf("Feature JSON : %s\n", output_json);
    printf("Result image : %s\n", vis_path.c_str());

    return 0;
}
