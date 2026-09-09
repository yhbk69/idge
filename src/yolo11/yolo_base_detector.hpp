// BaseDetector.hpp
#ifndef YOLO_DET_BASE_HPP
#define YOLO_DET_BASE_HPP

/**
 * @file YoloBaseDetector.hpp
 * @brief 边缘平台 YOLO 检测器实现的抽象基类
 *
 * 本头文件定义了运行在 D-Robotics RDKx5 (BPU) 和 Rockchip RK3588 (NPU)
 * 平台上的 YOLO 检测器的统一接口。
 *
 * 设计原则：
 * - Header-only 单文件设计，便于集成
 * - 跨不同 YOLO 版本（v5、v8、v11）的统一 API
 * - 平台特定优化（BPU/NPU）
 * - RAII 资源管理
 *
 */

#include <string>
#include <vector>
#include <memory>
#include <opencv2/opencv.hpp>
#include <fstream>
#include <iostream>
#include "rknn_api.h"
#include "common.hpp"

// ============================================================
// YoloBaseDetector 抽象基类 - YOLO检测器统一接口
// ============================================================
/**
 * @brief YOLO 目标检测器的抽象基类
 *
 * 作用：为所有YOLO检测器实现（v5/v8/v11）提供统一的抽象接口。
 * 采用策略模式，不同平台（RKNN NPU / BPU）的实现类继承此基类。
 * 基类提供通用的预处理工具方法（letterbox、resize、坐标缩放），
 * 派生类只需实现平台特定的模型加载、推理和后处理。
 *
 * 使用方式：
 *   std::unique_ptr<YoloBaseDetector> detector = std::make_unique<RKNNYoloDetector>();
 *   detector->init(modelPath, labelsPath);
 *   detector->detect(image, &results);
 */
class YoloBaseDetector
{
public:
    // ============================================================
    // 析构函数
    // ============================================================
    /**
     * @brief 虚析构函数，确保派生类的正确清理
     *
     * 作用：C++多态的基础要求，通过基类指针删除派生类对象时
     * 确保正确的析构函数被调用，防止内存泄漏。
     */
    virtual ~YoloBaseDetector() = default;

    // ============================================================
    // detect - 纯虚函数：执行目标检测
    // ============================================================
    /**
     * @brief 对提供的图像执行目标检测
     *
     * 作用：检测器的核心方法，完成从图像输入到检测结果输出的完整流程：
     * 1. 图像预处理（letterbox/resize，转换为模型输入格式）
     * 2. NPU模型推理（通过RKNN API调用硬件加速）
     * 3. 后处理（解码输出张量、NMS非极大值抑制）
     * 4. 坐标映射（将检测框从模型输入空间映射回原图空间）
     *
     * @param img 输入图像缓冲区（支持DMA零拷贝格式）
     * @param od_results 输出：检测结果列表（包含边界框、置信度、类别）
     * @param converted 输入图像是否已预处理（true则跳过预处理）
     * @param confThreshold 置信度阈值，低于此值的检测框被过滤（默认0.25）
     * @param nmsThreshold NMS IoU阈值，重叠度高于此值的框被抑制（默认0.45）
     */
    virtual void detect(image_buffer_t *img,
                                          
                                          object_detect_result_list* od_results,
                                          bool converted = false,
                                          float confThreshold = 0.25f,
                                          float nmsThreshold = 0.45f) = 0;

    // ============================================================
    // getInputSize - 纯虚函数：获取模型输入尺寸
    // ============================================================
    /**
     * @brief 获取模型期望的输入尺寸
     *
     * 作用：返回模型训练时的输入分辨率（如640x640）。
     * 用于预处理时确定缩放目标尺寸。
     *
     * @return cv::Size 输入尺寸（宽度，高度）
     */
    virtual cv::Size getInputSize() const = 0;

    // ============================================================
    // getNumClasses - 纯虚函数：获取类别数量
    // ============================================================
    /**
     * @brief 获取模型支持的类别数量
     *
     * 作用：返回模型可检测的物体类别总数。
     * COCO数据集为80类，自定义数据集可能不同。
     *
     * @return int 类别数量
     */
    virtual int getNumClasses() const = 0;

    // ============================================================
    // getClassNames - 纯虚函数：获取类别名称
    // ============================================================
    /**
     * @brief 获取从标签文件加载的类别名称
     *
     * 作用：返回类别名称向量，用于在检测结果中标注物体名称。
     * 例如COCO的80个类别：person、bicycle、car等。
     *
     * @return const std::vector<std::string>& 类别名称向量（不可修改引用）
     */
    virtual const std::vector<std::string> &getClassNames() const = 0;

    // ============================================================
    // setPreprocessType - 纯虚函数：设置预处理方法
    // ============================================================
    /**
     * @brief 设置预处理方法（Resize 或 LetterBox）
     *
     * 作用：切换图像预处理策略。
     * - RESIZE：直接拉伸，速度快但目标可能变形，适用于对精度要求不高的场景
     * - LETTERBOX：保持宽高比填充灰边，精度高但引入额外像素，适用于精确检测
     *
     * @param type PreprocessType 枚举值
     */
    virtual void setPreprocessType(PreprocessType type) = 0;

protected:
    // ============================================================
    // loadClassNames - 静态方法：加载类别名称文件
    // ============================================================
    /**
     * @brief 从文本文件加载类别名称
     *
     * 作用：读取标签文件（如coco.names），每行一个类别名称。
     * 用于在检测结果中将类别ID映射为可读的名称字符串。
     * 支持跨平台兼容，自动处理Windows/Linux换行符差异。
     *
     * @param labelsPath 标签文件路径（如"models/coco.names"）
     * @return std::vector<std::string> 类别名称向量
     */
    static std::vector<std::string> loadClassNames(const std::string &labelsPath)
    {
        std::vector<std::string> classNames;
        std::ifstream infile(labelsPath);

        if (infile)
        {
            std::string line;
            while (std::getline(infile, line))
            {
                // 移除回车符以实现跨平台兼容性（Windows \r\n → Linux \n）
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                classNames.emplace_back(line);
            }
        }
        else
        {
            std::cerr << "[错误] 无法从以下路径加载类别名称: " << labelsPath << std::endl;
        }

        std::cout << "[信息] 已加载 " << classNames.size() << " 个类别名称" << std::endl;
        return classNames;
    }

