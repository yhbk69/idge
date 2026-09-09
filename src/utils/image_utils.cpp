#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <math.h>
#include <sys/time.h>

// ============================================================================
// Rockchip RGA硬件加速图像处理头文件
// im2d.h: Rockchip RGA2D图像处理库接口
// drmrga.h: DRM/RGA硬件加速接口
// RGA (Rockchip Graphics Acceleration) 是Rockchip芯片内置的2D图形加速引擎
// 支持图像缩放、旋转、格式转换等硬件加速操作
// ============================================================================
#include "im2d.h"
#include "drmrga.h"

// ============================================================================
// STB_IMAGE图像库配置
// STB_IMAGE是一个轻量级单头文件图像加载库
// 仅启用JPEG和PNG支持以减小编译体积
// ============================================================================
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "image_utils.h"
#include "file_utils.h"

// ============================================================================
// 支持的图像文件扩展名过滤列表
// 用于目录扫描时筛选图像文件
// ============================================================================
static const char* filter_image_names[] = {
    "jpg",
    "jpeg",
    "JPG",
    "JPEG",
    "png",
    "PNG",
    "data",
    NULL
};

// ============================================================================
// JPEG读写实现（使用turbojpeg库）
// turbojpeg是libjpeg的高性能封装，支持硬件加速的JPEG编解码
// ============================================================================
#ifndef DISABLE_LIBJPEG
#include "turbojpeg.h"
static const char* subsampName[TJ_NUMSAMP] = {"4:4:4", "4:2:2", "4:2:0", "Grayscale", "4:4:0", "4:1:1"};
static const char* colorspaceName[TJ_NUMCS] = {"RGB", "YCbCr", "GRAY", "CMYK", "YCCK"};

// ============================================================================
// read_image_jpeg - 使用turbojpeg读取JPEG图像
// 流程：读取JPEG文件 -> 解码头信息 -> 解码RGB像素数据
// ============================================================================
static int read_image_jpeg(const char* path, image_buffer_t* image)
{
    FILE* jpegFile = NULL;
    unsigned long jpegSize;
    int flags = 0;
    int width, height;
    int origin_width, origin_height;
    unsigned char* imgBuf = NULL;
    unsigned char* jpegBuf = NULL;
    unsigned long size;
    unsigned short orientation = 1;
    struct timeval tv1, tv2;

    // 打开JPEG文件
    if ((jpegFile = fopen(path, "rb")) == NULL) {
        printf("open input file failure\n");
    }
    if (fseek(jpegFile, 0, SEEK_END) < 0 || (size = ftell(jpegFile)) < 0 || fseek(jpegFile, 0, SEEK_SET) < 0) {
        printf("determining input file size failure\n");
    }
    if (size == 0) {
        printf("determining input file size, Input file contains no data\n");
    }

    // 读取整个JPEG文件到内存
    jpegSize = (unsigned long)size;
    if ((jpegBuf = (unsigned char*)malloc(jpegSize * sizeof(unsigned char))) == NULL) {
        printf("allocating JPEG buffer\n");
    }
    if (fread(jpegBuf, jpegSize, 1, jpegFile) < 1) {
        printf("reading input file");
    }
    fclose(jpegFile);
    jpegFile = NULL;

    tjhandle handle = NULL;
    int subsample, colorspace;
    int padding = 1;
    int ret = 0;

    // 初始化turbojpeg解压缩句柄
    handle = tjInitDecompress();

    // 解析JPEG头信息，获取宽高、色彩空间、采样率等参数
    ret = tjDecompressHeader3(handle, jpegBuf, size, &origin_width, &origin_height, &subsample, &colorspace);
    if (ret < 0) {
        printf("header file error, errorStr:%s, errorCode:%d\n", tjGetErrorStr(), tjGetErrorCode(handle));
        return -1;
    }

    // 对图像做16像素对齐裁剪，便于后续RGA硬件加速操作
    // RGA要求图像宽高为4/16的倍数才能进行硬件加速
    int crop_width = origin_width / 16 * 16;
    int crop_height = origin_height / 16 * 16;

    printf("origin size=%dx%d crop size=%dx%d\n", origin_width, origin_height, crop_width, crop_height);

    ret = tjDecompressHeader3(handle, jpegBuf, size, &width, &height, &subsample, &colorspace);
    if (ret < 0) {
        printf("header file error, errorStr:%s, errorCode:%d\n", tjGetErrorStr(), tjGetErrorCode(handle));
        return -1;
    }
    printf("input image: %d x %d, subsampling: %s, colorspace: %s, orientation: %d\n", 
            width, height, subsampName[subsample], colorspaceName[colorspace], orientation);

    // 分配RGB输出缓冲区
    int sw_out_size = width * height * 3;
    unsigned char* sw_out_buf = image->virt_addr;
    if (sw_out_buf == NULL) {
        sw_out_buf = (unsigned char*)malloc(sw_out_size * sizeof(unsigned char));
    }
    if (sw_out_buf == NULL) {
        printf("sw_out_buf is NULL\n");
        if (jpegBuf) {
            free(jpegBuf);
        }
        return 0;
    }

    flags |= 0;

    // 执行JPEG解压缩，将JPEG数据解码为RGB888格式像素数据
    // 错误码为0时表示警告，-1时表示错误
    int pixelFormat = TJPF_RGB;
    ret = tjDecompress2(handle, jpegBuf, size, sw_out_buf, width, 0, height, pixelFormat, flags);
    if ((0 != tjGetErrorCode(handle)) && (ret < 0)) {
        printf("error : decompress to yuv failed, errorStr:%s, errorCode:%d\n", tjGetErrorStr(),
               tjGetErrorCode(handle));
        if (jpegBuf) {
            free(jpegBuf);
        }
        return 0;
    }
    if ((0 == tjGetErrorCode(handle)) && (ret < 0)) {
        printf("warning : errorStr:%s, errorCode:%d\n", tjGetErrorStr(), tjGetErrorCode(handle));
    }
    tjDestroy(handle);

    // 设置图像输出参数
    image->width = width;
    image->height = height;
    image->format = IMAGE_FORMAT_RGB888;  // 输出格式为RGB888
    image->virt_addr = sw_out_buf;
    image->size = sw_out_size;

    if (jpegBuf) {
        free(jpegBuf);
    }
    return 0;
}

