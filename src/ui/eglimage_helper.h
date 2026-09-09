#ifndef EGLIMAGE_HELPER_H
#define EGLIMAGE_HELPER_H

// ============================================================================
// EglImageHelper - EGL DMA-BUF 导入辅助类
// ============================================================================
//
// 【作用】
//   将 DMA-BUF 文件描述符(fd)导入为 EGLImage，然后绑定为 OpenGL 纹理。
//   这是实现"零拷贝渲染"的关键：GPU 显存直接作为 OpenGL 纹理使用。
//
// 【工作原理】
//   1. DMA-BUF fd → EGLImageKHR（通过 EGL 扩展）
//   2. EGLImageKHR → OpenGL 纹理（通过 glEGLImageTargetTexture2DOES）
//   3. OpenGL 渲染纹理到屏幕
//
//   整个过程 GPU 显存没有被 CPU 拷贝过，数据一直在 GPU 显存中。
//
// 【为什么需要 EGL 扩展？】
//   - 标准 OpenGL ES 不支持直接使用 DMA-BUF
//   - EGL_EXT_image_dma_buf_import 扩展提供了这个能力
//   - Rockchip GPU (Mali) 支持这个扩展
//
// 【数据流】
//   解码器(NV12 fd) → RGA(RGBA fd) → EGLImage → GL纹理 → 屏幕渲染
//                     ↑               ↑          ↑
//                     GPU显存         GPU显存    GPU显存
//                     （全程零拷贝）
//
// ============================================================================

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
    // 初始化 EGL 扩展函数
    // 必须在 OpenGL 上下文创建后调用（因为需要 eglGetCurrentDisplay）
    bool init()
    {
        // 获取当前 EGL 显示设备
        display_ = eglGetCurrentDisplay();
        if (display_ == EGL_NO_DISPLAY) return false;

        // 检查是否支持 DMA-BUF 导入扩展
        // 如果不支持，这个类就无法使用
        const char* exts = eglQueryString(display_, EGL_EXTENSIONS);
        if (!exts || !strstr(exts, "EGL_EXT_image_dma_buf_import")) {
            printf("[EGL] dma_buf_import NOT supported\n");
            return false;
        }

        // 获取 EGL 扩展函数指针
        // 这些函数在标准 EGL 中不存在，需要动态获取
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
     * 
     * 参数说明：
     *   - fd: DMA-BUF 文件描述符（RGA 转换输出的 RGBA 缓冲区）
     *   - width/height: 图像尺寸
     *   - stride: 每行字节数（可能因对齐而大于 width * 4）
     *
     * 返回：EGLImageKHR（成功）或 EGL_NO_IMAGE_KHR（失败）
     *
     * 关键：
     *   DRM_FORMAT_ABGR8888 对应内存中的 RGBA 顺序（小端序）
     *   EGL 会创建一个"引用"，指向 DMA-BUF 的物理内存，不会拷贝数据
     */
    EGLImageKHR importRGBA(int fd, int width, int height, int stride)
    {
        // EGL 属性列表：描述 DMA-BUF 的格式和布局
        EGLint attrs[] = {
            EGL_WIDTH,                     width,
            EGL_HEIGHT,                    height,
            EGL_LINUX_DRM_FOURCC_EXT,     DRM_FORMAT_ABGR8888,  // RGBA 内存序
            EGL_DMA_BUF_PLANE0_FD_EXT,    fd,           // DMA-BUF fd
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,           // 偏移量（通常为 0）
            EGL_DMA_BUF_PLANE0_PITCH_EXT,  stride,      // 每行字节数
            EGL_NONE
        };

        // 创建 EGLImage：将 DMA-BUF 包装为 EGL 可操作的对象
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
     * 
     * NV12 格式有两个平面：
     *   - Plane 0: Y（亮度）平面
     *   - Plane 1: UV（色度）平面
     *   两个平面在同一块 DMA-BUF 中，UV 紧跟在 Y 后面
     */
    EGLImageKHR importNV12(int fd, int width, int height, int stride)
    {
        EGLint attrs[] = {
            EGL_WIDTH,                     width,
            EGL_HEIGHT,                    height,
            EGL_LINUX_DRM_FOURCC_EXT,     DRM_FORMAT_NV12,
            // Plane 0: Y（亮度）
            EGL_DMA_BUF_PLANE0_FD_EXT,    fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,              // Y 从偏移 0 开始
            EGL_DMA_BUF_PLANE0_PITCH_EXT,  stride,
            // Plane 1: UV（色度）
            EGL_DMA_BUF_PLANE1_FD_EXT,    fd,              // 同一个 fd
            EGL_DMA_BUF_PLANE1_OFFSET_EXT, stride * height, // UV 紧跟在 Y 后面
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

    // ============================================================================
    // 将 EGLImage 绑定为 OpenGL 纹理
    // ============================================================================
    // 这一步将 EGLImage "挂载"到 OpenGL 纹理上
    // 之后 OpenGL 渲染这个纹理时，会直接读取 DMA-BUF 中的数据
    // 整个过程 GPU 内部完成，CPU 不参与
    // ============================================================================
    void bindToTexture(EGLImageKHR image, GLuint texture)
    {
        glBindTexture(GL_TEXTURE_2D, texture);
        // 关键函数：将 EGLImage 绑定到纹理
        // 之后 glBindTexture(GL_TEXTURE_2D, texture) 就能使用这个纹理
        glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, (GLeglImageOES)image);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // 销毁 EGLImage（释放引用，不释放物理内存）
    void destroyImage(EGLImageKHR image)
    {
        if (image != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR_(display_, image);
        }
    }

private:
    EGLDisplay display_ = EGL_NO_DISPLAY;

    // EGL 扩展函数指针（标准 EGL 中不存在，需要动态获取）
    typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(
        EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*);
    typedef EGLBoolean (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay, EGLImageKHR);
    typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum, GLeglImageOES);

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;
};

#endif // EGLIMAGE_HELPER_H
