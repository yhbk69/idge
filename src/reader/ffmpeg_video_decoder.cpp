// video_decoder.cpp
#include "ffmpeg_video_decoder.h"
#include "rga_converter.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QDateTime>
#include <cerrno>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include "DmaFrameBuffer.h"
#include "image_utils.h"
#include "ThreadPool.hpp"
#include "helmet_task.h"
#include "alarm_manager.h"
// X11 头文件把 None/Null/Bool 等定义成宏(0/int)，会顶掉 Qt 头文件里同名的
// 枚举成员（如 QUrl::None、QJsonValue::Null），这里统一去掉这些宏避免解析报错
#undef None
#undef Null
#undef Bool
#include "ConfigManager.h"
#include <fstream>

// ============================================================
// 报警截图异步写入器
//
// 为什么用后台线程：PNG/JPEG 编码比较费 CPU，若在解码线程同步执行会拖低帧率，
// 所以解码线程只负责【拷贝像素 + 提交】，编码写盘在独立后台线程完成。
//
// 为什么用 JPG 不用 PNG：
//   PNG 的 zlib 压缩在板子上非常慢；JPG(turbojpeg) 快得多、文件也小。
//   image_utils 的 jpg 写入只支持 RGB(3通道)，所以先把 RGBA 转成 RGB 再写。
//
// 目录：alarms/日期/通道N_类别_时间.jpg
// ============================================================
namespace {

// 一张待写盘的任务：存一份 RGB 像素拷贝（去掉 alpha，体积比 RGBA 小 25%）+ 路径
struct SnapJob {
    std::vector<unsigned char> rgb;    // RGB 像素拷贝（w*h*3 字节）
    int w = 0;
    int h = 0;
    QString path;                      // 保存路径(.jpg)
};

class SnapWriter {
public:
    static SnapWriter &get()
    {
        static SnapWriter w;            // 进程内唯一写盘线程，首次提交时启动
        return w;
    }

    // 提交一张截图：RGBA 像素先转成 RGB 再入队，立即返回（不阻塞解码线程）
    // 队列满（积压太多）时丢弃最旧的一张，避免内存无限增长
    void submit(int w, int h, const unsigned char *rgba, const QString &path)
    {
        SnapJob job;
        job.w = w;
        job.h = h;
        job.path = path;
        job.rgb.resize((size_t)w * h * 3);

        // RGBA(4字节/像素) -> RGB(3字节/像素)，跳过 alpha 字节
        const unsigned char *s = rgba;
        unsigned char *d = job.rgb.data();
        size_t n = (size_t)w * h;
        for (size_t i = 0; i < n; ++i) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            s += 4;
            d += 3;
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            if ((int)q_.size() >= kMaxPending) q_.pop();   // 满了丢最旧
            q_.push(std::move(job));
        }
        cv_.notify_one();
    }

private:
    static const int kMaxPending = 4;   // 最多积压 4 张，防止内存暴涨

    SnapWriter() { th_ = std::thread([this] { run(); }); }
    ~SnapWriter()
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();   // 退出前把队列里剩余的都写完
    }

    void run()
    {
        for (;;) {
            SnapJob job;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (stop_ && q_.empty()) return;
                job = std::move(q_.front());
                q_.pop();
            }
            writeJpg(job);
        }
    }

    // 真正把 RGB 像素编码成 JPG 并写盘（只在后台线程调用）
    static void writeJpg(SnapJob &job)
    {
        image_buffer_t img;
        memset(&img, 0, sizeof(img));
        img.format = image_format_t::IMAGE_FORMAT_RGB888;
        img.virt_addr = job.rgb.data();   // 指向 RGB 像素拷贝
        img.width = job.w;
        img.height = job.h;
        img.width_stride = job.w;
        write_image(job.path.toUtf8().constData(), &img);
    }

    std::thread th_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<SnapJob> q_;
    bool stop_ = false;
};

} // namespace

