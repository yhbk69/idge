// EglImageRenderer.h —— 基于 EGLImage 单纹理的 DMA-BUF 零拷贝渲染器（NV12 着色器版）
// ⚠ 本文件没有 include guard / #pragma once：只能被单个编译单元包含一次，
//   重复包含会触发类重定义编译错误。属遗留/备用实现，主流渲染路径见
//   gl_video_widget.cpp + eglimage_helper.h。若需多处包含请补充保护宏。
//
// ============================================================================
// 【所属模块】src/ui/widgets（零拷贝渲染三件套之一的存档件）
// 【当前状态】全仓零引用：没有 .cpp include 它（grep 过），也不会被编译检查。
//   留着的原因是它是唯一一份"把 NV12 在片段着色器里转 RGB"的参考实现，
//   主流水线（RGA 已经转成 RGBA、着色器只做一次 texture2D 采样）用不到它。
// 【职责与分工】与 GLVideoWidget 的关系是"同一件事的两种做法"：本类自带
//   着色器、VBO/EBO、EGLImage 与尺寸状态，是一个不依赖 Qt 的裸 GLES2 渲染器
//   ——调用方要自己保证 GL 上下文已 current、自己排布 viewport 与 swap。
// 【所有权】本类持有：一张 EGLImage、一个纹理、两个缓冲、一个 program。
//   它**没有析构函数**：成员资源不会随对象销毁自动回收，必须显式调 destroy()，
//   而且 destroy() 也得在上下文 current 时调（GL 对象删除走的是当前上下文）；
//   漏掉就靠"上下文销毁"兜底，而 EGLImage 不属于上下文资源、兜不住（同 EglFramePool）。
//   fd 一律不接管：updateFrame 只把 fd 交给 eglCreateImageKHR，close 归调用方。
// ============================================================================
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <cstdio>
#include <cstring>

