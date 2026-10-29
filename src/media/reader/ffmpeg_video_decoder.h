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
//   3. 构造参数 InferenceMode=PreviewOnly 时退化为"纯取流器"：只解码 + 转 RGBA +
//      emit frameReady/供 captureCurrentFramePtr 取帧，不加载任何 RKNN 模型、不建
//      推理线程（智能识别 QR/OCR 只要一帧原始画面，不该占用 NPU）。
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
#include <mutex>
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
#include "priority_queue.h"
#include "DmaBufferPool.h"
#include "easy_timer.h"
#include "yolo11_model.hpp"
#include "common.hpp"
#include "base_task.h"
#include "priority_queue.h"
#include "ppe_task.hpp"
#include "video_recorder.h"
#include "fence_shape.h"

class FFmpegVideoDecoder : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief 是否挂载级联推理任务
     *
     * Cascade     —— 默认，监控流水线用：构造时 buildCascadeTasks() 加载 RKNN 模型。
     * PreviewOnly —— 只取帧显示/抓帧（智能识别 QR、OCR）：不建 PpeTask、不分配
     *                640×640 缓冲池、不做每 3 帧预处理，NPU 完全不参与。
     */
    enum class InferenceMode { Cascade, PreviewOnly };

    explicit FFmpegVideoDecoder(QObject* parent = nullptr,
                                InferenceMode mode = InferenceMode::Cascade);
    ~FFmpegVideoDecoder();

    void start(const QString& url);
    void stop();
    // 两阶段停止：先 signalStop 通知所有解码器退出（不阻塞），
    // 再 waitForStop 等待线程实际退出。用于 closeEvent 并行停止多路解码器。
    void signalStop();
    void waitForStop();
    void setChannel(int ch) { channel_ = ch; }

    /**
     * @brief 本路 decodeLoop 是否还在跑（start() 拒绝重启用的同一判据）
     *
     * 热更新流程要靠它确认"旧循环真的退了"：signalStop()+waitForStop() 之后
     * 若仍为 true，说明解码线程被超时遗弃，此时绝不能碰 tasks_，也不能 start()。
     */
    bool isLoopAlive() const { return liveLoops_.load() > 0; }

    /**
     * @brief 丢弃本通道全部推理任务与槽位表，让下一次 start() 按新配置重建
     *
     * 调用前提（两个都必须满足，否则是 UAF）：
     *   1. 已在 GUI 线程 signalStop() + waitForStop()，且 isLoopAlive()==false；
     *   2. 由 GUI 线程调用（解码循环已退出，tasks_ 不再有并发访问者）。
     * 只 delete 推理线程已确认退出(isThreadFinished())的任务；未确认的转入
     * quarantine_ 并打告警——沿用 stop() "只停不删"的既有政策，detach 后的推理线程
     * 仍可能访问对象。正常路径走不到 quarantine_，只有 NPU 卡死才会攒。
     */
    void releaseTasks();

    /**
     * @brief 在线替换各槽位的报警类别（video.models[<id>].alarm_class）
     * @param cls 下标 = 槽位，空串 = 只画框不报警；长度必须等于当前槽位数
     * @return false 表示槽位数不匹配（模型列表变了），调用方应改走 releaseTasks+重开
     *
     * 这是唯一允许在解码器运行期从 GUI 线程调的接口：只换字符串，不动任务、
     * 不动队列、不碰 NPU 上下文。报警类别本就在每帧的 SlotContext 里现取。
     */
    bool updateSlotAlarmClasses(const std::vector<QString> &cls);

    /**
     * @brief 报警片段录制器（硬件编码环形缓冲区）
     * @return: 未启用录制时为空 shared_ptr
     *
     * 返回引用计数副本而非裸指针：报警片段是"报警后若干秒"才执行的延迟任务，
     * 任务持有自己的 shared_ptr，解码线程停止/本对象销毁后编码器依然存活。
     */
    std::shared_ptr<VideoRecorder> videoRecorder() const { return videoRecorder_; }

    /**
     * @brief 读当前捕获帧的几何信息（供智能识别 QR/OCR 使用）
     * @return 宽高都>0 时 true；尚未产出任何帧时 false（此时 stride 不可用）
     *
     * stride 是 dst_buf 的行字节跨度，**不等于** width*4，按它逐行拷才正确。
     */
    bool getCurrentFrameInfo(int &width, int &height, int &stride) const;

    /**
     * @brief 获取当前帧的 CPU 可读指针（供智能识别 QR/OCR 使用）
     * @return 指向 RGBA 像素数据的指针；尚未产出帧时返回 nullptr
     *
     * 指针来自 DmaFrameBuffer 的 DRM mmap，CPU 可直接读，无需再拷贝到堆。
     * 配合 getCurrentFrameInfo() 取宽高与 stride。
     *
     * 两条使用约束（本接口是"看一眼最新帧"，不是帧交付）：
     *   1. 解码器只有双缓冲（dst_bufs[2]），本帧指针在约 2 帧后就会被下一帧覆盖，
     *      拿到后要立刻读完/拷走，不能存起来晚点用。
     *   2. stop() 会把指针置空并随后释放 DmaFrameBuffer；跨线程调用方必须在
     *      stop() 之前停止取帧（智能识别页即按"先停扫描线程、再停解码器"收尾）。
     */
    const uint8_t* captureCurrentFramePtr() const;

    /**
     * @brief 配置报警视频录制（须在 start() 前调用）
     * @param cfg: 录制参数（来自 config.json 的 recording 段）
     *
     * 这里只保存参数：录制器与硬件编码器都要等 decodeLoop 打开流、
     * 拿到真实分辨率和帧率之后才能建立，所以构造动作在 decodeLoop 里。
     * cfg.enabled=false 时整条录制链路不分配任何硬件资源。
     */
    void setRecordingConfig(const RecorderConfig &cfg);

