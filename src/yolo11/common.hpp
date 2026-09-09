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

// ============================================================
// 预处理相关常量定义
// ============================================================
#define OBJ_NAME_MAX_SIZE 64       // 检测对象名称最大长度
#define OBJ_NUMB_MAX_SIZE 128      // 单帧最大检测对象数量
#define OBJ_CLASS_NUM 80           // COCO数据集类别数
#define NMS_THRESH 0.45            // NMS（非极大值抑制）默认IoU阈值
#define BOX_THRESH 0.25            // 置信度默认阈值

// ============================================================
// BoundingBox 结构体 - 表示图像中边界框
// ============================================================
/**
 * @brief 表示图像中边界框的结构体
 *
 * 作用：存储检测对象的矩形边界框信息，包含左上角坐标和宽高。
 * 用于目标检测、姿态估计、实例分割等多种任务中表示目标区域。
 */
struct BoundingBox {
    float x;      /**< 左上角 X 坐标 */
    float y;      /**< 左上角 Y 坐标 */
    float width;  /**< 边界框宽度 */
    float height; /**< 边界框高度 */

    // 默认构造函数，初始化为零值
    BoundingBox() : x(0), y(0), width(0), height(0) {}

    // 带参构造函数，直接指定边界框位置和尺寸
    BoundingBox(float x_, float y_, float width_, float height_)
        : x(x_), y(y_), width(width_), height(height_) {}

    /**
     * @brief 计算边界框的面积
     * 作用：用于NMS计算IoU时的面积归一化
     */
    float area() const { return width * height; }
};

// ============================================================
// Detection 结构体 - 单个检测结果
// ============================================================
/**
 * @brief 表示单个检测结果的结构体
 *
 * 作用：封装一次目标检测的完整结果，包括边界框、置信度和类别信息。
 * 是后处理NMS和结果输出的基本单元。
 */
struct Detection {
    BoundingBox box; /**< 检测对象的边界框 */
    float conf;      /**< 检测的置信度分数（0~1） */
    int classId;     /**< 检测对象的类别ID（对应COCO标签索引） */

    // 默认构造函数
    Detection() : conf(0.0f), classId(-1) {}

    // 带参构造函数
    Detection(const BoundingBox &box_, float conf_, int classId_)
        : box(box_), conf(conf_), classId(classId_) {}
};

// ============================================================
// PreprocessType 枚举 - 预处理方法选择
// ============================================================
/**
 * @brief 预处理方法枚举
 *
 * 作用：决定图像在送入NPU模型前的缩放策略。
 * - RESIZE：直接拉伸到模型输入尺寸，速度快但可能变形
 * - LETTERBOX：保持宽高比缩放并填充灰色边缘，精度更高
 */
enum class PreprocessType {
    RESIZE = 0,    /**< 直接缩放 - 不保持宽高比，速度优先 */
    LETTERBOX = 1  /**< 保持宽高比缩放（填充灰边） - 精度优先 */
};

// ============================================================
// InputImageType 枚举 - 模型输入格式
// ============================================================
/**
 * @brief 模型输入格式类型
 *
 * 作用：指定RKNN模型接受的输入数据格式。
 * RGB格式适用于大多数场景，NV12格式在某些NPU硬件上有加速优势。
 */
enum class InputImageType {
    RGB = 0,    /**< RGB 格式 (NCHW, uint8-128 量化) - 通用格式 */
    NV12 = 1    /**< NV12 格式 (YUV420SP) - 视频流直接输入格式 */
};

// ============================================================
// KeyPoint 结构体 - 姿态估计关键点
// ============================================================
/**
 * @brief 表示姿态估计中关键点的结构体
 *
 * 作用：存储人体姿态估计中单个关键点的坐标和置信度。
 * COCO格式定义了17个关键点（鼻子、眼睛、耳朵、肩膀、肘部、手腕、髋部、膝盖、脚踝）。
 */
struct KeyPoint {
    float x;          /**< 关键点 X 坐标（归一化或像素值） */
    float y;          /**< 关键点 Y 坐标（归一化或像素值） */
    float confidence; /**< 关键点置信度（0~1），低于阈值的关键点不显示 */

    // 默认构造函数
    KeyPoint() : x(0), y(0), confidence(0) {}

    // 带参构造函数，conf参数默认值为0
    KeyPoint(float x_, float y_, float conf_ = 0)
        : x(x_), y(y_), confidence(conf_) {}
};

// ============================================================
// PoseDetection 结构体 - 姿态检测结果
// ============================================================
/**
 * @brief 表示姿态检测结果的结构体
 *
 * 作用：封装一次姿态估计的完整结果，包含检测框、置信度和所有关键点。
 * 用于人体姿态估计任务，输出人体骨架信息。
 */
