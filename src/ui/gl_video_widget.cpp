// gl_video_widget.cpp
//
// ============================================================================
// GLVideoWidget - OpenGL ES 视频渲染组件
// ============================================================================
//
// 【作用】
//   将 DMA-BUF 中的视频帧渲染到 Qt 窗口上。
//   使用 OpenGL ES 2.0 + EGL DMA-BUF 导入，实现零拷贝渲染。
//
// 【渲染流程】
//   1. 接收 RenderFrame（包含 DMA-BUF fd）
//   2. 通过 EGL 将 DMA-BUF 导入为 EGLImage
//   3. 将 EGLImage 绑定为 OpenGL 纹理
//   4. 用着色器将纹理绘制到屏幕上
//
// 【为什么用 OpenGL 而不是 QPainter？】
//   - QPainter 需要 CPU 拷贝像素（CPU→GPU），效率低
//   - OpenGL 可以直接使用 DMA-BUF（GPU→GPU），零拷贝
//   - 对于 4K 视频，零拷贝可以节省 50%+ 的 CPU 使用率
//
// 【双缓冲机制】
//   - 两个 EGLImage 交替使用
//   - 当一个正在渲染时，另一个可以被更新
//   - 避免画面撕裂和闪烁
//
// ============================================================================

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

// ============================================================================
// 顶点着色器
// ============================================================================
// 输入：
//   - aPos: 顶点坐标（-1~1 归一化坐标系）
//   - aTexCoord: 纹理坐标（0~1，左下角为原点）
// 输出：
//   - vTexCoord: 传递给片元着色器的纹理坐标
//
// 作用：
//   将顶点坐标传递给 GPU，GPU 会自动插值生成三角形内的每个像素
// ============================================================================
static const char* VS_SRC = R"(
    attribute vec2 aPos;
    attribute vec2 aTexCoord;
    varying vec2 vTexCoord;
    void main() {
        gl_Position = vec4(aPos, 0.0, 1.0);
        vTexCoord = aTexCoord;
    }
)";

// ============================================================================
// 片元着色器
// ============================================================================
// 输入：
//   - vTexCoord: 插值后的纹理坐标
//   - tex: 纹理（就是 DMA-BUF 导入的那个）
// 输出：
//   - gl_FragColor: 这个像素的最终颜色
//
// 作用：
//   对纹理进行采样，获取每个像素的颜色值
//   texture2D(tex, vTexCoord) 会从 DMA-BUF 中读取像素数据
// ============================================================================
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
    // 设置 OpenGL ES 2.0 渲染格式
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGLES);
    fmt.setVersion(2, 0);
    setFormat(fmt);
    setMinimumSize(320, 240);
}

GLVideoWidget::~GLVideoWidget()
{
    // 清理 EGL 资源
    makeCurrent();
    for (int i = 0; i < 2; i++) {
        if (egl_images_[i] != EGL_NO_IMAGE_KHR)
            egl_.destroyImage(egl_images_[i]);
        if (textures_[i])
            glDeleteTextures(1, &textures_[i]);
    }
    delete shader_;
    doneCurrent();

    // 关闭残留的 fd（防止内存泄漏）
    if (pending_frame_.fd >= 0) ::close(pending_frame_.fd);
    if (prev_frame_.fd >= 0) ::close(prev_frame_.fd);
}

