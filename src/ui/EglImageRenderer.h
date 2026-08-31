#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <cstdio>
#include <cstring>

class EglImageRenderer
{
public:
    bool init()
    {
        // 获取函数指针
        eglCreateImageKHR_ = (PFNEGLCREATEIMAGEKHRPROC)
            eglGetProcAddress("eglCreateImageKHR");
        eglDestroyImageKHR_ = (PFNEGLDESTROYIMAGEKHRPROC)
            eglGetProcAddress("eglDestroyImageKHR");
        glEGLImageTargetTexture2DOES_ = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");

        if (!eglCreateImageKHR_ || !eglDestroyImageKHR_ ||
            !glEGLImageTargetTexture2DOES_) {
            return false;
        }

        // 创建纹理
        glGenTextures(1, &texture_);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        // 编译着色器
        compileShaders();

        return true;
    }

    /**
     * 更新帧（导入新的 DMA-BUF）
     */
    bool updateFrame(int fd, int width, int height, int stride,
                     uint32_t format, uint64_t modifier = 0)
    {
        // 销毁旧的 EGLImage
        if (egl_image_ != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR_(eglGetCurrentDisplay(), egl_image_);
            egl_image_ = EGL_NO_IMAGE_KHR;
        }

        // 导入新的 DMA-BUF
        egl_image_ = importDmaBuf(fd, width, height, stride, format, modifier);
        if (egl_image_ == EGL_NO_IMAGE_KHR) {
            return false;
        }

        // 绑定为纹理
        glBindTexture(GL_TEXTURE_2D, texture_);
        glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, (GLeglImageOES)egl_image_);
        glBindTexture(GL_TEXTURE_2D, 0);

        width_ = width;
        height_ = height;

        return true;
    }

    /**
     * 渲染
     */
    void render(int viewport_w, int viewport_h)
    {
        glViewport(0, 0, viewport_w, viewport_h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (egl_image_ == EGL_NO_IMAGE_KHR) return;

        glUseProgram(program_);

        // 计算保持宽高比
        float video_aspect = (float)width_ / height_;
        float view_aspect = (float)viewport_w / viewport_h;
        float sx = 1.0f, sy = 1.0f;

        if (video_aspect > view_aspect) {
            sy = view_aspect / video_aspect;
        } else {
            sx = video_aspect / view_aspect;
        }

        // 顶点数据
        GLfloat vertices[] = {
            // x,     y,     u,    v
            -sx,   -sy,    0.0f, 1.0f,
             sx,   -sy,    1.0f, 1.0f,
             sx,    sy,    1.0f, 0.0f,
            -sx,    sy,    0.0f, 0.0f,
        };

        GLuint indices[] = {0, 1, 2, 2, 3, 0};

        // 上传
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        // 属性
        GLint pos_loc = glGetAttribLocation(program_, "aPos");
        GLint tex_loc = glGetAttribLocation(program_, "aTexCoord");

        glEnableVertexAttribArray(pos_loc);
        glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(GLfloat), (void*)0);

        glEnableVertexAttribArray(tex_loc);
        glVertexAttribPointer(tex_loc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(GLfloat), (void*)(2 * sizeof(GLfloat)));

        // 纹理
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glUniform1i(glGetUniformLocation(program_, "tex"), 0);

        // 绘制
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

        // 清理
        glDisableVertexAttribArray(pos_loc);
        glDisableVertexAttribArray(tex_loc);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
    }

    void destroy()
    {
        if (egl_image_ != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR_(eglGetCurrentDisplay(), egl_image_);
            egl_image_ = EGL_NO_IMAGE_KHR;
        }
        if (texture_) {
            glDeleteTextures(1, &texture_);
            texture_ = 0;
        }
        if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
        if (ebo_) { glDeleteBuffers(1, &ebo_); ebo_ = 0; }
        if (program_) { glDeleteProgram(program_); program_ = 0; }
    }

private:
    EGLImageKHR importDmaBuf(int fd, int width, int height,
                              int stride, uint32_t format,
                              uint64_t modifier)
    {
        EGLint attrs[48];
        int i = 0;

        attrs[i++] = EGL_WIDTH;
        attrs[i++] = width;
        attrs[i++] = EGL_HEIGHT;
        attrs[i++] = height;
        attrs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
        attrs[i++] = (EGLint)format;

        // Plane 0
        attrs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        attrs[i++] = fd;
        attrs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        attrs[i++] = 0;
        attrs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        attrs[i++] = stride;

        // NV12: Plane 1 (UV)
        if (format == DRM_FORMAT_NV12 || format == DRM_FORMAT_NV21) {
            attrs[i++] = EGL_DMA_BUF_PLANE1_FD_EXT;
            attrs[i++] = fd;
            attrs[i++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
            attrs[i++] = stride * height;
            attrs[i++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
            attrs[i++] = stride;
        }

        // Modifier
        if (modifier != 0 && modifier != DRM_FORMAT_MOD_LINEAR) {
            attrs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            attrs[i++] = (EGLint)(modifier & 0xFFFFFFFF);
            attrs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            attrs[i++] = (EGLint)(modifier >> 32);
            if (format == DRM_FORMAT_NV12 || format == DRM_FORMAT_NV21) {
                attrs[i++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT;
                attrs[i++] = (EGLint)(modifier & 0xFFFFFFFF);
                attrs[i++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT;
                attrs[i++] = (EGLint)(modifier >> 32);
            }
        }

        attrs[i++] = EGL_NONE;

        return eglCreateImageKHR_(
            eglGetCurrentDisplay(), EGL_NO_CONTEXT,
            EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    }

    void compileShaders()
    {
        const char* vs_src = R"(
            attribute vec2 aPos;
            attribute vec2 aTexCoord;
            varying vec2 vTexCoord;
            void main() {
                gl_Position = vec4(aPos, 0.0, 1.0);
                vTexCoord = aTexCoord;
            }
        )";

        // NV12 → RGB 着色器
        const char* fs_src = R"(
            precision mediump float;
            varying vec2 vTexCoord;
            uniform sampler2D tex;
            uniform float tex_height;   // 纹理总高度
            uniform float img_height;   // 图像实际高度

            void main() {
                // NV12 布局：Y 占 2/3，UV 占 1/3
                float y_ratio = img_height / tex_height;  // 通常 = 2/3

                // 采样 Y
                vec2 y_coord = vec2(vTexCoord.x, vTexCoord.y * y_ratio);
                float y = texture2D(tex, y_coord).r;

                // 采样 UV
                vec2 uv_coord = vec2(vTexCoord.x,
                                     y_ratio + vTexCoord.y * (1.0 - y_ratio));
                vec2 uv = texture2D(tex, uv_coord).ra - vec2(0.5);

                // BT.601
                float r = y + 1.402 * uv.y;
                float g = y - 0.344136 * uv.x - 0.714136 * uv.y;
                float b = y + 1.772 * uv.x;

                gl_FragColor = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
            }
        )";

        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &vs_src, nullptr);
        glCompileShader(vs);

        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &fs_src, nullptr);
        glCompileShader(fs);

        program_ = glCreateProgram();
        glAttachShader(program_, vs);
        glAttachShader(program_, fs);
        glLinkProgram(program_);

        glDeleteShader(vs);
        glDeleteShader(fs);

        // 创建 VBO/EBO
        glGenBuffers(1, &vbo_);
        glGenBuffers(1, &ebo_);
    }

    EGLImageKHR egl_image_ = EGL_NO_IMAGE_KHR;
    GLuint texture_ = 0;
    GLuint program_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
    int width_ = 0;
    int height_ = 0;

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;
};
