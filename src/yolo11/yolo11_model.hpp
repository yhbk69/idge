#ifndef _YOLO11_MODEL_H_
#define _YOLO11_MODEL_H_

#include <iostream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <cstring>
#include <set>
#ifdef Status
#undef Status
#endif
#include <opencv2/opencv.hpp>
#include "rknn_api.h"
#include "common.hpp"
#include "yolo_base_detector.hpp"
#include "file_utils.h"
#include "image_drawing.h"
#include "image_utils.h"
#include "rknn_api.h"

/**
 * @brief YOLOv11 detector implementation for Rockchip RK3588 platform
 */

class YOLO11Model : public YoloBaseDetector
{

private:
    rknn_app_context_t app_ctx;
    int numClasses = 0;
    int dflLen = 16;
    std::string modelPath;
    std::string labelsPath;
    std::vector<std::string> classNames_;
    // 配置
    PreprocessType preprocessType_;

public:
    YOLO11Model(const std::string &modelPath,
                const std::string &labelsPath,
                rknn_core_mask core_mask,
                int numClasses = 80,
                int dflLen = 16)
        : dflLen(dflLen), modelPath(modelPath), labelsPath(labelsPath), numClasses(numClasses),
          preprocessType_(PreprocessType::LETTERBOX)
    {
        // 加载类别名称
        classNames_ = loadClassNames(labelsPath);

        init_yolo11_model(modelPath.c_str(), &this->app_ctx, core_mask);
    }

