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
// 【帧池机制】
//   - EglFramePool 预创建 4 个纹理，槽位复用时仅重建 EGLImage
//   - 渲染时只持有 1 个活动槽位，拿到新帧后才 release 上一槽
//   - 纹理 ID 稳定，避免逐帧 eglCreateImageKHR/Destroy 的驱动 ioctl
//
// 【fd 两级流水线与 close 时机】解码线程 dup 出 fd 随信号投递；
//   onFrameReady 把旧 pending 挪进 prev_frame_ 并在下下帧 close，
//   保证 paintGL 长时间不执行（窗口隐藏）时也不漏 fd；paintGL 消费
//   pending_frame_ 后立即 close 原 fd，帧池另持自己的 dup 副本。
//   EGLImage 在内核层另持引用，所以即便上游 dmaBuffer 已 release 回池、
//   fd 已 close，正在显示的旧帧仍完整可读——这是零拷贝链路里
//   防止画面撕裂的关键引用重叠。
//
// ⚠【析构与在途信号】若解码线程仍持有 frameReady 连接而 widget 先析构，
//   队列中已投递的 RenderFrame::fd 副本无人签收 → fd 泄漏；
//   PlayerWidget 析构约定"先 stopDecoder 再销毁窗口"正是为此。
// ============================================================================

