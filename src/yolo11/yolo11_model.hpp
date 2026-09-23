#ifndef _YOLO11_MODEL_H_
#define _YOLO11_MODEL_H_

// ============================================================================
// yolo11_model.hpp —— YOLO11 RKNN NPU 检测器（header-only 完整实现）
// ============================================================================
// 覆盖 YOLO11 的"加载(零拷贝 io-mem) → letterbox 跳过/回退 → rknn_run →
// anchor-free + DFL 张量解码 → 分桶排序 NMS → letterbox 坐标还原"全链路。
//
// 【输出张量解码总览（anchor-free，与 v8 同族）】
//   模型 3 个 FPN 层级(stride 8/16/32)，每层输出：
//     box  : [1, 4*dfl_len, Gh, Gw]  每边 dfl_len(=16) 个 bin 的分布 logits
//     score: [1, 80,      Gh, Gw]    每类 sigmoid 前/后概率(随导出而定)
//     sum  : [1, 1,       Gh, Gw]    可选的类别得分求和头(快速预筛，缺省不启用)
//   单帧张量元素总数 = Σ(4*16+80[+1]) × (80²+40²+20²) ≈ 121 万个标量，
//   后处理按"score_sum 粗筛 → 类别取最大 → 仅存活格做 DFL softmax"逐层收敛，
//   避免对全网格执行昂贵的指数运算。复杂度 O(G_sum × 80)，仅对通过阈值的
//   格子再付 O(dfl_len×4) 的 softmax 代价。
//
// 【DFL 距离解码】box[b] = Σ_i i·softmax(t[b*16+i])，得到"格中心到四条边
//   的距离"(单位=stride)，再由 x1=(-l+j+0.5)*stride 还原像素框（j+0.5 即
//   格中心）。详见 compute_dfl / process_* 注释。
// ============================================================================

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

/**
 * @brief YOLOv11 detector implementation for Rockchip RK3588 platform
 *
 * 审查注记（成员语义与生命周期）：
 *   - app_ctx 按值内嵌（非指针），构造失败即对象不可用；rknn 上下文与
 *     io-mem 都在 init_yolo11_model 里创建，成对由 release_yolo11_model
 *     销毁——但当前**没有任何地方调用它**（析构函数使用编译器默认版本），
 *     即 YOLO11Model 对象销毁时会泄漏一个 rknn 上下文。因模型池与
 *     PpeTask 同生命周期、进程级常驻，现状可接受，热重载模型前必须补上。
 *   - numClasses 仅作元数据返回(getNumClasses)；后处理得分通道遍历实际
 *     用的是编译期宏 OBJ_CLASS_NUM=80，二者在自定义类别数模型上不一致，
 *     换非 80 类模型时改宏或改代码，勿只改构造参数。
 *   - dflLen 成员同样只是记录值，真正的解码宽度在 post_process 里由
 *     输出张量 dims 反推（output_attrs[0].dims[1]/4），保证与模型一致。
 */

class YOLO11Model : public YoloBaseDetector
{

private:
    rknn_app_context_t app_ctx;      // NPU 上下文+io 张量属性/内存（见 common.hpp）
    int numClasses = 0;              // 元数据类别数（构造参数透传，见上方注记）
    int dflLen = 16;                 // DFL bin 数：YOLO11 默认 16（每条边 16 个离散距离档）
    std::string modelPath;
    std::string labelsPath;
    std::vector<std::string> classNames_;
    // 配置
    PreprocessType preprocessType_;  // 默认 LETTERBOX；生产链路实际由调用方
                                     // 预处理(RGA)后以 converted=true 跳过

public:
    // ⚠ 加载失败会 throw std::runtime_error：调用点(PpeTask::init)若在
    //   UI 线程栈上则异常沿 start() 上抛、在推理线程上则可能直接 terminate。
    //   现状依赖"模型文件必存在"的部署约定，新增容错时请在调用侧 catch。
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

