#ifndef FACE_ALIGN_H
#define FACE_ALIGN_H

#include <opencv2/opencv.hpp>
#include <cmath>

// ArcFace 112x112标准5点位置
static const float ARCFACE_DST[5][2] = {
    {38.2946f, 51.6963f},  // 左眼
    {73.5318f, 51.5014f},  // 右眼
    {56.0252f, 71.7366f},  // 鼻尖
    {41.5493f, 92.3655f},  // 左嘴角
    {70.7299f, 92.2041f}   // 右嘴角
};

// 使用5个关键点进行人脸对齐
cv::Mat align_face_5points(const cv::Mat& img, const float landmarks[5][2], int crop_size = 112) {
    // 源点（检测到的关键点）
    std::vector<cv::Point2f> src_points;
    for (int i = 0; i < 5; i++) {
        src_points.push_back(cv::Point2f(landmarks[i][0], landmarks[i][1]));
    }
    
    // 目标点（标准位置）
    std::vector<cv::Point2f> dst_points;
    for (int i = 0; i < 5; i++) {
        dst_points.push_back(cv::Point2f(ARCFACE_DST[i][0], ARCFACE_DST[i][1]));
    }
    
    // 计算相似变换矩阵
    cv::Mat transform = cv::estimateAffinePartial2D(src_points, dst_points);
    
    // 应用变换
    cv::Mat aligned;
    cv::warpAffine(img, aligned, transform, cv::Size(crop_size, crop_size));
    
    return aligned;
}

#endif