// ============================================================================
// camera_preview_decoder.cpp —— 摄像头预览解码/抓拍/检测实现
// ============================================================================
// 平台约定：
//   - 摄像头以 v4l2 输入 MJPEG 流优先走 mjpeg_rkmpp 硬解（输出 NV12
//     DMA-BUF，RGA 零拷贝转 RGBA），任何一环不可用都自动回退 libjpeg
//     软解 + swscale 转 RGBA（CPU 路径，分辨率高时明显吃 CPU）。
//   - 双缓冲 DmaFrameBuffer 在栈上构造，离开 decodeLoop 时析构自动
//     close(fd)/munmap，无需手工释放；RGA/swscale 均同步返回，
//     缓冲切换(back_buffer=1-back_buffer)保证下一帧不会覆盖正在
//     被 EGL 显示的帧（显示侧拿到的是 dup 出的独立 fd）。
//   - 缓存一致性：RGA/NPU 写完 CPU 要读之前必须 dma_sync_device_to_cpu，
//     CPU 写完 GPU 要读之前必须 dma_sync_cpu_to_device，否则在
//     uncached/weakly-cached DMA 内存上会读到脏数据。
// ============================================================================
#include "camera_preview_decoder.h"

#include "DmaFrameBuffer.h"
#include "dma_alloc.h"
#include "../rga/rga_converter.h"
#include "image_utils.h"
#include "image_drawing.h"

#include <QFile>
#include <QDebug>
#include <QMetaType>
#include <chrono>
#include <fstream>

#include <drm_fourcc.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace {

// 像素格式协商回调：与主流水线 GetHWFormat 同一思路——在解码器打开时
// 强制选择 DRM_PRIME（DMA-BUF 输出），拿不到就返回 NONE 让其回退。
// 仅在 hardware_decode==true 时被挂上 codec_context->get_format。
enum AVPixelFormat getDmaBufFormat(AVCodecContext*, const enum AVPixelFormat* formats)
{
    for (const enum AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_DRM_PRIME) return *format;
    }
    return AV_PIX_FMT_NONE;
}

QString ffmpegError(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

} // namespace

CameraPreviewDecoder::CameraPreviewDecoder(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<RenderFrame>("RenderFrame");
    
    // 初始化检测结果队列 (容量12,与video_decoder一致)
    detectResultQueue_ = std::make_shared<PriorityQueue<object_detect_result_list>>(12);
    
    // 初始化640x640的BGR888 DMA缓冲池 (8个缓冲,与video_decoder一致)
    dmaBufferPool_ = std::make_shared<DmaBufferPool>(8, 640, 640, RK_FORMAT_BGR_888, 16);
}

CameraPreviewDecoder::~CameraPreviewDecoder()
{
    stop();
}

void CameraPreviewDecoder::setDetectorConfigs(const std::vector<PreviewDetectorConfig>& configs)
{
    std::lock_guard<std::mutex> lock(detection_mutex_);
    detector_configs_ = configs;
}

