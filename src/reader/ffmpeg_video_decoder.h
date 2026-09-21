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
#include "video_recorder.h"

class FFmpegVideoDecoder : public QObject
{
    Q_OBJECT
public:
    explicit FFmpegVideoDecoder(QObject* parent = nullptr);
    ~FFmpegVideoDecoder();

    void start(const QString& url);
    void stop();
    void setChannel(int ch) { channel_ = ch; }

    /**
     * @brief 热更新所有模型
     *
     * 在不停止解码的情况下替换所有推理模型。
     * 用于生产环境中更换检测模型（如从安全帽检测切换到反光背心检测）。
     *
     * @param modelPaths: 新模型路径列表（空字符串表示跳过该模型）
     * @param labelPaths: 新标签文件路径列表
     * @return: 成功替换的模型数量
     *
     * @note 会短暂暂停推理（~100ms），解码线程不受影响
     */
    int reloadAllModels(const QStringList &modelPaths, const QStringList &labelPaths);

    /**
     * @brief 获取视频录制器
     * @return: VideoRecorder 指针
     */
    VideoRecorder* videoRecorder() const { return videoRecorder_; }

    /**
     * @brief 启用/禁用视频录制
     * @param enabled: true=启用，false=禁用
     * @param bufferSeconds: 环形缓冲区时长（秒），仅启用时有效
     */
    void setVideoRecordingEnabled(bool enabled, int bufferSeconds = 30);

    /**
     * @brief 将环形缓冲区 dump 到文件
     * @param filePath: 输出 MP4 文件路径
     * @return: true=成功
     */
    bool dumpVideoToFile(const QString &filePath);

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

    // ===== 视频录制 =====
    VideoRecorder* videoRecorder_ = nullptr;       // 视频录制器（环形缓冲区）
    bool videoRecordingEnabled_ = false;           // 是否启用视频录制
};

#endif
