// gl_video_widget.h
#ifndef GL_VIDEO_WIDGET_H
#define GL_VIDEO_WIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QMutex>
#include <QQueue>
#include <QElapsedTimer>

#include "eglimage_helper.h"

/* 
====================================================
作用：渲染帧数据结构
说明：用于在解码线程和渲染线程之间传递帧信息
====================================================
*/
struct RenderFrame {
    int fd = -1;          // DMA-BUF文件描述符（dup后的副本）
    int width = 0;        // 图像宽度
    int height = 0;       // 图像高度
    int stride = 0;       // 行跨度（字节数）
};

// 注册自定义类型，用于Qt信号槽系统
Q_DECLARE_METATYPE(RenderFrame)

/* 
====================================================
作用：OpenGL视频显示组件
说明：继承QOpenGLWidget，使用OpenGL ES渲染视频帧
硬件概念：利用DMA-BUF实现零拷贝渲染，GPU直接访问视频解码器输出
====================================================
*/
class GLVideoWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    /* 
    ====================================================
    作用：构造函数
    说明：初始化OpenGL视频显示组件
    参数：parent - 父窗口指针
    ====================================================
    */
    explicit GLVideoWidget(QWidget* parent = nullptr);

    /* 
    ====================================================
    作用：析构函数
    说明：释放OpenGL和EGL资源
    ====================================================
    */
    ~GLVideoWidget();

public slots:
    /* 
    ====================================================
    作用：帧就绪槽函数
    说明：接收解码线程发送的帧数据，准备渲染
    参数：frame - 渲染帧数据结构
    ====================================================
    */
    void onFrameReady(RenderFrame frame);

protected:
    /* 
    ====================================================
    作用：OpenGL初始化回调
    说明：创建着色器程序和OpenGL资源
    ====================================================
    */
    void initializeGL() override;

    /* 
    ====================================================
    作用：OpenGL渲染回调
    说明：执行实际的帧渲染操作
    ====================================================
    */
    void paintGL() override;

    /* 
    ====================================================
    作用：OpenGL视口调整回调
    说明：处理窗口大小变化，更新视口设置
    参数：w - 新宽度，h - 新高度
    ====================================================
    */
    void resizeGL(int w, int h) override;

private:
    /* 
    ====================================================
    作用：创建着色器程序
    说明：编译顶点和片段着色器，链接为着色器程序
    ====================================================
    */
    void createShaderProgram();

    /* 
    ====================================================
    作用：绘制四边形
    说明：使用VBO和EBO绘制纹理映射的四边形
    ====================================================
    */
    void drawQuad();

    // 成员变量
    EglImageHelper egl_;  // EGL图像辅助类
    QOpenGLShaderProgram* shader_ = nullptr;  // 着色器程序

    /* 
    ====================================================
    双缓冲EGLImage
    说明：使用两个纹理实现双缓冲，避免渲染撕裂
    ====================================================
    */
    EGLImageKHR egl_images_[2] = {EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};  // 两个EGL图像
    GLuint textures_[2] = {0, 0};  // 两个纹理
    int current_idx_ = 0;  // 当前使用的缓冲区索引
    bool frame_updated_ = false;  // 帧更新标记

    RenderFrame pending_frame_;  // 待处理的帧
    RenderFrame prev_frame_;     // 上一帧

    QMutex mutex_;  // 互斥锁，保护线程安全
    bool initialized_ = false;  // 初始化标记

    /* 
    ====================================================
    FPS统计
    说明：计算并显示帧率信息
    ====================================================
    */
    QElapsedTimer fpsTimer_;  // 帧计时器
    int fpsFrameCount_ = 0;   // 帧计数
    float currentFps_ = 0.0f; // 当前帧率
};

#endif