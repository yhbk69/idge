// ============================================================================
// test_video_sources: 视频流注册表与通道引用解析离线自检（无 GUI、无解码器）
// ============================================================================
// 只编 ConfigManager.cpp：这一段全是"读 JSON / 改 JSON"，不碰 FFmpeg、不碰模型、
// 不碰屏幕，所以本目标能在任何机器上跑，也是唯一能把"解析口径"逐条钉死的地方。
//
// 为什么值得单独钉：通道开什么流现在有**两种写法**（引用 video.sources[] 的 id、
// 或通道里内联 source），而界面、解码、将来的 /api 都只认一句
// resolveChannelSource()。这句写错的表现是"点了没反应"或"黑一块"，板端肉眼
// 看不出原因；把三层兜底和两条反直觉的口径钉成断言，改代码时才敢动。
//
// 契约清单（与 src/base/config/ConfigManager.h 的注释逐条对应）：
//   1. 出厂默认：新建配置就有一条 file-demo 流，四路通道都引用它 → 开箱即有画面
//   2. 三层解析：source_id 命中 → 整条条目（含 loop/超时/重连/硬解等 10 项参数）
//   3. 内联兜底：无 source_id + 内联非空 → url 用内联，**type 留空**（交给解码器按
//      地址前缀嗅探），其余字段全默认 —— 与加注册表之前的行为逐字一致
//   4. 悬空 id **不回退**内联：引用了不存在的流就是不开这一路，绝不"偷偷用旧地址"
//   5. 写内联即撤引用（setChannelSource 清 source_id）：否则手改地址不生效 = 假开关
//   6. 写引用保留内联（setChannelSourceId 不动 source）：切回内联时老地址还在
//   7. setVideoSource 拒空 id/空 url；同 id 重复条目一次写入即收口成一条
//   8. removeVideoSource 连带清掉引用它的通道的 source_id（不造悬空）
//   9. videoSourceReferrerCount 只数正在引用它的通道
//  10. type "mp4" 读入归一成 "file"：同一个意思不留两种写法
//  11. 存盘往返：setter → save() → 重新 load() 的值必须一模一样
//  12. 迁移只补空段：老配置（有 video 无 sources）补 "sources": [] 且**不改**任何
//      通道的内联 source；重复 load 不追加、不覆盖
//
// 运行（cwd 任意：测试配置写在 QDir::tempPath() 下，绝不碰 data/config.json）：
//   ./build/build_rk3588_linux/test_video_sources
// rc 0=PASS 1=FAIL
// ============================================================================
#include "ConfigManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <stdexcept>
#include <string>

static void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}

static void say(const QString &line)
{
    qInfo().noquote() << "   " << line;
}

// 测试配置一律写在这里：与运行时 data/config.json 完全隔离，
// 断言里任何一次 save() 都不可能碰到真配置。
static QString testPath(const QString &name)
{
    return QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("idge_video_sources_%1.json").arg(name));
}

// 建一份最小可用的 V2 配置（有 video.channels 就算 V2，load() 不会再跑旧 schema 迁移）
static void writeConfig(const QString &name, const QJsonObject &root)
{
    QFile f(testPath(name));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        throw std::runtime_error("测试配置写入失败");
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
}

static QJsonObject channelObject(const QString &sourceId, const QString &inlineSource)
{
    QJsonObject c;
    if (!sourceId.isEmpty())
        c["source_id"] = sourceId;
    if (!inlineSource.isEmpty())
        c["source"] = inlineSource;
    return c;
}

static QJsonObject rootWithChannels(const QJsonArray &sources, const QJsonArray &channels)
{
    QJsonObject video;
    video["sources"] = sources;
    video["channels"] = channels;
    QJsonObject root;
    root["video"] = video;
    return root;
}

