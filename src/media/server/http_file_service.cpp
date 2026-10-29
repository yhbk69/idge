// ============================================================================
// http_file_service.cpp — ZLMediaKit 内嵌 http 静态文件服务的薄壳
// ----------------------------------------------------------------------------
// 本文件是全工程唯一 include `mk_*.h` 的编译单元（约束与理由见 http_file_service.h）。
// ============================================================================

#include "http_file_service.h"

#include <QHostAddress>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QVector>
#include <QDebug>

// 只取用到的那部分 C API：环境初始化/配置项/http 服务器。
// 注意这些键**只能对照库内字符串**——mk_set_option 对未知键静默忽略，
// 上一轮我按 config.ini 里的 `http.directory` 找就白找了半天，真名是 `http.dirMenu`。
#include "mk_common.h"

namespace {

// ZLM 的 IO 线程数。0 会取 hardware_concurrency，在本板就是 8 个线程与 4 路解码/推理
// 抢 CPU；静态文件服务只服务"报警后取一次图和一段 mp4"这种突发小流量，2 个够。
// 实测若仍抢占节拍就降到 1（见 src/media/server/README.md 的性能条目）。
constexpr int kIoThreads = 2;

// 日志级别 0~4 = Trace/Debug/Info/Warn/Error。取 Warn(3)：
// Info 会给每个客户端连接/断开各打一条，多浏览器标签时把 idge 自己的检测日志刷掉。
constexpr int kLogLevelWarn = 3;

// 只往控制台打（run.sh 会把 stdout tee 进 logs/idge_*.log，等于已经落盘了）。
// 不给 ZLM 单独开文件日志：它会按自己的规则切文件，和本工程的日志放一起只会添乱。
constexpr const char *kLogFileNone = nullptr;

// 网卡名黑名单：这些口上的地址对端大概率到不了（容器网桥/回环/虚拟口）。
// 判据不精是有意为之——猜错的代价由配置项 media_base_url 兜住，写在这里就够。
const QVector<QRegularExpression> kSkipIfaceNames = {
    QRegularExpression(QStringLiteral("^(lo|docker|veth|br-|virbr|tap|vmnet)")),
};

bool ifaceNameSkipped(const QString &name)
{
    for (const QRegularExpression &re : kSkipIfaceNames) {
        if (re.match(name).hasMatch()) {
            return true;
        }
    }
    return false;
}

/// 去掉末尾的 '/'（拼接地址时统一按"前缀不带斜杠 + 路径带前导斜杠"处理）
QString trimmedPrefix(const QString &url)
{
    QString out = url;
    while (out.endsWith(QLatin1Char('/'))) {
        out.chop(1);
    }
    return out;
}

}  // namespace

HttpFileService &HttpFileService::instance()
{
    static HttpFileService svc;
    return svc;
}

bool HttpFileService::start(const QString &rootDir, quint16 port)
{
    if (running()) {
        return true;   // 幂等：已经在了就别再碰 mk_env_init1
    }
    if (rootDir.isEmpty() || port == 0) {
        qWarning() << "HttpFileService: 参数不完整，服务不起 root=" << rootDir << "port=" << port;
        return false;
    }
    if (envReady_) {
        // 走到这里 = 上一次 start 把库初始化了但没 bind 成功，而 stop() 之后库不允许再起。
        // 现场表现为"端口一直是空的"，日志说清原因，不要让人去查配置。
        qWarning() << "HttpFileService: ZLM 运行时已初始化过，本进程不允许再起第二次 port=" << port;
        return false;
    }

    // 参数顺序照 mk_common.h:100 的 mk_env_init1；ini/ssl 全给空 = 用库内默认值。
    mk_env_init1(kIoThreads, kLogLevelWarn, LOG_CONSOLE, kLogFileNone, 0,
                 0, nullptr, 0, nullptr, nullptr);
    envReady_ = true;

    const QByteArray rootUtf8 = rootDir.toUtf8();
    mk_set_option("http.rootPath", rootUtf8.constData());
    mk_set_option("http.dirMenu", "0");            // 关目录索引：请求 `/` 拿到 404 而不是全部报警文件名
    mk_set_option("http.keepAliveSecond", "5");    // 别挂着半死的长连接占 IO 线程

    // 回读一遍再打日志：未知键会被静默忽略，"设了没有生效"只有靠这一行才看得出来
    qInfo() << "HttpFileService: rootPath 回读=" << mk_get_option("http.rootPath")
            << "dirMenu 回读=" << mk_get_option("http.dirMenu");

    const uint16_t bound = mk_http_server_start(port, 0);   // ssl=0：明文，本轮不做 wss/https
    if (bound == 0) {
        // 最常见原因是端口被占（现场有人单独跑了 MediaServer，或上一个 idge 没退干净）
        port_ = 0;
        qWarning() << "HttpFileService: 起口失败 port=" << port
                   << "—— 报警媒体地址不申报，上行退化为只发报警文字";
        return false;
    }

    port_ = bound;
    qInfo() << "HttpFileService: 已监听" << bound << "root=" << rootDir;
    return true;
}

void HttpFileService::stop()
{
    if (!envReady_) {
        return;   // 没初始化过就别碰库（出厂态 uplink 关着，一次都不该调）
    }
    mk_stop_all_server();
    port_ = 0;
    qInfo() << "HttpFileService: 已停止（本进程不能再起）";
}

QString HttpFileService::firstUsableIpv4()
{
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
                || (flags & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        if (ifaceNameSkipped(iface.name())) {
            continue;
        }
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol || ip.isLoopback()) {
                continue;
            }
            const QString text = ip.toString();
            // 169.254/16 是没拿到 DHCP 时自己编的地址，对端不可能够得着
            if (text.startsWith(QStringLiteral("169.254."))) {
                continue;
            }
            return text;
        }
    }
    return QString();
}

QString HttpFileService::baseUrl(const QString &overrideUrl) const
{
    const QString override = trimmedPrefix(overrideUrl.trimmed());
    if (!override.isEmpty()) {
        return override;   // 配置说了算，连"服务到底起没起"都不管：地址可能指向别处的反代
    }
    if (!running()) {
        return QString();
    }
    // 自动口径必须用**实际绑定端口**而不是配置里那个：mk_http_server_start 传 0 时
    // 库会随机分配，回落到配置值就拼出一个没人听的地址
    const QString ip = firstUsableIpv4();
    if (ip.isEmpty()) {
        qWarning() << "HttpFileService: 没有可用非回环 IPv4，media_base_url 又留空 —— "
                      "对端拿不到可访问地址，请手填 media_base_url";
        return QString();
    }
    return QStringLiteral("http://%1:%2").arg(ip).arg(port_);
}
