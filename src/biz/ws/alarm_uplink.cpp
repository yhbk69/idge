// ============================================================================
// alarm_uplink.cpp — 报警上行编排（GUI 线程）
// ----------------------------------------------------------------------------
// 本文件不含协议细节，只有四件事：
//   1) 装配：读 alarm_uplink 配置段，把 recording 的实际状态折算进引擎设置
//      （send_clip=true 但 recording.enabled=false 是"视频那段永远不来"的组合，
//       必须在启动时说明白，而不是让它静默空转）
//   2) 接线：AlarmManager::alarmGenerated（解码线程发）→ QueuedConnection → 本对象
//   3) 节奏：submit 之后泵一次；发不完就挂一个**单发**定时器，下次时刻问引擎
//      （水位没退下来是 20ms，等确认或退避中就等到那个点），队列空即停表。
//      常驻定时器在 GUI 线程上是负担，所以只在有积压时才跑。
//   4) 媒体交接：引擎回执里的 first 决定"这一批该去读盘"，任务交给 MediaPump；
//      字节在它的工作线程里读好、算完哈希，再回到这里入引擎队列。
//      url 模式下读回的字节只用来定 size 与 sha256，真正上线的是地址。
// 未启用时 attach() 直接返回，一个 connect 都不做。
// ============================================================================

#include "alarm_uplink.h"

// 平铺 include（仓库约定，见 src/README.md）：路径由根 CMakeLists 的 include_directories 解析
#include "ConfigManager.h"
#include "media_pump.h"
#include "runtime_paths.h"
#include "ws_client.h"

#include <QFileInfo>

AlarmUplink &AlarmUplink::instance()
{
    static AlarmUplink uplink;
    return uplink;
}

AlarmUplink::AlarmUplink(QObject *parent)
    : QObject(parent)
    , flushTimer_(new QTimer(this))
{
    flushTimer_->setSingleShot(true);
    connect(flushTimer_, &QTimer::timeout, this, &AlarmUplink::onFlushTick);
}

AlarmUplink::~AlarmUplink()
{
    detach();
}

void AlarmUplink::setMediaBaseUrl(const QString &baseUrl)
{
    mediaBaseUrl_ = baseUrl.trimmed();
}

void AlarmUplink::attach()
{
    if (attached_) {
        return;
    }
    ConfigManager &cfg = ConfigManager::instance();
    if (!cfg.alarmUplinkEnabled()) {
        emit logMessage("system", "报警上行未启用 (alarm_uplink.enabled=false)");
        return;
    }

    // 判据与 frmMain 装配处必须一模一样（getter 已把值收敛成 url / ws_binary 两种）。
    // 两边不一致就会出现"服务在监听但媒体走 WS"这种双出口：装配起了服务而这里不认，
    // 或反过来申报了地址却没人监听
    const QString transport = cfg.alarmUplinkMediaTransport();
    const bool viaUrl = (transport == QLatin1String(uplink::kTransportUrl));
    mediaViaUrl_ = viaUrl;

    const bool recordingOn = cfg.recordingEnabled();
    uplink::AlarmUplinkEngine::Settings settings;
    settings.maxPending = cfg.alarmUplinkMaxPending();
    settings.sendSnapshot = cfg.alarmUplinkSendSnapshot();
    // 片段来自录像链路，录像关着就没有片段可发：这里直接降级成"不申报片段"，
    // 否则对端会一直等一个永远不会来的 media_id
    settings.sendClip = cfg.alarmUplinkSendClip() && recordingOn;
    settings.clipMime = cfg.recordingCodec().startsWith(QStringLiteral("hevc"))
        ? QString::fromLatin1(uplink::kMimeHevc)
        : QString::fromLatin1(uplink::kMimeAvc);
    clipMime_ = settings.clipMime;
    settings.chunkSize = cfg.alarmUplinkChunkSize();
    settings.expectAck = cfg.alarmUplinkExpectAck();
    settings.ackTimeoutMs = cfg.alarmUplinkAckTimeoutMs();
    settings.maxAttempts = cfg.alarmUplinkMaxAttempts();
    settings.mediaViaUrl = viaUrl;
    settings.mediaBaseUrl = mediaBaseUrl_;
    if (viaUrl) {
        // alarmsDir() 是相对 cwd 的（见 runtime_paths.h 顶部），文件服务要的是绝对路径。
        // mediaUrlFor 那边换算的 payload.path 也是相对同一个 cwd 的，两边基准必须一致
        settings.mediaRootDir = QFileInfo(RuntimePaths::alarmsDir()).absoluteFilePath();
        mediaRootDir_ = settings.mediaRootDir;
    }
    engine_.setSettings(settings);
    engine_.setTransport(&WsClient::instance());
    engine_.setLogger([this](const QString &category, const QString &message) {
        emit logMessage(category, message);
    });

    if (cfg.alarmUplinkSendClip() && !recordingOn) {
        emit logMessage("warning", "报警上行: send_clip=true 但 recording.enabled=false，"
                                   "本次只发截图与报警信息（现场老配置常见）");
    }
    if (viaUrl && settings.mediaBaseUrl.isEmpty()) {
        // 地址前缀空 = 装配点没能把文件服务起起来（端口被占 / 本机没有可用 IPv4 且没配前缀）。
        // 引擎会据此把两条媒体腿都不申报，这里只负责把"为什么没图没视频"说在前面
        emit logMessage("warning", "报警上行: media_transport=url 但媒体地址前缀为空，"
                                   "截图与片段都不申报，只发 alarm_info 文本");
    }

    // 报警在解码线程产生，必须排队投递到 GUI 线程再进引擎（引擎不加锁）
    connect(&AlarmManager::instance(), &AlarmManager::alarmGenerated,
            this, &AlarmUplink::onAlarmGenerated, Qt::QueuedConnection);
    // 片段比报警晚 2 秒以上（ClipWriter 到点才 dump），这两路都在那条后台线程发
    connect(&AlarmManager::instance(), &AlarmManager::videoClipAttached,
            this, &AlarmUplink::onVideoClipAttached, Qt::QueuedConnection);
    connect(&AlarmManager::instance(), &AlarmManager::videoClipSkipped,
            this, &AlarmUplink::onVideoClipSkipped, Qt::QueuedConnection);
    // 重连成功后先把积压的 alarm_info 泵出去，不用等下一个提交
    connect(&WsClient::instance(), &WsClient::connected,
            this, &AlarmUplink::onLinkConnected);
    // 对端的确认（expect_ack=true 才有内容）。WsClient 与引擎同在 GUI 线程，直连即可
    connect(&WsClient::instance(), &WsClient::uplinkAcked,
            this, &AlarmUplink::onUplinkAcked);

    attached_ = true;
    emit logMessage("system", QStringLiteral("报警上行已启用: transport=%1 snapshot=%2 clip=%3 chunk=%4 pending=%5")
                                  .arg(viaUrl ? QStringLiteral("url") : transport)
                                  .arg(settings.sendSnapshot ? "on" : "off")
                                  .arg(settings.sendClip ? "on" : "off")
                                  .arg(settings.chunkSize)
                                  .arg(settings.maxPending));
    if (viaUrl) {
        emit logMessage("system", QStringLiteral("报警媒体: 服务根目录=%1 地址前缀=%2")
                                      .arg(settings.mediaRootDir,
                                           settings.mediaBaseUrl.isEmpty()
                                               ? QStringLiteral("(空，不申报媒体)")
                                               : settings.mediaBaseUrl));
    }
}

