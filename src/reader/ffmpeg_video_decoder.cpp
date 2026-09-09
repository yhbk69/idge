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

// ============================================================
// 报警截图异步写入器
//
// 为什么需要后台线程：
//   PNG 编码 + 写盘很费 CPU（720p 一张要几百毫秒），如果放在解码线程里
//   同步执行，会直接把帧率拖到个位数。
//   所以这里：解码线程只负责【拷贝一份像素 + 提交】，编码和写盘全部
//   在独立的后台线程完成，解码线程绝不阻塞。
//
// 目录结构：alarms/日期/通道N_类别_时间.png
// 为什么用 PNG 不用 JPG：
//   画面像素是 RGBA(4通道)，而 image_utils 的 jpg 写入只支持 RGB(3通道)，
//   PNG 写入支持 4 通道，所以这里存 png。
// ============================================================
namespace {

// 一张待写盘的截图任务（含一份像素拷贝 + 目标路径）
struct SnapJob {
    std::vector<unsigned char> rgba;   // RGBA 像素拷贝（w*h*4 字节）
    int w = 0;
    int h = 0;
    QString path;                      // 保存路径
};

class SnapWriter {
public:
    static SnapWriter &get()
    {
        static SnapWriter w;            // 进程内唯一写盘线程，首次提交时启动
        return w;
    }

    // 提交一张截图：把像素复制一份入队，立即返回（不阻塞解码线程）
    // 队列满（积压太多）时丢弃最旧的一张，避免内存无限增长
    void submit(int w, int h, const unsigned char *px, const QString &path)
    {
        SnapJob job;
        job.w = w;
        job.h = h;
        job.path = path;
        job.rgba.assign(px, px + (size_t)w * h * 4);

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
            writePng(job);
        }
    }