// 生成截图路径并提交给后台线程，返回保存路径
static QString submitAlarmSnapshot(int channel, const QString &className,
                                   int w, int h, const unsigned char *rgba)
{
    // 一级目录 alarms/ 存截图根目录
    if (::mkdir("alarms", 0755) != 0 && errno != EEXIST) {
        return QString();
    }
    // 二级目录 alarms/yyyyMMdd 按天归档
    QString day = QDate::currentDate().toString("yyyyMMdd");
    QString dir = QString("alarms/%1").arg(day);
    if (::mkdir(dir.toUtf8().constData(), 0755) != 0 && errno != EEXIST) {
        return QString();
    }

    // 文件名：通道N_类别_时分秒_毫秒.jpg
    QString path = QString("%1/ch%2_%3_%4.jpg")
        .arg(dir)
        .arg(channel + 1)
        .arg(className)
        .arg(QDateTime::currentDateTime().toString("HHmmss_zzz"));

    // 提交给后台线程写盘（拷贝像素副本），解码线程立即返回不等待
    SnapWriter::get().submit(w, h, rgba, path);
    return path;
}

enum AVPixelFormat GetHWFormat(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;

    for (p = pix_fmts; *p != -1; p++) {
        if (*p == AV_PIX_FMT_DRM_PRIME)
            return *p;
    }

    fprintf(stderr, "Failed to get HW surface format.\n");
    return AV_PIX_FMT_NONE;
}

// FFmpeg 中断回调：当 running_ 为 false 时中断 av_read_frame / avcodec_receive_frame
static int decodeInterruptCallback(void *ctx)
{
    std::atomic<bool> *running = static_cast<std::atomic<bool> *>(ctx);
    return running->load() ? 0 : 1;
}

FFmpegVideoDecoder::FFmpegVideoDecoder(QObject *parent) : QObject(parent) 
{
    dmaBufferPool_ = std::make_shared<DmaBufferPool>(64, 640, 640, RK_FORMAT_BGR_888, 16);

    // 从 config.json 的"模型路径1 + 级联模型2~5"建立级联推理任务列表
    buildCascadeTasks();
}

