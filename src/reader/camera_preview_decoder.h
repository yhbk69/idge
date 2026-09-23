#ifndef CAMERA_PREVIEW_DECODER_H
#define CAMERA_PREVIEW_DECODER_H

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
    std::atomic<bool> running_{false};
    std::thread worker_;

    // 拍照相关
    std::mutex capture_mutex_;
    QString pending_capture_path_;
    QByteArray latest_jpeg_;
    int frame_count_ = 0;

    // RKNN推理相关 (参考FFmpegVideoDecoder)
    std::shared_ptr<PriorityQueue<object_detect_result_list>> detectResultQueue_;
    std::shared_ptr<DmaBufferPool> dmaBufferPool_;
    std::vector<PpeTask*> ppeTasks_;  // 动态创建的推理任务
    std::vector<std::unique_ptr<ScrfdFaceDetector>> faceDetectors_;
    
    // 配置和结果
    std::mutex detection_mutex_;
    std::vector<PreviewDetectorConfig> detector_configs_;
    std::vector<std::vector<PreviewDetectionBox>> detector_boxes_;
    std::vector<std::vector<std::string>> detector_labels_;
    std::vector<PreviewDetectionBox> detected_boxes_;  // 合并后的最新检测框
};

#endif // CAMERA_PREVIEW_DECODER_H
