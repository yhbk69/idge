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