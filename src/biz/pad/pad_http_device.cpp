// ============================================================================
// pad_http_device.cpp — v1 §7 设备状态与健康（GET /api/device/status、GET /api/health）
// ============================================================================
//
// 这两个接口是整张 HTTP 面上**唯一不写任何东西**的一对，所以它们也是联调时最先该打通的：
// Pad 只要拿到 200 就知道口、路径、信封三件事全对了。
//
// §7.1 与 §13.7.1 在这里有一处必须先裁决的自相矛盾，实现按 §7.1 那句走：
//   §13.7.1 把 battery/temperature_c/… 列成"必填/可选"混在一起，
//   §7.1 正文写的是**"只返回实际字段；Pad 对缺失字段显示 —"**。
//   → 所以本文件的每一条都有"来源读不到就整个键不写"的分支，绝不填 0 顶数。
//     填 0 会被对端读成"这台设备负载真的是 0"，那是对外报假数
//     （同一判断的来龙去脉见 src/base/sysinfo/device_stats.h 文件头第 1 条 NPU 那段实测）。
//
// 线程：handler 在 GUI 线程上同步跑。DeviceSampler 是有状态的（CPU 要两次 /proc/stat 做差），
// 所以它做成**函数内静态**——只在这根线程上被碰，不需要锁；
// 第一次装配时 seed() 建基线，于是第一个请求就有 cpu_usage（否则恒缺，看起来像"这台没 CPU"）。
// ============================================================================
#include "pad_http_api.h"

#include "ConfigManager.h"
#include "device_stats.h"
#include "model_registry.h"
#include "pad_http_server.h"
#include "pad_wire.h"
#include "pad_ws_server.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>

