/**
 * @file gb28181_rtp.cpp
 * @brief PS 打包与 RTP 发送的实现（手写比特流，无第三方打包库）
 *
 * 载荷切片固定 1400 字节：留出的余量给 12 字节 RTP 头 + PS 头 + UDP 分片安全边界，
 * 按 1500 MTU 算不会踢出第二个 IP 分片，比按 1300/1472 调的人都稳。
 *
 * RTP 头是 12 字节定长、无扩展、无 CSRC、无填充，所以 packet[0] 恒 0x80；
 * PT 固定 96（PS/90000，与 200 OK 里答的 m=video 那行必须一致）。
 * timestamp 用的是帧的 90kHz PTS，一个 PS 包内的所有分片共用同一个时间戳。
 *
 * TCP 模式走 RFC4571（2 字节大端长度前缀 + 整包 send 全量重试），发送失败会主动
 * 关掉 socket：因为 TCP 上一旦某个分片写坏，后面的字节流就错位了，留着不如重建。
 * UDP 失败只报 errno，socket 留着继续发（丢包是正常现象）。
 *
 * PS 侧只发 Pack Header + 关键帧带一张 PSM + 一个 PES(stream_id 0xE0)，
 * 不发 System Header。时钟一律跟随帧 PTS：Pack Header 的 SCR 直接取 pts，
 * PES 只带 PTS 不带 DTS，PSM 里 ES_cr 留空 —— 即"没有独立系统时钟，
 * 解码时钟就是显示时钟"，对只有视频、B 帧-less 的 zerolatency 编码是成立的。
 * 换成分层/B 帧流要重看这里。
 */
#include "gb28181_rtp.h"
#include "gb28181_ssrc.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <random>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {
constexpr size_t kRtpPayload = 1400;

void appendPts(std::vector<uint8_t> &out, uint64_t pts) {
    pts &= 0x1FFFFFFFFULL;
    out.push_back(static_cast<uint8_t>(0x21 | (((pts >> 30) & 0x07) << 1)));
    out.push_back(static_cast<uint8_t>(pts >> 22));
    out.push_back(static_cast<uint8_t>(((pts >> 14) & 0xFE) | 1));
    out.push_back(static_cast<uint8_t>(pts >> 7));
    out.push_back(static_cast<uint8_t>(((pts << 1) & 0xFE) | 1));
}

std::vector<uint8_t> packHeader(uint64_t pts) {
    const uint64_t scr = pts & 0x1FFFFFFFFULL;
    std::vector<uint8_t> h = {0, 0, 1, 0xBA, 0, 0, 0, 0, 0, 1, 0xFF, 0xFF, 0xFF, 0xF8};
    h[4] = static_cast<uint8_t>(0x44 | ((scr >> 27) & 0x38) | ((scr >> 28) & 0x03));
    h[5] = static_cast<uint8_t>(scr >> 20);
    h[6] = static_cast<uint8_t>(((scr >> 12) & 0xF8) | 0x04 | ((scr >> 13) & 0x03));
    h[7] = static_cast<uint8_t>(scr >> 5);
    h[8] = static_cast<uint8_t>(((scr << 3) & 0xF8) | 0x04);
    h[9] = 1;
    return h;
}

std::vector<uint8_t> pes(const std::vector<uint8_t> &payload, uint64_t pts) {
    std::vector<uint8_t> out = {0, 0, 1, 0xE0, 0, 0, 0x80, 0x80, 5};
    const size_t length = 3 + 5 + payload.size();
    const uint16_t encodedLength = length > 0xFFFF ? 0 : static_cast<uint16_t>(length);
    out[4] = static_cast<uint8_t>(encodedLength >> 8);
    out[5] = static_cast<uint8_t>(encodedLength);
    appendPts(out, pts);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> psm() {
    std::vector<uint8_t> map = {0, 0, 1, 0xBC, 0, 0, 0xE1, 0xFF, 0, 0, 0, 4,
                                0x1B, 0xE0, 0, 0};
    const uint16_t mapLength = static_cast<uint16_t>(map.size() - 6 + 4);
    map[4] = static_cast<uint8_t>(mapLength >> 8);
    map[5] = static_cast<uint8_t>(mapLength);
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < map.size(); ++i) {
        crc ^= static_cast<uint32_t>(map[i]) << 24;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80000000U) ? (crc << 1) ^ 0x04C11DB7U : (crc << 1);
    }
    map.push_back(static_cast<uint8_t>(crc >> 24)); map.push_back(static_cast<uint8_t>(crc >> 16));
    map.push_back(static_cast<uint8_t>(crc >> 8)); map.push_back(static_cast<uint8_t>(crc));
    return map;
}

