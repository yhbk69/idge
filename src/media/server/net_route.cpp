// ============================================================================
// net_route.cpp — /proc/net/route 的默认路由解析（判据与理由见 net_route.h）
// ----------------------------------------------------------------------------
// 这一层不碰 ZLM、不碰 QNetworkInterface，纯文本进、网卡名列表出，
// 所以能离线喂表验（tests/test_net_route.cpp），不需要真插拔网卡。
// ============================================================================

#include "net_route.h"

#include <QFile>
#include <QSet>
#include <QVector>

#include <algorithm>

namespace netroute {

namespace {

/// 一条默认路由候选：口名 + metric（越小越优先）
struct DefaultRoute {
    QString iface;
    int metric = 0;
};

/**
 * @brief 十六进制列是否"全零"——Destination/Mask 两列用它认默认路由
 * @return 只有"能按 16 位十六进制解出来、且值为 0"才为 true；非数字返回 false
 * @note 按值比而不是比字面量 "00000000"：内核列宽理论上可变，值 0 才是"默认路由"的语义。
 */
bool isHexAllZero(const QString &text)
{
    bool ok = false;
    const uint value = text.toUInt(&ok, 16);
    return ok && value == 0;
}

}  // namespace

QStringList defaultRouteIfaces(const QString &routeTableText)
{
    QVector<DefaultRoute> routes;
    const QStringList rows = routeTableText.split(QLatin1Char('\n'));
    for (const QString &row : rows) {
        // simplified() 顺手把连续 tab/空格压成单个空格并去首尾：表里既有 tab 分隔、
        // 行尾又跟着一长串空格（实测就是这样），按固定列切会错位。
        const QStringList cols = row.simplified().split(QLatin1Char(' '));
        if (cols.size() < 8) {
            continue;   // 空行、表头、被截断的行
        }
        bool metricOk = false;
        const int metric = cols.at(6).toInt(&metricOk);
        if (!metricOk) {
            continue;   // metric 列不是数字 = 表头那行，或内核改了列序 —— 整行不作数
        }
        if (!isHexAllZero(cols.at(1)) || !isHexAllZero(cols.at(7))) {
            continue;   // 网段路由/主机路由也在同一张表里，不能混成"默认路由"
        }
        DefaultRoute route;
        route.iface = cols.at(0);
        route.metric = metric;
        routes.push_back(route);
    }

    // 只按 metric 排，且用 stable_sort：同 metric 时保持表里的原顺序，
    // 免得同样合法的两条路由在一次运行里换来换去（对端看到的地址会变）。
    std::stable_sort(routes.begin(), routes.end(),
                    [](const DefaultRoute &a, const DefaultRoute &b) { return a.metric < b.metric; });

    QStringList out;
    QSet<QString> seen;
    for (const DefaultRoute &route : routes) {
        if (seen.contains(route.iface)) {
            continue;   // 一张口两条默认路由（多网关）→ 只留 metric 小的那个位置
        }
        seen.insert(route.iface);
        out.push_back(route.iface);
    }
    return out;
}

QStringList defaultRouteIfacesFromProc()
{
    QFile table(QStringLiteral("/proc/net/route"));
    if (!table.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QStringList();   // 非 Linux / proc 没挂：调用方回落到枚举顺序
    }
    return defaultRouteIfaces(QString::fromUtf8(table.readAll()));
}

}  // namespace netroute
