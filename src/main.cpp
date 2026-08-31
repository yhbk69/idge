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

//std::shared_ptr<dpool::ThreadPool> detectPool;

int main(int argc, char *argv[])
{

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


}