// ============================================================================
// start：重建全部检测器后拉起解码线程
// ============================================================================
// 时序要点（代码审查视角）：
//   1. 先 stop()：join 旧 worker_ 线程并 delete 旧 PpeTask，保证下方重建时
//      没有在途推理线程引用被销毁的对象（与 FFmpegVideoDecoder 的
//      "只停不删" 不同，这里任务生命周期与 start/stop 严格配对）。
//   2. detector_labels_ 预读：Windows 换行 \r 被剥离，保证与
//      YoloBaseDetector::loadClassNames 行为一致，cls_id→label 不偏移。
//   3. Face 槽与 Equipment 槽互斥：同一 detector_index 要么持有
//      ScrfdFaceDetector（同步调用），要么持有 PpeTask（异步队列），
//      faceDetectors_ 与 ppeTasks_ 通过"配置 type"分流而非数量对齐。
//   4. 全部容器更新在 detection_mutex_ 内完成，随后才 running_=true
//      并启动线程，避免 worker 首帧读到半初始化配置。
// ============================================================================
void CameraPreviewDecoder::start(const QString& device)
{
    stop();
    device_ = device;
    
    {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        pending_capture_path_.clear();
        latest_jpeg_.clear();
    }
    
    frame_count_ = 0;
    
    {
        std::lock_guard<std::mutex> lock(detection_mutex_);
        detected_boxes_.clear();
        detector_boxes_.assign(detector_configs_.size(), {});
        detector_labels_.assign(detector_configs_.size(), {});
        for (size_t i = 0; i < detector_configs_.size(); ++i) {
            std::ifstream labels(detector_configs_[i].label_path);
            std::string line;
            while (std::getline(labels, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                detector_labels_[i].push_back(line);
            }
        }
    }
    
    // Equipment uses asynchronous YOLO tasks; Face uses the dedicated SCRFD path.
    {
        std::lock_guard<std::mutex> lock(detection_mutex_);
        for (auto* task : ppeTasks_) {
            task->stop();
            delete task;
        }
        ppeTasks_.clear();
        faceDetectors_.clear();
        faceDetectors_.resize(detector_configs_.size());
        
        for (size_t detector_index = 0; detector_index < detector_configs_.size(); ++detector_index) {
            const auto& config = detector_configs_[detector_index];
            if (config.type == PreviewDetectorConfig::Type::Face) {
                auto detector = std::make_unique<ScrfdFaceDetector>();
                if (detector->init(config.model_path, config.npu_core_mask)) {
                    faceDetectors_[detector_index] = std::move(detector);
                    qDebug() << "Created SCRFD face detector: model="
                             << QString::fromStdString(config.model_path);
                } else {
                    qWarning() << "Failed to initialize SCRFD face detector: model="
                               << QString::fromStdString(config.model_path);
                }
                continue;
            }
            TaskConfig taskConfig;
            taskConfig.core_mask = config.npu_core_mask;
            taskConfig.modelPath = config.model_path;
            taskConfig.labelPath = config.label_path;
            taskConfig.result_id = static_cast<int>(detector_index);
            
            PpeTask* task = new PpeTask(taskConfig);
            ppeTasks_.push_back(task);
            
            qDebug() << "Created PpeTask: model=" << QString::fromStdString(config.model_path)
                     << "core=" << config.npu_core_mask;
        }
        
        // 启动所有推理任务
        for (auto* task : ppeTasks_) {
            task->start();
        }
    }
    
    running_ = true;
    worker_ = std::thread(&CameraPreviewDecoder::decodeLoop, this);
}

void CameraPreviewDecoder::stop()
{
    running_ = false;
    
    if (worker_.joinable()) {
        worker_.join();
    }
    
    // 停止所有推理任务
    std::lock_guard<std::mutex> lock(detection_mutex_);
    for (auto* task : ppeTasks_) {
        task->stop();
        delete task;
    }
    ppeTasks_.clear();
    faceDetectors_.clear();
}

void CameraPreviewDecoder::capture(const QString& path)
{
    if (path.isEmpty()) return;
    std::lock_guard<std::mutex> lock(capture_mutex_);
    pending_capture_path_ = path;
}

int CameraPreviewDecoder::interruptCallback(void* opaque)
{
    auto* decoder = static_cast<CameraPreviewDecoder*>(opaque);
    return decoder && !decoder->running_.load() ? 1 : 0;
}

void CameraPreviewDecoder::decodeLoop()
{
    static std::once_flag avdevice_once;
    std::call_once(avdevice_once, []() { avdevice_register_all(); });
    
    AVFormatContext* format_context = avformat_alloc_context();
    if (!format_context) {
        emit error(QStringLiteral("无法分配 FFmpeg 输入上下文"));
        emit finished();
        return;
    }
    format_context->interrupt_callback.callback = &CameraPreviewDecoder::interruptCallback;
    format_context->interrupt_callback.opaque = this;

    AVDictionary* options = nullptr;
    av_dict_set(&options, "video_size", "1920x1080", 0);
    av_dict_set(&options, "input_format", "mjpeg", 0);
    av_dict_set(&options, "framerate", "30", 0);
    const AVInputFormat* v4l2 = av_find_input_format("v4l2");
    const QByteArray device_bytes = device_.toLocal8Bit();
    int ret = avformat_open_input(&format_context, device_bytes.constData(), v4l2, &options);
    av_dict_free(&options);
    if (ret < 0) {
        emit error(QStringLiteral("无法打开摄像头 %1: %2").arg(device_, ffmpegError(ret)));
        avformat_close_input(&format_context);
        emit finished();
        return;
    }

    ret = avformat_find_stream_info(format_context, nullptr);
    if (ret < 0) {
        emit error(QStringLiteral("无法读取摄像头视频流: %1").arg(ffmpegError(ret)));
        avformat_close_input(&format_context);
        emit finished();
        return;
    }

    const int video_index = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO,
                                                -1, -1, nullptr, 0);
    if (video_index < 0) {
        emit error(QStringLiteral("摄像头没有可用的视频流"));
        avformat_close_input(&format_context);
        emit finished();
        return;
    }

    const AVCodec* codec = avcodec_find_decoder_by_name("mjpeg_rkmpp");
    bool hardware_decode = codec != nullptr;
    AVBufferRef* hardware_context = nullptr;
    if (hardware_decode) {
        const AVHWDeviceType hardware_type = av_hwdevice_find_type_by_name("rkmpp");
        if (hardware_type == AV_HWDEVICE_TYPE_NONE ||
            av_hwdevice_ctx_create(&hardware_context, hardware_type, nullptr, nullptr, 0) < 0) {
            hardware_decode = false;
            qWarning() << "RKMPP MJPEG decoder unavailable; using software MJPEG decode";
        }
    }
    if (!hardware_decode) codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
        emit error(QStringLiteral("FFmpeg 中没有可用的 MJPEG 解码器"));
        av_buffer_unref(&hardware_context);
        avformat_close_input(&format_context);
        emit finished();
        return;
    }

    AVCodecContext* codec_context = avcodec_alloc_context3(codec);
    if (!codec_context) {
        emit error(QStringLiteral("无法创建 MJPEG 解码器上下文"));
        av_buffer_unref(&hardware_context);
        avformat_close_input(&format_context);
        emit finished();
        return;
    }
    ret = avcodec_parameters_to_context(codec_context,
                                        format_context->streams[video_index]->codecpar);
    if (ret < 0) {
        emit error(QStringLiteral("无法配置 MJPEG 解码器: %1").arg(ffmpegError(ret)));
        av_buffer_unref(&hardware_context);
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);
        emit finished();
        return;
    }
    if (hardware_decode) codec_context->get_format = &getDmaBufFormat;

    if (hardware_decode) {
        codec_context->hw_device_ctx = av_buffer_ref(hardware_context);
    }
    av_buffer_unref(&hardware_context);

    ret = avcodec_open2(codec_context, codec, nullptr);
    if (ret < 0) {
        emit error(QStringLiteral("无法打开 MJPEG 解码器: %1").arg(ffmpegError(ret)));
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);
        emit finished();
        return;
    }

    const int width = codec_context->width > 0 ? codec_context->width : 1920;
    const int height = codec_context->height > 0 ? codec_context->height : 1080;
    const int output_format = DRM_FORMAT_RGBA8888;
    SwsContext* software_scaler = nullptr;
    DmaFrameBuffer output_buffers[2];
    for (auto& buffer : output_buffers) {
        buffer.setWidth(width);
        buffer.setHeight(height);
        buffer.setFormat(output_format);
        if (buffer.alloc() < 0) {
            emit error(QStringLiteral("无法分配摄像头 EGL DMA 缓冲区"));
            avcodec_free_context(&codec_context);
            avformat_close_input(&format_context);
            emit finished();
            return;
        }
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!packet || !frame) {
        emit error(QStringLiteral("无法分配摄像头解码帧"));
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);
        emit finished();
        return;
    }

    int back_buffer = 0;
    bool logged_software_frame = false;
    
    while (running_) {
        ret = av_read_frame(format_context, packet);
        // 审查点：v4l2 设备无数据时 av_read_frame 返回 EAGAIN，这里直接
        // continue 属"忙等"自旋——依赖 v4l2 默认阻塞读才会睡眠；若设备被
        // 置为非阻塞模式，此循环会空转吃满一个核。停止响应依赖
        // interruptCallback 检查 running_（阻塞读被打断）；EAGAIN 路径下
        // 每轮也能靠 while(running_) 头条件退出，两种模式均不会死锁。
        if (ret == AVERROR(EAGAIN)) continue;
        if (ret < 0) break;   // EOF/被中断/IO错误：跳出走统一清理
        if (packet->stream_index != video_index) {
            av_packet_unref(packet);
            continue;
        }

        // 保存原始JPEG (用于拍照)
        // 设计要点（Why）：摄像头输出的 MJPEG packet 本身就是一张完整
        // JPEG 图片，直接缓存原始码流后，抓拍只需按原样写盘——
        //   1) 零重编码：避免"解码→RGBA→turbojpeg 再编码"的质量损失和 CPU 开销；
        //   2) 分辨率即传感器分辨率（1920x1080），比缩放后的模型输入图清晰；
        //   3) 0xFFD8 是 JPEG SOI 魔数，用于甄别码流确为 JPEG（软解路径下
        //      packet 可能携带别的打包形式）；非 JPEG 帧不更新缓存，
        //      此时 capture() 会因 latest_jpeg_ 为空而静默放弃本次抓拍。
        // 代价：落盘的抓拍图不含检测框叠加（原始流无标注）。
        if (packet->size > 2 && packet->data[0] == 0xFF && packet->data[1] == 0xD8) {
            std::lock_guard<std::mutex> lock(capture_mutex_);
            latest_jpeg_ = QByteArray(reinterpret_cast<const char*>(packet->data), packet->size);
        }

        // send 失败（含解码器满 EAGAIN）与主流水线同一取舍：packet 已 unref，
        // 该帧直接丢弃，宁丢不堵；成功/失败均无泄漏（unref 在分支外统一执行）
        ret = avcodec_send_packet(codec_context, packet);
        av_packet_unref(packet);
        if (ret < 0) continue;

        while (running_ && avcodec_receive_frame(codec_context, frame) == 0) {
            DmaFrameBuffer& destination = output_buffers[back_buffer];
            bool converted = false;
            
            // ===== 解码: NV12 → RGBA =====
            // 硬解路径：从 DRM PRIME 描述符反推真实的内存几何参数（Why 见下）
            if (hardware_decode && frame->format == AV_PIX_FMT_DRM_PRIME && frame->data[0]) {
                auto* descriptor = reinterpret_cast<AVDRMFrameDescriptor*>(frame->data[0]);
                // 结构有效性校验：至少 1 个对象、1 个层、1 个平面（NV12 为
                // 单对象双层内平面布局：plane0=Y, plane1=UV 交错）
                if (descriptor && descriptor->nb_objects >= 1 && descriptor->nb_layers >= 1 &&
                    descriptor->layers[0].nb_planes >= 1) {
                    const AVDRMLayerDescriptor& layer = descriptor->layers[0];
                    const AVDRMPlaneDescriptor& y_plane = layer.planes[0];
                    const int object_index = y_plane.object_index;
                    if (object_index >= 0 && object_index < descriptor->nb_objects) {
                        const AVDRMObjectDescriptor& object = descriptor->objects[object_index];
                        // fd 所有权：object.fd 归 frame 所有（av_frame_unref 时
                        // 由 FFmpeg/MPP 关闭），本函数只借用、同步调用 RGA，绝不 close/dup
                        const int source_fd = object.fd;
                        const int source_stride = static_cast<int>(y_plane.pitch);
                        // 【vstride 反推——本文件最关键的健壮性设计】
                        // NV12 的 Y 平面与 UV 平面位于同一 DMA-BUF 内：
                        //   UV 平面起始偏移 = vstride × pitch（垂直方向按硬件要求
                        //   对齐，通常 16 行；如 1080 高会补齐到 1088）
                        // 因此用两个平面的 offset 差除以行字节数即可反推出真实
                        // vstride： vstride = (offset_UV - offset_Y) / pitch
                        // 若像主流水线那样直接用 frame->height 当 vstride，一旦
                        // 存在垂直 padding，RGA 会把 UV 平面读错位 → 色彩错位/
                        // 横条纹。退化保护：单平面、两平面不同 object、offset 不
                        // 递增（异常描述符）时回退 frame->height，避免除零/负值。
                        int source_vstride = frame->height;
                        if (layer.nb_planes > 1 &&
                            layer.planes[1].object_index == object_index &&
                            layer.planes[1].offset > y_plane.offset && source_stride > 0) {
                            source_vstride = static_cast<int>(
                                (layer.planes[1].offset - y_plane.offset) / source_stride);
                        }
                        // RGA 格式映射：NV12=YCbCr_420_SP(UV 序)，NV21=YCrCb_420_SP(VU 序)；
                        // 其余 fourcc 置 -1 走下方校验失败跳过，防按错误布局解释内存
                        int source_format = RK_FORMAT_YCbCr_420_SP;
                        if (layer.format == DRM_FORMAT_NV21) {
                            source_format = RK_FORMAT_YCrCb_420_SP;
                        } else if (layer.format != DRM_FORMAT_NV12) {
                            qWarning() << "Unsupported camera DRM PRIME format:"
                                       << QString::number(layer.format, 16);
                            source_format = -1;
                        }
                        if (source_fd >= 0 && source_stride > 0 && source_vstride > 0 &&
                            object.size > 0 && source_format >= 0) {
                        converted = RgaConverter::convertNV12ToRGBAbyRGA(
                            source_fd, destination.fd(), frame->width, frame->height,
                            source_stride, source_vstride, source_format);
                        }
                    }
                }
            } else if (!hardware_decode && frame->data[0]) {
                // 软解回退路径：CPU 内存帧 → sws_scale 转 RGBA 到 DMA 缓冲。
                // 懒创建：首帧才建立转换上下文（此时才拿到真实 frame 格式）。
                // 审查点：src 尺寸/格式固定取首帧，运行中摄像头改分辨率会
                // 读到旧尺寸（MJPEG 定拍场景实际不会发生，属可接受约束）；
                // dst 固定为 output_buffers 的 width×height。
                // dst linesize 用 destination.stride()（可能 > width*4），
                // 正确按行跨距写入，与后续 draw/EGL 的 stride 语义一致。
                if (!software_scaler) {
                    software_scaler = sws_getContext(
                        frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                        width, height, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR,
                        nullptr, nullptr, nullptr);
                }
                if (software_scaler) {
                    // CPU 即将写该 DMA 缓冲：先从设备侧收回所有权，避免
                    // 缓存行不一致（sws 写完后下方还有统一的 sync 流程）
                    dma_sync_device_to_cpu(destination.fd());
                    uint8_t* destination_data[4] = {destination.ptr(), nullptr, nullptr, nullptr};
                    int destination_linesize[4] = {destination.stride(), 0, 0, 0};
                    converted = sws_scale(software_scaler, frame->data, frame->linesize,
                                          0, frame->height, destination_data,
                                          destination_linesize) > 0;
                    if (converted && !logged_software_frame) {
                        qDebug() << "Software camera frame:" << frame->width << "x" << frame->height
                                 << "format" << frame->format;
                        logged_software_frame = true;
                    }
                }
            }

            if (converted) {
                ++frame_count_;
                
                // ===== 每5帧进行一次RKNN推理 =====
                // 降频系数 5（主流水线为 3）：预览链路还叠加 SCRFD，
                // 25fps 输入下检测频率约 5 次/秒，点名场景足够且给 GL
                // 显示留带宽。检测框对静止人脸不敏感，4 帧间隔内沿用上帧
                // 缓存（detector_boxes_ 粘滞），画面框不闪烁。
                if (frame_count_ % 5 == 0) {
                    // RGA 刚写完 destination，CPU（OpenCV/SCRFD 直接读指针）
                    // 读取前必须收回缓存所有权
                    dma_sync_device_to_cpu(destination.fd());
                    // SCRFD is synchronous here, while its latest result is
                    // kept in detector_boxes_ for the four intervening frames.
                    // 注意：detectRgba 在解码线程内联执行（预处理+rknn_run+
                    // 解码候选+NMS 全部同步），单次耗时直接串进预览帧间隔，
                    // 这是"点名链路无需独立任务线程"的取舍（见 README 注意事项）。
                    for (size_t detector_index = 0;
                         detector_index < faceDetectors_.size(); ++detector_index) {
                        if (!faceDetectors_[detector_index]) continue;
                        std::vector<ScrfdFaceBox> faces;
                        if (!faceDetectors_[detector_index]->detectRgba(
                                destination.ptr(), frame->width, frame->height,
                                destination.stride(), faces)) {
                            continue;
                        }
                        std::vector<PreviewDetectionBox> boxes;
                        boxes.reserve(faces.size());
                        for (const ScrfdFaceBox& face : faces) {
                            PreviewDetectionBox box;
                            box.rect = face.rect;
                            box.label = "face";
                            boxes.push_back(std::move(box));
                        }
                        std::lock_guard<std::mutex> lock(detection_mutex_);
                        if (detector_index < detector_boxes_.size()) {
                            detector_boxes_[detector_index] = std::move(boxes);
                            detected_boxes_.clear();
                            for (const auto& cached : detector_boxes_)
                                detected_boxes_.insert(detected_boxes_.end(), cached.begin(), cached.end());
                        }
                    }

                    auto rgabuffer = dmaBufferPool_->tryAcquireSharedPtr();
                    if (rgabuffer && !ppeTasks_.empty()) {
                        // RGA缩放: 1920x1080 RGBA → 640x640 BGR
                        const int resize_status = RgaConverter::rgba_to_rgb_resize(
                            destination.fd(), destination.width(), destination.height(), destination.stride(),
                            rgabuffer->fd, rgabuffer->width, rgabuffer->height, rgabuffer->width_stride,
                            true  // BGR格式
                        );
                        
                        if (resize_status == 0) {
                            // 封装推理任务数据
                            std::shared_ptr<image_buffer_t> image = std::make_shared<image_buffer_t>();
                            image->format = image_format_t::IMAGE_FORMAT_RGB888;
                            image->virt_addr = (uint8_t*)(rgabuffer->va);
                            image->width = 640;
                            image->height = 640;
                            image->width_stride = 640;
                            image->srcWidth = destination.width();
                            image->srcHeight = destination.height();
                            image->sp_dmaBuffer = rgabuffer;
                            
                            auto timestamp = std::chrono::system_clock::now();
                            image->time = timestamp.time_since_epoch().count();
                            
                            std::shared_ptr<TaskData> taskData = std::make_shared<TaskData>(
                                image->time,
                                image,
                                this->detectResultQueue_
                            );
                            
                            // 提交到所有配置的推理任务
                            {
                                std::lock_guard<std::mutex> lock(detection_mutex_);
                                for (auto* task : ppeTasks_) {
                                    task->put(taskData);
                                }
                            }
                        }
                    }
                }
                
                // ===== 获取推理结果并更新每个检测器的缓存 =====
                object_detect_result_list od_results;
                if (detectResultQueue_->tryPop(od_results)) {
                    // YOLO 后处理已经按 image->srcWidth/srcHeight 还原到原始画面坐标。
                    std::vector<PreviewDetectionBox> boxes;
                    
                    for (int i = 0; i < od_results.count; i++) {
                        object_detect_result* det = &(od_results.results[i]);
                        cv::Rect box(
                            det->box.left,
                            det->box.top,
                            det->box.right - det->box.left,
                            det->box.bottom - det->box.top
                        );
                        box &= cv::Rect(0, 0, destination.width(), destination.height());
                        if (box.width > 0 && box.height > 0) {
                            PreviewDetectionBox preview_box;
                            preview_box.rect = box;
                            if (od_results.id >= 0 &&
                                static_cast<size_t>(od_results.id) < detector_labels_.size() &&
                                det->cls_id >= 0 &&
                                static_cast<size_t>(det->cls_id) < detector_labels_[od_results.id].size()) {
                                preview_box.label = detector_labels_[od_results.id][det->cls_id];
                            }
                            boxes.push_back(std::move(preview_box));
                        }
                    }
                    
                    // 不同模型的结果异步到达，分别缓存后再合并，避免空结果清掉其他模型的框。
                    {
                        std::lock_guard<std::mutex> lock(detection_mutex_);
                        if (od_results.id >= 0 &&
                            static_cast<size_t>(od_results.id) < detector_boxes_.size()) {
                            detector_boxes_[od_results.id] = std::move(boxes);
                            detected_boxes_.clear();
                            for (const auto& detector_boxes : detector_boxes_) {
                                detected_boxes_.insert(detected_boxes_.end(),
                                                       detector_boxes.begin(), detector_boxes.end());
                            }
                        }
                    }
                }
                
                // RGA 写入后先同步到 CPU；GL 使用独立快照，避免复用 DMA buffer 时并发读写。
                dma_sync_device_to_cpu(destination.fd());

                // 在CPU内存中绘制检测框（写的是本双缓冲 destination 本身，
                // 因此抓拍缩略图与上屏画面都带框）
                drawDetectionBoxes(destination.ptr(), frame->width, frame->height, destination.stride());
                
                // 刷新缓存：优先 DMA-BUF 显式同步；失败退化为 msync 强制刷
                // 页缓存（老内核/部分驱动不支持方向性 sync 时的兜底），
                // 确保 EGL 端 import 该 fd 后看到的是画完框的最新内容
                if (dma_sync_cpu_to_device(destination.fd()) < 0) {
                    msync(destination.ptr(), destination.size(), MS_SYNC);
                }
                
                // 以 dup(fd) 传给 GL 渲染（与主流水线 ffmpeg_video_decoder 相同的 RenderFrame 契约）
                // 所有权：dup 出的新 fd 归接收方（GL 侧）close；本函数继续
                // 使用原始 destination.fd()，双缓冲回绕覆盖不影响已发出的帧
                //（EGLImage 按 fd 引用计数持有物理页）。
                RenderFrame rendered;
                rendered.fd = ::dup(destination.fd());
                rendered.width = frame->width;
                rendered.height = frame->height;
                rendered.stride = destination.stride();
                if (rendered.fd >= 0) {
                    emit frameReady(rendered);
                }
                
                // 处理拍照请求
                handleDecodedFrame(destination.ptr(), frame->width, frame->height,
                                   destination.stride(), false);
                
                back_buffer = 1 - back_buffer;
            }
            av_frame_unref(frame);
        }
    }

    // 统一清理出口：与 decodeLoop 入口的分配逐一配对——
    //   packet/frame：*_alloc ↔ *_free（任一分配失败已在分支内成对回收）；
    //   software_scaler：可能为 nullptr，sws_freeContext 容忍 NULL；
    //   codec_context/format_context：所有提前 return 分支同样按"已分配什么
    //   释放什么"成对回收（hardware_context 在 codec_context 建立后立刻
    //   unref，codec 经 av_buffer_ref 自持引用，不依赖本地引用存活）；
    //   output_buffers[2] 为栈对象，作用域结束自动 close fd + munmap。
    av_packet_free(&packet);
    av_frame_free(&frame);
    sws_freeContext(software_scaler);
    avcodec_free_context(&codec_context);
    avformat_close_input(&format_context);
    emit finished();
}

