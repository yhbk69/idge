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

// Meyers 单例：第一次调用必须在 GUI 线程（frmMain 构造里），因为它要建 QTimer 并
// 把引擎的回调绑在这根线程上；线程亲和性由首次调用点决定，本类不校验。
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

// 装配点注地址，本类不反向 include media 域（依赖方向 base ← media ← biz ← ui）。
// trimmed() 的作用是把配置里多余的首尾空白掐掉：带空格的地址会原样拼进 URL，而 url 档
// 的失败只表现为"对端取不到、Pad 显示无证据"，不会有任何报错，只能在这一层防。
// 必须在 attach() 之前调：Settings.mediaBaseUrl 是 attach 时一次性拷进引擎的。
void AlarmUplink::setMediaBaseUrl(const QString &baseUrl)
{
    mediaBaseUrl_ = baseUrl.trimmed();
}

/**
 * @brief 订阅本机报警与链路事件，并按配置装配引擎
 * @pre setMediaBaseUrl() 已先调过（url 档的地址只能由装配点注进来，attach() 里不再回查）
 *
 * 三段顺序是有讲究的，改代码时别挪动：
 *   1) 先算 Settings 再 connect——引擎在收到第一条报警前必须已经知道 chunkSize /
 *      mediaViaUrl，否则会出现"报警已进队列但分块尺寸还没定"；
 *   2) engine_.setTransport(&WsClient::instance()) 在这里首次创建 WsClient 单例，
 *      所以 attach() 必须在 GUI 线程（WsClient 的定时器与 socket 都归这根线程）；
 *   3) connect 全部显式 Qt::QueuedConnection（而不是默认 AutoConnection）：报警来自
 *      解码/片段线程，本对象住在 GUI 线程，写死排队让"引擎只在 GUI 线程被调用"
 *      这条不变量在代码里可读，不依赖运行时两端是否同线程。
 *
 * 重复调用被 attached_ 挡住；配置改动**不会**被感知——要换参数得先 detach() 再 attach()，
 * 运行期没有热更新入口（刻意的：上行链路只在启动时装一次，改配置请重启）。
 * 失败/降级都在日志里（三条 warning），返回值不表达失败：调用方（frmMain）不需要据此回滚。
 */
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

/**
 * @brief 断开全部订阅并清空引擎队列（退出流程用）
 *
 * 两次 wildcard disconnect 分别摘掉 AlarmManager 与 WsClient 上的所有连接，
 * 不逐个 disconnect 是因为这里接的是 6 个不同信号，逐个写迟早与 attach() 对不上。
 * engine_.clear() 丢掉在队与在飞的媒体：detach 之后 MediaPump 的回调还会回来
 * （那条工作线程不归本类管），requestMedia 的 lambda 开头有 attached_ 判据把它们丢掉。
 * 幂等：没 attach 过直接返回，不会误停一个从未启动的定时器。
 * 注意不关 socket——断链是 WsClient::stop() 的职责，本类只是它的订阅者之一。
 */
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

/**
 * @brief 一条报警到达（GUI 线程，已从解码线程排队过来）：登记 + 组 alarm_info + 派读盘任务
 *
 * 这里只做"AlarmRecord → AlarmInfoInput"的搬运，任何协议判断都不在本文件：
 * channel 保持内部 0-based，+1 发生在 buildAlarmInfoMessage（两处都改就会变成 +2）。
 * 抓拍图只在 result.snapshotFirst 为真时交给 MediaPump——判重依据是引擎的 slot 表，
 * 本文件不再自己记一份路径集合（两份迟早对不上）。
 * 副作用：向引擎队列压一项（可能挤掉最旧的一条，见 trimQueueToFit），并可能向
 * MediaPump 提交一次读盘任务。链路没连时只入队不发送，等 connected 再泵。
 * 图片还没落盘不是问题：读盘侧按"等文件写稳"处理（media_pump 的 kFileWaitTimeoutMs）。
 */
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

/**
 * @brief 片段落盘回填：把 alarm_info 里承诺过的 clip 变成真正的字节或地址
 * @param alarmIds 同一批共享这个片段的报警 id（ClipWriter 按批给）；空即返回
 * @param videoPath Annex-B 裸流路径 = DB 里 alarms.video_path 存的那个
 * @param webPath   同名 .mp4 副本，**只有 url 模式用**（浏览器打不开裸流）
 *
 * 两档共用一段代码，差别只有"取哪个路径"：url 档拿 webPath，ws_binary 档拿 videoPath。
 * webPath 在报警时刻可能压根没写成（副本转码失败），此时必须 submitMediaFailed 把承诺撤掉，
 * 否则对端会去取一个不存在的地址。
 * 注意 mediaId 只认**第一个**能反查到的 id（引擎的 clipMediaIdFor 语义）：
 * 一批共享一个片段本来就只有一个 id，跨批的 ids 传进来属于上游 bug，这里不兜。
 * 反查一次就 forgetClipPromises，避免承诺表只增不减（表上限 256，见 kMaxClipPromises）。
 */
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

