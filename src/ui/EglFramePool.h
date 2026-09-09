#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <cstdio>

/* 
====================================================
作用：EGL帧池类 - 管理DMA-BUF到OpenGL纹理的映射
说明：使用对象池模式复用EGLImage和OpenGL纹理，避免频繁创建销毁
硬件概念：DMA-BUF是Linux内核的缓冲区共享机制，允许不同硬件设备直接访问同一内存
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
          bindImg - OpenGL绑定EGL图像到纹理的函数指针
    硬件概念：OpenGL ES纹理是GPU可访问的图像数据格式
    ====================================================
    */
    void init(PFNEGLCREATEIMAGEKHRPROC createImg,
              PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bindImg)
    {
        // 保存函数指针，用于后续调用
        createImg_ = createImg;
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
    ====================================================
    */
    GLuint acquire(int fd, int width, int height, int stride, uint32_t format)
    {
        // 遍历查找空闲槽位
        for (int i = 0; i < MAX_FRAMES; i++) {
            if (!slots_[i].in_use) {
                slots_[i].in_use = true;  // 标记为使用中
                slots_[i].fd = dup(fd);  // 复制文件描述符，避免原始fd被关闭

                // 销毁旧的EGLImage（如果有）
                if (slots_[i].image != EGL_NO_IMAGE_KHR) {
                    eglDestroyImageKHR(eglGetCurrentDisplay(), slots_[i].image);
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

                // 如果EGLImage创建成功，绑定到OpenGL纹理
                if (slots_[i].image != EGL_NO_IMAGE_KHR) {
                    glBindTexture(GL_TEXTURE_2D, slots_[i].texture);  // 绑定纹理
                    // 将EGLImage绑定到纹理目标，使纹理内容指向DMA-BUF数据
                    bindImg_(GL_TEXTURE_2D, (GLeglImageOES)slots_[i].image);
                    glBindTexture(GL_TEXTURE_2D, 0);  // 解绑纹理
                    return slots_[i].texture;  // 返回纹理ID供渲染使用
                }
            }
        }
        return 0;  // 所有槽位已满，返回0表示失败
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

private:
    FrameSlot slots_[MAX_FRAMES];  // 帧槽数组
    PFNEGLCREATEIMAGEKHRPROC createImg_ = nullptr;  // EGL创建图像函数指针
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bindImg_ = nullptr;  // OpenGL绑定图像函数指针
};