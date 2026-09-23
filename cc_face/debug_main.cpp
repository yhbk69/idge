#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>
#include "rknn_api.h"

// 打印tensor属性
void print_tensor_attr(const char* prefix, rknn_tensor_attr* attr) {
    printf("%s:\n", prefix);
    printf("  index: %d\n", attr->index);
    printf("  name: %s\n", attr->name);
    printf("  n_dims: %d\n", attr->n_dims);
    printf("  dims: [");
    for (int i = 0; i < attr->n_dims; i++) {
        printf("%d", attr->dims[i]);
        if (i < attr->n_dims - 1) printf(", ");
    }
    printf("]\n");
    
    printf("  n_elems: %d\n", attr->n_elems);
    printf("  size: %d\n", attr->size);
    
    printf("  fmt: ");
    switch(attr->fmt) {
        case RKNN_TENSOR_NCHW: printf("NCHW\n"); break;
        case RKNN_TENSOR_NHWC: printf("NHWC\n"); break;
        case RKNN_TENSOR_NC1HWC2: printf("NC1HWC2\n"); break;
        default: printf("UNKNOWN(%d)\n", attr->fmt);
    }
    
    printf("  type: ");
    switch(attr->type) {
        case RKNN_TENSOR_FLOAT32: printf("FLOAT32\n"); break;
        case RKNN_TENSOR_FLOAT16: printf("FLOAT16\n"); break;
        case RKNN_TENSOR_INT8: printf("INT8\n"); break;
        case RKNN_TENSOR_UINT8: printf("UINT8\n"); break;
        case RKNN_TENSOR_INT16: printf("INT16\n"); break;
        case RKNN_TENSOR_INT32: printf("INT32\n"); break;
        case RKNN_TENSOR_INT64: printf("INT64\n"); break;
        default: printf("UNKNOWN(%d)\n", attr->type);
    }
    
    printf("  qnt_type: ");
    switch(attr->qnt_type) {
        case RKNN_TENSOR_QNT_NONE: printf("NONE\n"); break;
        case RKNN_TENSOR_QNT_DFP: printf("DFP\n"); break;
        case RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC: printf("AFFINE_ASYMMETRIC\n"); break;
        default: printf("UNKNOWN(%d)\n", attr->qnt_type);
    }
    
    if (attr->qnt_type == RKNN_TENSOR_QNT_DFP) {
        printf("  fl: %d\n", attr->fl);
    } else if (attr->qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) {
        printf("  scale: %f\n", attr->scale);
        printf("  zp: %d\n", attr->zp);
    }
    
    printf("\n");
}

