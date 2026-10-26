#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>

/* 
====================================================
作用：EGL帧池类 - 管理DMA-BUF到OpenGL纹理的映射
说明：使用对象池模式复用EGLImage和OpenGL纹理，避免频繁创建销毁
硬件概念：DMA-BUF是Linux内核的缓冲区共享机制，允许不同硬件设备直接访问同一内存
【设计动机】逐帧 eglCreateImageKHR/Destroy 会走驱动 ioctl 与内存表重建，
  60fps 下开销可观；本池预生成 4 个纹理，换帧时仅重建 EGLImage、纹理对象
  复用，渲染端拿到的 texture ID 稳定，省去 uniform 重绑。
【槽位用量】调用方（GLVideoWidget）任意时刻只持有 1 个活动槽位——拿到新帧
  后才 release 上一槽，因此稳态下在第 0/1 槽间轮换，其余 3 槽是余量：
  允许 paintGL 落后于解码节奏时不丢帧，也留出后续改三缓冲的余地。
【归还语义】release() 只标记空闲并 close 自己 dup 的 fd，故意保留
  EGLImage/纹理绑定——槽位再次 acquire 时才销毁旧 image 并重建，
  因此"已 release"的槽位显存里可能仍是上一帧画面（无害，未被采样）。
【生命周期】init() 一次 → acquire/release 循环 → shutdown() 收尸，
  shutdown() 之后不可再使用本池（纹理 ID 已失效）。
====================================================
*/
class EglFramePool {
public:
    // 最大帧槽数量，限制同时缓存的帧数
    static constexpr int MAX_FRAMES = 4;

    /* 
    ====================================================
    作用：帧槽结构体 - 存储单个帧的所有资源
    说明：每个槽位包含EGLImage、OpenGL纹理和DMA-BUF文件描述符
    ====================================================
    */
    struct FrameSlot {
        EGLImageKHR image = EGL_NO_IMAGE_KHR;  // EGL图像对象，用于导入DMA-BUF
        GLuint texture = 0;                     // OpenGL纹理ID，用于GPU渲染
        int fd = -1;  // DMA-BUF的文件描述符（dup的副本）
        bool in_use = false;  // 槽位使用状态标记
    };

    /* 
    ====================================================
    作用：初始化帧池
    说明：预创建OpenGL纹理，设置纹理参数
    参数：createImg - EGL创建图像的函数指针
          destroyImg - EGL销毁图像的函数指针
          bindImg - OpenGL绑定EGL图像到纹理的函数指针
    硬件概念：OpenGL ES纹理是GPU可访问的图像数据格式
    ====================================================
    */
    void init(PFNEGLCREATEIMAGEKHRPROC createImg,
              PFNEGLDESTROYIMAGEKHRPROC destroyImg,
              PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bindImg)
    {
        // 保存函数指针，用于后续调用
        createImg_ = createImg;
        destroyImg_ = destroyImg;
        bindImg_ = bindImg;

        // 预创建所有纹理，避免运行时开销
        for (int i = 0; i < MAX_FRAMES; i++) {
            glGenTextures(1, &slots_[i].texture);  // 生成OpenGL纹理
            glBindTexture(GL_TEXTURE_2D, slots_[i].texture);  // 绑定纹理
            
            // 设置纹理过滤参数
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);  // 缩小过滤：线性插值
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);  // 放大过滤：线性插值
            
