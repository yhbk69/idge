#ifndef EGLIMAGE_HELPER_H
#define EGLIMAGE_HELPER_H

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <cstdio>
#include <cstring>

class EglImageHelper
{
public:
    bool init()
    {
        display_ = eglGetCurrentDisplay();
        if (display_ == EGL_NO_DISPLAY) return false;

        // 检查扩展
        const char* exts = eglQueryString(display_, EGL_EXTENSIONS);
        if (!exts || !strstr(exts, "EGL_EXT_image_dma_buf_import")) {
            printf("[EGL] dma_buf_import NOT supported\n");
            return false;
        }

        eglCreateImageKHR_ = (PFNEGLCREATEIMAGEKHRPROC)
            eglGetProcAddress("eglCreateImageKHR");
        eglDestroyImageKHR_ = (PFNEGLDESTROYIMAGEKHRPROC)
            eglGetProcAddress("eglDestroyImageKHR");
        glEGLImageTargetTexture2DOES_ = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");

        return eglCreateImageKHR_ && eglDestroyImageKHR_ &&
               glEGLImageTargetTexture2DOES_;
    }

    /**
     * 导入 RGBA DMA-BUF 为 EGLImage
     */
    EGLImageKHR importRGBA(int fd, int width, int height, int stride)
    {
        EGLint attrs[] = {
            EGL_WIDTH,                     width,
            EGL_HEIGHT,                    height,
            EGL_LINUX_DRM_FOURCC_EXT,     DRM_FORMAT_ABGR8888,  // RGBA 内存序
            EGL_DMA_BUF_PLANE0_FD_EXT,    fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,  stride,
            EGL_NONE
        };

        EGLImageKHR img = eglCreateImageKHR_(
            display_, EGL_NO_CONTEXT,
            EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);

        if (img == EGL_NO_IMAGE_KHR) {
            printf("[EGL] import RGBA failed: 0x%x\n", eglGetError());
        }
        return img;
    }


    /**
     * 导入 RGBA DMA-BUF 为 EGLImage
     */
    EGLImageKHR importRGB(int fd, int width, int height, int stride)
    {
        EGLint attrs[] = {
            EGL_WIDTH,                     width,
            EGL_HEIGHT,                    height,
            EGL_LINUX_DRM_FOURCC_EXT,     DRM_FORMAT_BGR888,  // RGB 内存序
            EGL_DMA_BUF_PLANE0_FD_EXT,    fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,  stride,
            EGL_NONE
        };

        EGLImageKHR img = eglCreateImageKHR_(
            display_, EGL_NO_CONTEXT,
            EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);

        if (img == EGL_NO_IMAGE_KHR) {
            printf("[EGL] import RGB failed: 0x%x\n", eglGetError());
        }
        return img;
    }

    /**
     * 导入 NV12 DMA-BUF 为 EGLImage
     */
    EGLImageKHR importNV12(int fd, int width, int height, int stride)
    {
        EGLint attrs[] = {
            EGL_WIDTH,                     width,
            EGL_HEIGHT,                    height,
            EGL_LINUX_DRM_FOURCC_EXT,     DRM_FORMAT_NV12,
            // Plane 0: Y
            EGL_DMA_BUF_PLANE0_FD_EXT,    fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,  stride,
            // Plane 1: UV
            EGL_DMA_BUF_PLANE1_FD_EXT,    fd,
            EGL_DMA_BUF_PLANE1_OFFSET_EXT, stride * height,
            EGL_DMA_BUF_PLANE1_PITCH_EXT,  stride,
            EGL_NONE
        };

        EGLImageKHR img = eglCreateImageKHR_(
            display_, EGL_NO_CONTEXT,
            EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);

        if (img == EGL_NO_IMAGE_KHR) {
            printf("[EGL] import NV12 failed: 0x%x\n", eglGetError());
        }
        return img;
    }

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

    typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(
        EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*);
    typedef EGLBoolean (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay, EGLImageKHR);
    typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum, GLeglImageOES);

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;
};

#endif // EGLIMAGE_HELPER_H