    int init_yolo11_model(const char *model_path, rknn_app_context_t *app_ctx, rknn_core_mask core_mask)
    {
        int ret;
        int model_len = 0;
        char *model;
        rknn_context ctx = 0;

        memset(app_ctx, 0, sizeof(rknn_app_context_t));
        // Load RKNN Model
        model_len = read_data_from_file(model_path, &model);
        if (model == NULL)
        {
            printf("load_model fail!\n");
            return -1;
        }

        ret = rknn_init(&ctx, model, model_len, 0, NULL);
        free(model);
        if (ret < 0)
        {
            printf("rknn_init fail! ret=%d\n", ret);
            return -1;
        }

        if (core_mask)
        {
            ret = rknn_set_core_mask(ctx, core_mask);
            if (ret < 0)
            {
                printf("rknn_set_core_mask error! ret=%d\n", ret);
            }
        }

        // Get Model Input Output Number
        rknn_input_output_num io_num;
        ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
        if (ret != RKNN_SUCC)
        {
            printf("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
        printf("model input num: %d, output num: %d\n", io_num.n_input, io_num.n_output);

        // Get Model Input Info
        printf("input tensors:\n");
        rknn_tensor_attr input_native_attrs[io_num.n_input];
        memset(input_native_attrs, 0, sizeof(input_native_attrs));
        for (int i = 0; i < io_num.n_input; i++)
        {
            input_native_attrs[i].index = i;
            ret = rknn_query(ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &(input_native_attrs[i]), sizeof(rknn_tensor_attr));
            if (ret != RKNN_SUCC)
            {
                printf("rknn_query fail! ret=%d\n", ret);
                return -1;
            }
            dump_tensor_attr(&(input_native_attrs[i]));
        }

        // default input type is int8 (normalize and quantize need compute in outside)
        // if set uint8, will fuse normalize and quantize to npu
        input_native_attrs[0].type = RKNN_TENSOR_UINT8;
        app_ctx->input_mems[0] = rknn_create_mem(ctx, input_native_attrs[0].size_with_stride);

        // Set input tensor memory
        ret = rknn_set_io_mem(ctx, app_ctx->input_mems[0], &input_native_attrs[0]);
        if (ret < 0)
        {
            printf("input_mems rknn_set_io_mem fail! ret=%d\n", ret);
            return -1;
        }

        // Get Model Output Info
        printf("output tensors:\n");
        rknn_tensor_attr output_native_attrs[io_num.n_output];
        memset(output_native_attrs, 0, sizeof(output_native_attrs));
        for (int i = 0; i < io_num.n_output; i++)
        {
            output_native_attrs[i].index = i;
            ret = rknn_query(ctx, RKNN_QUERY_NATIVE_OUTPUT_ATTR, &(output_native_attrs[i]), sizeof(rknn_tensor_attr));
            if (ret != RKNN_SUCC)
            {
                printf("rknn_query fail! ret=%d\n", ret);
                return -1;
            }
            dump_tensor_attr(&(output_native_attrs[i]));
        }

        // Set output tensor memory
        for (uint32_t i = 0; i < io_num.n_output; ++i)
        {
            app_ctx->output_mems[i] = rknn_create_mem(ctx, output_native_attrs[i].size_with_stride);
            ret = rknn_set_io_mem(ctx, app_ctx->output_mems[i], &output_native_attrs[i]);
            if (ret < 0)
            {
                printf("output_mems rknn_set_io_mem fail! ret=%d\n", ret);
                return -1;
            }
        }

        // Set to context
        app_ctx->rknn_ctx = ctx;

        // TODO
        if (output_native_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC && output_native_attrs[0].type == RKNN_TENSOR_INT8)
        {
            app_ctx->is_quant = true;
        }
        else
        {
            app_ctx->is_quant = false;
        }

        rknn_tensor_attr input_attrs[io_num.n_input];
        memset(input_attrs, 0, sizeof(input_attrs));
        for (int i = 0; i < io_num.n_input; i++)
        {
            input_attrs[i].index = i;
            ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
            if (ret != RKNN_SUCC)
            {
                printf("rknn_query fail! ret=%d\n", ret);
                return -1;
            }
        }

        rknn_tensor_attr output_attrs[io_num.n_output];
        memset(output_attrs, 0, sizeof(output_attrs));
        for (int i = 0; i < io_num.n_output; i++)
        {
            output_attrs[i].index = i;
            ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
            if (ret != RKNN_SUCC)
            {
                printf("rknn_query fail! ret=%d\n", ret);
                return -1;
            }
        }

        app_ctx->io_num = io_num;
        app_ctx->input_attrs = (rknn_tensor_attr *)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
        memcpy(app_ctx->input_attrs, input_attrs, io_num.n_input * sizeof(rknn_tensor_attr));
        app_ctx->output_attrs = (rknn_tensor_attr *)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
        memcpy(app_ctx->output_attrs, output_attrs, io_num.n_output * sizeof(rknn_tensor_attr));

        app_ctx->input_native_attrs = (rknn_tensor_attr *)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
        memcpy(app_ctx->input_native_attrs, input_native_attrs, io_num.n_input * sizeof(rknn_tensor_attr));
        app_ctx->output_native_attrs = (rknn_tensor_attr *)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
        memcpy(app_ctx->output_native_attrs, output_native_attrs, io_num.n_output * sizeof(rknn_tensor_attr));

        if (input_attrs[0].fmt == RKNN_TENSOR_NCHW)
        {
            printf("model is NCHW input fmt\n");
            app_ctx->model_channel = input_attrs[0].dims[1];
            app_ctx->model_height = input_attrs[0].dims[2];
            app_ctx->model_width = input_attrs[0].dims[3];
        }
        else
        {
            printf("model is NHWC input fmt\n");
            app_ctx->model_height = input_attrs[0].dims[1];
            app_ctx->model_width = input_attrs[0].dims[2];
            app_ctx->model_channel = input_attrs[0].dims[3];
        }
        printf("model input height=%d, width=%d, channel=%d\n",
               app_ctx->model_height, app_ctx->model_width, app_ctx->model_channel);

        return 0;
    }

    void dump_tensor_attr(rknn_tensor_attr *attr)
    {
        char dims[128] = {0};
        for (int i = 0; i < attr->n_dims; ++i)
        {
            int idx = strlen(dims);
            sprintf(&dims[idx], "%d%s", attr->dims[i], (i == attr->n_dims - 1) ? "" : ", ");
        }
        printf("  index=%d, name=%s, n_dims=%d, dims=[%s], n_elems=%d, size=%d, w_stride = %d, size_with_stride = %d, "
               "fmt=%s, type=%s, qnt_type=%s, "
               "zp=%d, scale=%f\n",
               attr->index, attr->name, attr->n_dims, dims, attr->n_elems, attr->size, attr->w_stride, attr->size_with_stride,
               get_format_string(attr->fmt), get_type_string(attr->type), get_qnt_type_string(attr->qnt_type), attr->zp,
               attr->scale);
    }

    int NC1HWC2_i8_to_NCHW_i8(const int8_t *src, int8_t *dst, int *dims, int channel, int h, int w, int zp, float scale)
    {
        int batch = dims[0];
        int C1 = dims[1];
        int C2 = dims[4];
        int hw_src = dims[2] * dims[3];
        int hw_dst = h * w;
        for (int i = 0; i < batch; i++)
        {
            const int8_t *src_b = src + i * C1 * hw_src * C2;
            int8_t *dst_b = dst + i * channel * hw_dst;
            for (int c = 0; c < channel; ++c)
            {
                int plane = c / C2;
                const int8_t *src_bc = plane * hw_src * C2 + src_b;
                int offset = c % C2;
                for (int cur_h = 0; cur_h < h; ++cur_h)
                    for (int cur_w = 0; cur_w < w; ++cur_w)
                    {
                        int cur_hw = cur_h * w + cur_w;
                        dst_b[c * hw_dst + cur_hw] = src_bc[C2 * cur_hw + offset]; // int8-->int8
                    }
            }
        }

        return 0;
    }

    int release_yolo11_model(rknn_app_context_t *app_ctx)
    {
        int ret;
        if (app_ctx->input_attrs != NULL)
        {
            free(app_ctx->input_attrs);
            app_ctx->input_attrs = NULL;
        }
        if (app_ctx->output_attrs != NULL)
        {
            free(app_ctx->output_attrs);
            app_ctx->output_attrs = NULL;
        }
        if (app_ctx->input_native_attrs != NULL)
        {
            free(app_ctx->input_native_attrs);
            app_ctx->input_native_attrs = NULL;
        }
        if (app_ctx->output_native_attrs != NULL)
        {
            free(app_ctx->output_native_attrs);
            app_ctx->output_native_attrs = NULL;
        }

        for (int i = 0; i < app_ctx->io_num.n_input; i++)
        {
            if (app_ctx->input_mems[i] != NULL)
            {
                ret = rknn_destroy_mem(app_ctx->rknn_ctx, app_ctx->input_mems[i]);
                if (ret != RKNN_SUCC)
                {
                    printf("rknn_destroy_mem fail! ret=%d\n", ret);
                    return -1;
                }
            }
        }
        for (int i = 0; i < app_ctx->io_num.n_output; i++)
        {
            if (app_ctx->output_mems[i] != NULL)
            {
                ret = rknn_destroy_mem(app_ctx->rknn_ctx, app_ctx->output_mems[i]);
                if (ret != RKNN_SUCC)
                {
                    printf("rknn_destroy_mem fail! ret=%d\n", ret);
                    return -1;
                }
            }
        }
        if (app_ctx->rknn_ctx != 0)
        {
            ret = rknn_destroy(app_ctx->rknn_ctx);
            if (ret != RKNN_SUCC)
            {
                printf("rknn_destroy fail! ret=%d\n", ret);
                return -1;
            }
            app_ctx->rknn_ctx = 0;
        }
        return 0;
    }

    

    static float CalculateOverlap(float xmin0, float ymin0, float xmax0, float ymax0, float xmin1, float ymin1, float xmax1,
                                  float ymax1)
    {
        float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0);
        float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0);
        float i = w * h;
        float u = (xmax0 - xmin0 + 1.0) * (ymax0 - ymin0 + 1.0) + (xmax1 - xmin1 + 1.0) * (ymax1 - ymin1 + 1.0) - i;
        return u <= 0.f ? 0.f : (i / u);
    }

