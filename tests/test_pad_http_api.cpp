// ============================================================================
// test_pad_http_api.cpp — v1 HTTP `/api/` 面的线上自检（loopback 真服务端 + 真 socket）
// ----------------------------------------------------------------------------
// 起真 PadHttpServer 在 127.0.0.1 的一个临时口上、真 QTcpSocket 连进来按 §4~§7 走一遍。
// 为什么值得单独钉（每一条都对应一次实测踩到或差点踩到的事）：
//   · 信封与错误码是**对端唯一能看的**东西：400/2001 写成 422/4001，Pad 的提示就指错方向；
//   · §2.5 幂等只认 requestId：重放必须逐字节相同，少一次 store 就是"重试会再执行一遍"；
//   · §5.3 那九种状态组合里，2004/2005/2006 走错一个，Pad 的围栏页就会出现
//     "点了没反应"或"明明有草稿说没有"；
//   · §6.1 的 liveUrl 会带上视频源口令——**必须**验它被剥掉了（这是本文件的契约 6）；
//   · cameraId 用注册表 id 时，两路引用同一条流会得到两个相同 cameraId，
//     而 §5.1 的唯一键正是 (taskId, cameraId)（契约 7，本机第一版就是这么写的）；
//   · 处置照片落盘的路径**不能**用 multipart 的 filename（契约 13 给的是 ../../etc/passwd）。
//
// 覆盖二十一条契约：
//   1) GET /api/health → code 0，capabilities.stream.startStop=false、fence.maxRegions=8
//   2) GET /api/device/status → snake_case、四个 cameras、device_id 非空
//   3) GET /api/streams 缺 taskId → 400/2001
//   4) GET /api/streams?taskId= → 回显 taskId、streams 恒数组、每条三个 ID/状态键齐
//   5) status=ONLINE 的那路 liveUrl 非空且 liveProtocol=RTSP
//   6) liveUrl **不含** rtsp 源里的 user:pass（默认剥凭据）
//   7) cameraId 是通道号口径 CH1，不是注册表 id cam-a（两路引用同一条流也不撞车）
//   8) GET /api/streams/CH1/status → 200；未知 id → 404/2002
//   9) 未知路径 → 404/2002（detail 带方法与路径）
//  10) GET /api/dispose-options → options 非空、按 sortOrder 升序
//  11) save-draft 新建 → 200 + fenceId/version=1/DRAFT/updatedAt
//  12) 同 requestId 重放 → **逐字节相同**，且不重复建版本
//  13) dispose 的 multipart：filename 是 ../../etc/passwd 也只当数据，
//      照片落在 dispose/<alarmId>/ 下、photoUrls 出得来、库里 dispose_photo 是 JSON 数组
//  14) save-draft 的 RECT 只给 3 点 → 422/4001
//  15) 已有草稿再新建同 (taskId,cameraId) → 409/2004
//  16) apply（媒体底址没配）→ 503/3003；配了则 200 + frameUrl 且 PNG 真在磁盘上
//  17) disable 草稿 → 409/2005；delete-draft 生效版本 → 409/2005
//  18) POST /api/alarms/{id}/read → 200 + readAt 用**对端报的那个**（比本机时刻早）
//  19) false-alarm 成功后再标 → 409/2003；终态再 dispose → 409/2003
//  20) keep-alive：同一条连接连发三个请求都能各自收到完整响应
//  21) 处置成功后紧接着模拟"关机整批回写"，状态必须还是 disposed
//     （内存窗口那份副本漏改的话，处置会在第二天早上神秘复原）
//
// 仓库根运行：./build/build_rk3588_linux/test_pad_http_api，rc 0=PASS 1=FAIL
// ============================================================================

#include "pad_http_api.h"

#include "ConfigManager.h"
#include "alarm_dao.h"
#include "alarm_manager.h"
#include "database_manager.h"
#include "pad_http_server.h"
#include "pad_request_cache.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <functional>
#include <iostream>

namespace {

int failures = 0;

void check(bool ok, const char *message)
{
    if (!ok) {
        ++failures;
        std::cout << "  FAIL: " << message << std::endl;
    }
}

// ============================================================================
// 最小 HTTP 客户端
// ----------------------------------------------------------------------------
// 不用 Qt5::Test（板端没装），也不自己起 QEventLoop：服务端与客户端在同一条线程上，
// 所以"写出去 → 反复 processEvents 直到收完一个完整响应"就够，
// 而且这个写法能把服务端的 newConnection/readyRead 一起推动（waitForReadyRead 做不到）。
// ============================================================================
struct HttpResponse {
    int status = 0;
    QByteArray body;
    QJsonObject json;
    bool hasJson = false;
    int code = -1;                       ///< 信封里的 code（-1 = 没解出来）
};

class HttpClient {
public:
    bool open(quint16 port)
    {
        // Qt 5.12 的枚举叫 LocalHost，没有 LocalHostIPv4（板端实测编译报 no member）
        socket_.connectToHost(QHostAddress(QHostAddress::LocalHost), port);
        return spinUntil([&]() { return socket_.state() == QAbstractSocket::ConnectedState; },
                         2000);
    }
    void close()
    {
        socket_.abort();
    }

    bool send(const QByteArray &request, HttpResponse *out)
    {
        buffer_.clear();
        socket_.write(request);
        socket_.flush();
        if (!spinUntil([&]() { return parseComplete(out); }, 3000))
            return false;
        return out->status != 0;
    }

private:
    bool spinUntil(const std::function<bool()> &done, int timeoutMs)
    {
        QElapsedTimer clock;
        clock.start();
        while (!done()) {
            QCoreApplication::processEvents();
            const QByteArray incoming = socket_.readAll();
            if (!incoming.isEmpty())
                buffer_.append(incoming);
            if (done())
                return true;
            if (clock.elapsed() > timeoutMs)
                return false;
            QThread::msleep(1);
        }
        return true;
    }