struct Nalu { const uint8_t *data = nullptr; size_t size = 0; int type = 0; };

std::vector<Nalu> splitNalus(const std::vector<uint8_t> &data) {
    std::vector<Nalu> result;
    size_t i = 0;
    auto findStart = [&](size_t from, size_t *length) -> size_t {
        for (size_t p = from; p + 3 <= data.size(); ++p) {
            if (data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 1) { *length = 3; return p; }
            if (p + 4 <= data.size() && data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 0 && data[p + 3] == 1) { *length = 4; return p; }
        }
        return data.size();
    };
    while (i < data.size()) {
        size_t startLength = 0;
        const size_t start = findStart(i, &startLength);
        if (!startLength || start + startLength >= data.size()) break;
        size_t nextLength = 0;
        const size_t next = findStart(start + startLength, &nextLength);
        const size_t end = nextLength ? next : data.size();
        if (end > start + startLength) {
            const uint8_t *nalu = data.data() + start + startLength;
            result.push_back({nalu, end - start - startLength, nalu[0] & 0x1F});
        }
        i = end;
    }
    return result;
}
}

Gb28181RtpSender::Gb28181RtpSender() : remote_(new sockaddr_in{}) {}

Gb28181RtpSender::~Gb28181RtpSender() {
    stop();
    delete remote_;
}

bool Gb28181RtpSender::open(const std::string &host, uint16_t port, const std::string &transport,
                            const std::string &ssrc) {
    stop();
    lastError_ = 0;
    ssrc_ = 0;
    if (!gb28181ParseSsrc(ssrc, ssrc_)) return false;
    tcp_ = transport == "tcp";
    socket_ = ::socket(AF_INET, tcp_ ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (socket_ < 0) return false;
    std::memset(remote_, 0, sizeof(*remote_));
    remote_->sin_family = AF_INET;
    remote_->sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &remote_->sin_addr) != 1) { stop(); return false; }
    sequence_ = static_cast<uint16_t>(std::random_device{}());
    if (tcp_ && ::connect(socket_, reinterpret_cast<sockaddr *>(remote_), sizeof(*remote_)) < 0) { stop(); return false; }
    return true;
}

void Gb28181RtpSender::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (socket_ >= 0) { ::close(socket_); socket_ = -1; }
}

bool Gb28181RtpSender::sendPacket(const uint8_t *data, size_t size) {
    if (socket_ < 0) { lastError_ = ENOTCONN; return false; }
    if (tcp_) {
        if (size > 0xFFFF) { lastError_ = EMSGSIZE; return false; }
        const uint8_t header[2] = {static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size)};
        const auto sendAll = [this](const uint8_t* bytes, size_t length) {
            size_t offset = 0;
            while (offset < length) {
                const ssize_t sent = ::send(socket_, bytes + offset, length - offset, MSG_NOSIGNAL);
                if (sent < 0 && errno == EINTR) continue;
                if (sent <= 0) { lastError_ = sent < 0 ? errno : ECONNRESET; return false; }
                offset += static_cast<size_t>(sent);
            }
            return true;
        };
        if (!sendAll(header, sizeof(header)) || !sendAll(data, size)) return false;
        return true;
    }
    const ssize_t sent = ::sendto(socket_, data, size, MSG_NOSIGNAL, reinterpret_cast<sockaddr *>(remote_), sizeof(*remote_));
    if (sent != static_cast<ssize_t>(size)) { lastError_ = sent < 0 ? errno : EIO; return false; }
    return true;
}

