// ============================================================================
// test_net_route.cpp — "本机对外是哪张网卡"的判据离线自检（不碰 socket、不碰 ZLM）
// ----------------------------------------------------------------------------
// 喂文本给 netroute::defaultRouteIfaces()，验默认路由解析这一件事。
// 为什么值得单独钉（2026-11-05 现场故障）：板载 4G 模块（MDM9207，rndis_host）在 host 侧
// 就是一张 `usb0`，ifindex 比 `wlan0` 小，于是"取枚举里第一个可用 IPv4"恒定选中它，
// Pad 收到的 evidenceSnapshotUrl/clipUrl/liveUrl 全成了模块内置 DHCP 给的
// 192.168.200.23 —— 点对点口，局域网里的 Pad 路由不到，图片和直播一起挂。
// 这一层的每个判据都对应一种现场故障：
//   · 排序不看 metric 而看文件顺序 = 内核换了口序就重新挑错；
//   · 网段路由当成默认路由 = 选中的口跟着目的地址变，对端每次拿到的地址都不一样；
//   · 表头/垃圾行让它崩或返回半张表 = 装配处直接不申报媒体地址，退化成"只有文字报警"；
//   · 同 metric 不稳定 = 同一台 Pad 两次连接拿到两个地址，缓存/围栏全部错位。
//
// 覆盖十二条契约：
//   1) 本机实测那张表（wlan0 metric=600、usb0 metric=20100）→ 第一个是 wlan0【回归钉】
//   2) 把 metric 对调 → 第一个变 usb0：证明看的是 metric，不是口名也不是行顺序
//   3) 表里只有网段路由（Destination 或 Mask 非零）→ 空列表
//   4) Destination 为零但 Mask 非零（主机/宽路由那种形状）→ 不算默认路由
//   5) 同一张口两条默认路由（多网关）→ 只出现一次
//   6) 两条默认路由 metric 相同 → 保持表内原顺序（稳定）
//   7) 真实表形状：表头行 + 行间 tab + 行尾一长串空格 + 末尾空行 → 解析正确
//   8) 字段不足 / 垃圾行 → 只跳过那一行，其余照常返回，不崩
//   9) metric 列不是数字（表头那行）→ 该行不作数
//  10) 空文本 / 只有换行 → 空列表
//  11) CRLF（\r\n）分隔 → 同样解析（不把 '\r' 混进口名）
//  12) 真 /proc/net/route 自洽：非空时每项都能在 allInterfaces() 找到同名口且无重复
//     ← 这条把列序钉在**当前内核**上：列一变，合成用例还全绿，这条会红
//
// 仓库根运行：./build/build_rk3588_linux/test_net_route，rc 0=PASS 1=FAIL
// ============================================================================

#include "net_route.h"

#include <QCoreApplication>
#include <QNetworkInterface>
#include <QString>
#include <QStringList>

#include <iostream>

