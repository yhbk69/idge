#ifndef HTTP_FILE_SERVICE_H
#define HTTP_FILE_SERVICE_H

// ============================================================================
// http_file_service.h — 内嵌 HTTP 静态文件服务（报警媒体"出地址"的那台服务器）
// ----------------------------------------------------------------------------
// 用途：报警上行改成「JSON 只发地址、字节由对端自己下载」之后，本机必须有一个
// 能被对端访问到的 http 服务，把 data/alarms/yyyyMMdd/ 下的抓拍图与 .mp4 片段
// 按路径暴露出去。服务体是 vendored 的 ZLMediaKit（3rdparty/zlmediakit），
// **同进程内嵌**调用它的 C API，不 fork MediaServer。
//
// 为什么这个目录是全工程唯一允许 include `mk_*.h` 的地方：
// `mk_*` 是全局 C 符号，散出去就没人分得清"哪些文件能调 ZLM"，
// 而它的生命周期约束（见下）比一般库苛刻得多，必须集中管理。
// 公开接口一律 Qt 类型，`mk_` 的指纹不外泄（3rdparty/zlmediakit/README.md 同一条规矩）。
//
// 三条必须知道的库约束（都是实测/库内字符串核对过的，不是抄文档）：
//   1) **进程级一次，起停不成对**：mk_env_init1 初始化整个库的运行时，
//      `stop()` 之后**不支持再 start**。所以 start 在 frmMain（配置已 load、出厂态判得准），
//      stop 在 main 的事件循环之后 —— 这一对故意跨作用域，别"顺手"挪进 ~frmMain。
//      推论：端口和开关改了必须重启进程，不提供运行期重开。
//   2) **配置键只能对照库内字符串**：mk_set_option 对未知键**静默忽略**，
//      表现是"看着设了其实没生效"。本文件只设 http.rootPath / http.dirMenu /
//      http.keepAliveSecond 三个，其余一律不动。
//   3) **特权口 bind 不上**：板端 uid 1000 + net.ipv4.ip_unprivileged_port_start=1024，
//      80/443 直接失败，默认端口取 8090。
//
// 实测语义（curl 逐条核对过，实施时若换 ZLM 版本要重跑）：
//   .jpg → image/jpeg、.mp4 → video/mp4、.h265 → text/plain（**这就是片段发 mp4 的原因**）；
//   Range: bytes=0-1023 → 206 + Content-Range（网页可以拖进度条）；
//   http.dirMenu=0 → 请求 `/` 与 `/日期/` 一律 404（目录索引确实关得掉）；
//   `%2e%2e%2f` 与裸 `../` 穿越 → 404；/index/api、/live、/hls 前缀均 404，不与静态文件抢路径。
//
// 安全口径：明文 http + 零鉴权，一旦 rootPath 定在 data/alarms，
// **能连到本机的任何人都能按路径取走所有报警媒体**。唯一的可见性防线是
// "对端不知道文件名" + dirMenu 关掉列表 + 404 兜底，这不是访问控制。
// 因此本服务只在 alarm_uplink.enabled 且 media_transport="url" 时才起，出厂态根本不 bind。
// ============================================================================

#include <QString>
#include <QtGlobal>

/// 内嵌 http 文件服务（进程级单例，用法见文件头三条约束）
class HttpFileService
{
public:
    static HttpFileService &instance();

    // ============================================================================
    // start: 初始化 ZLM 运行时并把 http 静态服务起在指定口
    // ============================================================================
    // rootDir 必须是**绝对路径**：RuntimePaths::alarmsDir() 返回的是随 cwd 变的相对路径
    // （src/base/runtime_paths.h 的文件头说明），传相对路径进去，ZLM 会退回它自己的默认
    // rootPath（可执行文件所在目录/www），症状是"URL 全 404，但本地文件明明在"。
    // port 期望监听端口。返回 false = 没起来（端口被占、已在跑、rootDir 为空），
    // 失败已打 qWarning，调用方据此把媒体上行降级为"只发报警文字"。
    // 幂等：已经 running 时再调一次直接返回 true，不会重复 mk_env_init1。
    bool start(const QString &rootDir, quint16 port);

    // ============================================================================
    // stop: 关掉 ZLM 的全部服务器（进程退出清理里调一次，调完不能再 start）
    // ============================================================================
    void stop();

    quint16 port() const { return port_; }        ///< 实际绑定端口，0 = 没在跑
    bool running() const { return port_ != 0; }

    // ============================================================================
    // baseUrl: 报警 JSON 里那个地址前缀（不含末尾斜杠）
    // ============================================================================
    // overrideUrl 非空 → 原样采用（去掉末尾斜杠），用于对端只能通过 NAT/DNS 名访问的场景，
    //   也用于"本机多网卡、想指定对外那张"。
    // overrideUrl 为空 → 自动：http://<第一个非回环 IPv4>:<实际绑定端口>。
    // 服务没起来、或一个可用 IPv4 都拿不到 → 返回空串（调用方据此不申报媒体）。
    QString baseUrl(const QString &overrideUrl) const;

    /// 第一个可用的非回环 IPv4（过滤 down/loopback，与 QtHelper::getLocalIPs 同一判据，
    /// 但不依赖 ui 域）。空串 = 没有。
    static QString firstUsableIpv4();

private:
    HttpFileService() = default;
    ~HttpFileService() = default;
    HttpFileService(const HttpFileService &) = delete;
    HttpFileService &operator=(const HttpFileService &) = delete;

    bool envReady_ = false;   ///< mk_env_init1 是否已跑过（进程级只许一次）
    quint16 port_ = 0;        ///< 实际绑定端口；0 = 未运行
};

#endif  // HTTP_FILE_SERVICE_H
