/**
 * @file draw_utils.h
 * @brief base/imaging 域 —— OpenCV Mat 上的检测框绘制与人脸区域裁剪（无状态工具类）
 */
// 文件职责：人脸/目标框绘制工具类的声明，封装绿色实线框（唯一新人脸）、
// 黄色虚线框（重复人脸）绘制与人脸区域裁剪，供人员点名与设备盘点模块复用。
#ifndef DRAW_UTILS_H
#define DRAW_UTILS_H

#include <opencv2/opencv.hpp>

/**
 * @brief 全静态工具类：无成员、无状态、无需构造，因此也没有线程安全约束——
 *        约束落在**入参**上：三个函数都原地写调用方给的 Mat，同一 Mat 并发绘制会互相踩踏。
 *        画法口径（绿=唯一、黄虚线=重复）由本类固化，业务侧不要再自己选颜色。
 */
class DrawUtils {
public:
    // 绘制绿色实线框（新人脸）
    // image 为 8UC3/8UC4 的 BGR 语义；box 可越界（OpenCV 自行裁剪）；thickness<=0 视为填充
    static void drawSolidGreenBox(cv::Mat& image, const cv::Rect& box, int thickness = 2);
    
    // 绘制黄色虚线框（重复人脸）
    // 虚线周期固定用 drawDashedRect 的 10px 实段 / 5px 空段，本函数不提供调整入口
    static void drawDashedYellowBox(cv::Mat& image, const cv::Rect& box, int thickness = 2);
    
    // 裁剪人脸区域
    // 返回 clone 出来的深拷贝（可脱离 image 存活）；与图像无交集时返回空 Mat，调用方须判 .empty()
    static cv::Mat cropFaceRegion(const cv::Mat& image, const cv::Rect& box);
    
private:
    // 绘制虚线
    // dash/gap 默认 10/5 的取值理由见 .cpp：一个周期 15px，再小就与实线框难以肉眼区分
    static void drawDashedRect(cv::Mat& image, const cv::Rect& rect, 
                              const cv::Scalar& color, int thickness, 
                              int dash_length = 10, int gap_length = 5);
};

#endif // DRAW_UTILS_H