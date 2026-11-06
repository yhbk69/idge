// ============================================================================
// http_file_service.cpp — ZLMediaKit 内嵌 http 静态文件服务的薄壳
// ----------------------------------------------------------------------------
// 本文件是全工程唯一 include `mk_*.h` 的编译单元（约束与理由见 http_file_service.h）。
// ============================================================================

#include "http_file_service.h"

#include <QHostAddress>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSet>
#include <QVector>
#include <QDebug>

#include "net_route.h"   // 默认路由判据：多网卡时"对外是哪张口"由内核的路由表说了算
#include "zlm_env.h"   // mk_env_init1 的进程级一次性门（与 RtspLiveService 共享）

// 只取用到的那部分 C API：配置项/http 服务器。
// 注意这些键**只能对照库内字符串**——mk_set_option 对未知键静默忽略，
// 上一轮我按 config.ini 里的 `http.directory` 找就白找了半天，真名是 `http.dirMenu`。
#include "mk_common.h"

namespace {

// IO 线程数/日志级别由 zlm_env.cpp 统一决定（HttpFileService 与 RtspLiveService 共享），
// 这里不再重复定义。

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

/**
 * @brief 这张口上"能给对端用"的第一个 IPv4，不合格返回空串
 * @note 判据四道：IsUp && IsRunning && !IsLoopBack、网卡名不撞 kSkipIfaceNames、
 *       是 IPv4 且非回环、不在 169.254/16（没拿到 DHCP 时自己编的地址，对端够不着）。
 * @note 与 ui 域的 QtHelper::getLocalIPs 同一判据，但不 include ui 头（分层方向）。
 */
QString usableIpv4On(const QNetworkInterface &iface)
{
    const QNetworkInterface::InterfaceFlags flags = iface.flags();
    if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
            || (flags & QNetworkInterface::IsLoopBack)) {
        return QString();
    }
    if (ifaceNameSkipped(iface.name())) {
        return QString();
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
    return QString();
}

/**
 * @brief 候选口的顺序：内核默认路由经过的口排最前，其余按 allInterfaces() 原顺序补在后面
 * @note 为什么要排（2026-11-05 实测）：板载 4G 模块的 rndis 口 `usb0` ifindex=3、
 *       `wlan0` ifindex=4，枚举顺序恒定把 usb0 排在前面，于是媒体地址长期发成
 *       模块内置 DHCP 给的 192.168.200.23——那是个点对点口，局域网里的 Pad 路由不到。
 *       内核路由表里默认路由是 wlan0 metric=600、usb0 metric=20100，所以拿它当前排依据。
 * @note 路由表读不到（非 Linux/proc 没挂）就是原枚举顺序，行为退化而不是报错。
 * @note 只排顺序、不改判据：能不能用仍然由 usableIpv4On() 说了算，所以默认路由那张口
 *       如果没地址（比如 WiFi 断了），会正常落到下一张口。
 */
QVector<QNetworkInterface> ifacesByRoutePriority()
{
    // Qt 5.12 的 allInterfaces() 回 QList，6.x 才改成 QVector —— 升 Qt 时连测试一起改
    const QList<QNetworkInterface> all = QNetworkInterface::allInterfaces();
    QVector<QNetworkInterface> ordered;
    QSet<QString> taken;
    for (const QString &name : netroute::defaultRouteIfacesFromProc()) {
        if (taken.contains(name)) {
            continue;
        }
        for (const QNetworkInterface &iface : all) {
            if (iface.name() == name) {
                ordered.push_back(iface);
                taken.insert(name);
                break;
            }
        }
    }
    for (const QNetworkInterface &iface : all) {
        if (!taken.contains(iface.name())) {
            ordered.push_back(iface);
        }
    }
    return ordered;
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

/**
 * @brief 进程级单例（函数内 static，C++11 起懒汉初始化本身线程安全）
 * @note 刻意**不是** QObject：没有线程亲和、没有信号槽，内部只有 C API 调用与
 *       QNetworkInterface 查询，所以哪个线程调都行；真正的约束是下面那条"起停时序"。
 */
HttpFileService &HttpFileService::instance()
{
    static HttpFileService svc;
    return svc;
}

/**
 * @brief 初始化 ZLM 运行时并把静态文件服务起在 port 上
 * @param rootDir **必须是绝对路径**。传相对（RuntimePaths::alarmsDir() 就是相对的）
 *        会被 ZLM 退回它自己的默认根（可执行文件目录 /www），
 *        症状是"URL 全 404，但本地文件明明在"——正口径见调用侧
 *        `QFileInfo(RuntimePaths::alarmsDir()).absoluteFilePath()`。
 * @param port 期望端口；0 视为参数不完整直接失败
 * @return true = 已在监听（**含"上一次已经起来了"这种幂等重入**）；
 *         false = 根目录空 / port 为 0 / 库已初始化过但没起成功 / bind 失败。
 *         失败都已 qWarning，调用方**不需要**再打一条。
 * @note ⚠ 三条不可绕过的库约束：
 *       1) `mk_env_init1` 进程级只许一次。envReady_ 一旦置真，本函数后面再也返回 false，
 *          所以"改端口必须重启"不是保守说法而是硬结果；
 *       2) `stop()` 之后本函数也不会再成功（同样撞 envReady_ 这条早退）；
 *       3) `mk_env_init1` 的返回值**没有被检查**——envReady_=true 表示"调过了"，
 *          不表示"库起来了"。真失败会一路走到 mk_http_server_start 拿到 0，
 *          表现与端口被占完全一样，只能靠前面那行 qInfo 回读判断键有没有生效。
 * @note 线程要求：现网 start 在 frmMain 构造（GUI 线程）、stop 在 main 事件循环之后，
 *       两者时序串行且没有并发；类内**没有锁**，别把它改成运行期可重开的服务。
 */
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

    // mk_env_init1 走 zlm::ensureEnvInit 这道进程级门：RtspLiveService 可能已经先调过了，
    // 这里只需要"保证已就绪"，不能自己再调一次（库硬约束：第二次是 UB）。
    zlm::ensureEnvInit();
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

/**
 * @brief 关掉 ZLM 的全部服务器（mk_stop_all_server），并把 port_ 归零
 * @note 幂等：envReady_ 为假（出厂态 alarm_uplink 关着，一次都没 start 过）时直接返回，
 *       **一次都不碰 mk_***。这是刻意的——库没初始化就调它的清理入口是未定义行为。
 * @note ⚠ 调完本进程**不支持再 start**（见 start() 的约束第 2 条），所以这一对
 *       跨作用域不成对：start 在 frmMain 构造、stop 在 main 的事件循环之后。
 *       别"顺手"把 stop 挪进 ~HttpFileService 或 ~frmMain —— frmMain 是 main 的栈对象，
 *       析构点在事件循环之后、WS 清理之前，挪进去等于把停服务提到还没关 WS 的时刻。
 * @note 无返回值，失败不可感知；本函数只在退出路径上跑一次。
 */
void HttpFileService::stop()
{
    if (!envReady_) {
        return;   // 没初始化过就别碰库（出厂态 uplink 关着，一次都不该调）
    }
    mk_stop_all_server();
    port_ = 0;
    qInfo() << "HttpFileService: 已停止（本进程不能再起）";
}

/**
 * @brief 挑一个能给对端用的本机 IPv4
 * @return 地址字符串；一个都没有返回**空串**（调用方据此不申报媒体地址）
 * @note 顺序 = 内核默认路由经过的口优先，其余按枚举顺序（见 ifacesByRoutePriority），
 *       每张口再各自过 usableIpv4On() 那四道判据。
 * @note ⚠ 仍然是 **best-effort，不是"对端一定够得着"**：它对的是"本机自己上网走哪张口"，
 *       不是"对端从哪张网过来"。两条默认路由都合法、或对端在第三条网上（例如它走
 *       USB 共享上网那条点对点口连进来）就会挑错，所以配置里留了 media_base_url 这个
 *       逃生口——现场一旦报"图片下载不到"，先手填前缀而不是来改这里的判据。
 */
QString HttpFileService::firstUsableIpv4()
{
    for (const QNetworkInterface &iface : ifacesByRoutePriority()) {
        const QString ip = usableIpv4On(iface);
        if (!ip.isEmpty()) {
            return ip;
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
