/*
 * @Author: Li RF
 * @Date: 2025-03-14 09:16:36
 * @LastEditors: Li RF
 * @LastEditTime: 2025-03-22 16:54:33
 * @Description: 共享类型定义 - 包含系统中所有模块共用的数据结构和枚举
 * Email: 1125962926@qq.com
 * Copyright (c) 2025 Li RF, All Rights Reserved.
 */

#ifndef SHAREDTYPES_H
#define SHAREDTYPES_H

#include <string>

/* 
====================================================
NPU（神经网络处理单元）核心数量
说明：RK3588芯片集成了3个NPU核心，用于AI推理加速
====================================================
*/
const int NPU_CORE_NUM = 3;

/* 
====================================================
2D加速器类型枚举
说明：定义图像处理中的硬件加速方式
====================================================
*/
enum ACCELS_2D {
    ACC_OPENCV = 1,  // OpenCV软件加速（CPU处理）
    ACC_RGA = 2,     // RGA（Rockchip Graphics Acceleration）硬件加速
                     // RGA是Rockchip的2D图形加速引擎，支持图像缩放、旋转、格式转换等
};

/* 
====================================================
输入源格式枚举
说明：定义系统的输入数据类型
====================================================
*/
enum INPUT_FORMAT {
    IN_VIDEO = 1,    // 视频文件输入（如MP4、AVI等格式）
    IN_CAMERA = 2,   // 摄像头实时输入（V4L2设备）
};

/* 
====================================================
视频读取引擎枚举
说明：定义用于读取和解码视频的软件引擎
====================================================
*/
enum READ_ENGINE {
    EN_FFMPEG = 1,   // FFmpeg引擎 - 开源多媒体处理框架，支持多种格式
    EN_OPENCV = 2,   // OpenCV引擎 - 计算机视觉库，提供视频读取功能
};

/* 
====================================================
应用配置结构体
说明：存储所有运行时配置参数，用于控制程序行为
====================================================
*/ 
struct AppConfig {
    // 在屏幕显示 FPS（每秒帧数）信息
    // FPS是衡量视频处理性能的重要指标
    bool screen_fps = false;
    
    // 在终端打印 FPS 信息，用于调试和性能监控
    bool print_fps = false;
    
    // 是否使用 OpenCL（Open Computing Language）
    // OpenCL是一种用于异构平台的并行编程框架，可利用GPU加速计算
    bool opencl = true;
    
    // 是否打印详细的命令行参数信息，用于调试
    bool verbose = false;
    
    // 视频加载引擎，默认为 FFmpeg
    // FFmpeg支持硬件解码，适合处理大分辨率视频
    int read_engine = READ_ENGINE::EN_FFMPEG;
    
    // 输入格式，默认为视频文件
    // 不同的输入格式会影响解码和处理流程
    int input_format = INPUT_FORMAT::IN_VIDEO;
    
    // 2D硬件加速类型，默认为 RGA
    // RGA可以显著提升图像处理性能，减少CPU负载
    int accels_2D = ACCELS_2D::ACC_RGA;
    
    // 处理线程数，默认为1
    // 增加线程数可以提升处理速度，但会增加系统负载
    int threads = 1;
    
    // RKNN（Rockchip Neural Network）模型路径
    // RKNN是Rockchip的AI推理框架，用于NPU加速
    std::string model_path = "";
    
    // 输入源路径（视频文件路径或摄像头设备号）
    std::string input = "";
    
    // 解码器类型，默认为 h264_rkmpp
    // h264_rkmpp是Rockchip的硬件H.264解码器，利用MPP（Media Process Platform）加速
    std::string decodec = "h264_rkmpp";
};


#endif // SHAREDTYPES_H