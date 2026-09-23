#include <stdio.h>
#include <opencv2/opencv.hpp>
#include "rknn_api.h"

int main() {
    // 加载检测模型
    FILE* fp = fopen("detection.rknn", "rb");
    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    void* data = malloc(size);
    fread(data, 1, size, fp);
    fclose(fp);
    
    rknn_context ctx;
    rknn_init(&ctx, data, size, 0, nullptr);
    free(data);
    
    // 读取图片
    cv::Mat img = cv::imread("test.jpg");
    cv::Mat det_input;
    cv::resize(img, det_input, cv::Size(640, 640));
    cv::cvtColor(det_input, det_input, cv::COLOR_BGR2RGB);
    
    std::vector<float> det_f32(640 * 640 * 3);
    for (size_t i = 0; i < det_input.total() * 3; i++) {
        det_f32[i] = (static_cast<float>(det_input.data[i]) - 127.5f) / 128.0f;
    }
    
    rknn_input inputs[1] = {};
    inputs[0].type = RKNN_TENSOR_FLOAT32;
    inputs[0].size = det_f32.size() * sizeof(float);
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].buf = det_f32.data();
    
    rknn_inputs_set(ctx, 1, inputs);
    rknn_run(ctx, nullptr);
    
    rknn_output outputs[9] = {};
    for (int i = 0; i < 9; i++) outputs[i].want_float = 1;
    rknn_outputs_get(ctx, 9, outputs, nullptr);
    
    // 找一个高分检测框
    float* score_data = (float*)outputs[0].buf;  // stride 8
    float* bbox_data = (float*)outputs[3].buf;
    float* kps_data = (float*)outputs[6].buf;
    
    int best_idx = -1;
    float best_score = 0;
    for (int i = 0; i < 12800; i++) {
        if (score_data[i] > best_score) {
            best_score = score_data[i];
            best_idx = i;
        }
    }
    
    printf("Best detection at idx=%d, score=%.4f\n\n", best_idx, best_score);
    
    // 打印原始关键点数据
    printf("Raw keypoint data (10 values):\n");
    for (int i = 0; i < 10; i++) {
        printf("  kps[%d] = %.4f\n", i, kps_data[best_idx * 10 + i]);
    }
    printf("\n");
    
    // 尝试3种解析方式
    int gy = best_idx / 80;
    int gx = best_idx % 80 / 2;  // 因为每格2个anchor
    float cx = (gx + 0.5f) * 8;
    float cy = (gy + 0.5f) * 8;
    
    printf("Anchor center: (%.1f, %.1f)\n\n", cx, cy);
    
    float scale_x = img.cols / 640.0f;
    float scale_y = img.rows / 640.0f;
    
    // 方式1: [x0,y0,x1,y1,...] 交错
    printf("Method 1: [x0,y0,x1,y1,...] interleaved\n");
    for (int k = 0; k < 5; k++) {
        float kx = kps_data[best_idx * 10 + k * 2 + 0] * 8;
        float ky = kps_data[best_idx * 10 + k * 2 + 1] * 8;
        float x = (cx + kx) * scale_x;
        float y = (cy + ky) * scale_y;
        printf("  Point %d: (%.1f, %.1f)\n", k, x, y);
    }
    printf("\n");
    
    // 方式2: [x0,x1,x2,x3,x4, y0,y1,y2,y3,y4]
    printf("Method 2: [x0,x1,...,x4, y0,y1,...,y4] grouped\n");
    for (int k = 0; k < 5; k++) {
        float kx = kps_data[best_idx * 10 + k] * 8;
        float ky = kps_data[best_idx * 10 + k + 5] * 8;
        float x = (cx + kx) * scale_x;
        float y = (cy + ky) * scale_y;
        printf("  Point %d: (%.1f, %.1f)\n", k, x, y);
    }
    printf("\n");
    
    // 方式3: 绝对坐标（0-1归一化）
    printf("Method 3: Absolute normalized coordinates\n");
    for (int k = 0; k < 5; k++) {
        float kx = kps_data[best_idx * 10 + k];
        float ky = kps_data[best_idx * 10 + k + 5];
        float x = kx * img.cols;
        float y = ky * img.rows;
        printf("  Point %d: (%.1f, %.1f)\n", k, x, y);
    }
    
    rknn_outputs_release(ctx, 9, outputs);
    rknn_destroy(ctx);
    
    return 0;
}