// ============================================================================
// write_image_jpeg - 使用turbojpeg将RGB图像编码为JPEG文件
// 流程：初始化压缩器 -> 设置参数 -> 执行压缩 -> 写入文件
// ============================================================================
static int write_image_jpeg(const char* path, int quality, image_buffer_t* image)
{
    int ret;
    int jpegSubsamp = TJSAMP_422;  // 使用4:2:2色度子采样，平衡质量与文件大小
    unsigned char* jpegBuf = NULL;
    unsigned long jpegSize = 0;
    int flags = 0;

    const unsigned char* data = image->virt_addr;
    int width = image->width;
    int height = image->height;
    int pixelFormat = TJPF_RGB;

    // 初始化turbojpeg压缩句柄
    tjhandle handle = tjInitCompress();

    if (image->format == IMAGE_FORMAT_RGB888) {
        // 执行JPEG压缩，quality参数控制压缩质量（1-100）
        ret = tjCompress2(handle, data, width, 0, height, pixelFormat, &jpegBuf, &jpegSize, jpegSubsamp, quality, flags);
    } else {
        printf("write_image_jpeg: pixel format %d not support\n", image->format);
        return -1;
    }

    // 将压缩后的JPEG数据写入文件
    if (jpegBuf != NULL && jpegSize > 0) {
        write_data_to_file(path, (char*)jpegBuf, (unsigned int)jpegSize);
        tjFree(jpegBuf);
    }
    tjDestroy(handle);

    return 0;
}
#endif

// ============================================================================
// image_file_filter - 文件过滤函数
// 用于目录扫描时筛选支持的图像文件格式
// ============================================================================
static int image_file_filter(const struct dirent *entry)
{
    const char ** filter;

    for (filter = filter_image_names; *filter; ++filter) {
        if(strstr(entry->d_name, *filter) != NULL) {
            return 1;
        }
    }
    return 0;
}

// ============================================================================
// read_image_raw - 读取原始二进制图像数据
// 用于读取未经编码的RAW格式图像（如摄像头原始输出）
// ============================================================================
static int read_image_raw(const char* path, image_buffer_t* image)
{
    FILE *fp = fopen(path, "rb");
    if(fp == NULL) {
        printf("fopen %s fail!\n", path);
        return -1;
    }

    // 获取文件大小
    fseek(fp, 0, SEEK_END);
    int file_size = ftell(fp);

    unsigned char *data = image->virt_addr;
    if (image->virt_addr == NULL) {
        data = (unsigned char *)malloc(file_size+1);
    }
    data[file_size] = 0;

    // 读取全部数据
    fseek(fp, 0, SEEK_SET);
    if(file_size != fread(data, 1, file_size, fp)) {
        printf("fread %s fail!\n", path);
        free(data);
        return -1;
    }
    if(fp) {
        fclose(fp);
    }

    if (image->virt_addr == NULL) {
        image->virt_addr = data;
        image->size = file_size;
    }

    return 0;
}

