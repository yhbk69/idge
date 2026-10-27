#ifndef IDGE_GB28181_RTP_H
#define IDGE_GB28181_RTP_H

/**
 * @file gb28181_rtp.h
 * @brief 国标媒体出口：H.264 → MPEG-PS 打包（Gb28181PsMuxer）+ RTP 发送（Gb28181RtpSender）
 *
 * 国标平台收的是 PT=96 的 PS 流（一个视频帧装成一个 PS 包，再切片发 RTP），
 * 不是裸 H.264。所以链路是 media → PsMuxer::writeVideoFrame → 回调 → RtpSender::sendPs。
 */

#include <cstdint>
#include <atomic>
#include <functional>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <vector>

/**
 * @brief 一路媒体会话的 RTP 出口（UDP 数据报或 TCP 上的 RFC4571 长度前缀）
 *
 * 传输方式由 INVITE 的 SDP 决定，不看 config.json 的 transport。
 * 线程安全：sendPs()/stop() 内部加锁，可被媒体线程与 SIP 线程交替调用。
 */
class Gb28181RtpSender {
public:
    Gb28181RtpSender();
    ~Gb28181RtpSender();

    /**
     * @brief 打开到平台媒体收端的 socket（重复调用会先关掉上一个）
     * @param transport "tcp" 走 SOCK_STREAM 并立即 connect，其余一律按 UDP
     * @param ssrc      十进制 SSRC 字符串，解析不了直接返回 false
     * @return host 非 IPv4 字面量、SSRC 非法、或 TCP connect 失败时 false
     *
     * 序列号每次 open 随机起（国标只要求同一会话内单调，不回绕判定由收端做）。
     */
    bool open(const std::string &host, uint16_t port, const std::string &transport,
              const std::string &ssrc);
    void stop();

    /**
     * @brief 把一个完整 PS 包切成 ≤1400 字节的 RTP 载荷发出
     * @return 任一分片失败即 false；TCP 下失败会顺手关 socket，
     *         调用方（manager）据此停掉媒体源、等下一次 INVITE 重建
     *
     * 载荷末片的 RTP marker 位置 1，平台以此判一帧结束。
     */
    bool sendPs(const std::vector<uint8_t> &ps, uint32_t timestamp);

    /// 最近一次失败的 errno（成功发送会清 0），仅用于打日志定位
    int lastError() const { return lastError_.load(); }

private:
    int socket_ = -1;
    bool tcp_ = false;
    uint32_t ssrc_ = 0;
    uint16_t sequence_ = 0;
    struct sockaddr_in *remote_ = nullptr;
    std::mutex mutex_;
    std::atomic<int> lastError_{0};

    /// 调用方（sendPs）须已持 mutex_
    bool sendPacket(const uint8_t *data, size_t size);
};

/**
 * @brief 把 Annex-B H.264 帧封成国标 PS 包（PS Pack Header + 关键帧带 PSM + PES）
 *
 * 只处理视频（STREAM_ID 0xE0），不产 PCR/系统流，属国标实测够用的最小打包。
 * SPS/PPS 会被缓存，随每个关键帧一起塞进载荷，保证平台随时能从 I 帧起解。
 */
class Gb28181PsMuxer {
public:
    using PacketCallback = std::function<void(const std::vector<uint8_t> &, uint32_t)>;

    /// 每个 PS 包通过 callback 交出去（在写帧的那个线程上同步执行）
    explicit Gb28181PsMuxer(PacketCallback callback);

    /// 丢弃缓存的 SPS/PPS 与"已起流"标志：重建 RTP 会话时必调，否则新会话会
    /// 直接从一个非 IDR 帧开始发，平台侧解不出画面
    void reset();

    /// 等不到"SPS+PPS+IDR 齐全"的第一帧前，所有输入帧都被静默丢弃（不半截起流）
    void writeVideoFrame(const std::vector<uint8_t> &annexB, uint32_t pts90k, bool keyFrame);

private:
    PacketCallback callback_;
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
    bool started_ = false;
};

#endif