signals:
    void frameReady(RenderFrame frame);
    void finished();
    void error(QString msg);
    void statusChanged(int channel, int online);

private:
    void decodeLoop();
    /**
     * @brief 为本通道构建级联推理任务（首次 start() 时调用，幂等）
     *
     * 槽位来自 ModelRegistry::resolveChannelSlots(channel_)：通道在 config 里
     * 只写模型库 id 列表，路径/标签/报警类别由模型库与参数表解析。
     * 必须在 setChannel() 之后调用，否则 channel_ 还是默认的 0，四路会共用
     * 通道 0 的模型列表。
     */
    void buildCascadeTasks();

    QString url_;
    int channel_ = 0;
    QThread* thread_ = nullptr;
    // 运行标志：true→false 时通过 FFmpeg interrupt_callback 打断阻塞的
    // av_read_frame/avcodec_receive_frame，实现解码线程快速退出；
    // 由 start() 置 true（stop() 不再复位，避免复活超时遗弃的旧循环）
    std::atomic<bool> running_{false};
    // 存活 decodeLoop 实例计数：++/-- 由 RAII 守卫覆盖所有退出路径。
    // start() 拒绝在 >0 时重启（旧线程还持有本对象状态）；
    // stop() 等待超时后遗弃线程时靠它兜底，QThread 由 finished→deleteLater 回收
    std::atomic<int> liveLoops_{0};
    // 640x640 RGB(BGR888) DMA 缓冲池（借出-归还语义：tryAcquireSharedPtr
    // 返回 shared_ptr，最后一个引用(含 image_buffer_t::sp_dmaBuffer)释放时自动归还）
    std::shared_ptr<DmaBufferPool> dmaBufferPool_;

    // ===== 级联多模型：每个非空模型一个推理任务 + 一个结果队列 =====
    // PreviewOnly 模式下这三个容器保持为空，见下方 inferenceEnabled_。
    std::vector<PpeTask*> tasks_;                  // 推理任务(每个一个线程)
    std::vector<std::shared_ptr<PriorityQueue<object_detect_result_list>>> slotQueues_; // 各任务的结果
    // 每槽位独立的类别名表：slotClassNames_[k] 对应 tasks_[k] 模型的标签文件。
    // 级联各模型类别不同（coco80 / helmet 2类 / vest 2类…），共用一张表会把
    // helmet 的 cls_id 套用 coco 类名导致报警类别错标（2026-09-24 修复）。
    std::vector<std::vector<std::string>> slotClassNames_;
    // 每槽位模型的报警类别（config 的 video.models[<id>].alarm_class），空=只画框不报警。
    // 与 slotClassNames_ 同下标；随结果一起传进 AlarmManager::ingest 的 SlotContext，
    // 不再写进 AlarmManager 单例的全局槽表（按通道配置后那张表会被别的通道覆盖）。
    // **唯一允许运行期改写的槽位向量**：写方是 GUI 线程的 updateSlotAlarmClasses()，
    // 读方是解码线程每帧的 SlotContext 构造，故由 alarmClsMtx_ 保护（只锁这一次拷贝，
    // 与模型加载/NPU 调用完全不重叠）。其余三个向量运行期只读。
    std::vector<QString> slotAlarmClasses_;
    std::mutex alarmClsMtx_;                 ///< 只保护 slotAlarmClasses_ 的读写

    // releaseTasks() 里"推理线程未确认退出所以不敢 delete"的任务转入这里（见其注释）。
    // 只在 NPU 卡死导致 stopBestEffort 超时 detach 的异常路径上增长，每次一条告警。
    std::vector<PpeTask*> quarantine_;

    // 见 InferenceMode：false 时本对象只做"解码 + 显示 + 取帧"，
    // tasks_/slotQueues_/dmaBufferPool_ 全都不分配。
    bool inferenceEnabled_ = true;

    // ===== 检测框时序平滑 =====
    // 每 3 帧才做一次推理，非推理帧直接复用上一轮结果会导致框"跳"。
    // BoxSmoother 维护浮点精度的显示坐标，每帧向最新检测目标做指数平滑，
    // 视觉上框匀速滑到新位置而非瞬移。仅影响绘制，报警/围栏仍用原始坐标。
    struct BoxTrack {
        float dx, dy, dw, dh;
        float tx, ty, tw, th;
        int cls_id = -1;
        float prop = 0;
        int fade = 0;
    };
    struct BoxSmoother {
        std::vector<BoxTrack> tracks;
    };
    std::vector<BoxSmoother> boxSmoother_;

    // ===== 围栏事件表（进入沿触发）=====
    // 每个元素 = 一次进行中的围栏违规事件，记最近一次命中时的框与时刻。
    //   框用模型 640 空间（与围栏候选的 det.box 同口径，跨帧可直接算 IoU），
    //   时刻用 steady_clock 纳秒（只与自身做差，不与报警里的 epoch 时间混用）。
    // 命中已有事件 → 只续期，不再报警；匹配不上 → 视为"刚进入违规状态"，报警并新建事件；
    // 连续 kFenceTrackKeepNs 没被命中 → 认为人已离开，事件结束，再进来算新事件。
    // 只被本通道解码线程访问，无需加锁（与 boxSmoother_ 同一口径）。
    //
    // role 是这张表能同时管两种事件的关键：闯入（禁区）与离岗（岗区域）是两个
    //   独立事件，IoU 匹配时必须先比角色再算重叠 —— 同一个人的闯入 track 若被
    //   他的离岗命中"续期"掉，此后这条通道的离岗报警会永久静默，且不报错。
    struct FenceInsideTrack {
        float x, y, w, h;
        long long lastHitNs = 0;
        geofence::FenceRole role = geofence::FenceRole::Forbidden;
    };
    std::vector<FenceInsideTrack> fenceTracks_;

    // ===== 视频录制 =====
    // 硬件编码环形缓冲区录制器：用 shared_ptr 持有，ClipWriter 的延迟 dump
    // 任务各持一份引用，因此本对象析构不会让后台线程摸到已释放的编码器。
    std::shared_ptr<VideoRecorder> videoRecorder_;
    RecorderConfig recordingCfg_;                  // 录制参数（start() 前注入）

    // ===== 帧捕获（供智能识别 QR/OCR 按需取帧）=====
    // 解码线程每帧在 emit frameReady 之后更新，消费者只读、不入队、不复制。
    // ptr 指向 dst_bufs[back_buf] 的 DRM mmap 区域：仅解码线程写，且只有双缓冲，
    // 所以一次取用必须在接下来约两帧内读完（约束写在 captureCurrentFramePtr 上）。
    // stop() 里统一置空，避免解码线程退出后消费者摸到已释放的缓冲区。
    mutable std::atomic<int> captureW_{0};
    mutable std::atomic<int> captureH_{0};
    mutable std::atomic<int> captureStride_{0};
    mutable std::atomic<const uint8_t*> capturePtr_{nullptr};
};

#endif
