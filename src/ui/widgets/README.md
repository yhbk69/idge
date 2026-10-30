# src/ui/widgets

> 所属域：**ui 界面域**。原 `src/ui/widgets` 模块目录，S4e 域分组时更名 `widgets`
> （避免与域目录同名），承载可复用控件与 EGL/GL 渲染。

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
| alarm_list_widget.h/.cpp | AlarmListWidget：类别/围栏双 Tab 报警表，300ms 防抖刷新与筛选；围栏 Tab 第 4 列「围栏类型」区分**闯入禁区/离岗** |
| video_alarm_widget.h/.cpp | VideoAlarmWidget：视频监控页右侧报警卡片列表（QScrollArea），点击跳转详情 |
| alarm_detail_dialog.h/.cpp | AlarmDetailDialog：报警详情对话框，支持误报标记与单条确认；"视频"行展示片段文件名/大小与网页副本是否随行 |

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
- **`PlayerWidget` 的占位件是"这一路到底死在哪一环"的唯一入口**（2026-10-30）：
  `placeholder_` 是个 `QLabel`，加在**同一个 grid cell (0,0)、`fenceOverlay_` 之后**（浮在视频上），
  `WA_TransparentForMouseEvents` —— 否则既挡 ⊕ 放大也挡围栏绘制。文案优先级在 `updatePlaceholder()` 里单点定义：
  `未启用` → `视频待接入（未配置地址）` → `接入失败：<msg>` → `信号中断` → `正在接入…` → 出帧即隐藏。
  两条依赖是刻意的，别顺手"优化"掉：
  ① `setChannel(i)` 必须**早于**按 `channelEnabled/channelSource` 开流那一段
  （`frmMain::initForm()` 现在就是这个顺序），这样"未启用/没地址"在 `setChannel()` 里就能查出来，
  不依赖额外信号；② `decoder_->error` 那条 lambda 以前只 `qWarning`，现在既 warning 又刷占位，
  存的是 `msg.section('\n',0,0).left(80)` —— **不打印媒体字节、不 `fromUtf8` 碰二进制**。
- **`statusChanged(ch,0)` 会在本地 mp4 播完时如实报"信号中断"，这不是 bug**；
  而画面右上角的 `FPS: 25.3` 在这种时候是**陈旧的**——`currentFps_` 只在有帧的那 1 秒窗口里更新，
  EOF 后冻结在最后一个值。别拿它当"流还活着"的证据（本轮误判过一次，占位文案其实是对的）。
- **触屏命中区**：本目录的控件尺寸同样走 `src/ui/core/README.md`「触屏命中区设计令牌」那套档（10 寸屏、1920x1200 实测）。
  两处值得单独记：`PlayerWidget::expandBtn_` 是 T4 固定 48x48，其内联 QSS **必须显式 `padding: 0px`**——
  全局 `.QPushButton` 现在是触屏档 `padding-left/right:14px`，48px 的固定钮被它吃掉后 `⊕` 需要 50px 只剩 46px（探针实测截字）；
  报警侧栏卡片行 `setFixedHeight(104)` 是在 `.ui` 锁死的 280px 列宽内定的，加高可以、加宽不行，改前先看列宽。
- **围栏 Tab 的筛选靠 `isFenceAlarm`（`alarmType` 前缀 `fence`）**：新增的 `fence_in`/`fence_out` 与
  历史数据里裸的 `"fence"` 都要落进同一个 Tab，判据一旦改成精确相等，重启回读的老围栏报警会
  掉进「类别」Tab 显示成"person 检测到报警"。第 4 列的文字按 `fence_out` 分「离岗」，其余围栏出「闯入禁区」。
- **`contentRect()` 是围栏层的唯一几何真值**：GLVideoWidget 里"视频实际占了哪块 widget 像素"只有一处算式
  （`aspectScale()` 给 sx/sy，`drawQuad()` 摆顶点和 `contentRect()` 算画面块共用它），
  所以渲染与围栏映射不可能各写一边再漂移。paintGL 与 resizeGL 末尾各调一次 `notifyContentRect()`，
  判重后才 emit `contentRectChanged` —— 30fps x 4 路不能每帧发事件。
  `video_width_/height_` 只在 GUI 线程被 paintGL 写，`contentRect()` 也只在 GUI 线程读，**不拿 `mutex_`**
  （拿锁会在 paintGL 末尾形成"通知时又回头抢自己"的重入路径）。
  无帧或宽高比取不到时退化为整个 widget（铺满、无黑边）。
- **线程约束**：一切 GL/EGL 调用只能发生在 widget 的 GUI 上下文 current 期间（initializeGL/paintGL 内）；跨线程调用 EGLImage 或纹理是 UB。fd 可从任意线程 dup 后投递。
- **dup 所有权协议**：`EglFramePool::acquire` 与 `RenderFrame` 均持有 dup 副本并负责唯一一次 close；原 fd 归解码侧。EGLImage 建立后与原 fd 解耦（内核各持引用）。
- EglFramePool 导入失败槽位仍被占用（in_use 未回滚 + dup fd 泄漏，4 槽漏完池失效）；仅支持单平面格式，勿传 NV12；无析构，GL 对象靠上下文销毁兜底。
- GLVideoWidget 每帧创建/销毁 EGLImage（未接池，高帧率热点）；paintGL 中原生 GL 与 QPainter 混用未走 beginNativePainting 交接，改动需谨慎；widget 析构与在途信号竞态会漏收 fd——须先停解码器。
- 每帧 `eglCreateImageKHR` 走驱动 ioctl，4 通道 25fps 下注意驱动开销与 fd 表压力。
- **报警详情对话框的"视频"行是尽力而为的快照**：片段在报警后 2 秒才由 ClipWriter 落盘，
  `videoPath` 由 `AlarmManager::attachVideoClip` 事后回填且**不发信号**，对话框只在打开时
  读一次 `alarms()`。所以刚报警立刻点开可能显示"未生成"；片段文件已被按天/容量清理时显示
  "已清理"。要看到路径请等 2 秒后再打开。
- **录制参数只在 `PlayerWidget::open()` 注入一次**：编码器在解码线程打开流后按真实宽高创建，
  运行期改 `config.json` 的 recording 段不会重建编码器，必须换流/重启才生效（界面不提供入口）。
- EglImageRenderer：无 include guard、着色器 uniform 未赋值（tex_height/img_height 默认 0 → 除零 NaN），且 BT.601 假设与 RGBA 主路径不符——仅存档参考。
- 业务界面：报警时间由 `AlarmRecord::alarmTime`（ISO 字符串）解析展示；`timestamp` 字段单位口径见 yolo11 README 警示；通道筛选硬编码 4 路（filterChannel_ 0~3）；Dashboard CPU 差分基线为函数级 static 变量（多实例互相污染）。
