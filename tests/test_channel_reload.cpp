// ============================================================================
// test_channel_reload: 通道配置热更新自测（无 GUI，单解码器）
// ============================================================================
// 覆盖"改模型列表 = 停本路 → releaseTasks → 重开"这条路径的三条契约：
//   1. 停止判据：signalStop() + waitForStop() 之后 isLoopAlive() 必须为 false，
//      否则调用方无法确认"现在可以动 tasks_ 了"。
//   2. 重建正确性：releaseTasks() 后重新 start()，推理任务集合按**新**配置重建
//      （槽位数与模型路径都对得上；旧对象被真正 delete，不是继续沿用）。
//   3. 反复重开不涨：线程数回到基线、RSS 增长有界。热更新若漏掉推理线程或
//      旧 BlockingQueue，四路 × 若干次点击就会把进程撑大。
// 顺带用 updateSlotParams() 的返回值当"槽位数探针"：它只在形状相符时接受改写，
// 正是设置页"报警类别/置信度即时生效 vs 整路重开"的分流判据；改写之后还要求
// 继续出帧，等于确认"在线换运行期参数"这条路既没重连流、也没把解码循环锁死。
// 另加一条阈值契约：config 里的 detect.conf_threshold 与某模型的 conf_threshold
// 覆盖，必须原样出现在 buildCascadeTasks 打的 "slot params" 行里（回落链落地）。
//
// 测试配置里所有模型的 alarm_class 都是空、围栏不启用，在线换那步写的是
// 一个不存在的类别名，所以整轮不产生报警，不会写报警库，也不截图/录片段。
//
// 运行（cwd 必须是 build 目录：模型库按 ./model/library 解析）：
//   ./build/build_rk3588_linux/test_channel_reload [视频源]
// rc 0=PASS 1=FAIL
// ============================================================================
#include "ffmpeg_video_decoder.h"
#include "ConfigManager.h"
#include "model_registry.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

static void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}

// ============================================================================
// 日志捕获：buildCascadeTasks 的槽位重建日志是本测试唯一的"任务集合"观测点
// ============================================================================
// 生产代码没有（也不该有）tasks_ 的公开访问器，所以这里按 qInfo 的格式解析：
//   "cascade slot ch 0 slot 0 npu_core_mask 1 /abs/path/model.rknn"
static QtMessageHandler g_prevHandler = nullptr;
static std::mutex g_logMtx;
static std::vector<std::string> g_logs;

static void captureHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    {
        std::lock_guard<std::mutex> lk(g_logMtx);
        g_logs.push_back(msg.toStdString());
    }
    if (g_prevHandler)
        g_prevHandler(type, ctx, msg);
}

static void clearLogs()
{
    std::lock_guard<std::mutex> lk(g_logMtx);
    g_logs.clear();
}

/// 某通道本轮重建出来的槽位数与模型路径（末尾字段）
struct SlotLog
{
    int count = 0;
    std::vector<std::string> paths;
};

static SlotLog collectCascadeSlots(int channel)
{
    const std::string prefix = "cascade slot ch " + std::to_string(channel) + " slot ";
    SlotLog out;
    std::lock_guard<std::mutex> lk(g_logMtx);
    for (const std::string &line : g_logs) {
        if (line.compare(0, prefix.size(), prefix) != 0)
            continue;
        ++out.count;
        const auto sp = line.find_last_of(' ');
        if (sp != std::string::npos) {
            std::string path = line.substr(sp + 1);
            // qInfo() << QString 会打印成对双引号，不剥掉就和注册表里的路径对不上
            if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
                path = path.substr(1, path.size() - 2);
            out.paths.push_back(path);
        }
    }
    return out;
}

// ============================================================================
// 每槽生效阈值：回落链落地的唯一可观测点
// ============================================================================
// buildCascadeTasks 里那句 "slot params ch 0 slot 0 conf 0.600 nms 0.400" 是
// config → ModelRegistry 回落链 → 解码器槽位表 这三段的端到端证据。
// 前缀与 "cascade slot ch …" 刻意不同：那句的**最后一个 token 是模型路径**，
// 上面 collectCascadeSlots 就靠它，混用会把那条契约解析弄坏。
struct SlotThresh
{
    std::vector<double> conf;
    std::vector<double> nms;
};