static VideoSourceCfg sampleStream()
{
    // 十个字段全给非默认值：往返断言才有意义（默认值 0/false 分不清"没读"和"读成 0"）
    VideoSourceCfg s;
    s.id = QStringLiteral("cam-front");
    s.type = QStringLiteral("camera");
    s.url = QStringLiteral("/dev/video41");
    s.note = QStringLiteral("东门");
    s.width = 1280;
    s.height = 720;
    s.fps = 25;
    s.inputFormat = QStringLiteral("nv12");
    s.loop = false;
    s.hardwareDecode = false;
    s.timeoutMs = 4000;
    s.reconnectMs = 1500;
    return s;
}

// ---------------------------------------------------------------------------
// 1. 出厂默认：新文件里就有一条流，四路都引用它
// ---------------------------------------------------------------------------
static void testDefaults()
{
    QFile::remove(testPath(QStringLiteral("defaults")));
    ConfigManager::instance().load(testPath(QStringLiteral("defaults")));
    ConfigManager &cfg = ConfigManager::instance();

    const QList<VideoSourceCfg> streams = cfg.videoSources();
    check(!streams.isEmpty(), "默认配置必须有至少一条视频流");
    bool demo = false;
    for (const VideoSourceCfg &s : streams) {
        if (s.id == QLatin1String("file-demo"))
            demo = true;
    }
    check(demo, "默认配置缺少 file-demo 这条演示流");
    const VideoSourceCfg demoCfg = cfg.videoSource(QStringLiteral("file-demo"));
    check(demoCfg.type == QLatin1String("file"), "file-demo 的类型应为 file");
    check(demoCfg.loop, "file-demo 默认循环播放（演示用，不能播完就黑）");
    check(!demoCfg.url.isEmpty(), "file-demo 必须有地址");

    for (int ch = 0; ch < cfg.channelCount(); ++ch) {
        check(cfg.channelSourceId(ch) == QLatin1String("file-demo"),
              "默认配置里每一路都应引用 file-demo");
        check(cfg.resolveChannelSource(ch).url == demoCfg.url,
              "默认配置下各通道解析出的地址应与那条流一致");
    }
    say(QStringLiteral("默认种子：%1 条流，%2 路全部引用 file-demo")
            .arg(streams.size()).arg(cfg.channelCount()));
}