namespace {

int failures = 0;

void check(bool ok, const char *message)
{
    if (!ok) {
        ++failures;
        std::cout << "  FAIL: " << message << std::endl;
    }
}

void report(const char *name)
{
    std::cout << (failures == 0 ? "PASS " : "FAIL ") << name << std::endl;
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // ── 1：本机实测那张表 → wlan0 在前 ────────────────────────────────────────
    // 逐字抄自 2026-11-05 的 /proc/net/route（wlan0 连的是 192.168.124.0/24 那个 WiFi，
    // usb0 是 4G 模块的 rndis 口）。旧判据在这里会回 usb0。
    {
        const QString table = QStringLiteral(
            "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
            "wlan0\t00000000\t017CA8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
            "usb0\t00000000\t01C8A8C0\t0003\t0\t0\t20100\t00000000\t0\t0\t0\n"
            "wlan0\t007CA8C0\t00000000\t0001\t0\t0\t600\t00FFFFFF\t0\t0\t0\n"
            "usb0\t00C8A8C0\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0\n");
        const QStringList ifaces = netroute::defaultRouteIfaces(table);
        check(ifaces.size() == 2, "默认路由只该有两条（网段路由不能混进来）");
        check(ifaces.value(0) == QLatin1String("wlan0"), "metric 600 的 wlan0 必须排第一");
        check(ifaces.value(1) == QLatin1String("usb0"), "metric 20100 的 usb0 排后面");
    }

    // ── 2：metric 对调 → 第一名跟着换，证明判据是 metric ──────────────────────
    {
        const QString table = QStringLiteral(
            "usb0\t00000000\t01C8A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
            "wlan0\t00000000\t017CA8C0\t0003\t0\t0\t20100\t00000000\t0\t0\t0\n");
        check(netroute::defaultRouteIfaces(table).value(0) == QLatin1String("usb0"),
              "metric 小的那张口赢，与口名/行顺序无关");
    }

    // ── 3：只有网段路由 → 空列表（回落到枚举顺序，不是"挑一张最近的"）──────────
    {
        const QString table = QStringLiteral(
            "lo\t0000007F\t00000000\t0001\t0\t0\t0\t00000000\t0\t0\t0\n"
            "wlan0\t007CA8C0\t00000000\t0001\t0\t0\t600\t00FFFFFF\t0\t0\t0\n");
        check(netroute::defaultRouteIfaces(table).isEmpty(),
              "Destination 非零的行不是默认路由，一个都不该返回");
    }

    // ── 4：Destination 零但 Mask 非零 → 也不算默认路由 ────────────────────────
    {
        const QString table = QStringLiteral(
            "usb0\t00000000\t01C8A8C0\t0003\t0\t0\t100\t00FFFFFF\t0\t0\t0\n");
        check(netroute::defaultRouteIfaces(table).isEmpty(),
              "Mask 非全零 = 目的网段受限，不能当默认路由用");
    }

    // ── 5：一张口两条默认路由（多网关）→ 去重 ────────────────────────────────
    {
        const QString table = QStringLiteral(
            "eth0\t00000000\t0101A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n"
            "eth0\t00000000\t0201A8C0\t0003\t0\t0\t300\t00000000\t0\t0\t0\n"
            "usb0\t00000000\t01C8A8C0\t0003\t0\t0\t20100\t00000000\t0\t0\t0\n");
        const QStringList ifaces = netroute::defaultRouteIfaces(table);
        check(ifaces.size() == 2, "同一张口两条默认路由要去重");
        check(ifaces.value(0) == QLatin1String("eth0") && ifaces.value(1) == QLatin1String("usb0"),
              "去重后保留的是 metric 最小的那个位置");
    }

    // ── 6：同 metric → 稳定保持表内原顺序 ────────────────────────────────────
    {
        const QString tableA = QStringLiteral(
            "wlan0\t00000000\t017CA8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
            "eth1\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n");
        const QString tableB = QStringLiteral(
            "eth1\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
            "wlan0\t00000000\t017CA8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n");
        check(netroute::defaultRouteIfaces(tableA).value(0) == QLatin1String("wlan0")
                  && netroute::defaultRouteIfaces(tableB).value(0) == QLatin1String("eth1"),
              "metric 相同要用稳定顺序，否则对端两次拿到两个地址");
    }

    // ── 7：真实表形状（tab 分隔 + 行尾一长串空格 + 空行）──────────────────────
    {
        const QString table = QStringLiteral(
            "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT"
            "                                       \n"
            "wlan0\t00000000\t017CA8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0                 \n"
            "                                                                                    \n"
            "usb0\t00000000\t01C8A8C0\t0003\t0\t0\t20100\t00000000\t0\t0\t0                 \n");
        const QStringList ifaces = netroute::defaultRouteIfaces(table);
        check(ifaces.size() == 2 && ifaces.value(0) == QLatin1String("wlan0"),
              "行尾空格/纯空白行不能污染解析");
    }

    // ── 8：垃圾行只跳过自己，其余照常 ────────────────────────────────────────
    {
        const QString table = QStringLiteral(
            "garbage line without enough fields\n"
            "wlan0\t00000000\t017CA8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
            "########\n"
            "eth9\tzzzzzzzz\t0101A8C0\t0003\t0\t0\t50\t00000000\t0\t0\t0\n"
            "usb0\t00000000\t01C8A8C0\t0003\t0\t0\t20100\t00000000\t0\t0\t0\n");
        const QStringList ifaces = netroute::defaultRouteIfaces(table);
        check(ifaces.size() == 2, "垃圾行与非法十六进制的行都不作数");
        check(ifaces.value(0) == QLatin1String("wlan0") && ifaces.value(1) == QLatin1String("usb0"),
              "垃圾行夹在中间也要按 metric 排对");
    }

    // ── 9：metric 列不是数字（表头那行）→ 该行不作数 ──────────────────────────
    {
        const QString table = QStringLiteral(
            "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n"
            "wlan0\t00000000\t017CA8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n");
        const QStringList ifaces = netroute::defaultRouteIfaces(table);
        check(ifaces.size() == 1 && ifaces.value(0) == QLatin1String("wlan0"),
              "表头行的 Destination/Mask 都是文字，不能混成一条默认路由");
    }

    // ── 10：空文本 → 空列表 ───────────────────────────────────────────────────
    {
        check(netroute::defaultRouteIfaces(QString()).isEmpty(), "空文本回空列表");
        check(netroute::defaultRouteIfaces(QStringLiteral("\n\n\t \n")).isEmpty(),
              "只有换行/空白也回空列表");
    }

    // ── 11：CRLF 分隔 → '\r' 不能混进口名 ────────────────────────────────────
    {
        const QString table = QStringLiteral(
            "wlan0\t00000000\t017CA8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\r\n"
            "usb0\t00000000\t01C8A8C0\t0003\t0\t0\t20100\t00000000\t0\t0\t0\r\n");
        const QStringList ifaces = netroute::defaultRouteIfaces(table);
        check(ifaces.value(0) == QLatin1String("wlan0"),
              "CRLF 的行尾 \\r 要当作空白吃掉，口名必须精确等于 wlan0");
    }

    // ── 12：真 /proc/net/route 自洽（把列序钉在当前内核上）────────────────────
    // 非 Linux / proc 没挂时列表为空 → 整条跳过，不算失败。
    {
        const QStringList real = netroute::defaultRouteIfacesFromProc();
        if (!real.isEmpty()) {
            QStringList uniq;
            bool namesKnown = true;
            const QList<QNetworkInterface> all = QNetworkInterface::allInterfaces();
            for (const QString &name : real) {
                if (uniq.contains(name)) {
                    namesKnown = false;   // 去重判据失效
                    break;
                }
                uniq.append(name);
                bool found = false;
                for (const QNetworkInterface &iface : all) {
                    if (iface.name() == name) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    namesKnown = false;
                    break;
                }
            }
            check(namesKnown,
                  "真路由表解出来的口名必须都能在 allInterfaces() 里找到、且不重复");
        } else {
            std::cout << "  SKIP: /proc/net/route 读不到（非 Linux），契约 12 不参与判定"
                      << std::endl;
        }
    }

    report("test_net_route");
    return failures == 0 ? 0 : 1;
}