struct PoseDetection {
    BoundingBox box;              /**< 人体边界框 */
    float conf;                   /**< 检测置信度 */
    int classId;                  /**< 类别ID (通常是 person，COCO中为0) */
    std::vector<KeyPoint> keypoints; /**< 关键点列表 (COCO: 17个关键点) */

    // 默认构造函数
    PoseDetection() : conf(0.0f), classId(-1) {}

    // 带参构造函数
    PoseDetection(const BoundingBox &box_, float conf_, int classId_)
        : box(box_), conf(conf_), classId(classId_) {}
};

// ============================================================
// Segmentation 结构体 - 实例分割结果
// ============================================================
/**
 * @brief 表示实例分割结果的结构体
 *
 * 作用：封装一次实例分割的完整结果，包含检测框、置信度和像素级分割掩码。
 * mask为单通道cv::Mat，每个像素值为对应的类别ID。
 */
struct Segmentation {
    BoundingBox box;  /**< 对象边界框 */
    float conf;       /**< 检测置信度 */
    int classId;      /**< 类别ID */
    cv::Mat mask;     /**< 分割掩码 (单通道,与原图同尺寸，像素值为类别ID) */

    // 默认构造函数
    Segmentation() : conf(0.0f), classId(-1) {}

    // 带参构造函数
    Segmentation(const BoundingBox &box_, float conf_, int classId_)
        : box(box_), conf(conf_), classId(classId_) {}
};

// ============================================================
// COCO_POSE_SKELETON 骨架连接定义
// ============================================================
/**
 * @brief COCO 人体姿态关键点骨架连接
 *
 * 作用：定义17个关键点之间的骨骼连接关系，用于绘制人体骨架。
 *
 * 17个关键点索引:
 * 0: 鼻子, 1-2: 眼睛, 3-4: 耳朵, 5-6: 肩膀
 * 7-8: 肘部, 9-10: 手腕, 11-12: 髋部, 13-14: 膝盖, 15-16: 脚踝
 */
static const std::vector<std::pair<int, int>> COCO_POSE_SKELETON = {
    // 面部连接：鼻子→左右眼，左右眼→左右耳
    {0,1}, {0,2}, {1,3}, {2,4},
    // 头部到肩膀：左右耳→左右肩
    {3,5}, {4,6},
    // 手臂：左右肩→左右肘，左右肘→左右手腕
    {5,7}, {7,9}, {6,8}, {8,10},
    // 躯干：左右肩连接，左右肩→左右髋，左右髋连接
    {5,6}, {5,11}, {6,12}, {11,12},
    // 腿部：左右髋→左右膝，左右膝→左右脚踝
    {11,13}, {13,15}, {12,14}, {14,16}
};

// ============================================================
// image_format_t 枚举 - 图像像素格式
// ============================================================
/**
 * @brief 图像像素格式枚举
 *
 * 作用：定义图像缓冲区支持的像素格式。
 * 不同NPU硬件对输入格式有不同要求，选择合适格式可提升推理效率。
 */
typedef enum {
    IMAGE_FORMAT_GRAY8,          // 8位灰度图
    IMAGE_FORMAT_RGB888,         // RGB三通道8位
    IMAGE_FORMAT_RGBA8888,       // RGBA四通道8位（含透明度）
    IMAGE_FORMAT_YUV420SP_NV21,  // YUV420半平面格式（NV21，Android常用）
    IMAGE_FORMAT_YUV420SP_NV12,  // YUV420半平面格式（NV12，NPU常用）
} image_format_t;

// ============================================================
// image_buffer_t 结构体 - 图像缓冲区
// ============================================================
/**
 * @brief 图像缓冲区结构体
 *
 * 作用：封装图像数据的内存管理，支持普通内存和DMA缓冲区。
 * 用于RKNN NPU推理时的零拷贝数据传输，通过DMA实现CPU和NPU之间的高效数据共享。
 */
struct image_buffer_t{
    // 如果 virt_address 是已经转换的数据，srcWidth/srcHeight 是转换前的原始宽高
    int srcWidth;           // 原始图像宽度（预处理前）
    int srcHeight;          // 原始图像高度（预处理前）
    int width;              // 当前图像宽度
    int height;             // 当前图像高度
    int width_stride;       // 宽度对齐步长（NPU要求内存对齐）
    int height_stride;      // 高度对齐步长
    image_format_t format;  // 像素格式
    unsigned char* virt_addr; // 虚拟内存地址（CPU可访问的指针）
    int size;               // 缓冲区总大小（字节）
    int fd;                 // DMA缓冲区文件描述符（用于NPU零拷贝）
    DmaBuffer* dmaBuffer;           // DMA缓冲区指针（手动管理）
    std::shared_ptr<DmaBuffer> sp_dmaBuffer; // DMA缓冲区智能指针（自动管理）

    long time;              // 时间戳，用于性能分析

