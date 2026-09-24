/*-------------------------------------------
                Includes
-------------------------------------------*/
// ============================================================================
// main.cpp - 程序入口点
// ============================================================================
//
// 本程序支持两种运行模式：
//   1. CLI 模式：命令行直接运行检测（无 GUI）
//   2. GUI 模式：Qt 图形界面（默认）
//
// CLI 模式用法：
//   ./idge -m model/yolo11n.rknn -i input.mp4
//
// GUI 模式：
//   ./idge
//
// ============================================================================

#include <chrono>
#include <functional>
#include "runtime_paths.h"
#include "frmmain.h"
#include "appinit.h"
#include "qthelper.h"

#include "qdebug.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/signal.h>
#include <algorithm>
#include <map>
#include <thread>
#include <chrono>
#include <opencv2/opencv.hpp>
#include <iostream>
#include <QTimer>
#include <QDateTime>

// RGA (Rocket Graphics Acceleration) 头文件
// 用于硬件加速的图像色彩转换和缩放
#include "im2d.h"
#include "rga.h"
#include "RgaUtils.h"

// RKNN (Rockchip Neural Network) 头文件
// 用于 NPU 硬件加速的 AI 推理
#include "rknn_api.h"

//#include "rkmedia/utils/mpp_decoder.h"
//#include "rkmedia/utils/mpp_encoder.h"

// MediaKit 头文件（可能用于流媒体协议支持）
#include "mk_mediakit.h"
#include <QWidget>
#include <gelf.h>
#include <GL/gl.h>
#include "easy_timer.h"
#include "queue/priority_queue.h"
#include "gl_video_widget.h"
#include "ThreadPool.hpp"

#include "config/parse_config.hpp"
#include "yolo11/yolo11_model.hpp"
#include "alarm_manager.h"
#include "database/database_manager.h"

//std::shared_ptr<dpool::ThreadPool> detectPool;

// ============================================================================
// 信号处理 - 优雅关闭
// ============================================================================
// 处理 SIGTERM (kill) 和 SIGINT (Ctrl+C) 信号
// 收到信号后设置标志位，让主循环优雅退出
// ============================================================================
static volatile sig_atomic_t g_shutdownRequested = 0;

static void signalHandler(int signum)
{
    Q_UNUSED(signum);
    g_shutdownRequested = 1;
    qInfo() << "Shutdown signal received, cleaning up...";
}

// ============================================================================
// 判断是否为 CLI 模式
// ============================================================================
// 通过检查命令行参数判断：
//   -m / --model_path: 指定模型路径
//   -i / --input: 指定输入（视频文件或摄像头）
//   -h / --help: 显示帮助
//
// 如果包含这些参数，则进入 CLI 模式（无 GUI）
// ============================================================================
static bool isCliMode(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-m" || arg == "--model_path" ||
            arg == "-i" || arg == "--input" ||
            arg == "-h" || arg == "--help") {
            return true;
        }
    }
    return false;
}

// ============================================================================
// CLI 模式运行
// ============================================================================
// 不启动 Qt GUI，直接在命令行中运行检测
//
// 流程：
//   1. 解析命令行参数
//   2. 加载 RKNN 模型
//   3. 打开视频源（文件或摄像头）
//   4. 循环读取帧 → 检测 → 打印结果
//
// 参数：
//   -m: 模型路径（如 model/yolo11n.rknn）
//   -i: 输入源（视频文件路径或摄像头编号）
//   -d: 解码器（默认 h264_rkmpp）
//   -v: 显示 FPS
// ============================================================================
static int runCli(int argc, char *argv[])
{
    // 创建 QCoreApplication（无 GUI 的 Qt 应用）
    QCoreApplication a(argc, argv);

    // 解析命令行参数
    ConfigParser parser;
    AppConfig config = parser.parse_arguments(argc, argv);
    if (config.verbose) {
        parser.printConfig(config);
    }

    const std::string labelsPath = "model/coco_80_labels_list.txt";

    // 加载 RKNN 模型
    printf("[CLI] Loading model: %s\n", config.model_path.c_str());
    auto model = std::make_shared<YOLO11Model>(
        config.model_path,
        labelsPath,
        RKNN_NPU_CORE_0);  // 使用 NPU 核心 0
    printf("[CLI] Model loaded.\n");

    // 打开视频源
    cv::VideoCapture cap;
    bool isCamera = (config.input_format == INPUT_FORMAT::IN_CAMERA);

    if (isCamera) {
        // 打开摄像头
        int camIdx = std::stoi(config.input);
        printf("[CLI] Opening camera /dev/video%d ...\n", camIdx);
        if (!cap.open(camIdx)) {
            fprintf(stderr, "[CLI] Failed to open camera %d\n", camIdx);
            return -1;
        }
    } else {
        // 打开视频文件
        printf("[CLI] Opening video: %s\n", config.input.c_str());
        if (!cap.open(config.input)) {
            fprintf(stderr, "[CLI] Failed to open video: %s\n", config.input.c_str());
            return -1;
        }
    }

    // 主循环：读取帧 → 检测 → 打印结果
    int frameCount = 0;
    TIMER fpsTimer;
    fpsTimer.tik();

    while (true) {
        cv::Mat frame;
        if (!cap.read(frame)) {
            printf("[CLI] End of stream.\n");
            break;
        }
        if (frame.empty()) {
            continue;
        }

        // 将 OpenCV Mat 转换为 RKNN 输入格式
        image_buffer_t imgBuf;
        memset(&imgBuf, 0, sizeof(imgBuf));
        imgBuf.width = frame.cols;
        imgBuf.height = frame.rows;
        imgBuf.srcWidth = frame.cols;
        imgBuf.srcHeight = frame.rows;
        imgBuf.width_stride = frame.cols;
        imgBuf.height_stride = frame.rows;
        imgBuf.format = IMAGE_FORMAT_RGB888;
        imgBuf.virt_addr = frame.data;
        imgBuf.size = frame.total() * frame.elemSize();
        imgBuf.time = cv::getTickCount();

        // 执行目标检测
        object_detect_result_list odResults;
        model->detect(&imgBuf, &odResults, false);

        frameCount++;

        // 打印 FPS（每 30 帧）
        if (config.print_fps && frameCount % 30 == 0) {
            fpsTimer.tok();
            float elapsed = fpsTimer.get_time();   // 最近 30 帧耗时（毫秒）
            // 30000 = 30 帧 × 1000（毫秒/秒），即 FPS = 帧数×1000/耗时(ms)
            float fps = 30000.0f / elapsed;
            fpsTimer.tik();
            printf("[CLI] FPS: %.1f  Frame: %d  Detected: %d\n",
                   fps, frameCount, odResults.count);
        }

        // 打印检测结果（每 30 帧）
        if (frameCount % 30 == 0) {
            printf("----- Frame %d -----\n", frameCount);
            for (int i = 0; i < odResults.count; i++) {
                auto &r = odResults.results[i];
                printf("  [%d] cls=%d  box=(%d,%d,%d,%d)  conf=%.3f\n",
                       i, r.cls_id,
                       r.box.left, r.box.top,
                       r.box.right, r.box.bottom,
                       r.prop);
            }
        }
    }

    fpsTimer.tok();
    printf("[CLI] Done. Total frames: %d, elapsed: %.2f s\n",
           frameCount, fpsTimer.get_time() / 1000.0f);

    return 0;
}

