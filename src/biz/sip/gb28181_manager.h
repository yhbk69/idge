#ifndef IDGE_GB28181_MANAGER_H
#define IDGE_GB28181_MANAGER_H

/**
 * @file gb28181_manager.h
 * @brief GB28181 设备侧信令管理器（REGISTER / 心跳 / MANSCDP 查询 / INVITE 点流）
 *
 * 一个 manager 管一条本机 UDP socket（localSipPort），上面跑所有 enabled 流
 * 的注册、心跳与信令应答；媒体侧每条流各有一对 Gb28181MediaSource + RtpSender
 * + PsMuxer，由平台 INVITE 触发才起（详见 gb28181_manager.cpp 文件头）。
 */

#include "gb28181_config.h"
#include "gb28181_media.h"
#include "gb28181_rtp.h"

#include <atomic>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/**
 * @brief 国标信令与会话管理（非 QObject，跑在自己的 std::thread 上）
 *
 * 生命周期：start(configPath) 读配置、开 socket、起事件线程；stop()（析构自动调）
 * 停事件线程并逐路停媒体。running() 只反映 manager 线程，不代表已注册成功。
 */
class Gb28181Manager {
public:
    Gb28181Manager();
    /// 等价于 stop()；事件线程与各路媒体线程都在此 join，析构后不留后台线程
    ~Gb28181Manager();

    /// 读配置失败、socket 开不起来、或所有流都 enabled=false，都返回 false（自带 qWarning）
    bool start(const QString &configPath);
    /**
     * @brief 停事件线程 → 关 socket → 逐路停媒体 → 清空 streams_/calls_
     *
     * 可重复调用（幂等）；未 start() 过直接调用只走兜底清理，不解 joinable 的线程。
     * 会阻塞：join 事件线程最长等一轮 select（200ms），join 媒体线程最长等
     * ffmpeg 的 interrupt_callback 生效（开流阻塞期间），所以不要在需要秒退的路径上
     * 指望它立即返回。调用方须是 start() 的那个线程（main.cpp）。
     */
    void stop();
    /// 只反映事件线程是否活着，不代表已注册成功（注册状态在 Runtime::registered）
    bool running() const { return running_.load(); }

private:
    /// 一路通道的全量运行态：配置 + 媒体三件套 + 注册/会话协商出来的 SIP 字段
    struct Runtime {
        Gb28181StreamConfig config;
        std::unique_ptr<Gb28181MediaSource> media;
        std::unique_ptr<Gb28181RtpSender> rtp;
        std::unique_ptr<Gb28181PsMuxer> muxer;
        bool registered = false;   ///< 收到 REGISTER 的 2xx；心跳只在这之后才开始（平台侧口径）
        bool pushing = false;      ///< startMedia() 是否已把媒体源起起来，stopMedia() 据此判重
        // 401 挑战带回来的摘要认证参数
        QString nonce;
        QString realm;
        QString qop;               ///< 挑战里多值（"auth,auth-int"）时只取第一个；非空走 qop 分支
        QString cnonce;
        QString registerCallId;    ///< 注册事务的 Call-ID，handleResponse() 靠它反查是哪一路
        // INVITE 建立的媒体会话
        QString callId;
        QString remoteHost;      ///< 媒体收端地址，来自 INVITE 的 SDP c=/m=
        uint16_t remotePort = 0;
        QString transport;       ///< udp|tcp，同样取自 SDP，与配置里的 transport 无关
        QString sessionSsrc;     ///< 协商后的 10 位十进制 SSRC，写进应答 SDP 的 y=
        int cseq = 1;            ///< SIP CSeq 与 MANSCDP SN 共用这一个计数器，只保证单调不回绕
        qint64 nextRegisterMs = 0;   ///< 墙钟毫秒；0 = 立刻发 REGISTER（start() 与首轮都靠它）
        qint64 nextHeartbeatMs = 0;  ///< 注册成功时被置 0，让第一条 Keepalive 不等间隔就发出去
        // 计数由媒体线程在 muxer 回调里自增，SIP 线程在 startMedia/stopMedia 读；
        // 非原子，只用于打日志，别拿它做判决条件。
        uint64_t psFrames = 0;
        uint64_t rtpSendErrors = 0;
    };

    Gb28181Config config_;
    std::vector<std::unique_ptr<Runtime>> streams_;
    std::unordered_map<std::string, Runtime *> calls_;   ///< call-id → 会话，仅事件线程访问
    int socket_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
    uint64_t callCounter_ = 1;

    /// 事件线程主体：首轮无认证注册 → select(200ms) 收包 → 推进续注/心跳；读 streams_/calls_ 的唯一线程
    void eventLoop();
    /// 绑 localHost:localSipPort；localHost 非 IPv4 字面量或 bind 失败返回 false（自带 qWarning）
    bool openSocket();
    /// shutdown + close 并把 socket_ 复位 -1，可重复调用
    void closeSocket();
    /// @param withAuth false=首轮裸发、等 401；true=带 Digest 重发
    /// 副作用：写 runtime.registerCallId/cseq/nextRegisterMs；serverHost 不是 IPv4 字面量时静默 return（无日志）
    void sendRegister(Runtime &runtime, bool withAuth);
    /// 发 MESSAGE/Keepalive，顺带把 nextHeartbeatMs 推后 heartbeatInterval 秒；同上，serverHost 非法则静默 return
    void sendHeartbeat(Runtime &runtime);
    /// 收包分派：以 "SIP/2.0" 开头判响应，否则当请求（peer 是请求的来址，回包目标）
    void handleDatagram(const std::string &message, const sockaddr_in &peer);
    /// 按 Call-ID 认领注册事务：401/407 抽认证参数并带认证重发，2xx 置 registered
    void handleResponse(const std::string &message);
    /// 按方法分派 MESSAGE/INVITE/BYE；其余（含 ACK）不处理——推流在 200 OK 之后就已开始
    void handleRequest(const std::string &message, const sockaddr_in &peer);
    /// MANSCDP 查询应答：只认 DeviceInfo / Catalog / DeviceStatus，其余命令静默不回 Response
    void handleQuery(Runtime &runtime, const QString &requestBody, const sockaddr_in &peer);
    /// 协商媒体收端与 SSRC（y= 非法回 488），回 200 OK 后起媒体
    void handleInvite(Runtime &runtime, const std::string &message, const sockaddr_in &peer);
    /// 回 200 OK、停媒体并清 callId；流配置保留，平台可再次 INVITE
    void handleBye(Runtime &runtime, const std::string &message, const sockaddr_in &peer);
    /// 开 RTP 出口 → reset muxer → 起媒体线程；只被事件线程调用
    void startMedia(Runtime &runtime);
    /// 停媒体线程并关 RTP 出口；事件线程与 stop() 都会调，内部靠 media->stop() 保证幂等
    void stopMedia(Runtime &runtime);

    /// 生成"设备编码-墙钟毫秒-自增计数@对外地址"形式的 Call-ID；callCounter_ 非原子，只在事件线程用
    QString newCallId(const QString &deviceCode);
    /// Via/From/To/Contact 里本机那一段；0.0.0.0 在配置校验阶段就被拦掉，这里不再兜底
    QString advertisedAddress() const;
    /// RFC2617 MD5 摘要；nonce 为空返回空串（调用方据此就不该发带认证的请求）
    QString digestAuthorization(const Runtime &runtime, const QString &method,
                                const QString &uri) const;
    /// 唯一的发包出口：sendto 失败不重试也不报错，靠注册/心跳的周期性重发兜底
    void sendMessage(const std::string &message, const sockaddr_in &peer);
};

#endif
