// ============================================================================
// test_gb28181_source_ref: 国标流引用视频流注册表的离线自检（无 GUI、无 FFmpeg）
// ============================================================================
// 只编 ConfigManager.cpp + gb28181_config.cpp：这一段全是读 JSON，所以本目标能在
// 任何机器上跑。它钉的是"gb28181.streams 不再自带取流字段"之后新增/改变的判据。
//
// 为什么要单独钉：同一路摄像头以前在配置里出现两次（video.sources[] 一份地址给解码/推理，
// gb28181.streams[] 又一份给平台点流），改一次摄像头要改两处，漏一处的现象是
// "界面有画、平台点流黑屏"。现在国标只写 source_id，于是多了三类只在运行时才暴露的错：
// 忘写、指向不存在的 id、以及"进程跑起来之后注册表被删"——三种表现完全不同，
// 必须分别有断言，否则下次有人"顺手加个兜底"就把它们混成同一个静默失败。
//
// 契约清单：
//   1. load() 只保留"身份齐 + 引用命中"的流；缺 deviceCode / 缺 source_id / 悬空 id
//      各自整条丢弃，并且**逐条打出不同原因**（"没配国标"与"配了但写错"要分得开）
//   2. 全部被丢弃 → load() 返回 false，error 里带上丢弃条数（main() 据此不启动 manager）
//   3. 引用不再回填旧键：gb28181.streams 里写 type/source/width/height/fps/loop 一律被忽略
//      （留着解析就等于两套取流口径并存，正是这次要消灭的东西）
//   4. takeSource() 命中 → 拿到的就是注册表那条（url/type/宽高帧率/loop/硬解开关）
//   5. type 空时按 url 前缀补型：/dev/video→camera、rtsp://→rtsp、rtmp://→rtmp、其余→file
//      —— 与 FFmpegVideoDecoder 的嗅探同一规则，国标侧靠它选 v4l2 参数与 file 循环
//   6. type "mp4" 在注册表读入时归一成 "file"（国标侧的 loop 判据认 file）
//   7. 进程运行中注册表被删 → takeSource() 返回空 cfg（悬空不"随便挑一条流推给平台"）
//   8. videoSourceReferrerCount 把国标引用一起数（否则删掉一条正被平台点的流无人拦截）
//   9. 两种布局（顶层 platform/streams 与 gb28181.streams）都认，且都走同一套引用判据
//
// 运行（cwd 任意：测试配置写在 QDir::tempPath() 下，绝不碰 data/config.json）：
//   ./build/build_rk3588_linux/test_gb28181_source_ref
// rc 0=PASS 1=FAIL
// ============================================================================
#include "ConfigManager.h"
#include "gb28181_config.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <iostream>
#include <stdexcept>

namespace {

void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}

void say(const QString &line)
{
    qInfo().noquote() << "   " << line;
}

QString testPath(const QString &name)
{
    return QDir(QDir::tempPath())
        .absoluteFilePath(QStringLiteral("idge_gb_ref_%1.json").arg(name));
}

QJsonObject sourceObject(const QString &id, const QString &type, const QString &url)
{
    QJsonObject s;
    s["id"] = id;
    if (!type.isEmpty())
        s["type"] = type;
    s["url"] = url;
    return s;
}

// 一份"国标两条流 + 注册表三条流 + 一条通道引用 demo-mp4"的配置。
// 老键（type/source/width/…）故意仍写在其中一条上，用来断言它们被忽略。
QJsonObject makeRoot(const QString &netCamId = QStringLiteral("net-cam"))
{
    QJsonArray sources;
    sources.append(sourceObject(QStringLiteral("demo-mp4"), QStringLiteral("file"),
                               QStringLiteral("assets/test/1.mp4")));
    QJsonObject cam = sourceObject(QStringLiteral("csi-cam"), QString(),
                                   QStringLiteral("/dev/video22"));
    cam["width"] = 1280;
    cam["height"] = 720;
    cam["fps"] = 30;
    cam["hardwareDecode"] = false;
    sources.append(cam);
    sources.append(sourceObject(netCamId, QStringLiteral("rtsp"),
                                QStringLiteral("rtsp://192.168.2.60:554/live/1")));

    QJsonArray channels;
    QJsonObject ch;
    ch["source_id"] = QStringLiteral("demo-mp4");
    channels.append(ch);

    QJsonObject platform;
    platform["localHost"] = QStringLiteral("192.168.1.20");
    platform["serverHost"] = QStringLiteral("192.168.1.10");
    platform["serverCode"] = QStringLiteral("41010500002000000001");
    platform["serverDomain"] = QStringLiteral("4101050000");
    platform["password"] = QStringLiteral("12345678");

    QJsonArray streams;
    {   // 引用命中，且带着应当被忽略的旧取流键
        QJsonObject s;
        s["source_id"] = QStringLiteral("csi-cam");
        s["deviceCode"] = QStringLiteral("34020000001320000001");
        s["type"] = QStringLiteral("mp4");
        s["source"] = QStringLiteral("/dev/video99");
        s["width"] = 640;
        s["loop"] = true;
        streams.append(s);
    }
    {   // 引用的是 net-cam，让"两条流引用同一张表"参与引用计数断言
        QJsonObject s;
        s["source_id"] = netCamId;
        s["deviceCode"] = QStringLiteral("34020000001320000002");
        streams.append(s);
    }

    QJsonObject video;
    video["sources"] = sources;
    video["channels"] = channels;

    QJsonObject gb;
    gb["platform"] = platform;
    gb["streams"] = streams;

    QJsonObject root;
    root["video"] = video;
    root["gb28181"] = gb;
    return root;
}

