// ============================================================================
// test_stream_params: video.sources 的四个"打开参数"板端实测与回归（无 GUI）
// ============================================================================
// 驱动**真** FFmpegVideoDecoder（InferenceMode::PreviewOnly：一个 RKNN 模型都不加载、
// 不建 DMA 缓冲池、不画框），只验"一条流的参数到底落没落到 FFmpeg 上"。
//
// 为什么值得单独钉：这四个参数是本轮新接的（type/loop/hardwareDecode/timeoutMs/reconnectMs
// 全部来自 video.sources[]），而它们的失效方向**全都是静默的**：
//   · loop 不生效 → 演示片播完黑屏，但没有任何错误；
//   · timeoutMs 不生效 → 打不开的地址把这一路挂在 avformat_open_input 里，界面停在"正在接入…"；
//   · reconnectMs 不生效 → 摄像头拔插后这一路永久死掉，只能重启程序；
//   · hardwareDecode=false 不生效 → 软解档位形同虚设，CPU 占用与帧率对不上账没人发现。
// 这四条在板端肉眼各看一眼都判不出来，所以做成可重复的数。
//
// 覆盖六组判据：
//   1) loop=false：一遍放完就结束（出帧数≈素材总帧数，finished 会发）
//   2) loop=true ：EOF 后回卷继续出帧（帧数明显超过一遍，日志出现"loop 回卷"）
//   3) timeoutMs ：打不开的地址必须在 ≈timeoutMs 内失败返回，而不是无限等
//   4) reconnectMs：失败的开流会按间隔重试；signalStop 能在一个分片内脱身（不睡满整个间隔）
//   5) hardwareDecode=false：软解档确实出帧（走 CPU 帧那条已有分支），并打出与硬解档的
//      出帧速率与 RSS 对比——这两个数是**给人看的实测记录**，不作断言
//   6) 参数缺省（内联旧口径）：type 空 + 其余默认，同一个文件照样能出帧（兜底没坏）
//
// 素材：需要一段**很短**的本地视频（默认 3 秒档）。没有就跳过 1/2/5 组并打印生成命令，
// 其余照跑（1/2/5 需要它是因为回卷必须读完一整遍才发生，拿 20 秒的片子会让本测试跑很久）：
//   ffmpeg -y -v error -ss 0 -t 3 -i assets/test/1.mp4 -c copy /tmp/idge_probe_3s.mp4
//
// ⚠ 必须在构建目录下跑（assets/ 相对路径与 librockchip_mpp 的 rpath 都按那儿算）：
//   ./build/build_rk3588_linux/test_stream_params [3秒素材路径] [黑洞地址]
//   黑洞地址默认 rtsp://192.0.2.1:554/live（TEST-NET-1，不可路由 → 连接被静默丢弃，
//   正好用来验 timeoutMs；换成局域网内一个没人应答的 IP 也行）。
// rc 0=PASS 1=FAIL
// ============================================================================
#include "ffmpeg_video_decoder.h"
#include "ConfigManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <unistd.h>   // close()：RenderFrame.fd 是 dup 副本，接收方负责关掉
}

namespace {

void check(bool ok, const char *message)
{
    if (!ok) {
        throw std::runtime_error(message);
    }
}

// ── 日志捕获：回卷/重连/失败这些"参数生效的证据"只存在于日志里 ──────────────
// 与 test_channel_reload 同一手法（生产代码不该为测试开访问器）。
QtMessageHandler g_prevHandler = nullptr;
std::mutex g_logMtx;
std::vector<std::string> g_logs;

void captureHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    {
        std::lock_guard<std::mutex> lk(g_logMtx);
        g_logs.push_back(msg.toStdString());
    }
    if (g_prevHandler) {
        g_prevHandler(type, ctx, msg);
    }
}

int countLinesContaining(const QString &needle)
{
    std::lock_guard<std::mutex> lk(g_logMtx);
    int n = 0;
    for (const std::string &line : g_logs) {
        if (QString::fromUtf8(line.c_str()).contains(needle, Qt::CaseInsensitive)) {
            ++n;
        }
    }
    return n;
}

void clearLogs()
{
    std::lock_guard<std::mutex> lk(g_logMtx);
    g_logs.clear();
}

