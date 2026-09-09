// video_decoder.h
#ifndef VIDEO_DECODER_H
#define VIDEO_DECODER_H

#include <QObject>
#include <QThread>
#include <atomic>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>
}

#include "gl_video_widget.h"
#include "queue/priority_queue.h"
#include "DmaBufferPool.h"
#include "easy_timer.h"
#include "yolo11_model.hpp"
#include "common.hpp"
#include "base_task.h"
#include "priority_queue.h"
#include "frame_queue.h"
#include "ModelPool.hpp"
#include "ppe_task.hpp"
class FFmpegVideoDecoder : public QObject
{
    Q_OBJECT
public:
    explicit FFmpegVideoDecoder(QObject* parent = nullptr);
    ~FFmpegVideoDecoder();

    void start(const QString& url);
    void stop();
    void setChannel(int ch) { channel_ = ch; }

signals:
    void frameReady(RenderFrame frame);
    void finished();
    void error(QString msg);
    void detectionResult(int channel, object_detect_result_list results);
    void statusChanged(int channel, int online);

private:
    void decodeLoop();
    void doInfer();

    QString url_;
    int channel_ = 0;
    QThread* thread_ = nullptr;
    std::atomic<bool> running_{false};
    std::shared_ptr<PriorityQueue<object_detect_result_list>> detectResultQueue_; 
    std::shared_ptr<DmaBufferPool> dmaBufferPool_;
    TIMER timer_;
    YOLO11Model* yolo11;
    std::shared_ptr<FrameQueue> frameQueue_;
    std::thread inferThread;
    std::shared_ptr<ModelPool> modelPool_;
    PpeTask* ppeTask_;
    PpeTask* ppeTask2_;
    PpeTask* ppeTask3_;

    
};

#endif
