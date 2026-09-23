# src/ui

## 功能概述（在流水线中的位置）
零拷贝渲染终点与业务界面前端。上游（FFmpeg 硬解 → RGA 转 RGBA 的 DMA-BUF fd）经 Qt 队列信号送入本模块，由 EGL `EGL_EXT_image_dma_buf_import` 把 fd 直接导入为 GLES 纹理绘制——像素全程不落主存；同时提供数据看板、报警列表、播放窗体等业务界面。

## 文件清单
| 文件 | 职责 |
|---|---|
| egl_dma_buf.h | EglDmaBufImporter：扩展检查/函数指针获取、NV12 双平面与 RGB modifier 的通用 EGLImage 导入 |
| eglimage_helper.h | EglImageHelper：importRGBA/importRGB/importNV12 便捷封装 + 纹理绑定/销毁 |
| EglFramePool.h | EGL 帧槽对象池：4 槽纹理复用，acquire 时 dup(fd) 建立 GPU 侧独立引用 |
| EglImageRenderer.h | 备用单纹理渲染器（NV12 BT.601 着色器版；无 include guard，遗留实现） |
| gl_video_widget.h/.cpp | GLVideoWidget：QOpenGLWidget 双缓冲零拷贝视频控件（含 FPS 叠加） |
| player_widget.h/.cpp | PlayerWidget：视频 + OSD + 围栏叠加 + 放大按钮的通道窗体，持有解码器 |
| dashboard_widget.h/.cpp | DashboardWidget：统计卡片、/proc 系统监控（CPU/内存/温度）、类别排行条 |
| alarm_list_widget.h/.cpp | AlarmListWidget：类别/围栏双 Tab 报警表，300ms 防抖刷新与筛选 |
| video_alarm_widget.h/.cpp | VideoAlarmWidget：视频监控页右侧报警卡片列表（QScrollArea），点击跳转详情 |
| alarm_detail_dialog.h/.cpp | AlarmDetailDialog：报警详情对话框，支持误报标记与单条确认 |

## 核心类与数据流
帧流：解码线程 dup(RGBA fd) → `RenderFrame` 经 `QueuedConnection` 投递 → GLVideoWidget::onFrameReady（互斥锁保护，prev/pending 两级 fd 管理）→ paintGL 中 `importRGBA → bindToTexture` 写入后台缓冲 → 双缓冲交换 drawQuad。EGL 引用计数保证"fd 已 close、画面仍可读"的重叠窗口。fd 生命周期：每个副本恰好一次 close（帧更替或析构）。界面流：AlarmManager 信号 → 防抖定时器 → 全表重建；QTimer 2s → DashboardWidget::refreshStats。

## 使用方法
```cpp
#include "gl_video_widget.h"
auto* w = new GLVideoWidget(parent);
connect(decoder, &FFmpegVideoDecoder::frameReady,
        w, &GLVideoWidget::onFrameReady, Qt::QueuedConnection);
// 解码线程侧（fd 必须是 dup 的副本）：
RenderFrame rf{ ::dup(rgba_fd), width, height, stride };
emit frameReady(rf);
// 纹理池版本（高频路径）：
GLuint tex = pool.acquire(fd, w, h, stride, DRM_FORMAT_ABGR8888);
// ...绘制... pool.release(tex);
```

## 依赖关系
- 依赖 EGL/GLES2（Mali）、drm_fourcc、Qt Widgets/OpenGL；buffer（DMA-BUF 约定）、rga（格式与 stride 来源）、alarm/fence 管理器等业务模块。

## 注意事项
- **线程约束**：一切 GL/EGL 调用只能发生在 widget 的 GUI 上下文 current 期间（initializeGL/paintGL 内）；跨线程调用 EGLImage 或纹理是 UB。fd 可从任意线程 dup 后投递。
- **dup 所有权协议**：`EglFramePool::acquire` 与 `RenderFrame` 均持有 dup 副本并负责唯一一次 close；原 fd 归解码侧。EGLImage 建立后与原 fd 解耦（内核各持引用）。
- EglFramePool 导入失败槽位仍被占用（in_use 未回滚 + dup fd 泄漏，4 槽漏完池失效）；仅支持单平面格式，勿传 NV12；无析构，GL 对象靠上下文销毁兜底。
- GLVideoWidget 每帧创建/销毁 EGLImage（未接池，高帧率热点）；paintGL 中原生 GL 与 QPainter 混用未走 beginNativePainting 交接，改动需谨慎；widget 析构与在途信号竞态会漏收 fd——须先停解码器。
- 每帧 `eglCreateImageKHR` 走驱动 ioctl，4 通道 25fps 下注意驱动开销与 fd 表压力。
- EglImageRenderer：无 include guard、着色器 uniform 未赋值（tex_height/img_height 默认 0 → 除零 NaN），且 BT.601 假设与 RGBA 主路径不符——仅存档参考。
- 业务界面：报警时间由 `AlarmRecord::alarmTime`（ISO 字符串）解析展示；`timestamp` 字段单位口径见 yolo11 README 警示；通道筛选硬编码 4 路（filterChannel_ 0~3）；Dashboard CPU 差分基线为函数级 static 变量（多实例互相污染）。