// ============================================================================
// OpenGL 初始化
// ============================================================================
// 在 OpenGL 上下文创建后调用，只调用一次
// 初始化内容：
//   1. 初始化 EGL DMA-BUF 导入扩展
//   2. 编译着色器
//   3. 创建两个纹理（双缓冲）
// ============================================================================
void GLVideoWidget::initializeGL()
{
    initializeOpenGLFunctions();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    // 初始化 EGL DMA-BUF 导入扩展
    // 如果 GPU 不支持这个扩展，就无法使用零拷贝渲染
    if (!egl_.init()) {
        qWarning() << "EGL DMA-BUF import not available";
        return;
    }

    createShaderProgram();

    // 创建两个 OpenGL 纹理（双缓冲）
    // 纹理本身不分配内存，只是创建一个"容器"
    // 实际内存是 DMA-BUF，通过 EGLImage 绑定到纹理上
    for (int i = 0; i < 2; i++) {
        glGenTextures(1, &textures_[i]);
        glBindTexture(GL_TEXTURE_2D, textures_[i]);
        // 线性过滤：缩放时使用双线性插值，画面更平滑
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // 边缘处理：重复边缘像素（避免黑边）
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    initialized_ = true;
    fpsTimer_.start();
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

// ============================================================================
// 帧就绪回调
// ============================================================================
// 当解码线程通过信号调用此函数时，更新待渲染的帧
// 
// 参数：
//   - frame: 包含 DMA-BUF fd 的渲染帧
//   - fd 是 dup() 后的，调用者可以安全地 close() 原始 fd
//
// 注意：
//   - 此函数在 UI 线程中执行（Qt 信号/槽机制）
//   - pending_frame_ 的生命周期由 UI 线程管理
//   - prev_frame_ 是上一帧，需要关闭其 fd 防止内存泄漏
// ============================================================================
void GLVideoWidget::onFrameReady(RenderFrame frame)
{
    QMutexLocker lock(&mutex_);

    // 关闭上上帧的 fd（避免内存泄漏）
    if (prev_frame_.fd >= 0) {
        ::close(prev_frame_.fd);
        prev_frame_.fd = -1;
    }

    // 更新帧缓冲
    prev_frame_ = pending_frame_;
    pending_frame_ = frame;
    frame_updated_ = true;

    // FPS 统计
    fpsFrameCount_++;
    qint64 elapsed = fpsTimer_.elapsed();
    if (elapsed >= 1000) {
        currentFps_ = fpsFrameCount_ * 1000.0f / elapsed;
        fpsFrameCount_ = 0;
        fpsTimer_.restart();
    }

    // 请求重绘（Qt 会在下一个事件循环中调用 paintGL）
    update();
}

// ============================================================================
// OpenGL 渲染
// ============================================================================
// 每次窗口需要重绘时调用（如帧更新、窗口大小改变）
//
// 渲染流程：
//   1. 检查是否有新帧
//   2. 将 DMA-BUF 导入为 EGLImage
//   3. 将 EGLImage 绑定为 OpenGL 纹理
//   4. 用着色器绘制矩形（纹理映射）
// ============================================================================
void GLVideoWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);

    QMutexLocker lock(&mutex_);
    if (!initialized_ || pending_frame_.fd < 0) return;

    if (frame_updated_) {
        int back = 1 - current_idx_;  // 双缓冲切换

        // 销毁旧 EGLImage（释放对旧 DMA-BUF 的引用）
        if (egl_images_[back] != EGL_NO_IMAGE_KHR) {
            egl_.destroyImage(egl_images_[back]);
            egl_images_[back] = EGL_NO_IMAGE_KHR;
        }

        // ===== 关键步骤：将 DMA-BUF 导入为 EGLImage =====
        // 这一步将 DMA-BUF fd 包装为 EGL 可操作的对象
        // EGL 会创建一个"引用"，指向 DMA-BUF 的物理内存，不会拷贝数据
        egl_images_[back] = egl_.importRGBA(
            pending_frame_.fd,
            pending_frame_.width,
            pending_frame_.height,
            pending_frame_.stride);

        if (egl_images_[back] != EGL_NO_IMAGE_KHR) {
            // 将 EGLImage 绑定为 OpenGL 纹理
            // 之后 OpenGL 渲染这个纹理时，会直接读取 DMA-BUF 中的数据
            egl_.bindToTexture(egl_images_[back], textures_[back]);
            current_idx_ = back;
        }

        frame_updated_ = false;
    }

    // 绘制当前帧
    drawQuad();
}

// ============================================================================
// 绘制视频帧
// ============================================================================
// 用两个三角形拼成一个矩形，将纹理映射到矩形上
//
// 为什么用三角形？：
//   - GPU 最基本的图元就是三角形
//   - 两个三角形可以拼成任意四边形
//   - 这是 OpenGL 渲染的标准做法
//
// 保持宽高比：
//   - 视频的宽高比（如 16:9）可能与窗口不同
//   - 通过缩放 sx/sy 保持视频不变形
// ============================================================================
void GLVideoWidget::drawQuad()
{
    if (egl_images_[current_idx_] == EGL_NO_IMAGE_KHR) return;

    shader_->bind();

    // 计算保持宽高比的缩放系数
    float video_aspect = (float)pending_frame_.width / pending_frame_.height;
    float view_aspect = (float)width() / height();
    float sx = 1.0f, sy = 1.0f;

    if (video_aspect > view_aspect)
        sy = view_aspect / video_aspect;  // 视频更宽，垂直方向缩放
    else
        sx = video_aspect / view_aspect;  // 视频更高，水平方向缩放

    // 顶点数据：位置(x,y) + 纹理坐标(u,v)
    GLfloat vertices[] = {
        -sx, -sy,  0.0f, 1.0f,  // 左下角
         sx, -sy,  1.0f, 1.0f,  // 右下角
         sx,  sy,  1.0f, 0.0f,  // 右上角
        -sx,  sy,  0.0f, 0.0f,  // 左上角
    };
    GLuint indices[] = {0, 1, 2, 2, 3, 0};  // 两个三角形

    GLuint vbo, ebo;
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // 顶点属性：位置（2 float）+ 纹理坐标（2 float），间隔 16 字节
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
    glEnableVertexAttribArray(1);

    // 绑定纹理并绘制
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

    // 绘制 FPS 文字
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::green);
    QFont font = painter.font();
    font.setPixelSize(24);
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(10, 30, QString("FPS: %1").arg(currentFps_, 0, 'f', 1));
    painter.end();

}

void GLVideoWidget::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
}
