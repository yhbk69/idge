/**
 * @file gb28181_media.cpp
 * @brief 国标推流的取流 + 转码工作线程（FFmpeg，尽量走 RK3588 硬解硬编）
 *
 * 一条通道一个线程，流程固定：
 *   开流 → 找视频流 → 判直通还是重编 → 逐帧产出 Annex-B H.264 + 90kHz PTS
 * 开流挂 interrupt_callback 指向 running_，所以 stop()/requestStop() 能把阻塞在
 * av_read_frame 里的线程叫醒；没有它，关流会卡到网络超时才退。
 *
 * 两条编码路径，配置项的含义随之不同：
 *   - 源已是 H.264：只接 h264_mp4toannexb 透传。此时 width/height/fps/bitrate
 *     **全部不生效**，出来的分辨率码率就是源的，配置里改它们没用。
 *   - 其它（camera 的 MJPEG、RTSP 的 H.265 等）：按 codec_id 找 h264_rkmpp /
 *     hevc_rkmpp / mjpeg_rkmpp 硬解并挂 rkmpp hwdevice；找不到同名解码器或建
 *     hwdevice 失败，就在 openDecoder 里换回普通软解；跑起来之后 DRM_PRIME 帧
 *     同步到内存失败，还会中途再换成纯软解继续推（openSoftwareDecoder 分支）。
 *     之后 swscale 到目标尺寸 → h264_rkmpp 编码，硬编打不开退 libx264
 *     (veryfast + zerolatency)。这条路上 width/height/bitrate 才真的读。
 *
 * 摄像头源的三个特殊处理：
 *   - 强制 input_format=mjpeg（板子上的 v4l2 设备大多给不了未压缩的 1080p）；
 *   - 按配置的 fps 开失败且不是 30 时，再用 30fps MJPEG 重试一次；
 *   - 开成功后用流的 avg_frame_rate 覆盖 config_.fps，所以配置值只是"期望"。
 * mp4 源读完是否重播由 config_.loop 决定，其它类型不看这个键。
 *
 * 首 5 秒 PTS 按墙钟 sleep_until 逐帧限速（回放/文件源瞬间读满会把平台收流缓冲
 * 打爆），超过 5 秒后按解码速度直发。
 */
#include "gb28181_media.h"

#include <QDebug>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace {
QString ffmpegError(int error) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, buffer, sizeof(buffer));
    return QString::fromLocal8Bit(buffer);
}

QString sourceUrl(const Gb28181StreamConfig &config) {
    if (config.type == QStringLiteral("camera")) {
        if (!config.source.isEmpty()) return config.source;
        return QStringLiteral("/dev/video%1").arg(config.cameraIndex < 0 ? 0 : config.cameraIndex);
    }
    return config.source;
}

int ffmpegInterrupt(void *opaque) {
    const auto *running = static_cast<const std::atomic<bool> *>(opaque);
    return running && !running->load() ? 1 : 0;
}

bool copyAnnexBPacket(const AVPacket *packet, std::vector<uint8_t> *out) {
    if (!packet || !packet->data || packet->size <= 0 || !out) return false;
    const uint8_t *data = packet->data;
    const int size = packet->size;
    for (int i = 0; i + 3 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 &&
            ((data[i + 2] == 1) || (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1))) {
            out->assign(data, data + size);
            return true;
        }
    }
    size_t offset = 0;
    std::vector<uint8_t> converted;
    while (offset + 4 <= static_cast<size_t>(size)) {
        const uint32_t naluSize = (static_cast<uint32_t>(data[offset]) << 24) |
                                  (static_cast<uint32_t>(data[offset + 1]) << 16) |
                                  (static_cast<uint32_t>(data[offset + 2]) << 8) |
                                  static_cast<uint32_t>(data[offset + 3]);
        offset += 4;
        if (naluSize == 0 || offset + naluSize > static_cast<size_t>(size)) return false;
        converted.insert(converted.end(), {0, 0, 0, 1});
        converted.insert(converted.end(), data + offset, data + offset + naluSize);
        offset += naluSize;
    }
    if (offset == static_cast<size_t>(size) && !converted.empty()) {
        *out = std::move(converted);
        return true;
    }
    return false;
}

