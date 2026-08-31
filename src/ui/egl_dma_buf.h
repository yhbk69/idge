#ifndef EGL_DMA_BUF_H
#define EGL_DMA_BUF_H

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <cstdio>

// EGL 扩展函数指针
typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay, EGLContext,
                                                  EGLenum, EGLClientBuffer,
                                                  const EGLint*);
typedef EGLBoolean (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay, EGLImageKHR);
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum, GLeglImageOES);

class EglDmaBufImporter
{
public:
    bool init()
    {
        display_ = eglGetCurrentDisplay();
        if (display_ == EGL_NO_DISPLAY) {
            printf("[EGL] No current display\n");
            return false;
        }

        // 检查扩展
        const char* exts = eglQueryString(display_, EGL_EXTENSIONS);
        if (!exts) {
            printf("[EGL] Cannot get extensions\n");
            return false;
        }

        if (!strstr(exts, "EGL_EXT_image_dma_buf_import")) {
            printf("[EGL] EGL_EXT_image_dma_buf_import NOT supported\n");
            return false;
        }

        printf("[EGL] dma_buf_import supported\n");

        // 获取函数指针
        eglCreateImageKHR_ = (PFNEGLCREATEIMAGEKHRPROC)
            eglGetProcAddress("eglCreateImageKHR");
        eglDestroyImageKHR_ = (PFNEGLDESTROYIMAGEKHRPROC)
            eglGetProcAddress("eglDestroyImageKHR");
        glEGLImageTargetTexture2DOES_ = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");

        if (!eglCreateImageKHR_ || !eglDestroyImageKHR_ ||
            !glEGLImageTargetTexture2DOES_) {
            printf("[EGL] Failed to get function pointers\n");
            return false;
        }

        return true;
    }

    /**
     * 将 DMA-BUF fd 导入为 EGLImage
     * @param fd       DMA-BUF fd
     * @param width    宽度
     * @param height   高度
     * @param stride   Y plane stride
     * @param format   DRM 格式（如 DRM_FORMAT_NV12）
     * @param modifier format modifier（通常为 0 = linear）
     * @return EGLImageKHR，失败返回 EGL_NO_IMAGE_KHR
     */
    EGLImageKHR importDmaBuf(int fd, int width, int height,
                              int stride, uint32_t format,
                              uint64_t modifier = 0)
    {
        EGLint attrs[64];
        int i = 0;

        attrs[i++] = EGL_WIDTH;
        attrs[i++] = width;
        attrs[i++] = EGL_HEIGHT;
        attrs[i++] = height;
        attrs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
        attrs[i++] = (EGLint)format;

        // Plane 0 (Y)
        attrs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        attrs[i++] = fd;
        attrs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        attrs[i++] = 0;
        attrs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        attrs[i++] = stride;

        // Plane 1 (UV) for NV12
        if (format == DRM_FORMAT_NV12 || format == DRM_FORMAT_NV21) {
            attrs[i++] = EGL_DMA_BUF_PLANE1_FD_EXT;
            attrs[i++] = fd;  // 同一个 fd
            attrs[i++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
            attrs[i++] = stride * height;  // UV offset
            attrs[i++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
            attrs[i++] = stride;
        }

        // Modifier（如果是 linear 则不设置）
        if (modifier != 0 && modifier != DRM_FORMAT_MOD_LINEAR) {
            attrs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            attrs[i++] = (EGLint)(modifier & 0xFFFFFFFF);
            attrs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            attrs[i++] = (EGLint)(modifier >> 32);
        }

        attrs[i++] = EGL_NONE;

        EGLImageKHR image = eglCreateImageKHR_(
            display_, EGL_NO_CONTEXT,
            EGL_LINUX_DMA_BUF_EXT,
            nullptr, attrs);

        if (image == EGL_NO_IMAGE_KHR) {
            printf("[EGL] importDmaBuf failed: 0x%x\n", eglGetError());
        }

        return image;
    }

    EGLImageKHR importDmaBufRgb(int fd, int width, int height, int stride, uint32_t format)
    {
        EGLint attrs[] = {
            EGL_WIDTH, width,
            EGL_HEIGHT, height,
            EGL_LINUX_DRM_FOURCC_EXT, (EGLint)format,
            EGL_DMA_BUF_PLANE0_FD_EXT, fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
            EGL_NONE
        };

        EGLImageKHR image = eglCreateImageKHR_(
            eglGetCurrentDisplay(),
            EGL_NO_CONTEXT,
            EGL_LINUX_DMA_BUF_EXT,
            nullptr,
            attrs
        );
        if (image == EGL_NO_IMAGE_KHR)
        {
            printf("import_dmabuf_rgb failed 0x%x\n", eglGetError());
        }
        return image;
        
    }
    /**
     * 将 EGLImage 绑定为 GL 纹理
     */
    void bindToTexture(EGLImageKHR image, GLuint texture)
    {
        glBindTexture(GL_TEXTURE_2D, texture);
        glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, (GLeglImageOES)image);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void destroyImage(EGLImageKHR image)
    {
        if (image != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR_(display_, image);
        }
    }

private:
    EGLDisplay display_ = EGL_NO_DISPLAY;
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;
};

#endif // EGL_DMA_BUF_H