// ============================================================================
// 主函数
// ============================================================================
// 根据命令行参数选择运行模式：
//   - CLI 模式：直接运行检测，无 GUI
//   - GUI 模式：启动 Qt 图形界面
// ============================================================================
int main(int argc, char *argv[])
{
    // 运行时数据路径一次性迁移（旧布局 → data/），必须先于一切文件打开
    RuntimePaths::migrateLegacy();

    // 检查是否为 CLI 模式
    if (isCliMode(argc, argv)) {
        return runCli(argc, argv);
    }

    //detectPool= std::make_shared<dpool::ThreadPool>(1);
    
    //Config::getInstance().load("../config.json");

    //StreamLoaderManager &manager = StreamLoaderManager::getInstance();
    
    // 设置 Qt 使用 EGL 渲染（硬件加速 OpenGL）
    // QT_XCB_GL_INTEGRATION=xcb_egl: 使用 EGL 后端，支持硬件加速
    qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");

    QApplication a(argc, argv);

    // ============================================================================
    // 配置 OpenGL ES 格式
    // ============================================================================
    // 使用 OpenGL ES 3.0（支持更多特性）
    // 双缓冲（避免画面撕裂）
    // 调试上下文（开发时启用，生产环境可关闭）
    // ============================================================================
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGLES);
    fmt.setVersion(3, 0);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);

    fmt.setOptions(QSurfaceFormat::DebugContext);  // 调试用
    QSurfaceFormat::setDefaultFormat(fmt);

    // 注册自定义类型（用于 Qt 信号/槽跨线程传递）
    qRegisterMetaType<RenderFrame>("RenderFrame");

    // 初始化 Qt 辅助工具
    QtHelper::initMain();
    QtHelper::initOpenGL(2, true, false);
    AppInit::Instance()->start();

    // 设置字体和编码
    QtHelper::setFont(16);  // 统一基础字号 16px（字阶：13提示/16正文/18卡片标题/22页面标题/30大数字）
    QtHelper::setCode();

    // 初始化数据库
    AlarmManager::instance().initDatabase(RuntimePaths::database());
    AlarmManager::instance().loadAlarmsFromDatabase();

    // 注册信号处理（优雅关闭）
    signal(SIGTERM, signalHandler);  // kill 命令
    signal(SIGINT, signalHandler);   // Ctrl+C

    // 创建并显示主窗口
    frmMain w;

    // 定时备份数据库（每天凌晨 3 点）
    // 使用 QTimer 定期检查是否需要备份
    QTimer backupTimer;
    QObject::connect(&backupTimer, &QTimer::timeout, []() {
        QDateTime now = QDateTime::currentDateTime();
        // 每天凌晨 3 点执行备份
        if (now.time().hour() == 3 && now.time().minute() == 0) {
            QString backupPath = RuntimePaths::backupFile(now.toString("yyyyMMdd"));
            if (DatabaseManager::instance().backup(backupPath)) {
                qInfo() << "Daily backup completed:" << backupPath;
            }
        }
    });
    backupTimer.start(60000);  // 每分钟检查一次

    // 启动时立即执行一次备份（确保有备份）
    {
        QString backupPath = RuntimePaths::backupFile(
            QDateTime::currentDateTime().toString("yyyyMMdd"));
        DatabaseManager::instance().backup(backupPath);
    }

    // 定时检查关闭信号（收到 SIGTERM/SIGINT 时优雅退出）
    QTimer shutdownCheckTimer;
    QObject::connect(&shutdownCheckTimer, &QTimer::timeout, []() {
        if (g_shutdownRequested) {
            qApp->quit();
        }
    });
    shutdownCheckTimer.start(500);  // 每 500ms 检查一次

    QtHelper::setFormInCenter(&w);
    w.show();

    // 进入 Qt 事件循环
    a.exec();

    // 程序退出前清理和同步
    qInfo() << "Shutting down, cleaning up...";
    AlarmManager::instance().syncToDatabase();
    AlarmManager::instance().cleanOldData(30);

    return 0;
}
