// video_decoder.h
#ifndef VIDEO_DECODER_H
#define VIDEO_DECODER_H

#include <QObject>
#include <QThread>
#include <QString>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <memory>

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
    void statusChanged(int channel, int online);

private:
    void decodeLoop();
    // 从 config.json 读取"模型路径1 + 级联模型2~5"，为每个非空模型建一个推理任务
    void buildCascadeTasks();

    QString url_;
    int channel_ = 0;
    QThread* thread_ = nullptr;
    std::atomic<bool> running_{false};
    std::shared_ptr<DmaBufferPool> dmaBufferPool_;

    // ===== 级联多模型：每个非空模型一个推理任务 + 一个结果队列 =====
    std::vector<PpeTask*> tasks_;                  // 推理任务(每个一个线程)
    std::vector<std::shared_ptr<PriorityQueue<object_detect_result_list>>> slotQueues_; // 各任务的结果
    std::vector<std::string> classNames_;          // 类别名(所有模型共用同一标签文件)
};

#endif