// 分析float数据的统计信息
void analyze_float_data(const char* name, float* data, int count, int print_samples = 20) {
    if (count == 0) return;
    
    float min_val = data[0];
    float max_val = data[0];
    double sum = 0.0;
    int nan_count = 0;
    int inf_count = 0;
    
    for (int i = 0; i < count; i++) {
        float val = data[i];
        if (std::isnan(val)) {
            nan_count++;
            continue;
        }
        if (std::isinf(val)) {
            inf_count++;
            continue;
        }
        if (val < min_val) min_val = val;
        if (val > max_val) max_val = val;
        sum += val;
    }
    
    float mean = sum / count;
    
    printf("%s statistics:\n", name);
    printf("  count: %d\n", count);
    printf("  min: %.6f\n", min_val);
    printf("  max: %.6f\n", max_val);
    printf("  mean: %.6f\n", mean);
    printf("  nan_count: %d\n", nan_count);
    printf("  inf_count: %d\n", inf_count);
    
    printf("  first %d values: [", std::min(print_samples, count));
    for (int i = 0; i < std::min(print_samples, count); i++) {
        printf("%.4f", data[i]);
        if (i < std::min(print_samples, count) - 1) printf(", ");
    }
    printf("]\n\n");
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("Usage: %s <det_model.rknn> <rec_model.rknn> <image.jpg>\n", argv[0]);
        return -1;
    }
    
    const char* det_model_path = argv[1];
    const char* rec_model_path = argv[2];
    const char* image_path = argv[3];
    
    printf("=========================================\n");
    printf("LOADING DETECTION MODEL\n");
    printf("=========================================\n\n");
    
    // ========== 加载检测模型 ==========
    FILE* fp = fopen(det_model_path, "rb");
    if (!fp) {
        printf("Failed to open detection model\n");
        return -1;
    }
    
    fseek(fp, 0, SEEK_END);
    int det_model_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    void* det_model_data = malloc(det_model_size);
    fread(det_model_data, 1, det_model_size, fp);
    fclose(fp);
    
    printf("Detection model size: %d bytes\n\n", det_model_size);
    
    rknn_context det_ctx;
    int ret = rknn_init(&det_ctx, det_model_data, det_model_size, 0, nullptr);
    free(det_model_data);
    
    if (ret < 0) {
        printf("rknn_init detection model failed! ret=%d\n", ret);
        return -1;
    }
    
    // ========== 查询检测模型信息 ==========
    rknn_sdk_version version;
    ret = rknn_query(det_ctx, RKNN_QUERY_SDK_VERSION, &version, sizeof(version));
    if (ret == 0) {
        printf("RKNN SDK version: %s\n", version.api_version);
        printf("Driver version: %s\n\n", version.drv_version);
    }
    
    rknn_input_output_num det_io;
    ret = rknn_query(det_ctx, RKNN_QUERY_IN_OUT_NUM, &det_io, sizeof(det_io));
    if (ret < 0) {
        printf("rknn_query io_num failed!\n");
        return -1;
    }
    
    printf("Detection model I/O:\n");
    printf("  n_input: %d\n", det_io.n_input);
    printf("  n_output: %d\n\n", det_io.n_output);
    
    // ========== 检测模型输入信息 ==========
    printf("=========================================\n");
    printf("DETECTION MODEL INPUT INFO\n");
    printf("=========================================\n\n");
    
    rknn_tensor_attr det_input_attrs[det_io.n_input];
    memset(det_input_attrs, 0, sizeof(det_input_attrs));
    for (int i = 0; i < det_io.n_input; i++) {
        det_input_attrs[i].index = i;
        ret = rknn_query(det_ctx, RKNN_QUERY_INPUT_ATTR, &det_input_attrs[i], sizeof(rknn_tensor_attr));
        if (ret < 0) {
            printf("rknn_query input[%d] failed!\n", i);
            continue;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "Detection Input[%d]", i);
        print_tensor_attr(buf, &det_input_attrs[i]);
    }
    
    // ========== 检测模型输出信息 ==========
    printf("=========================================\n");
    printf("DETECTION MODEL OUTPUT INFO\n");
    printf("=========================================\n\n");
    
    rknn_tensor_attr det_output_attrs[det_io.n_output];
    memset(det_output_attrs, 0, sizeof(det_output_attrs));
    for (int i = 0; i < det_io.n_output; i++) {
        det_output_attrs[i].index = i;
        ret = rknn_query(det_ctx, RKNN_QUERY_OUTPUT_ATTR, &det_output_attrs[i], sizeof(rknn_tensor_attr));
        if (ret < 0) {
            printf("rknn_query output[%d] failed!\n", i);
            continue;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "Detection Output[%d]", i);
        print_tensor_attr(buf, &det_output_attrs[i]);
    }
    
    // ========== 读取图片 ==========
    printf("=========================================\n");
    printf("PROCESSING IMAGE\n");
    printf("=========================================\n\n");
    
    cv::Mat img = cv::imread(image_path);
    if (img.empty()) {
        printf("Failed to read image: %s\n", image_path);
        rknn_destroy(det_ctx);
        return -1;
    }
    
    printf("Image: %s\n", image_path);
    printf("Original size: %d x %d x %d\n\n", img.cols, img.rows, img.channels());
    
    // ========== 预处理 ==========
    int input_width = det_input_attrs[0].dims[2];
    int input_height = det_input_attrs[0].dims[1];
    
    printf("Resizing to: %d x %d\n", input_width, input_height);
    
    cv::Mat det_input;
    cv::resize(img, det_input, cv::Size(input_width, input_height));
    cv::cvtColor(det_input, det_input, cv::COLOR_BGR2RGB);
    
    printf("Input data size: %d bytes\n\n", det_input.total() * det_input.elemSize());
    
    // ========== 执行检测推理 ==========
    printf("=========================================\n");
    printf("RUNNING DETECTION INFERENCE\n");
    printf("=========================================\n\n");
    
    rknn_input det_inputs[1];
    memset(det_inputs, 0, sizeof(det_inputs));
    det_inputs[0].index = 0;
    det_inputs[0].type = RKNN_TENSOR_UINT8;
    det_inputs[0].size = input_width * input_height * 3;
    det_inputs[0].fmt = RKNN_TENSOR_NHWC;
    det_inputs[0].buf = det_input.data;
    
    ret = rknn_inputs_set(det_ctx, 1, det_inputs);
    if (ret < 0) {
        printf("rknn_inputs_set failed! ret=%d\n", ret);
        rknn_destroy(det_ctx);
        return -1;
    }
    
    printf("rknn_inputs_set: OK\n");
    
    ret = rknn_run(det_ctx, nullptr);
    if (ret < 0) {
        printf("rknn_run failed! ret=%d\n", ret);
        rknn_destroy(det_ctx);
        return -1;
    }
    
    printf("rknn_run: OK\n\n");
    
    // ========== 获取检测输出 ==========
    printf("=========================================\n");
    printf("DETECTION OUTPUT DATA ANALYSIS\n");
    printf("=========================================\n\n");
    
    rknn_output det_outputs[det_io.n_output];
    memset(det_outputs, 0, sizeof(det_outputs));
    for (int i = 0; i < det_io.n_output; i++) {
        det_outputs[i].want_float = 1;  // 要求转换为float
    }
    
    ret = rknn_outputs_get(det_ctx, det_io.n_output, det_outputs, nullptr);
    if (ret < 0) {
        printf("rknn_outputs_get failed! ret=%d\n", ret);
        rknn_destroy(det_ctx);
        return -1;
    }
    
    printf("rknn_outputs_get: OK\n\n");
    
    // 分析每个输出
    for (int i = 0; i < det_io.n_output; i++) {
        printf("--- Output[%d]: %s ---\n", i, det_output_attrs[i].name);
        printf("is_prealloc: %d\n", det_outputs[i].is_prealloc);
        printf("want_float: %d\n", det_outputs[i].want_float);
        printf("buf: %p\n", det_outputs[i].buf);
        printf("size: %zu\n\n", det_outputs[i].size);
        
        if (det_outputs[i].buf) {
            float* data = (float*)det_outputs[i].buf;
            int count = det_output_attrs[i].n_elems;
            
            char name[128];
            snprintf(name, sizeof(name), "Output[%d]-%s", i, det_output_attrs[i].name);
            analyze_float_data(name, data, count, 30);
        }
    }
    
    // ========== 清理 ==========
    rknn_outputs_release(det_ctx, det_io.n_output, det_outputs);
    rknn_destroy(det_ctx);
    
    printf("=========================================\n");
    printf("DETECTION DEBUG DONE\n");
    printf("=========================================\n\n");
    
    printf("请将上面的输出发给我，我会根据实际的tensor信息调整解析代码\n");
    
    return 0;
}