#include "gl_video_widget.h"
#include "perf_probe.h"
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
    // 帧池的 EGLImage 必须显式销毁（不随 GL 上下文回收），纹理一并删除
    makeCurrent();
    framePool_.shutdown();
    current_texture_ = 0;
    // 清理 VBO/EBO
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (ebo_) { glDeleteBuffers(1, &ebo_); ebo_ = 0; }
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
//   3. 初始化帧池（预创建纹理，复用避免逐帧 ioctl）
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

    // 创建 VBO 和 EBO（只创建一次，每帧复用）
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);

    // EBO 索引数据固定（两个三角形组成四边形），只需上传一次
    GLuint indices[] = {0, 1, 2, 2, 3, 0};
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    // 初始化帧池：预创建纹理，后续仅重建 EGLImage，纹理对象复用
    framePool_.init(egl_.getCreateImageProc(), egl_.getDestroyImageProc(), egl_.getBindTextureProc());

    initialized_ = true;
    fpsTimer_.start();
    qDebug() << "GLVideoWidget initialized with EglFramePool";
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
//   - pending_frame_ 的 fd 最终由 paintGL 关闭（帧池另持 dup 副本）
//   - 若 paintGL 未及时执行而新帧又来，旧 pending 的 fd 挪进 prev_frame_，
//     在下一次 onFrameReady 时关闭，避免覆盖泄漏
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
    // 卡顿探针：frame_updated_ 仍为 true 说明上一帧还没被 paintGL 消费就有新帧
    // 到了 —— 解码侧产出正常、显示侧跟不上，丢帧就发生在这里
    if (frame_updated_) droppedFrames_++;
    frame_updated_ = true;

    // FPS 统计
    // 注意：这里统计的是"收到的帧率"（onFrameReady 触发频率），
    // 而非实际渲染帧率——Qt 合并 update() 时 paintGL 可能少于一帧一次，
    // 网络丢帧/背压时两者会明显背离，排查性能问题时勿混用该数字。
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
//   2. 从帧池获取槽位，将 DMA-BUF 导入为 EGLImage 并绑定到纹理
//   3. 释放上一帧的槽位
//   4. 用着色器绘制矩形（纹理映射）
// ============================================================================
void GLVideoWidget::paintGL()
{
    // 卡顿探针（IDGE_PERF=1 才输出）：paintGL 间隔远大于 1/目标帧率，说明 GUI
    // 事件循环被别的东西占住了。它是"画面卡但渲染本身不慢"这类问题的关键指标——
    // 实测报警面板一次重建能把 4 个 widget 的间隔一起顶到 900ms 以上。
    // 阈值取 200ms 而不是 60ms：4 路 17~23fps 出帧、推理串行轮询，60~110ms 的间隔
    // 是产帧侧的正常抖动（实测这类间隔里 framesArrivedWhileStalled 几乎全为 0，
    // 即"没有帧可画"而不是"有帧没轮到画"），只有几百毫秒级才是 GUI 线程被独占。
    if (!paintProbe_.isValid()) paintProbe_.start();
    const qint64 paintStartNs = paintProbe_.nsecsElapsed();
    const qint64 gapMs = lastPaintNs_ ? (paintStartNs - lastPaintNs_) / 1000000 : 0;
    lastPaintNs_ = paintStartNs;
    const int arrivedWhileStalled = droppedFrames_;
    droppedFrames_ = 0;
    if (perfProbe() && gapMs > 200) {
        qWarning() << "[PROBE] paintGL gap" << gapMs << "ms widget" << this
                   << "framesArrivedWhileStalled" << arrivedWhileStalled;
    }
    // 速率对照：paints/s 明显低于 arrivals/s 说明帧到了但没上屏（GUI 合成受限）；
    // 两者一致且都只有十几，则瓶颈在生产侧。
    if (perfProbe()) {
        ++paintCount_;
        if (paintCount_ % 100 == 0) {
            if (!lastRateNs_) lastRateNs_ = paintStartNs;
            const double winS = (paintStartNs - lastRateNs_) / 1e9;
            lastRateNs_ = paintStartNs;
            qWarning() << "[PROBE] paint" << this << "paints/s"
                       << (winS > 0 ? 100.0 / winS : 0.0)
                       << "arrivals/s" << currentFps_;
        }
    }

    glClear(GL_COLOR_BUFFER_BIT);

    QMutexLocker lock(&mutex_);
    if (!initialized_ || pending_frame_.fd < 0) return;

    if (frame_updated_) {
        // 保存视频尺寸（用于 drawQuad 的宽高比计算）
        video_width_ = pending_frame_.width;
        video_height_ = pending_frame_.height;

        // 从帧池获取新槽位（内部 dup fd、创建 EGLImage、绑定到预创建纹理）
        GLuint newTex = framePool_.acquire(
            pending_frame_.fd,
            pending_frame_.width,
            pending_frame_.height,
            pending_frame_.stride,
            DRM_FORMAT_ABGR8888);

        if (newTex != 0) {
            // 释放上一帧的槽位（close dup 的 fd，标记空闲）
            if (current_texture_ != 0) {
                framePool_.release(current_texture_);
            }
            current_texture_ = newTex;
        }

        // 关闭 pending_frame_ 的 fd（帧池已 dup 副本）
        if (pending_frame_.fd >= 0) {
            ::close(pending_frame_.fd);
            pending_frame_.fd = -1;
        }

        frame_updated_ = false;
    }

    // 绘制当前帧
    drawQuad();

    // 卡顿探针：paintGL 自身耗时（GL 提交 + QPainter 画 FPS 文字）。
    // 与上面的 gap 一起看才能区分"画得慢"和"没轮到画"。
    const qint64 paintCostMs = (paintProbe_.nsecsElapsed() - paintStartNs) / 1000000;
    if (perfProbe() && paintCostMs > 20) {
        qWarning() << "[PROBE] paintGL cost" << paintCostMs << "ms widget" << this;
    }
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
    if (current_texture_ == 0) return;

    shader_->bind();

    // 计算保持宽高比的缩放系数
    float video_aspect = (video_width_ > 0 && video_height_ > 0)
                             ? (float)video_width_ / video_height_
                             : (float)width() / height();
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

    // 复用已创建的 VBO/EBO，只更新顶点数据（每帧顶点位置会变）
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    // EBO 数据固定（索引不变），只需在首次绑定，这里简化处理也一并更新

    // 顶点属性：位置（2 float）+ 纹理坐标（2 float），间隔 16 字节
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
    glEnableVertexAttribArray(1);

    // 绑定纹理并绘制
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, current_texture_);
    shader_->setUniformValue("tex", 0);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    shader_->release();

    // 绘制 FPS 文字
    // ⚠ 原生 GL 与 QPainter 混绘：paintGL 里先用裸 GL 调用画视频，再直接
    //   构造 QPainter(this) 画文字。Qt 约定同一 paint 周期内混用需
    //   beginNativePainting()/endNativePainting() 配对交接状态；此处
    //   依赖 QPainter 在 GL 之后创建、且 end() 前不再发 GL 调用的顺序
    //   侥幸正确——改动本函数时勿在 painter.end() 之后再插入 GL 调用，
    //   且不同 Qt 版本下可能出现 FPS 文字丢失（升级需回归验证）。
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