// ── 资源读数：RSS 用来给硬解/软解两档做对比 ────────────────────────────────
long rssKb()
{
    QFile f(QStringLiteral("/proc/self/status"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return 0;
    }
    for (const QByteArray &line : f.readAll().split('\n')) {
        if (line.startsWith("VmRSS:")) {
            return line.mid(6).trimmed().split(' ').first().toLong();
        }
    }
    return 0;
}

struct RunResult {
    int frames = 0;
    qint64 elapsedMs = 0;
    bool ended = false;      ///< 收到 finished()（= 这一路的外环退出了）
    long rssKb = 0;
    QString describe() const
    {
        return QStringLiteral("帧 %1 ｜ 耗时 %2ms ｜ ended=%3 ｜ RSS %4MB")
            .arg(frames).arg(elapsedMs).arg(ended ? 1 : 0).arg(rssKb / 1024);
    }
};

/**
 * @brief 按给定流参数开一路、观察一个时间窗，然后停干净
 * @param windowMs 观察窗上限；期间收到 finished() 就提前结束
 *
 * 计数与 ended 都走**队列连接**回主线程（发射方是解码线程），所以必须转事件循环；
 * 用 20ms 的 poll 而不是死等，是为了让"窗还没到就结束了"这种情况也能立刻收敛。
 */
RunResult observe(const VideoSourceCfg &cfg, int windowMs, const char *what)
{
    FFmpegVideoDecoder dec(nullptr, FFmpegVideoDecoder::InferenceMode::PreviewOnly);
    std::atomic<int> frames{0};
    std::atomic<bool> ended{false};
    dec.setChannel(0);

    QObject::connect(&dec, &FFmpegVideoDecoder::frameReady, &dec, [&](RenderFrame frame) {
        frames.fetch_add(1, std::memory_order_release);
        if (frame.fd >= 0) {
            ::close(frame.fd);   // 本函数就是"接收方"，负责释放这份 dup 副本
        }
    });
    QObject::connect(&dec, &FFmpegVideoDecoder::finished, &dec,
                     [&]() { ended.store(true, std::memory_order_release); });

    clearLogs();
    const qint64 rssBefore = rssKb();
    QElapsedTimer clock;
    clock.start();
    dec.start(cfg);

    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (ended.load(std::memory_order_acquire) || clock.elapsed() >= windowMs) {
            loop.quit();
        }
    });
    poll.start(20);
    loop.exec();

    RunResult r;
    r.frames = frames.load(std::memory_order_acquire);
    r.elapsedMs = clock.elapsed();
    r.ended = ended.load(std::memory_order_acquire);
    r.rssKb = rssKb();
    std::cout << "   [" << what << "] " << r.describe().toUtf8().constData()
              << " ｜ RSS 增量 " << double(r.rssKb - rssBefore) / 1024.0 << "MB\n";

    // 收干净：观察窗到点就停，绝不把线程留给下一个用例
    dec.signalStop();
    dec.waitForStop();
    return r;
}