void AlarmUplink::detach()
{
    if (!attached_) {
        return;
    }
    disconnect(&AlarmManager::instance(), nullptr, this, nullptr);
    disconnect(&WsClient::instance(), nullptr, this, nullptr);
    flushTimer_->stop();
    engine_.clear();
    attached_ = false;
}

void AlarmUplink::onAlarmGenerated(const AlarmRecord &alarm)
{
    // 只搬运字段，判断留给引擎：channel 保持内部的 0-based，上行 JSON 里才 +1
    uplink::AlarmInfoInput input;
    input.alarmId = alarm.id;
    input.alarmType = alarm.alarmType;
    input.alarmLevel = alarm.alarmLevel;
    input.alarmTime = alarm.alarmTime;
    input.channel = alarm.channel;
    input.classId = alarm.clsId;
    input.className = alarm.className;
    input.confidence = alarm.confidence;
    input.isFenceAlarm = alarm.isFenceAlarm;
    input.status = alarm.status;
    input.imagePath = alarm.imagePath;
    input.videoPath = alarm.videoPath;
    // 施工信息在报警产生时就固化在记录里了，这里只搬运、不回查会话态：
    // 退出登录后重传的那条，仍带它产生当时的施工信息。
    input.constructionJson = alarm.constructionJson;

    const uplink::SubmitResult result = engine_.submit(input);
    // 第二段（截图）：只有批次首次登记时才去读盘。同一张图可能被 N 条报警行共享，
    // 判重依据必须是引擎的 slot 表本身，这里不再自己记一份路径集合
    if (result.snapshotFirst) {
        requestMedia(result.snapshotMediaId, input.alarmId,
                    QString::fromLatin1(uplink::kKindSnapshot), input.imagePath,
                    QString::fromLatin1(uplink::kMimeJpeg));
    }
    pumpAndReschedule();
}

void AlarmUplink::onVideoClipAttached(const QStringList &alarmIds, const QString &videoPath,
                                      const QString &webPath)
{
    if (!attached_ || alarmIds.isEmpty()) {
        return;
    }
    // media_id 是报警那一刻在 alarm_info 里预告过的，此刻只能按报警 id 找回
    const QString mediaId = engine_.clipMediaIdFor(alarmIds);
    engine_.forgetClipPromises(alarmIds);
    if (mediaId.isEmpty()) {
        return;   // 这批报警没承诺过片段（上行接入前就产生的报警、或承诺已被淘汰）
    }
    // url 模式要的是 .mp4 网页副本：给对端一个裸流地址它照样播不了，没有副本就等于没有视频
    const QString path = mediaViaUrl_ ? webPath : videoPath;
    if (path.isEmpty()) {
        engine_.submitMediaFailed(mediaId, alarmIds.first(),
                                  QString::fromLatin1(uplink::kKindClip),
                                  mediaViaUrl_ ? QStringLiteral("mp4 副本没写成")
                                               : QStringLiteral("片段路径为空"));
        pumpAndReschedule();
        return;
    }
    // MIME 按真实后缀算（.mp4 → video/mp4，裸流 → video/H265），clipMime_ 只兜未知后缀
    requestMedia(mediaId, alarmIds.first(), QString::fromLatin1(uplink::kKindClip), path,
                 uplink::mimeForMediaPath(path, clipMime_));
}