            // 设置纹理环绕参数
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);  // S轴：边缘裁剪
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);  // T轴：边缘裁剪
            
            glBindTexture(GL_TEXTURE_2D, 0);  // 解绑纹理
        }
    }

    /* 
    ====================================================
    作用：获取空闲槽位并导入DMA-BUF
    说明：从池中查找空闲槽位，将DMA-BUF导入为EGLImage并绑定到纹理
    参数：fd - DMA-BUF文件描述符
          width - 图像宽度
          height - 图像高度
          stride - 行跨度（字节数）
          format - 像素格式（DRM四字符码）
    返回值：成功返回纹理ID，失败返回0
    硬件概念：DMA-BUF导入允许GPU直接访问视频解码器输出的内存，无需CPU拷贝

    【为什么 dup(fd)】dup 出的副本与调用方原 fd 指向同一 dma-buf 内核
    对象（引用计数+1）：之后解码线程可以随时 close 原始 fd 归还缓冲池，
    而 GPU 侧的 EGLImage 引用依旧有效——这就是零拷贝流水线里
    "生产者回收"与"消费者渲染"解耦的所有权协议。副本由本池在
    release() 中 close，一次 acquire 对应至多一次 release。

    ⚠【失败即回滚】createImg_ 返回 EGL_NO_IMAGE_KHR 时（非单平面格式、
      stride 不合规、上游 fd 已失效都会走到这里）：本槽位立即 close 副本
      fd 并复位为空闲，继续尝试下一个槽；不会留下"in_use 但无人 release"
      的死槽。若所有槽都创建失败则返回 0，调用方需自行丢弃该帧。

    ⚠【单平面属性表】attrs 只描述 Plane0：仅适用于 RGBA/RGB/XRGB 等
      单平面 FourCC。传 DRM_FORMAT_NV12 会缺少 Plane1 的 fd/offset/pitch，
      GPU 读到未定义色度（花屏），勿复用本池导入 NV12。

    ⚠【线程约束】GL 对象（glGenTextures/glBindTexture/glTexParameteri）
      绑定到调用时的当前 GL 上下文：init/acquire/release 都必须在
      拥有该 widget 上下文的渲染线程（GUI 线程 paintGL 期间）执行；
      跨线程调用要么失败要么悄悄操作错误上下文。池本身无锁，
      非线程安全，仅供单一渲染线程独用。
    ====================================================
    */
    GLuint acquire(int fd, int width, int height, int stride, uint32_t format)
    {
        // 遍历查找空闲槽位
        for (int i = 0; i < MAX_FRAMES; i++) {
            if (slots_[i].in_use) continue;

            // 先 dup 再占用槽位：dup 失败说明入参 fd 无效，直接放弃
            int dupFd = dup(fd);
            if (dupFd < 0) {
                printf("[EglPool] dup(%d) failed: %s\n", fd, strerror(errno));
                return 0;
            }
            slots_[i].in_use = true;
            slots_[i].fd = dupFd;

            // 销毁旧的EGLImage（如果有）
            if (slots_[i].image != EGL_NO_IMAGE_KHR) {
                destroyImg_(eglGetCurrentDisplay(), slots_[i].image);
                slots_[i].image = EGL_NO_IMAGE_KHR;
            }

            // 设置EGLImage属性，描述DMA-BUF布局
            EGLint attrs[] = {
                EGL_WIDTH, width,  // 图像宽度
                EGL_HEIGHT, height,  // 图像高度
                EGL_LINUX_DRM_FOURCC_EXT, (EGLint)format,  // DRM像素格式
                EGL_DMA_BUF_PLANE0_FD_EXT, slots_[i].fd,  // DMA-BUF文件描述符
                EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,  // 平面偏移
                EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,  // 行跨度
                EGL_NONE  // 属性列表结束标记
            };

            // 创建EGLImage，将DMA-BUF导入为GPU可访问的图像
            slots_[i].image = createImg_(
                eglGetCurrentDisplay(), EGL_NO_CONTEXT,
                EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);

            if (slots_[i].image == EGL_NO_IMAGE_KHR) {
                printf("[EglPool] eglCreateImageKHR failed: 0x%x\n", eglGetError());
                close(slots_[i].fd);
                slots_[i].fd = -1;
                slots_[i].in_use = false;
                continue;
            }

            // 绑定到OpenGL纹理，纹理内容直接指向DMA-BUF数据
            glBindTexture(GL_TEXTURE_2D, slots_[i].texture);
            bindImg_(GL_TEXTURE_2D, (GLeglImageOES)slots_[i].image);
            glBindTexture(GL_TEXTURE_2D, 0);
            return slots_[i].texture;
        }
        return 0;  // 所有槽位已满或全部导入失败
    }

    /* 
    ====================================================
    作用：释放帧槽
    说明：将指定纹理对应的槽位标记为空闲，并关闭文件描述符
    参数：texture - 要释放的纹理ID
    ====================================================
    */
    void release(GLuint texture)
    {
        // 查找对应的槽位
        for (int i = 0; i < MAX_FRAMES; i++) {
            if (slots_[i].texture == texture) {
                slots_[i].in_use = false;  // 标记为空闲
                
                // 关闭文件描述符，释放DMA-BUF资源
                if (slots_[i].fd >= 0) {
                    close(slots_[i].fd);
                    slots_[i].fd = -1;
                }
                return;
            }
        }
    }

    /* 
    ====================================================
    作用：销毁帧池的全部 GL/EGL 资源
    说明：必须在拥有本池上下文的线程调用（makeCurrent 之后）
    ⚠ 不能指望"上下文销毁时驱动统一回收"：纹理（GL 对象）确实会随
      上下文消失，但 EGLImage 是 EGL 资源，不 eglDestroyImageKHR 就
      一直持有 dma-buf 的内核引用。widget 会被反复创建销毁
      （如 PhotoSelectionDialog 栈上构造），漏掉这一步等于每次开关
      对话框泄漏至多 MAX_FRAMES 块显存缓冲。
    ====================================================
    */
    void shutdown()
    {
        if (!destroyImg_) return;  // init() 未成功，无资源可清理
        for (int i = 0; i < MAX_FRAMES; i++) {
            if (slots_[i].image != EGL_NO_IMAGE_KHR) {
                destroyImg_(eglGetCurrentDisplay(), slots_[i].image);
                slots_[i].image = EGL_NO_IMAGE_KHR;
            }
            if (slots_[i].fd >= 0) {
                close(slots_[i].fd);
                slots_[i].fd = -1;
            }
            if (slots_[i].texture) {
                glDeleteTextures(1, &slots_[i].texture);
                slots_[i].texture = 0;
            }
            slots_[i].in_use = false;
        }
    }

private:
    FrameSlot slots_[MAX_FRAMES];  // 帧槽数组
    PFNEGLCREATEIMAGEKHRPROC createImg_ = nullptr;  // EGL创建图像函数指针
    PFNEGLDESTROYIMAGEKHRPROC destroyImg_ = nullptr;  // EGL销毁图像函数指针
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bindImg_ = nullptr;  // OpenGL绑定图像函数指针
};