// video_decoder.h
//
// ============================================================================
// FFmpegVideoDecoder —— 主流水线"解码-推理-画框"驱动器（视频通道侧总控）
// ============================================================================
// 职责：
//   1. 在独立 QThread 中运行 decodeLoop()：FFmpeg(h264_rkmpp) 硬解出
//      NV12 DMA-BUF → RGA 转 RGBA(双缓冲) → 每3帧 RGA 缩放 640x640 分发给
//      级联推理任务(PpeTask) → 从各任务结果队列取框画到 RGBA 上 → emit frameReady。
//   2. 持有并管理级联多模型：每个非空模型路径一个 PpeTask(独占推理线程)
//      + 一个独立结果队列(slotQueues_)，互不干扰、按槽上色。
//
// 线程模型与所有权约定（重要）：
//   - this 被 moveToThread(thread_)，decodeLoop 在该线程执行；
//     running_ 为 atomic<bool>，跨线程读写安全（配合 FFmpeg 中断回调退出）。
//   - frameReady 携带的 RenderFrame.fd 是 dup() 出来的独立 fd，
//     接收方(渲染侧)负责 close()，与本类的双缓冲生命周期解耦。
//   - tasks_ 中的 PpeTask 在析构/stop() 中只请求退出、不 delete（生命周期
//     随进程，属已知的"故意泄漏"，避免 detach 后 delete 触发 use-after-free）。
// ============================================================================
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
    // 运行标志：true→false 时通过 FFmpeg interrupt_callback 打断阻塞的
    // av_read_frame/avcodec_receive_frame，实现解码线程快速退出；
    // stop() 末尾会重置为 true，为下次 start() 做准备
    std::atomic<bool> running_{false};
    // 640x640 RGB(BGR888) DMA 缓冲池（借出-归还语义：tryAcquireSharedPtr
    // 返回 shared_ptr，最后一个引用(含 image_buffer_t::sp_dmaBuffer)释放时自动归还）
    std::shared_ptr<DmaBufferPool> dmaBufferPool_;

    // ===== 级联多模型：每个非空模型一个推理任务 + 一个结果队列 =====
    std::vector<PpeTask*> tasks_;                  // 推理任务(每个一个线程)
    std::vector<std::shared_ptr<PriorityQueue<object_detect_result_list>>> slotQueues_; // 各任务的结果
    std::vector<std::string> classNames_;          // 类别名(所有模型共用同一标签文件)
};

#endif
