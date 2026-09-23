#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmath>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <string>
#include <limits>

#include <opencv2/opencv.hpp>
#include "rknn_api.h"

// ============================================================================
// InsightFace SCRFD(det_10g) + ArcFace(w600k_r50) RKNN C++ final version
//
// 已确认并修正：
// 1. SCRFD 9 outputs 顺序：
//    0/1/2 = score stride 8/16/32
//    3/4/5 = bbox  stride 8/16/32
//    6/7/8 = kps   stride 8/16/32
// 2. anchor center = (j * stride, i * stride)，不加 0.5
// 3. 5 点输出顺序 = [dx0,dy0, dx1,dy1, ... dx4,dy4]
// 4. 检测输入保持宽高比，左上放置，右/下补黑到 640x640
// 5. 坐标恢复只除以一个 det_scale
// 6. 人脸对齐使用 Umeyama Similarity Transform，等价于
//    skimage.transform.SimilarityTransform().estimate(src, dst) 的核心算法
// 7. ArcFace 标准 112x112 五点模板不修改
//
// 运行：
//   ./app det.rknn rec.rknn multi.jpg single.jpg [out.jpg] [similarity_threshold]
//
// 例：
//   ./app det_10g.rknn w600k_r50.rknn test.jpg 3.jpg out.jpg 0.8
// ============================================================================

static const int DET_W = 640;
static const int DET_H = 640;
static const int REC_W = 112;
static const int REC_H = 112;
static const int REC_FEAT_DIM = 512;

struct FaceBox {
    float x1, y1, x2, y2;
    float score;
    float landmarks[5][2];
};

struct DetPrep {
    cv::Mat canvas_bgr;
    cv::Mat canvas_rgb;
    float det_scale;
    int new_w;
    int new_h;
};

// ============================================================================
// RKNN debug helpers
// ============================================================================

static const char* tensor_type_str(rknn_tensor_type type) {
    switch (type) {
        case RKNN_TENSOR_FLOAT32: return "FLOAT32";
        case RKNN_TENSOR_FLOAT16: return "FLOAT16";
        case RKNN_TENSOR_INT8:    return "INT8";
        case RKNN_TENSOR_UINT8:   return "UINT8";
        case RKNN_TENSOR_INT16:   return "INT16";
        case RKNN_TENSOR_UINT16:  return "UINT16";
        case RKNN_TENSOR_INT32:   return "INT32";
        case RKNN_TENSOR_UINT32:  return "UINT32";
        case RKNN_TENSOR_INT64:   return "INT64";
        case RKNN_TENSOR_BOOL:    return "BOOL";
        default:                  return "UNKNOWN";
    }
}

static const char* tensor_fmt_str(rknn_tensor_format fmt) {
    switch (fmt) {
        case RKNN_TENSOR_NCHW:       return "NCHW";
        case RKNN_TENSOR_NHWC:       return "NHWC";
        case RKNN_TENSOR_NC1HWC2:    return "NC1HWC2";
        case RKNN_TENSOR_UNDEFINED:  return "UNDEFINED";
        default:                     return "UNKNOWN";
    }
}

static void print_tensor_attr(const char* tag, const rknn_tensor_attr& a) {
    printf("%s index=%u name=%s\n", tag, a.index, a.name);
    printf("    n_dims=%u dims=[", a.n_dims);
    for (uint32_t i = 0; i < a.n_dims; ++i) {
        printf("%u", a.dims[i]);
        if (i + 1 < a.n_dims) printf(", ");
    }
    printf("]\n");

    printf("    n_elems=%u size=%u fmt=%s(%d) type=%s(%d)\n",
           a.n_elems, a.size,
           tensor_fmt_str(a.fmt), (int)a.fmt,
           tensor_type_str(a.type), (int)a.type);

    printf("    qnt_type=%d zp=%d scale=%g\n",
           (int)a.qnt_type, (int)a.zp, a.scale);
}

static bool query_model_io(rknn_context ctx,
                           const char* model_tag,
                           rknn_input_output_num& io_num,
                           std::vector<rknn_tensor_attr>& input_attrs,
                           std::vector<rknn_tensor_attr>& output_attrs) {
    memset(&io_num, 0, sizeof(io_num));

    int ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM,
                         &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        printf("[%s] RKNN_QUERY_IN_OUT_NUM failed, ret=%d\n",
               model_tag, ret);
        return false;
    }

    input_attrs.resize(io_num.n_input);
    output_attrs.resize(io_num.n_output);

    printf("\n================ %s MODEL IO ================\n", model_tag);
    printf("n_input=%u, n_output=%u\n", io_num.n_input, io_num.n_output);

    for (uint32_t i = 0; i < io_num.n_input; ++i) {
        memset(&input_attrs[i], 0, sizeof(rknn_tensor_attr));
        input_attrs[i].index = i;

        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR,
                         &input_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            printf("[%s] query input[%u] failed ret=%d\n",
                   model_tag, i, ret);
            return false;
        }

        char tag[64];
        snprintf(tag, sizeof(tag), "INPUT[%u]", i);
        print_tensor_attr(tag, input_attrs[i]);
    }

    for (uint32_t i = 0; i < io_num.n_output; ++i) {
        memset(&output_attrs[i], 0, sizeof(rknn_tensor_attr));
        output_attrs[i].index = i;

        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR,
                         &output_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            printf("[%s] query output[%u] failed ret=%d\n",
                   model_tag, i, ret);
            return false;
        }

        char tag[64];
        snprintf(tag, sizeof(tag), "OUTPUT[%u]", i);
        print_tensor_attr(tag, output_attrs[i]);
    }

    printf("====================================================\n\n");
    return true;
}

