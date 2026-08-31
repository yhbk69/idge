// Common.hpp
#ifndef YOLOS_EDGEPLATFORM_COMMON_HPP
#define YOLOS_EDGEPLATFORM_COMMON_HPP

/**
 * @file Common.hpp
 * @brief YOLOs-CPP-EdgePlatform 通用数据结构和工具
 *
 * @author FANKYT
 * @date 2025
 */

#include <vector>
#include <string>
#include <opencv2/opencv.hpp>
#include <memory>
#include "rknn_api.h"
#include "DmaBufferPool.h"

#define OBJ_NAME_MAX_SIZE 64
#define OBJ_NUMB_MAX_SIZE 128
#define OBJ_CLASS_NUM 80
#define NMS_THRESH 0.45
#define BOX_THRESH 0.25

/**
 * @brief 表示图像中边界框的结构体
 */
struct BoundingBox {
    float x;      /**< 左上角 X 坐标 */
    float y;      /**< 左上角 Y 坐标 */
    float width;  /**< 边界框宽度 */
    float height; /**< 边界框高度 */

    BoundingBox() : x(0), y(0), width(0), height(0) {}

    BoundingBox(float x_, float y_, float width_, float height_)
        : x(x_), y(y_), width(width_), height(height_) {}

    /**
     * @brief 计算边界框的面积
     */
    float area() const { return width * height; }
};

/**
 * @brief 表示单个检测结果的结构体
 */
struct Detection {
    BoundingBox box; /**< 检测对象的边界框 */
    float conf;      /**< 检测的置信度分数 */
    int classId;     /**< 检测对象的类别ID */

    Detection() : conf(0.0f), classId(-1) {}

    Detection(const BoundingBox &box_, float conf_, int classId_)
        : box(box_), conf(conf_), classId(classId_) {}
};

/**
 * @brief 预处理方法枚举
 */
enum class PreprocessType {
    RESIZE = 0,    /**< 直接缩放 */
    LETTERBOX = 1  /**< 保持宽高比缩放（填充灰边） */
};

/**
 * @brief 模型输入格式类型
 */
enum class InputImageType {
    RGB = 0,    /**< RGB 格式 (NCHW, uint8-128 量化) */
    NV12 = 1    /**< NV12 格式 (YUV420SP) */
};

/**
 * @brief 表示姿态估计中关键点的结构体
 */
struct KeyPoint {
    float x;          /**< 关键点 X 坐标 */
    float y;          /**< 关键点 Y 坐标 */
    float confidence; /**< 关键点置信度 */

    KeyPoint() : x(0), y(0), confidence(0) {}

    KeyPoint(float x_, float y_, float conf_ = 0)
        : x(x_), y(y_), confidence(conf_) {}
};

/**
 * @brief 表示姿态检测结果的结构体
 */
struct PoseDetection {
    BoundingBox box;              /**< 人体边界框 */
    float conf;                   /**< 检测置信度 */
    int classId;                  /**< 类别ID (通常是 person) */
    std::vector<KeyPoint> keypoints; /**< 关键点列表 (COCO: 17个关键点) */

    PoseDetection() : conf(0.0f), classId(-1) {}

    PoseDetection(const BoundingBox &box_, float conf_, int classId_)
        : box(box_), conf(conf_), classId(classId_) {}
};

/**
 * @brief 表示实例分割结果的结构体
 */
struct Segmentation {
    BoundingBox box;  /**< 对象边界框 */
    float conf;       /**< 检测置信度 */
    int classId;      /**< 类别ID */
    cv::Mat mask;     /**< 分割掩码 (单通道,与原图同尺寸) */

    Segmentation() : conf(0.0f), classId(-1) {}

    Segmentation(const BoundingBox &box_, float conf_, int classId_)
        : box(box_), conf(conf_), classId(classId_) {}
};

/**
 * @brief COCO 人体姿态关键点骨架连接
 *
 * 17个关键点索引:
 * 0: 鼻子, 1-2: 眼睛, 3-4: 耳朵, 5-6: 肩膀
 * 7-8: 肘部, 9-10: 手腕, 11-12: 髋部, 13-14: 膝盖, 15-16: 脚踝
 */
static const std::vector<std::pair<int, int>> COCO_POSE_SKELETON = {
    // 面部连接
    {0,1}, {0,2}, {1,3}, {2,4},
    // 头部到肩膀
    {3,5}, {4,6},
    // 手臂
    {5,7}, {7,9}, {6,8}, {8,10},
    // 躯干
    {5,6}, {5,11}, {6,12}, {11,12},
    // 腿部
    {11,13}, {13,15}, {12,14}, {14,16}
};

/**
 * @brief Image pixel format
 * 
 */
typedef enum {
    IMAGE_FORMAT_GRAY8,
    IMAGE_FORMAT_RGB888,
    IMAGE_FORMAT_RGBA8888,
    IMAGE_FORMAT_YUV420SP_NV21,
    IMAGE_FORMAT_YUV420SP_NV12,
} image_format_t;

/**
 * @brief Image buffer
 * 
 */
struct image_buffer_t{
    //如果virt_address是已经转换的数据，srcWidth是转换前的width和height
    int srcWidth;
    int srcHeight;
    int width;
    int height;
    int width_stride;
    int height_stride;
    image_format_t format;
    unsigned char* virt_addr;
    int size;
    int fd;
    DmaBuffer* dmaBuffer;
    std::shared_ptr<DmaBuffer> sp_dmaBuffer;

    long time;

    ~image_buffer_t()
    {
        

    }

} ;

/**
 * @brief Image rectangle
 * 
 */
typedef struct {
    int left;
    int top;
    int right;
    int bottom;
} image_rect_t;

/**
 * @brief Image obb rectangle
 * 
 */
typedef struct {
    int x;
    int y;
    int w;
    int h;
    float angle;
} image_obb_box_t;

typedef struct
{
    char *dma_buf_virt_addr;
    int dma_buf_fd;
    int size;
} rknn_dma_buf;

typedef struct {
    rknn_context rknn_ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr* input_attrs;
    rknn_tensor_attr* output_attrs;

    rknn_dma_buf img_dma_buf;
    rknn_tensor_mem* input_mems[1];
    rknn_tensor_mem* output_mems[9];
    rknn_tensor_attr* input_native_attrs;
    rknn_tensor_attr* output_native_attrs;
    int model_channel;
    int model_width;
    int model_height;
    bool is_quant;
} rknn_app_context_t;

typedef struct {
    image_rect_t box;
    float prop;
    int cls_id;
} object_detect_result;


struct object_detect_result_list{
    int id;
    int count;
    long time;
    object_detect_result results[OBJ_NUMB_MAX_SIZE];
    bool operator<(const object_detect_result_list &other) const
    {
        return time < other.time;
    }
};


#endif // YOLOS_EDGEPLATFORM_COMMON_HPP
