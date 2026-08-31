#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <cstdio>
class EglFramePool {
public:
    static constexpr int MAX_FRAMES = 4;

    struct FrameSlot {
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        GLuint texture = 0;
        int fd = -1;  // dup 的 fd
        bool in_use = false;
    };

    void init(PFNEGLCREATEIMAGEKHRPROC createImg,
              PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bindImg)
    {
        createImg_ = createImg;
        bindImg_ = bindImg;

        for (int i = 0; i < MAX_FRAMES; i++) {
            glGenTextures(1, &slots_[i].texture);
            glBindTexture(GL_TEXTURE_2D, slots_[i].texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }

    /**
     * 获取一个空闲 slot，导入 DMA-BUF
     * @return 纹理 ID，失败返回 0
     */
    GLuint acquire(int fd, int width, int height, int stride, uint32_t format)
    {
        for (int i = 0; i < MAX_FRAMES; i++) {
            if (!slots_[i].in_use) {
                slots_[i].in_use = true;
                slots_[i].fd = dup(fd);  // dup fd

                // 销毁旧 image
                if (slots_[i].image != EGL_NO_IMAGE_KHR) {
                    eglDestroyImageKHR(eglGetCurrentDisplay(), slots_[i].image);
                }

                // 导入新 image
                EGLint attrs[] = {
                    EGL_WIDTH, width,
                    EGL_HEIGHT, height,
                    EGL_LINUX_DRM_FOURCC_EXT, (EGLint)format,
                    EGL_DMA_BUF_PLANE0_FD_EXT, slots_[i].fd,
                    EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                    EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
                    EGL_NONE
                };

                slots_[i].image = createImg_(
                    eglGetCurrentDisplay(), EGL_NO_CONTEXT,
                    EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);

                if (slots_[i].image != EGL_NO_IMAGE_KHR) {
                    glBindTexture(GL_TEXTURE_2D, slots_[i].texture);
                    bindImg_(GL_TEXTURE_2D, (GLeglImageOES)slots_[i].image);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    return slots_[i].texture;
                }
            }
        }
        return 0;  // 池满
    }

    /**
     * 释放 slot
     */
    void release(GLuint texture)
    {
        for (int i = 0; i < MAX_FRAMES; i++) {
            if (slots_[i].texture == texture) {
                slots_[i].in_use = false;
                if (slots_[i].fd >= 0) {
                    close(slots_[i].fd);
                    slots_[i].fd = -1;
                }
                return;
            }
        }
    }

private:
    FrameSlot slots_[MAX_FRAMES];
    PFNEGLCREATEIMAGEKHRPROC createImg_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bindImg_ = nullptr;
};