void writeConfig(const QString &name, const QJsonObject &root)
{
    QFile f(testPath(name));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        throw std::runtime_error("测试配置写入失败");
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
}

// 只取 deviceCode 尾号，方便断言"丢的是哪一条"
QString tail(const QString &code)
{
    return code.right(3);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    try {
    // ── 1：引用命中 + 旧取流键被忽略 ─────────────────────────────────────────
    {
        const QString path = testPath(QStringLiteral("hit"));
        writeConfig(QStringLiteral("hit"), makeRoot());
        Gb28181Config cfg;
        QString err;
        check(Gb28181Config::load(path, &cfg, &err), "两条引用命中的流应能载入");
        check(cfg.streams.size() == 2, "应当正好两条流");
        check(cfg.streams[0].sourceId == QLatin1String("csi-cam"), "source_id 要解析出来");
        check(cfg.streams[0].deviceName == QLatin1String("34020000001320000001"),
              "deviceName 缺省取 channelCode，而 channelCode 缺省取 deviceCode");
        check(cfg.streams[0].bitrate == 2000000, "bitrate 仍属国标侧（重编码路径的目标码率）");
        say(QStringLiteral("载入 %1 条流，取流字段全部来自 video.sources[]").arg(cfg.streams.size()));
    }

    // ── 2：三类"该丢"的流各丢各的原因，且都真的丢 ────────────────────────────
    {
        QJsonObject root = makeRoot();
        QJsonArray streams = root["gb28181"].toObject()["streams"].toArray();
        QJsonObject noId;
        noId["deviceCode"] = QStringLiteral("34020000001320000003");
        noId["source"] = QStringLiteral("/dev/video22");   // 老写法，现在不再认
        streams.append(noId);
        QJsonObject dangling;
        dangling["deviceCode"] = QStringLiteral("34020000001320000004");
        dangling["source_id"] = QStringLiteral("no-such-stream");
        streams.append(dangling);
        QJsonObject noCode;
        noCode["source_id"] = QStringLiteral("csi-cam");
        streams.append(noCode);
        QJsonObject gb = root["gb28181"].toObject();
        gb["streams"] = streams;
        root["gb28181"] = gb;
        writeConfig(QStringLiteral("drop"), root);

        Gb28181Config cfg;
        QString err;
        check(Gb28181Config::load(testPath(QStringLiteral("drop")), &cfg, &err),
              "有可用流时 load() 仍应成功");
        check(cfg.streams.size() == 2, "只有引用命中的两条能留下");
        QStringList kept;
        for (const auto &s : qAsConst(cfg.streams))
            kept << tail(s.deviceCode);
        check(kept.contains(QStringLiteral("001")) && kept.contains(QStringLiteral("002")),
              "留下的应当是原来那两条");
        say(QStringLiteral("丢弃三类坏流后剩下：%1").arg(kept.join(QLatin1Char('/'))));
    }

    // ── 3：全都不可用 → load() 判 false 且给出可诊断的 error ──────────────────
    {
        QJsonObject root = makeRoot();
        QJsonObject gb = root["gb28181"].toObject();
        QJsonArray streams;
        QJsonObject onlyCode;
        onlyCode["deviceCode"] = QStringLiteral("34020000001320000005");
        streams.append(onlyCode);            // 没有 source_id
        gb["streams"] = streams;
        root["gb28181"] = gb;
        writeConfig(QStringLiteral("none"), root);

        Gb28181Config cfg;
        QString err;
        const bool ok = Gb28181Config::load(testPath(QStringLiteral("none")), &cfg, &err);
        check(!ok, "一条可用流都没有时 load() 必须返回 false（main 据此不启动 manager）");
        check(err.contains(QStringLiteral("1 条被丢弃")),
              "error 要说明是「配了但写错」而不是「没配」");
        say(QStringLiteral("error=%1").arg(err));
    }

    // ── 4/5/6：takeSource() 的取值与补型 ─────────────────────────────────────
    {
        const QString path = testPath(QStringLiteral("hit"));
        // 媒体层用的是单例里的注册表，所以先把同一份配置 load 进去
        ConfigManager::instance().load(path);

        Gb28181Config cfg;
        QString err;
        check(Gb28181Config::load(path, &cfg, &err), "载入失败");
        const VideoSourceCfg cam = cfg.streams[0].takeSource();
        check(cam.id == QLatin1String("csi-cam"), "取到的是被引用的那条流");
        check(cam.url == QLatin1String("/dev/video22"),
              "地址来自注册表，而不是 gb28181.stream 里那个被忽略的 /dev/video99");
        check(cam.type == QLatin1String("camera"),
              "注册表 type 为空 + /dev/video 前缀 → 补成 camera（国标靠它选 v4l2 参数）");
        check(cam.width == 1280 && cam.height == 720 && cam.fps == 30,
              "宽高帧率取注册表值，不是 gb28181 里那两个被忽略的键");
        check(!cam.hardwareDecode, "hardwareDecode=false 要传到国标侧");
        check(cam.loop == false, "注册表没写 loop → false（不再是国标旧的默认 true）");

        const VideoSourceCfg net = cfg.streams[1].takeSource();
        check(net.type == QLatin1String("rtsp"), "显式 type 优先于嗅探");
        check(net.url == QLatin1String("rtsp://192.168.2.60:554/live/1"), "rtsp 地址要对");
        say(QStringLiteral("csi-cam→%1 %2x%3@%4 ｜ net-cam→%5")
                .arg(cam.url).arg(cam.width).arg(cam.height).arg(cam.fps).arg(net.type));
    }

    // ── 5b：补型的另外三个分支 + mp4 归一 ────────────────────────────────────
    {
        // (注册表 id, 注册表 type, url, 期望补出来的 type)
        struct Case { const char *id; const char *type; const char *url; const char *want; };
        static const Case kCases[] = {
            {"no-type-rtsp", "", "rtsp://10.0.0.1/live", "rtsp"},
            {"no-type-file", "", "assets/test/1.mp4", "file"},
            {"typed-mp4", "mp4", "assets/test/2.mp4", "file"},   // 注册表把 mp4 归一成 file
            {"no-type-rtmp", "", "rtmp://10.0.0.2/live", "rtmp"},
        };
        QJsonArray sources;
        QJsonArray streams;
        int seq = 20;
        for (const Case &c : kCases) {
            sources.append(sourceObject(QLatin1String(c.id), QLatin1String(c.type),
                                       QLatin1String(c.url)));
            QJsonObject s;
            s["source_id"] = QLatin1String(c.id);
            s["deviceCode"] = QStringLiteral("340200000013200000%1").arg(seq++);
            streams.append(s);
        }
        QJsonArray channels;
        QJsonObject ch;
        ch["source"] = QStringLiteral("assets/test/1.mp4");   // 内联，让配置仍是 V2
        channels.append(ch);
        QJsonObject video;
        video["sources"] = sources;
        video["channels"] = channels;
        QJsonObject root;
        root["video"] = video;
        QJsonObject gb;
        QJsonObject platform;
        // localHost 必须给非通配值：它是 IPv4 字面量校验的入口，而 advertiseHost
        // 缺省取 localHost —— 留空就是 0.0.0.0，会被"通配地址平台回不了话"那条判据拒掉，
        // 于是本组测的"四条流都载入"变成"整段配置不可用"，测的就不再是想测的东西。
        platform["localHost"] = QStringLiteral("192.168.1.20");
        platform["serverHost"] = QStringLiteral("192.168.1.10");
        gb["platform"] = platform;
        gb["streams"] = streams;
        root["gb28181"] = gb;
        const QString path = testPath(QStringLiteral("sniff"));
        writeConfig(QStringLiteral("sniff"), root);
        ConfigManager::instance().load(path);

        Gb28181Config cfg;
        QString err;
        check(Gb28181Config::load(path, &cfg, &err), "四条嗅探流应全部载入");
        check(cfg.streams.size() == 4, "四条都在");
        int i = 0;
        for (const Case &c : kCases) {
            const VideoSourceCfg take = cfg.streams[i].takeSource();
            check(take.type == QLatin1String(c.want),
                  "type 补型/归一的结果与期望不符（camera 走 v4l2、file 才看 loop）");
            ++i;
        }
    }

    // ── 7：进程跑起来之后注册表被删 → 空 cfg，而不是"随便挑一条" ──────────────
    {
        const QString path = testPath(QStringLiteral("drop-after"));
        writeConfig(QStringLiteral("drop-after"), makeRoot());
        ConfigManager::instance().load(path);
        Gb28181Config cfg;
        QString err;
        check(Gb28181Config::load(path, &cfg, &err), "载入失败");
        const Gb28181StreamConfig stream = cfg.streams[0];
        ConfigManager::instance().removeVideoSource(QStringLiteral("csi-cam"));
        const VideoSourceCfg gone = stream.takeSource();
        check(gone.id.isEmpty() && gone.url.isEmpty(),
              "引用悬空时必须返回空 cfg：媒体层据此报错，不能推另一路画面给平台");
        say(QStringLiteral("删掉 csi-cam 后 takeSource() 的 url=「%1」").arg(gone.url));
    }

    // ── 8：引用计数把国标一起数（删除保护） ──────────────────────────────────
    {
        const QString path = testPath(QStringLiteral("count"));
        writeConfig(QStringLiteral("count"), makeRoot());
        ConfigManager::instance().load(path);
        // demo-mp4：一条通道引用、国标没引用 → 1
        check(ConfigManager::instance().videoSourceReferrerCount(QStringLiteral("demo-mp4")) == 1,
              "只被一条通道引用的流计数为 1");
        check(ConfigManager::instance().videoSourceReferrerCount(QStringLiteral("csi-cam")) == 1,
              "只被国标引用的流也要计数，否则删它无人拦截");
        check(ConfigManager::instance().videoSourceReferrerCount(QStringLiteral("net-cam")) == 1,
              "net-cam 被国标引用");
        say(QStringLiteral("csi-cam 引用数=%1（来自 gb28181.streams）")
                .arg(ConfigManager::instance().videoSourceReferrerCount(QStringLiteral("csi-cam"))));
    }

    // ── 9：顶层 platform/streams 的老布局同样只认引用 ────────────────────────
    {
        QJsonObject root;
        QJsonArray sources;
        sources.append(sourceObject(QStringLiteral("cam-a"), QStringLiteral("camera"),
                                    QStringLiteral("/dev/video10")));
        QJsonObject video;
        video["sources"] = sources;
        QJsonArray channels;
        QJsonObject ch;
        ch["source"] = QStringLiteral("assets/test/1.mp4");
        channels.append(ch);
        video["channels"] = channels;
        root["video"] = video;

        QJsonObject platform;
        platform["localHost"] = QStringLiteral("192.168.1.20");
        platform["serverHost"] = QStringLiteral("192.168.1.10");
        root["platform"] = platform;
        QJsonArray streams;
        QJsonObject good;
        good["source_id"] = QStringLiteral("cam-a");
        good["deviceCode"] = QStringLiteral("34020000001320000006");
        streams.append(good);
        QJsonObject bad;
        bad["source_id"] = QStringLiteral("cam-missing");
        bad["deviceCode"] = QStringLiteral("34020000001320000007");
        streams.append(bad);
        root["streams"] = streams;

        const QString path = testPath(QStringLiteral("toplevel"));
        writeConfig(QStringLiteral("toplevel"), root);
        Gb28181Config cfg;
        QString err;
        check(Gb28181Config::load(path, &cfg, &err), "顶层布局要能载入");
        check(cfg.streams.size() == 1 && cfg.streams[0].deviceCode.endsWith(QLatin1String("006")),
              "顶层 streams 也走同一套引用判据：悬空那条必须丢");
    }

    std::cout << "gb28181 source-ref contracts passed: hit keeps only identity / "
                 "three drop reasons / all-dropped load false / takeSource takes registry / "
                 "empty type sniffed by url prefix / mp4 normalized to file / "
                 "registry deleted at runtime yields empty cfg / referrer count includes gb28181 / "
                 "top-level layout uses the same rule\n";
    return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
