#ifndef CAMERA_PREVIEW_DECODER_H
#define CAMERA_PREVIEW_DECODER_H

// ============================================================================
// CameraPreviewDecoder —— MIPI/USB 摄像头预览解码器（人员点名/盘点链路的前端）
// ============================================================================
// 职责：
//   1. 以 v4l2 + mjpeg(硬解 mjpeg_rkmpp，失败回退软解) 打开摄像头设备，
//      在独立 std::thread 中循环解码 → RGA/swscale 转 RGBA 双缓冲 → 画框
//      → emit frameReady(RenderFrame) 供 EGL/GL 上屏。
//   2. 抓拍：直接缓存最近一个 JPEG 原始码流（见 latest_jpeg_），拍照时
//      零重编码写盘，并用当前 RGBA DMA 帧同步生成缩略图。
//   3. 检测：人脸槽走同步 SCRFD（detectRgba，每 5 帧一次）；
//      装备/衣物槽走异步 PpeTask（YOLO）+ 单一共享结果队列按 result_id 路由。
//
// 与主流水线 FFmpegVideoDecoder 的差异：单结果队列多路复用(靠 od.id 区分)、
// 推理降频系数为 5（预览 CPU/NPU 预算更低）、检测框在 CPU 内存直绘 RGBA。
// ============================================================================

#include <QObject>
#include <QByteArray>
#include <QImage>
#include <QString>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <string>
#include <memory>
#include <opencv2/core/types.hpp>

// 引入video_decoder相同的头文件
#include "../ui/gl_video_widget.h"
#include "../buffer/DmaBufferPool.h"
#include "../queue/priority_queue.h"
#include "../task/ppe_task.hpp"
#include "../yolo11/common.hpp"
#include "scrfd_face_detector.h"

struct PreviewDetectorConfig {
    enum class Type { Face, Equipment };
    Type type = Type::Face;
    std::string model_path;      // RKNN模型路径
    std::string label_path;      // 标签文件路径(可选)
    rknn_core_mask npu_core_mask = RKNN_NPU_CORE_0;  // NPU核心掩码
};

struct PreviewDetectionBox {
    cv::Rect rect;
    std::string label;
};

class CameraPreviewDecoder final : public QObject
{
    Q_OBJECT

public:
    explicit CameraPreviewDecoder(QObject* parent = nullptr);
    ~CameraPreviewDecoder() override;

    void start(const QString& device = QStringLiteral("/dev/video41"));
    void stop();
    void setDetectorConfigs(const std::vector<PreviewDetectorConfig>& configs);
    void capture(const QString& path);

signals:
    void frameReady(RenderFrame frame);
    void photoCaptured(const QString& path, const QImage& thumbnail);
    void error(const QString& message);
    void finished();

private:
    void decodeLoop();
    void handleDecodedFrame(const uint8_t* pixels, int width, int height, int stride, bool bgr888);
    void drawDetectionBoxes(uint8_t* pixels, int width, int height, int stride);
    static int interruptCallback(void* opaque);

    QString device_;
    // 退出时序：stop() 置 running_=false → FFmpeg interruptCallback 返回 1
    // 打断阻塞的 av_read_frame → decodeLoop 走清理路径自然返回 → join() 回收。
    // 审查点：join() 是无界阻塞（主流水线已改为"协作退出 + 超时遗弃，绝不
    // terminate"），若 FFmpeg
    // 某版本不响应中断，stop() 可能长时间挂起，属已登记风险。
    std::atomic<bool> running_{false};
    std::thread worker_;

    // 拍照相关
    // capture_mutex_ 同时保护 pending_capture_path_ 与 latest_jpeg_：
    // UI 线程经 capture() 只登记目标路径；解码线程每收到一个 JPEG packet
    // 就刷新 latest_jpeg_，两者在 handleDecodedFrame() 中配对消费。
    std::mutex capture_mutex_;
    QString pending_capture_path_;
    // 最新一帧 JPEG 原始码流（0xFFD8 魔数校验后才缓存）。设计意图：
    // 抓拍直接落盘摄像头编码好的码流，避免"解码→画布→再编码"的
    // 质量损失与 CPU 开销；副作用是抓拍图不带检测框（原始流无叠加）。
    QByteArray latest_jpeg_;
    int frame_count_ = 0;

    // RKNN推理相关 (参考FFmpegVideoDecoder)
    // 与主流水线不同：这里所有 YOLO 任务共用一条结果队列，靠
    // od_results.id(=TaskConfig.result_id=检测器槽位) 区分来源；
    // 队列容量 12，满时自动淘汰优先级最低(time 最小=最旧)的元素；
    // 注意 PriorityQueue::tryPop 实为"只读堆顶不移除"（见 queue/README），
    // 因此每帧都能读到"迄今最新"的那份结果，天然实现结果粘滞显示。
    std::shared_ptr<PriorityQueue<object_detect_result_list>> detectResultQueue_;
    // 8 个 640x640 BGR888 DMA 缓冲（借出-归还，shared_ptr 引用计数归零自动回收）
    std::shared_ptr<DmaBufferPool> dmaBufferPool_;
    std::vector<PpeTask*> ppeTasks_;  // 动态创建的推理任务（start()重建、stop()/析构 delete，
                                      // 与 FFmpegVideoDecoder 的"只停不删"策略不同）
    // SCRFD 人脸检测器：与 ppeTasks_ 按槽位对齐（同一索引二选一），
    // 在解码线程内同步推理，无独立线程，因此不需要额外加锁保护其内部状态
    std::vector<std::unique_ptr<ScrfdFaceDetector>> faceDetectors_;
    
    // 配置和结果
    // detection_mutex_ 保护 detector_configs_/detector_boxes_/detected_boxes_：
    // 写方为解码线程（SCRFD 同步结果 + YOLO 异步结果两路都汇聚在解码线程），
    // UI 侧不直接读取这三组容器，锁主要用于 start/stop 重建时与在途回调互斥。
    std::mutex detection_mutex_;
    std::vector<PreviewDetectorConfig> detector_configs_;
    std::vector<std::vector<PreviewDetectionBox>> detector_boxes_;  // 按检测器槽位分缓存
    std::vector<std::vector<std::string>> detector_labels_;
    std::vector<PreviewDetectionBox> detected_boxes_;  // 合并后的最新检测框
};

#endif // CAMERA_PREVIEW_DECODER_H
