#include "DmaFrameBuffer.h"



DmaFrameBuffer::DmaFrameBuffer(int width, int height, int format)
        : m_width(width), m_height(height), m_format(format)
{
    
}

int DmaFrameBuffer::alloc()
{
    m_fd = alloc_drm_buffer();
    return m_fd;

}

DmaFrameBuffer::~DmaFrameBuffer()
{
    
    release();
    // if (m_frame->data[0] != NULL)
    // {
    //     av_frame_unref(m_frame);
    // }
    
    
}

void DmaFrameBuffer::release ()
{
    free_drm_buffer();
}

int DmaFrameBuffer::alloc_drm_buffer() {
    std::lock_guard<std::mutex> lock(dma_mutex);   // 加锁，确保线程安全

    m_drm_fd = open("/dev/dri/card0", O_RDWR);
    if (m_drm_fd < 0) {
        perror("open /dev/dri/card0");
        return -1;
    }

    int bpp;
    switch (m_format) {
        case DRM_FORMAT_NV12:
        case DRM_FORMAT_NV21:
            bpp = 12;
            break;
        case DRM_FORMAT_RGB888:
        case DRM_FORMAT_BGR888:
            bpp = 24;
            break;
        default:  // XRGB8888, ARGB8888, RGBA8888 等
            bpp = 32;
            break;
    }

    struct drm_mode_create_dumb create = {};
    create.width = m_width;
    create.height = m_height;
    //create.bpp = 8;
    create.bpp = bpp;
    create.flags = 0;

    

    if (ioctl(m_drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        close(m_drm_fd);
        return -1;
    }

    m_stride = create.pitch;
    m_size = create.size;
    
    int fd = -1;
    struct drm_prime_handle prime_handle = {
        .handle = create.handle,
        .flags = DRM_CLOEXEC,
        .fd = -1,
    };
    m_handle = create.handle;
    

    if (ioctl(m_drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_handle) != 0) {
        perror("DRM_IOCTL_PRIME_HANDLE_TO_FD");
        close(m_drm_fd);
        return -1;
    }
    fd = prime_handle.fd;

    struct drm_mode_map_dumb map = {};
    map.handle = create.handle;
    if (ioctl(m_drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        close(fd);
        close(m_drm_fd);
        return -1;
    }
    void* ptr = mmap(0, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_drm_fd, map.offset);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        close(m_drm_fd);
        return -1;
    }
    m_ptr = (uint8_t*)ptr;

    
    close(m_drm_fd);
    return fd;
}

void DmaFrameBuffer::free_dma_buffer() {
    

}

void DmaFrameBuffer::free_drm_buffer() {
    std::lock_guard<std::mutex> lock(dma_mutex);   // 加锁，保护 close/munmap 并发
    if (m_ptr) {
        munmap(m_ptr, m_size);
        m_ptr = nullptr;
    }
    if (m_fd >= 0) {
        close(m_fd);
        m_fd= -1;
    }
    if (m_handle != 0) {
        struct drm_mode_destroy_dumb destroy = {};
        destroy.handle = m_handle;
        ioctl(m_drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        m_handle = 0;
    }
    m_size = 0;
    printf("DmaFrameBuffer released.\n");

}

int DmaFrameBuffer::fd() const {return m_fd;}
uint8_t* DmaFrameBuffer::ptr() const {return m_ptr;}
size_t DmaFrameBuffer::size() const {return m_size;}
int DmaFrameBuffer::width() const {return m_width;}
void DmaFrameBuffer::setWidth(int width) {m_width = width;}
int DmaFrameBuffer::height() const {return m_height;}
void DmaFrameBuffer::setHeight(int height) {m_height = height;}
int DmaFrameBuffer::format() const {return m_format;}
void DmaFrameBuffer::setFormat(int format) {m_format = format;}
void DmaFrameBuffer::setFrame(AVFrame* frame) {m_frame = frame;}
uint32_t DmaFrameBuffer::handle() {return m_handle;}
int DmaFrameBuffer::stride() const {return m_stride;}

