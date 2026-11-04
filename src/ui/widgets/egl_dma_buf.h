// ============================================================================
// egl_dma_buf.h - EglDmaBufImporter：EGL DMA-BUF 导入的底层封装（header-only）
// ============================================================================
//
// 【所属模块】src/ui/widgets（零拷贝渲染三件套：本文件 + eglimage_helper.h +
//   EglFramePool.h）。职责只有一件：把 dma-buf 的 fd 按 DRM fourcc/pitch/offset
//   拼成 EGL 属性表，交给 EGL_EXT_image_dma_buf_import 换成 EGLImageKHR。
//
// 【当前状态：全仓零引用，属存档】grep 全仓没有任何 .cpp/.h include 本文件，
//   主流水线用的是 eglimage_helper.h 的 EglImageHelper（GLVideoWidget 的值成员
//   egl_）+ EglFramePool（它的 PFN 函数指针由 EglImageHelper::get*Proc() 喂）。
//   ⇒ 改了本文件编译器不会帮忙检查，也不会有任何运行期效果；真要接进来，
//     先决定和 EglImageHelper 谁是唯一入口，别留两份会各自漂移的导入实现。
//
// 【所有权约定（本文件不参与，但必须知道）】所有 import* 函数都**不接管 fd、
//   也不 dup**：eglCreateImageKHR 成功时驱动自己给这块 dma-buf 加一份内核引用，
//   调用方随后 close 自己的 fd 不影响 EGLImage。谁 close fd、谁 destroyImage，
//   由持有帧的那一层（RenderFrame / EglFramePool）负责，见各自文件头。
//
// 【线程约束】每个方法都必须在该 EGLDisplay/上下文 current 的线程上调用
//   （即 GUI 线程的 initializeGL/paintGL 期间）。eglGetCurrentDisplay() 只认
//   "当前线程已 current"的上下文，别的线程上调恒得 EGL_NO_DISPLAY。
// ============================================================================

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
// NOTE: 这三个 typedef 与 <EGL/eglext.h> 里的同名 typedef 是**重复定义**。
// 本机 Mesa 头（/usr/include/EGL/eglext.h:189 起）签名一致（attrib_list 为
// const EGLint*），C++11 起"完全相同的 typedef 重复"合法，所以现在编得过；
// 换成 Khronos EGL 1.5 的头（那里是 const EGLAttrib*）就是 conflicting
// declaration 直接编译失败。启用本文件前先确认工具链那一份头，或删掉本地 typedef。
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
    【调用时机】必须在"本线程已有 current EGL 上下文"时调用，即 GUI 线程的
    initializeGL（或任何 makeCurrent 之后）。否则 eglGetCurrentDisplay 返回
    EGL_NO_DISPLAY，第一行就 false 出去。可重复调用（幂等），每次重新抓指针。
    【失败面】三条 false 分支各有原因：无 current display / display 查不到扩展串
    / 缺 EGL_EXT_image_dma_buf_import（Mali 不会走到这条，走到的话是别的驱动）。
    【注意】display_ 是 init 那一刻缓存的，GL 上下文若被销毁重建，本对象不会
    自动跟进，必须再 init() 一次；否则 destroyImage 会拿着陈旧的 display 句柄调
    eglDestroyImageKHR，得到 EGL_BAD_DISPLAY 且 EGLImage 没被销毁（内核引用泄漏）。
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
    【fd 引用计数语义】eglCreateImageKHR 成功时驱动内部会对 fd 指向的
    dma-buf 增加一次内核引用；返回后调用方即可关闭自己的 fd 副本，
    EGLImage 的生命期与原 fd 无关（但物理内存会活到 destroyImage 为止）。
    因此传 dup 与否都可，本函数不接管 fd 所有权、也不做 dup。
    【NV12 的 UV 偏移】plane1 offset = stride * height：这是"Y 平面占满
    stride*height 字节后紧接着半分辨率 UV"的紧凑布局口径（RGA/硬解的
    NV12 都是这个排法）。上游若把 height 或 UV 行再做一次对齐（例如按 64
    向上取整），这里就得用对齐后的值，照抄 height 会读到错位的色度。
    【modifier 分支】只给 Plane0 写 LO/HI，NV12 的 Plane1 漏了（对比
    EglImageRenderer.h 的私有版本是两个平面都写）。本机 RGA 输出是线性
    布局，modifier 恒为 0/DRM_FORMAT_MOD_LINEAR，走不进这条分支，
    所以现状无害；真要上 AFBC 压缩格式，先补 Plane1 再测。
    ====================================================
    */
    EGLImageKHR importDmaBuf(int fd, int width, int height,
                              int stride, uint32_t format,
                              uint64_t modifier = 0)
    {
        // EGL属性列表
        // attrs 是"键,值"交替数组：最多用 2*(3+2+2+4)+1=25 个槽（含
        // NV12 双平面+modifier 分支），64 为安全上限，写满溢出即栈越界
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
    【与 importDmaBuf 的三点差别】① display 每次现场取 eglGetCurrentDisplay()
    而不是用 init() 缓存的 display_（历史遗留，两条路径的失败模式因此不同）；
    ② 只描述 Plane0，故只能喂单平面 fourcc（RGBA/RGB/XRGB），
    传 NV12 会得到一个缺色度平面的 image（导入本身可能成功，采样出来是花的）；
    ③ 不写 modifier，压缩布局（AFBC/UBWC）一律按线性解释。
    本函数仍要求先 init() 成功——它用的是 init() 抓来的 eglCreateImageKHR_ 指针。
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
    【前置条件】texture 必须是 glGenTextures 出来的有效 ID（传 0 会 GL_INVALID_OP）。
    规范里 glEGLImageTargetTexture2DOES 作用在"当前绑定到该 target 的纹理对象"上，
    所以顺序只能是 bind→target→unbind，本函数就是这个顺序。重复绑定同一纹理只是
    让它改指新的 image，不会泄漏，但也**不会**释放旧的 EGLImage
    （那是调用方 destroyImage 的事）。
    【线程】同文件头：只能在上下文 current 的线程上调。
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
    【为什么必须显式调它】EGLImage 是 EGL 侧资源，**不随 GL 上下文销毁而回收**：
    不 destroy 就一直占着那块 dma-buf 的内核引用。纹理对象才是"上下文没了就没了"
    的那一半（对照 EglFramePool::shutdown 的同一条注释）。
    【注意】用的是 init() 缓存的 display_，未 init 时是 EGL_NO_DISPLAY，
    调用只会得 EGL_BAD_DISPLAY 且 EGLImage 仍在；image 传 EGL_NO_IMAGE_KHR 直接跳过。
    调用点必须保证此时没有任何在途绘制还在采样这张图。
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