// ============================================================================
// read_image_stb - 使用STB_IMAGE读取图像文件
// 支持PNG、JPEG、BMP等格式的自动识别和加载
// ============================================================================
static int read_image_stb(const char* path, image_buffer_t* image)
{
    // 加载图像，自动检测通道数（1=灰度, 3=RGB, 4=RGBA）
    int w, h, c;
    unsigned char* pixeldata = stbi_load(path, &w, &h, &c, 0);
    if (!pixeldata) {
        printf("error: read image %s fail\n", path);
        return -1;
    }

    int size = w * h * c;

    // 设置图像数据到输出结构体
    if (image->virt_addr != NULL) {
        // 如果输出缓冲区已分配，复制数据
        memcpy(image->virt_addr, pixeldata, size);
        stbi_image_free(pixeldata);
    } else {
        // 否则直接使用STB分配的内存（调用者需负责释放）
        image->virt_addr = pixeldata;
    }

    image->width = w;
    image->height = h;

    // 根据通道数设置对应的像素格式
    if (c == 4) {
        image->format = IMAGE_FORMAT_RGBA8888;
    } else if (c == 1) {
        image->format = IMAGE_FORMAT_GRAY8;
    } else {
        image->format = IMAGE_FORMAT_RGB888;
    }
    return 0;
}

// ============================================================================
// read_image - 通用图像读取接口
// 根据文件扩展名自动选择合适的解码器
// ============================================================================
int read_image(const char* path, image_buffer_t* image)
{
    const char* _ext = strrchr(path, '.');
    if (!_ext) {
        return -1;  // 缺少文件扩展名
    }

    if (strcmp(_ext, ".data") == 0) {
        // 原始RAW数据文件
        return read_image_raw(path, image);
#ifndef DISABLE_LIBJPEG
    } else if (strcmp(_ext, ".jpg") == 0 || strcmp(_ext, ".jpeg") == 0 || strcmp(_ext, ".JPG") == 0 ||
        strcmp(_ext, ".JPEG") == 0) {
        // JPEG文件使用turbojpeg解码（性能更优）
        return read_image_jpeg(path, image);
#endif
    } else {
        // 其他格式（PNG/BMP等）使用STB_IMAGE解码
        return read_image_stb(path, image);
    }
}

// ============================================================================
// write_image - 通用图像写入接口
// 根据文件扩展名自动选择合适的编码器
// ============================================================================
int write_image(const char* path, image_buffer_t* img)
{
    int ret;
    int width = img->width;
    int height = img->height;
    int channel = 3;
    void* data = img->virt_addr;

    // 根据像素格式确定通道数
    switch (img->format)
    {
    case IMAGE_FORMAT_RGBA8888:
        channel = 4;
        break;
    case IMAGE_FORMAT_RGB888:
        channel = 3;
    default:
        channel = 3;
        break;
    }

    printf("write_image path: %s width=%d height=%d channel=%d data=%p\n",
        path, width, height, channel, data);

    const char* _ext = strrchr(path, '.');
    if (!_ext) {
        return -1;  // 缺少文件扩展名
    }

    if (strcmp(_ext, ".png") == 0 | strcmp(_ext, ".PNG") == 0) {
        // PNG格式：使用STB_IMAGE_WRITE编码
        ret = stbi_write_png(path, width, height, channel, data, 0);

    } else if (strcmp(_ext, ".jpg") == 0 || strcmp(_ext, ".jpeg") == 0 || strcmp(_ext, ".JPG") == 0 ||
        strcmp(_ext, ".JPEG") == 0) {
        int quality = 95;  // JPEG压缩质量
#ifndef DISABLE_LIBJPEG
        // 使用turbojpeg编码（性能更优）
        ret = write_image_jpeg(path, quality, img);
#else
        // 备用方案：使用STB_IMAGE_WRITE编码
        ret = stbi_write_jpg(path, width, height, channel, data, quality);
#endif
    } else if (strcmp(_ext, ".data") == 0 | strcmp(_ext, ".DATA") == 0) {
        // 原始数据格式：直接写入二进制数据
        int size = get_image_size(img);
        ret = write_data_to_file(path, (char*)data, size);
    } else {
        return -1;  // 不支持的文件格式
    }
    return ret;
}