// ============================================================================
// drawDetectionBoxes —— CPU 直绘 RGBA 检测框
// ============================================================================
// 先在锁内值拷贝一份 boxes 快照再解锁绘制（画框耗时不阻塞检测结果更新）；
// 像素寻址 pixels + y*stride + x*4 与 DMA 缓冲的行跨距对齐，绘制顺序
// 先矩形后标签底框+文字，坐标全部 clamp 到画面内，防越界写坏相邻行。
// ============================================================================
void CameraPreviewDecoder::drawDetectionBoxes(uint8_t* pixels, int width, int height, int stride)
{
    if (!pixels) return;
    constexpr int thickness = 4;
    
    std::vector<PreviewDetectionBox> boxes;
    {
        std::lock_guard<std::mutex> lock(detection_mutex_);
        boxes = detected_boxes_;
    }
    
    image_buffer_t display_image{};
    display_image.format = image_format_t::IMAGE_FORMAT_RGBA8888;
    display_image.virt_addr = pixels;
    display_image.width = width;
    display_image.height = height;
    display_image.width_stride = stride;

    // 绘制检测框和标签 (绿色边框, RGBA格式)
    for (const PreviewDetectionBox& detection : boxes) {
        const cv::Rect& box = detection.rect;
        const int x0 = std::max(0, box.x);
        const int y0 = std::max(0, box.y);
        const int x1 = std::min(width - 1, box.x + box.width - 1);
        const int y1 = std::min(height - 1, box.y + box.height - 1);
        
        for (int t = 0; t < thickness; ++t) {
            const int top = std::min(height - 1, y0 + t);
            const int bottom = std::max(0, y1 - t);
            const int left = std::min(width - 1, x0 + t);
            const int right = std::max(0, x1 - t);
            
            // 绘制水平线
            for (int x = left; x <= right; ++x) {
                uint8_t* p1 = pixels + top * stride + x * 4;
                uint8_t* p2 = pixels + bottom * stride + x * 4;
                // RGBA: R=0, G=255, B=0, A=255 (绿色)
                p1[0] = p2[0] = 0;   
                p1[1] = p2[1] = 255; 
                p1[2] = p2[2] = 0;   
                p1[3] = p2[3] = 255;
            }
            
            // 绘制垂直线
            for (int y = top; y <= bottom; ++y) {
                uint8_t* p1 = pixels + y * stride + left * 4;
                uint8_t* p2 = pixels + y * stride + right * 4;
                p1[0] = p2[0] = 0;   
                p1[1] = p2[1] = 255; 
                p1[2] = p2[2] = 0;   
                p1[3] = p2[3] = 255;
            }
        }

        if (!detection.label.empty()) {
            constexpr int label_font_size = 36;
            constexpr int label_padding = 6;
            const int label_height = label_font_size * 2;
            const int label_width = std::max(
                label_font_size,
                static_cast<int>(detection.label.size()) * label_font_size + label_padding * 2);
            const int label_x = std::min(x0, std::max(0, width - label_width));
            const int label_y = y0 >= label_height ? y0 - label_height : y0;
            draw_rectangle(&display_image, label_x, label_y, label_width, label_height,
                           COLOR_BLACK, -1);
            draw_text(&display_image, detection.label.c_str(), label_x + label_padding,
                      label_y, COLOR_GREEN, label_font_size);
        }
    }
}

