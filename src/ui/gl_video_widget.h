// gl_video_widget.h
#ifndef GL_VIDEO_WIDGET_H
#define GL_VIDEO_WIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QMutex>
#include <QQueue>

#include "eglimage_helper.h"

// 传递给渲染线程的帧信息
struct RenderFrame {
    int fd = -1;          // RGBA DMA-BUF fd（dup 后的）
    int width = 0;
    int height = 0;
    int stride = 0;
};

Q_DECLARE_METATYPE(RenderFrame)

class GLVideoWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    explicit GLVideoWidget(QWidget* parent = nullptr);
    ~GLVideoWidget();

public slots:
    void onFrameReady(RenderFrame frame);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;

private:
    void createShaderProgram();
    void drawQuad();

    EglImageHelper egl_;
    QOpenGLShaderProgram* shader_ = nullptr;

    // 双缓冲 EGLImage
    EGLImageKHR egl_images_[2] = {EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
    GLuint textures_[2] = {0, 0};
    int current_idx_ = 0;
    bool frame_updated_ = false;

    RenderFrame pending_frame_;
    RenderFrame prev_frame_;

    QMutex mutex_;
    bool initialized_ = false;
};

#endif