bool Gb28181RtpSender::sendPs(const std::vector<uint8_t> &ps, uint32_t timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (socket_ < 0 || ps.empty()) { lastError_ = socket_ < 0 ? ENOTCONN : EINVAL; return false; }
    lastError_ = 0;
    size_t offset = 0;
    while (offset < ps.size()) {
        const size_t count = std::min(kRtpPayload, ps.size() - offset);
        std::vector<uint8_t> packet(12 + count);
        packet[0] = 0x80;
        packet[1] = static_cast<uint8_t>((offset + count == ps.size() ? 0x80 : 0) | 96);
        packet[2] = static_cast<uint8_t>(sequence_ >> 8); packet[3] = static_cast<uint8_t>(sequence_++);
        packet[4] = static_cast<uint8_t>(timestamp >> 24); packet[5] = static_cast<uint8_t>(timestamp >> 16);
        packet[6] = static_cast<uint8_t>(timestamp >> 8); packet[7] = static_cast<uint8_t>(timestamp);
        packet[8] = static_cast<uint8_t>(ssrc_ >> 24); packet[9] = static_cast<uint8_t>(ssrc_ >> 16);
        packet[10] = static_cast<uint8_t>(ssrc_ >> 8); packet[11] = static_cast<uint8_t>(ssrc_);
        std::memcpy(packet.data() + 12, ps.data() + offset, count);
        if (!sendPacket(packet.data(), packet.size())) {
            if (tcp_) { ::close(socket_); socket_ = -1; }
            return false;
        }
        offset += count;
    }
    return true;
}

Gb28181PsMuxer::Gb28181PsMuxer(PacketCallback callback) : callback_(std::move(callback)) {}

void Gb28181PsMuxer::reset() {
    sps_.clear(); pps_.clear(); started_ = false;
}

void Gb28181PsMuxer::writeVideoFrame(const std::vector<uint8_t> &annexB, uint32_t pts90k, bool keyFrame) {
    const auto nalus = splitNalus(annexB);
    if (nalus.empty()) return;
    std::vector<uint8_t> idr;
    std::vector<uint8_t> other;
    for (const Nalu &nalu : nalus) {
        if (nalu.type == 7) sps_.assign(nalu.data, nalu.data + nalu.size);
        else if (nalu.type == 8) pps_.assign(nalu.data, nalu.data + nalu.size);
        else if (nalu.type == 5) { idr.insert(idr.end(), {0, 0, 0, 1}); idr.insert(idr.end(), nalu.data, nalu.data + nalu.size); }
        else if (nalu.type == 1) { other.insert(other.end(), {0, 0, 0, 1}); other.insert(other.end(), nalu.data, nalu.data + nalu.size); }
    }
    if (!started_) {
        if (idr.empty() || sps_.empty() || pps_.empty()) return;
        started_ = true;
    }
    std::vector<uint8_t> payload;
    const bool idrFrame = keyFrame || !idr.empty();
    if (idrFrame) {
        payload.insert(payload.end(), {0, 0, 0, 1}); payload.insert(payload.end(), sps_.begin(), sps_.end());
        payload.insert(payload.end(), {0, 0, 0, 1}); payload.insert(payload.end(), pps_.begin(), pps_.end());
        payload.insert(payload.end(), idr.begin(), idr.end());
    } else payload = std::move(other);
    if (payload.empty()) return;
    std::vector<uint8_t> ps = packHeader(pts90k);
    if (idrFrame) { const auto map = psm(); ps.insert(ps.end(), map.begin(), map.end()); }
    const auto packet = pes(payload, pts90k);
    ps.insert(ps.end(), packet.begin(), packet.end());
    if (callback_) callback_(ps, pts90k);
}