// ============================================================================
// crop_and_scale_image_c - 通用图像裁剪缩放（纯C实现）
// 使用双线性插值算法实现图像缩放
// 支持任意通道数的图像处理
// ============================================================================
static int crop_and_scale_image_c(int channel, unsigned char *src, int src_width, int src_height,
                                    int crop_x, int crop_y, int crop_width, int crop_height,
                                    unsigned char *dst, int dst_width, int dst_height,
                                    int dst_box_x, int dst_box_y, int dst_box_width, int dst_box_height) {
    if (dst == NULL) {
        printf("dst buffer is null\n");
        return -1;
    }

    // 计算缩放比例：源区域与目标区域的尺寸比
    float x_ratio = (float)crop_width / (float)dst_box_width;
    float y_ratio = (float)crop_height / (float)dst_box_height;

    // 双线性插值缩放算法
    // 遍历目标图像的每个像素，在源图像中找到对应位置并进行插值
    for (int dst_y = dst_box_y; dst_y < dst_box_y + dst_box_height; dst_y++) {
        for (int dst_x = dst_box_x; dst_x < dst_box_x + dst_box_width; dst_x++) {
            int dst_x_offset = dst_x - dst_box_x;
            int dst_y_offset = dst_y - dst_box_y;

            // 映射到源图像坐标
            int src_x = (int)(dst_x_offset * x_ratio) + crop_x;
            int src_y = (int)(dst_y_offset * y_ratio) + crop_y;

            // 计算插值权重
            float x_diff = (dst_x_offset * x_ratio) - (src_x - crop_x);
            float y_diff = (dst_y_offset * y_ratio) - (src_y - crop_y);

            // 获取四个相邻像素的索引
            int index1 = src_y * src_width * channel + src_x * channel;
            int index2 = index1 + src_width * channel;    // 下方像素
            if (src_y == src_height - 1) {
                // 边界处理：到底部边缘时使用上方像素
                index2 = index1 - src_width * channel;
            }
            int index3 = index1 + 1 * channel;            // 右侧像素
            int index4 = index2 + 1 * channel;            // 右下方像素
            if (src_x == src_width - 1) {
                // 边界处理：到右边缘时使用左侧像素
                index3 = index1 - 1 * channel;
                index4 = index2 - 1 * channel;
            }

            // 对每个通道执行双线性插值
            // 公式: P = A*(1-dx)*(1-dy) + B*dx*(1-dy) + C*dy*(1-dx) + D*dx*dy
            for (int c = 0; c < channel; c++) {
                unsigned char A = src[index1+c];
                unsigned char B = src[index3+c];
                unsigned char C = src[index2+c];
                unsigned char D = src[index4+c];

                unsigned char pixel = (unsigned char)(
                    A * (1 - x_diff) * (1 - y_diff) +
                    B * x_diff * (1 - y_diff) +
                    C * y_diff * (1 - x_diff) +
                    D * x_diff * y_diff
                );

                dst[(dst_y * dst_width  + dst_x) * channel + c] = pixel;
            }
        }
    }

    return 0;
}

// ============================================================================
// crop_and_scale_image_yuv420sp - YUV420SP(NV12/NV21)格式裁剪缩放
// YUV420SP格式：Y分量全分辨率，UV分量1/4分辨率（色度子采样）
// 分别对Y和UV分量进行缩放
// ============================================================================
static int crop_and_scale_image_yuv420sp(unsigned char *src, int src_width, int src_height,
                                    int crop_x, int crop_y, int crop_width, int crop_height,
                                    unsigned char *dst, int dst_width, int dst_height,
                                    int dst_box_x, int dst_box_y, int dst_box_width, int dst_box_height) {

    // YUV420SP内存布局：Y分量在前，UV交错存储在后
    unsigned char* src_y = src;
    unsigned char* src_uv = src + src_width * src_height;

    unsigned char* dst_y = dst;
    unsigned char* dst_uv = dst + dst_width * dst_height;

    // 对Y分量进行缩放（单通道）
    crop_and_scale_image_c(1, src_y, src_width, src_height, crop_x, crop_y, crop_width, crop_height,
        dst_y, dst_width, dst_height, dst_box_x, dst_box_y, dst_box_width, dst_box_height);
    
    // 对UV分量进行缩放（双通道，宽高各为Y的一半）
    crop_and_scale_image_c(2, src_uv, src_width / 2, src_height / 2, crop_x / 2, crop_y / 2, crop_width / 2, crop_height / 2,
        dst_uv, dst_width / 2, dst_height / 2, dst_box_x, dst_box_y, dst_box_width, dst_box_height);

    return 0;
}