// ---------------------------------------------------------------------------
// 2~4. 三层解析 + 悬空不回退
// ---------------------------------------------------------------------------
static void testResolutionTiers()
{
    ConfigManager &cfg = ConfigManager::instance();

    QJsonArray sources;
    sources.append(QJsonObject{
        {"id", "cam-front"}, {"type", "camera"}, {"url", "/dev/video41"},
        {"note", "东门"}, {"width", 1280}, {"height", 720}, {"fps", 25},
        {"inputFormat", "nv12"}, {"hardwareDecode", false},
        {"timeoutMs", 4000}, {"reconnectMs", 1500}});
    // 四个通道分别是：①引用命中 ②只有内联 ③两者都空 ④引用悬空（且内联非空）
    QJsonArray channels;
    channels.append(channelObject(QStringLiteral("cam-front"), QString()));
    channels.append(channelObject(QString(), QStringLiteral("/dev/video9")));
    channels.append(channelObject(QString(), QString()));
    channels.append(channelObject(QStringLiteral("no-such-stream"), QStringLiteral("/dev/video9")));
    writeConfig(QStringLiteral("tiers"), rootWithChannels(sources, channels));
    cfg.load(testPath(QStringLiteral("tiers")));

    // ① 引用命中：十个参数整条带过来，不是只拿 url
    const VideoSourceCfg hit = cfg.resolveChannelSource(0);
    check(hit.id == QLatin1String("cam-front"), "引用命中时应返回注册表里的那条流");
    check(hit.type == QLatin1String("camera"), "解析结果的 type 应来自注册表");
    check(hit.url == QLatin1String("/dev/video41"), "解析结果的 url 应来自注册表");
    check(hit.width == 1280 && hit.height == 720 && hit.fps == 25,
          "解析结果必须带上注册表里的尺寸与帧率");
    check(hit.inputFormat == QLatin1String("nv12"), "解析结果必须带上输入格式");
    check(!hit.hardwareDecode, "hardwareDecode=false 必须原样解析出来（true 是默认值，分不清没读）");
    check(hit.timeoutMs == 4000 && hit.reconnectMs == 1500, "解析结果必须带超时与重连间隔");

    // ② 内联兜底：地址用内联，type 故意留空 = 让解码器嗅探；参数全默认
    const VideoSourceCfg inlineOnly = cfg.resolveChannelSource(1);
    check(inlineOnly.url == QLatin1String("/dev/video9"), "内联 source 应作为兜底地址");
    check(inlineOnly.id.isEmpty(), "内联兜底不是注册表条目，id 必须为空");
    check(inlineOnly.type.isEmpty(), "内联兜底的 type 必须留空（走地址前缀嗅探＝旧行为）");
    check(inlineOnly.width == 0 && inlineOnly.height == 0 && inlineOnly.fps == 0,
          "内联兜底的尺寸/帧率必须是不指定");
    check(inlineOnly.hardwareDecode, "内联兜底默认硬解（改动前没有软解开关）");
    check(!inlineOnly.loop && inlineOnly.timeoutMs == 0 && inlineOnly.reconnectMs == 0,
          "内联兜底的循环/超时/重连必须全默认＝改动前行为");

    // ③ 都空：不开这一路
    const VideoSourceCfg empty = cfg.resolveChannelSource(2);
    check(empty.url.isEmpty() && empty.id.isEmpty(), "未配置的通道应解析成空流");

    // ④ 悬空引用：**不**回退到内联 —— 这条是本测试存在的最大理由
    const VideoSourceCfg dangling = cfg.resolveChannelSource(3);
    check(dangling.url.isEmpty(), "source_id 悬空时不得回退到内联 source（宁可不开，也不偷偷换源）");

    // channelSource() 是旧调用方唯一认识的那句，必须等于解析后的地址
    check(cfg.channelSource(0) == QLatin1String("/dev/video41"),
          "channelSource() 必须返回解析后的地址，而不是内联键的原值");
    check(cfg.channelSource(1) == QLatin1String("/dev/video9"),
          "channelSource() 在内联兜底时也要返回地址（旧调用方不受注册表影响）");
    check(cfg.channelSource(3).isEmpty(), "channelSource() 在悬空引用时必须是空串");
    say(QStringLiteral("三层解析 + 悬空不回退：通过"));
}

// ---------------------------------------------------------------------------
// 5~6. 两种写法的互斥与保留
// ---------------------------------------------------------------------------
static void testSetterSemantics()
{
    ConfigManager &cfg = ConfigManager::instance();
    QJsonArray sources;
    sources.append(QJsonObject{{"id", "a"}, {"type", "file"}, {"url", "/tmp/a.mp4"}});
    sources.append(QJsonObject{{"id", "b"}, {"type", "file"}, {"url", "/tmp/b.mp4"}});
    QJsonArray channels;
    channels.append(channelObject(QStringLiteral("a"), QStringLiteral("/tmp/legacy.mp4")));
    channels.append(channelObject(QString(), QString()));
    channels.append(channelObject(QString(), QString()));
    channels.append(channelObject(QString(), QString()));
    writeConfig(QStringLiteral("setters"), rootWithChannels(sources, channels));
    cfg.load(testPath(QStringLiteral("setters")));

    // 引用与内联同时存在时引用优先（这条不变量由解析层保证，见 ①/④）
    check(cfg.resolveChannelSource(0).url == QLatin1String("/tmp/a.mp4"),
          "source_id 与内联同时存在时应以引用为准");

    // 5. 写内联 = 撤引用：否则"手改地址→保存→重启"画面还是旧流，是个假开关
    cfg.setChannelSource(0, QStringLiteral("/tmp/handwritten.mp4"));
    check(cfg.channelSourceId(0).isEmpty(), "写内联地址必须同时清掉 source_id");
    check(cfg.resolveChannelSource(0).url == QLatin1String("/tmp/handwritten.mp4"),
          "写内联地址后解析结果必须就是这个新地址");

    // 6. 写引用 = 保留内联：切回"不用注册表"时老地址还在，不用凭记忆重填
    cfg.setChannelSourceId(0, QStringLiteral("b"));
    check(cfg.channelSourceId(0) == QLatin1String("b"), "引用应写进 source_id");
    check(cfg.channelInlineSource(0) == QLatin1String("/tmp/handwritten.mp4"),
          "写引用不得抹掉内联地址");
    check(cfg.resolveChannelSource(0).url == QLatin1String("/tmp/b.mp4"),
          "写引用后解析结果必须立刻跟着引用走");

    // 清空引用后回到内联（界面"（内联地址）"那一项就是这个效果）
    cfg.setChannelSourceId(0, QString());
    check(cfg.resolveChannelSource(0).url == QLatin1String("/tmp/handwritten.mp4"),
          "清掉 source_id 应落回内联地址");
    say(QStringLiteral("内联/引用互斥与保留：通过"));
}