// ============================================================================
// handleDecodedFrame —— 消费待处理拍照请求（名叫"处理解码帧"，实为抓拍落盘）
// ============================================================================
// 两段式加锁的意图：
//   第一段取走 path+jpeg 快照即放锁，文件 IO（可能几十 ms）不在锁内执行，
//   避免阻塞解码循环里每个 packet 都要抢 capture_mutex_ 刷新 latest_jpeg_
//   的路径；第二段仅在"请求未被新值替换"时清空（若期间用户又按了快门且
//   path 已更新，则保留新 path 让下一帧消费——幂等防丢拍）。
// 缩略图零成本来源：直接包装调用方传入的 RGBA DMA 帧（已含画框叠加），
// QImage 构造不拷贝，故必须 copy() 使信号负载脱离双缓冲生命周期
//（下一帧 back_buffer 回绕会覆盖该内存）。
// 参数 bgr888 恒为 false（当前唯一调用点走 RGBA 路径），保留以兼容
// 未来 BGR 直出的抓取源。
// ============================================================================
void CameraPreviewDecoder::handleDecodedFrame(const uint8_t* pixels, int width, int height,
                                              int stride, bool bgr888)
{
    QString path;
    QByteArray jpeg;
    {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (pending_capture_path_.isEmpty()) return;
        path = pending_capture_path_;
        jpeg = latest_jpeg_;
    }
    
    if (jpeg.isEmpty()) return;

    // 清除拍照请求标志
    {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (pending_capture_path_ == path) {
            pending_capture_path_.clear();
        }
    }

    // 写入JPEG文件
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        emit error(QStringLiteral("照片保存失败: 无法打开文件 %1").arg(path));
        return;
    }
    
    if (output.write(jpeg) != jpeg.size()) {
        output.close();
        emit error(QStringLiteral("照片保存失败: 写入失败 %1").arg(path));
        return;
    }
    output.close();

    // 创建缩略图 (从已解码的RGBA DMA-BUF创建,无需重新解码JPEG)
    const QImage thumbnail(pixels, width, height, stride,
                           bgr888 ? QImage::Format_BGR888 : QImage::Format_RGBA8888);
    
    // 发送拍照完成信号 (copy()确保数据独立)
    emit photoCaptured(path, thumbnail.copy());
    
    qDebug() << "Camera photo captured:" << path << "size:" << jpeg.size() << "bytes";
}