static bool validate_scrfd_9_outputs(
        const std::vector<rknn_tensor_attr>& attrs) {

    printf("\n================ SCRFD OUTPUT ORDER CHECK ================\n");

    if (attrs.size() != 9) {
        printf("ERROR: detector has %zu outputs, expected 9.\n",
               attrs.size());
        printf("===========================================================\n\n");
        return false;
    }

    const uint32_t expected[9] = {
        80u * 80u * 2u,
        40u * 40u * 2u,
        20u * 20u * 2u,

        80u * 80u * 2u * 4u,
        40u * 40u * 2u * 4u,
        20u * 20u * 2u * 4u,

        80u * 80u * 2u * 10u,
        40u * 40u * 2u * 10u,
        20u * 20u * 2u * 10u
    };

    const char* meaning[9] = {
        "score_s8", "score_s16", "score_s32",
        "bbox_s8",  "bbox_s16",  "bbox_s32",
        "kps_s8",   "kps_s16",   "kps_s32"
    };

    bool all_ok = true;

    for (int i = 0; i < 9; ++i) {
        bool ok = (attrs[i].n_elems == expected[i]);

        printf("output[%d] assumed %-9s : actual n_elems=%u, expected=%u -> %s\n",
               i, meaning[i],
               attrs[i].n_elems,
               expected[i],
               ok ? "OK" : "MISMATCH");

        if (!ok) all_ok = false;
    }

    printf("Result: %s\n",
           all_ok
           ? "standard SCRFD 9-output ordering confirmed."
           : "output ordering/shape mismatch.");

    printf("===========================================================\n\n");

    return all_ok;
}

static void dump_output_stats(
        const char* tag,
        rknn_output* outputs,
        const std::vector<rknn_tensor_attr>& attrs) {

    printf("\n================ DETECTOR OUTPUT STATS [%s] ================\n",
           tag);

    const uint32_t first_count = 12;

    for (size_t i = 0; i < attrs.size(); ++i) {
        if (!outputs[i].buf) {
            printf("output[%zu]: buf=null\n", i);
            continue;
        }

        const float* p =
            reinterpret_cast<const float*>(outputs[i].buf);

        uint32_t n = attrs[i].n_elems;

        float mn = std::numeric_limits<float>::infinity();
        float mx = -std::numeric_limits<float>::infinity();
        double sum = 0.0;
        uint32_t finite_count = 0;

        for (uint32_t j = 0; j < n; ++j) {
            float v = p[j];

            if (std::isfinite(v)) {
                mn = std::min(mn, v);
                mx = std::max(mx, v);
                sum += v;
                ++finite_count;
            }
        }

        double mean =
            finite_count ? sum / static_cast<double>(finite_count) : 0.0;

        printf("output[%zu] name=%s n=%u min=%g max=%g mean=%g first=[",
               i, attrs[i].name, n, mn, mx, mean);

        uint32_t m = std::min(n, first_count);

        for (uint32_t j = 0; j < m; ++j) {
            printf("%g", p[j]);
            if (j + 1 < m) printf(", ");
        }

        printf("]\n");
    }

    printf("============================================================\n\n");
}

// ============================================================================
// Model loading
// ============================================================================