    // 真正把像素编码成 PNG 并写盘（只在后台线程调用）
    static void writePng(SnapJob &job)
    {
        image_buffer_t img;
        memset(&img, 0, sizeof(img));
        img.format = image_format_t::IMAGE_FORMAT_RGBA8888;
        img.virt_addr = job.rgba.data();      // 指向像素拷贝
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

    // 文件名：通道N_类别_时分秒_毫秒.png
    QString path = QString("%1/ch%2_%3_%4.png")
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
    detectResultQueue_= std::make_shared<PriorityQueue<object_detect_result_list>>(12);
    dmaBufferPool_ = std::make_shared<DmaBufferPool>(64, 640, 640, RK_FORMAT_BGR_888, 16);
    yolo11 = new YOLO11Model("model/yolo11n.rknn", 
                        "model/coco_80_labels_list.txt", 
                        RKNN_NPU_CORE_0);
    frameQueue_ = std::make_shared<FrameQueue>(2, dmaBufferPool_);
    AppConfig appConfig;
    modelPool_ = std::make_shared<ModelPool>(appConfig);
    modelPool_->init();
    TaskConfig taskConfig;
    taskConfig.core_mask = RKNN_NPU_CORE_0;
    taskConfig.modelPath = "model/yolo11n.rknn";
    taskConfig.labelPath = "model/coco_80_labels_list.txt";
    ppeTask_ = new PpeTask(taskConfig);

    TaskConfig taskConfig2;
    taskConfig2.core_mask = RKNN_NPU_CORE_1;
    taskConfig2.modelPath = "model/yolo11n.rknn";
    taskConfig2.labelPath = "model/coco_80_labels_list.txt";
    ppeTask2_ = new PpeTask(taskConfig2);

    TaskConfig taskConfig3;
    taskConfig3.core_mask = RKNN_NPU_CORE_2;
    taskConfig3.modelPath = "model/yolo11n.rknn";
    taskConfig3.labelPath = "model/coco_80_labels_list.txt";
    ppeTask3_ = new PpeTask(taskConfig3);
}

FFmpegVideoDecoder::~FFmpegVideoDecoder() { stop(); }

void FFmpegVideoDecoder::start(const QString &url)
{
    if (running_)
        stop();

    url_ = url;
    running_ = true;
    //inferThread = std::thread(&FFmpegVideoDecoder::doInfer, this);
    ppeTask_->start();
    ppeTask2_->start();
    ppeTask3_->start();

    thread_ = new QThread;
    moveToThread(thread_);
    connect(thread_, &QThread::started, this, &FFmpegVideoDecoder::decodeLoop);
    connect(thread_, &QThread::finished, thread_, &QThread::deleteLater);

    thread_->start();

    

}

void FFmpegVideoDecoder::stop()
{
    running_ = false;

    // 先唤醒并请求推理线程退出（不阻塞），避免它们在解码线程还在投递时被 join 卡住
    ppeTask_->requestStop();
    ppeTask2_->requestStop();
    ppeTask3_->requestStop();

    // 再停止解码线程（interrupt_callback 中断 av_read_frame，线程处理完当前帧后退出）
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

    // 最后回收推理线程（此时解码线程已停，不会再投递新任务）
    ppeTask_->join();
    ppeTask2_->join();
    ppeTask3_->join();

    running_ = true;  // 重置，为下次 start() 准备
}

void FFmpegVideoDecoder::doInfer()
{
    TIMER t;
    while (running_)
    {
        image_buffer_t frame;
        if (frameQueue_->wait_and_pop(frame))
        {
            printf("do infer \n");
            t.tik();
            std::shared_ptr<YOLO11Model> model1 = modelPool_->getModel("1");
            object_detect_result_list od_results1;
            if (!running_)
            {
                break;
            }
            
            model1->detect(&frame, &od_results1, true);
            od_results1.time = frame.time;
            detectResultQueue_->push(od_results1);
            t.tok();
            t.print_time("model1->detect");
            t.tik();

            std::shared_ptr<YOLO11Model> model2 = modelPool_->getModel("2");
            object_detect_result_list od_results2;
            if (!running_)
            {
                break;
            }
            //model2->detect(&frame, &od_results2, true);
            //od_results2.time = frame.time;
            //detectResultQueue_->push(od_results2);
            
            dmaBufferPool_->release(frame.dmaBuffer);
            t.tok();
            t.print_time("model2->detect");
        }
    }
    

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

                            std::shared_ptr<TaskData> taskData = std::make_shared<TaskData>(t.time_since_epoch().count(),
                                                                                    image,
                                                                                    this->detectResultQueue_);
                            ppeTask_->put(taskData);
                            ppeTask2_->put(taskData);
                            ppeTask3_->put(taskData);
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

                    // 画框和概率
                    object_detect_result_list od_results;
                    if (detectResultQueue_->tryPop(od_results))
                    {
                        image_buffer_t dislayImage;
                        dislayImage.format = image_format_t::IMAGE_FORMAT_RGBA8888;
                        dislayImage.virt_addr = (unsigned char*)dst_buf->ptr();
                        dislayImage.width = dst_buf->width();
                        dislayImage.height = dst_buf->height();

                        char text[256];
                        for (int i = 0; i < od_results.count; i++)
                        {
                            object_detect_result *det_result = &(od_results.results[i]);

                            int x1 = det_result->box.left;
                            int y1 = det_result->box.top;   
                            int x2 = det_result->box.right;
                            int y2 = det_result->box.bottom;

                            draw_rectangle(&dislayImage, x1, y1, x2 - x1, y2 - y1, COLOR_BLUE, 3);

                            const auto &cls = yolo11->getClassNames();
                            if (det_result->cls_id >= 0 && det_result->cls_id < (int)cls.size())
                                snprintf(text, sizeof(text), "%s %.1f%%", cls[det_result->cls_id].c_str(), det_result->prop * 100);
                            else
                                snprintf(text, sizeof(text), "cls_%d %.1f%%", det_result->cls_id, det_result->prop * 100);
                            draw_text(&dislayImage, text, x1, y1 - 20, COLOR_RED, 10);
                        }

                        // ============ 报警判定 + 截图 ============
                        // 1) ingest(): 把检测结果交给报警管理器做"统计 + 报警判定"
                        //    - 累加各类别检测数量（看板用）
                        //    - 判断检测到的类别是否在报警名单里（config.json alarm.classes）
                        //    - 同一个通道同一类别在 2 秒内只算一次报警（去重限流）
                        //    返回值 newAlarms 是"这次真正要上报的报警"列表（可能为空）
                        QVector<AlarmRecord> newAlarms =
                            AlarmManager::instance().ingest(channel_, od_results);

                        // 2) 如果有新报警，就把"当前这一帧"的像素交给后台线程写盘
                        //    这里只拷贝像素+提交，不做 PNG 编码/写盘，避免拖慢解码线程
                        if (!newAlarms.isEmpty()) {
                            QString snap = submitAlarmSnapshot(
                                channel_,
                                newAlarms.front().className,
                                dislayImage.width,
                                dislayImage.height,
                                (const unsigned char *)dislayImage.virt_addr);
                            // 同一帧的多个报警共用同一张截图
                            for (auto &a : newAlarms) {
                                a.imgPath = snap;
                            }
                            // 3) storeAndNotify(): 真正入库并通知界面
                            //    -> 界面收到 alarmGenerated 信号：弹 toast、报警列表自动插入新行
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