/* 
====================================================
作用：EGL图像渲染器 - 将DMA-BUF数据渲染到OpenGL表面
说明：实现零拷贝视频渲染，直接将硬件解码输出显示到屏幕
硬件概念：DMA-BUF是Linux内核的缓冲区共享机制，允许不同硬件设备直接访问同一内存
====================================================
*/
class EglImageRenderer
{
public:
    /* 
    ====================================================
    作用：初始化渲染器
    说明：获取EGL扩展函数指针，创建纹理和着色器程序
    返回值：成功返回true，失败返回false
    ⚠ 与 eglimage_helper/egl_dma_buf 的 init 不同，这里未查询
      EGL_EXT_image_dma_buf_import 扩展、未检查当前上下文是否 current，
      仅验证函数指针非空；且 compileShaders() 不检查编译/链接结果，
      着色器失败时 program_=0，render() 里 glUseProgram(0) 静默黑屏。
      失败几乎只能在 updateFrame 的 egl 报错里间接看出。
    ====================================================
    */
    bool init()
    {
        /* 
        ====================================================
        获取EGL扩展函数指针
        说明：这些函数不是EGL标准的一部分，需要动态获取
        ====================================================
        */
        // 创建EGL图像的函数
        eglCreateImageKHR_ = (PFNEGLCREATEIMAGEKHRPROC)
            eglGetProcAddress("eglCreateImageKHR");
        // 销毁EGL图像的函数
        eglDestroyImageKHR_ = (PFNEGLDESTROYIMAGEKHRPROC)
            eglGetProcAddress("eglDestroyImageKHR");
        // 将EGL图像绑定到OpenGL纹理的函数
        glEGLImageTargetTexture2DOES_ = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");

        // 检查函数指针是否有效
        if (!eglCreateImageKHR_ || !eglDestroyImageKHR_ ||
            !glEGLImageTargetTexture2DOES_) {
            return false;
        }

        /* 
        ====================================================
        创建OpenGL纹理
        说明：纹理是GPU可访问的图像数据格式
        ====================================================
        */
        glGenTextures(1, &texture_);  // 生成纹理对象
        glBindTexture(GL_TEXTURE_2D, texture_);  // 绑定纹理
        
        // 设置纹理过滤参数
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);  // 缩小过滤：线性插值
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);  // 放大过滤：线性插值
        
        // 设置纹理环绕参数
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);  // S轴：边缘裁剪
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);  // T轴：边缘裁剪
        
        glBindTexture(GL_TEXTURE_2D, 0);  // 解绑纹理

        // 编译着色器程序
        compileShaders();

        return true;
    }

    /* 
    ====================================================
    作用：更新帧数据
    说明：导入新的DMA-BUF并绑定到纹理
    参数：fd - DMA-BUF文件描述符
          width/height - 图像尺寸
          stride - 行跨度（字节数）
          format - 像素格式（DRM四字符码）
          modifier - 内存布局修饰符（用于压缩格式）
    返回值：成功返回true，失败返回false
    硬件概念：DMA-BUF导入允许GPU直接访问视频解码器输出的内存
    【时序：先毁后建】和 EglFramePool::acquire 的"先建新、成功再毁旧"相反，
      这里是先 destroy 旧 image 再 create 新的。副作用是导入失败时既丢了旧帧
      又没拿到新帧：egl_image_ 变成 EGL_NO_IMAGE_KHR，下一次 render() 在开头
      判空直接 return，屏幕上是一整帧黑，而不是"停在上一帧"。
    【fd 不接管】本函数不 dup 也不 close fd，调用方按自己的协议回收；
      create 成功时驱动已另持一份内核引用，所以之后 close 原 fd 不影响这张图。
    【线程】只能在 GL/EGL 上下文 current 的线程调；width_/height_ 会被 render()
      和 aspect 计算读，所以这两个成员跟着 image 一起在同一个线程更新，无需加锁。
    ====================================================
    */
    bool updateFrame(int fd, int width, int height, int stride,
                     uint32_t format, uint64_t modifier = 0)
    {
        // 销毁旧的EGLImage（如果有）
        if (egl_image_ != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR_(eglGetCurrentDisplay(), egl_image_);
            egl_image_ = EGL_NO_IMAGE_KHR;
        }

        // 导入新的DMA-BUF为EGLImage
        egl_image_ = importDmaBuf(fd, width, height, stride, format, modifier);
        if (egl_image_ == EGL_NO_IMAGE_KHR) {
            return false;
        }

        /* 
        ====================================================
        绑定EGLImage到纹理
        说明：使纹理内容指向DMA-BUF数据，实现零拷贝
        ====================================================
        */
        glBindTexture(GL_TEXTURE_2D, texture_);  // 绑定纹理
        glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, (GLeglImageOES)egl_image_);  // 绑定图像到纹理
        glBindTexture(GL_TEXTURE_2D, 0);  // 解绑纹理

        width_ = width;
        height_ = height;

        return true;
    }

    /* 
    ====================================================
    作用：渲染帧
    说明：使用OpenGL着色器将纹理渲染到视口
    参数：viewport_w - 视口宽度，viewport_h - 视口高度
    说明：保持视频原始宽高比，自动添加黑边
    性能提示：每帧都重建顶点数组、glBufferData 重传 VBO/EBO 并两次
      glGetAttrib/UniformLocation 查询—— quad 是静态几何，只有 sx/sy
      随宽高比变化，稳态运行属可避免的上传开销（帧池/常量 VBO 可优化）。
    【两个除零入口】viewport_h 由调用方给：widget 还没布局出来（0 高）时
      view_aspect 除零得 inf，sy 变 0，quad 退化成一条线；height_ 为 0 只在
      "updateFrame 成功但传了 height=0"这种上游出错的场合出现。两条都不会崩，
      只是画不出来——排查时先看 viewport 而不是先看 EGL。
    【viewport 语义】glViewport 用的是**设备像素**，不是逻辑尺寸；
      QOpenGLWidget 下要传 framebufferSize()（HiDPI 时比 size() 大），传错会得到
      画面只占左上角一块。
    【状态泄漏】结尾只关了属性 0/1、纹理与 program，没关 GL_ARRAY_BUFFER/
      ELEMENT_ARRAY_BUFFER 绑定，也没关 glEnableVertexAttribArray 之外的东西
      —— 与别的渲染器混用时记得自己复位（对照 gl_video_widget.cpp 的 drawQuad，
      它把两个 buffer 也解绑了）。
    ====================================================
    */
    void render(int viewport_w, int viewport_h)
    {
        // 设置视口
        glViewport(0, 0, viewport_w, viewport_h);
        // 清除颜色缓冲区（黑色背景）
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // 如果没有图像数据，直接返回
        if (egl_image_ == EGL_NO_IMAGE_KHR) return;

        // 使用着色器程序
        glUseProgram(program_);

        /* 
        ====================================================
        计算保持宽高比的缩放因子
        说明：确保视频在不同尺寸的窗口中不变形
        ====================================================
        */
        float video_aspect = (float)width_ / height_;  // 视频宽高比
        float view_aspect = (float)viewport_w / viewport_h;  // 视口宽高比
        float sx = 1.0f, sy = 1.0f;  // 缩放因子

        if (video_aspect > view_aspect) {
            // 视频更宽，垂直方向缩放
            sy = view_aspect / video_aspect;
        } else {
            // 视频更高，水平方向缩放
            sx = video_aspect / view_aspect;
        }

        /* 
        ====================================================
        顶点数据：位置 + 纹理坐标
        说明：位置坐标范围[-1,1]，纹理坐标范围[0,1]
        ====================================================
        */
        GLfloat vertices[] = {
            // x,     y,     u,    v
            -sx,   -sy,    0.0f, 1.0f,  // 左下角
             sx,   -sy,    1.0f, 1.0f,  // 右下角
             sx,    sy,    1.0f, 0.0f,  // 右上角
            -sx,    sy,    0.0f, 0.0f,  // 左上角
        };

        // 索引数据：两个三角形组成矩形
        GLuint indices[] = {0, 1, 2, 2, 3, 0};

        /* 
        ====================================================
        上传顶点和索引数据到GPU
        说明：VBO（顶点缓冲对象）和EBO（索引缓冲对象）
        ====================================================
        */
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        /* 
        ====================================================
        设置顶点属性
        说明：告诉GPU如何解释顶点数据
        ====================================================
        */
        GLint pos_loc = glGetAttribLocation(program_, "aPos");  // 位置属性
        GLint tex_loc = glGetAttribLocation(program_, "aTexCoord");  // 纹理坐标属性

        // 启用并设置位置属性
        glEnableVertexAttribArray(pos_loc);
        glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(GLfloat), (void*)0);

        // 启用并设置纹理坐标属性
        glEnableVertexAttribArray(tex_loc);
        glVertexAttribPointer(tex_loc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(GLfloat), (void*)(2 * sizeof(GLfloat)));

        /* 
        ====================================================
        绑定纹理并绘制
        说明：将纹理数据传递给着色器进行渲染
        ====================================================
        */
        glActiveTexture(GL_TEXTURE0);  // 激活纹理单元0
        glBindTexture(GL_TEXTURE_2D, texture_);  // 绑定纹理
        glUniform1i(glGetUniformLocation(program_, "tex"), 0);  // 设置纹理采样器

        // 绘制矩形（6个顶点，两个三角形）
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

        /* 
        ====================================================
        清理状态
        ====================================================
        */
        glDisableVertexAttribArray(pos_loc);
        glDisableVertexAttribArray(tex_loc);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
    }

    /* 
    ====================================================
    作用：销毁渲染器
    说明：释放所有OpenGL和EGL资源
    【调用时机】必须在 GL 上下文仍然 current 时调（GLVideoWidget 的做法是析构里
      先 makeCurrent()、清完再 doneCurrent()）；上下文没了之后再调，GL 对象是
      空删、EGLImage 才是真漏（它不随上下文回收）。
    【可重复调用】每个分支都先判非 0/非 EGL_NO_IMAGE_KHR 再置回初值，
      二次调用无害；但 init() 之前调也安全——所有句柄都是 0 起步。
    【不是析构函数】本类没有 ~EglImageRenderer，忘调 destroy() 编译器不会提醒。
    ====================================================
    */
    void destroy()
    {
        // 销毁EGL图像
        if (egl_image_ != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR_(eglGetCurrentDisplay(), egl_image_);
            egl_image_ = EGL_NO_IMAGE_KHR;
        }
        // 删除纹理
        if (texture_) {
            glDeleteTextures(1, &texture_);
            texture_ = 0;
        }
        // 删除缓冲区
        if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
        if (ebo_) { glDeleteBuffers(1, &ebo_); ebo_ = 0; }
        // 删除着色器程序
        if (program_) { glDeleteProgram(program_); program_ = 0; }
    }