    /// 收到完整响应 = 头齐了且正文字节数达到 Content-Length
    bool parseComplete(HttpResponse *out)
    {
        const int headEnd = buffer_.indexOf("\r\n\r\n");
        if (headEnd < 0)
            return false;
        const QByteArray head = buffer_.left(headEnd);
        const int statusLineEnd = head.indexOf("\r\n");
        const QByteArray statusLine = statusLineEnd < 0 ? head : head.left(statusLineEnd);
        const QList<QByteArray> parts = statusLine.split(' ');
        if (parts.size() < 2)
            return false;
        // Content-Length 的取法：本服务端恒写这一条（serializeReply），所以没找到就当 0，
        // 不做"读到连接关闭为止"那种兜底——那会把 keep-alive 的空等当成响应不完整。
        int length = 0;
        const int lengthAt = head.indexOf("Content-Length:");
        if (lengthAt >= 0) {
            QByteArray value = head.mid(lengthAt + qstrlen("Content-Length:"));
            const int lineEnd = value.indexOf('\r');
            if (lineEnd >= 0)
                value = value.left(lineEnd);
            length = value.trimmed().toInt();
        }
        const QByteArray body = buffer_.mid(headEnd + 4);
        if (body.size() < length)
            return false;
        out->status = parts.at(1).toInt();
        out->body = body.left(length);
        // 剩下的字节留给同一条连接的下一个请求（keep-alive 契约 20 用到这点）
        buffer_.remove(0, headEnd + 4 + out->body.size());
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(out->body, &error);
        out->hasJson = doc.isObject();
        if (out->hasJson) {
            out->json = doc.object();
            out->code = out->json.value(QStringLiteral("code")).toInt(-1);
        }
        return true;
    }

    QTcpSocket socket_;
    QByteArray buffer_;
};

QByteArray requestLine(const QString &method, const QString &target, int bodySize,
                       const QByteArray &contentType = QByteArray())
{
    QByteArray raw = method.toUtf8() + " " + target.toUtf8() + " HTTP/1.1\r\n";
    raw += "Host: 127.0.0.1\r\n";
    if (!contentType.isEmpty())
        raw += "Content-Type: " + contentType + "\r\n";
    raw += "Content-Length: " + QByteArray::number(bodySize) + "\r\n";
    raw += "\r\n";
    return raw;
}

QByteArray jsonPost(const QString &target, const QJsonObject &body)
{
    const QByteArray raw = QJsonDocument(body).toJson(QJsonDocument::Compact);
    return requestLine(QStringLiteral("POST"), target, raw.size(),
                       QByteArray("application/json")) + raw;
}

QByteArray get(const QString &target)
{
    return requestLine(QStringLiteral("GET"), target, 0);
}

/// 造 §4.2 那份 multipart：payload + 任意张照片（filename 由调用方给，故意给坏的）
QByteArray multipartPost(const QString &target, const QJsonObject &payload,
                         const QList<QPair<QString, QByteArray>> &photos)
{
    const QByteArray boundary = "----TestBoundary";
    QByteArray body;
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"payload\"\r\n";
    body += "Content-Type: application/json\r\n\r\n";
    body += QJsonDocument(payload).toJson(QJsonDocument::Compact);
    body += "\r\n";
    for (const auto &photo : photos) {
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"photos\"; filename=\""
                + photo.first.toUtf8() + "\"\r\n";
        body += "Content-Type: image/jpeg\r\n\r\n";
        body += photo.second;
        body += "\r\n";
    }
    body += "--" + boundary + "--\r\n";
    return requestLine(QStringLiteral("POST"), target, body.size(),
                       QByteArray("multipart/form-data; boundary=") + boundary) + body;
}

AlarmRecord makeAlarm(const QString &id, const QString &taskId)
{
    AlarmRecord record;
    record.id = id;
    record.alarmType = QStringLiteral("detection");
    record.alarmLevel = 3;
    record.alarmTime = QStringLiteral("2026-11-05T09:00:00");
    record.updateTime = QStringLiteral("2026-11-05T09:00:00");
    record.channel = 0;
    record.clsId = 1;
    record.className = QStringLiteral("un_helmet");
    record.confidence = 0.8f;
    record.status = QStringLiteral("pending");
    record.taskId = taskId;
    record.createTime = record.alarmTime;
    return record;
}

/// 装一台服务端并返回它的实际端口。ctx 由调用方给（契约 6/7/16 要换 ctx 重装）。
quint16 bootServer(const pad::HttpApiContext &ctx, quint16 port)
{
    pad::PadHttpServer &server = pad::PadHttpServer::instance();
    server.stop();
    pad::registerPadHttpRoutes(&server, ctx);
    pad::HttpServerConfig config;
    config.port = port;
    config.deviceId = QStringLiteral("EDGE-TEST");
    QString error;
    if (!server.start(config, &error)) {
        std::cout << "  服务端起不来：" << error.toLocal8Bit().constData() << std::endl;
        return 0;
    }
    return server.port();
}

bool ask(quint16 port, const QByteArray &request, HttpResponse *out)
{
    HttpClient client;
    if (!client.open(port))
        return false;
    const bool ok = client.send(request, out);
    client.close();
    return ok;
}

}  // namespace

