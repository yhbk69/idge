# src/media/reader

> 所属域：**media 视频管线域**（目录重组 S4b 迁入）

## 功能概述

视频/摄像头解码与检测调度层。包含三条链路：

1. **主流水线**（`FFmpegVideoDecoder`）：RTSP/文件 → FFmpeg `h264_rkmpp` 硬解出 NV12 DMA-BUF → RGA 转 RGBA 画框 + RGA 缩放 640x640 送 PpeTask（RKNN NPU YOLO11）推理 → 结果回填后 EGL 渲染上屏；画完框的 RGBA 帧还有一路硬件消费者——`VideoRecorder` 硬编环形缓冲（报警片段）；
2. **摄像头预览**（`CameraPreviewDecoder`）：v4l2 设备（MJPEG）→ `mjpeg_rkmpp` 硬解（失败回退 `avcodec` 软解 + sws 转 RGB）→ 同步跑 SCRFD 人脸检测 + 异步跑 PpeTask → CPU 画框 → 帧信号上屏；同时缓存 JPEG 原始码流供抓拍**零重编码**直存；
3. **人脸检测器**（`ScrfdFaceDetector`）：RKNN 版 SCRFD，anchor-free 解码，输出框 + 5 关键点。

## 文件清单

| 文件 | 职责 |
|---|---|
| `ffmpeg_video_decoder.h/.cpp` | 主流水线解码总控：硬解、零拷贝 DMA-BUF 借还、级联多模型任务分发、结果合成画框、帧限速（截止期节拍：按流 avg_frame_rate 定间隔，落后重锚丢弃连发）、报警片段录制挂钩、**帧捕获槽**（供智能识别按需拉最新帧） |
| `video_recorder.h/.cpp` | `VideoRecorder`：画框后的 RGBA → RGA → rkmpp 硬编（hevc/h264）→ Annex-B 环形缓冲区；报警时 dump 裸流/MP4 片段 |
| `camera_preview_decoder.h/.cpp` | 摄像头预览解码：v4l2/MJPEG、JPEG 魔数缓存抓拍、SCRFD 同步检测、PpeTask 异步检测、CPU 画框 |
| `scrfd_face_detector.h/.cpp` | SCRFD 人脸检测器（RKNN NPU）：letterbox 预处理、9 输出解码、类别内 NMS |

## 核心类与数据流

```
FFmpegVideoDecoder（QObject，独立 QThread）
  av_read_frame → AVPacket(h264) → avcodec(MPP) → AVFrame(DRM_PRIME NV12, fd)
      ├─ fd → RGA: NV12→RGBA（显示缓冲，双缓冲池借出）
      └─ fd → RGA: NV12→缩放 letterbox 640x640 RGB888（image_buffer_t，
                dmaBuffer 借自 dmaBufferPool_）
                    → tasks_[k]->put(TaskData{time=epoch纳秒, image, resultQueue})
                    → PpeTask 推理 → resultQueue.push(od_results)
  解码线程 tryPop 各任务结果 → 坐标还原/画框进 RGBA → emit frameReady(RenderFrame)
  （RenderFrame 携带 dup 出的 fd：所有权移交接收方，接收方负责 close）

CameraPreviewDecoder（QObject，std::thread decodeLoop）
  v4l2 DQBUF → AVPacket(MJPEG) → 缓存 0xFFD8 起始原始码流（capture() 直存抓拍）
      → 硬解 NV12（vstride 由 planes[1].offset-planes[0].offset / pitch 反推）
      → 软解路径: sws 转 BGR888 / 硬解路径: RGA→CPU memcpy 得 RGBA
      → 每 5 帧: ScrfdFaceDetector::detectRgba（同步，解码线程内）
              + PpeTask::put（异步，结果队列多路复用按 od_results.id 区分）
      → drawDetectionBoxes（CPU 画框进 mmap 像素）→ emit frameReady

ScrfdFaceDetector（final，禁拷贝）
  RGBA → letterbox(左上对齐黑边)→ (px-127.5)/128 float NHWC 640x640
      → rknn_run → 3 层级 [score/bbox/kps]×8/16/32 解码 → 阈值过滤 → NMS
```

### 报警片段录制（VideoRecorder，全程不过 CPU）