AVCodecContext *openDecoder(const AVCodecParameters *parameters) {
    const AVCodec *codec = nullptr;
    if (parameters->codec_id == AV_CODEC_ID_H264)
        codec = avcodec_find_decoder_by_name("h264_rkmpp");
    else if (parameters->codec_id == AV_CODEC_ID_HEVC)
        codec = avcodec_find_decoder_by_name("hevc_rkmpp");
    else if (parameters->codec_id == AV_CODEC_ID_MJPEG)
        codec = avcodec_find_decoder_by_name("mjpeg_rkmpp");
    const bool requestedHardware = codec != nullptr;
    if (!codec) codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) return nullptr;

    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (!context) return nullptr;
    if (requestedHardware) {
        AVBufferRef *device = nullptr;
        const AVHWDeviceType type = av_hwdevice_find_type_by_name("rkmpp");
        if (type == AV_HWDEVICE_TYPE_NONE || av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0) < 0) {
            avcodec_free_context(&context);
            context = nullptr;
            codec = avcodec_find_decoder(parameters->codec_id);
            if (!codec) return nullptr;
            context = avcodec_alloc_context3(codec);
            if (!context) return nullptr;
        } else {
            context->hw_device_ctx = device;
        }
    }
    if (avcodec_parameters_to_context(context, parameters) < 0 || avcodec_open2(context, codec, nullptr) < 0) {
        avcodec_free_context(&context);
        return nullptr;
    }
    return context;
}

AVCodecContext *openSoftwareDecoder(const AVCodecParameters *parameters) {
    const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) return nullptr;
    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (!context || avcodec_parameters_to_context(context, parameters) < 0 || avcodec_open2(context, codec, nullptr) < 0) {
        if (context) avcodec_free_context(&context);
        return nullptr;
    }
    return context;
}

AVCodecContext *openEncoder(const Gb28181StreamConfig &config) {
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_rkmpp");
    bool hardware = codec != nullptr;
    if (!codec) {
        codec = avcodec_find_encoder_by_name("libx264");
        if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        hardware = false;
    }
    if (!codec) return nullptr;

    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (!context) return nullptr;
    context->width = config.width;
    context->height = config.height;
    context->time_base = AVRational{1, config.fps > 0 ? config.fps : 25};
    context->framerate = AVRational{config.fps > 0 ? config.fps : 25, 1};
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->bit_rate = config.bitrate > 0 ? config.bitrate : 2000000;
    context->gop_size = config.fps > 0 ? config.fps * 2 : 50;
    context->max_b_frames = 0;
    if (!hardware) {
        av_opt_set(context->priv_data, "preset", "veryfast", 0);
        av_opt_set(context->priv_data, "tune", "zerolatency", 0);
    }
    if (avcodec_open2(context, codec, nullptr) < 0) {
        avcodec_free_context(&context);
        if (hardware) {
            codec = avcodec_find_encoder_by_name("libx264");
            if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
            if (!codec) return nullptr;
            context = avcodec_alloc_context3(codec);
            if (!context) return nullptr;
            context->width = config.width;
            context->height = config.height;
            context->time_base = AVRational{1, config.fps > 0 ? config.fps : 25};
            context->framerate = AVRational{config.fps > 0 ? config.fps : 25, 1};
            context->pix_fmt = AV_PIX_FMT_YUV420P;
            context->bit_rate = config.bitrate > 0 ? config.bitrate : 2000000;
            context->gop_size = config.fps > 0 ? config.fps * 2 : 50;
            context->max_b_frames = 0;
            av_opt_set(context->priv_data, "preset", "veryfast", 0);
            av_opt_set(context->priv_data, "tune", "zerolatency", 0);
            if (avcodec_open2(context, codec, nullptr) < 0) avcodec_free_context(&context);
        }
    }
    return context;
}
}

Gb28181MediaSource::Gb28181MediaSource(const Gb28181StreamConfig &config) : config_(config) {}

Gb28181MediaSource::~Gb28181MediaSource() { stop(); }

bool Gb28181MediaSource::start(FrameCallback frameCallback, ErrorCallback errorCallback) {
    if (running_.exchange(true)) return true;
    frameCallback_ = std::move(frameCallback);
    errorCallback_ = std::move(errorCallback);
    thread_ = std::thread(&Gb28181MediaSource::run, this);
    return true;
}