// ============================================================================
// convert_image_cpu - CPU方式实现图像转换
// 使用纯软件算法进行图像格式转换和缩放
// 作为RGA硬件加速不可用时的备用方案
// ============================================================================
static int convert_image_cpu(image_buffer_t *src, image_buffer_t *dst, image_rect_t *src_box, image_rect_t *dst_box, char color) {
    int ret;
    if (dst->virt_addr == NULL) {
        return -1;
    }
    if (src->virt_addr == NULL) {
        return -1;
    }
    // CPU转换要求源和目标格式相同
    if (src->format != dst->format) {
        return -1;
    }

    // 解析源图像裁剪区域（默认为整个图像）
    int src_box_x = 0;
    int src_box_y = 0;
    int src_box_w = src->width;
    int src_box_h = src->height;
    if (src_box != NULL) {
        src_box_x = src_box->left;
        src_box_y = src_box->top;
        src_box_w = src_box->right - src_box->left + 1;
        src_box_h = src_box->bottom - src_box->top + 1;
    }

    // 解析目标图像裁剪区域（默认为整个图像）
    int dst_box_x = 0;
    int dst_box_y = 0;
    int dst_box_w = dst->width;
    int dst_box_h = dst->height;
    if (dst_box != NULL) {
        dst_box_x = dst_box->left;
        dst_box_y = dst_box->top;
        dst_box_w = dst_box->right - dst_box->left + 1;
        dst_box_h = dst_box->bottom - dst_box->top + 1;
    }

    // 用填充颜色清空目标图像背景
    if (dst_box_w != dst->width || dst_box_h != dst->height) {
        int dst_size = get_image_size(dst);
        memset(dst->virt_addr, color, dst_size);
    }

    int need_release_dst_buffer = 0;
    int reti = 0;

    // 根据像素格式选择对应的缩放函数
    if (src->format == IMAGE_FORMAT_RGB888) {
        reti = crop_and_scale_image_c(3, src->virt_addr, src->width, src->height,
            src_box_x, src_box_y, src_box_w, src_box_h,
            dst->virt_addr, dst->width, dst->height,
            dst_box_x, dst_box_y, dst_box_w, dst_box_h);
    } else if (src->format == IMAGE_FORMAT_RGBA8888) {
        reti = crop_and_scale_image_c(4, src->virt_addr, src->width, src->height,
            src_box_x, src_box_y, src_box_w, src_box_h,
            dst->virt_addr, dst->width, dst->height,
            dst_box_x, dst_box_y, dst_box_w, dst_box_h);
    } else if (src->format == IMAGE_FORMAT_GRAY8) {
        reti = crop_and_scale_image_c(1, src->virt_addr, src->width, src->height,
            src_box_x, src_box_y, src_box_w, src_box_h,
            dst->virt_addr, dst->width, dst->height,
            dst_box_x, dst_box_y, dst_box_w, dst_box_h);
    } else if (src->format == IMAGE_FORMAT_YUV420SP_NV12 || src->format == IMAGE_FORMAT_YUV420SP_NV21) {
        reti = crop_and_scale_image_yuv420sp(src->virt_addr, src->width, src->height,
            src_box_x, src_box_y, src_box_w, src_box_h,
            dst->virt_addr, dst->width, dst->height,
            dst_box_x, dst_box_y, dst_box_w, dst_box_h);
    } else {
        printf("no support format %d\n", src->format);
    }

    if (reti != 0) {
        printf("convert_image_cpu fail %d\n", reti);
        return -1;
    }
    printf("finish\n");
    return 0;
}

// ============================================================================
// get_rga_fmt - 将内部图像格式转换为RGA格式标识
// RGA使用不同的格式枚举值来指定图像格式
// ============================================================================
static int get_rga_fmt(image_format_t fmt) {
    switch (fmt)
    {
    case IMAGE_FORMAT_RGB888:
        return RK_FORMAT_RGB_888;
    case IMAGE_FORMAT_RGBA8888:
        return RK_FORMAT_RGBA_8888;
    case IMAGE_FORMAT_YUV420SP_NV12:
        return RK_FORMAT_YCbCr_420_SP;   // NV12: YUV交错，U在前V在后
    case IMAGE_FORMAT_YUV420SP_NV21:
        return RK_FORMAT_YCrCb_420_SP;   // NV21: YUV交错，V在前U在后
    default:
        return -1;
    }
}

// ============================================================================
// get_image_size - 根据图像格式和尺寸计算图像数据大小
// ============================================================================
int get_image_size(const image_buffer_t* image)
{
    if (image == NULL) {
        return 0;
    }
    switch (image->format)
    {
    case IMAGE_FORMAT_GRAY8:
        return image->width * image->height;           // 灰度图: 1字节/像素
    case IMAGE_FORMAT_RGB888:
        return image->width * image->height * 3;       // RGB: 3字节/像素
    case IMAGE_FORMAT_RGBA8888:
        return image->width * image->height * 4;       // RGBA: 4字节/像素
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        return image->width * image->height * 3 / 2;   // YUV420: 1.5字节/像素
    default:
        break;
    }
}