static SlotThresh collectSlotParams(int channel)
{
    const std::string prefix = "slot params ch " + std::to_string(channel) + " slot ";
    SlotThresh out;
    std::lock_guard<std::mutex> lk(g_logMtx);
    for (const std::string &line : g_logs) {
        if (line.compare(0, prefix.size(), prefix) != 0)
            continue;
        const QStringList toks = QString::fromStdString(line).split(QLatin1Char(' '),
                                                                    QString::SkipEmptyParts);
        double conf = -1.0;
        double nms = -1.0;
        for (int i = 0; i + 1 < toks.size(); ++i) {
            if (toks[i] == QLatin1String("conf"))
                conf = toks[i + 1].toDouble();
            else if (toks[i] == QLatin1String("nms"))
                nms = toks[i + 1].toDouble();
        }
        // 认不出数字就当这行不存在：宁可让下面的"行数与槽位数不符"先炸，
        // 也不要拿一个 0.0 去和期望值比出个莫名其妙的差值
        if (conf < 0.0 || nms < 0.0)
            continue;
        out.conf.push_back(conf);
        out.nms.push_back(nms);
    }
    return out;
}

// ============================================================================
// 事件循环辅助
// ============================================================================
static void spinMs(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

/// 等解码线程产出 want 帧（帧到达走的是队列连接，必须转事件循环）
static void waitFrames(std::atomic<int> &frames, int want, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (frames.load(std::memory_order_acquire) >= want || t.elapsed() >= timeoutMs)
            loop.quit();
    });
    poll.start(20);
    loop.exec();
    if (frames.load(std::memory_order_acquire) < want)
        throw std::runtime_error("解码线程没有在规定时间内产出足够帧数");
    frames.store(0);
}

// ============================================================================
// 进程资源读数：线程数与 RSS（热更新的泄漏判据都靠这两个）
// ============================================================================
static int threadCount()
{
    return QDir("/proc/self/task").entryList(QDir::Dirs, QDir::NoSort).size();
}

