#pragma once

/**
 * @file gb28181_ssrc.h
 * @brief GB28181 SSRC 字符串的解析与应答选择
 *
 * 国标里 SSRC 是**十进制**字符串、固定补零到 10 位（前 5 位为信令来源编码、
 * 后 5 位为序号），不是 RTP 头里那个 32 位二进制字段；因此这里按字符逐位解析，
 * 而不是 strtol。0 视为非法（平台下发 y= 缺省或全零时不能直接回显）。
 */

#include <cstdint>
#include <limits>
#include <string>

/**
 * @brief 解析十进制 SSRC 字符串
 * @return 合法（纯数字、长度≤10、值在 (0, UINT32_MAX]）时 true，否则 false 且不写 value
 */
inline bool gb28181ParseSsrc(const std::string& text, uint32_t& value) {
    if (text.empty() || text.size() > 10) return false;
    uint64_t parsed = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        parsed = parsed * 10 + static_cast<unsigned>(c - '0');
    }
    if (parsed == 0 || parsed > std::numeric_limits<uint32_t>::max()) return false;
    value = static_cast<uint32_t>(parsed);
    return true;
}

/**
 * @brief 选出应答用的 SSRC 并补零成 10 位
 * @param offered  INVITE SDP 里平台给的 y= 值，为空时退回 fallback
 * @param fallback 流配置里的 ssrc
 * @param answer    输出：10 位补零的十进制字符串
 * @return 两个来源都不是合法 SSRC 时 false（调用方应回 488）
 */
inline bool gb28181SelectSsrc(const std::string& offered, const std::string& fallback,
                             std::string& answer) {
    uint32_t value = 0;
    if (!gb28181ParseSsrc(offered.empty() ? fallback : offered, value)) return false;
    answer = std::to_string(value);
    answer.insert(0, 10 - answer.size(), '0');
    return true;
}