```
config.json recording 段 → PlayerWidget::open() 组装 RecorderConfig
  → FFmpegVideoDecoder::setRecordingConfig()（start 前注入，只存参数不分配资源）
decodeLoop 打开流后：用 dec_ctx 的实际宽高 + 流的 avg_frame_rate(退化取 r_frame_rate)
  → std::make_shared<VideoRecorder>(rc) → init(w,h)：
      rkmpp hwdevice + DRM_PRIME/NV12 帧池 + hevc/h264_rkmpp
      + avcodec_parameters 快照（供后台 dump remux 用）
  （init 失败 → reset() + qWarning，只关录制，主流程不受影响）
每帧画完框（RGBA DMA-BUF 已含检测框）：
  videoRecorder_->encodeFrame(dst_buf->fd(), w, h, dst_buf->stride()/4 /*像素口径*/, 墙钟ms)
    → RgaConverter::rgba_to_nv12(src fd → 池帧 NV12 fd)
    → avcodec_send_frame(DRM_PRIME 池帧) → collectPackets() 收 Annex-B 包入环
    → 环满 drop-front（容量 = bufferSeconds × fps）
报警出片段（ClipWriter 后台线程，与 SnapWriter 同一套范式）：
  报警分支 storeAndNotify() 之后 submitAlarmClip(videoRecorder_, ch, cls, ids, 墙钟ms)
    → 路径 data/alarms/yyyyMMdd/ch{N}_{类别}_{HHmmss_zzz}.{h265|h264}（+ 同名 .mp4）
      扩展名跟 recorder->encoderName() 走（退档成 h264_rkmpp 时不会误标 .h265）
    → 任务带 due = 报警时刻 + kClipPostMs(2s)，到点才取窗口：
      "前 3 秒 + 后 2 秒"里的后 2 秒在报警那一刻还没发生
    → dumpAnnexB(上传用裸流) + dumpMp4(网页副本，同批包只加封装不重编)
    → AlarmManager::attachVideoClip(ids, 裸流路径) 回填内存 + AlarmDAO::updateVideoPath
  队列上限 8，积压时丢最旧（队首窗口的帧早被环覆盖，留着也是残缺）
```

`shared_ptr` 的所有权约定：片段是"报警后 2 秒"才做的延迟任务，ClipJob 自带一份
`shared_ptr<VideoRecorder>`，所以解码线程停止、`FFmpegVideoDecoder` 析构都不会让
后台 dump 摸到已释放的编码器。`VideoRecorder` 因此不再是 QObject（无 parent 语义）。

- 起点自动对齐到窗口内第一个关键帧，故实际时长可能短于 `pre+post`；GOP 默认 1s，误差 ≤1s。
- 每个 IDR 前编码器已内嵌 VPS/SPS/PPS，裸流窗口自足可解码，dump 侧不需要补参数集。
- 池帧借不到时先排空编码器再重试一次，仍失败就丢该帧并计入 `droppedFrameCount()`，绝不阻塞解码线程。

## 使用方法

```cpp
// 主流水线（config 指向含模型路径的 config.json，内部 buildCascadeTasks）
auto* decoder = new FFmpegVideoDecoder(this);
decoder->setChannel(0);
connect(decoder, &FFmpegVideoDecoder::frameReady, this, &MyView::onFrame);
decoder->start("rtsp://192.168.1.10/stream1");
decoder->stop();               // 置 running_=false + interrupt_callback 打断阻塞读帧

// 帧捕获槽：智能识别页（QR/OCR）在自己的线程上"拉最新一帧"，不占队列、不拷贝
const uint8_t* px = decoder->captureCurrentFramePtr();   // RGBA 的 DRM mmap 指针
int w, h, stride;
if (px && decoder->getCurrentFrameInfo(w, h, stride)) {  // 无帧时 ptr=nullptr、返回 false
    // 立刻读完/拷走：只有双缓冲，约 2 帧后这块就被下一帧覆盖
    for (int y = 0; y < h; ++y) memcpy(dst + y * w * 4, px + (size_t)y * stride, (size_t)w * 4);
}
// stride 是**字节**跨度（≠ w*4，RGA 会按对齐放大），QImage 构造要按它逐行拷

// 摄像头预览 + 抓拍
CameraPreviewDecoder preview;
preview.setDetectorConfigs({{.model_path=..., .npu_core_mask=RKNN_NPU_CORE_0, ...}});
preview.start("/dev/video41");
preview.capture("/data/capture/snap_001.jpg");   // 直接落盘缓存的 JPEG 原始码流

// SCRFD 人脸检测（须先 init，绑定单线程使用）
ScrfdFaceDetector det;
det.init("/app/models/scrfd_32g.kmodel", RKNN_NPU_CORE_2);
std::vector<ScrfdFaceBox> faces;                 // {cv::Rect rect; float score;}
det.detectRgba(rgbaPixels, width, height, stride /*>=width*4*/,
               faces, 0.5f, 0.4f);               // 返回 false=链路失败而非"没人"
```