void AlarmUplink::onVideoClipSkipped(const QStringList &alarmIds, const QString &reason)
{
    if (!attached_ || alarmIds.isEmpty()) {
        return;
    }
    const QString mediaId = engine_.clipMediaIdFor(alarmIds);
    if (mediaId.isEmpty()) {
        return;   // 没承诺过就无所谓"跳过"，别给日志添噪音
    }
    engine_.forgetClipPromises(alarmIds);
    // 对端已经收到过一条带 clip 的 alarm_info，这段媒体不会来了：说清原因并把承诺撤掉
    engine_.submitMediaFailed(mediaId, alarmIds.first(),
                              QString::fromLatin1(uplink::kKindClip), reason);
    pumpAndReschedule();
}

void AlarmUplink::requestMedia(const QString &mediaId, const QString &alarmId,
                               const QString &kind, const QString &path, const QString &mime)
{
    uplink::MediaPump::Request req;
    req.mediaId = mediaId;
    req.alarmId = alarmId;
    req.kind = kind;
    req.path = path;
    req.mime = mime;
    req.context = this;   // 回调只在 GUI 线程跑：引擎与 socket 都只属于这条线程
    // mediaId/alarmId/kind 按值捕获：失败时载荷是空的，撤承诺要用的是请求里那几个 id
    req.onDone = [this, mediaId, alarmId, kind](const uplink::MediaPayload &payload,
                                                const QString &error) {
        if (!attached_) {
            return;   // detach 之后队列已清，回来的字节没人认领，直接丢
        }
        if (!error.isEmpty()) {
            // 读盘失败不重试：图没写出来就是没写出来，文件不会自己变好。
            // 但对端手里还攥着这条 planned，必须撤掉（url 模式下它真会去取一个 404）
            engine_.submitMediaFailed(mediaId, alarmId, kind, error);
            pumpAndReschedule();
            return;
        }
        uplink::MediaJob job;
        job.mediaId = payload.mediaId;
        job.alarmId = payload.alarmId;
        job.kind = payload.kind;
        job.name = payload.name;     // 真实文件名，覆盖 alarm_info 里的那个猜测
        job.mime = payload.mime;
        if (mediaViaUrl_) {
            // 字节到此为止：url 模式上线的是地址，data 只贡献 total_bytes 这一个数
            const QString url = uplink::mediaUrlFor(
                mediaBaseUrl_, mediaRootDir_, QFileInfo(payload.path).absoluteFilePath());
            if (url.isEmpty()) {
                engine_.submitMediaFailed(job.mediaId, job.alarmId, job.kind,
                                          QStringLiteral("路径不在媒体服务根目录之下: %1")
                                              .arg(payload.path));
            } else {
                engine_.submitMediaUrl(job, url, payload.data.size(), payload.sha256Hex);
            }
        } else {
            engine_.submitMedia(job, payload.data, payload.sha256Hex);
        }
        pumpAndReschedule();
    };
    uplink::MediaPump::get().submit(std::move(req));
}

void AlarmUplink::pumpAndReschedule()
{
    engine_.pumpOut();
    if (!engine_.hasPending()) {
        flushTimer_->stop();
        return;
    }
    // 链路没连上时不空转定时器：WsClient::connected 会再叫一次本函数
    if (WsClient::instance().isConnected()) {
        // 下次泵的节奏由引擎定：只是水位没退下来就是 20ms，等确认或退避中就等到那个点。
        // 写死 20ms 的话 expect_ack 一开，GUI 线程会被按每秒 50 次叫醒直到超时（15 秒）
        flushTimer_->start(engine_.nextWaitMs());
    }
}

void AlarmUplink::onFlushTick()
{
    pumpAndReschedule();
}

void AlarmUplink::onLinkConnected()
{
    // 断线时没发完的那一段：游标清零重头发。留着半截游标等于给对端一段
    // 缺头或缺尾的裸流，它按 sha256 只能判"废"，不如整个重发
    engine_.restartInFlightMedia();
    pumpAndReschedule();
}

void AlarmUplink::onUplinkAcked(const QString &messageId, const QString &mediaId,
                                const QString &phase, const QString &status,
                                const QString &error)
{
    // 认领、退避、判失败都在引擎里（那里才有队首状态），这里只负责"确认到了，再泵一次"
    engine_.handleAck(messageId, mediaId, phase, status, error);
    pumpAndReschedule();
}