        int ret = init_yolo11_model(modelPath.c_str(), &this->app_ctx, core_mask);
        if (ret < 0) {
            std::cerr << "[YOLO11Model] 模型加载失败: " << modelPath << std::endl;
            throw std::runtime_error("Failed to load model: " + modelPath);
        }
    }

    // ========================================================================
    // init_yolo11_model —— 模型加载 + 零拷贝 io-mem 绑定
    // ========================================================================
    // 流程与设计要点：
    //   1. read_data_from_file 读入 .rknn 后立刻 rknn_init（内部拷贝），
    //      随即 free(model)，避免整份权重常驻堆上。
    //   2. core_mask 非 0 才 set：0 表示"交给运行时自动选核(RKNN_NPU_CORE_AUTO)"，
    //      级联多模型时上层显式传 0/1/2 错核实现三核并行。
    //      审查点：set_core_mask 失败仅打印不中止——最坏退化为默认核分配。
    //   3. 查询的是 NATIVE_*_ATTR（硬件真实布局，如 NC1HWC2/w_stride），
    //      并强制把输入类型设为 UINT8（见下方注释）——这决定了整条零拷贝链
    //      的喂数方式：RGA 输出的 RGB888 字节流就是 NPU 的"已量化域输入"。
    //   4. rknn_create_mem 分配 DMA 连续内存(带 fd)，rknn_set_io_mem 绑定后
    //      推理全程免 CPU memcpy；这就是 infer 里 dst_img.virt_addr 直接
    //      指向 input_mems[0] 的原因。
    //   5. 审查点：`rknn_tensor_attr input_native_attrs[io_num.n_input]` 等
    //      使用 G++ 扩展 VLA（io_num 运行时才知道大小）；n_input=1/n_output≤9
    //      时栈占用可控，迁移到严格标准 C++ 时需换 vector。
    //   6. is_quant 判定只看输出[0]（TODO 原注释）：约定整个 YOLO 导出
    //      INT8 量化则所有输出同属性，混合精度模型会误判，需逐张量检查。
    //   7. 输入尺寸解析：NCHW→dims[1..3]=C,H,W；NHWC→dims[1..3]=H,W,C，
    //      与 RKNN 文档一致；后续 letterbox/RGA 目标尺寸以 model_width/height 为准。
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
        // 中文注记：声明"我喂给你的就是 0~255 的 uint8 原始像素"，于是模型
        // 导出时烘焙的归一化(/255、减均值除方差)与 INT8 量化由 NPU 运行时
        // 在片上融合完成，CPU 侧不再做 (px-127.5)/128 之类的浮点预处理——
        // 这正是解码线程能把 RGA 缩放结果 DMA 直送 NPU、零 CPU 转码的前提。
        input_native_attrs[0].type = RKNN_TENSOR_UINT8;
        // 按 size_with_stride（含硬件行对齐）分配 DMA 内存，绑定后 app 与
        // NPU 共享同一物理页；fd 可再被 rknn 内部/其他 DMA 用户引用
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

    // ========================================================================
    // dump_tensor_attr —— 打印张量元数据（调试期核对 dims/zp/scale/布局的关键）
    // ========================================================================
    // 关注字段含义：w_stride=硬件行对齐后的宽度；size_with_stride=按对齐
    // 排布的真实占用；qnt_type/zp/scale 即仿射量化参数（见量化函数注释）。
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

    // ========================================================================
    // NC1HWC2_i8_to_NCHW_i8 —— NPU 原生布局重排为常规 NCHW（int8 直拷）
    // ========================================================================
    // RKNN NPU 输出常为 NC1HWC2 分块布局：通道维拆成 C1×C2（C2 为硬件向量
    // 宽，int8 常见 2/16），内存序 [N][C1][H][W][C2]。还原公式：
    //   逻辑通道 c → 块号 plane=c/C2、块内偏移 offset=c%C2
    //   src[((i*C1+plane)*H + h)*W*C2 + w*C2 + offset]
    // 注意：只做布局搬运、**不做反量化**（int8→int8，zp/scale 形参保留但
    // 未参与计算，供未来需要在此顺路量化变换时使用）。
    // 现状：唯一调用点已被注释掉（infer 走 rknn_outputs_get 由驱动侧整理
    // 为 NCHW），本函数留存备用；若改回 io-mem 直读零拷贝后处理可启用，
    // 能省一次驱动侧拷贝。
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

    // ========================================================================
    // release_yolo11_model —— 反向释放 init 的全部资源（属性数组/io-mem/上下文）
    // ========================================================================
    // 审查点：如类头注记所述，当前无人调用（无析构函数转发）；任何"换模型/
    // 重启任务"的功能都必须先接上它，否则每重建一次泄漏一个 NPU 上下文。
    // 释放次序讲究：先销毁绑定的 io-mem，后销毁 ctx（mem 依赖 ctx 生命周期）。
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

    

    // ------------------------------------------------------------------------
    // CalculateOverlap —— IoU（离散像素惯例）：长宽均按 "+1" 计像素数，
    // 即闭区间 [x1,x2] 的整数面积。与 SCRFD 侧连续坐标 (+0) 的算法差 1px 级，
    // 只影响阈值临界个案。分子交集、分母并集=A1+A2-交集，O(1)。
    // ------------------------------------------------------------------------
    static float CalculateOverlap(float xmin0, float ymin0, float xmax0, float ymax0, float xmin1, float ymin1, float xmax1,
                                  float ymax1)
    {
        float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0);
        float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0);
        float i = w * h;
        float u = (xmax0 - xmin0 + 1.0) * (ymax0 - ymin0 + 1.0) + (xmax1 - xmin1 + 1.0) * (ymax1 - ymin1 + 1.0) - i;
        return u <= 0.f ? 0.f : (i / u);
    }

    // ========================================================================
    // nms —— "按类别分桶"的非极大值抑制（in-place 标记法）
    // ========================================================================
    // 输入约定：outputLocations=validCount×[x,y,w,h] 扁平数组；order=按得分
    // 降序排好的"候选下标数组"（quick_sort_indice_inverse 的产物）；
    // filterId=本次只处理的类别。
    // 算法：从高到低遍历 order，跳过已抑制(-1)与异类项；对每个保留框把与它
    // IoU>threshold 的同类低分框在 order 中标 -1。不做物理删除，最终由
    // post_process 汇总时按 -1 过滤——避免 vector 逐个 erase 的 O(N²) 搬移。
    // 复杂度：外层×内层两两比较，最坏 O(V²)（V=validCount，实测几十~几百）；
    // 分桶后每类子集更小，且 classIds[n]!=filterId 的 continue 只是廉价跳过，
    // 不改变两两扫描的上界。总体仍远小于输出网格扫描本身的后处理占比。
    // ========================================================================
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

    // ========================================================================
    // quick_sort_indice_inverse —— 对 input 降序快排，indices 跟随同步置换
    // ========================================================================
    // 双路填坑式 partition（pivot 取最左），排序后 indices[k] = 原第 k 大元素
    // 的下标——post_process 即以此作为 NMS 的"高分优先"扫描序，并保证
    // objProbs[i] 恒为第 i 大的分数（与 indexArray 一一联动置换）。
    // 复杂度平均 O(N log N)、最坏 O(N²)（选元固定取 left，检测框分数天然
    // 乱序，实际不会触发退化）；indices 与 input 必须等长，调用方保证。
    // ========================================================================
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

    // ------------------------------------------------------------------------
    // 概率域工具：sigmoid(v5 时代对 logit 求概率)与 unsigmoid(把阈值反变换进
    // 量化域再逐帧比较)。当前 v8/v11 导出链的 score 已在网络内 sigmoid，
    // 本对函数在本文件内无调用点，属保留的跨版本工具。
    // ------------------------------------------------------------------------
    static float sigmoid(float x) { return 1.0 / (1.0 + expf(-x)); }

    static float unsigmoid(float y) { return -1.0 * logf((1.0 / y) - 1.0); }

    // 浮点夹到 [min,max] 后隐式截断取整（float→int 走 f 赋值截断）
    inline static int32_t __clip(float val, float min, float max)
    {
        float f = val <= min ? min : (val >= max ? max : val);
        return f;
    }

    // ========================================================================
    // 仿射量化/反量化四件套
    // ========================================================================
    // RKNN 非对称仿射量化约定：
    //   反量化：float = (q - zp) * scale        （deqnt_affine_*_to_f32）
    //   量化：  q   = float / scale + zp, 截断并夹到 int8[-128,127]/uint8[0,255]
    //   zp=zero point（浮点 0 的量化值），scale=步长；均为 rknn_query 出来的
    //   张量属性，故阈值比较可以整体搬进"单调的量化整数域"完成——
    //   对每个格子免去逐点反量化的浮点乘法，只给存活框反量化一次。
    // 审查注记（两个易困惑点）：
    //   1) qnt 中用"截断"而非"四舍五入"，对阈值边界有 ≤1 LSB 偏差，工程可忽略；
    //   2) process_i8 里 max_score 初值取 -score_zp：沿用 rknn_model_zoo 对
    //      int8 非对称量化 zp 的符号惯例（阈值量化与它比较同处一个域即可，
    //      语义是"分数从 0 起步"），修改量化后端时两处要一起核对。
    // ========================================================================
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

    // ========================================================================
    // compute_dfl —— DFL(Distribution Focal Loss 头) 期望距离解码
    // ========================================================================
    // 语义：box[b] = Σ_i i · softmax(tensor[b*dfl_len + i])
    //   即把"该边到格中心的距离"建模为 dfl_len(=16) 个离散档位上的类别分布，
    // 取期望得到连续距离（单位：stride 格）。tensor 通道序为 [左,上,右,下]×16。
    // 优化点（原注释"优化后的函数"所指）：不做两趟（先 softmax 再求期望），
    // 单趟同步累加 exp 和 与 exp*i 加权和，最后一次性相除——与 softmax 期望
    // 数学恒等，省一次 dfl_len 遍历与中间数组；exp 仍是热点（4×16 次/框），
    // 所以外层务必靠 score 阈值控制进到这里的有效框数量。
    // ========================================================================
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

    // ------------------------------------------------------------------------
    // process_u8 —— 与 process_i8 同一算法、uint8 量化域版本
    // 审查注记：当前 post_process 只分流到 process_i8/process_fp32，本函数
    // 无调用点（保留自 rknn_model_zoo 上游，应对 uint8 输出量化模型）。
    // ------------------------------------------------------------------------
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

    // ========================================================================
    // process_i8 —— INT8 量化输出 → 有效检测框（量化域比较版）
    // ========================================================================
    // 张量寻址（三个 process_* 共用）：输出为 NCHW [1,C,Gh,Gw]，
    //   同一通道内空间连续：idx = i*grid_w + j（行主序网格）
    //   跨通道跳步固定为 grid_len = Gh*Gw（c 通道第 offset 格 = offset + c*grid_len）
    // 三段式漏斗，逐级收敛计算量：
    //   1) score_sum 粗筛（可选头，仅当模型每支有第 3 个输出时启用）：
    //      每格类别得分和一次比较剔掉绝大多数背景格；
    //   2) 80 类 argmax 循环：全部在量化整数域比较（阈值已用同映射量化），
    //      只有超过 conf 阈值且为该类最大者才更新，命中后才对最大分反量化；
    //   3) 存活格才做 4×dfl_len 次反量化 + compute_dfl 的 softmax 期望，
    //      再套 anchor-free 公式还原像素框：
    //        x1=(-l + j + 0.5)*stride   y1=(-t + i + 0.5)*stride
    //        x2=( r + j + 0.5)*stride   y2=( b + i + 0.5)*stride
    //      其中 j+0.5、i+0.5 为格中心，l/t/r/b 为期望距离(单位:格)，
    //      stride=model_in/网格边长；输出 x,y,w,h 仍在 640 模型空间，
    //      letterbox 还原统一推迟到 post_process 出口（只减 pad 除 scale 一次）。
    // 复杂度：O(Gh*Gw) 每格常数(粗筛) + O(Gh*Gw*80) 最坏类别扫描 +
    //          O(有效格×(4*16 exp))；三层 80²/40²/20² 合计约 8400 格/帧。
    // ========================================================================
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

                // 类别 argmax：offset 起步于当前格，每次 +=grid_len 即"下一类
                // 同一格"（NCHW 通道跳步）；max_score 初值 -score_zp 代表浮点 0
                //（见量化函数处的符号惯例说明），全部比较留在整数域，免反量化
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

                // compute box：仅存活格付费——offset 回卷到本格起点，
                // 沿通道取 4*dfl_len 个 DFL logits（同为通道跳步寻址）
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

    // ------------------------------------------------------------------------
    // process_fp32 —— 非量化模型（或 want_float 反量化后）的浮点版本：
    // 省去阈值量化与逐值反量化，比较/累加直接用 float，其余漏斗逻辑与
    // process_i8 一致（见其头部算法注记）。is_quant==false 时由 post_process 选用。
    // ------------------------------------------------------------------------
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

    // ========================================================================
    // post_process —— 三分支张量解码 → 全局排序 → 分类 NMS → letterbox 还原
    // ========================================================================
    // 输出张量索引布局（YOLO 导出的硬约定，n_output = 3 分支 × output_per_branch）：
    //   output_per_branch == 3：每支为 [box, score, score_sum] → sum 头参与粗筛
    //   output_per_branch == 2：每支为 [box, score]           → sum=nullptr 关闭粗筛
    //   即第 i 支的 box/score/sum 位于 index i*p、i*p+1、i*p+2。
    // 审查点：若换入非"3 等分分支"布局的模型（如 seg 额外掩码系数头），
    //   n_output/3 整除截断会错位索引，届时需按 dims 名称匹配输出头。
    // dfl_len 由 box 头通道数反推（dims[1]=4*dfl_len），与构造参数解耦，
    // 保证 16/32 档 DFL 导出均自适应。
    //
    // 结果字段回填策略（Why）：本函数只填 count/results（坐标已还原为原始帧
    //   像素系整数），id 与 time 刻意留 0/不填——路由槽位由 PpeTask 依
    //   TaskConfig.result_id 写入，帧时间戳由 PpeTask 用 TaskData::time 回填；
    //   这样模型层保持"纯函数"，帧级元数据始终随 TaskData 单点传递。
    // ========================================================================
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

        // 整体清零：memset 对 POD 全字段安全；同时保证"无目标"时
        // count=0/时间字段全 0 的确定性输出（消费端 tryPop 直接整包拷贝）
        memset(od_results, 0, sizeof(object_detect_result_list));

        // default 3 branch
        // 三层级(80/40/20 网格)等分输出；dfl_len 从 box 头通道数反推（见函数头注记）
        int dfl_len = app_ctx->output_attrs[0].dims[1] / 4;
        int output_per_branch = app_ctx->io_num.n_output / 3;
        for (int i = 0; i < 3; i++)
        {
            // 仅 3 头/支时存在 score_sum 粗筛头；2 头/支保持 nullptr 关闭该漏斗
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

            // 网格边长 dims[2]/dims[3]（NCHW 的 H/W）；stride=下采样倍率，
            // 由 640/网格反推（8→80、16→40、32→20），供框解码公式换算像素
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
            return 0;   // 空帧快路径：od_results 已被 memset 清零，count=0
        }
        // 构造"候选下标 0..V-1"，随后按分数降序联排置换（indexArray 排序后
        // 即 NMS 的扫描优先级序；同一次置换保证 objProbs[k] 是第 k 大分数）
        std::vector<int> indexArray;
        for (int i = 0; i < validCount; ++i)
        {
            indexArray.push_back(i);
        }
        quick_sort_indice_inverse(objProbs, 0, validCount - 1, indexArray);

        // 按出现的类别去重后逐类做 NMS：跨类别重叠框互不抑制
        //（人压在安全帽上是两个合法目标，同类重叠才是重复检测）
        std::set<int> class_set(std::begin(classId), std::end(classId));

        for (auto c : class_set)
        {
            nms(validCount, filterBoxes, classId, indexArray, c, nms_threshold);
        }

        int last_count = 0;
        od_results->count = 0;

        /* box valid detect target */
        // 按降序扫描候选：indexArray[i]==-1 表示已被同类高分框抑制；
        // OBJ_NUMB_MAX_SIZE(128) 截断保护——超限时保留的是分数最高的前 128 个
        //（因扫描序即分数序，天然"高分优先留存"）。
        for (int i = 0; i < validCount; ++i)
        {
            if (indexArray[i] == -1 || last_count >= OBJ_NUMB_MAX_SIZE)
            {
                continue;
            }
            int n = indexArray[i];

            // ===== letterbox 坐标还原（模型 640 空间 → 原始帧空间）=====
            // 正变换: x_model = x_src*scale + x_pad  ⇒  逆变换:
            //   x_src = (x_model - x_pad) / scale；w/h 由 x2-x1 差值导出，
            //   故宽高无需单独除以 scale（差值线性部分只缩放一次，正确）。
            // 审查点：clamp 作用在 640 空间、除 scale 作用在裁剪后——即"先裁
            // 到模型视野再映射"，越界框会贴到原图边缘而非按比例外推，符合直觉。
            float x1 = filterBoxes[n * 4 + 0] - letter_box->x_pad;
            float y1 = filterBoxes[n * 4 + 1] - letter_box->y_pad;
            float x2 = x1 + filterBoxes[n * 4 + 2];
            float y2 = y1 + filterBoxes[n * 4 + 3];
            int id = classId[n];
            // objProbs 已被原位降序排序 ⇒ 第 i 大分数即 objProbs[i]（与
            // indexArray 的置换联动成立，勿改成 objProbs[n]）
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
        return cv::Size(app_ctx.model_width, app_ctx.model_height);
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


    // ========================================================================
    // detect —— YoloBaseDetector 接口实现（裸指针与 shared_ptr 两个重载等价）
    // ========================================================================
    // 线程安全警示：本类**不可并发调用**——单 rknn_context 串行执行
    // inputs_set/run/outputs_get，且输入 DMA 内存(input_mems[0])唯一。
    // 现状安全的原因：每个 YOLO11Model 与一个 PpeTask 推理线程一一绑定
    // （ModelPool 仅按 ID 出借共享指针，同一实例绝不跨任务并发使用）。
    // 若未来让多个线程共享同一模型实例，必须整体加互斥锁保护 detect。
    void detect(image_buffer_t *img,
                                  object_detect_result_list* od_results,
                                  bool converted = false,
                                  float confThreshold = 0.25f,
                                  float nmsThreshold = 0.45f)
    {

        infer(&this->app_ctx, img, od_results, converted, confThreshold, nmsThreshold);


    }

    void detect(std::shared_ptr<image_buffer_t> img,
                                  object_detect_result_list* od_results,
                                  bool converted = false,
                                  float confThreshold = 0.25f,
                                  float nmsThreshold = 0.45f)
    {

        infer(&this->app_ctx, img.get(), od_results, converted, confThreshold, nmsThreshold);


    }
    
    // ========================================================================
    // infer —— 单帧完整推理：零拷贝喂数 → rknn_run → 取输出 → post_process
    // ========================================================================
    // 数据通路（生产链路 converted==true）：
    //   解码线程 RGA 已把原始帧 letterbox 成 640×640 RGB 写入 DmaBufferPool
    //   缓冲（img->virt_addr/sp_dmaBuffer）。本函数把 dst_img 直接指到
    //   NPU 输入 DMA 内存 input_mems[0]，convert_image_with_letterbox 退化为
    //   "640→640 的同尺寸搬运"（内部走 RGA fd 路径），数据落进 NPU 侧内存，
    //   全程无 CPU memcpy——这就是"零拷贝检测流水线"的收口点。
    // 关键参数：
    //   - converted==true 时必须用 getLetter(srcWidth/srcHeight) 重算 letter_box：
    //     搬运函数按 img 的 640×640 源几何只会给出 scale=1/pad=0 的恒等值，
    //     而坐标还原需要的是"原始帧→640"的真实 scale 与居中 pad，二者不同。
    //   - bg_color=114：Ultralytics 训练期 letterbox 标准灰（与 RGA 侧填充一致，
    //     预处理两端的背景色必须相同，否则边缘目标分布外）。
    //   - 阈值 0.25/0.45：与 YoloBaseDetector::detect 文档口径一致的代码级默认。
    // 返回值审查点：成功路径返回的是 rknn_outputs_get 的 ret(0)；post_process
    //   的返回值未向上透传，调用方(PpeTask)以"结构体内容"而非返回码判断结果。
    // ========================================================================
    int infer(rknn_app_context_t *app_ctx, image_buffer_t *img, object_detect_result_list *od_results, bool converted, float confThreshold = 0.25f, float nmsThreshold = 0.45f)
    {
        int ret;
        image_buffer_t dst_img;
        letterbox_t letter_box;
        const float nms_threshold = nmsThreshold;
        const float box_conf_threshold = confThreshold;
        int bg_color = 114;

        if ((!app_ctx) || !(img) || (!od_results))
        {
            return -1;
        }

        memset(od_results, 0x00, sizeof(*od_results));
        memset(&letter_box, 0, sizeof(letterbox_t));
        memset(&dst_img, 0, sizeof(image_buffer_t));

        // Pre Process
        // dst_img 不另配内存，直接复用 NPU 输入 io-mem（fd+virt 成对），
        // 之后所有"写入 dst_img"都等价于"喂给 NPU"
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
        // converted==true：img 已是上游 RGA letterbox 的 640 图，这里的
        // getLetter 用"原始帧尺寸(srcWidth/srcHeight)→640"重算真实
        // scale/x_pad/y_pad，覆盖搬运函数给出的恒等值（坐标还原的唯一依据）
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
        // 取回输出：want_float 与 is_quant 互补——
        //   量化模型: want_float=0，buf 即绑定的 int8 输出内存(process_i8 直读，
        //             零额外拷贝，这也是上方大段 NC1HWC2 手工重排代码被注释掉后
        //             仍能工作的前提：驱动已按 NCHW 视图整理好 io-mem)；
        //   非量化模型: want_float=1，runtime 反量化出 float buf(process_fp32 直读)。
        // 审查点（内存所有权·换 librknnrt 版本必查）：官方范例约定
        // rknn_outputs_get 配对的释放是 rknn_outputs_release(ctx, n, outputs)；
        // 此处改写为逐个 free(outputs[i].buf)，依赖的是"get 时为每个输出另
        // malloc 拷贝"的旧运行时行为。若新版驱动在 set_io_mem 模式下返回
        // 指向 output_mems 的原生指针，free 将错杀 DMA 内存导致崩溃。
        // goto out 路径发生在 outputs_get 失败时——彼时无任何 buf 持有，
        // 跳过 free 循环是正确的，不存在泄漏分支。
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