private:
    /* 
    ====================================================
    作用：导入DMA-BUF为EGLImage
    说明：将DMA-BUF文件描述符转换为EGL可访问的图像对象
    参数：fd - 文件描述符，width/height - 尺寸
          stride - 行跨度，format - 像素格式
          modifier - 内存布局修饰符
    返回值：EGLImage句柄，失败返回EGL_NO_IMAGE_KHR
    硬件概念：DMA-BUF导入是零拷贝的关键，避免CPU内存拷贝
    【attrs[48] 够用】最长路径 = 6(宽高+fourcc) + 6(plane0) + 6(plane1)
      + 4(plane0 modifier) + 4(plane1 modifier) + 1(EGL_NONE) = 27 个槽，
      48 是拍脑袋的保守值（egl_dma_buf.h 那边写的是 64，两处各自独立，
      加分支时得分别重算，不存在"改一处两边同步"）。
    【与 egl_dma_buf.h 的两处漂移】① 这里 NV12 时给 plane1 也写了 modifier，
      那边只写 plane0；② 这里每次现场 eglGetCurrentDisplay()，那边用 init()
      缓存的 display_。两份实现都无人引用，别把其中一份的修复当另一份也修好了。
    【不校验指针】直接解引用 eglCreateImageKHR_：init() 没调过就是空指针调用，
      本函数返回 bool 与否都救不了，调用方（updateFrame）也没判。
    ====================================================
    */
    EGLImageKHR importDmaBuf(int fd, int width, int height,
                              int stride, uint32_t format,
                              uint64_t modifier)
    {
        // EGL属性列表
        EGLint attrs[48];
        int i = 0;

        // 基本图像属性
        attrs[i++] = EGL_WIDTH;
        attrs[i++] = width;
        attrs[i++] = EGL_HEIGHT;
        attrs[i++] = height;
        attrs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
        attrs[i++] = (EGLint)format;

        /* 
        ====================================================
        Plane 0 属性（Y平面或RGB平面）
        说明：DMA-BUF的第一个平面
        ====================================================
        */
        attrs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        attrs[i++] = fd;
        attrs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        attrs[i++] = 0;  // 平面偏移
        attrs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        attrs[i++] = stride;  // 行跨度

        /* 
        ====================================================
        NV12/NV21格式的Plane 1（UV平面）
        说明：YUV420半平面格式需要两个平面
        ====================================================
        */
        if (format == DRM_FORMAT_NV12 || format == DRM_FORMAT_NV21) {
            attrs[i++] = EGL_DMA_BUF_PLANE1_FD_EXT;
            attrs[i++] = fd;  // 同一个文件描述符
            attrs[i++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
            attrs[i++] = stride * height;  // UV平面偏移
            attrs[i++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
            attrs[i++] = stride;  // UV平面行跨度
        }

        /* 
        ====================================================
        内存布局修饰符
        说明：用于压缩格式（如AFBC、UBWC等）
        ====================================================
        */
        if (modifier != 0 && modifier != DRM_FORMAT_MOD_LINEAR) {
            // 修饰符低32位
            attrs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            attrs[i++] = (EGLint)(modifier & 0xFFFFFFFF);
            // 修饰符高32位
            attrs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            attrs[i++] = (EGLint)(modifier >> 32);
            
            // NV12格式需要为UV平面也设置修饰符
            if (format == DRM_FORMAT_NV12 || format == DRM_FORMAT_NV21) {
                attrs[i++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT;
                attrs[i++] = (EGLint)(modifier & 0xFFFFFFFF);
                attrs[i++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT;
                attrs[i++] = (EGLint)(modifier >> 32);
            }
        }

        attrs[i++] = EGL_NONE;  // 属性列表结束标记

        // 创建EGL图像
        return eglCreateImageKHR_(
            eglGetCurrentDisplay(), EGL_NO_CONTEXT,
            EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    }

    /* 
    ====================================================
    作用：编译着色器程序
    说明：创建顶点着色器和片段着色器，用于NV12到RGB转换
    【线程】glCreateShader/glLinkProgram 都作用于当前上下文的"program 对象"，
      所以只能在 init() 的那次 current 期间调用（init 里就是这个位置）。
    【没有错误检查】既不查 GL_COMPILE_STATUS 也不查 GL_LINK_STATUS：编译失败时
      program_ 仍是个有效的 0 之外句柄但不可用，render() 里 glUseProgram 静默失败。
      与 init() 的注释是同一条已知缺陷，两处一起看。
    【顺带建了 VBO/EBO】函数名只说"编译着色器"，却还 glGenBuffers 两个缓冲对象
      —— 这是本文件里唯一一处"名字与职责不符"，接手时别只盯着着色器部分改。
    ====================================================
    */
    void compileShaders()
    {
        /* 
        ====================================================
        顶点着色器
        说明：处理顶点位置和纹理坐标
        ====================================================
        */
        const char* vs_src = R"(
            attribute vec2 aPos;      // 顶点位置
            attribute vec2 aTexCoord; // 纹理坐标
            varying vec2 vTexCoord;   // 传递给片段着色器的纹理坐标
            void main() {
                gl_Position = vec4(aPos, 0.0, 1.0);  // 设置顶点位置
                vTexCoord = aTexCoord;  // 传递纹理坐标
            }
        )";

        /* 
        ====================================================
        片段着色器：NV12 → RGB 转换
        说明：将YUV颜色空间转换为RGB颜色空间
        硬件概念：NV12是YUV420半平面格式，Y和UV分开存储
        ⚠ 与 updateFrame 的用法耦合风险：
          1) uniform tex_height/img_height 从未 glUniform 赋值（默认0），
             y_ratio = img_height/tex_height 是 0/0 → NaN，采样坐标未定义；
             只有按 NV12"Y 占 2/3 高度、UV 占 1/3"打包成单纹理
             （软件重排布局）时公式才成立；
          2) 若按 DMA-BUF 双平面直接导入 NV12 或导入 RGBA（本项目
             主流水线是 RGA 转好的 RGBA），此着色器的 .r/.ra 通道解释
             与 BT.601 矩阵全部错误，画面呈绿色噪声；
          3) 转换系数用的是 BT.601 而非 RGA 路径常用的 BT.709，
             两路渲染并存时色彩会有可感知偏差。
        ====================================================
        */
        const char* fs_src = R"(
            precision mediump float;  // 精度设置
            varying vec2 vTexCoord;   // 从顶点着色器接收的纹理坐标
            uniform sampler2D tex;    // 纹理采样器
            uniform float tex_height; // 纹理总高度
            uniform float img_height; // 图像实际高度

            void main() {
                // NV12布局：Y占2/3，UV占1/3
                float y_ratio = img_height / tex_height;  // 通常 = 2/3

                // 采样Y分量
                vec2 y_coord = vec2(vTexCoord.x, vTexCoord.y * y_ratio);
                float y = texture2D(tex, y_coord).r;  // Y在红色通道

                // 采样UV分量
                vec2 uv_coord = vec2(vTexCoord.x,
                                     y_ratio + vTexCoord.y * (1.0 - y_ratio));
                vec2 uv = texture2D(tex, uv_coord).ra - vec2(0.5);  // U在红色，V在透明通道

                // BT.601颜色空间转换公式
                float r = y + 1.402 * uv.y;
                float g = y - 0.344136 * uv.x - 0.714136 * uv.y;
                float b = y + 1.772 * uv.x;

                // 输出RGB颜色，钳制到[0,1]范围
                gl_FragColor = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
            }
        )";

        // 编译顶点着色器
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &vs_src, nullptr);
        glCompileShader(vs);

        // 编译片段着色器
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &fs_src, nullptr);
        glCompileShader(fs);

        // 创建着色器程序并链接
        program_ = glCreateProgram();
        glAttachShader(program_, vs);
        glAttachShader(program_, fs);
        glLinkProgram(program_);

        // 删除着色器对象（已链接到程序）
        glDeleteShader(vs);
        glDeleteShader(fs);

        // 创建顶点缓冲对象（VBO）和索引缓冲对象（EBO）
        glGenBuffers(1, &vbo_);
        glGenBuffers(1, &ebo_);
    }

    // 成员变量
    EGLImageKHR egl_image_ = EGL_NO_IMAGE_KHR;  // EGL图像对象
    GLuint texture_ = 0;  // OpenGL纹理ID
    GLuint program_ = 0;  // 着色器程序ID
    GLuint vbo_ = 0;      // 顶点缓冲对象
    GLuint ebo_ = 0;      // 索引缓冲对象
    int width_ = 0;       // 图像宽度
    int height_ = 0;      // 图像高度

    // EGL扩展函数指针
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;
};