namespace pad {
namespace {

/// 本进程里这台 HTTP 服务的起始时钟（静态局部：随首次装配启动，重新装配**不**重置，
/// 因为 §13.7.2 的 uptimeSec 语义是"服务运行秒数"，调试页点一次「保存并应用」
/// 不该让对端看到它归零——真重启进程才会归零，那时本来也就是新的运行时长）。
QElapsedTimer &serviceClock()
{
    static QElapsedTimer clock;
    static bool started = false;
    if (!started) {
        clock.start();
        started = true;
    }
    return clock;
}

/// §7.1 的 model_version：把各通道**在用**的模型去重后拼出来。
/// 来源是 ModelRegistry 的 meta（id + 入库版本标记），不是任何硬编码字符串——
/// 换模型库之后这个数自己会跟着变，这才是"AI 模型版本"该有的样子。
/// 一个都没解析出来时返回空串，调用方据此省略该字段。
QString activeModelVersion()
{
    const ConfigManager &cfg = ConfigManager::instance();
    QStringList parts;
    for (int ch = 0; ch < cfg.channelCount(); ++ch) {
        const QStringList ids = cfg.channelModelIds(ch);
        for (const QString &id : ids) {
            const ModelMeta meta = ModelRegistry::instance().meta(id);
            if (meta.id.isEmpty())
                continue;   // 库里查不到（配置的 id 拼错/模型已移除）→ 不进这个字符串
            QString one = meta.id;
            if (!meta.version.isEmpty())
                one += QLatin1Char('@') + meta.version;
            if (!parts.contains(one))
                parts << one;
        }
    }
    return parts.join(QLatin1Char(','));
}

}  // namespace

void registerDeviceRoutes(PadHttpServer *server, const HttpApiContext &ctx)
{
    if (!server)
        return;

    static sysinfo::DeviceSampler sampler;
    static bool seeded = false;
    if (!seeded) {
        sampler.seed();
        seeded = true;
    }
    // 装配这一刻就把时钟起好。为什么不能等第一个请求再起（那是本函数最早版的写法）：
    // §13.7.2 的 uptimeSec 是"服务运行秒数"，懒起会得到服务已经听了几分钟、
    // 第一次请求却回 uptimeSec=0 —— 实测 curl 一次就打出来了这一行。
    serviceClock();
    // ── §7.1 GET /api/device/status ────────────────────────────────────
    // 字段名是 **snake_case**（§2.6 明写这一条与其余各节不同，别"顺手"改成 camelCase）。
    server->addRoute(QStringLiteral("GET"), QStringLiteral("/api/device/status"),
                     [ctx](const padhttp::Request &) -> padhttp::Reply {
                         // 每次请求现采一次：Pad 是"打开设备页时拉一次 + 手动刷新"，
                         // 不做后台轮询（§7.1），所以这里没有频率问题，也不需要缓存。
                         const sysinfo::DeviceStats stats = sampler.sample();
                         QJsonObject data;
                         // 两条必填：device_id 与 online。
                         // online 恒 true 不是敷衍——这条链路的"在线"定义就是"这个口能答话"，
                         // 答不了话时 Pad 走的是请求失败分支，不会看到一个 false。
                         if (!ctx.deviceId.isEmpty())
                             data.insert(QStringLiteral("device_id"), ctx.deviceId);
                         data.insert(QStringLiteral("online"), true);

                         if (stats.hasBattery)
                             data.insert(QStringLiteral("battery"), stats.batteryPercent);
                         if (stats.hasTemperature) {
                             data.insert(QStringLiteral("temperature_c"),
                                         qRound(stats.temperatureC * 10.0) / 10.0);
                         }
                         if (stats.hasCpu) {
                             data.insert(QStringLiteral("cpu_usage"),
                                         qRound(stats.cpuUsagePercent * 10.0) / 10.0);
                         }
                         // npu_usage：板端**恒不给**（debugfs 对 uid 1000 权限不够，
                         // 而 /sys/class/devfreq 那个节点空闲时也报 100）——
                         // 见 src/base/sysinfo/device_stats.h 文件头第 1 条的实测记录。
                         if (stats.hasNpu) {
                             data.insert(QStringLiteral("npu_usage"),
                                         qRound(stats.npuUsagePercent * 10.0) / 10.0);
                         }
                         if (stats.hasMemory) {
                             data.insert(QStringLiteral("mem_total_mb"), stats.memTotalMb);
                             data.insert(QStringLiteral("mem_used_mb"), stats.memUsedMb);
                         }
                         if (stats.hasDisk) {
                             data.insert(QStringLiteral("disk_total_gb"),
                                         qRound(stats.diskTotalGb * 10.0) / 10.0);
                             data.insert(QStringLiteral("disk_used_gb"),
                                         qRound(stats.diskUsedGb * 10.0) / 10.0);
                         }
                         if (!stats.systemVersion.isEmpty()) {
                             data.insert(QStringLiteral("sys_version"), stats.systemVersion);
                         }
                         // firmware_version **省略**：本机没有任何可信的固件版本来源
                         // （没读到过对应节点，也不肯把板型字符串当成固件版本报出去）。
                         if (stats.hasUptime)
                             data.insert(QStringLiteral("uptime_s"), stats.uptimeSec);
                         const QString models = activeModelVersion();
                         if (!models.isEmpty())
                             data.insert(QStringLiteral("model_version"), models);

                         // cameras[]：一行一个**通道**。name 用界面上那句通道备注，
                         // §7.1 明写"不使用安装位置"，所以这里只取 note/回落"通道N"。
                         // 三个身份字段全部走 channelIdentity()，与 §6 的 streams[] 同一份口径。
                         QJsonArray cameras;
                         const ConfigManager &cfg = ConfigManager::instance();
                         for (int ch = 0; ch < cfg.channelCount(); ++ch) {
                             const ChannelIdentity identity =
                                 channelIdentity(ch, ctx.exposeStreamCredentials);
                             QJsonObject one;
                             one.insert(QStringLiteral("camera_id"), identity.cameraId);
                             one.insert(QStringLiteral("name"), identity.name);
                             one.insert(QStringLiteral("online"),
                                         identity.enabled && channelUp(ctx, ch));
                             // 本机一条通道就一个取流地址，没有主子码流并列的结构
                             one.insert(QStringLiteral("stream_count"), 1);
                             cameras.append(one);
                         }
                         data.insert(QStringLiteral("cameras"), cameras);
                         data.insert(QStringLiteral("last_seen_at"), pad::wireNow());
                         return padhttp::replyOk(data);
                     });

    // ── §7.2 GET /api/health ───────────────────────────────────────────
    server->addRoute(QStringLiteral("GET"), QStringLiteral("/api/health"),
                     [&ctx](const padhttp::Request &) -> padhttp::Reply {
                         Q_UNUSED(ctx);
                         QJsonObject data;
                         data.insert(QStringLiteral("ok"), true);
                         data.insert(QStringLiteral("serverTime"), pad::wireNow());
                         // 当前连着几台 Pad：WS 那台的连接数，只用于"设备与链路"页显示。
                         data.insert(QStringLiteral("wsClients"),
                                     PadWsServer::instance().clientCount());
                         data.insert(QStringLiteral("uptimeSec"),
                                     serviceClock().elapsed() / 1000);

                         // capabilities 是 §13.7.2 那张表，**照实报**：
                         // 没实现的能力报 true 会让 Pad 把按钮点下去拿 404，
                         // 比报 false（Pad 直接不显示那个入口）坏得多。
                         // 四个分组必须嵌在 data.capabilities 里（§7.2 的示例与 §13.7.2 都是
                         // 这个形状）；摊平到 data 顶层，对端按 capabilities.fence.polygon
                         // 取值全落空 → 围栏/视频/清点入口整片消失（本函数第一版就是这么错的）。
                         QJsonObject fence;
                         fence.insert(QStringLiteral("polygon"), true);
                         fence.insert(QStringLiteral("versions"), true);
                         fence.insert(QStringLiteral("regions"), true);
                         fence.insert(QStringLiteral("maxRegions"), 8);   // §5.2 的硬上限

                         QJsonObject stream;
                         stream.insert(QStringLiteral("list"), true);
                         stream.insert(QStringLiteral("status"), true);
                         // 本机没有按时序录的回放产物（片段是"报警前后那一小段"，不是流的回放），
                         // 所以 playback=false，§6.1 也整条不给 playbackUrl。
                         stream.insert(QStringLiteral("playback"), false);
                         // §6.1 明写"无 stream_start/stop"，这条规约要求恒为 false。
                         stream.insert(QStringLiteral("startStop"), false);

                         // 清点/销点走的是 §11，本轮**没有装那套 HTTP 路由**（A1 未定，见
                         // plan/口径确认单_2026-11-03.md），所以整组报 false。
                         // ⚠ 这不表示本机没有点名与盘点功能——那两个在界面上是能用的
                         //   （src/ui/form/roll_call_widget.cpp、equipment_inventory_widget.cpp），
                         //   只是还没有 HTTP 契约里的这四个入口。
                         QJsonObject count;
                         count.insert(QStringLiteral("person"), false);
                         count.insert(QStringLiteral("tools"), false);
                         count.insert(QStringLiteral("faces"), false);
                         count.insert(QStringLiteral("toolCloseups"), false);

                         QJsonObject capabilities;
                         capabilities.insert(QStringLiteral("fence"), fence);
                         capabilities.insert(QStringLiteral("stream"), stream);
                         capabilities.insert(QStringLiteral("count"), count);
                         capabilities.insert(QStringLiteral("checkout"), false);
                         data.insert(QStringLiteral("capabilities"), capabilities);
                         return padhttp::replyOk(data);
                     });
}

}  // namespace pad