// ============================================================================
// convert_image_rga - 使用Rockchip RGA硬件加速实现图像转换
// RGA是Rockchip芯片内置的2D图形硬件加速器
// 支持图像缩放、旋转、格式转换等操作，速度远快于CPU处理
// 通过DRM框架与内核驱动交互，实现零拷贝图像处理
// ============================================================================
static int convert_image_rga(image_buffer_t* src_img, image_buffer_t* dst_img, image_rect_t* src_box, image_rect_t* dst_box, char color)
{
    // 配置RGA调度核心：使用RGA3的双核心进行并行处理
    imconfig(IM_CONFIG_SCHEDULER_CORE, IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1);
    int ret = 0;

    int srcWidth = src_img->width;
    int srcHeight = src_img->height;
    void *src = src_img->virt_addr;
    int src_fd = src_img->fd;               // DMA缓冲区文件描述符，用于零拷贝访问
    void *src_phy = NULL;                    // 物理地址（部分场景需要）
    int srcFmt = get_rga_fmt(src_img->format);

    int dstWidth = dst_img->width;
    int dstHeight = dst_img->height;
    void *dst = dst_img->virt_addr;
    int dst_fd = dst_img->fd;
    void *dst_phy = NULL;
    int dstFmt = get_rga_fmt(dst_img->format);

    int rotate = 0;  // 旋转角度：0/90/180/270

    int use_handle = 0;
#if defined(LIBRGA_IM2D_HANDLE)
    use_handle = 1;
#endif

    int usage = 0;
    IM_STATUS ret_rga = IM_STATUS_NOERROR;

    usage |= rotate;

    // 设置源图像裁剪区域
    im_rect srect;
    im_rect drect;
    im_rect prect;      // 叠加图像区域（此处未使用）
    memset(&prect, 0, sizeof(im_rect));

    if (src_box != NULL) {
        srect.x = src_box->left;
        srect.y = src_box->top;
        srect.width = src_box->right - src_box->left + 1;
        srect.height = src_box->bottom - src_box->top + 1;
    } else {
        srect.x = 0;
        srect.y = 0;
        srect.width = srcWidth;
        srect.height = srcHeight;
    }

    // 设置目标图像裁剪区域
    if (dst_box != NULL) {
        drect.x = dst_box->left;
        drect.y = dst_box->top;
        drect.width = dst_box->right - dst_box->left + 1;
        drect.height = dst_box->bottom - dst_box->top + 1;
    } else {
        drect.x = 0;
        drect.y = 0;
        drect.width = dstWidth;
        drect.height = dstHeight;
    }

    // 初始化RGA缓冲区对象
    rga_buffer_t rga_buf_src;
    rga_buffer_t rga_buf_dst;
    rga_buffer_t pat;                       // 模板缓冲区（用于overlay操作）
    rga_buffer_handle_t rga_handle_src = 0;
    rga_buffer_handle_t rga_handle_dst = 0;
    memset(&pat, 0, sizeof(rga_buffer_t));

    // 源图像参数
    im_handle_param_t in_param;
    in_param.width = srcWidth;
    in_param.height = srcHeight;
    in_param.format = srcFmt;

    // 目标图像参数
    im_handle_param_t dst_param;
    dst_param.width = dstWidth;
    dst_param.height = dstHeight;
    dst_param.format = dstFmt;

    // 导入源缓冲区到RGA
    // 支持三种方式：物理地址、DMA文件描述符、虚拟地址
    if (use_handle) {
        if (src_phy != NULL) {
            rga_handle_src = importbuffer_physicaladdr((uint64_t)src_phy, &in_param);
        } else if (src_fd > 0) {
            rga_handle_src = importbuffer_fd(src_fd, &in_param);  // DMA零拷贝方式
        } else {
            rga_handle_src = importbuffer_virtualaddr(src, &in_param);
        }
        if (rga_handle_src <= 0) {
            printf("src handle error %d\n", rga_handle_src);
            ret = -1;
            goto err;
        }
        rga_buf_src = wrapbuffer_handle(rga_handle_src, srcWidth, srcHeight, srcFmt, srcWidth, srcHeight);
    } else {
        if (src_phy != NULL) {
            rga_buf_src = wrapbuffer_physicaladdr(src_phy, srcWidth, srcHeight, srcFmt, srcWidth, srcHeight);
        } else if (src_fd > 0) {
            rga_buf_src = wrapbuffer_fd(src_fd, srcWidth, srcHeight, srcFmt, srcWidth, srcHeight);
        } else {
            rga_buf_src = wrapbuffer_virtualaddr(src, srcWidth, srcHeight, srcFmt, srcWidth, srcHeight);
        }
    }

    // 导入目标缓冲区到RGA
    if (use_handle) {
        if (dst_phy != NULL) {
            rga_handle_dst = importbuffer_physicaladdr((uint64_t)dst_phy, &dst_param);
        } else if (dst_fd > 0) {
            rga_handle_dst = importbuffer_fd(dst_fd, &dst_param);
        } else {
            rga_handle_dst = importbuffer_virtualaddr(dst, &dst_param);
        }
        if (rga_handle_dst <= 0) {
            printf("dst handle error %d\n", rga_handle_dst);
            ret = -1;
            goto err;
        }
        rga_buf_dst = wrapbuffer_handle(rga_handle_dst, dstWidth, dstHeight, dstFmt, dstWidth, dstHeight);
    } else {
        if (dst_phy != NULL) {
            rga_buf_dst = wrapbuffer_physicaladdr(dst_phy, dstWidth, dstHeight, dstFmt, dstWidth, dstHeight);
        } else if (dst_fd > 0) {
            rga_buf_dst = wrapbuffer_fd(dst_fd, dstWidth, dstHeight, dstFmt, dstWidth, dstHeight);
        } else {
            rga_buf_dst = wrapbuffer_virtualaddr(dst, dstWidth, dstHeight, dstFmt, dstWidth, dstHeight);
        }
    }

    // 如果目标区域不等于整个图像，先用填充颜色清空目标图像
    if (drect.width != dstWidth || drect.height != dstHeight) {
        im_rect dst_whole_rect = {0, 0, dstWidth, dstHeight};
        char imcolor;
        char* p_imcolor = &imcolor;
        p_imcolor[0] = color;
        p_imcolor[1] = color;
        p_imcolor[2] = color;
        p_imcolor[3] = color;
        printf("fill dst image (x y w h)=(%d %d %d %d) with color=0x%x\n",
            dst_whole_rect.x, dst_whole_rect.y, dst_whole_rect.width, dst_whole_rect.height, imcolor);

        // 使用RGA填充背景色（当地址超过4GB限制时回退到memset）
        ret_rga = imfill(rga_buf_dst, dst_whole_rect, imcolor); 
        if (ret_rga <= 0) {
            if (dst != NULL) {
                size_t dst_size = get_image_size(dst_img);
                memset(dst, color, dst_size);  // 回退方案：使用CPU填充
            } else {
                printf("Warning: Can not fill color on target image\n");
            }
        }
    }

    // 执行RGA硬件加速图像处理
    // 这是核心操作，完成图像缩放、格式转换等功能
    struct timeval start_time, stop_time;
    gettimeofday(&start_time, NULL);
    ret_rga = improcess(rga_buf_src, rga_buf_dst, pat, srect, drect, prect, usage);
    gettimeofday(&stop_time, NULL);
    if (ret_rga <= 0) {
        printf("Error on improcess STATUS=%d\n", ret_rga);
        printf("RGA error message: %s\n", imStrError((IM_STATUS)ret_rga));
        ret = -1;
    }

err:
    // 释放RGA缓冲区句柄
    if (rga_handle_src > 0) {
        releasebuffer_handle(rga_handle_src);
    }
    if (rga_handle_dst > 0) {
        releasebuffer_handle(rga_handle_dst);
    }

    return ret;
}

