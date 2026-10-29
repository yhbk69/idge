#ifndef MEDIA_PAYLOAD_H
#define MEDIA_PAYLOAD_H

/**
 * @file media_payload.h
 * @brief 把落盘的报警媒体读成"待发字节"：等文件写稳 → 读全量 → 算 sha256
 *
 * 为什么单独一个文件：这三步是纯文件操作，不碰 socket、不碰 Qt 对象，
 * 所以既能被 MediaPump 的后台线程调，也能被离线自检直接调（不用起线程、
 * 不用事件循环）。协议侧的分块换算在 alarm_uplink_engine.h 的 planChunks。
 *
 * 为什么要"等写稳"：抓拍图与片段的落盘都不是原子写（write_data_to_file 直接写目标路径），
 * 报警那一刻提交、几十毫秒后才写完。立刻读会读到半张 JPEG——字节数与 sha256
 * 都是错的，而对端只能靠这两个值判完整性。判据是"连续两次采样 size 相同且 >0"。
 */

#include <QByteArray>
#include <QString>

namespace uplink {

/// 一段媒体的读盘结果（交回 GUI 线程后再按 chunk_size 切块）
struct MediaPayload {
    QString mediaId;       ///< 与 alarm_info 里预告的那个一致
    QString alarmId;       ///< 归属报警
    QString kind;          ///< snapshot | clip
    QString path;          ///< 本机落盘路径
    QString name;          ///< 真实文件名（media_begin 用它覆盖 alarm_info 里的猜测）
    QString mime;          ///< 由真实扩展名换算
    QByteArray data;       ///< 整个文件的字节
    QString sha256Hex;     ///< data 的 sha256，十六进制小写
};

/**
 * @brief 等文件写稳，然后读全量并算哈希（**阻塞**，只在后台线程调用）
 * @param timeoutMs 等多久；超时就返回 false（截图链路挂了、片段被淘汰都属正常情况）
 * @param outError  失败原因，只写 media_id 与路径，绝不含文件内容
 * @return false = 超时没出现 / 读失败 / 空文件；这三种都不重试（文件不会再自己变好）
 */
bool loadMediaPayload(const QString &mediaId, const QString &alarmId, const QString &kind,
                      const QString &path, const QString &mime, int timeoutMs,
                      MediaPayload *out, QString *outError);

/// 由真实文件名换算 MIME：裸流扩展名 h265/h264，容器 mp4，图 jpg
/// ClipWriter 的编码器会退档（hevc→h264），所以只能按实际落盘的后缀算
QString mimeForMediaPath(const QString &path, const QString &fallback);

}  // namespace uplink

#endif  // MEDIA_PAYLOAD_H
