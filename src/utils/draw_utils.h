#ifndef DRAW_UTILS_H
#define DRAW_UTILS_H

#include <opencv2/opencv.hpp>

class DrawUtils {
public:
    // 绘制绿色实线框（新人脸）
    static void drawSolidGreenBox(cv::Mat& image, const cv::Rect& box, int thickness = 2);
    
    // 绘制黄色虚线框（重复人脸）
    static void drawDashedYellowBox(cv::Mat& image, const cv::Rect& box, int thickness = 2);
    
    // 裁剪人脸区域
    static cv::Mat cropFaceRegion(const cv::Mat& image, const cv::Rect& box);
    
private:
    // 绘制虚线
    static void drawDashedRect(cv::Mat& image, const cv::Rect& rect, 
                              const cv::Scalar& color, int thickness, 
                              int dash_length = 10, int gap_length = 5);
};

#endif // DRAW_UTILS_H