    static int nms(int validCount, std::vector<float> &outputLocations, std::vector<int> classIds, std::vector<int> &order,
                   int filterId, float threshold)
    {
        for (int i = 0; i < validCount; ++i)
        {
            int n = order[i];
            if (n == -1 || classIds[n] != filterId)
            {
                continue;
            }
            for (int j = i + 1; j < validCount; ++j)
            {
                int m = order[j];
                if (m == -1 || classIds[m] != filterId)
                {
                    continue;
                }
                float xmin0 = outputLocations[n * 4 + 0];
                float ymin0 = outputLocations[n * 4 + 1];
                float xmax0 = outputLocations[n * 4 + 0] + outputLocations[n * 4 + 2];
                float ymax0 = outputLocations[n * 4 + 1] + outputLocations[n * 4 + 3];

                float xmin1 = outputLocations[m * 4 + 0];
                float ymin1 = outputLocations[m * 4 + 1];
                float xmax1 = outputLocations[m * 4 + 0] + outputLocations[m * 4 + 2];
                float ymax1 = outputLocations[m * 4 + 1] + outputLocations[m * 4 + 3];

                float iou = CalculateOverlap(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1);

                if (iou > threshold)
                {
                    order[j] = -1;
                }
            }
        }
        return 0;
    }

    static int quick_sort_indice_inverse(std::vector<float> &input, int left, int right, std::vector<int> &indices)
    {
        float key;
        int key_index;
        int low = left;
        int high = right;
        if (left < right)
        {
            key_index = indices[left];
            key = input[left];
            while (low < high)
            {
                while (low < high && input[high] <= key)
                {
                    high--;
                }
                input[low] = input[high];
                indices[low] = indices[high];
                while (low < high && input[low] >= key)
                {
                    low++;
                }
                input[high] = input[low];
                indices[high] = indices[low];
            }
            input[low] = key;
            indices[low] = key_index;
            quick_sort_indice_inverse(input, left, low - 1, indices);
            quick_sort_indice_inverse(input, low + 1, right, indices);
        }
        return low;
    }

