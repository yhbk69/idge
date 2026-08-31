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

/**
 * @brief YOLO 目标检测器的抽象基类
 *
 * 此类为所有 YOLO 检测器实现提供统一接口。
 * 派生类必须实现平台特定的初始化、推理和后处理方法。
 */
class YoloBaseDetector
{
public:
    /**
     * @brief 虚析构函数，确保派生类的正确清理
     */
    virtual ~YoloBaseDetector() = default;

    /**
     * @brief 对提供的图像执行目标检测
     *
     * @param image 输入 OpenCV Mat 图像（BGR 格式）
     * @param confThreshold 置信度阈值，用于过滤检测结果（默认 0.25）
     * @param nmsThreshold NMS IoU 阈值（默认 0.45）
     * @return std::vector<Detection> 检测结果向量
     */
    virtual void detect(image_buffer_t *img,
                                          
                                          object_detect_result_list* od_results,
                                          bool converted = false,
                                          float confThreshold = 0.25f,
                                          float nmsThreshold = 0.45f) = 0;

    /**
     * @brief 获取模型期望的输入尺寸
     *
     * @return cv::Size 输入尺寸（宽度，高度）
     */
    virtual cv::Size getInputSize() const = 0;

    /**
     * @brief 获取模型支持的类别数量
     *
     * @return int 类别数量
     */
    virtual int getNumClasses() const = 0;

    /**
     * @brief 获取从标签文件加载的类别名称
     *
     * @return const std::vector<std::string>& 类别名称向量
     */
    virtual const std::vector<std::string> &getClassNames() const = 0;

    /**
     * @brief 设置预处理方法（Resize 或 LetterBox）
     *
     * @param type PreprocessType 枚举值
     */
    virtual void setPreprocessType(PreprocessType type) = 0;

protected:
    /**
     * @brief 从文本文件加载类别名称
     *
     * @param labelsPath 标签文件路径
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
                // 移除回车符以实现跨平台兼容性
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

    /**
     * @brief 应用 LetterBox 预处理以保持宽高比
     *
     * @param src 源图像
     * @param dst 目标图像（调整大小并填充）
     * @param targetSize 预处理后的目标尺寸
     * @param xShift 输出：由于填充导致的 X 轴偏移
     * @param yShift 输出：由于填充导致的 Y 轴偏移
     * @param scale 输出：应用的缩放因子
     */
    static void letterBox(const cv::Mat &src, cv::Mat &dst, const cv::Size &targetSize,
                          int &xShift, int &yShift, float &scale)
    {
        int srcH = src.rows;
        int srcW = src.cols;
        int dstH = targetSize.height;
        int dstW = targetSize.width;

        // 计算缩放因子以使图像适应目标尺寸
        scale = std::min(static_cast<float>(dstH) / srcH, static_cast<float>(dstW) / srcW);

        // 计算新尺寸
        int newW = static_cast<int>(srcW * scale);
        int newH = static_cast<int>(srcH * scale);

        // 计算填充
        xShift = (dstW - newW) / 2;
        yShift = (dstH - newH) / 2;
        int xOther = dstW - newW - xShift;
        int yOther = dstH - newH - yShift;

        // 调整大小并添加填充
        cv::resize(src, dst, cv::Size(newW, newH));
        cv::copyMakeBorder(dst, dst, yShift, yOther, xShift, xOther,
                           cv::BORDER_CONSTANT, cv::Scalar(127, 127, 127));
    }

    /**
     * @brief 应用简单的缩放预处理
     *
     * @param src 源图像
     * @param dst 目标图像（已缩放）
     * @param targetSize 预处理后的目标尺寸
     */
    static void resizeImage(const cv::Mat &src, cv::Mat &dst, const cv::Size &targetSize)
    {
        cv::resize(src, dst, targetSize);
    }

    /**
     * @brief 将检测坐标缩放回原始图像空间
     *
     * @param detection 要缩放的检测结果
     * @param xShift letterbox 填充导致的 X 轴偏移
     * @param yShift letterbox 填充导致的 Y 轴偏移
     * @param scale 预处理的缩放因子
     */
    static void scaleCoords(Detection &detection, int xShift, int yShift, float scale)
    {
        detection.box.x = (detection.box.x - xShift) / scale;
        detection.box.y = (detection.box.y - yShift) / scale;
        detection.box.width = detection.box.width / scale;
        detection.box.height = detection.box.height / scale;
    }

    static int clamp(float val, int min, int max) { return val > min ? (val < max ? val : max) : min; }


};

#endif // YOLO_DET_BASE_HPP
