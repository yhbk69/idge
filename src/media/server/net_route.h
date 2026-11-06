#ifndef NET_ROUTE_H
#define NET_ROUTE_H

// ============================================================================
// net_route — "这台机器对外是哪张网卡"的判据：读内核路由表的默认路由
// ----------------------------------------------------------------------------
// 为什么需要这一层（2026-11-05 板端实测，现象是"Pad 收到的媒体地址全是 192.168.200.23，
// 图片和直播都打不开"）：
//   板载 4G 模块（Qualcomm MDM9207，idVendor=2dee idProduct=4d38，7 个 USB 接口）挂在
//   板内 USB hub 上，host 侧看就是一个 USB 复合设备：3-1:1.0 由 rndis_host 驱动 →
//   内核多出一张 `usb0`，地址是模块自己内置 DHCP 发的（网关 192.168.200.1、租约约 1 h，
//   其余 1.2~1.5 是 ttyUSB0..3 那几条 AT 口，`nmcli` 认它 TYPE=gsm）。
//   这种口是**点对点**的：除了模块本身，局域网里没人能路由到 192.168.200.23。
//   而 QNetworkInterface::allInterfaces() 按 ifindex 排（本机 usb0=3 在 wlan0=4 前面），
//   所以旧的"取枚举里第一个可用 IPv4"恒定选中 usb0，且只要模块有电就每次都这样。
//
// 判据换成"内核自己认为对外走哪张口"：/proc/net/route 里 Destination 与 Mask 都是
// 00000000 的那些行就是默认路由，metric 小的那条在用（本机 wlan0=600 < usb0=20100）。
// 这张表由内核维护、跟着真实路由决策走，比按网卡名猜可靠；非 Linux / 读不到 / 表格式
// 变了 → 解析出空列表，调用方回落到枚举顺序（行为退化成旧判据，不是报错）。
//
// 这里**只出网卡名顺序，不做可用性过滤**：黑名单、IsUp/IsRunning、IPv4、169.254/16
// 那几道判据留在 http_file_service.cpp 的 usableIpv4On() 里，一个地方一套判据。
// 仍然有猜错的一天（对端走的不是默认路由那张网），所以手填逃生口
// `pad_server.media_base_url` 继续留着。⚠ 但它现在同时管两件事：非空时装配处**连本地
// 8090 都不起**（frmMain.cpp 的 `mediaOverride.isEmpty() && !media.running()`），
// 所以"填前缀"只在这台服务已经起来的前提下有效，填个没人听的地址不会有任何提示。
// ============================================================================

#include <QStringList>

namespace netroute {

/**
 * @brief 从 /proc/net/route 的文本里解析"默认路由的网卡名，按 metric 升序"
 * @param routeTableText 整张表的文本（带表头也没关系；\n 或 \r\n 都吃）
 * @return 去重后的网卡名，metric 小的在前、同 metric 保持表内原顺序；没有默认路由就回空列表
 *
 * 列口径（实测本机表头）：`Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT`
 * —— 第 1 列是口名、第 2 列 Destination、第 7 列 Metric、第 8 列 Mask，全大写十六进制、
 * 按小端字节序存（所以 192.168.124.0/24 写作 007CA8C0），默认路由那两列都是 00000000。
 * 分隔符可能是 tab 也可能是空格、行尾可能一串空格，所以统一 simplified() 再按空格切。
 *
 * 宽容是刻意的：字段不足、metric 不是数、表头行、空行一律跳过，不返回"半张表"也不 assert
 * ——本函数的失败模式必须是"回落到枚举顺序"，不能是崩。
 */
QStringList defaultRouteIfaces(const QString &routeTableText);

/// 读真实的 /proc/net/route 并解析。读不到（非 Linux、权限、文件为空）返回空列表。
QStringList defaultRouteIfacesFromProc();

}  // namespace netroute

#endif  // NET_ROUTE_H