void Gb28181MediaSource::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void Gb28181MediaSource::reportError(const QString &message) const {
    if (errorCallback_) errorCallback_(message);
    else qWarning() << "GB28181 media:" << message;
}

void Gb28181MediaSource::run() {
    AVFormatContext *format = nullptr;
    AVDictionary *options = nullptr;
    if (config_.type == QStringLiteral("rtsp")) av_dict_set(&options, "rtsp_transport", "tcp", 0);
    if (config_.type == QStringLiteral("camera")) {
        av_dict_set(&options, "video_size", QStringLiteral("%1x%2").arg(config_.width).arg(config_.height).toLocal8Bit().constData(), 0);
        av_dict_set(&options, "input_format", "mjpeg", 0);
        av_dict_set(&options, "framerate", QByteArray::number(config_.fps > 0 ? config_.fps : 25).constData(), 0);
    }
    const QByteArray url = sourceUrl(config_).toLocal8Bit();
    format = avformat_alloc_context();
    if (format) {
        format->interrupt_callback.callback = ffmpegInterrupt;
        format->interrupt_callback.opaque = &running_;
    }
    const AVInputFormat *inputFormat = config_.type == QStringLiteral("camera") ? av_find_input_format("v4l2") : nullptr;
    int result = avformat_open_input(&format, url.constData(), inputFormat, &options);
    av_dict_free(&options);
    if (result < 0 && config_.type == QStringLiteral("camera") && config_.fps != 30) {
        qWarning() << "GB28181 camera open failed at" << config_.fps
                   << "fps; retrying MJPEG at 30 fps:" << ffmpegError(result);
        avformat_close_input(&format);
        format = avformat_alloc_context();
        if (format) {
            format->interrupt_callback.callback = ffmpegInterrupt;
            format->interrupt_callback.opaque = &running_;
            AVDictionary *retryOptions = nullptr;
            av_dict_set(&retryOptions, "video_size", QStringLiteral("%1x%2").arg(config_.width).arg(config_.height).toLocal8Bit().constData(), 0);
            av_dict_set(&retryOptions, "input_format", "mjpeg", 0);
            av_dict_set(&retryOptions, "framerate", "30", 0);
            result = avformat_open_input(&format, url.constData(), inputFormat, &retryOptions);
            av_dict_free(&retryOptions);
            if (result < 0)
                qWarning() << "GB28181 camera MJPEG retry at 30 fps failed:" << ffmpegError(result);
        }
    }
    if (result < 0) {
        reportError(QStringLiteral("cannot open %1: %6 (requested %2x%3 @ %4 fps, input_format=%5)")
                        .arg(QString::fromLocal8Bit(url)).arg(config_.width).arg(config_.height)
                        .arg(config_.fps).arg(config_.type == QStringLiteral("camera") ? QStringLiteral("mjpeg") : QStringLiteral("auto"))
                        .arg(ffmpegError(result)));
        running_ = false;
        return;
    }
    result = avformat_find_stream_info(format, nullptr);
    if (result < 0) {
        reportError(QStringLiteral("cannot read stream info: %1").arg(ffmpegError(result)));
        avformat_close_input(&format);
        running_ = false;
        return;
    }
    const int videoIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0) {
        reportError(QStringLiteral("source has no video stream"));
        avformat_close_input(&format);
        running_ = false;
        return;
    }

    AVStream *stream = format->streams[videoIndex];
    qInfo() << "GB28181 media opened" << sourceUrl(config_) << "format="
            << (format->iformat ? format->iformat->name : "unknown")
            << "codec=" << avcodec_get_name(stream->codecpar->codec_id)
            << "size=" << stream->codecpar->width << "x" << stream->codecpar->height
            << "time_base=" << stream->time_base.num << "/" << stream->time_base.den;
    if (config_.type == QStringLiteral("camera")) {
        const AVRational sourceRate = stream->avg_frame_rate.num > 0 ? stream->avg_frame_rate : stream->r_frame_rate;
        const int detectedFps = sourceRate.num > 0 && sourceRate.den > 0
            ? static_cast<int>(av_q2d(sourceRate) + 0.5) : 0;
        if (detectedFps > 0 && detectedFps != config_.fps) {
            qInfo() << "GB28181 camera frame rate adjusted" << config_.fps << "->" << detectedFps;
            config_.fps = detectedFps;
        }
    }
    const bool passthrough = stream->codecpar->codec_id == AV_CODEC_ID_H264;
    AVBSFContext *bsf = nullptr;
    if (passthrough) {
        const AVBitStreamFilter *filter = av_bsf_get_by_name("h264_mp4toannexb");
        if (filter && av_bsf_alloc(filter, &bsf) == 0) {
            avcodec_parameters_copy(bsf->par_in, stream->codecpar);
            bsf->time_base_in = stream->time_base;
            if (av_bsf_init(bsf) < 0) {
                av_bsf_free(&bsf);
                bsf = nullptr;
            }
        }
    }

    AVCodecContext *decoder = nullptr;
    AVCodecContext *encoder = nullptr;
    SwsContext *scale = nullptr;
    AVFrame *decoded = nullptr;
    AVFrame *converted = nullptr;
    int64_t fallbackFramePts = 0;
    AVPacket *packet = av_packet_alloc();
    AVPacket *encoded = av_packet_alloc();
    bool pacingStarted = false;
    uint64_t firstPts90k = 0;
    uint64_t emittedFrames = 0;
    const auto wallStart = std::chrono::steady_clock::now();
    const auto emitFrame = [this, &pacingStarted, &firstPts90k, &wallStart, &emittedFrames](Gb28181EncodedFrame &frame) {
        if (!pacingStarted) { pacingStarted = true; firstPts90k = frame.pts90k; }
        if (frame.pts90k >= firstPts90k && frame.pts90k - firstPts90k < 450000) {
            const auto target = wallStart + std::chrono::milliseconds(static_cast<int64_t>((frame.pts90k - firstPts90k) / 90));
            std::this_thread::sleep_until(target);
        }
        if (running_ && frameCallback_) {
            ++emittedFrames;
            if (emittedFrames == 1 || (emittedFrames % 100) == 0)
                qInfo() << "GB28181 media frame" << sourceUrl(config_) << "count=" << emittedFrames
                        << "bytes=" << frame.data.size() << "pts90k=" << frame.pts90k
                        << "key=" << frame.keyFrame;
            frameCallback_(frame);
        }
    };
    if (!packet || !encoded) {
        reportError(QStringLiteral("cannot allocate FFmpeg packets"));
        goto cleanup;
    }
    if (!passthrough) {
        decoder = openDecoder(stream->codecpar);
        if (!decoder) {
            reportError(QStringLiteral("cannot open decoder for %1").arg(stream->codecpar->codec_id));
            goto cleanup;
        }
        encoder = openEncoder(config_);
        if (!encoder) {
            reportError(QStringLiteral("cannot open H.264 encoder (rkmpp and software fallback failed)"));
            goto cleanup;
        }
        decoded = av_frame_alloc();
        converted = av_frame_alloc();
        if (!decoded || !converted) {
            reportError(QStringLiteral("cannot allocate conversion frames"));
            goto cleanup;
        }
        converted->format = encoder->pix_fmt;
        converted->width = encoder->width;
        converted->height = encoder->height;
        if (av_frame_get_buffer(converted, 32) < 0) goto cleanup;
    }

    while (running_) {
        result = av_read_frame(format, packet);
        if (result < 0) {
            if (config_.loop && config_.type == QStringLiteral("mp4")) {
                if (av_seek_frame(format, videoIndex, 0, AVSEEK_FLAG_BACKWARD) < 0) break;
                if (decoder) avcodec_flush_buffers(decoder);
                continue;
            }
            break;
        }
        if (packet->stream_index != videoIndex) {
            av_packet_unref(packet);
            continue;
        }
        const uint64_t pts = packet->pts == AV_NOPTS_VALUE
            ? 0 : static_cast<uint64_t>(av_rescale_q(packet->pts, stream->time_base, AVRational{1, 90000}));
        if (passthrough) {
            if (bsf) {
                av_bsf_send_packet(bsf, packet);
                while (av_bsf_receive_packet(bsf, encoded) == 0) {
                    Gb28181EncodedFrame frame;
                    if (copyAnnexBPacket(encoded, &frame.data)) {
                        frame.pts90k = encoded->pts == AV_NOPTS_VALUE ? pts : static_cast<uint64_t>(av_rescale_q(encoded->pts, stream->time_base, AVRational{1, 90000}));
                        frame.keyFrame = (encoded->flags & AV_PKT_FLAG_KEY) != 0;
                        emitFrame(frame);
                    }
                    av_packet_unref(encoded);
                }
            } else {
                Gb28181EncodedFrame frame;
                if (copyAnnexBPacket(packet, &frame.data)) {
                    frame.pts90k = pts;
                    frame.keyFrame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
                    emitFrame(frame);
                }
            }
        } else {
            if (avcodec_send_packet(decoder, packet) >= 0) {
                while (avcodec_receive_frame(decoder, decoded) == 0 && running_) {
                    AVFrame *frameForScale = decoded;
                    AVFrame *transferred = nullptr;
                    if (decoded->format == AV_PIX_FMT_DRM_PRIME) {
                        transferred = av_frame_alloc();
                        if (!transferred || av_hwframe_transfer_data(transferred, decoded, 0) < 0) {
                            if (transferred) av_frame_free(&transferred);
                            AVCodecContext *softwareDecoder = openSoftwareDecoder(stream->codecpar);
                            if (softwareDecoder) {
                                avcodec_free_context(&decoder);
                                decoder = softwareDecoder;
                                av_frame_unref(decoded);
                                if (scale) { sws_freeContext(scale); scale = nullptr; }
                                continue;
                            }
                            reportError(QStringLiteral("hardware decoder frame transfer failed and software fallback failed"));
                            break;
                        }
                        frameForScale = transferred;
                    }
                    if (!scale) {
                        scale = sws_getContext(frameForScale->width, frameForScale->height, static_cast<AVPixelFormat>(frameForScale->format),
                                            encoder->width, encoder->height, encoder->pix_fmt,
                                            SWS_BILINEAR, nullptr, nullptr, nullptr);
                    }
                    if (!scale) {
                        if (transferred) av_frame_free(&transferred);
                        reportError(QStringLiteral("cannot create pixel format converter"));
                        break;
                    }
                    if (av_frame_make_writable(converted) < 0) {
                        if (transferred) av_frame_free(&transferred);
                        break;
                    }
                    sws_scale(scale, frameForScale->data, frameForScale->linesize, 0, frameForScale->height,
                              converted->data, converted->linesize);
                    converted->pts = decoded->pts == AV_NOPTS_VALUE
                        ? fallbackFramePts++
                        : av_rescale_q(decoded->pts, stream->time_base, encoder->time_base);
                    if (avcodec_send_frame(encoder, converted) >= 0) {
                        while (avcodec_receive_packet(encoder, encoded) == 0) {
                            Gb28181EncodedFrame frame;
                            if (copyAnnexBPacket(encoded, &frame.data)) {
                                frame.pts90k = static_cast<uint64_t>(av_rescale_q(encoded->pts == AV_NOPTS_VALUE ? converted->pts : encoded->pts,
                                                                                   encoder->time_base, AVRational{1, 90000}));
                                frame.keyFrame = (encoded->flags & AV_PKT_FLAG_KEY) != 0;
                                emitFrame(frame);
                            }
                            av_packet_unref(encoded);
                        }
                    }
                    if (transferred) av_frame_free(&transferred);
                    av_frame_unref(decoded);
                }
            }
        }
        av_packet_unref(packet);
    }

cleanup:
    if (bsf) av_bsf_free(&bsf);
    if (scale) sws_freeContext(scale);
    if (decoded) av_frame_free(&decoded);
    if (converted) av_frame_free(&converted);
    if (decoder) avcodec_free_context(&decoder);
    if (encoder) avcodec_free_context(&encoder);
    if (packet) av_packet_free(&packet);
    if (encoded) av_packet_free(&encoded);
    if (format) avformat_close_input(&format);
    qInfo() << "GB28181 media stopped" << sourceUrl(config_) << "frames=" << emittedFrames;
    running_ = false;
}
