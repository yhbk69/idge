/*-------------------------------------------
                Includes
-------------------------------------------*/
#include <chrono>
#include <functional>
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
#include <algorithm>
#include <map>
#include <thread>
#include <chrono>
#include <opencv2/opencv.hpp>
#include <iostream>

#include "im2d.h"
#include "rga.h"
#include "RgaUtils.h"

#include "rknn_api.h"

//#include "rkmedia/utils/mpp_decoder.h"
//#include "rkmedia/utils/mpp_encoder.h"

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

//std::shared_ptr<dpool::ThreadPool> detectPool;

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

static int runCli(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    ConfigParser parser;
    AppConfig config = parser.parse_arguments(argc, argv);
    if (config.verbose) {
        parser.printConfig(config);
    }

    const std::string labelsPath = "model/coco_80_labels_list.txt";

    printf("[CLI] Loading model: %s\n", config.model_path.c_str());
    auto model = std::make_shared<YOLO11Model>(
        config.model_path,
        labelsPath,
        RKNN_NPU_CORE_0);
    printf("[CLI] Model loaded.\n");

    cv::VideoCapture cap;
    bool isCamera = (config.input_format == INPUT_FORMAT::IN_CAMERA);

    if (isCamera) {
        int camIdx = std::stoi(config.input);
        printf("[CLI] Opening camera /dev/video%d ...\n", camIdx);
        if (!cap.open(camIdx)) {
            fprintf(stderr, "[CLI] Failed to open camera %d\n", camIdx);
            return -1;
        }
    } else {
        printf("[CLI] Opening video: %s\n", config.input.c_str());
        if (!cap.open(config.input)) {
            fprintf(stderr, "[CLI] Failed to open video: %s\n", config.input.c_str());
            return -1;
        }
    }

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

        object_detect_result_list odResults;
        model->detect(&imgBuf, &odResults, false);

        frameCount++;

        if (config.print_fps && frameCount % 30 == 0) {
            fpsTimer.tok();
            float elapsed = fpsTimer.get_time();
            float fps = 30000.0f / elapsed;
            fpsTimer.tik();
            printf("[CLI] FPS: %.1f  Frame: %d  Detected: %d\n",
                   fps, frameCount, odResults.count);
        }

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

int main(int argc, char *argv[])
{
    if (isCliMode(argc, argv)) {
        return runCli(argc, argv);
    }

    //detectPool= std::make_shared<dpool::ThreadPool>(1);
    
    //Config::getInstance().load("../config.json");

    //StreamLoaderManager &manager = StreamLoaderManager::getInstance();
    
    qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");

    QApplication a(argc, argv);

    //设置全局 OpenGL ES 格式
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGLES);
    fmt.setVersion(3, 0);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);

    fmt.setOptions(QSurfaceFormat::DebugContext);  // 调试用
    QSurfaceFormat::setDefaultFormat(fmt);

    // 注册类型
    //qRegisterMetaType<VideoFrame>("VideoFrame");
    // 注册自定义类型
    qRegisterMetaType<RenderFrame>("RenderFrame");

    QtHelper::initMain();
    QtHelper::initOpenGL(2, true, false);
    AppInit::Instance()->start();

    QtHelper::setFont();
    QtHelper::setCode();

    frmMain w;

    QtHelper::setFormInCenter(&w);
    w.show();

    a.exec();

    return 0;
}
