# src/media/reader

> 所属域：**media 视频管线域**（目录重组 S4b 迁入）

## 功能概述

视频/摄像头解码与检测调度层。包含三条链路：

1. **主流水线**（`FFmpegVideoDecoder`）：RTSP/文件 → FFmpeg `h264_rkmpp` 硬解出 NV12 DMA-BUF → RGA 转 RGBA 画框 + RGA 缩放 640x640 送 PpeTask（RKNN NPU YOLO11）推理 → 结果回填后 EGL 渲染上屏；
2. **摄像头预览**（`CameraPreviewDecoder`）：v4l2 设备（MJPEG）→ `mjpeg_rkmpp` 硬解（失败回退 `avcodec` 软解 + sws 转 RGB）→ 同步跑 SCRFD 人脸检测 + 异步跑 PpeTask → CPU 画框 → 帧信号上屏；同时缓存 JPEG 原始码流供抓拍**零重编码**直存；
3. **人脸检测器**（`ScrfdFaceDetector`）：RKNN 版 SCRFD，anchor-free 解码，输出框 + 5 关键点。

## 文件清单

| 文件 | 职责 |
|---|---|
| `ffmpeg_video_decoder.h/.cpp` | 主流水线解码总控：硬解、零拷贝 DMA-BUF 借还、级联多模型任务分发、结果合成画框、帧限速（33333µs≈1/30s） |
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

## 使用方法

```cpp
// 主流水线（config 指向含模型路径的 config.json，内部 buildCascadeTasks）
auto* decoder = new FFmpegVideoDecoder(this);
decoder->setChannel(0);
connect(decoder, &FFmpegVideoDecoder::frameReady, this, &MyView::onFrame);
decoder->start("rtsp://192.168.1.10/stream1");
decoder->stop();               // 置 running_=false + interrupt_callback 打断阻塞读帧

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

- **模型输入约定**：PpeTask 收到的是 RGA letterbox 后的 640x640 RGB888 DMA-BUF，坐标还原靠 `letterbox_t{x_pad,y_pad,scale}`；SCRFD 是左上对齐黑边填充，逆变换只除 scale。
- **fd 所有权**：`RenderFrame` 的 fd 为 `dup()` 产物，接收方必须 `close()`；AVFrame DRM fd 归 ffmpeg，禁止关闭；`pHWDeviceCtx` 存在 1 个 buffer 引用泄漏（已登记）。
- **线程约束**：ScrfdFaceDetector 的 `rknn_context` 非线程安全，仅在解码线程内使用；`stop()` 后 `tasks_` 故意不 delete（detach 线程可能仍在回调，防 UAF）；`dmaBufferPool_` 借用遵循"最后一个引用释放自动归还"，新增提前 return 必须保证归还。
- **已知隐患**（详见各文件头注释）：
  - `image_buffer_t::time`/`TaskData::time` 实际为 **epoch 纳秒**，而 `common.hpp` 文档写毫秒、`src/biz/alarm` 限流常量按纳秒比较——单位三方不一致，勿随手"换算"；
  - EOF drain 分支仅 unref 尾部 packet，可能丢最后几帧；
  - `camera_preview_decoder` 的 `ppeTasks_` 生命周期策略与主流水线"只停不删"不同，退出时序敏感；
  - EAGAIN 忙等依赖 v4l2 阻塞模式，非规范做法（性能可接受，已注释登记）。
