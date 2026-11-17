#ifndef FENCE_FRAME_RENDERER_H
#define FENCE_FRAME_RENDERER_H

// ============================================================================
// fence_frame_renderer.h — v1 §5.2 那句"边缘设备渲染好全部区域的一张合成生效画面 PNG"
// ============================================================================
//
// §5.3 对这张图的要求是硬的：`frameUrl` **必填**，缺失时 Pad 按失败处理
// （提示"边缘设备未返回生效画面"），并且明写"不本地伪造画面"。
// 所以 apply 这一步必须真产出一个文件、并且能通过媒体服务取到，否则就回 503/3003。
//
// 底图两档（2026-11-17 用户裁定）：
//   - 传了 background（非空 QImage）→ 真实画面等比居中铺底，围栏叠在上面。
//     apply 产出的生效画面走这一档：管理页从通道解码器抓帧传入，HTTP apply
//     路由经 setFrameProvider() 注册的回调按 cameraId 取帧（回调由 UI 域注入，
//     biz 不反向依赖 ui；路由跑在 GUI 线程，取帧与画布抓帧同一条安全路径）。
//   - 没传 → 深色底 + 十等分网格。草稿预览走这一档（草稿没有生效画面，
//     黑底上画区域即可）；取不到帧时的 apply 也退到这里，不伪造画面。
//
// 归一化坐标到像素的换算按**底图内容矩形**走：传了 background 时是它 letterbox
// 居中后那块（扣黑边），没传时是满幅画布（1280×720）。与视频页 overlay /
// 编辑画布的 contentRect 口径一致——各算各的会让示意图围栏与现场位置分家。
// 这与 fence_shape.h 那套"kFenceNormBase=10000、按视频内容矩形"的口径**是两件事**：
// 那套服务于本机自己的围栏绘制与判定，这套服务于 v1 的 0~1 坐标。
// ============================================================================

#include <QImage>
#include <QString>
#include <QVector>

#include <functional>

#include "pad_fence_store.h"   // PadRegion

namespace geofence {

/// 合成画面的画布尺寸（16:9，与现场四路的主流分辨率一致；固定值，不做配置）
constexpr int kFrameWidth = 1280;
constexpr int kFrameHeight = 720;

/**
 * @brief 在内存里渲染一张围栏画面（不落盘）
 * @param regions    区域数组（RECT 按外接矩形画，与顶点存储顺序无关）
 * @param background 底图帧；空图 = 深色底 + 十等分网格（草稿预览档）
 *
 * 管理页的草稿预览直接用它出黑底图；renderFenceFrame 也经它出落盘图，
 * 两处的围栏画法因此只有一份。
 */
QImage renderFenceImage(const QVector<PadRegion> &regions, const QImage &background = QImage());

/**
 * @brief 注册"按 cameraId 取当前帧"的回调（UI 域在装配时注入，进程级一次）
 *
 * HTTP apply 路由跑在 GUI 线程（PadHttpServer 的 readyRead 同步分派），
 * 回调里走通道解码器 captureCurrentFramePtr 取帧是安全的。未注册/取不到
 * 返回空图，渲染退回网格底。cameraId 口径 = "CH1".."CH4"（§7.1）。
 */
void setFrameProvider(std::function<QImage(const QString &cameraId)> provider);

/// 经注册的回调取一帧；没注册或回调返回空图时给空图。
QImage frameForCamera(const QString &cameraId);

/**
 * @brief 把一个版本的 regions 渲染成一张 PNG 并落盘
 * @param mediaRootDir 媒体服务的根（**绝对路径**，就是 HttpFileService 的 rootDir）
 * @param subDir       根之下的子目录，v1 §9 的写法是 "fence-frames"
 * @param absPathOut   [out] 写成功的绝对路径
 * @param relPathOut   [out] 相对 mediaRootDir 的路径（**库里存这个**，出 URL 时再拼前缀）
 * @param err          [out] 中文失败原因
 * @param background   底图帧（apply 时刻的真实画面）；空图 = 网格底
 * @return true = 文件已经存在且可读
 *
 * 落盘形状固定为 `<root>/<subDir>/<fenceId>/v<version>.png`，与 §9 表里那条
 * `GET /fence-frames/{fenceId}/v{n}.png` 一一对应。
 * ⚠ fenceId 直接来自 URL 路径参数，调用方**必须先按 §5 的字符集校验过**再传进来，
 *   本函数不做二次过滤（过滤点只该有一处，见 pad_http_fences.cpp 的 isSafeFenceId）。
 * 同名文件**覆盖**：同一个版本被重复下发时画面要跟着最后一次走，
 * 而留下旧文件会让 Pad 显示的围栏和实际生效的不是同一份。
 */
bool renderFenceFrame(const QString &fenceId, int version, const QVector<PadRegion> &regions,
                      const QString &mediaRootDir, const QString &subDir,
                      QString *absPathOut, QString *relPathOut, QString *err,
                      const QImage &background = QImage());

}  // namespace geofence

#endif  // FENCE_FRAME_RENDERER_H
