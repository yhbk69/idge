#ifndef EGL_DMA_BUF_H
#define EGL_DMA_BUF_H

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <cstdio>

/* 
====================================================
EGL扩展函数指针类型定义
说明：这些函数是EGL的扩展，用于DMA-BUF导入
硬件概念：DMA-BUF是Linux内核的缓冲区共享机制，允许不同硬件设备直接访问同一内存
====================================================
*/
typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay, EGLContext,
                                                  EGLenum, EGLClientBuffer,
                                                  const EGLint*);  // 创建EGL图像
typedef EGLBoolean (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay, EGLImageKHR);  // 销毁EGL图像
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum, GLeglImageOES);  // 绑定图像到纹理

/* 
====================================================
作用：EGL DMA-BUF导入器类
说明：负责将DMA-BUF文件描述符导入为EGL图像，并绑定到OpenGL纹理
硬件概念：实现视频解码器输出到GPU的零拷贝路径
====================================================
*/
class EglDmaBufImporter
{
public:
    /* 
    ====================================================
    作用：初始化导入器
    说明：检查EGL扩展支持，获取函数指针
    返回值：成功返回true，失败返回false
    ====================================================
    */
    bool init()
    {
        // 获取当前EGL显示设备
        display_ = eglGetCurrentDisplay();
        if (display_ == EGL_NO_DISPLAY) {
            printf("[EGL] No current display\n");
            return false;
        }

        /* 
        ====================================================
        检查DMA-BUF导入扩展支持
        说明：EGL_EXT_image_dma_buf_import是必需的扩展
        ====================================================
        */
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

        /* 
        ====================================================
        获取扩展函数指针
        说明：这些函数需要通过eglGetProcAddress动态获取
        ====================================================
        */
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

    /* 
    ====================================================
    作用：导入DMA-BUF为EGL图像
    说明：将DMA-BUF文件描述符转换为EGL可访问的图像对象
    参数：fd - DMA-BUF文件描述符
          width/height - 图像尺寸
          stride - Y平面行跨度（字节数）
          format - DRM像素格式（如DRM_FORMAT_NV12）
          modifier - 内存布局修饰符（0表示线性布局）
    返回值：EGLImageKHR句柄，失败返回EGL_NO_IMAGE_KHR
    硬件概念：DMA-BUF导入是零拷贝的关键，避免CPU内存拷贝
    ====================================================
    */
    EGLImageKHR importDmaBuf(int fd, int width, int height,
                              int stride, uint32_t format,
                              uint64_t modifier = 0)
    {
        // EGL属性列表
        EGLint attrs[64];
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
        Plane 0 属性（Y平面）
        说明：YUV420格式中，Y分量单独存储
        ====================================================
        */
        attrs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        attrs[i++] = fd;  // DMA-BUF文件描述符
        attrs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        attrs[i++] = 0;  // 平面偏移
        attrs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        attrs[i++] = stride;  // 行跨度

        /* 
        ====================================================
        Plane 1 属性（UV平面）- NV12/NV21格式
        说明：UV分量交织存储，占Y分量的一半高度
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
        线性布局（DRM_FORMAT_MOD_LINEAR）不需要设置修饰符
        ====================================================
        */
        if (modifier != 0 && modifier != DRM_FORMAT_MOD_LINEAR) {
            attrs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            attrs[i++] = (EGLint)(modifier & 0xFFFFFFFF);  // 修饰符低32位
            attrs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            attrs[i++] = (EGLint)(modifier >> 32);  // 修饰符高32位
        }

        attrs[i++] = EGL_NONE;  // 属性列表结束标记

        // 创建EGL图像
        EGLImageKHR image = eglCreateImageKHR_(
            display_, EGL_NO_CONTEXT,
            EGL_LINUX_DMA_BUF_EXT,
            nullptr, attrs);

        if (image == EGL_NO_IMAGE_KHR) {
            printf("[EGL] importDmaBuf failed: 0x%x\n", eglGetError());
        }

        return image;
    }

    /* 
    ====================================================
    作用：导入RGB格式的DMA-BUF
    说明：简化版导入函数，用于RGB格式图像
    参数：fd - 文件描述符，width/height - 尺寸
          stride - 行跨度，format - DRM格式
    返回值：EGLImageKHR句柄
    ====================================================
    */
    EGLImageKHR importDmaBufRgb(int fd, int width, int height, int stride, uint32_t format)
    {
        // RGB格式只有单平面
        EGLint attrs[] = {
            EGL_WIDTH, width,
            EGL_HEIGHT, height,
            EGL_LINUX_DRM_FOURCC_EXT, (EGLint)format,
            EGL_DMA_BUF_PLANE0_FD_EXT, fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
            EGL_NONE
        };

        // 创建EGL图像
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

    /* 
    ====================================================
    作用：将EGL图像绑定为GL纹理
    说明：建立纹理到DMA-BUF数据的映射
    参数：image - EGL图像句柄，texture - OpenGL纹理ID
    硬件概念：绑定后纹理内容直接指向DMA-BUF内存
    ====================================================
    */
    void bindToTexture(EGLImageKHR image, GLuint texture)
    {
        glBindTexture(GL_TEXTURE_2D, texture);  // 绑定纹理
        glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, (GLeglImageOES)image);  // 绑定图像到纹理
        glBindTexture(GL_TEXTURE_2D, 0);  // 解绑纹理
    }

    /* 
    ====================================================
    作用：销毁EGL图像
    说明：释放EGL图像资源
    参数：image - 要销毁的EGL图像句柄
    ====================================================
    */
    void destroyImage(EGLImageKHR image)
    {
        if (image != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR_(display_, image);
        }
    }

private:
    EGLDisplay display_ = EGL_NO_DISPLAY;  // EGL显示设备
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ = nullptr;  // 创建图像函数
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ = nullptr;  // 销毁图像函数
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;  // 绑定纹理函数
};

#endif // EGL_DMA_BUF_H