static long rssKb()
{
    QFile f(QStringLiteral("/proc/self/status"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return 0;
    for (const QByteArray &line : f.readAll().split('\n')) {
        if (line.startsWith("VmRSS:"))
            return line.mid(6).trimmed().split(' ').first().toLong();
    }
    return 0;
}

// ============================================================================
// writeChannelConfig: 生成一份最小可用 config（只有通道 0 在用，其余停用）
// ============================================================================
// 所有模型的 alarm_class 留空 = 只画框不报警；geofence.enabled=false。
// 这样整轮测试不碰报警库，也不触发截图与片段录制。
//
// 阈值故意配成"两档都在"：全局 0.30 + 只给 yolo11n-ppe 挂一条 0.60 的每模型覆盖。
// 于是回落链的两个分支同轮可验——ppe 的槽位必须打 0.600，其它槽位必须打 0.300。
// ============================================================================
static const char *kConfOverrideModel = "yolo11n-ppe";
static const double kGlobalConf = 0.30;
static const double kGlobalNms = 0.40;
static const double kOverrideConf = 0.60;

static void writeChannelConfig(const QString &path, const QStringList &models,
                               const QString &source)
{
    QJsonArray idArr;
    QJsonObject paramObj;
    for (const QString &id : models) {
        idArr.append(id);
        QJsonObject one;
        one[QStringLiteral("alarm_class")] = QString();
        one[QStringLiteral("note")] = QStringLiteral("test_channel_reload");
        if (id == QLatin1String(kConfOverrideModel))
            one[QStringLiteral("conf_threshold")] = kOverrideConf;
        paramObj[id] = one;
    }

    QJsonArray channels;
    for (int ch = 0; ch < 4; ++ch) {
        QJsonObject o;
        o[QStringLiteral("channel")] = ch;
        o[QStringLiteral("enabled")] = ch == 0;
        o[QStringLiteral("note")] = QStringLiteral("test");
        o[QStringLiteral("source")] = ch == 0 ? source : QString();
        o[QStringLiteral("models")] = ch == 0 ? idArr : QJsonArray();
        channels.append(o);
    }

    QJsonObject video;
    video[QStringLiteral("channels")] = channels;
    video[QStringLiteral("models")] = paramObj;

    QJsonObject geofence;
    geofence[QStringLiteral("enabled")] = false;

    // 全局档 = 回落链的底座；没有每模型覆盖的槽位必须落在这一档上
    QJsonObject detect;
    detect[QStringLiteral("conf_threshold")] = kGlobalConf;
    detect[QStringLiteral("nms_threshold")] = kGlobalNms;

    QJsonObject root;
    root[QStringLiteral("video")] = video;
    root[QStringLiteral("geofence")] = geofence;
    root[QStringLiteral("detect")] = detect;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        throw std::runtime_error("临时配置写入失败");
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
}

static bool hasPath(const SlotLog &rebuilt, const QString &path)
{
    // 日志里是 config/注册表给的原始路径，测试侧 ./model 可能是软链：
    // 两边都取 canonical 再比，否则 ./model 与 ../../model 会判成不同文件。
    const QString want = QFileInfo(path).canonicalFilePath();
    for (const std::string &p : rebuilt.paths) {
        if (QFileInfo(QString::fromStdString(p)).canonicalFilePath() == want)
            return true;
    }
    return false;
}

// ============================================================================
// preflight: 环境不对就第一时间失败
// ============================================================================
// 模型库与视频源都按当前工作目录解析：不在 build 目录里跑就该直接报"环境不符"，
// 而不是后面拿一堆"槽位数不符"的假信号。
// ============================================================================
static void preflight(const QString &source)
{
    check(QFile::exists(source), "找不到测试视频 assets/test/1.mp4（或参数给的路径不存在）");
    check(QDir(QStringLiteral("model/library/yolo11n-ppe")).exists(),
          "模型库不可用：请在 build 目录下运行（该目录需有 ./model 软链）");
    ModelRegistry::instance().rescan();   // modelPath() 只认扫描过的库
    check(!ModelRegistry::instance().modelPath(QStringLiteral("yolo11n-ppe")).isEmpty(),
          "yolo11n-ppe 未入库，测试无法解析槽位");
}

int main(int argc, char *argv[])
{
    // 默认日志规则可能过滤掉 qInfo，而槽位重建只写在 qInfo 里
    qputenv("QT_LOGGING_RULES", "*.debug=true;*.info=true");
    QCoreApplication app(argc, argv);
    g_prevHandler = qInstallMessageHandler(captureHandler);
    qRegisterMetaType<RenderFrame>("RenderFrame");

    QString source = argc > 1 ? QString::fromLocal8Bit(argv[1])
                              : (QFile::exists(QStringLiteral("assets/test/1.mp4"))
                                     ? QStringLiteral("assets/test/1.mp4")
                                     : QStringLiteral("../../assets/test/1.mp4"));
    source = QFileInfo(source).absoluteFilePath();

    const QString cfgPath = QDir::temp().filePath(QStringLiteral("idge_channel_reload_config.json"));

    // 每轮的期望槽位集合：加一个、减一个、再加回来（同数量换模型也顺带覆盖）
    const QList<QStringList> rounds = {
        {QStringLiteral("yolo11n-ppe")},
        {QStringLiteral("yolo11n-ppe"), QStringLiteral("yolo11n-coco")},
        {QStringLiteral("yolo11n-coco")},
        {QStringLiteral("yolo11n-ppe"), QStringLiteral("yolo11n-coco")},
    };

    FFmpegVideoDecoder dec;
    dec.setChannel(0);
    std::atomic<int> frames{0};
    std::vector<std::string> decoderErrors;
    QObject::connect(&dec, &FFmpegVideoDecoder::frameReady, &app,
                     [&](RenderFrame frame) {
                         frames.fetch_add(1, std::memory_order_release);
                         if (frame.fd >= 0)
                             ::close(frame.fd);   // 本测试就是"接收方"，负责释放副本
                     });
    QObject::connect(&dec, &FFmpegVideoDecoder::error, &app, [&](const QString &msg) {
        std::lock_guard<std::mutex> lk(g_logMtx);
        decoderErrors.push_back(msg.toStdString());
    });

    const int baseThreads = threadCount();
    const long baseRssKb = rssKb();
    std::cout << "基线: 线程 " << baseThreads << " 个, RSS " << baseRssKb / 1024 << " MB\n";

    try {
        preflight(source);
        for (const QStringList &models : rounds) {
            writeChannelConfig(cfgPath, models, source);
            ConfigManager::instance().load(cfgPath);

            // 两阶段停止：本轮第一次进入时上一轮还在跑，最后一轮进入时是干净的
            dec.signalStop();
            dec.waitForStop();
            check(!dec.isLoopAlive(), "waitForStop 返回后解码循环仍判定存活");

            clearLogs();
            dec.releaseTasks();
            dec.start(source);
            waitFrames(frames, 30, 20000);

            // 契约 2：任务集合按新配置重建
            const SlotLog rebuilt = collectCascadeSlots(0);
            if (rebuilt.count != models.size())
                throw std::runtime_error("重建出的槽位数与配置里的模型数不符: "
                                         + std::to_string(rebuilt.count) + " != "
                                         + std::to_string(models.size()));
            for (const QString &id : models) {
                const QString path = ModelRegistry::instance().modelPath(id);
                if (hasPath(rebuilt, path))
                    continue;
                std::string got;
                for (const std::string &p : rebuilt.paths)
                    got += p + " | ";
                throw std::runtime_error("槽位里没有 " + id.toStdString() + " 的模型路径，本轮实际: "
                                         + (got.empty() ? "(没有槽位日志)" : got));
            }

            // 契约 2b：阈值回落链真的把值送进了槽位表（每模型覆盖 > 全局 > 代码默认）
            const SlotThresh thresh = collectSlotParams(0);
            if (thresh.conf.size() != static_cast<size_t>(rebuilt.count))
                throw std::runtime_error("slot params 行数与槽位数不符: "
                                         + std::to_string(thresh.conf.size()) + " != "
                                         + std::to_string(rebuilt.count));
            for (int i = 0; i < rebuilt.count; ++i) {
                const double wantConf = models[i].compare(QLatin1String(kConfOverrideModel)) == 0
                    ? kOverrideConf : kGlobalConf;
                const QString where = QStringLiteral("槽位%1(%2)").arg(i).arg(models[i]);
                check(qAbs(thresh.conf[i] - wantConf) < 1e-3,
                      ("生效置信度与回落链算出的不一致 " + where.toStdString()
                       + ": 期望 " + std::to_string(wantConf)
                       + " 实际 " + std::to_string(thresh.conf[i])).c_str());
                check(qAbs(thresh.nms[i] - kGlobalNms) < 1e-3,
                      ("生效 NMS 应等于全局档 " + where.toStdString()).c_str());
            }

            // 形状相符 → 运行期参数可在线换；形状不符 → 必须拒绝（设置页据此改走重开）
            // 写一个"不可能命中"的非空类别，而不是照抄配置里的空值：
            //   1) 这一步真的改到了状态（空→非空），不是在原地写同一份；
            //   2) 仍然不会产生报警——本测试全程不写报警库（见文件头）；
            //   3) 换完必须继续出帧：解码线程每帧要在 slotParamMtx_ 内拷一份，
            //      锁写歪（临界区盖住 NPU 调用）的表现就是这一路永久丢帧，
            //      而那正是"改完设置画面卡住"这类现场问题的形态。
            // 阈值一并拧到 0.95：这一句同时验证"TaskData 带得动阈值"这条载体的形状，
            // 至于框是否真的少了，属于板上肉眼判据（见 plan §六-2），不在无头测试里断言。
            std::vector<SlotRuntimeParam> params(models.size());
            for (SlotRuntimeParam &p : params) {
                p.alarmClass = QStringLiteral("__no_such_class__");
                p.confThreshold = 0.95f;
                p.nmsThreshold = 0.45f;
            }
            check(dec.updateSlotParams(params), "槽位数相符时在线换运行期参数应被接受");
            std::vector<SlotRuntimeParam> wrongParams(models.size() + 1);
            check(!dec.updateSlotParams(wrongParams), "槽位数不符时 updateSlotParams 必须拒绝");
            waitFrames(frames, 10, 5000);

            std::cout << "轮[" << models.join(QStringLiteral(",")).toStdString()
                      << "] 槽位 " << rebuilt.count << " 个 ✓  线程 " << threadCount()
                      << " RSS " << rssKb() / 1024 << " MB\n";
        }

        // 契约 3：收尾后再看一次，线程必须回落（QThread 靠 finished→deleteLater 回收，
        // 得转一圈事件循环才真正析构）
        dec.signalStop();
        dec.waitForStop();
        check(!dec.isLoopAlive(), "收尾停止后解码循环仍判定存活");
        dec.releaseTasks();
        spinMs(500);
        const int endThreads = threadCount();
        const long endRssKb = rssKb();
        std::cout << "收尾: 线程 " << endThreads << " 个, RSS " << endRssKb / 1024 << " MB\n";
        check(endThreads <= baseThreads + 1, "反复重开后线程数没有回落（推理线程泄漏）");
        check(endRssKb - baseRssKb < 256 * 1024, "RSS 增长超出预期（旧任务/队列没回收）");

        if (!decoderErrors.empty())
            throw std::runtime_error("解码器上报了错误，热更新路径不完整: " + decoderErrors.front());
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }

    std::cout << "channel reload stop/release/restart contracts passed\n";
    return 0;
}