    static float sigmoid(float x) { return 1.0 / (1.0 + expf(-x)); }

    static float unsigmoid(float y) { return -1.0 * logf((1.0 / y) - 1.0); }

    inline static int32_t __clip(float val, float min, float max)
    {
        float f = val <= min ? min : (val >= max ? max : val);
        return f;
    }

    static int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale)
    {
        float dst_val = (f32 / scale) + zp;
        int8_t res = (int8_t)__clip(dst_val, -128, 127);
        return res;
    }

    static uint8_t qnt_f32_to_affine_u8(float f32, int32_t zp, float scale)
    {
        float dst_val = (f32 / scale) + zp;
        uint8_t res = (uint8_t)__clip(dst_val, 0, 255);
        return res;
    }

    static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale) { return ((float)qnt - (float)zp) * scale; }

    static float deqnt_affine_u8_to_f32(uint8_t qnt, int32_t zp, float scale) { return ((float)qnt - (float)zp) * scale; }

    // 优化后的函数
    static void compute_dfl(float *tensor, int dfl_len, float *box)
    {
        for (int b = 0; b < 4; b++)
        {
            float exp_sum = 0.0f;
            float acc_sum = 0.0f;
            // 提前计算 exp(tensor[i + b * dfl_len]) 和 exp_sum
            for (int i = 0; i < dfl_len; i++)
            {
                float exp_val = exp(tensor[i + b * dfl_len]);
                exp_sum += exp_val;
                acc_sum += exp_val * i; // 直接累加加权值
            }
            box[b] = acc_sum / exp_sum;
        }
    }

    static int process_u8(uint8_t *box_tensor, int32_t box_zp, float box_scale,
                          uint8_t *score_tensor, int32_t score_zp, float score_scale,
                          uint8_t *score_sum_tensor, int32_t score_sum_zp, float score_sum_scale,
                          int grid_h, int grid_w, int stride, int dfl_len,
                          std::vector<float> &boxes,
                          std::vector<float> &objProbs,
                          std::vector<int> &classId,
                          float threshold)
    {
        int validCount = 0;
        int grid_len = grid_h * grid_w;
        uint8_t score_thres_u8 = qnt_f32_to_affine_u8(threshold, score_zp, score_scale);
        uint8_t score_sum_thres_u8 = qnt_f32_to_affine_u8(threshold, score_sum_zp, score_sum_scale);

        for (int i = 0; i < grid_h; i++)
        {
            for (int j = 0; j < grid_w; j++)
            {
                int offset = i * grid_w + j;
                int max_class_id = -1;

                // Use score sum to quickly filter
                if (score_sum_tensor != nullptr)
                {
                    if (score_sum_tensor[offset] < score_sum_thres_u8)
                    {
                        continue;
                    }
                }

                uint8_t max_score = -score_zp;
                for (int c = 0; c < OBJ_CLASS_NUM; c++)
                {
                    if ((score_tensor[offset] > score_thres_u8) && (score_tensor[offset] > max_score))
                    {
                        max_score = score_tensor[offset];
                        max_class_id = c;
                    }
                    offset += grid_len;
                }

                // compute box
                if (max_score > score_thres_u8)
                {
                    offset = i * grid_w + j;
                    float box[4];
                    float before_dfl[dfl_len * 4];
                    for (int k = 0; k < dfl_len * 4; k++)
                    {
                        before_dfl[k] = deqnt_affine_u8_to_f32(box_tensor[offset], box_zp, box_scale);
                        offset += grid_len;
                    }
                    compute_dfl(before_dfl, dfl_len, box);

                    float x1, y1, x2, y2, w, h;
                    x1 = (-box[0] + j + 0.5) * stride;
                    y1 = (-box[1] + i + 0.5) * stride;
                    x2 = (box[2] + j + 0.5) * stride;
                    y2 = (box[3] + i + 0.5) * stride;
                    w = x2 - x1;
                    h = y2 - y1;
                    boxes.push_back(x1);
                    boxes.push_back(y1);
                    boxes.push_back(w);
                    boxes.push_back(h);

                    objProbs.push_back(deqnt_affine_u8_to_f32(max_score, score_zp, score_scale));
                    classId.push_back(max_class_id);
                    validCount++;
                }
            }
        }
        return validCount;
    }

    static int process_i8(int8_t *box_tensor, int32_t box_zp, float box_scale,
                          int8_t *score_tensor, int32_t score_zp, float score_scale,
                          int8_t *score_sum_tensor, int32_t score_sum_zp, float score_sum_scale,
                          int grid_h, int grid_w, int stride, int dfl_len,
                          std::vector<float> &boxes,
                          std::vector<float> &objProbs,
                          std::vector<int> &classId,
                          float threshold)
    {
        int validCount = 0;
        int grid_len = grid_h * grid_w;
        int8_t score_thres_i8 = qnt_f32_to_affine(threshold, score_zp, score_scale);
        int8_t score_sum_thres_i8 = qnt_f32_to_affine(threshold, score_sum_zp, score_sum_scale);

        for (int i = 0; i < grid_h; i++)
        {
            for (int j = 0; j < grid_w; j++)
            {
                int offset = i * grid_w + j;
                int max_class_id = -1;

                // 通过 score sum 起到快速过滤的作用
                if (score_sum_tensor != nullptr)
                {
                    if (score_sum_tensor[offset] < score_sum_thres_i8)
                    {
                        continue;
                    }
                }

                int8_t max_score = -score_zp;
                for (int c = 0; c < OBJ_CLASS_NUM; c++)
                {
                    if ((score_tensor[offset] > score_thres_i8) && (score_tensor[offset] > max_score))
                    {
                        max_score = score_tensor[offset];
                        max_class_id = c;
                    }
                    offset += grid_len;
                }

                // compute box
                if (max_score > score_thres_i8)
                {
                    offset = i * grid_w + j;
                    float box[4];
                    float before_dfl[dfl_len * 4];
                    for (int k = 0; k < dfl_len * 4; k++)
                    {
                        before_dfl[k] = deqnt_affine_to_f32(box_tensor[offset], box_zp, box_scale);
                        offset += grid_len;
                    }
                    compute_dfl(before_dfl, dfl_len, box);

                    float x1, y1, x2, y2, w, h;
                    x1 = (-box[0] + j + 0.5) * stride;
                    y1 = (-box[1] + i + 0.5) * stride;
                    x2 = (box[2] + j + 0.5) * stride;
                    y2 = (box[3] + i + 0.5) * stride;
                    w = x2 - x1;
                    h = y2 - y1;
                    boxes.push_back(x1);
                    boxes.push_back(y1);
                    boxes.push_back(w);
                    boxes.push_back(h);

                    objProbs.push_back(deqnt_affine_to_f32(max_score, score_zp, score_scale));
                    classId.push_back(max_class_id);
                    validCount++;
                }
            }
        }
        return validCount;
    }

    static int process_fp32(float *box_tensor, float *score_tensor, float *score_sum_tensor,
                            int grid_h, int grid_w, int stride, int dfl_len,
                            std::vector<float> &boxes,
                            std::vector<float> &objProbs,
                            std::vector<int> &classId,
                            float threshold)
    {
        int validCount = 0;
        int grid_len = grid_h * grid_w;
        for (int i = 0; i < grid_h; i++)
        {
            for (int j = 0; j < grid_w; j++)
            {
                int offset = i * grid_w + j;
                int max_class_id = -1;

                // 通过 score sum 起到快速过滤的作用
                if (score_sum_tensor != nullptr)
                {
                    if (score_sum_tensor[offset] < threshold)
                    {
                        continue;
                    }
                }

                float max_score = 0;
                for (int c = 0; c < OBJ_CLASS_NUM; c++)
                {
                    if ((score_tensor[offset] > threshold) && (score_tensor[offset] > max_score))
                    {
                        max_score = score_tensor[offset];
                        max_class_id = c;
                    }
                    offset += grid_len;
                }

                // compute box
                if (max_score > threshold)
                {
                    offset = i * grid_w + j;
                    float box[4];
                    float before_dfl[dfl_len * 4];
                    for (int k = 0; k < dfl_len * 4; k++)
                    {
                        before_dfl[k] = box_tensor[offset];
                        offset += grid_len;
                    }
                    compute_dfl(before_dfl, dfl_len, box);

                    float x1, y1, x2, y2, w, h;
                    x1 = (-box[0] + j + 0.5) * stride;
                    y1 = (-box[1] + i + 0.5) * stride;
                    x2 = (box[2] + j + 0.5) * stride;
                    y2 = (box[3] + i + 0.5) * stride;
                    w = x2 - x1;
                    h = y2 - y1;
                    boxes.push_back(x1);
                    boxes.push_back(y1);
                    boxes.push_back(w);
                    boxes.push_back(h);

                    objProbs.push_back(max_score);
                    classId.push_back(max_class_id);
                    validCount++;
                }
            }
        }
        return validCount;
    }

    int post_process(rknn_app_context_t *app_ctx, void *outputs, letterbox_t *letter_box, float conf_threshold, float nms_threshold, object_detect_result_list *od_results)
    {
        rknn_output *_outputs = (rknn_output *)outputs;
        std::vector<float> filterBoxes;
        std::vector<float> objProbs;
        std::vector<int> classId;
        int validCount = 0;
        int stride = 0;
        int grid_h = 0;
        int grid_w = 0;
        int model_in_w = app_ctx->model_width;
        int model_in_h = app_ctx->model_height;

        memset(od_results, 0, sizeof(object_detect_result_list));

        // default 3 branch
        int dfl_len = app_ctx->output_attrs[0].dims[1] / 4;
        int output_per_branch = app_ctx->io_num.n_output / 3;
        for (int i = 0; i < 3; i++)
        {

            void *score_sum = nullptr;
            int32_t score_sum_zp = 0;
            float score_sum_scale = 1.0;
            if (output_per_branch == 3)
            {
                score_sum = _outputs[i * output_per_branch + 2].buf;
                score_sum_zp = app_ctx->output_attrs[i * output_per_branch + 2].zp;
                score_sum_scale = app_ctx->output_attrs[i * output_per_branch + 2].scale;
            }
            int box_idx = i * output_per_branch;
            int score_idx = i * output_per_branch + 1;

            grid_h = app_ctx->output_attrs[box_idx].dims[2];
            grid_w = app_ctx->output_attrs[box_idx].dims[3];
            stride = model_in_h / grid_h;

            if (app_ctx->is_quant)
            {

                validCount += process_i8((int8_t *)_outputs[box_idx].buf, app_ctx->output_attrs[box_idx].zp, app_ctx->output_attrs[box_idx].scale,
                                         (int8_t *)_outputs[score_idx].buf, app_ctx->output_attrs[score_idx].zp, app_ctx->output_attrs[score_idx].scale,
                                         (int8_t *)score_sum, score_sum_zp, score_sum_scale,
                                         grid_h, grid_w, stride, dfl_len,
                                         filterBoxes, objProbs, classId, conf_threshold);
            }
            else
            {
                validCount += process_fp32((float *)_outputs[box_idx].buf, (float *)_outputs[score_idx].buf, (float *)score_sum,
                                           grid_h, grid_w, stride, dfl_len,
                                           filterBoxes, objProbs, classId, conf_threshold);
            }
        }

        // no object detect
        if (validCount <= 0)
        {
            return 0;
        }
        std::vector<int> indexArray;
        for (int i = 0; i < validCount; ++i)
        {
            indexArray.push_back(i);
        }
        quick_sort_indice_inverse(objProbs, 0, validCount - 1, indexArray);

        std::set<int> class_set(std::begin(classId), std::end(classId));

        for (auto c : class_set)
        {
            nms(validCount, filterBoxes, classId, indexArray, c, nms_threshold);
        }

        int last_count = 0;
        od_results->count = 0;

        /* box valid detect target */
        for (int i = 0; i < validCount; ++i)
        {
            if (indexArray[i] == -1 || last_count >= OBJ_NUMB_MAX_SIZE)
            {
                continue;
            }
            int n = indexArray[i];

            float x1 = filterBoxes[n * 4 + 0] - letter_box->x_pad;
            float y1 = filterBoxes[n * 4 + 1] - letter_box->y_pad;
            float x2 = x1 + filterBoxes[n * 4 + 2];
            float y2 = y1 + filterBoxes[n * 4 + 3];
            int id = classId[n];
            float obj_conf = objProbs[i];

            od_results->results[last_count].box.left = (int)(clamp(x1, 0, model_in_w) / letter_box->scale);
            od_results->results[last_count].box.top = (int)(clamp(y1, 0, model_in_h) / letter_box->scale);
            od_results->results[last_count].box.right = (int)(clamp(x2, 0, model_in_w) / letter_box->scale);
            od_results->results[last_count].box.bottom = (int)(clamp(y2, 0, model_in_h) / letter_box->scale);
            od_results->results[last_count].prop = obj_conf;
            od_results->results[last_count].cls_id = id;
            last_count++;
        }
        od_results->count = last_count;
        return 0;
    }


    // char *coco_cls_to_name(int cls_id)
    // {

    //     if (cls_id >= classNames_.size())
    //     {
    //         return "null";
    //     }

    //     return classNames_[cls_id].c_str();
        
    // }

    void deinit_post_process()
    {
        
    }
    /**
     * @brief 获取模型期望的输入尺寸
     *
     * @return cv::Size 输入尺寸（宽度，高度）
     */
    cv::Size getInputSize() const
    {
        return cv::Size(10, 10);
    }

    /**
     * @brief 获取模型支持的类别数量
     *
     * @return int 类别数量
     */
    int getNumClasses() const
    {
        return this->numClasses;
    }

    /**
     * @brief 获取从标签文件加载的类别名称
     *
     * @return const std::vector<std::string>& 类别名称向量
     */
    const std::vector<std::string> &getClassNames() const
    {
        return this->classNames_;
    }

    /**
     * @brief 设置预处理方法（Resize 或 LetterBox）
     *
     * @param type PreprocessType 枚举值
     */
    void setPreprocessType(PreprocessType type)
    {
        this->preprocessType_ = type;
    }


    void detect(image_buffer_t *img,
                                  object_detect_result_list* od_results,
                                  bool converted = false,
                                  float confThreshold = 0.25f,
                                  float nmsThreshold = 0.45f)
    {

        infer(&this->app_ctx, img, od_results, converted);


    }

    void detect(std::shared_ptr<image_buffer_t> img,
                                  object_detect_result_list* od_results,
                                  bool converted = false,
                                  float confThreshold = 0.25f,
                                  float nmsThreshold = 0.45f)
    {

        infer(&this->app_ctx, img.get(), od_results, converted);


    }
    
    int infer(rknn_app_context_t *app_ctx, image_buffer_t *img, object_detect_result_list *od_results, bool converted)
    {
        int ret;
        image_buffer_t dst_img;
        letterbox_t letter_box;
        const float nms_threshold = NMS_THRESH;      // 默认的NMS阈值
        const float box_conf_threshold = BOX_THRESH; // 默认的置信度阈值
        int bg_color = 114;

        if ((!app_ctx) || !(img) || (!od_results))
        {
            return -1;
        }

        memset(od_results, 0x00, sizeof(*od_results));
        memset(&letter_box, 0, sizeof(letterbox_t));
        memset(&dst_img, 0, sizeof(image_buffer_t));

        // Pre Process
        dst_img.width = app_ctx->model_width;
        dst_img.height = app_ctx->model_height;
        dst_img.format = IMAGE_FORMAT_RGB888;
        dst_img.size = get_image_size(&dst_img);
        dst_img.fd = app_ctx->input_mems[0]->fd;
        dst_img.virt_addr = (unsigned char *)app_ctx->input_mems[0]->virt_addr;

        if (dst_img.virt_addr == NULL && dst_img.fd == 0)
        {
            printf("malloc buffer size:%d fail!\n", dst_img.size);
            return -1;
        }

        TIMER timer;

        timer.tik();
        ret = convert_image_with_letterbox(img, &dst_img, &letter_box, bg_color);
        timer.tok();
        //timer.print_time("convert_image_with_letterbox time:");
        if (ret < 0)
        {
            printf("convert_image_with_letterbox fail! ret=%d\n", ret);
            return -1;
        }
        // letterbox
        if (converted)
        {
            getLetter(img->srcWidth, img->srcHeight, dst_img.width, dst_img.height, &letter_box);
        }
        

        // Run
        for (size_t i = 0; i < 1; i++)
        {

            //printf("rknn_run\n");
            timer.tik();
            ret = rknn_run(app_ctx->rknn_ctx, nullptr);
            timer.tok();
            //timer.print_time("rknn run time:");
        }

        // ret = rknn_run(app_ctx->rknn_ctx, nullptr);
        if (ret < 0)
        {
            printf("rknn_run fail! ret=%d\n", ret);
            return -1;
        }

        // NC1HWC2 to NCHW
        timer.tik();
        rknn_output outputs[app_ctx->io_num.n_output];
        memset(outputs, 0, sizeof(outputs));
        // Get Output
        for (int i = 0; i < app_ctx->io_num.n_output; i++)
        {
            outputs[i].index = i;
            outputs[i].want_float = (!app_ctx->is_quant);
        }
        ret = rknn_outputs_get(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs, NULL);
        if (ret < 0)
        {
            printf("rknn_outputs_get fail! ret=%d\n", ret);
            goto out;
        }
        timer.tok();
        //timer.print_time("rknn_outputs_get");
        // for (uint32_t i = 0; i < app_ctx->io_num.n_output; i++)
        // {
        //     int channel = app_ctx->output_attrs[i].dims[1];
        //     int h = app_ctx->output_attrs[i].n_dims > 2 ? app_ctx->output_attrs[i].dims[2] : 1;
        //     int w = app_ctx->output_attrs[i].n_dims > 3 ? app_ctx->output_attrs[i].dims[3] : 1;
        //     int hw = h * w;
        //     int zp = app_ctx->output_native_attrs[i].zp;
        //     float scale = app_ctx->output_native_attrs[i].scale;
        //     if (app_ctx->is_quant)
        //     {
        //         outputs[i].size = app_ctx->output_native_attrs[i].n_elems * sizeof(int8_t);
        //         outputs[i].buf = (int8_t *)malloc(outputs[i].size);
        //         if (app_ctx->output_native_attrs[i].fmt == RKNN_TENSOR_NC1HWC2)
        //         {
        //             timer.tik();
        //             NC1HWC2_i8_to_NCHW_i8((int8_t *)app_ctx->output_mems[i]->virt_addr, (int8_t *)outputs[i].buf,
        //                                   (int *)app_ctx->output_native_attrs[i].dims, channel, h, w, zp, scale);
        //             timer.tok();
        //             timer.print_time("NC1HWC2 to NCHW");
        //         }
        //         else
        //         {
        //             memcpy(outputs[i].buf, app_ctx->output_mems[i]->virt_addr, outputs[i].size);
        //         }
        //     }
        //     else
        //     {
        //         printf("Currently zero copy does not support fp16!\n");
        //         goto out;
        //     }
        // }
        
        // Post Process
        timer.tik();
        post_process(app_ctx, outputs, &letter_box, box_conf_threshold, nms_threshold, od_results);

        for (int i = 0; i < app_ctx->io_num.n_output; i++)
        {
            free(outputs[i].buf);
        }

        timer.tok();
        //timer.print_time("post_process");
    out:
        return ret;
    }

};

#endif