VideoSourceCfg fileCfg(const QString &url, bool loop, bool hardwareDecode)
{
    VideoSourceCfg cfg;
    cfg.id = QStringLiteral("probe");
    cfg.type = QStringLiteral("file");
    cfg.url = url;
    cfg.loop = loop;
    cfg.hardwareDecode = hardwareDecode;
    return cfg;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    g_prevHandler = qInstallMessageHandler(captureHandler);

    const QString clipPath = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                      : QStringLiteral("/tmp/idge_probe_3s.mp4");
    const QString blackHole = argc > 2 ? QString::fromLocal8Bit(argv[2])
                                       : QStringLiteral("rtsp://192.0.2.1:554/live");
    const bool haveClip = QFileInfo(clipPath).exists();

    try {
        if (!haveClip) {
            std::cout << "SKIP 1/2/5/6 组：找不到 3 秒素材 " << clipPath.toUtf8().constData()
                      << "\n   生成命令：ffmpeg -y -v error -ss 0 -t 3 -i assets/test/1.mp4 "
                         "-c copy /tmp/idge_probe_3s.mp4\n";
        } else {
            std::cout << "素材：" << clipPath.toUtf8().constData() << "\n";
        }

        // ── 1：loop=false 放完就结束 ────────────────────────────────────────
        if (haveClip) {
            const RunResult r = observe(fileCfg(clipPath, false, true), 12000, "loop=false");
            check(r.ended, "loop=false 播完必须发 finished()（外环退出，不重连）");
            check(r.frames > 20, "loop=false 至少要出帧——一帧都没有说明 type=file 这条路没打开");
            std::cout << "   单遍实测：" << r.frames << " 帧 / " << r.elapsedMs << "ms\n";
        }

        // ── 2：loop=true 回卷继续出帧 ───────────────────────────────────────
        if (haveClip) {
            // 观察窗给到单遍时长的两倍多一点：回卷至少要"读完一遍 + 再跑一段"才看得出差别。
            const RunResult r = observe(fileCfg(clipPath, true, true), 9000, "loop=true");
            check(!r.ended, "loop=true 不该在窗口内结束（结束了就是没回卷，演示片播完会黑屏）");
            check(countLinesContaining(QStringLiteral("loop 回卷")) >= 1,
                  "日志里要出现 loop 回卷那行——这是回卷真的发生了的直接证据");
            check(r.frames > 20, "回卷后仍要持续出帧");
        }

        // ── 3：timeoutMs 把打开阶段夹住 ─────────────────────────────────────
        {
            VideoSourceCfg cfg;
            cfg.id = QStringLiteral("probe-timeout");
            cfg.type = QStringLiteral("rtsp");
            cfg.url = blackHole;
            cfg.timeoutMs = 3000;
            cfg.reconnectMs = 0;      // 只测一次打开，别把重连混进来
            const RunResult r = observe(cfg, 30000, "timeoutMs=3000");
            check(r.ended, "打不开的地址必须最终退出外环（reconnectMs=0 → 失败就停）");
            // 3000ms 是"打开+探测"的总预算；再留 12 秒给 FFmpeg 内部各阶段的中断粒度。
            // 真不生效的话这里会是几十秒到几分钟（TCP 默认超时），差两个量级，不会被误判。
            check(r.elapsedMs < 15000,
                  "timeoutMs 没起作用：打开阶段没有被墙上时钟夹住，界面会一直停在\"正在接入…\"");
            std::cout << "   黑洞地址 " << blackHole.toUtf8().constData()
                      << " 实测失败返回耗时 " << r.elapsedMs << "ms（预算 3000ms）\n";
        }

        // ── 4：reconnectMs 会重试，且停止不被整个间隔睡住 ────────────────────
        {
            VideoSourceCfg cfg;
            cfg.id = QStringLiteral("probe-reconnect");
            cfg.type = QStringLiteral("file");
            cfg.url = QStringLiteral("/tmp/idge_probe_definitely_missing.mp4");
            cfg.reconnectMs = 1500;
            cfg.timeoutMs = 0;        // 文件打开失败是即时的，不需要墙上时钟
            FFmpegVideoDecoder dec(nullptr, FFmpegVideoDecoder::InferenceMode::PreviewOnly);
            std::atomic<bool> ended{false};
            dec.setChannel(0);
            QObject::connect(&dec, &FFmpegVideoDecoder::finished, &dec,
                             [&]() { ended.store(true); });
            clearLogs();
            dec.start(cfg);

            QElapsedTimer clock;
            clock.start();
            QEventLoop loop;
            QTimer poll;
            QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
                if (ended.load() || clock.elapsed() >= 4200) {
                    loop.quit();
                }
            });
            poll.start(20);
            loop.exec();
            const int attempts = countLinesContaining(QStringLiteral("按 reconnectMs="));
            check(!ended.load(), "配了 reconnectMs 就不该在打不开后彻底退出（这就是 reconnectMs=0 的旧行为）");
            check(attempts >= 2,
                  "重连没生效：4 秒窗口里没看到第二次打开尝试");
            std::cout << "   4.2s 窗口内打开尝试 " << attempts << " 次（间隔 1500ms）\n";

            // 关键一条：等待间隔是 100ms 分片，所以停止必须**很快**返回，
            // 而不是睡满整个 reconnectMs（否则现场"点停止"要等 1.5 秒才响应）。
            QElapsedTimer stopClock;
            stopClock.start();
            dec.signalStop();
            dec.waitForStop();
            const qint64 stopMs = stopClock.elapsed();
            std::cout << "   signalStop→waitForStop 实测 " << stopMs << "ms（重连间隔 1500ms）\n";
            check(stopMs < 700,
                  "停止被整个重连间隔睡住了：分片睡没起作用，界面会点完按钮卡 1.5 秒");
        }

        // ── 5：hardwareDecode=false 走软解 ──────────────────────────────────
        if (haveClip) {
            const RunResult soft = observe(fileCfg(clipPath, false, false), 12000, "软解");
            check(soft.frames > 20,
                  "hardwareDecode=false 没出帧：软解分支（CPU 帧 + sws_scale）没走到");
            const RunResult hard = observe(fileCfg(clipPath, false, true), 12000, "硬解");
            check(hard.frames > 20, "硬解档必须照常出帧（这是回归保护，不是新能力）");
            // 这两个数是给人核对的实测记录（板子上每次跑都会漂，不设断言）：
            std::cout << "   软解 " << soft.frames << " 帧 / " << soft.elapsedMs << "ms ｜ "
                      << "硬解 " << hard.frames << " 帧 / " << hard.elapsedMs << "ms\n";
        }

        // ── 6：参数全默认 = 内联旧口径 ──────────────────────────────────────
        if (haveClip) {
            VideoSourceCfg legacy;
            legacy.url = clipPath;   // type/loop/尺寸/帧率/超时/重连全不写
            const RunResult r = observe(legacy, 12000, "内联兜底");
            check(r.frames > 20 && r.ended,
                  "只给 url 的旧口径必须照常开流并放完（兜底不能被注册表改坏）");
        }
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }

    qInstallMessageHandler(g_prevHandler);
    std::cout << "stream params contracts passed: loop off/on(回卷日志) / timeoutMs 夹住打开阶段 / "
                 "reconnectMs 重试 + 分片停止 / 软解出帧 / 只给 url 的旧口径\n";
    return 0;
}