/**
 * @brief 片段确定不会有了（录像队列丢最旧 / 编码器没就绪 / dump 失败）：撤掉那条承诺
 * @param reason 由 ClipWriter 给的中文原因，原样进 media_failed 报文（不含内容字节）
 *
 * 与 onVideoClipAttached 的关键差别：这里不能先 forget 再判空——反查不到 id 说明
 * 这批报警从未申报过片段，清表只是白清，还会给日志添噪音（所以判空在前、forget 在后）。
 * 不撤的后果是对端手里永远攥着一条 planned=true 的 clip：ws_binary 档无限等 begin，
 * url 档会去下载一个不存在的地址。
 */
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

/**
 * @brief 把一段媒体交给后台读盘，读好回到 GUI 线程才进引擎队列
 * @param mediaId/alarmId/kind 按值捕获进回调：失败路径上 payload 是空的，
 *        撤承诺只能靠请求里带的这几个 id（这是它们不入 MediaPayload 的原因）
 * @param path 本机落盘路径，相对/绝对都行——url 模式在回调里统一 absoluteFilePath() 再换算，
 *             因为 QDir::relativeFilePath 对相对入参是按 rootDir 解析的（会给双重前缀）
 * @param mime 兜底值；真实后缀优先（mimeForMediaPath）
 *
 * req.context = this 是这条链唯一的线程交接保险：对象析构后 Qt 会丢掉事件，
 * 回调不会打到悬垂 this 上。但 detach() 之后对象还活着，所以回调里还得再判一次
 * attached_（队列已清，回来的字节没人认领，直接丢）。
 * 读盘失败**不重试**：文件没写出来就是没写出来；但必须 submitMediaFailed。
 * MediaPump 队列满时本任务会把更早的那个挤掉（返回值没接，见 media_pump.h 的 kMaxPending）——
 * 被挤掉的一方由 reportDropped 走同一条回调报失败，不需要这里额外处理。
 */
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

/**
 * @brief 发一轮，有余货就按引擎给的时刻重挂**单发**定时器
 *
 * 常驻定时器在 GUI 线程上是负担，所以队列一空就 stop()；只有提交动作、ack、重连
 * 这三类事件会把表重新挂起来。
 * 链路没连上时**不挂表**：否则断线期间定时器空转，而 pumpOut 在 !transportConnected
 * 时本来就直接返回 0。等 WsClient::connected 再叫一次本函数，积压原样还在队列里。
 * 时长来自 engine_.nextWaitMs()（20ms 只是水位没退下来时的重试间隔；开了 expect_ack
 * 就改成等到那个 deadline），写死 20ms 会让 GUI 线程在 15 秒超时期间被叫醒 750 次。
 */
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

// 定时器到点只做一件事，存在的意义是把"重挂表"这个动作集中到 pumpAndReschedule 里。
void AlarmUplink::onFlushTick()
{
    pumpAndReschedule();
}

/**
 * @brief 链路（重）连成功：先把半截的媒体段退回第一块，再把积压泵出去
 *
 * 挂在 WsClient::connected 上，用的是默认 AutoConnection——两端同在 GUI 线程，
 * 这里是直连；别改成 QueuedConnection（会把泵的动作推迟到下一轮事件，白等一次）。
 * restartInFlightMedia 必须先于 pump：否则游标还停在半截处，接着往下发就是给对端
 * 一段缺头或缺尾的裸流，它按 sha256 只能判废，不如整段重发。
 */
void AlarmUplink::onLinkConnected()
{
    // 断线时没发完的那一段：游标清零重头发。留着半截游标等于给对端一段
    // 缺头或缺尾的裸流，它按 sha256 只能判"废"，不如整个重发
    engine_.restartInFlightMedia();
    pumpAndReschedule();
}

/**
 * @brief 透传对端确认给引擎，然后泵一次
 * @note 五个参数不在这里解释：认领哪一项、算不算成功、要不要退避都在引擎里判
 *       （只有那里有队首状态）。expect_ack=false（出厂默认）时 Pad 不回这个类型，
 *       本槽一次也不会被触发，空转成本为零。
 */
void AlarmUplink::onUplinkAcked(const QString &messageId, const QString &mediaId,
                                const QString &phase, const QString &status,
                                const QString &error)
{
    // 认领、退避、判失败都在引擎里（那里才有队首状态），这里只负责"确认到了，再泵一次"
    engine_.handleAck(messageId, mediaId, phase, status, error);
    pumpAndReschedule();
}