// ============================================================================
// convert_image - 通用图像转换接口
// 自动选择RGA硬件加速或CPU软处理方式
// 优先使用RGA以获得更高性能，当宽高不满足16对齐时回退到CPU
// ============================================================================
int convert_image(image_buffer_t* src_img, image_buffer_t* dst_img, image_rect_t* src_box, image_rect_t* dst_box, char color)
{
    int ret;

    // RGA要求图像宽高为16像素对齐才能进行硬件加速
    if(src_img->width % 16 == 0 && dst_img->width % 16 == 0) {
        ret = convert_image_rga(src_img, dst_img, src_box, dst_box, color);
        if (ret != 0) {
            printf("try convert image use cpu\n");
            // RGA失败时回退到CPU处理
            ret = convert_image_cpu(src_img, dst_img, src_box, dst_box, color);
        }
    } else {
        printf("src width is not 4/16-aligned, convert image use cpu\n");
        // 宽高不对齐时直接使用CPU处理
        ret = convert_image_cpu(src_img, dst_img, src_box, dst_box, color);
    }
    return ret;
}

// ============================================================================
// convert_image_with_letterbox - 带LetterBox的图像转换
// LetterBox是目标检测常用的预处理方式：
// 1. 保持图像宽高比缩放到目标尺寸
// 2. 在短边方向居中填充指定颜色
// 3. 返回填充偏移量和缩放比例，用于后处理还原检测框坐标
// ============================================================================
int convert_image_with_letterbox(image_buffer_t* src_image, image_buffer_t* dst_image, letterbox_t* letterbox, char color)
{
    int ret = 0;
    int allow_slight_change = 1;  // 允许轻微调整尺寸以满足对齐要求
    int src_w = src_image->width;
    int src_h = src_image->height;
    int dst_w = dst_image->width;
    int dst_h = dst_image->height;
    int resize_w = dst_w;
    int resize_h = dst_h;

    int padding_w = 0;
    int padding_h = 0;

    int _left_offset = 0;
    int _top_offset = 0;
    float scale = 1.0;

    // 源图像裁剪区域（整个图像）
    image_rect_t src_box;
    src_box.left = 0;
    src_box.top = 0;
    src_box.right = src_image->width - 1;
    src_box.bottom = src_image->height - 1;

    // 目标图像裁剪区域（整个图像）
    image_rect_t dst_box;
    dst_box.left = 0;
    dst_box.top = 0;
    dst_box.right = dst_image->width - 1;
    dst_box.bottom = dst_image->height - 1;

    // 计算缩放比例：取宽高缩放比例中的较小值，确保图像完全放入目标框
    float _scale_w = (float)dst_w / src_w;
    float _scale_h = (float)dst_h / src_h;
    if(_scale_w < _scale_h) {
        scale = _scale_w;
        resize_h = (int) src_h*scale;
    } else {
        scale = _scale_h;
        resize_w = (int) src_w*scale;
    }

    // 微调尺寸以满足4像素对齐要求（便于RGA硬件加速）
    if (allow_slight_change == 1 && (resize_w % 4 != 0)) {
        resize_w -= resize_w % 4;
    }
    if (allow_slight_change == 1 && (resize_h % 2 != 0)) {
        resize_h -= resize_h % 2;
    }

    // 计算填充量
    padding_h = dst_h - resize_h;
    padding_w = dst_w - resize_w;

    // 居中放置：计算填充偏移量
    if (_scale_w < _scale_h) {
        dst_box.top = padding_h / 2;
        if (dst_box.top % 2 != 0) {
            dst_box.top -= dst_box.top % 2;
            if (dst_box.top < 0) {
                dst_box.top = 0;
            }
        }
        dst_box.bottom = dst_box.top + resize_h - 1;
        _top_offset = dst_box.top;
    } else {
        dst_box.left = padding_w / 2;
        if (dst_box.left % 2 != 0) {
            dst_box.left -= dst_box.left % 2;
            if (dst_box.left < 0) {
                dst_box.left = 0;
            }
        }
        dst_box.right = dst_box.left + resize_w - 1;
        _left_offset = dst_box.left;
    }

    // 输出LetterBox参数（用于后处理坐标还原）
    if(letterbox != NULL){
        letterbox->scale = scale;
        letterbox->x_pad = _left_offset;
        letterbox->y_pad = _top_offset;
    }

    // 分配目标图像缓冲区
    if (dst_image->virt_addr == NULL && dst_image->fd <= 0) {
        int dst_size = get_image_size(dst_image);
        dst_image->virt_addr = (uint8_t *)malloc(dst_size);
        if (dst_image->virt_addr == NULL) {
            printf("malloc size %d error\n", dst_size);
            return -1;
        }
    }

    ret = convert_image(src_image, dst_image, &src_box, &dst_box, color);
    return ret;
}