    // 析构函数 - 注意：DMA缓冲区由外部管理，此处不释放
    ~image_buffer_t()
    {
        // 由DmaBufferPool管理生命周期
    }

} ;

// ============================================================
// image_rect_t 结构体 - 图像矩形区域
// ============================================================
/**
 * @brief 图像矩形区域结构体
 *
 * 作用：表示图像中的矩形区域（轴对齐边界框），用于检测结果的整数坐标表示。
 * 与BoundingBox（float坐标）不同，此结构体用于像素级精确定位。
 */
typedef struct {
    int left;    // 左边界X坐标
    int top;     // 上边界Y坐标
    int right;   // 右边界X坐标
    int bottom;  // 下边界Y坐标
} image_rect_t;

// ============================================================
// image_obb_box_t 结构体 - 旋转边界框
// ============================================================
/**
 * @brief 旋转边界框结构体（OBB - Oriented Bounding Box）
 *
 * 作用：表示带旋转角度的边界框，用于旋转目标检测（如文本检测、航拍目标检测）。
 * 相比普通矩形框，旋转框能更紧密地包裹倾斜目标。
 */
typedef struct {
    int x;       // 中心点X坐标
    int y;       // 中心点Y坐标
    int w;       // 框宽度
    int h;       // 框高度
    float angle; // 旋转角度（度）
} image_obb_box_t;

// ============================================================
// rknn_dma_buf 结构体 - RKNN DMA缓冲区
// ============================================================
/**
 * @brief RKNN DMA缓冲区结构体
 *
 * 作用：封装DMA缓冲区信息，用于RKNN NPU的零拷贝推理。
 * DMA缓冲区允许CPU和NPU共享同一块物理内存，避免数据拷贝开销。
 */
typedef struct
{
    char *dma_buf_virt_addr;  // DMA缓冲区虚拟地址
    int dma_buf_fd;           // DMA缓冲区文件描述符
    int size;                 // 缓冲区大小（字节）
} rknn_dma_buf;

// ============================================================
// rknn_app_context_t 结构体 - RKNN应用上下文
// ============================================================
/**
 * @brief RKNN应用上下文结构体
 *
 * 作用：封装RKNN模型推理所需的全部运行时状态，是NPU推理的核心数据结构。
 * 包含模型上下文、输入输出张量属性、内存缓冲区等。
 * 在模型加载时初始化，推理时使用，销毁时释放。
 */
typedef struct {
    rknn_context rknn_ctx;              // RKNN模型上下文句柄（由rknn_init创建）
    rknn_input_output_num io_num;       // 输入输出张量数量
    rknn_tensor_attr* input_attrs;      // 输入张量属性数组（包含形状、量化信息等）
    rknn_tensor_attr* output_attrs;     // 输出张量属性数组

    rknn_dma_buf img_dma_buf;           // 输入图像DMA缓冲区（零拷贝）
    rknn_tensor_mem* input_mems[1];     // 输入张量内存（固定1个输入）
    rknn_tensor_mem* output_mems[9];    // 输出张量内存（YOLO最多9个输出头）
    rknn_tensor_attr* input_native_attrs;   // 原始输入张量属性（量化前）
    rknn_tensor_attr* output_native_attrs;  // 原始输出张量属性（量化前）
    int model_channel;     // 模型输入通道数（RGB=3）
    int model_width;       // 模型输入宽度（如640）
    int model_height;      // 模型输入高度（如640）
    bool is_quant;         // 是否为量化模型（INT8量化可加速推理）
} rknn_app_context_t;

// ============================================================
// object_detect_result 结构体 - 单个检测结果（C接口）
// ============================================================
/**
 * @brief 单个检测结果结构体（C兼容接口）
 *
 * 作用：以C语言兼容的方式表示单个检测结果，用于跨语言接口和底层C代码。
 * 与Detection结构体功能相同，但使用整数坐标和C风格数据类型。
 */
typedef struct {
    image_rect_t box;   // 检测框（整数坐标）
    float prop;         // 置信度分数
    int cls_id;         // 类别ID
} object_detect_result;

// ============================================================
// object_detect_result_list 结构体 - 检测结果列表
// ============================================================
/**
 * @brief 检测结果列表结构体
 *
 * 作用：存储单帧图像的所有检测结果，支持按时间戳排序。
 * 用于多线程场景下按时间顺序处理检测结果，确保时序正确性。
 */
struct object_detect_result_list{
    int id;                         // 帧ID/序列号
    int count;                      // 检测结果数量
    long time;                      // 时间戳（毫秒）
    object_detect_result results[OBJ_NUMB_MAX_SIZE]; // 检测结果数组（最大128个）

    // 重载小于运算符，用于按时间戳排序（升序）
    bool operator<(const object_detect_result_list &other) const
    {
        return time < other.time;
    }
};


#endif // YOLOS_EDGEPLATFORM_COMMON_HPP
