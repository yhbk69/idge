/* 蔡超添加
封装绿色实线框、黄色虚线框的绘制。
主要用于人员点名和设备盘点模块
视频视频统一用绿色实线框
照片比对识别到且不重复的就是绿色实线框  重复的就是黄色虚线框 
*/
#include "draw_utils.h"

void DrawUtils::drawSolidGreenBox(cv::Mat& image, const cv::Rect& box, int thickness) {
    cv::rectangle(image, box, cv::Scalar(0, 255, 0), thickness);
}

void DrawUtils::drawDashedYellowBox(cv::Mat& image, const cv::Rect& box, int thickness) {
    drawDashedRect(image, box, cv::Scalar(0, 255, 255), thickness);
}

void DrawUtils::drawDashedRect(cv::Mat& image, const cv::Rect& rect,
                               const cv::Scalar& color, int thickness,
                               int dash_length, int gap_length) {
    int pattern_length = dash_length + gap_length;
    
    // 上边
    for (int x = rect.x; x < rect.x + rect.width; x += pattern_length) {
        int end_x = std::min(x + dash_length, rect.x + rect.width);
        cv::line(image, cv::Point(x, rect.y), cv::Point(end_x, rect.y), color, thickness);
    }
    
    // 下边
    for (int x = rect.x; x < rect.x + rect.width; x += pattern_length) {
        int end_x = std::min(x + dash_length, rect.x + rect.width);
        int y = rect.y + rect.height - 1;
        cv::line(image, cv::Point(x, y), cv::Point(end_x, y), color, thickness);
    }
    
    // 左边
    for (int y = rect.y; y < rect.y + rect.height; y += pattern_length) {
        int end_y = std::min(y + dash_length, rect.y + rect.height);
        cv::line(image, cv::Point(rect.x, y), cv::Point(rect.x, end_y), color, thickness);
    }
    
    // 右边
    for (int y = rect.y; y < rect.y + rect.height; y += pattern_length) {
        int end_y = std::min(y + dash_length, rect.y + rect.height);
        int x = rect.x + rect.width - 1;
        cv::line(image, cv::Point(x, y), cv::Point(x, end_y), color, thickness);
    }
}

cv::Mat DrawUtils::cropFaceRegion(const cv::Mat& image, const cv::Rect& box) {
    // 确保裁剪区域在图像范围内
    cv::Rect safe_box = box & cv::Rect(0, 0, image.cols, image.rows);
    if (safe_box.area() == 0) {
        return cv::Mat();
    }
    return image(safe_box).clone();
}