// ============================================================
// 级联任务构建：
//   model.path(模型1) + cascade.models 2~5 中非空的路径，各建一个 PpeTask。
//   PpeTask 每个占一个 std::thread，NPU 核心 0/1/2 轮流分配，避免全部挤在同一核。
//   每个任务配一个独立结果队列 slotQueues_[i]，画框时按模型上色、互不干扰。
//   空路径 = 不启用该槽（界面里空着即可）。
// ============================================================
void FFmpegVideoDecoder::buildCascadeTasks()
{
    ConfigManager &cfg = ConfigManager::instance();
    // 解码器构造比主窗口 ConfigManager::load() 早，这里确保配置已读入
    if (cfg.modelPath().isEmpty() && cfg.labelPath().isEmpty()) {
        cfg.load("config.json");
    }
    QString labelPath = cfg.labelPath();
    if (labelPath.isEmpty()) labelPath = "model/coco_80_labels_list.txt";

    // 收集启用的模型路径：模型1 永远取全局 model.path；2~5 读 cascade 配置
    QStringList modelPaths;
    QString m1 = cfg.modelPath().trimmed();
    if (!m1.isEmpty()) modelPaths << m1;
    for (int i = 2; i <= 5; ++i) {
        QString p = cfg.cascadeModelPath(i).trimmed();
        if (!p.isEmpty()) modelPaths << p;
    }
    if (modelPaths.isEmpty()) {
        // 兜底：什么也没配就用默认模型
        modelPaths << "model/yolo11n.rknn";
    }

    // 读一次类别名（各模型共用同一标签文件）
    classNames_.clear();
    {
        std::ifstream infile(labelPath.toStdString());
        std::string line;
        while (infile && std::getline(infile, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            classNames_.push_back(line);
        }
    }

    // 每个非空模型一个推理任务 + 一个结果队列
    int idx = 0;
    for (const QString &mp : modelPaths) {
        TaskConfig taskConfig;
        taskConfig.core_mask = (rknn_core_mask)(RKNN_NPU_CORE_0 + (idx % 3)); // 0/1/2 轮流
        taskConfig.modelPath = mp.toStdString();
        taskConfig.labelPath = labelPath.toStdString();

        PpeTask *task = new PpeTask(taskConfig);
        tasks_.push_back(task);

        // 每个任务一个独立结果队列（容量 8，满了丢旧保新）
        slotQueues_.push_back(std::make_shared<PriorityQueue<object_detect_result_list>>(8));
        idx++;
    }

    qDebug() << "Cascade tasks:" << tasks_.size() << "label:" << labelPath;
    for (size_t k = 0; k < tasks_.size(); ++k) {
        qDebug() << "  slot" << k + 1 << "->" << modelPaths[k];
    }
}

FFmpegVideoDecoder::~FFmpegVideoDecoder() { stop(); }

void FFmpegVideoDecoder::start(const QString &url)
{
    if (running_)
        stop();

    url_ = url;
    running_ = true;

    // 启动所有级联推理任务
    for (PpeTask *task : tasks_) {
        task->start();
    }

    thread_ = new QThread;
    moveToThread(thread_);
    connect(thread_, &QThread::started, this, &FFmpegVideoDecoder::decodeLoop);
    connect(thread_, &QThread::finished, thread_, &QThread::deleteLater);

    thread_->start();
}

void FFmpegVideoDecoder::stop()
{
    running_ = false;

    // 1) 先请求所有推理任务退出（关队列唤醒，不阻塞）
    for (PpeTask *task : tasks_) {
        task->requestStop();
    }

    // 2) 停解码线程（interrupt_callback 中断 av_read_frame，处理完当前帧即退出）
    if (thread_)
    {
        thread_->quit();
        if (!thread_->wait(3000)) {
            qWarning() << "Decoder thread did not stop in time, terminating";
            thread_->terminate();
            thread_->wait(1500);
        }
        thread_ = nullptr;
    }

    // 3) 有界回收推理线程：最多等 1.5s，超时 detach，绝不阻塞 UI 线程
    //    （否则 npu 忙时 PpeTask::join 会把主线程卡死——之前卡死根因）
    for (PpeTask *task : tasks_) {
        task->stopBestEffort(1500);
    }

    running_ = true;  // 重置，为下次 start() 准备
}

void FFmpegVideoDecoder::decodeLoop()
{
    int ret;

    // ===== 1. 打开文件 =====
    AVFormatContext *fmt_ctx = nullptr;

    // 设置中断回调，使 av_read_frame 在 running_=false 时能被中断
    AVIOInterruptCB interrupt_cb = {decodeInterruptCallback, &running_};
    fmt_ctx = avformat_alloc_context();
    fmt_ctx->interrupt_callback = interrupt_cb;

    ret = avformat_open_input(&fmt_ctx, url_.toUtf8().constData(), nullptr, nullptr);
    if (ret < 0)
    {
        emit error("Cannot open file");
        return;
    }
    avformat_find_stream_info(fmt_ctx, nullptr);

    int vidx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidx < 0)
    {
        emit error("No video stream");
        avformat_close_input(&fmt_ctx);
        return;
    }

    // ===== 2. 初始化解码器 =====
    const AVCodec *codec = avcodec_find_decoder_by_name("h264_rkmpp");
    if (!codec)
        codec = avcodec_find_decoder_by_name("hevc_rkmpp");
    if (!codec)
        codec = avcodec_find_decoder(fmt_ctx->streams[vidx]->codecpar->codec_id);
    if (!codec)
    {
        emit error("No decoder");
        avformat_close_input(&fmt_ctx);
        return;
    }

    AVCodecContext *dec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec_ctx, fmt_ctx->streams[vidx]->codecpar);

    enum AVPixelFormat stHWPixFmt = AV_PIX_FMT_DRM_PRIME;
    dec_ctx->get_format = GetHWFormat;

    AVBufferRef *pHWDeviceCtx;

    enum AVHWDeviceType type;
    type = av_hwdevice_find_type_by_name("rkmpp");

    int err = 0;
    if ((err = av_hwdevice_ctx_create(&pHWDeviceCtx, type,
									  NULL, NULL, 0)) < 0) {
		qWarning() << "Failed to create DRM HW device context";
        char errrbuf[256];
        av_strerror(err, errrbuf, sizeof(errrbuf));
        printf("failed %s\n", errrbuf);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
		return ;
	}

    dec_ctx->hw_device_ctx = av_buffer_ref(pHWDeviceCtx);

    dec_ctx->thread_count = 1;
    avcodec_open2(dec_ctx, codec, nullptr);

    int vid_w = dec_ctx->width;
    int vid_h = dec_ctx->height;
    qDebug() << "Decoder:" << codec->name << vid_w << "x" << vid_h;

    emit statusChanged(channel_, 1);

    // ===== 3. 分配 RGA 输出 RGBA Buffer =====
    // 打开 DRM 设备用于分配 DUMB buffer
    int drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0)
        drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);

    // 双缓冲 RGBA（RGA 输出 → EGLImage 输入）
    DmaBuffer rgba_bufs[2];


    int back_buf = 0;

    // for (int i = 0; i < 2; i++)
    // {
    //     //rgba_bufs[i] = dmabuf_alloc(drm_fd, vid_w, vid_h, DRM_FORMAT_ABGR8888);
    //     rgba_bufs[i] = dmabuf_alloc(drm_fd, vid_w, vid_h, DRM_FORMAT_RGBA8888);
    //     if (!rgba_bufs[i].valid())
    //     {
    //         emit error("Failed to alloc RGBA buffer");
    //         return;
    //     }
    //     qDebug() << "RGBA buffer" << i
    //              << "fd=" << rgba_bufs[i].fd
    //              << "stride=" << rgba_bufs[i].stride
    //              << "size=" << rgba_bufs[i].size;
    // }

    DmaFrameBuffer* dst_bufs[2];
    dst_bufs[0] = new DmaFrameBuffer();
    dst_bufs[0]->setWidth(vid_w);
    dst_bufs[0]->setHeight(vid_h);
    dst_bufs[0]->setFormat(DRM_FORMAT_RGBA8888);
    dst_bufs[0]->alloc();
    
    dst_bufs[1] = new DmaFrameBuffer();
    dst_bufs[1]->setWidth(vid_w);
    dst_bufs[1]->setHeight(vid_h);
    dst_bufs[1]->setFormat(DRM_FORMAT_RGBA8888);
    dst_bufs[1]->alloc();


    // ===== 4. 解码循环 =====
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    int frame_count = 0;

    QElapsedTimer timer;
    timer.start();
    timer.elapsed();
    TIMER t;

    while (running_)
    {
        ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0)
        {
            // Flush
            avcodec_send_packet(dec_ctx, nullptr);
            while (running_ && avcodec_receive_frame(dec_ctx, frame) == 0)
            {
                // 处理剩余帧（同下方逻辑）
                av_frame_unref(frame);
            }
            break;
        }

        if (pkt->stream_index != vidx)
        {
            av_packet_unref(pkt);
            continue;
        }

        avcodec_send_packet(dec_ctx, pkt);
        av_packet_unref(pkt);

        while (running_ && avcodec_receive_frame(dec_ctx, frame) == 0)
        {
            t.tik();
            frame_count++;

            int src_fd = -1;
            int src_stride = vid_w;

            if (frame->format == AV_PIX_FMT_DRM_PRIME)
            {
                AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)frame->data[0];
                if (desc && desc->nb_objects > 0)
                {
                    src_fd = desc->objects[0].fd;
                    src_stride = desc->layers[0].planes[0].pitch;
                }
            }

            if (src_fd >= 0)
            {
                // ===== RGA: NV12 → RGBA（fd → fd，零拷贝） =====
                //DmaBuffer &dst = rgba_bufs[back_buf];

                DmaFrameBuffer* dst_buf = dst_bufs[back_buf];
                bool ok = RgaConverter::convertNV12ToRGBAbyRGA(
                    src_fd, 
                    dst_buf->fd(), 
                    frame->width, 
                    frame->height, 
                    src_stride, 
                    frame->height);
                //t.tok();
                //t.print_time("convertNV12ToRGBAbyRGA");
                // bool ok = RgaConverter::nv12_to_rgba(
                //     src_fd, vid_w, vid_h, src_stride,
                //     dst.fd, vid_w, vid_h, dst.stride);

                if (ok)
                {
                    
                    if (frame_count % 3==0) {
                        
                        //非阻塞获取
                        //auto rgabuffer = dmaBufferPool_->tryAcquire();
                        auto rgabuffer = dmaBufferPool_->tryAcquireSharedPtr();
                        if (rgabuffer)
                        {
                            //std::shared_ptr<RgaBuffer> detImg =  rgaBufferPool->acquire();
                            // DmaFrameBuffer * detectFrameBufer = new DmaFrameBuffer();
                            // detectFrameBufer->setWidth(640);
                            // detectFrameBufer->setHeight(640);
                            // detectFrameBufer->setFormat(DRM_FORMAT_RGB888);
                            // detectFrameBufer->alloc();
                            // RgaConverter::rgba_to_rgb_resize(dst_buf->fd(), dst_buf->width(), dst_buf->height(), dst_buf->stride(), 
                            //                                     detectFrameBufer->fd(), detectFrameBufer->width(), 
                            //                                     detectFrameBufer->height(), detectFrameBufer->stride(), true);

                            //t.tik();
                            RgaConverter::rgba_to_rgb_resize(dst_buf->fd(), dst_buf->width(), dst_buf->height(), dst_buf->stride(), 
                                                                rgabuffer->fd,  rgabuffer->width, 
                                                                rgabuffer->height, rgabuffer->width_stride, true);

                            //t.tok();
                            //t.print_time("rgba_to_rgb_resize");                                 
                            
                            //t.tik();
                            std::shared_ptr<image_buffer_t>  image = std::make_shared<image_buffer_t>();
                            image->format = image_format_t::IMAGE_FORMAT_RGB888;
                            image->virt_addr = (uint8_t*)(rgabuffer->va);
                            image->width = 640;
                            image->height = 640;
                            image->width_stride = 640;
                            image->srcWidth = dst_buf->width();
                            image->srcHeight = dst_buf->height();
                            //image->dmaBuffer = rgabuffer;
                            image->sp_dmaBuffer = rgabuffer;
                            auto t = chrono::system_clock::now();
                            image->time = t.time_since_epoch().count();

                            // 每 3 帧做一次推理；把这一帧同时派给"所有已启用的级联模型"
                            // 每个模型一个独立 TaskData（共享同一张图像），各自排队推理，
                            // 结果写回各自的 slotQueues_[k]，解码线程按模型上色画框
                            auto now = chrono::system_clock::now();
                            long ts = now.time_since_epoch().count();
                            for (size_t k = 0; k < tasks_.size(); ++k) {
                                auto td = std::make_shared<TaskData>(ts, image, slotQueues_[k]);
                                tasks_[k]->put(td);
                            }
                            //timer_.tok();
                            //timer_.print_time("frameQueue_->pushAndReplace(image);");
                            // object_detect_result_list od_results;
                            // yolo11->detect(&image, &od_results, true);
                            // t.tok();
                            // t.print_time("detect");     
                            
                            // t.tik();
                            
                            // dmaBufferPool_->release(rgabuffer);
                            //write_image("../out123.png", &dislayImage);
                            //write_image("../12345.jpg", &src_image);

                        }

                    }
                    
                    // rgaBufferPool_->release(rgabufer);
                    //detectFrameBufer->release();

                    // ============ 级联结果合并画框（整帧 + 分工） ============
                    // 每个启用模型的推理结果在各自的 slotQueues_[k] 里，
                    // 这里依次取出并用不同颜色画到同一帧上：
                    //   模型1=蓝, 2=绿, 3=红, 4=黄, 5=橙 —— 一眼能分清是哪个模型检出的
                    // 文字前缀 M{槽位}，如 "M1 person 87.3%"
                    const int kSlotColors[5] = {
                        (int)COLOR_BLUE, (int)COLOR_GREEN, (int)COLOR_RED,
                        (int)COLOR_YELLOW, (int)COLOR_ORANGE
                    };

                    image_buffer_t dislayImage;
                    dislayImage.format = image_format_t::IMAGE_FORMAT_RGBA8888;
                    dislayImage.virt_addr = (unsigned char*)dst_buf->ptr();
                    dislayImage.width = dst_buf->width();
                    dislayImage.height = dst_buf->height();

                    for (size_t k = 0; k < slotQueues_.size(); ++k) {
                        // 取出该模型最新一次结果（tryPop 不清空，取最新的一批）
                        object_detect_result_list od;
                        if (!slotQueues_[k]->tryPop(od)) continue;

                        int color = kSlotColors[(int)k % 5];
                        char text[256];
                        for (int i = 0; i < od.count; i++) {
                            const object_detect_result &d = od.results[i];
                            draw_rectangle(&dislayImage,
                                           d.box.left, d.box.top,
                                           d.box.right - d.box.left,
                                           d.box.bottom - d.box.top,
                                           color, 3);

                            if (d.cls_id >= 0 && d.cls_id < (int)classNames_.size())
                                snprintf(text, sizeof(text), "M%zu %s %.1f%%",
                                         k + 1, classNames_[d.cls_id].c_str(), d.prop * 100);
                            else
                                snprintf(text, sizeof(text), "M%zu cls_%d %.1f%%",
                                         k + 1, d.cls_id, d.prop * 100);
                            draw_text(&dislayImage, text, d.box.left, d.box.top - 20, color, 10);
                        }

                        // ============ 报警判定 + 截图（每个模型的结果都检查一遍） ============
                        // ingest(): 统计 + 判断是否命中报警类别 + 2秒去重限流。
                        // 多个相同模型对同一目标会重复命中，但去重限流保证同通道同类 2s 只报 1 次，
                        // 所以相同模型实验下不会产生重复报警。
                        QVector<AlarmRecord> newAlarms =
                            AlarmManager::instance().ingest(channel_, od);
                        if (!newAlarms.isEmpty()) {
                            QString snap;
                            if (AlarmManager::instance().screenshotsEnabled()) {
                                // 只拷贝像素提交给后台线程写盘，不阻塞解码线程
                                snap = submitAlarmSnapshot(
                                    channel_, newAlarms.front().className,
                                    dislayImage.width, dislayImage.height,
                                    (const unsigned char *)dislayImage.virt_addr);
                            }
                            for (auto &a : newAlarms) a.imgPath = snap;
                            // 入库并通知界面（弹 toast / 报警列表新增）
                            AlarmManager::instance().storeAndNotify(newAlarms);
                        }
                    }

                    
                    // dup fd 传递给渲染线程（独立生命周期）
                    RenderFrame rf;
                    //rf.fd = dup(dst.fd);
                    rf.fd = dup(dst_buf->fd());
                    rf.width = vid_w;
                    rf.height = vid_h;
                    rf.stride = dst_buf->stride();

                    emit frameReady(rf);
                   
                }

                back_buf = 1 - back_buf; // 切换缓冲
            }

            av_frame_unref(frame);

            // 帧率控制（30fps）
            qint64 expected_us = frame_count * 33333;
            qint64 elapsed_us = timer.elapsed() * 1000;
            if (expected_us > elapsed_us)
            {
                QThread::usleep(expected_us - elapsed_us);
            }

            t.tok();
            //t.print_time("decode and infer");
        }
    }

    qDebug() << "Decode finished, frames:" << frame_count;

    emit statusChanged(channel_, 0);

    // ===== 5. 清理 =====
    for (int i = 0; i < 2; i++)
    {
        delete dst_bufs[i];
        dst_bufs[i] = nullptr;
    }
    close(drm_fd);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);

    emit finished();
}