// ---------------------------------------------------------------------------
// 7~10. 注册表本身的写入规则
// ---------------------------------------------------------------------------
static void testRegistryWrites()
{
    ConfigManager &cfg = ConfigManager::instance();
    writeConfig(QStringLiteral("writes"),
                rootWithChannels(QJsonArray(), QJsonArray{channelObject(QString(), QString()),
                                                          channelObject(QString(), QString()),
                                                          channelObject(QString(), QString()),
                                                          channelObject(QString(), QString())}));
    cfg.load(testPath(QStringLiteral("writes")));

    // 7. 拒空：主键或地址没给就一个字节都不改
    VideoSourceCfg noId = sampleStream();
    noId.id.clear();
    check(!cfg.setVideoSource(noId), "id 为空必须拒绝写入");
    VideoSourceCfg noUrl = sampleStream();
    noUrl.url.clear();
    check(!cfg.setVideoSource(noUrl), "url 为空必须拒绝写入");
    check(cfg.videoSources().isEmpty(), "被拒绝的写入不得留下半条条目");

    check(cfg.setVideoSource(sampleStream()), "正常条目应写入成功");
    check(cfg.videoSources().size() == 1, "写入后应恰好有一条流");

    // 7b. 同 id 重复收口：手改 JSON 很容易留下两条同 id，命中哪条取决于数组顺序
    writeConfig(QStringLiteral("dup"),
                rootWithChannels(QJsonArray{
                    QJsonObject{{"id", "x"}, {"type", "file"}, {"url", "/tmp/first.mp4"}},
                    QJsonObject{{"id", "x"}, {"type", "file"}, {"url", "/tmp/second.mp4"}}},
                    QJsonArray{channelObject(QStringLiteral("x"), QString()),
                               channelObject(QString(), QString()),
                               channelObject(QString(), QString()),
                               channelObject(QString(), QString())}));
    cfg.load(testPath(QStringLiteral("dup")));
    check(cfg.videoSources().size() == 2, "重复 id 的配置在改动前应原样保留（迁移不擅自删条目）");
    VideoSourceCfg fix;
    fix.id = QStringLiteral("x");
    fix.type = QStringLiteral("file");
    fix.url = QStringLiteral("/tmp/fixed.mp4");
    check(cfg.setVideoSource(fix), "重复 id 的条目应能被覆盖");
    const QList<VideoSourceCfg> after = cfg.videoSources();
    int stillX = 0;
    for (const VideoSourceCfg &s : after) {
        if (s.id == QLatin1String("x"))
            ++stillX;
    }
    check(stillX == 1, "一次 setVideoSource 后同 id 必须只剩一条");
    check(cfg.resolveChannelSource(0).url == QLatin1String("/tmp/fixed.mp4"),
          "重复 id 收口后通道解析到的应是收口结果");

    // 9. 引用计数：删除确认要用它，数错就是"删了正在用的流却没人警告"
    writeConfig(QStringLiteral("refs"),
                rootWithChannels(QJsonArray{QJsonObject{{"id", "shared"}, {"type", "file"},
                                                        {"url", "/tmp/shared.mp4"}},
                                            QJsonObject{{"id", "idle"}, {"type", "file"},
                                                        {"url", "/tmp/idle.mp4"}}},
                                    QJsonArray{channelObject(QStringLiteral("shared"), QString()),
                                               channelObject(QStringLiteral("shared"), QString()),
                                               channelObject(QStringLiteral("idle"), QString()),
                                               channelObject(QString(), QString())}));
    cfg.load(testPath(QStringLiteral("refs")));
    check(cfg.videoSourceReferrerCount(QStringLiteral("shared")) == 2, "shared 应被两路引用");
    check(cfg.videoSourceReferrerCount(QStringLiteral("idle")) == 1, "idle 应被一路引用");
    check(cfg.videoSourceReferrerCount(QStringLiteral("nobody")) == 0, "没人引用的条计数应为 0");

    // 8. 删条目连带清引用：只删条目就会造出悬空 id，而那一路从此解析不出地址
    cfg.removeVideoSource(QStringLiteral("shared"));
    check(cfg.videoSource(QStringLiteral("shared")).url.isEmpty(), "删除后这条流不该还在");
    check(cfg.channelSourceId(0).isEmpty() && cfg.channelSourceId(1).isEmpty(),
          "删除必须清掉引用它的通道的 source_id（不造悬空）");
    check(cfg.channelSourceId(2) == QLatin1String("idle"), "删除不得动到别的通道的引用");
    check(cfg.videoSourceReferrerCount(QStringLiteral("shared")) == 0, "删除后计数应归零");

    // 10. "mp4" 归一成 "file"：同一个意思不留两种写法
    writeConfig(QStringLiteral("mp4"),
                rootWithChannels(QJsonArray{QJsonObject{{"id", "m"}, {"type", "mp4"},
                                                        {"url", "/tmp/m.mp4"}}},
                                    QJsonArray{channelObject(QStringLiteral("m"), QString()),
                                               channelObject(QString(), QString()),
                                               channelObject(QString(), QString()),
                                               channelObject(QString(), QString())}));
    cfg.load(testPath(QStringLiteral("mp4")));
    check(cfg.videoSource(QStringLiteral("m")).type == QLatin1String("file"),
          "type=mp4 应读成 file（解码器只认 file/camera/rtsp/rtmp）");
    say(QStringLiteral("注册表写入规则：通过"));
}

