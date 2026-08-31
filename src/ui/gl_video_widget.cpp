// gl_video_widget.cpp
#include "gl_video_widget.h"
#include <QDebug>
#include <QMutexLocker>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <QPainter>

static const char* VS_SRC = R"(
    attribute vec2 aPos;
    attribute vec2 aTexCoord;
    varying vec2 vTexCoord;
    void main() {
        gl_Position = vec4(aPos, 0.0, 1.0);
        vTexCoord = aTexCoord;
    }
)";

// RGBA 纹理直接输出
static const char* FS_SRC = R"(
    precision mediump float;
    varying vec2 vTexCoord;
    uniform sampler2D tex;
    void main() {
        gl_FragColor = texture2D(tex, vTexCoord);
    }
)";

GLVideoWidget::GLVideoWidget(QWidget* parent)
    : QOpenGLWidget(parent)
{
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGLES);
    fmt.setVersion(2, 0);
    setFormat(fmt);
    setMinimumSize(320, 240);
}

GLVideoWidget::~GLVideoWidget()
{
    makeCurrent();
    for (int i = 0; i < 2; i++) {
        if (egl_images_[i] != EGL_NO_IMAGE_KHR)
            egl_.destroyImage(egl_images_[i]);
        if (textures_[i])
            glDeleteTextures(1, &textures_[i]);
    }
    delete shader_;
    doneCurrent();

    // 关闭残留的 fd
    if (pending_frame_.fd >= 0) ::close(pending_frame_.fd);
    if (prev_frame_.fd >= 0) ::close(prev_frame_.fd);
}

void GLVideoWidget::initializeGL()
{
    initializeOpenGLFunctions();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    if (!egl_.init()) {
        qWarning() << "EGL DMA-BUF import not available";
        return;
    }

    createShaderProgram();

    // 创建两个纹理（双缓冲）
    for (int i = 0; i < 2; i++) {
        glGenTextures(1, &textures_[i]);
        glBindTexture(GL_TEXTURE_2D, textures_[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    initialized_ = true;
    qDebug() << "GLVideoWidget initialized";
}

void GLVideoWidget::createShaderProgram()
{
    shader_ = new QOpenGLShaderProgram(this);
    shader_->addShaderFromSourceCode(QOpenGLShader::Vertex, VS_SRC);
    shader_->addShaderFromSourceCode(QOpenGLShader::Fragment, FS_SRC);
    shader_->bindAttributeLocation("aPos", 0);
    shader_->bindAttributeLocation("aTexCoord", 1);
    shader_->link();
}

void GLVideoWidget::onFrameReady(RenderFrame frame)
{
    QMutexLocker lock(&mutex_);

    // 关闭上上帧的 fd
    if (prev_frame_.fd >= 0) {
        ::close(prev_frame_.fd);
        prev_frame_.fd = -1;
    }

    prev_frame_ = pending_frame_;
    pending_frame_ = frame;
    frame_updated_ = true;

    update();
}

void GLVideoWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);

    QMutexLocker lock(&mutex_);
    if (!initialized_ || pending_frame_.fd < 0) return;

    if (frame_updated_) {
        int back = 1 - current_idx_;

        // 销毁旧 EGLImage
        if (egl_images_[back] != EGL_NO_IMAGE_KHR) {
            egl_.destroyImage(egl_images_[back]);
            egl_images_[back] = EGL_NO_IMAGE_KHR;
        }

        // 导入新的 RGBA DMA-BUF
        egl_images_[back] = egl_.importRGBA(
            pending_frame_.fd,
            pending_frame_.width,
            pending_frame_.height,
            pending_frame_.stride);

        if (egl_images_[back] != EGL_NO_IMAGE_KHR) {
            egl_.bindToTexture(egl_images_[back], textures_[back]);
            current_idx_ = back;
        }

        frame_updated_ = false;
    }

    // 绘制当前帧
    drawQuad();
}

void GLVideoWidget::drawQuad()
{
    if (egl_images_[current_idx_] == EGL_NO_IMAGE_KHR) return;

    shader_->bind();

    // 保持宽高比
    float video_aspect = (float)pending_frame_.width / pending_frame_.height;
    float view_aspect = (float)width() / height();
    float sx = 1.0f, sy = 1.0f;

    if (video_aspect > view_aspect)
        sy = view_aspect / video_aspect;
    else
        sx = video_aspect / view_aspect;

    GLfloat vertices[] = {
        -sx, -sy,  0.0f, 1.0f,
         sx, -sy,  1.0f, 1.0f,
         sx,  sy,  1.0f, 0.0f,
        -sx,  sy,  0.0f, 0.0f,
    };
    GLuint indices[] = {0, 1, 2, 2, 3, 0};

    GLuint vbo, ebo;
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
    glEnableVertexAttribArray(1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures_[current_idx_]);
    shader_->setUniformValue("tex", 0);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindTexture(GL_TEXTURE_2D, 0);
    shader_->release();

    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ebo);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::red);
    painter.drawText(100,100 , "afasf");
    painter.end();

}

void GLVideoWidget::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
}
