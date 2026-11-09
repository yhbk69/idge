#pragma once

// ============================================================================
// alarm_media_prune.h — 报警媒体按天目录的裁剪判据（可注入、可离线跑）
// ============================================================================
//
// 【为什么要从 AlarmManager 里拆出来】原来这是一段 static 函数，root 走
// RuntimePaths::alarmsDir()、上限是 4GB 常量、"今天"取墙钟——三个都写死，于是
// "超上限时删哪些、留什么"这条判据在板上**没法验**：造 4GB 文件不现实，
// 而改动它的后果恰恰是"三周前的报警点开有没有图"这种只有事后才发现的问题。
// 拆成 (root, daysToKeep, maxBytes, today) 四个入参之后，离线用例能精确造出
// "两天前 + 超上限"那种组合，一次跑完。AlarmManager 那边只剩取路径与打日志。
//
// 【目录形状约定】root 下每**一天**一个子目录，目录名是 yyyyMMdd（写侧是
// ffmpeg_video_decoder 的 SnapWriter/ClipWriter，路径口径见 RuntimePaths::alarmsDir）。
// 认不出这个形状的目录一律不碰——现场有人手工往 data/alarms 里放东西是常事。
// ============================================================================

#include <QDate>
#include <QString>

namespace alarm_media {

/// 一次裁剪的统计，供调用方打日志、也供离线用例直接断言（不必再去猜文件系统状态）
struct PruneResult {
    int dayDirsScanned = 0;       ///< 认出来的按天目录数
    int dayDirsRemoved = 0;       ///< 整目录删掉的（过期归档，或上限兜底）
    int bigFilesRemoved = 0;      ///< 只删大文件的那一档里清掉的文件数
    int thumbnailsKept = 0;       ///< 同上一档里保住的 sidecar 缩略图数
    qint64 bytesFreed = 0;        ///< 释放的字节数（两个档都算在这里）
};

/**
 * @brief 按"天数 + 容量"两道闸门裁剪报警媒体目录
 * @param root       媒体根（RuntimePaths::alarmsDir()）
 * @param daysToKeep 保留天数，与 DB 的 cleanOldAlarms 同口径；内部按至少 1 天处理
 * @param maxBytes   容量上限，超过就从最旧的一天开始端；<=0 表示不设容量闸门
 * @param today      "今天"，只影响过期判据；默认墙钟，离线用例传固定日期才稳
 *
 * 两道闸门的分工（顺序也是有意的：先按天，再按容量，容量那一档才不会被过期目录占额度）：
 *   1) 过期：目录日期 < today-daysToKeep → **整目录删**。
 *      DB 行同一天一起没，留着没人指向的文件才是白占盘。
 *   2) 超上限：从最旧开始**只删大文件、保住 *_thumb.jpg**。
 *      sidecar 是 64x48 的 JPEG（一千多字节），与整幅抓拍 + 片段差两个数量级；
 *      而报警行要留 30 天，全删的结果就是列表里那条还在、点开却"无截图"。
 *      只有连缩略图都还超上限，才回头整目录删——上限是硬约束，分区写满会拖垮落库与检测。
 */
PruneResult pruneDayDirs(const QString &root, int daysToKeep, qint64 maxBytes,
                         const QDate &today = QDate::currentDate());

/// sidecar 判据：与写侧 qt_image_utils.h 的 thumbnailSidecarPath 同一个后缀。
/// 单独暴露是因为详情框的"整幅没了回退缩略图"那条路径也认它，两边必须一条判据。
inline bool isThumbnailSidecar(const QString &fileName)
{
    return fileName.endsWith(QLatin1String("_thumb.jpg"), Qt::CaseInsensitive);
}

}  // namespace alarm_media
