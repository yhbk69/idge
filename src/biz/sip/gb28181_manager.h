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
    ~Gb28181Manager();

    /// 读配置失败、socket 开不起来、或所有流都 enabled=false，都返回 false（自带 qWarning）
    bool start(const QString &configPath);
    void stop();
    bool running() const { return running_.load(); }

private:
    /// 一路通道的全量运行态：配置 + 媒体三件套 + 注册/会话协商出来的 SIP 字段
    struct Runtime {
        Gb28181StreamConfig config;
        std::unique_ptr<Gb28181MediaSource> media;
        std::unique_ptr<Gb28181RtpSender> rtp;
        std::unique_ptr<Gb28181PsMuxer> muxer;
        bool registered = false;
        bool pushing = false;
        // 401 挑战带回来的摘要认证参数
        QString nonce;
        QString realm;
        QString qop;
        QString cnonce;
        QString registerCallId;
        // INVITE 建立的媒体会话
        QString callId;
        QString remoteHost;      ///< 媒体收端地址，来自 INVITE 的 SDP c=/m=
        uint16_t remotePort = 0;
        QString transport;       ///< udp|tcp，同样取自 SDP，与配置里的 transport 无关
        QString sessionSsrc;
        int cseq = 1;
        qint64 nextRegisterMs = 0;
        qint64 nextHeartbeatMs = 0;
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

    void eventLoop();
    bool openSocket();
    void closeSocket();
    void sendRegister(Runtime &runtime, bool withAuth);
    void sendHeartbeat(Runtime &runtime);
    void handleDatagram(const std::string &message, const sockaddr_in &peer);
    void handleResponse(const std::string &message);
    void handleRequest(const std::string &message, const sockaddr_in &peer);
    void handleQuery(Runtime &runtime, const QString &requestBody, const sockaddr_in &peer);
    void handleInvite(Runtime &runtime, const std::string &message, const sockaddr_in &peer);
    void handleBye(Runtime &runtime, const std::string &message, const sockaddr_in &peer);
    void startMedia(Runtime &runtime);
    void stopMedia(Runtime &runtime);

    QString newCallId(const QString &deviceCode);
    QString advertisedAddress() const;
    QString digestAuthorization(const Runtime &runtime, const QString &method,
                                const QString &uri) const;
    void sendMessage(const std::string &message, const sockaddr_in &peer);
};

#endif