// ---------------------------------------------------------------------------
// 11~12. 落盘往返 + 迁移只补空段
// ---------------------------------------------------------------------------
static void testPersistAndMigration()
{
    ConfigManager &cfg = ConfigManager::instance();
    writeConfig(QStringLiteral("round"),
                rootWithChannels(QJsonArray(), QJsonArray{channelObject(QString(), QString()),
                                                         channelObject(QString(), QString()),
                                                         channelObject(QString(), QString()),
                                                         channelObject(QString(), QString())}));
    cfg.load(testPath(QStringLiteral("round")));
    check(cfg.setVideoSource(sampleStream()), "往返测试的前置写入应成功");
    cfg.save();
    cfg.load(testPath(QStringLiteral("round")));   // 重新从盘上读回来
    const VideoSourceCfg back = cfg.videoSource(QStringLiteral("cam-front"));
    const VideoSourceCfg want = sampleStream();
    check(back.id == want.id && back.type == want.type && back.url == want.url
              && back.note == want.note,
          "字符串字段存盘往返必须一致");
    check(back.width == want.width && back.height == want.height && back.fps == want.fps
              && back.timeoutMs == want.timeoutMs && back.reconnectMs == want.reconnectMs,
          "整数字段存盘往返必须一致（0 与非 0 分得清才算）");
    check(back.inputFormat == want.inputFormat && back.hardwareDecode == want.hardwareDecode
              && back.loop == want.loop,
          "输入格式与两个开关存盘往返必须一致");

    // 12. 老配置（有 video.channels、没有 video.sources）：只补空数组，内联一个字都不动
    QJsonObject legacyVideo;
    QJsonArray legacyChannels;
    legacyChannels.append(channelObject(QString(), QStringLiteral("/dev/video0")));
    legacyChannels.append(channelObject(QString(), QStringLiteral("rtsp://10.0.0.1/1")));
    legacyChannels.append(channelObject(QString(), QString()));
    legacyChannels.append(channelObject(QString(), QString()));
    legacyVideo["channels"] = legacyChannels;
    QJsonObject legacyRoot;
    legacyRoot["video"] = legacyVideo;
    writeConfig(QStringLiteral("legacy"), legacyRoot);

    cfg.load(testPath(QStringLiteral("legacy")));
    check(cfg.videoSources().isEmpty(), "迁移补出来的注册表必须是空的（不猜内容）");
    check(cfg.channelSourceId(0).isEmpty(), "迁移不得给老通道凭空写 source_id");
    check(cfg.resolveChannelSource(0).url == QLatin1String("/dev/video0"),
          "迁移后内联地址仍要照常解析出来");
    check(cfg.resolveChannelSource(1).url == QLatin1String("rtsp://10.0.0.1/1"),
          "迁移后 rtsp 内联地址仍要照常解析出来");
    {
        QFile f(testPath(QStringLiteral("legacy")));
        if (!f.open(QIODevice::ReadOnly))
            throw std::runtime_error("迁移后的配置读不回来");
        const QJsonObject saved = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        if (!saved["video"].toObject().contains(QStringLiteral("sources")))
            throw std::runtime_error("迁移必须把 video.sources 这段补进文件（让运维看得见）");
    }
    // 幂等：再 load 两次不能追加、不能覆盖现场改过的内容
    writeConfig(QStringLiteral("legacy2"),
                rootWithChannels(QJsonArray{QJsonObject{{"id", "cam-front"}, {"type", "camera"},
                                                        {"url", "/dev/video99"}}},
                                    legacyChannels));
    cfg.load(testPath(QStringLiteral("legacy2")));
    cfg.load(testPath(QStringLiteral("legacy2")));
    check(cfg.videoSources().size() == 1, "已有 video.sources 时迁移不得覆盖或追加");
    check(cfg.videoSource(QStringLiteral("cam-front")).url == QLatin1String("/dev/video99"),
          "现场改过的流不能被启动流程改回默认");
    say(QStringLiteral("存盘往返 + 只补空段的迁移：通过"));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    // 三条契约各自都会打一条 qWarning（悬空 id 的告警是设计的一部分），
    // 这里只关心断言结果，日志降噪到 Warning 以上由默认 handler 处理。
    struct Run {
        const char *name;
        void (*fn)();
    };
    const Run runs[] = {
        {"出厂默认种子", testDefaults},
        {"三层解析与悬空不回退", testResolutionTiers},
        {"内联与引用互斥", testSetterSemantics},
        {"注册表写入规则", testRegistryWrites},
        {"存盘往返与迁移幂等", testPersistAndMigration},
    };
    for (const Run &r : runs) {
        qInfo().noquote() << QString("[group] %1").arg(r.name);
        try {
            r.fn();
        } catch (const std::exception &e) {
            qCritical().noquote() << QString("[FAIL] %1 :: %2").arg(r.name, e.what());
            return 1;
        }
    }
    qInfo().noquote() << "[PASS] test_video_sources";
    return 0;
}
