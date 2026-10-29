// ============================================================================
// media_payload.cpp — 报警媒体读盘（等写稳 → 全量读 → sha256）
// ----------------------------------------------------------------------------
// 三个函数没有 Qt 对象、没有线程、没有事件循环，纯文件 IO；调用方必须在后台线程，
// 因为等待是按毫秒 sleep 的。
// ============================================================================

#include "media_payload.h"

// kMime* 是协议字面量，集中在 alarm_uplink_engine.h 的 uplink 命名空间里
#include "alarm_uplink_engine.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QThread>

namespace {
/// 两次 size 采样的间隔。60ms 是拍脑袋的下限之上：SnapWriter 写一张 ~365KB 的
/// JPEG 通常十几毫秒，间隔太小会两次都读到"还在写"的同一个值
constexpr int kStableSampleMs = 60;
}

namespace uplink {

QString mimeForMediaPath(const QString &path, const QString &fallback)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg")) {
        return QString::fromLatin1(kMimeJpeg);
    }
    if (suffix == QLatin1String("h265") || suffix == QLatin1String("hevc")) {
        return QString::fromLatin1(kMimeHevc);
    }
    if (suffix == QLatin1String("h264") || suffix == QLatin1String("avc")) {
        return QString::fromLatin1(kMimeAvc);
    }
    if (suffix == QLatin1String("mp4")) {
        return QStringLiteral("video/mp4");
    }
    return fallback;
}

bool loadMediaPayload(const QString &mediaId, const QString &alarmId, const QString &kind,
                      const QString &path, const QString &mime, int timeoutMs,
                      MediaPayload *out, QString *outError)
{
    if (!out) {
        return false;
    }
    if (path.isEmpty()) {
        if (outError) {
            *outError = QStringLiteral("上行: 媒体路径为空 media=%1 kind=%2").arg(mediaId, kind);
        }
        return false;
    }

    // 等写稳：连续两次采样 size 相同且 >0 才认为落盘结束。
    // 超时是正常结局（截图线程挂了、片段被 ClipWriter 丢掉、pruneAlarmMedia 删了目录），
    // 调用方按"这段没有"处理，不重试——文件不会自己变好
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + qMax(0, timeoutMs);
    qint64 lastSize = -1;
    qint64 size = -1;
    for (;;) {
        const QFileInfo info(path);
        size = info.exists() ? info.size() : -1;
        if (size > 0 && size == lastSize) {
            break;
        }
        if (QDateTime::currentMSecsSinceEpoch() >= deadline) {
            if (outError) {
                *outError = QStringLiteral("上行: 媒体文件等待超时 media=%1 kind=%2 size=%3 path=%4")
                                .arg(mediaId, kind).arg(size).arg(path);
            }
            return false;
        }
        lastSize = size;
        QThread::msleep(kStableSampleMs);
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (outError) {
            *outError = QStringLiteral("上行: 媒体文件打开失败 media=%1 path=%2 err=%3")
                            .arg(mediaId, path, file.errorString());
        }
        return false;
    }
    const QByteArray data = file.readAll();
    file.close();

    if (data.isEmpty()) {
        if (outError) {
            *outError = QStringLiteral("上行: 媒体文件为空 media=%1 path=%2").arg(mediaId, path);
        }
        return false;
    }

    // 读到的字节数与最后一次采样不一致，说明文件仍在长（或已被截断）。
    // 这里**不回头重采样**：以实际读到的字节为准，sha256 与 total_bytes 都按这份算，
    // 对端拿到的是一段自洽但不完整的媒体——media_end 的 seqs_sent 与 sha256 会暴露它
    out->mediaId = mediaId;
    out->alarmId = alarmId;
    out->kind = kind;
    out->path = path;
    out->name = QFileInfo(path).fileName();
    out->mime = mimeForMediaPath(path, mime);
    out->data = data;
    out->sha256Hex = QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    return true;
}

}  // namespace uplink
