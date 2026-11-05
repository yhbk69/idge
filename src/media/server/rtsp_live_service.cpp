// ============================================================================
// rtsp_live_service.cpp — 带检测框视频的 RTSP 直播分发
// ============================================================================

#include "rtsp_live_service.h"

#include <QDebug>
#include <QMutex>
#include <QMutexLocker>
#include <QHash>

#include "http_file_service.h"   // firstUsableIpv4（复用同一套非回环 IPv4 判据）
#include "zlm_env.h"
#include "mk_common.h"
#include "mk_media.h"

namespace {

// ZLM 约定的默认 vhost 名。app 固定 "live"，与 rtsp://host:port/live/<stream> 对应。
const char *kVhost = "__defaultVhost__";
const char *kApp = "live";

// mk_media_init_video 的 codec_id：0=H264, 1=H265（见 mk_media.h 注释）
int codecIdFor(const QString &codec)
{
    return codec.compare(QStringLiteral("hevc"), Qt::CaseInsensitive) == 0 ? 1 : 0;
}

// 全服务共享的 media source 表（streamId -> mk_media 句柄）。
// 所有方法都走这一张表；写操作用 mtx 保护。
QMutex g_sourcesMtx;
QHash<QString, void *> g_sources;

void *findSourceLocked(const QString &streamId)
{
    return g_sources.value(streamId, nullptr);
}

}  // namespace

RtspLiveService &RtspLiveService::instance()
{
    static RtspLiveService svc;
    return svc;
}

bool RtspLiveService::start(quint16 port)
{
    if (running()) {
        return true;
    }
    if (port == 0) {
        qWarning() << "RtspLiveService: port=0，不起服务";
        return false;
    }
    if (!zlm::ensureEnvInit()) {
        qWarning() << "RtspLiveService: ZLM 运行时初始化失败";
        return false;
    }

    const uint16_t bound = mk_rtsp_server_start(port, 0);   // ssl=0：明文
    if (bound == 0) {
        qWarning() << "RtspLiveService: 起口失败 port=" << port
                   << "（端口被占或库未就绪）";
        return false;
    }
    port_ = bound;
    qInfo() << "RtspLiveService: RTSP 已监听" << bound;
    return true;
}

void RtspLiveService::stop()
{
    // 不调 mk_stop_all_server：进程退出时由 HttpFileService::stop 统一停掉所有 ZLM 服务。
    // 这里只释放本服务持有的 media source，避免源泄漏。
    QHash<QString, void *> snapshot;
    {
        QMutexLocker lock(&g_sourcesMtx);
        snapshot = g_sources;
        g_sources.clear();
    }
    for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it) {
        mk_media_release(it.value());
    }
    if (!snapshot.isEmpty()) {
        qInfo() << "RtspLiveService: 已释放" << snapshot.size() << "路 media source";
    }
}

QString RtspLiveService::baseUrl() const
{
    if (!running()) {
        return QString();
    }
    const QString ip = HttpFileService::firstUsableIpv4();
    if (ip.isEmpty()) {
        qWarning() << "RtspLiveService: 没有可用非回环 IPv4，liveUrl 无法拼接";
        return QString();
    }
    return QStringLiteral("rtsp://%1:%2").arg(ip).arg(port_);
}

void RtspLiveService::createSource(const QString &streamId, const QString &codec,
                                   int width, int height, int fps, int bitRate)
{
    if (streamId.isEmpty()) {
        return;
    }
    QMutexLocker lock(&g_sourcesMtx);

    auto it = g_sources.find(streamId);
    if (it != g_sources.end()) {
        mk_media_release(it.value());
        g_sources.erase(it);
    }

    mk_media m = mk_media_create(kVhost, kApp, streamId.toUtf8().constData(),
                                 0.0f, 0, 0);   // 直播：duration=0、不生成 hls/mp4
    if (!m) {
        qWarning() << "RtspLiveService: mk_media_create 失败 stream=" << streamId;
        return;
    }
    mk_media_init_video(m, codecIdFor(codec), width, height,
                        fps > 0 ? static_cast<float>(fps) : 25.0f, bitRate);
    mk_media_init_complete(m);   // 单 track，加快流生成
    g_sources.insert(streamId, m);
    qInfo() << "RtspLiveService: 注册直播流" << streamId
            << codec << width << "x" << height << fps << "fps";
}

void RtspLiveService::pushH264(const QString &streamId, const uint8_t *data, int len, uint64_t pts_ms)
{
    void *m = nullptr;
    {
        QMutexLocker lock(&g_sourcesMtx);
        m = findSourceLocked(streamId);
    }
    if (!m || !data || len <= 0) {
        return;
    }
    mk_media_input_h264(m, data, len, pts_ms, pts_ms);
}

void RtspLiveService::pushH265(const QString &streamId, const uint8_t *data, int len, uint64_t pts_ms)
{
    void *m = nullptr;
    {
        QMutexLocker lock(&g_sourcesMtx);
        m = findSourceLocked(streamId);
    }
    if (!m || !data || len <= 0) {
        return;
    }
    mk_media_input_h265(m, data, len, pts_ms, pts_ms);
}

void RtspLiveService::removeSource(const QString &streamId)
{
    void *m = nullptr;
    {
        QMutexLocker lock(&g_sourcesMtx);
        m = g_sources.take(streamId);
    }
    if (m) {
        mk_media_release(m);
        qInfo() << "RtspLiveService: 注销直播流" << streamId;
    }
}

bool RtspLiveService::hasSource(const QString &streamId) const
{
    QMutexLocker lock(&g_sourcesMtx);
    return g_sources.contains(streamId);
}