static bool load_rknn_model(const char* path, rknn_context& ctx) {
    FILE* fp = fopen(path, "rb");

    if (!fp) {
        printf("ERROR: cannot open model: %s\n", path);
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long model_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (model_size <= 0) {
        printf("ERROR: invalid model size: %s\n", path);
        fclose(fp);
        return false;
    }

    std::vector<unsigned char> data(
        static_cast<size_t>(model_size));

    size_t got =
        fread(data.data(), 1, data.size(), fp);

    fclose(fp);

    if (got != data.size()) {
        printf("ERROR: failed to read model completely: %s\n", path);
        return false;
    }

    int ret = rknn_init(
        &ctx,
        data.data(),
        static_cast<uint32_t>(data.size()),
        0,
        nullptr);

    if (ret != RKNN_SUCC) {
        printf("ERROR: rknn_init failed for %s, ret=%d\n",
               path, ret);
        return false;
    }

    return true;
}

// ============================================================================
// Geometry / NMS
// ============================================================================

static float iou(const FaceBox& a, const FaceBox& b) {
    float inter_x1 = std::max(a.x1, b.x1);
    float inter_y1 = std::max(a.y1, b.y1);
    float inter_x2 = std::min(a.x2, b.x2);
    float inter_y2 = std::min(a.y2, b.y2);

    float iw = std::max(0.0f, inter_x2 - inter_x1);
    float ih = std::max(0.0f, inter_y2 - inter_y1);

    float inter_area = iw * ih;

    float area_a =
        std::max(0.0f, a.x2 - a.x1) *
        std::max(0.0f, a.y2 - a.y1);

    float area_b =
        std::max(0.0f, b.x2 - b.x1) *
        std::max(0.0f, b.y2 - b.y1);

    float denom = area_a + area_b - inter_area;

    return denom > 0.0f ? inter_area / denom : 0.0f;
}

static std::vector<FaceBox> nms(
        std::vector<FaceBox> boxes,
        float threshold) {

    std::sort(
        boxes.begin(),
        boxes.end(),
        [](const FaceBox& a, const FaceBox& b) {
            return a.score > b.score;
        });

    std::vector<FaceBox> result;
    std::vector<bool> suppressed(boxes.size(), false);

    for (size_t i = 0; i < boxes.size(); ++i) {
        if (suppressed[i])
            continue;

        result.push_back(boxes[i]);

        for (size_t j = i + 1; j < boxes.size(); ++j) {
            if (suppressed[j])
                continue;

            if (iou(boxes[i], boxes[j]) > threshold) {
                suppressed[j] = true;
            }
        }
    }

    return result;
}

// ============================================================================
// Umeyama Similarity Transform
//
// 复现 skimage SimilarityTransform 的核心估计方式。
// src / dst: N x 2
//
// dst ~= scale * R * src + t
//
// 输出 M:
// [m00 m01 tx]
// [m10 m11 ty]
// ============================================================================

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

static cv::Mat align_face_insightface(
        const cv::Mat& img,
        const float landmarks[5][2]) {

    cv::Mat M;

    double scale = 0.0;
    double angle_deg = 0.0;
    double rms = 0.0;

    printf("\n---------------- ALIGN DEBUG (UMEYAMA) ----------------\n");

    static const char* names[5] = {
        "left_eye",
        "right_eye",
        "nose",
        "left_mouth",
        "right_mouth"
    };

    printf("Source landmarks in ORIGINAL image coordinates:\n");

    for (int k = 0; k < 5; ++k) {
        printf("  kps[%d] %-11s = (%.3f, %.3f)\n",
               k,
               names[k],
               landmarks[k][0],
               landmarks[k][1]);
    }

    bool ok =
        estimate_similarity_umeyama(
            landmarks,
            M,
            &scale,
            &angle_deg,
            &rms);

    if (!ok) {
        printf("ERROR: Umeyama SimilarityTransform failed.\n");
        return cv::Mat();
    }

    printf("Umeyama M = [%.10f %.10f %.10f; %.10f %.10f %.10f]\n",
           M.at<double>(0, 0),
           M.at<double>(0, 1),
           M.at<double>(0, 2),
           M.at<double>(1, 0),
           M.at<double>(1, 1),
           M.at<double>(1, 2));

    printf("scale=%.10f, rotation_deg=%.6f, landmark_RMS_error=%.6f px\n",
           scale,
           angle_deg,
           rms);

    printf("----------------------------------------------------------\n\n");

    cv::Mat aligned;

    cv::warpAffine(
        img,
        aligned,
        M,
        cv::Size(REC_W, REC_H),
        cv::INTER_LINEAR,
        cv::BORDER_CONSTANT,
        cv::Scalar(0, 0, 0));

    return aligned;
}

// ============================================================================
// SCRFD preprocessing
//
// InsightFace SCRFD：
// - 保持宽高比
// - resize 后放在 640x640 左上角
// - 右侧/底部补 0
// ============================================================================

static DetPrep prepare_scrfd_input(const cv::Mat& img) {
    DetPrep p;

    float im_ratio =
        static_cast<float>(img.rows) /
        static_cast<float>(img.cols);

    float model_ratio =
        static_cast<float>(DET_H) /
        static_cast<float>(DET_W);

    if (im_ratio > model_ratio) {
        p.new_h = DET_H;

        p.new_w =
            static_cast<int>(
                static_cast<float>(p.new_h) / im_ratio);
    } else {
        p.new_w = DET_W;

        p.new_h =
            static_cast<int>(
                static_cast<float>(p.new_w) * im_ratio);
    }

    p.det_scale =
        static_cast<float>(p.new_h) /
        static_cast<float>(img.rows);

    cv::Mat resized;

    cv::resize(
        img,
        resized,
        cv::Size(p.new_w, p.new_h));

    p.canvas_bgr =
        cv::Mat::zeros(
            DET_H,
            DET_W,
            CV_8UC3);

    resized.copyTo(
        p.canvas_bgr(
            cv::Rect(
                0,
                0,
                p.new_w,
                p.new_h)));

    cv::cvtColor(
        p.canvas_bgr,
        p.canvas_rgb,
        cv::COLOR_BGR2RGB);

    if (!p.canvas_rgb.isContinuous()) {
        p.canvas_rgb = p.canvas_rgb.clone();
    }

    printf(
        "SCRFD preprocess: original=%dx%d -> resized=%dx%d "
        "-> canvas=%dx%d, det_scale=%.8f\n",
        img.cols,
        img.rows,
        p.new_w,
        p.new_h,
        DET_W,
        DET_H,
        p.det_scale);

    return p;
}

// ============================================================================
// SCRFD debug
// ============================================================================

static void debug_scrfd_scale_peak(
        const float* score_data,
        const float* bbox_data,
        const float* kps_data,
        int stride,
        int grid_h,
        int grid_w,
        int num_anchors,
        float det_scale,
        int score_idx,
        int bbox_idx,
        int kps_idx) {

    int total =
        grid_h *
        grid_w *
        num_anchors;

    if (total <= 0)
        return;

    int best_idx = 0;
    float best_score = score_data[0];

    for (int idx = 1; idx < total; ++idx) {
        if (score_data[idx] > best_score) {
            best_score = score_data[idx];
            best_idx = idx;
        }
    }

    int cell =
        best_idx / num_anchors;

    int a =
        best_idx % num_anchors;

    int i =
        cell / grid_w;

    int j =
        cell % grid_w;

    float cx =
        static_cast<float>(j * stride);

    float cy =
        static_cast<float>(i * stride);

    printf("\n[SCRFD DEBUG stride=%d]\n", stride);

    printf(
        "  mapping: score_out=%d bbox_out=%d kps_out=%d\n",
        score_idx,
        bbox_idx,
        kps_idx);

    printf(
        "  peak idx=%d -> grid(i=%d,j=%d), anchor=%d, score=%.8f\n",
        best_idx,
        i,
        j,
        a,
        best_score);

    printf(
        "  anchor center on 640 canvas = (%.3f, %.3f)\n",
        cx,
        cy);

    const float* b =
        bbox_data +
        best_idx * 4;

    printf(
        "  raw bbox distance = [l=%g, t=%g, r=%g, b=%g]\n",
        b[0],
        b[1],
        b[2],
        b[3]);

    printf(
        "  decoded bbox on 640 canvas = [%.3f, %.3f, %.3f, %.3f]\n",
        cx - b[0] * stride,
        cy - b[1] * stride,
        cx + b[2] * stride,
        cy + b[3] * stride);

    printf(
        "  decoded bbox on original   = [%.3f, %.3f, %.3f, %.3f]\n",
        (cx - b[0] * stride) / det_scale,
        (cy - b[1] * stride) / det_scale,
        (cx + b[2] * stride) / det_scale,
        (cy + b[3] * stride) / det_scale);

    const float* k =
        kps_data +
        best_idx * 10;

    printf("  raw kps 10 values = [");

    for (int n = 0; n < 10; ++n) {
        printf("%g", k[n]);
        if (n != 9)
            printf(", ");
    }

    printf("]\n");

    static const char* names[5] = {
        "left_eye",
        "right_eye",
        "nose",
        "left_mouth",
        "right_mouth"
    };

    for (int n = 0; n < 5; ++n) {
        float dx =
            k[2 * n + 0];

        float dy =
            k[2 * n + 1];

        float x640 =
            cx + dx * stride;

        float y640 =
            cy + dy * stride;

        printf(
            "    kps[%d] %-11s raw=(%g,%g) "
            "canvas=(%.3f,%.3f) original=(%.3f,%.3f)\n",
            n,
            names[n],
            dx,
            dy,
            x640,
            y640,
            x640 / det_scale,
            y640 / det_scale);
    }
}

// ============================================================================
// SCRFD parsing
// ============================================================================

static std::vector<FaceBox> parse_scrfd_outputs(
        rknn_output* outputs,
        const std::vector<rknn_tensor_attr>& attrs,
        float det_scale,
        float score_threshold = 0.5f,
        bool print_debug = true) {

    std::vector<FaceBox> boxes;

    if (attrs.size() != 9) {
        printf(
            "ERROR: this parser requires exactly 9 SCRFD outputs.\n");

        return boxes;
    }

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

    for (int s = 0; s < 3; ++s) {
        const ScaleInfo& sc =
            scales[s];

        float* score_data =
            reinterpret_cast<float*>(
                outputs[sc.score_idx].buf);

        float* bbox_data =
            reinterpret_cast<float*>(
                outputs[sc.bbox_idx].buf);

        float* kps_data =
            reinterpret_cast<float*>(
                outputs[sc.kps_idx].buf);

        if (!score_data ||
            !bbox_data ||
            !kps_data) {

            printf(
                "ERROR: null detector output at stride=%d\n",
                sc.stride);

            continue;
        }

        uint32_t expected_score =
            static_cast<uint32_t>(
                sc.grid_h *
                sc.grid_w *
                sc.num_anchors);

        uint32_t expected_bbox =
            expected_score * 4;

        uint32_t expected_kps =
            expected_score * 10;

        if (attrs[sc.score_idx].n_elems != expected_score ||
            attrs[sc.bbox_idx].n_elems != expected_bbox ||
            attrs[sc.kps_idx].n_elems != expected_kps) {

            printf(
                "ERROR: unexpected tensor size at stride=%d\n",
                sc.stride);

            return std::vector<FaceBox>();
        }

        if (print_debug) {
            debug_scrfd_scale_peak(
                score_data,
                bbox_data,
                kps_data,
                sc.stride,
                sc.grid_h,
                sc.grid_w,
                sc.num_anchors,
                det_scale,
                sc.score_idx,
                sc.bbox_idx,
                sc.kps_idx);
        }

        for (int i = 0; i < sc.grid_h; ++i) {
            for (int j = 0; j < sc.grid_w; ++j) {
                for (int a = 0;
                     a < sc.num_anchors;
                     ++a) {

                    int idx =
                        (i * sc.grid_w + j) *
                        sc.num_anchors +
                        a;

                    float score =
                        score_data[idx];

                    if (!std::isfinite(score) ||
                        score < score_threshold) {
                        continue;
                    }

                    // 官方 SCRFD anchor center
                    float cx =
                        static_cast<float>(
                            j * sc.stride);

                    float cy =
                        static_cast<float>(
                            i * sc.stride);

                    FaceBox box;

                    box.score =
                        score;

                    float left =
                        bbox_data[idx * 4 + 0] *
                        sc.stride;

                    float top =
                        bbox_data[idx * 4 + 1] *
                        sc.stride;

                    float right =
                        bbox_data[idx * 4 + 2] *
                        sc.stride;

                    float bottom =
                        bbox_data[idx * 4 + 3] *
                        sc.stride;

                    // canvas -> original
                    box.x1 =
                        (cx - left) /
                        det_scale;

                    box.y1 =
                        (cy - top) /
                        det_scale;

                    box.x2 =
                        (cx + right) /
                        det_scale;

                    box.y2 =
                        (cy + bottom) /
                        det_scale;

                    // SCRFD KPS:
                    // [dx0,dy0, dx1,dy1, dx2,dy2, dx3,dy3, dx4,dy4]
                    for (int k = 0; k < 5; ++k) {
                        float dx =
                            kps_data[
                                idx * 10 +
                                2 * k + 0];

                        float dy =
                            kps_data[
                                idx * 10 +
                                2 * k + 1];

                        box.landmarks[k][0] =
                            (cx + dx * sc.stride) /
                            det_scale;

                        box.landmarks[k][1] =
                            (cy + dy * sc.stride) /
                            det_scale;
                    }

                    boxes.push_back(box);
                }
            }
        }
    }

    printf(
        "\nSCRFD parser: candidates above threshold %.3f = %zu\n",
        score_threshold,
        boxes.size());

    return boxes;
}

// ============================================================================
// Debug visualization
// ============================================================================

static void draw_face_debug(
        cv::Mat& img,
        const std::vector<FaceBox>& faces,
        const std::string& path) {

    const cv::Scalar point_colors[5] = {
        cv::Scalar(0,   0,   255),
        cv::Scalar(0,   255, 0),
        cv::Scalar(255, 0,   0),
        cv::Scalar(0,   255, 255),
        cv::Scalar(255, 0,   255)
    };

    for (size_t i = 0; i < faces.size(); ++i) {
        const FaceBox& f =
            faces[i];

        cv::rectangle(
            img,
            cv::Point(
                static_cast<int>(std::round(f.x1)),
                static_cast<int>(std::round(f.y1))),
            cv::Point(
                static_cast<int>(std::round(f.x2)),
                static_cast<int>(std::round(f.y2))),
            cv::Scalar(255, 255, 0),
            2);

        char text[64];

        snprintf(
            text,
            sizeof(text),
            "id=%zu score=%.3f",
            i,
            f.score);

        cv::putText(
            img,
            text,
            cv::Point(
                static_cast<int>(f.x1),
                std::max(
                    15,
                    static_cast<int>(f.y1) - 5)),
            cv::FONT_HERSHEY_SIMPLEX,
            0.45,
            cv::Scalar(255, 255, 0),
            1);

        for (int k = 0; k < 5; ++k) {
            cv::Point p(
                static_cast<int>(
                    std::round(
                        f.landmarks[k][0])),
                static_cast<int>(
                    std::round(
                        f.landmarks[k][1])));

            cv::circle(
                img,
                p,
                4,
                point_colors[k],
                -1);

            char ktxt[8];

            snprintf(
                ktxt,
                sizeof(ktxt),
                "%d",
                k);

            cv::putText(
                img,
                ktxt,
                p + cv::Point(5, -5),
                cv::FONT_HERSHEY_SIMPLEX,
                0.45,
                point_colors[k],
                1);
        }
    }

    if (cv::imwrite(path, img)) {
        printf(
            "Saved landmark debug image: %s\n",
            path.c_str());
    } else {
        printf(
            "WARNING: failed to save debug image: %s\n",
            path.c_str());
    }
}

// ============================================================================
// Detector execution
// ============================================================================

static std::vector<FaceBox> detect_faces(
        rknn_context det_ctx,
        const cv::Mat& img,
        const std::vector<rknn_tensor_attr>& det_output_attrs,
        uint32_t det_n_output,
        const char* debug_tag,
        float score_threshold = 0.5f,
        float nms_threshold = 0.4f) {

    std::vector<FaceBox> empty;

    DetPrep prep =
        prepare_scrfd_input(img);

    std::string det_input_path =
        std::string(debug_tag) +
        "_det_input_640.jpg";

    cv::imwrite(
        det_input_path,
        prep.canvas_bgr);

    printf(
        "Saved SCRFD input canvas: %s\n",
        det_input_path.c_str());

    std::vector<float> det_f32(
        DET_W *
        DET_H *
        3);

    const float det_norm_scale =
        1.0f / 128.0f;

    for (size_t i = 0;
         i < det_f32.size();
         ++i) {

        det_f32[i] =
            (static_cast<float>(
                prep.canvas_rgb.data[i]) -
             127.5f) *
            det_norm_scale;
    }

    rknn_input input;

    memset(
        &input,
        0,
        sizeof(input));

    input.index = 0;
    input.type = RKNN_TENSOR_FLOAT32;
    input.size =
        det_f32.size() *
        sizeof(float);

    input.fmt =
        RKNN_TENSOR_NHWC;

    input.buf =
        det_f32.data();

    input.pass_through = 0;

    int ret =
        rknn_inputs_set(
            det_ctx,
            1,
            &input);

    if (ret != RKNN_SUCC) {
        printf(
            "ERROR: rknn_inputs_set(det) failed ret=%d\n",
            ret);

        return empty;
    }

    ret =
        rknn_run(
            det_ctx,
            nullptr);

    if (ret != RKNN_SUCC) {
        printf(
            "ERROR: rknn_run(det) failed ret=%d\n",
            ret);

        return empty;
    }

    std::vector<rknn_output> outputs(
        det_n_output);

    memset(
        outputs.data(),
        0,
        outputs.size() *
        sizeof(rknn_output));

    for (uint32_t i = 0;
         i < det_n_output;
         ++i) {

        outputs[i].index = i;
        outputs[i].want_float = 1;
    }

    ret =
        rknn_outputs_get(
            det_ctx,
            det_n_output,
            outputs.data(),
            nullptr);

    if (ret != RKNN_SUCC) {
        printf(
            "ERROR: rknn_outputs_get(det) failed ret=%d\n",
            ret);

        return empty;
    }

    dump_output_stats(
        debug_tag,
        outputs.data(),
        det_output_attrs);

    std::vector<FaceBox> faces =
        parse_scrfd_outputs(
            outputs.data(),
            det_output_attrs,
            prep.det_scale,
            score_threshold,
            true);

    rknn_outputs_release(
        det_ctx,
        det_n_output,
        outputs.data());

    faces =
        nms(
            faces,
            nms_threshold);

    printf(
        "SCRFD after NMS %.3f: %zu face(s)\n",
        nms_threshold,
        faces.size());

    cv::Mat vis =
        img.clone();

    std::string landmark_path =
        std::string(debug_tag) +
        "_landmarks.jpg";

    draw_face_debug(
        vis,
        faces,
        landmark_path);

    return faces;
}

// ============================================================================
// ArcFace
// ============================================================================

static void l2_normalize(
        float* feat,
        int dim) {

    double norm2 = 0.0;

    for (int i = 0; i < dim; ++i) {
        norm2 +=
            static_cast<double>(feat[i]) *
            feat[i];
    }

    double norm =
        std::sqrt(norm2);

    if (norm > 0.0) {
        for (int i = 0; i < dim; ++i) {
            feat[i] =
                static_cast<float>(
                    feat[i] / norm);
        }
    }
}

static float cosine_similarity(
        const float* feat1,
        const float* feat2,
        int dim) {

    double dot = 0.0;

    for (int i = 0; i < dim; ++i) {
        dot +=
            static_cast<double>(feat1[i]) *
            feat2[i];
    }

    return static_cast<float>(dot);
}

static std::vector<float> extract_feature(
        rknn_context rec_ctx,
        const cv::Mat& img,
        const FaceBox& face,
        const char* debug_path = nullptr) {

    std::vector<float> empty;

    cv::Mat aligned =
        align_face_insightface(
            img,
            face.landmarks);

    if (aligned.empty()) {
        printf(
            "ERROR: aligned face is empty.\n");

        return empty;
    }

    if (debug_path) {
        cv::imwrite(
            debug_path,
            aligned);

        printf(
            "Saved aligned face: %s\n",
            debug_path);
    }

    cv::Mat rec_rgb;

    cv::cvtColor(
        aligned,
        rec_rgb,
        cv::COLOR_BGR2RGB);

    if (!rec_rgb.isContinuous()) {
        rec_rgb =
            rec_rgb.clone();
    }

    std::vector<float> rec_f32(
        REC_W *
        REC_H *
        3);

    const float rec_norm_scale =
        1.0f / 127.5f;

    for (size_t i = 0;
         i < rec_f32.size();
         ++i) {

        rec_f32[i] =
            (static_cast<float>(
                rec_rgb.data[i]) -
             127.5f) *
            rec_norm_scale;
    }

    rknn_input input;

    memset(
        &input,
        0,
        sizeof(input));

    input.index = 0;
    input.type = RKNN_TENSOR_FLOAT32;
    input.size =
        rec_f32.size() *
        sizeof(float);

    input.fmt =
        RKNN_TENSOR_NHWC;

    input.buf =
        rec_f32.data();

    input.pass_through = 0;

    int ret =
        rknn_inputs_set(
            rec_ctx,
            1,
            &input);

    if (ret != RKNN_SUCC) {
        printf(
            "ERROR: rknn_inputs_set(rec) failed ret=%d\n",
            ret);

        return empty;
    }

    ret =
        rknn_run(
            rec_ctx,
            nullptr);

    if (ret != RKNN_SUCC) {
        printf(
            "ERROR: rknn_run(rec) failed ret=%d\n",
            ret);

        return empty;
    }

    rknn_output output;

    memset(
        &output,
        0,
        sizeof(output));

    output.index = 0;
    output.want_float = 1;

    ret =
        rknn_outputs_get(
            rec_ctx,
            1,
            &output,
            nullptr);

    if (ret != RKNN_SUCC ||
        !output.buf) {

        printf(
            "ERROR: rknn_outputs_get(rec) failed ret=%d\n",
            ret);

        return empty;
    }

    float* feature =
        reinterpret_cast<float*>(
            output.buf);

    std::vector<float> feature_vec(
        feature,
        feature + REC_FEAT_DIM);

    double norm2 = 0.0;

    for (int i = 0;
         i < REC_FEAT_DIM;
         ++i) {

        norm2 +=
            static_cast<double>(
                feature_vec[i]) *
            feature_vec[i];
    }

    double raw_norm =
        std::sqrt(norm2);

    printf(
        "ArcFace raw feature L2 norm = %.8f\n",
        raw_norm);

    printf(
        "ArcFace raw feature first 10 = [");

    for (int i = 0; i < 10; ++i) {
        printf(
            "%g",
            feature_vec[i]);

        if (i != 9)
            printf(", ");
    }

    printf("]\n");

    l2_normalize(
        feature_vec.data(),
        REC_FEAT_DIM);

    rknn_outputs_release(
        rec_ctx,
        1,
        &output);

    return feature_vec;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    if (argc < 5) {
        printf("Usage:\n");
        printf(
            "  %s <det.rknn> <rec.rknn> "
            "<multi_faces.jpg> <single_face.jpg> "
            "[output.jpg] [similarity_threshold]\n",
            argv[0]);

        return -1;
    }

    const char* det_model_path =
        argv[1];

    const char* rec_model_path =
        argv[2];

    const char* multi_faces_path =
        argv[3];

    const char* single_face_path =
        argv[4];

    const char* output_path =
        (argc > 5)
        ? argv[5]
        : "comparison_result.jpg";

    float similarity_threshold =
        (argc > 6)
        ? static_cast<float>(
            atof(argv[6]))
        : 0.5f;

    printf(
        "Similarity threshold = %.4f\n",
        similarity_threshold);

    rknn_context det_ctx = 0;
    rknn_context rec_ctx = 0;

    if (!load_rknn_model(
            det_model_path,
            det_ctx)) {
        return -1;
    }

    if (!load_rknn_model(
            rec_model_path,
            rec_ctx)) {

        rknn_destroy(det_ctx);
        return -1;
    }

    rknn_input_output_num det_io;
    rknn_input_output_num rec_io;

    std::vector<rknn_tensor_attr> det_inputs;
    std::vector<rknn_tensor_attr> det_outputs_attr;

    std::vector<rknn_tensor_attr> rec_inputs;
    std::vector<rknn_tensor_attr> rec_outputs_attr;

    if (!query_model_io(
            det_ctx,
            "DETECTOR",
            det_io,
            det_inputs,
            det_outputs_attr)) {

        rknn_destroy(det_ctx);
        rknn_destroy(rec_ctx);

        return -1;
    }

    if (!validate_scrfd_9_outputs(
            det_outputs_attr)) {

        printf(
            "ERROR: SCRFD tensor layout is not the validated 9-output layout.\n");

        rknn_destroy(det_ctx);
        rknn_destroy(rec_ctx);

        return -1;
    }

    if (!query_model_io(
            rec_ctx,
            "RECOGNITION",
            rec_io,
            rec_inputs,
            rec_outputs_attr)) {

        rknn_destroy(det_ctx);
        rknn_destroy(rec_ctx);

        return -1;
    }

    if (det_io.n_output != 9) {
        printf(
            "ERROR: detector n_output=%u, expected 9.\n",
            det_io.n_output);

        rknn_destroy(det_ctx);
        rknn_destroy(rec_ctx);

        return -1;
    }

    if (rec_io.n_output != 1 ||
        rec_outputs_attr.empty() ||
        rec_outputs_attr[0].n_elems != REC_FEAT_DIM) {

        printf(
            "ERROR: recognition model output must be one 512-D tensor.\n");

        rknn_destroy(det_ctx);
        rknn_destroy(rec_ctx);

        return -1;
    }

    // ------------------------------------------------------------------------
    // Target face
    // ------------------------------------------------------------------------

    printf(
        "\n\n################ TARGET FACE ################\n");

    printf(
        "Image: %s\n",
        single_face_path);

    cv::Mat single_img =
        cv::imread(
            single_face_path);

    if (single_img.empty()) {
        printf(
            "ERROR: failed to read target image.\n");

        rknn_destroy(det_ctx);
        rknn_destroy(rec_ctx);

        return -1;
    }

    std::vector<FaceBox> target_faces =
        detect_faces(
            det_ctx,
            single_img,
            det_outputs_attr,
            det_io.n_output,
            "target",
            0.5f,
            0.4f);

    if (target_faces.empty()) {
        printf(
            "ERROR: no face detected in target image.\n");

        rknn_destroy(det_ctx);
        rknn_destroy(rec_ctx);

        return -1;
    }

    printf(
        "Target detected faces=%zu; "
        "using highest-score face after NMS.\n",
        target_faces.size());

    std::vector<float> target_feature =
        extract_feature(
            rec_ctx,
            single_img,
            target_faces[0],
            "target_aligned.jpg");

    if (target_feature.size() != REC_FEAT_DIM) {
        printf(
            "ERROR: target feature extraction failed.\n");

        rknn_destroy(det_ctx);
        rknn_destroy(rec_ctx);

        return -1;
    }

    // ------------------------------------------------------------------------
    // Multi-face image
    // ------------------------------------------------------------------------

    printf(
        "\n\n################ MULTI FACE ################\n");

    printf(
        "Image: %s\n",
        multi_faces_path);

    cv::Mat multi_img =
        cv::imread(
            multi_faces_path);

    if (multi_img.empty()) {
        printf(
            "ERROR: failed to read multi-face image.\n");

        rknn_destroy(det_ctx);
        rknn_destroy(rec_ctx);

        return -1;
    }

    std::vector<FaceBox> multi_faces =
        detect_faces(
            det_ctx,
            multi_img,
            det_outputs_attr,
            det_io.n_output,
            "multi",
            0.5f,
            0.4f);

    printf(
        "\nComputing similarities:\n");

    printf(
        "%-8s %-12s %-10s\n",
        "Face ID",
        "Similarity",
        "Match");

    printf(
        "------------------------------------\n");

    cv::Mat result_img =
        multi_img.clone();

    int match_count = 0;

    for (size_t i = 0;
         i < multi_faces.size();
         ++i) {

        char aligned_path[128];

        snprintf(
            aligned_path,
            sizeof(aligned_path),
            "face_%zu_aligned.jpg",
            i);

        std::vector<float> feature =
            extract_feature(
                rec_ctx,
                multi_img,
                multi_faces[i],
                aligned_path);

        if (feature.size() != REC_FEAT_DIM) {
            printf(
                "%-8zu %-12s %-10s\n",
                i,
                "ERROR",
                "NO");

            continue;
        }

        float similarity =
            cosine_similarity(
                target_feature.data(),
                feature.data(),
                REC_FEAT_DIM);

        bool is_match =
            similarity >
            similarity_threshold;

        if (is_match)
            ++match_count;

        printf(
            "%-8zu %.6f     %s\n",
            i,
            similarity,
            is_match ? "YES" : "NO");

        cv::Scalar color =
            is_match
            ? cv::Scalar(0, 255, 0)
            : cv::Scalar(0, 0, 255);

        int thickness =
            is_match ? 3 : 2;

        cv::rectangle(
            result_img,
            cv::Point(
                static_cast<int>(
                    multi_faces[i].x1),
                static_cast<int>(
                    multi_faces[i].y1)),
            cv::Point(
                static_cast<int>(
                    multi_faces[i].x2),
                static_cast<int>(
                    multi_faces[i].y2)),
            color,
            thickness);

        for (int k = 0; k < 5; ++k) {
            cv::circle(
                result_img,
                cv::Point(
                    static_cast<int>(
                        multi_faces[i].landmarks[k][0]),
                    static_cast<int>(
                        multi_faces[i].landmarks[k][1])),
                3,
                cv::Scalar(255, 0, 0),
                -1);
        }

        char label[64];

        if (is_match) {
            snprintf(
                label,
                sizeof(label),
                "MATCH %.3f",
                similarity);
        } else {
            snprintf(
                label,
                sizeof(label),
                "%.3f",
                similarity);
        }

        int baseline = 0;

        cv::Size text_size =
            cv::getTextSize(
                label,
                cv::FONT_HERSHEY_SIMPLEX,
                0.7,
                2,
                &baseline);

        int x =
            std::max(
                0,
                static_cast<int>(
                    multi_faces[i].x1));

        int y =
            std::max(
                text_size.height + 10,
                static_cast<int>(
                    multi_faces[i].y1));

        cv::rectangle(
            result_img,
            cv::Point(
                x,
                y - text_size.height - 10),
            cv::Point(
                x + text_size.width + 10,
                y),
            color,
            -1);

        cv::putText(
            result_img,
            label,
            cv::Point(
                x + 5,
                y - 5),
            cv::FONT_HERSHEY_SIMPLEX,
            0.7,
            cv::Scalar(255, 255, 255),
            2);
    }

    if (!cv::imwrite(
            output_path,
            result_img)) {

        printf(
            "WARNING: failed to save result image: %s\n",
            output_path);
    }

    printf(
        "\n========================================\n");

    printf(
        "Summary:\n");

    printf(
        "  Total faces: %zu\n",
        multi_faces.size());

    printf(
        "  Matched faces (similarity > %.3f): %d\n",
        similarity_threshold,
        match_count);

    printf(
        "  Result saved to: %s\n",
        output_path);

    printf(
        "\nDebug images created:\n");

    printf(
        "  target_det_input_640.jpg\n");

    printf(
        "  target_landmarks.jpg\n");

    printf(
        "  target_aligned.jpg\n");

    printf(
        "  multi_det_input_640.jpg\n");

    printf(
        "  multi_landmarks.jpg\n");

    printf(
        "  face_N_aligned.jpg\n");

    printf(
        "========================================\n");

    rknn_destroy(det_ctx);
    rknn_destroy(rec_ctx);

    return 0;
}
