// gl_video_widget.h
#ifndef GL_VIDEO_WIDGET_H
#define GL_VIDEO_WIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QMutex>
#include <QQueue>
#include <QRect>
#include <QElapsedTimer>

#include "eglimage_helper.h"
#include "EglFramePool.h"

/* 
====================================================
作用：渲染帧数据结构
说明：用于在解码线程和渲染线程之间传递帧信息
====================================================
*/
struct RenderFrame {
    int fd = -1;          // DMA-BUF文件描述符（dup后的副本）
                          // 所有权协议：fd 是 dup 副本，跨线程经队列信号
                          // 投递后由本 widget 独占，最终由 close() 释放；
                          // 生产者 close 自己的原 fd 不影响已投递副本。
                          // ⚠ 若信号投递成功但 widget 已析构（槽永不执行），
                          //   该副本 fd 泄漏，停机时需保证解码器先停。
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

    /*
    ====================================================
    作用：视频内容矩形（本 widget 像素坐标）
    说明：QOpenGLWidget 按宽高比居中渲染，widget 里会留出上下或左右黑边；
          这个矩形就是"画面真正占的那块"。电子围栏用它做归一化坐标 ↔ 像素的换算，
          所以它与 drawQuad 的顶点必须出自同一个算式（见 aspectScale）。
          没有帧信息时退化为整个 widget（比例 1:1 贴合，与旧行为一致）。
    ====================================================
    */
    QRect contentRect() const;

    /*
    ====================================================
    作用：这一路到底有没有真的收到过帧
    说明：PlayerWidget 的"视频待接入"占位靠它决定何时消失。
          video_width_/height_ 只由 GUI 线程的 paintGL 写，本方法也只会被 GUI 线程调
          （占位轮询定时器），同线程读写不并发 ⇒ 不拿 mutex_，理由与 contentRect 相同。
    ====================================================
    */
    bool hasFrame() const { return video_width_ > 0 && video_height_ > 0; }

signals:
    /*
    ====================================================
    作用：视频内容矩形变化通知
    说明：围栏叠加层用它做"归一化坐标 ↔ 像素"的换算基准。widget 尺寸变了、
          或来的帧宽高比变了，都会让画面块在 widget 里挪位/改大小。
          只在算出来的矩形真的变了时才发（paintGL 是逐帧跑的，不判重等于
          每秒给 GUI 线程白加几十次槽调用）。
    ====================================================
    */
    void contentRectChanged(const QRect &contentRect);

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
    作用：算出保持宽高比所需的归一化缩放系数（NDC 半轴，取值 0..1）
    说明：drawQuad 的顶点与 contentRect 都从这里取，letterbox 的口径只能有一处；
          以前它内联在 drawQuad 里，围栏换算另写一遍，两处一旦漂移围栏就贴不准画面。
          只读 video_width_/video_height_，这两个成员只在 GUI 线程被 paintGL 写，
          调用方（drawQuad / 围栏所在的 GUI 线程）也在 GUI 线程，故不需要拿 mutex_。
    ====================================================
    */
    void aspectScale(float &sx, float &sy) const;

    /* 
    ====================================================
    作用：绘制四边形
    说明：使用VBO和EBO绘制纹理映射的四边形
    ====================================================
    */
    void drawQuad();

    /*
    ====================================================
    作用：内容矩形变了就发通知
    说明：paintGL（帧尺寸/宽高比变化）与 resizeGL（视图尺寸变化）末尾各调一次，
          判重后 emit。围栏层是唯一的消费者，它需要的是"画面对应的像素块"。
    ====================================================
    */
    void notifyContentRect();

    // 成员变量
    EglImageHelper egl_;  // EGL图像辅助类
    QOpenGLShaderProgram* shader_ = nullptr;  // 着色器程序
    GLuint vbo_ = 0;  // 顶点缓冲对象（创建一次，复用）
    GLuint ebo_ = 0;  // 索引缓冲对象（创建一次，复用）

    /* 
    ====================================================
    帧池：复用纹理对象，仅重建 EGLImage，避免逐帧 ioctl 开销
    ====================================================
    */
    EglFramePool framePool_;
    GLuint current_texture_ = 0;  // 当前渲染使用的纹理ID
    int video_width_ = 0;         // 视频宽度（用于宽高比计算）
    int video_height_ = 0;        // 视频高度（用于宽高比计算）
    bool frame_updated_ = false;  // 帧更新标记

    QRect lastContentRect_;       // 上次 emit 的内容矩形，notifyContentRect 判重用

    RenderFrame pending_frame_;  // 待 paintGL 消费的帧（fd 由 paintGL 关闭）
    RenderFrame prev_frame_;     // 滞留帧：paintGL 迟迟未执行时兜底关闭其 fd

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

    QElapsedTimer paintProbe_;      // 卡顿探针计时（输出由 IDGE_PERF 控制）
    qint64 lastPaintNs_ = 0;        // 上次 paintGL 起点，用于算间隔
    int droppedFrames_ = 0;         // 上次 paintGL 期间到达却没被画的帧数
    // "收帧速率 vs 上屏速率"对照探针：currentFps_ 只数 onFrameReady（收帧），
    // 界面显示十几帧时必须知道 paintGL 到底跑了多少次才能定性是生产慢还是合成慢。
    int paintCount_ = 0;            // paintGL 累计次数
    qint64 lastRateNs_ = 0;         // 上一个速率窗口起点
};

#endif