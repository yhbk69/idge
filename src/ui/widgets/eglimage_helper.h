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

// ============================================================================
// EglImageHelper - EGL DMA-BUF 导入辅助类
// ============================================================================
//
// 【用法与所有权】非 QObject、无锁、无析构负担（它只管缓存 EGLDisplay 和三个
//   函数指针，不持有任何 EGLImage/纹理）。一个 EGL 上下文配一份，作为值成员
//   嵌在 GLVideoWidget 里；它的存在意义只剩两件事：init() 做能力检查 +
//   get*Proc() 把函数指针交给 EglFramePool。
// 【现状】importRGBA / importRGB / importNV12 / bindToTexture / destroyImage
//   在全仓**零调用**（每帧走的是 EglFramePool 那条 acquire 路径）。
//   保留是为了留一份"不池化、直接导"的对照实现；改签名前想清楚是不是真要
//   再多一条 fd→EGLImage 的路径。
// 【线程】init() 的线程要求写在它自己头上（必须上下文 current）。display_ 是
//   缓存值：上下文若被销毁重建，必须重新 init()，否则 import*/destroyImage
//   拿着旧 EGLDisplay 只会得 EGL_BAD_DISPLAY。
// ============================================================================
class EglImageHelper
{
public:
    // 初始化 EGL 扩展函数
    // 必须在 OpenGL 上下文创建后调用（因为需要 eglGetCurrentDisplay）
    // ⚠ 线程约束：eglGetCurrentDisplay 只认"当前线程已 current 的上下文"，
    //   须在 GUI 线程的 initializeGL（或 makeCurrent 之后）调用，
    //   否则恒返回 EGL_NO_DISPLAY 导致 init 失败。
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
     * NOTE: 上面这行旧说明与实现不符——本函数走的是**单平面 RGB**
     * （fourcc 硬编码 DRM_FORMAT_BGR888，与 importRGBA 的 ABGR8888 不同），
     * 且不接 format 入参，传什么都按 BGR888 解释。真要用它，stride 必须
     * ≥ width*3 且上游确实是 R8G8B8 排法；RGA 那边目前不产这种缓冲，所以全仓零调用。
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
     * 约束：plane1 的 offset 用 stride*height，即"紧凑排布、无竖向对齐填充"；
     *   上游若把 height 再按 16/64 对齐，这里要换成对齐后的高度，否则色度错位。
     *   与 EglFramePool::acquire 的区别就在这一条——池只描述 Plane0，
     *   所以**别拿池去导 NV12**（会花屏）；将来要零拷贝直采解码器 NV12，
     *   走的应该是这里 + 一份带 BT.601/709 的着色器。
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
    // 【引用计数模型】dma-buf 物理内存由内核引用计数决定生死：
    //   分配者(fd) / 每个 dup / EGLImage 各持一份引用，全部释放后
    //   内核才回收页。所以 destroyImage 之后若源缓冲仍被解码器/池
    //   持有则数据依旧存在；反之先 close 了所有 fd 也不会让
    //   已导入的 EGLImage 失效——两边顺序无强约束，但"渲染中
    //   的 image 被 destroy"会导致纹理内容未定义，须保证
    //   destroy 晚于最后一帧采样（双缓冲交替即为此设计）。
    void destroyImage(EGLImageKHR image)
    {
        if (image != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR_(display_, image);
        }
    }

    // 获取 EGL 扩展函数指针（供 EglFramePool 使用）
    PFNEGLCREATEIMAGEKHRPROC getCreateImageProc() const { return eglCreateImageKHR_; }
    PFNEGLDESTROYIMAGEKHRPROC getDestroyImageProc() const { return eglDestroyImageKHR_; }
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC getBindTextureProc() const { return glEGLImageTargetTexture2DOES_; }

private:
    EGLDisplay display_ = EGL_NO_DISPLAY;

    // EGL 扩展函数指针（标准 EGL 中不存在，需要动态获取）
    // 使用 EGL 标准类型定义（与 EglFramePool 一致）
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;
};

#endif // EGLIMAGE_HELPER_H