    // ============================================================
    // letterBox - 静态方法：LetterBox预处理（保持宽高比）
    // ============================================================
    /**
     * @brief 应用 LetterBox 预处理以保持宽高比
     *
     * 作用：将输入图像缩放到目标尺寸，同时保持原始宽高比。
     * 不足部分用灰色（127,127,127）填充，避免引入黑色边框对推理的干扰。
     * 这是YOLO系列模型的标准预处理方式。
     *
     * 处理流程：
     * 1. 计算保持宽高比的缩放因子（取宽高中较小的比例）
     * 2. 计算缩放后的图像尺寸
     * 3. 计算居中偏移量（上下左右均匀填充）
     * 4. 执行resize和copyMakeBorder
     *
     * @param src 源图像（原始输入）
     * @param dst 目标图像（输出：调整大小并填充后的图像）
     * @param targetSize 预处理后的目标尺寸（如{640, 640}）
     * @param xShift 输出：X轴偏移量（用于后处理坐标映射）
     * @param yShift 输出：Y轴偏移量（用于后处理坐标映射）
     * @param scale 输出：缩放因子（用于后处理坐标映射）
     */
    static void letterBox(const cv::Mat &src, cv::Mat &dst, const cv::Size &targetSize,
                          int &xShift, int &yShift, float &scale)
    {
        int srcH = src.rows;   // 原图高度
        int srcW = src.cols;   // 原图宽度
        int dstH = targetSize.height; // 目标高度
        int dstW = targetSize.width;  // 目标宽度

        // 计算缩放因子：取宽高比中较小值，确保图像完全放入目标区域
        scale = std::min(static_cast<float>(dstH) / srcH, static_cast<float>(dstW) / srcW);

        // 计算缩放后的实际尺寸（整数取整）
        int newW = static_cast<int>(srcW * scale);
        int newH = static_cast<int>(srcH * scale);

        // 计算居中填充量（左右、上下尽量均匀分配）
        xShift = (dstW - newW) / 2;   // 左侧填充
        yShift = (dstH - newH) / 2;   // 上方填充
        int xOther = dstW - newW - xShift; // 右侧填充
        int yOther = dstH - newH - yShift; // 下方填充

        // 执行缩放和填充：先resize到目标尺寸，再用灰色(127)填充边框
        cv::resize(src, dst, cv::Size(newW, newH));
        cv::copyMakeBorder(dst, dst, yShift, yOther, xShift, xOther,
                           cv::BORDER_CONSTANT, cv::Scalar(127, 127, 127));
    }

    // ============================================================
    // resizeImage - 静态方法：简单缩放预处理
    // ============================================================
    /**
     * @brief 应用简单的缩放预处理（不保持宽高比）
     *
     * 作用：将图像直接拉伸到目标尺寸，速度快但可能使目标变形。
     * 适用于对速度要求高、精度要求不严格的场景。
     *
     * @param src 源图像
     * @param dst 目标图像（输出：已缩放）
     * @param targetSize 预处理后的目标尺寸
     */
    static void resizeImage(const cv::Mat &src, cv::Mat &dst, const cv::Size &targetSize)
    {
        cv::resize(src, dst, targetSize);
    }

    // ============================================================
    // scaleCoords - 静态方法：坐标映射（模型空间→原图空间）
    // ============================================================
    /**
     * @brief 将检测坐标缩放回原始图像空间
     *
     * 作用：模型推理输出的检测框坐标是在模型输入尺寸（如640x640）下的坐标，
     * 需要逆向映射回原始图像的坐标系。此方法处理letterbox预处理引入的
     * 缩放和偏移，还原真实的目标位置和大小。
     *
     * 映射公式：
     *   原图坐标 = (模型坐标 - 偏移量) / 缩放因子
     *
     * @param detection 要缩放的检测结果（就地修改）
     * @param xShift letterbox填充导致的X轴偏移量
     * @param yShift letterbox填充导致的Y轴偏移量
     * @param scale 预处理时应用的缩放因子
     */
    static void scaleCoords(Detection &detection, int xShift, int yShift, float scale)
    {
        // 逆向映射：先减去偏移量，再除以缩放因子
        detection.box.x = (detection.box.x - xShift) / scale;
        detection.box.y = (detection.box.y - yShift) / scale;
        detection.box.width = detection.box.width / scale;
        detection.box.height = detection.box.height / scale;
    }

    // ============================================================
    // clamp - 静态方法：值裁剪工具
    // ============================================================
    /**
     * @brief 将浮点值裁剪到指定整数范围
     *
     * 作用：用于检测结果的边界处理，确保坐标值不超出图像边界。
     * 将float值四舍五入为int后，限制在[min, max]范围内。
     *
     * @param val 输入浮点值
     * @param min 最小值
     * @param max 最大值
     * @return int 裁剪后的整数值
     */
    static int clamp(float val, int min, int max) { return val > min ? (val < max ? val : max) : min; }


};

#endif // YOLO_DET_BASE_HPP
