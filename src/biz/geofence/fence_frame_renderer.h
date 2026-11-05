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
// ⚠ 这张图上**没有视频帧**，只有区域本身（深色底 + 十等分网格 + 多边形 + 序号）。
//   为什么不叠一帧真实画面：拿"当前帧"要碰解码器，而解码器实例归 UI 域
//   （src/ui/form/frmvideowindow.cpp 那边持有），biz/geofence 反向依赖 src/ui 是分层禁止的
//   （base ← media ← ai ← biz ← ui）。要叠帧就得加一条"渲染请求 → UI 取帧 → 回填"的异步链路，
//   那是另一轮的工作，已记进 src/biz/pad/README.md 的待确认清单。
//   这张图仍然满足规约的字面要求（"渲染好全部区域的一张合成 PNG"），
//   但现场用它核对围栏位置时只能靠网格估比例，**不能**当"围栏贴画面对不对"的依据 ——
//   这句必须让对方知道，所以也写进了 README。
//
// 归一化坐标到像素的换算按**画布**（1280×720）走，横轴除宽、纵轴除高，
// 与 fence_shape.h 那套"kFenceNormBase=10000、按视频内容矩形"的口径**是两件事**：
// 那套服务于本机自己的围栏绘制与判定，这套服务于 v1 的 0~1 坐标。
// 两套之间没有转换函数，也不该有——除非把 §5 的围栏接进检测（那才是本轮没做的缺口）。
// ============================================================================

#include <QString>
#include <QVector>

#include "pad_fence_store.h"   // PadRegion

namespace geofence {

/// 合成画面的画布尺寸（16:9，与现场四路的主流分辨率一致；固定值，不做配置）
constexpr int kFrameWidth = 1280;
constexpr int kFrameHeight = 720;

/**
 * @brief 把一个版本的 regions 渲染成一张 PNG 并落盘
 * @param mediaRootDir 媒体服务的根（**绝对路径**，就是 HttpFileService 的 rootDir）
 * @param subDir       根之下的子目录，v1 §9 的写法是 "fence-frames"
 * @param absPathOut   [out] 写成功的绝对路径
 * @param relPathOut   [out] 相对 mediaRootDir 的路径（**库里存这个**，出 URL 时再拼前缀）
 * @param err          [out] 中文失败原因
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
                      QString *absPathOut, QString *relPathOut, QString *err);

}  // namespace geofence

#endif  // FENCE_FRAME_RENDERER_H