## 依赖关系

- **上游调用**：`src/ui/form/*`（UI 窗口持有解码器）、`main.cpp`；
- **下游依赖**：`src/base/buffer`（DmaBufferPool/DmaFrameBuffer）、`src/media/rga`（RgaUtils）、`src/ai/yolo11`+`src/ai/task`（PpeTask/TaskData）、`src/base/queue`（PriorityQueue）、FFmpeg(libav*) + MPP、RKNN runtime（SCRFD）、EGL（fd→纹理渲染）、Qt（信号槽）。

## 注意事项

- **录制只在硬编可用时开启**：`init()` 不回落 CPU 软编——4 路 1080p 的 x264 会把 CPU 吃满、
  直接拖垮检测与显示，"宁可不录也不能为录制牺牲主流程"。因此 `droppedFrameCount()` 持续增长
  或日志出现 `recording disabled` 时，应先看 VPU/RGA 负载而不是改码率。
- **RGBA stride 必须换算成像素再传**：`DmaFrameBuffer::stride()` 是字节 pitch，而 RGA 的
  `wstride/hstride` 单位是像素/行（详见 `src/media/rga/README.md`），RGBA 要 `/4`。
  传错时内核会报 "required ... byte but only got ..."，`VideoRecorder::encodeFrame()`
  入口另有 `wstride` 合理性检查（`<w` 或 `>2w` 直接判为误传并丢帧）。
- **模型输入约定**：PpeTask 收到的是 RGA letterbox 后的 640x640 RGB888 DMA-BUF，坐标还原靠 `letterbox_t{x_pad,y_pad,scale}`；SCRFD 是左上对齐黑边填充，逆变换只除 scale。
- **fd 所有权**：`RenderFrame` 的 fd 为 `dup()` 产物，接收方必须 `close()`；AVFrame DRM fd 归 ffmpeg，禁止关闭；`pHWDeviceCtx` 存在 1 个 buffer 引用泄漏（已登记）。
- **帧捕获槽是"看一眼"，不是帧交付**：`captureCurrentFramePtr()` 返回的是解码线程正在轮流写的双缓冲之一，
  指针本身在解码器运行期间一直有效，但**内容约 2 帧后就被覆盖**，取到必须当场读完；调用方（智能识别页）
  负责"先停自己的取帧线程、再 `decoder->stop()`"，`stop()` 会把指针置 nullptr 作为兜底。
  需要跨线程安全交付帧请走 `frameReady` 信号（自带 dup fd）或 `src/base/queue/latest_frame_slot.h`，别拿这个接口当队列用。
- **线程约束**：ScrfdFaceDetector 的 `rknn_context` 非线程安全，仅在解码线程内使用；`stop()` 后 `tasks_` 故意不 delete（detach 线程可能仍在回调，防 UAF）；`dmaBufferPool_` 借用遵循"最后一个引用释放自动归还"，新增提前 return 必须保证归还。
- **已知隐患**（详见各文件头注释）：
  - `image_buffer_t::time`/`TaskData::time` 实际为 **epoch 纳秒**，而 `common.hpp` 文档写毫秒、`src/biz/alarm` 限流常量按纳秒比较——单位三方不一致，勿随手"换算"；
  - EOF drain 分支仅 unref 尾部 packet，可能丢最后几帧；
  - `camera_preview_decoder` 的 `ppeTasks_` 生命周期策略与主流水线"只停不删"不同，退出时序敏感；
  - EAGAIN 忙等依赖 v4l2 阻塞模式，非规范做法（性能可接受，已注释登记）。