// ============================================================================
// getLetter - 仅计算LetterBox参数（不执行图像转换）
// 用于在不需要实际转换时快速获取缩放参数
// ============================================================================
void getLetter(int srcW, int srcH, int dstW, int dstH, letterbox_t* letterbox)
{
    int src_w = srcW;
    int src_h = srcH;
    int dst_w = dstW;
    int dst_h = dstH;
    int resize_w = dst_w;
    int resize_h = dst_h;
    int allow_slight_change = 1;

    int padding_w = 0;
    int padding_h = 0;

    int _left_offset = 0;
    int _top_offset = 0;
    float scale = 1.0;

    image_rect_t src_box;
    src_box.left = 0;
    src_box.top = 0;
    src_box.right = src_w - 1;
    src_box.bottom = src_h - 1;

    image_rect_t dst_box;
    dst_box.left = 0;
    dst_box.top = 0;
    dst_box.right = dst_w - 1;
    dst_box.bottom = dst_h - 1;

    // 计算缩放比例
    float _scale_w = (float)dst_w / src_w;
    float _scale_h = (float)dst_h / src_h;
    if(_scale_w < _scale_h) {
        scale = _scale_w;
        resize_h = (int) src_h*scale;
    } else {
        scale = _scale_h;
        resize_w = (int) src_w*scale;
    }

    // 尺寸对齐微调
    if (allow_slight_change == 1 && (resize_w % 4 != 0)) {
        resize_w -= resize_w % 4;
    }
    if (allow_slight_change == 1 && (resize_h % 2 != 0)) {
        resize_h -= resize_h % 2;
    }

    // 计算填充量
    padding_h = dst_h - resize_h;
    padding_w = dst_w - resize_w;

    // 居中放置
    if (_scale_w < _scale_h) {
        dst_box.top = padding_h / 2;
        if (dst_box.top % 2 != 0) {
            dst_box.top -= dst_box.top % 2;
            if (dst_box.top < 0) {
                dst_box.top = 0;
            }
        }
        dst_box.bottom = dst_box.top + resize_h - 1;
        _top_offset = dst_box.top;
    } else {
        dst_box.left = padding_w / 2;
        if (dst_box.left % 2 != 0) {
            dst_box.left -= dst_box.left % 2;
            if (dst_box.left < 0) {
                dst_box.left = 0;
            }
        }
        dst_box.right = dst_box.left + resize_w - 1;
        _left_offset = dst_box.left;
    }
    
    // 输出LetterBox参数
    letterbox->scale = scale;
    letterbox->x_pad = _left_offset;
    letterbox->y_pad = _top_offset;
}