int main(int argc, char *argv[])
{
    // 契约 16 会真渲染围栏底图（QPainter + 字体度量）。QCoreApplication 下没有平台插件，
    // QFontDatabase::findFont 直接段错误（板端实测：gdb #0 在 libQt5Gui 里），
    // 所以和 tests/test_equipment.cpp:42 一样先钉 offscreen 再建 QGuiApplication。
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    std::cout << "=== test_pad_http_api ===" << std::endl;

    // ── 前置：临时库 + 临时配置 + 临时媒体根 ──────────────────────────
    QTemporaryDir workspace;
    check(workspace.isValid(), "前置：临时目录要能建出来");
    if (!workspace.isValid())
        return 1;
    const QString root = workspace.path();
    const QString dbPath = root + QStringLiteral("/idge_http_api_test.db");
    const QString mediaRoot = root + QStringLiteral("/alarms");
    QDir().mkpath(mediaRoot);

    // 一份最小 config：三路通道都引用**同一条**流 cam-a（这是现场真出现过的形状，
    // 也正是 cameraId 撞车的那个用例），外加一条没配流的第四路。
    {
        QFile config(QDir(root).filePath(QStringLiteral("config.json")));
        config.open(QIODevice::WriteOnly);
        config.write(R"({
  "video": {
    "sources": [
      {"id": "cam-a", "type": "rtsp", "url": "rtsp://admin:Aa%40123@10.0.0.9:554/live", "note": "网口摄像头"},
      {"id": "demo", "type": "file", "url": "file:///tmp/clip.mp4", "note": "本地片段"}
    ],
    "channels": [
      {"source_id": "cam-a", "note": "通道一"},
      {"source_id": "cam-a", "note": "通道二"},
      {"source_id": "demo", "note": "通道三"},
      {}
    ]
  }
})");
        config.close();
        ConfigManager::instance().load(QFileInfo(config).absoluteFilePath());
    }

    check(AlarmManager::instance().initDatabase(dbPath), "前置：报警库要能初始化");
    {
        AlarmDAO dao;
        const QString taskId = QStringLiteral("SMOKE-T1");
        dao.insertAlarm(makeAlarm(QStringLiteral("aa000000000000000000000000000001"), taskId));
        dao.insertAlarm(makeAlarm(QStringLiteral("bb000000000000000000000000000002"), taskId));
        dao.insertAlarm(makeAlarm(QStringLiteral("cc000000000000000000000000000003"), taskId));
    }

    pad::HttpApiContext ctx;
    ctx.deviceId = QStringLiteral("EDGE-TEST");
    ctx.mediaBaseUrl = QStringLiteral("http://127.0.0.1:9");   // 只用来拼地址，不真去取
    ctx.mediaRootDir = mediaRoot;
    ctx.fenceFrameDir = QStringLiteral("fence-frames");
    ctx.exposeStreamCredentials = false;
    ctx.channelOnline = [](int channel) { return channel == 0; };   // 只有通道一在线

    quint16 port = bootServer(ctx, 18899);
    check(port != 0, "前置：服务端要监听起来");
    if (port == 0)
        return 1;
    const QString base = QStringLiteral("http://127.0.0.1:%1/api").arg(port);
    Q_UNUSED(base);   // 客户端只发相对目标，Host 头由 requestLine 里那个 127.0.0.1 顶着

    // ── 1) health ──────────────────────────────────────────────────────
    {
        HttpResponse reply;
        check(ask(port, get(QStringLiteral("/api/health")), &reply) && reply.status == 200,
              "契约1：/api/health 要 200");
        const QJsonObject data = reply.json.value(QStringLiteral("data")).toObject();
        check(reply.code == 0 && data.value(QStringLiteral("ok")).toBool(),
              "契约1：信封 code=0 且 data.ok=true");
        const QJsonObject stream = data.value(QStringLiteral("capabilities")).toObject()
                                      .value(QStringLiteral("stream")).toObject();
        // 这两条是规约硬要求：startStop 恒 false（§6.1 无启停），maxRegions=8（§5.2 上限）
        check(stream.value(QStringLiteral("startStop")).toBool() == false,
              "契约1：capabilities.stream.startStop 必须 false");
        check(data.value(QStringLiteral("capabilities")).toObject()
                  .value(QStringLiteral("fence")).toObject()
                  .value(QStringLiteral("maxRegions")).toInt() == 8,
              "契约1：capabilities.fence.maxRegions=8");
    }

    // ── 2) device/status 用 snake_case ────────────────────────────────
    {
        HttpResponse reply;
        check(ask(port, get(QStringLiteral("/api/device/status")), &reply) && reply.status == 200,
              "契约2：/api/device/status 要 200");
        const QJsonObject data = reply.json.value(QStringLiteral("data")).toObject();
        // §2.6：整个规约只有这一节是 snake_case。混进 camelCase 就是"同一份数据两个名字"，
        // 对端读到的是没填充的那一个，界面显示"—"，而库里的数明明是对的。
        check(data.contains(QStringLiteral("device_id")), "契约2：device_id 用 snake_case");
        check(!data.contains(QStringLiteral("deviceId")), "契约2：不能混进 camelCase 的键");
        check(!data.contains(QStringLiteral("lastSeenAt")), "契约2：同上（last_seen_at 那一项）");
        check(data.value(QStringLiteral("cameras")).toArray().size()
                  == ConfigManager::instance().channelCount(),
              "契约2：cameras[] 一路一行");
        check(!data.value(QStringLiteral("device_id")).toString().isEmpty(),
              "契约2：device_id 必填（口径 A17）");
    }

    // ── 3/4/5/6/7) streams ─────────────────────────────────────────────
    {
        HttpResponse missing;
        check(ask(port, get(QStringLiteral("/api/streams")), &missing)
                  && missing.status == 400 && missing.code == 2001,
              "契约3：缺 taskId → 400/2001（§13.6.1 必填）");

        HttpResponse reply;
        check(ask(port, get(QStringLiteral("/api/streams?taskId=SMOKE-T1")), &reply)
                  && reply.status == 200,
              "契约4：带 taskId 要 200");
        const QJsonObject data = reply.json.value(QStringLiteral("data")).toObject();
        check(data.value(QStringLiteral("taskId")).toString() == QStringLiteral("SMOKE-T1"),
              "契约4：data.taskId 与请求一致（回显）");
        check(data.value(QStringLiteral("streams")).isArray()
                  && data.value(QStringLiteral("streams")).toArray().size() == 4,
              "契约4：streams 恒数组、一路一条");

        const QJsonArray streams = data.value(QStringLiteral("streams")).toArray();
        const QJsonObject first = streams.at(0).toObject();
        check(first.value(QStringLiteral("streamId")).toString() == QStringLiteral("CH1"),
              "契约4：streamId 按通道号派生");
        // 契约 7：通道一与通道二引用的是**同一条**注册表流。cameraId 若用注册表 id，
        // 两条都会是 cam-a → §5.1 的 (taskId,cameraId) 唯一键撞车。这里钉住"用通道号"。
        check(first.value(QStringLiteral("cameraId")).toString() == QStringLiteral("CH1"),
              "契约7：cameraId 是通道号口径，不是注册表 id");
        check(streams.at(1).toObject().value(QStringLiteral("cameraId")).toString()
                  == QStringLiteral("CH2"),
              "契约7：两路引用同一条流时 cameraId 也不撞车");
        // 契约 5：只有通道一在线（channelOnline 给的判据），它的地址必须非空
        check(first.value(QStringLiteral("status")).toString() == QStringLiteral("ONLINE")
                  && !first.value(QStringLiteral("liveUrl")).toString().isEmpty()
                  && first.value(QStringLiteral("liveProtocol")).toString() == QStringLiteral("RTSP"),
              "契约5：ONLINE 那路 liveUrl 非空、liveProtocol=RTSP");
        // 契约 6：口令不能出门
        check(!first.value(QStringLiteral("liveUrl")).toString().contains(QStringLiteral("admin")),
              "契约6：liveUrl 默认剥掉视频源口令（admin 不该出现）");
        check(!first.value(QStringLiteral("liveUrl")).toString().contains(QStringLiteral("Aa%40123")),
              "契约6：转义写法的口令同样要剥掉");
        check(first.value(QStringLiteral("liveUrl")).toString()
                  == QStringLiteral("rtsp://10.0.0.9:554/live"),
              "契约6：剥完剩下 host:port/path，地址本身仍然是对的");
        // 本地文件那一路：不是对端能取的东西 → 不给地址、报 ERROR
        const QJsonObject third = streams.at(2).toObject();
        check(third.value(QStringLiteral("status")).toString() == QStringLiteral("ERROR")
                  && third.value(QStringLiteral("liveUrl")).toString().isEmpty(),
              "契约5：本地文件源报 ERROR 且不给 liveUrl（不给对端取不到的地址）");

        // ctx 打开 expose_stream_credentials → 重装路由，口令就该留着
        pad::HttpApiContext exposeCtx = ctx;
        exposeCtx.exposeStreamCredentials = true;
        check(bootServer(exposeCtx, port) != 0, "契约6 前置：换 ctx 重装路由要成功");
        HttpResponse kept;
        check(ask(port, get(QStringLiteral("/api/streams?taskId=SMOKE-T1")), &kept)
                  && kept.json.value(QStringLiteral("data")).toObject()
                         .value(QStringLiteral("streams")).toArray().at(0).toObject()
                         .value(QStringLiteral("liveUrl")).toString()
                         .contains(QStringLiteral("admin")),
              "契约6：显式打开 expose_stream_credentials 时才原样给（可配，不是默认）");
        check(bootServer(ctx, port) != 0, "契约6 收尾：装回默认 ctx");
    }

    // ── 8/9) 流状态与未知路径 ──────────────────────────────────────────
    {
        HttpResponse ok;
        check(ask(port, get(QStringLiteral("/api/streams/CH1/status")), &ok) && ok.status == 200
                  && ok.json.value(QStringLiteral("data")).toObject()
                         .contains(QStringLiteral("status")),
              "契约8：/api/streams/CH1/status → 200 且带 status");
        HttpResponse gone;
        check(ask(port, get(QStringLiteral("/api/streams/CH9/status")), &gone)
                  && gone.status == 404 && gone.code == 2002,
              "契约8：本机没这条流 → 404/2002");
        HttpResponse nope;
        check(ask(port, get(QStringLiteral("/api/nope")), &nope)
                  && nope.status == 404 && nope.code == 2002
                  && nope.json.value(QStringLiteral("detail")).toString().contains(QStringLiteral("/api/nope")),
              "契约9：未知路径 → 404/2002，detail 里要带上方法+路径");
    }

    // ── 10) dispose-options ───────────────────────────────────────────
    {
        HttpResponse reply;
        check(ask(port, get(QStringLiteral("/api/dispose-options?alarmType=HELMET")), &reply)
                  && reply.status == 200,
              "契约10：/api/dispose-options 要 200（GET 不带 requestId）");
        const QJsonArray options = reply.json.value(QStringLiteral("data")).toObject()
                                       .value(QStringLiteral("options")).toArray();
        check(!options.isEmpty(), "契约10：选项不能是空表（Pad 拿到空表就没得选）");
        int previous = -1;
        bool sorted = true;
        for (const QJsonValue &item : options) {
            const int order = item.toObject().value(QStringLiteral("sortOrder")).toInt();
            if (order < previous)
                sorted = false;
            previous = order;
            check(!item.toObject().value(QStringLiteral("label")).toString().isEmpty(),
                  "契约10：每项都要有 label（提交时就是它当 measure）");
        }
        check(sorted, "契约10：按 sortOrder 升序");
    }

    // ── 11/12/14/15) save-draft 的新建、幂等、非法、冲突 ───────────────
    const auto regionRect = [](const QString &regionId) {
        QJsonArray points;
        const double coords[4][2] = {{0.1, 0.1}, {0.3, 0.1}, {0.3, 0.3}, {0.1, 0.3}};
        for (int i = 0; i < 4; ++i) {
            QJsonArray pair;
            pair.append(coords[i][0]);
            pair.append(coords[i][1]);
            points.append(pair);
        }
        QJsonObject region;
        region.insert(QStringLiteral("regionId"), regionId);
        region.insert(QStringLiteral("shapeType"), QStringLiteral("RECT"));
        region.insert(QStringLiteral("points"), points);
        region.insert(QStringLiteral("ruleType"), QStringLiteral("INTRUSION"));
        return region;
    };
    // 一份**合法**的 save-draft body：fenceId 空 = 新建，非空 = 编辑草稿/复制
    const auto draftBody = [&](const QString &requestId, const QString &fenceId) {
        QJsonObject body;
        body.insert(QStringLiteral("requestId"), requestId);
        if (!fenceId.isEmpty())
            body.insert(QStringLiteral("fenceId"), fenceId);
        body.insert(QStringLiteral("taskId"), QStringLiteral("SMOKE-T1"));
        body.insert(QStringLiteral("cameraId"), QStringLiteral("CH1"));
        body.insert(QStringLiteral("streamId"), QStringLiteral("CH1"));
        body.insert(QStringLiteral("name"), QStringLiteral("测试围栏"));
        body.insert(QStringLiteral("regions"), QJsonArray{regionRect(QStringLiteral("RG-TEST-1"))});
        QJsonObject plan;
        plan.insert(QStringLiteral("taskId"), QStringLiteral("SMOKE-T1"));
        body.insert(QStringLiteral("plan"), plan);
        QJsonObject owner;
        owner.insert(QStringLiteral("id"), QStringLiteral("05-0101"));
        owner.insert(QStringLiteral("name"), QStringLiteral("张伟"));
        body.insert(QStringLiteral("owner"), owner);
        return body;
    };

    QString fenceId;
    {
        HttpResponse created;
        check(ask(port, jsonPost(QStringLiteral("/api/fences/save-draft"),
                                 draftBody(QStringLiteral("req-create-1"), QString())), &created)
                  && created.status == 200,
              "契约11：save-draft 新建要 200");
        const QJsonObject data = created.json.value(QStringLiteral("data")).toObject();
        fenceId = data.value(QStringLiteral("fenceId")).toString();
        check(!fenceId.isEmpty() && data.value(QStringLiteral("version")).toInt() == 1
                  && data.value(QStringLiteral("status")).toString() == QStringLiteral("DRAFT")
                  && !data.value(QStringLiteral("updatedAt")).toString().isEmpty(),
              "契约11：响应是扁平结果（fenceId/version/status/updatedAt，不含形状）");

        // 契约 12：同 requestId 重放必须**逐字节**相同，且不能再建一个版本
        HttpResponse replay;
        check(ask(port, jsonPost(QStringLiteral("/api/fences/save-draft"),
                                 draftBody(QStringLiteral("req-create-1"), QString())), &replay)
                  && replay.body == created.body,
              "契约12：同 requestId 重放的响应逐字节相同（§2.5）");
        HttpResponse list;
        check(ask(port, get(QStringLiteral("/api/fences?taskId=SMOKE-T1")), &list)
                  && list.json.value(QStringLiteral("data")).toObject()
                         .value(QStringLiteral("fences")).toArray().size() == 1,
              "契约12：重放没有多建一条围栏");

        // 契约 14：RECT 只给 3 个点 → 422/4001
        QJsonObject bad = draftBody(QStringLiteral("req-bad-1"), QString());
        QJsonArray regions = bad.value(QStringLiteral("regions")).toArray();
        QJsonObject rect = regions.at(0).toObject();
        QJsonArray points = rect.value(QStringLiteral("points")).toArray();
        points.removeLast();
        rect.insert(QStringLiteral("points"), points);
        regions[0] = rect;
        bad.insert(QStringLiteral("regions"), regions);
        HttpResponse invalid;
        check(ask(port, jsonPost(QStringLiteral("/api/fences/save-draft"), bad), &invalid)
                  && invalid.status == 422 && invalid.code == 4001,
              "契约14：RECT 顶点数不对 → 422/4001");

        // 契约 15：已有草稿（v1 还没下发）时再新建同 (taskId,cameraId) → 2004
        HttpResponse clash;
        check(ask(port, jsonPost(QStringLiteral("/api/fences/save-draft"),
                                 draftBody(QStringLiteral("req-clash-1"), QString())), &clash)
                  && clash.status == 409 && clash.code == 2004,
              "契约15：同一摄像头已有草稿再建 → 409/2004");

        // 契约 14b：缺 requestId → 400/2001（这是所有写接口的共同门）
        QJsonObject noId = draftBody(QString(), QString());
        noId.remove(QStringLiteral("requestId"));
        HttpResponse missingId;
        check(ask(port, jsonPost(QStringLiteral("/api/fences/save-draft"), noId), &missingId)
                  && missingId.status == 400 && missingId.code == 2001,
              "契约14b：写接口缺 requestId → 400/2001");
    }

    // ── 16) apply：底址没配 → 503；配了 → 200 + PNG 真在 ───────────────
    {
        pad::HttpApiContext noMedia = ctx;
        noMedia.mediaBaseUrl.clear();
        noMedia.mediaRootDir.clear();
        check(bootServer(noMedia, port) != 0, "契约16 前置：装一台没有媒体底址的");
        HttpResponse blocked;
        check(ask(port, jsonPost(QStringLiteral("/api/fences/%1/apply").arg(fenceId),
                                 QJsonObject{{QStringLiteral("requestId"), QStringLiteral("req-apply-0")},
                                            {QStringLiteral("version"), 1}}), &blocked)
                  && blocked.status == 503 && blocked.code == 3003,
              "契约16：出不了 frameUrl 时 apply 回 503/3003，而不是回一个取不到的地址");
        check(bootServer(ctx, port) != 0, "契约16 前置：装回有底址的那台");

        HttpResponse applied;
        check(ask(port, jsonPost(QStringLiteral("/api/fences/%1/apply").arg(fenceId),
                                 QJsonObject{{QStringLiteral("requestId"), QStringLiteral("req-apply-1")},
                                            {QStringLiteral("version"), 1}}), &applied)
                  && applied.status == 200,
              "契约16：apply 草稿要 200");
        const QJsonObject data = applied.json.value(QStringLiteral("data")).toObject();
        check(data.value(QStringLiteral("status")).toString() == QStringLiteral("ACTIVE")
                  && !data.value(QStringLiteral("appliedAt")).toString().isEmpty()
                  && data.contains(QStringLiteral("frameUrl")),
              "契约16：响应带 status/appliedAt/frameUrl（§5.3 必填）");
        const QString frameUrl = data.value(QStringLiteral("frameUrl")).toString();
        check(frameUrl.contains(QStringLiteral("/fence-frames/"))
                  && frameUrl.contains(QStringLiteral("v1.png")),
              "契约16：frameUrl 形状对上 §9 那张表的 /fence-frames/{fenceId}/v{n}.png");
        const QString png = QDir(mediaRoot).filePath(
            QStringLiteral("fence-frames/%1/v1.png").arg(fenceId));
        check(QFileInfo::exists(png) && QFileInfo(png).size() > 1000,
              "契约16：那张 PNG 真在磁盘上（不是只回了个地址）");

        // 契约 17：生效版本不能删草稿、不能停用"非生效"
        HttpResponse deleteActive;
        check(ask(port, jsonPost(QStringLiteral("/api/fences/%1/delete-draft").arg(fenceId),
                                 QJsonObject{{QStringLiteral("requestId"), QStringLiteral("req-del-1")},
                                            {QStringLiteral("version"), 1}}), &deleteActive)
                  && deleteActive.status == 409 && deleteActive.code == 2005,
              "契约17：生效版本不是草稿 → 409/2005，不能删");
        HttpResponse disableDraft;
        check(ask(port, jsonPost(QStringLiteral("/api/fences/%1/disable").arg(fenceId),
                                 QJsonObject{{QStringLiteral("requestId"), QStringLiteral("req-dis-1")},
                                            {QStringLiteral("version"), 3}}), &disableDraft)
                  && disableDraft.status == 404 && disableDraft.code == 2002,
              "契约17：不存在的版本号 → 404/2002（不是 2005，方向要指对）");
        HttpResponse disabled;
        check(ask(port, jsonPost(QStringLiteral("/api/fences/%1/disable").arg(fenceId),
                                 QJsonObject{{QStringLiteral("requestId"), QStringLiteral("req-dis-2")},
                                            {QStringLiteral("version"), 1}}), &disabled)
                  && disabled.status == 200
                  && disabled.json.value(QStringLiteral("data")).toObject()
                             .value(QStringLiteral("status")).toString() == QStringLiteral("DISABLED"),
              "契约17：停用生效版本 → DISABLED（终态）");
        HttpResponse applyDisabled;
        check(ask(port, jsonPost(QStringLiteral("/api/fences/%1/apply").arg(fenceId),
                                 QJsonObject{{QStringLiteral("requestId"), QStringLiteral("req-apply-2")},
                                            {QStringLiteral("version"), 1}}), &applyDisabled)
                  && applyDisabled.status == 409 && applyDisabled.code == 2005,
              "契约17：DISABLED 是终态，再下发 → 409/2005");
    }

    // ── 13/18/19/20) 报警那四条 + keep-alive ──────────────────────────
    {
        const QString readId = QStringLiteral("aa000000000000000000000000000001");
        HttpResponse read;
        check(ask(port, jsonPost(QStringLiteral("/api/alarms/%1/read").arg(readId),
                                 QJsonObject{{QStringLiteral("requestId"), QStringLiteral("req-read-1")},
                                            {QStringLiteral("taskId"), QStringLiteral("SMOKE-T1")},
                                            {QStringLiteral("readAt"), QStringLiteral("2026-11-05T08:00:00+08:00")}}),
                          &read)
                  && read.status == 200,
              "契约18：/read 要 200");
        // 对端报的 08:00 比本机此刻早，"保留最早"就该用 08:00（不是收请求的那一刻）
        check(read.json.value(QStringLiteral("data")).toObject()
                  .value(QStringLiteral("readAt")).toString().startsWith(QStringLiteral("2026-11-05T08:00")),
              "契约18：readAt 用对端报的那个更早的值（§4.4 保留最早）");

        const QString disposeId = QStringLiteral("bb000000000000000000000000000002");
        QJsonObject payload;
        payload.insert(QStringLiteral("requestId"), QStringLiteral("req-dispose-1"));
        payload.insert(QStringLiteral("taskId"), QStringLiteral("SMOKE-T1"));
        payload.insert(QStringLiteral("measure"), QStringLiteral("已现场纠正"));
        payload.insert(QStringLiteral("personId"), QStringLiteral("05-0101"));
        payload.insert(QStringLiteral("disposedAt"), QStringLiteral("2026-11-05T09:30:00+08:00"));
        HttpResponse disposed;
        check(ask(port, multipartPost(QStringLiteral("/api/alarms/%1/dispose").arg(disposeId),
                                     payload,
                                     {{QStringLiteral("../../etc/passwd"), QByteArray::fromHex("ffd8ffe0")},
                                      {QStringLiteral("ok.jpg"), QByteArray::fromHex("ffd8ffdb")}}),
                          &disposed)
                  && disposed.status == 200,
              "契约13：multipart 处置要 200");
        const QJsonObject disposeData = disposed.json.value(QStringLiteral("data")).toObject();
        check(disposeData.value(QStringLiteral("status")).toString() == QStringLiteral("DISPOSED")
                  && disposeData.value(QStringLiteral("syncState")).toString()
                          == QStringLiteral("SUBMITTED"),
              "契约13：DISPOSED + syncState=SUBMITTED（本机没有上传链路，不能报 UPLOADED）");
        const QJsonArray photoUrls = disposeData.value(QStringLiteral("dispose")).toObject()
                                        .value(QStringLiteral("photoUrls")).toArray();
        check(photoUrls.size() == 2, "契约13：两张照片都出了地址");
        // 关键安全判据：对端给的 filename 一个字都不能进路径。
        // 这里**不**去断言"/etc/passwd 还在"（它本来就与我写的目录无关，断言恒真），
        // 判据是落盘文件名必须是我自己生成的 dispose1/dispose2 那两条。
        const QString storedDir = QDir(mediaRoot).filePath(QStringLiteral("dispose/%1").arg(disposeId));
        const QStringList entries = QDir(storedDir).entryList(QDir::Files);
        check(entries.size() == 2, "契约13：照片落在 dispose/<alarmId>/ 下、正好两张");
        bool namesAreOurs = true;
        for (const QString &entry : entries) {
            if (!entry.startsWith(QStringLiteral("dispose")))
                namesAreOurs = false;
        }
        check(namesAreOurs, "契约13：文件名由本机生成（dispose1.jpg/dispose2.jpg），不是对端的 filename");
        check(!QDir(mediaRoot).entryList(QDir::Files).contains(QStringLiteral("passwd")),
              "契约13：坏 filename 没有在媒体根里留下任何同名文件");
        {
            AlarmDAO dao;
            const AlarmRecord after = dao.queryById(disposeId);
            check(after.disposePersonId == QStringLiteral("05-0101"),
                  "契约13：personId 原文落进 dispose_person_id（不折成数字）");
            check(after.disposePhoto.startsWith(QLatin1Char('[')),
                  "契约13：dispose_photo 是 JSON 数组文本");
            check(after.status == QStringLiteral("disposed"), "契约13：状态落成 disposed");
        }
        HttpResponse againDispose;
        check(ask(port, jsonPost(QStringLiteral("/api/alarms/%1/dispose").arg(disposeId),
                                 QJsonObject{{QStringLiteral("requestId"), QStringLiteral("req-dispose-2")},
                                            {QStringLiteral("taskId"), QStringLiteral("SMOKE-T1")},
                                            {QStringLiteral("measure"), QStringLiteral("再处置一次")},
                                            {QStringLiteral("disposedAt"), QStringLiteral("2026-11-05T09:31:00+08:00")}}),
                          &againDispose)
                  && againDispose.status == 409 && againDispose.code == 2003,
              "契约19：已处置再处置 → 409/2003（§4.2 终态不回退）");

        const QString falseId = QStringLiteral("cc000000000000000000000000000003");
        HttpResponse marked;
        check(ask(port, jsonPost(QStringLiteral("/api/alarms/%1/false-alarm").arg(falseId),
                                 QJsonObject{{QStringLiteral("requestId"), QStringLiteral("req-false-1")},
                                            {QStringLiteral("taskId"), QStringLiteral("SMOKE-T1")},
                                            {QStringLiteral("markedAt"), QStringLiteral("2026-11-05T09:40:00+08:00")}}),
                          &marked)
                  && marked.status == 200
                  && marked.json.value(QStringLiteral("data")).toObject()
                             .value(QStringLiteral("status")).toString() == QStringLiteral("FALSE_ALARM"),
              "契约19：误报成功回 FALSE_ALARM");
        HttpResponse markedAgain;
        check(ask(port, jsonPost(QStringLiteral("/api/alarms/%1/false-alarm").arg(falseId),
                                 QJsonObject{{QStringLiteral("requestId"), QStringLiteral("req-false-2")},
                                            {QStringLiteral("taskId"), QStringLiteral("SMOKE-T1")},
                                            {QStringLiteral("markedAt"), QStringLiteral("2026-11-05T09:41:00+08:00")}}),
                          &markedAgain)
                  && markedAgain.status == 409 && markedAgain.code == 2003,
              "契约19：再标误报 → 409/2003");

        HttpResponse ghost;
        check(ask(port, jsonPost(QStringLiteral("/api/alarms/ffffffffffffffffffffffffffffffff/read"),
                                 QJsonObject{{QStringLiteral("requestId"), QStringLiteral("req-ghost")},
                                            {QStringLiteral("taskId"), QStringLiteral("SMOKE-T1")},
                                            {QStringLiteral("readAt"), QStringLiteral("2026-11-05T10:00:00+08:00")}}),
                          &ghost)
                  && ghost.status == 404 && ghost.code == 2002,
              "契约19：库里没这条报警 → 404/2002");

        // 契约 19b：路径穿越的 alarmId 要当场拒（它会进照片落盘的路径）
        HttpResponse traversal;
        check(ask(port, jsonPost(QStringLiteral("/api/alarms/..%2f..%2fetc/read"),
                                 QJsonObject{{QStringLiteral("requestId"), QStringLiteral("req-x")}}),
                          &traversal)
                  && (traversal.status == 400 || traversal.status == 404),
              "契约19b：带编码斜杠的 alarmId 不能被当路径用（400 或 404，但不能写盘）");
    }

    // ── 20) keep-alive：一条连接三个请求 ──────────────────────────────
    {
        HttpClient client;
        check(client.open(port), "契约20：连接要建起来");
        HttpResponse first;
        HttpResponse second;
        HttpResponse third;
        const bool one = client.send(get(QStringLiteral("/api/health")), &first);
        const bool two = client.send(get(QStringLiteral("/api/dispose-options")), &second);
        const bool three = client.send(get(QStringLiteral("/api/health")), &third);
        check(one && two && three && first.status == 200 && second.status == 200
                  && third.status == 200,
              "契约20：同一条连接上连发三个请求要各自收到完整响应（Pad 的连接池不会为每个请求重连）");
        client.close();
    }

    // ── 21) 处置必须连内存窗口那份副本一起改（关机回写不撤销处置）────────
    {
        // 这条钉的是本模块最隐蔽的一个失效方向：AlarmManager 的 RAM 窗口里那 1000 条
        // 与库里的行是**同一批记录的两个副本**，关机时 syncToDatabase() 会把窗口整批
        // INSERT OR REPLACE 回去。处置只改库不改窗口，第二天早上就"神秘复原"
        // ——与 B2 那次 task_id 回填被打回 NULL 是同一个病灶。
        const QString ramId = QStringLiteral("dd000000000000000000000000000004");
        AlarmManager::instance().storeAndNotify(
            QVector<AlarmRecord>{makeAlarm(ramId, QStringLiteral("SMOKE-T1"))});
        QString error;
        bool landed = false;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 3000) {
            QCoreApplication::processEvents();   // 落库在 AlarmDbWriter 那条后台线程上
            AlarmDAO probe;
            if (probe.queryById(ramId).id == ramId) {
                landed = true;
                break;
            }
            QThread::msleep(5);
        }
        check(landed, "契约21 前置：报警要已经落进库里（后台写线程排干了）");
        QString disposeError;
        const bool disposed = AlarmManager::instance().disposeAlarmById(
            ramId, QStringLiteral("已现场纠正"), QStringLiteral("补充"),
            QStringLiteral("05-0101"), QStringLiteral("张伟"), QStringLiteral("[]"),
            QString(), 0, &disposeError);
        check(disposed, qPrintable(QStringLiteral("契约21：disposeAlarmById 要成功（%1）")
                                       .arg(disposeError)));
        AlarmManager::instance().syncToDatabase();   // 模拟关机整批回写
        AlarmDAO dao;
        const AlarmRecord after = dao.queryById(ramId);
        check(after.status == QStringLiteral("disposed")
                  && after.disposeResult == QStringLiteral("已现场纠正"),
              "契约21：关机回写之后处置还在（内存副本没被漏改就是这条）");
        // ⚠ 这一条是本轮审查抓出来的真窟窿：dispose_person_id 只在 UPDATE 里出现过，
        // 两个 INSERT 的列表都没它，而 syncToDatabase() 用的是 INSERT OR REPLACE
        // （语义=删旧行再插新行）——于是处置完的第二天早上工号被静默清成 NULL，
        // 库里只剩姓名，v1 §13.2.3 的 dispose.personId 对不上人。症状是"过一夜就丢"，
        // 白天怎么点都正常，所以必须钉在**回写之后**而不是回写之前。
        check(after.disposePersonId == QStringLiteral("05-0101"),
              qPrintable(QStringLiteral("契约21b：关机回写之后处置人**工号**还在（实为「%1」）")
                             .arg(after.disposePersonId)));
    }

    // ========== 契约22：溯源六列要能整圈活过"落库→回读→回写→再回读" ==========
    // 这六列是 2026-11-06 按 §13.2.1 加的。它们和 dispose_* 一样会经过
    // syncToDatabase() 的 INSERT OR REPLACE，所以少写一个绑定就是"过一夜被抹平"，
    // 而且是**没有任何报错**的那种。这里把四个键的往返一次验完。
    {
        AlarmRecord stamped;
        stamped.id = QStringLiteral("STAMP-ROUNDTRIP");
        stamped.alarmType = QStringLiteral("fence_in");
        stamped.alarmLevel = 2;
        stamped.alarmTime = QStringLiteral("2026-11-06T10:00:00");
        stamped.channel = 1;
        stamped.className = QStringLiteral("person");
        stamped.confidence = 0.9f;
        stamped.status = QStringLiteral("pending");
        stamped.createTime = stamped.alarmTime;
        stamped.updateTime = stamped.alarmTime;
        stamped.rule = QStringLiteral("禁区闯入检测");
        stamped.modelVersion = QStringLiteral("yolo11n-ppe@2026-10-21");
        stamped.fenceId = QStringLiteral("NFENCE-001");
        stamped.fenceVersion = 3;
        stamped.regionId = QStringLiteral("RG-7F3A21C9");
        stamped.disposeOptionId = 0;

        AlarmDAO dao;
        check(dao.insertAlarm(stamped) == 1, "契约22：带溯源的报警没插进去");
        const AlarmRecord first = dao.queryById(QStringLiteral("STAMP-ROUNDTRIP"));
        check(first.rule == stamped.rule && first.modelVersion == stamped.modelVersion
                  && first.fenceId == stamped.fenceId
                  && first.fenceVersion == stamped.fenceVersion
                  && first.regionId == stamped.regionId,
              "契约22：溯源六列回读不一致");
        // 关键一步：拿**回读**的那份再走一次批量 REPLACE（就是关机时发生的事）
        QVector<AlarmRecord> batch;
        batch << first;
        AlarmManager::instance().syncToDatabase();
        check(dao.insertAlarms(batch) == 1, "契约22：整批回写没执行成功");
        const AlarmRecord again = dao.queryById(QStringLiteral("STAMP-ROUNDTRIP"));
        check(again.rule == stamped.rule && again.modelVersion == stamped.modelVersion
                  && again.fenceId == stamped.fenceId
                  && again.fenceVersion == stamped.fenceVersion
                  && again.regionId == stamped.regionId,
              qPrintable(QStringLiteral("契约22：整批回写之后溯源列被抹平了（rule=「%1」 fenceVersion=%2）")
                             .arg(again.rule).arg(again.fenceVersion)));
    }

    pad::PadHttpServer::instance().stop();
    DatabaseManager::instance().close();

    std::cout << (failures == 0 ? "PASS" : "FAIL") << " (" << failures << " 条失败)" << std::endl;
    return failures == 0 ? 0 : 1;
}
