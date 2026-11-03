// ============================================================================
// device_stats.cpp — 见 device_stats.h（三条板端实测口径都写在那份文件头）
// ============================================================================

#include "device_stats.h"
#include "runtime_paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QStringList>

#include <unistd.h>   // sysconf(_SC_CLK_TCK)

namespace sysinfo {

namespace {

const double kBytesPerGb = 1024.0 * 1024.0 * 1024.0;

// /proc 与 /sys 的读法统一：整读一个文件、失败返回空串。
QString readOneFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(f.readAll()).trimmed();
}

// 取 /proc/stat 首行之后的数字序列用；这里只要"第 n 列"，越界返回 -1。
qint64 fieldAt(const QString &line, int index, bool *ok = nullptr)
{
    // 首列是 "cpu"（用户态汇总行），数值从第 2 列开始
    const QList<QByteArray> parts = line.trimmed().toLatin1().split(' ');
    int seen = 0;
    for (const QByteArray &p : parts) {
        const QByteArray token = p.trimmed();
        if (token.isEmpty()) {
            continue;           // 连续空格会切出空串，不能算一列
        }
        if (seen == 0) {        // "cpu"
            ++seen;
            continue;
        }
        if (seen - 1 == index) {
            bool good = false;
            const qint64 v = token.toLongLong(&good);
            if (ok) {
                *ok = good;
            }
            return good ? v : -1;
        }
        ++seen;
    }
    if (ok) {
        *ok = false;
    }
    return -1;
}

} // namespace

bool readCpuTicks(qint64 *idleMs, qint64 *totalMs)
{
    if (!idleMs || !totalMs) {
        return false;
    }
    const QString content = readOneFile(QStringLiteral("/proc/stat"));
    if (content.isEmpty()) {
        return false;
    }
    const QString cpuLine = content.section(QLatin1Char('\n'), 0, 0);

    // USER_HZ 是 100 才有"1 tick = 10ms"这个换算；内核改了就不能按字面算。
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) {
        hz = 100;
    }
    const double msPerTick = 1000.0 / static_cast<double>(hz);

    bool okIdle = false;
    const qint64 idleTicks = fieldAt(cpuLine, 3, &okIdle);        // idle
    bool okIo = false;
    const qint64 ioTicks = fieldAt(cpuLine, 4, &okIo);            // iowait
    if (!okIdle) {
        return false;
    }

    qint64 totalTicks = 0;
    bool any = false;
    for (int col = 0; col <= 7; ++col) {       // user nice system idle iowait irq softirq steal
        // 再往后的 guest/guest_nice 已经含在 user 里，加进去会重复计一次；
        // 老内核给不满 8 列，取到几列算几列。
        bool colOk = false;
        const qint64 v = fieldAt(cpuLine, col, &colOk);
        if (colOk && v >= 0) {
            totalTicks += v;
            any = true;
        }
    }
    if (!any) {
        return false;
    }

    *idleMs = static_cast<qint64>((idleTicks + (okIo ? ioTicks : 0)) * msPerTick);
    *totalMs = static_cast<qint64>(totalTicks * msPerTick);
    return true;
}

bool cpuPercentFromTicks(qint64 prevIdleMs, qint64 prevTotalMs,
                         qint64 idleMs, qint64 totalMs, double *outPercent)
{
    if (!outPercent) {
        return false;
    }
    const qint64 totalDelta = totalMs - prevTotalMs;
    const qint64 idleDelta = idleMs - prevIdleMs;
    // 时间没往前走 = 没有基线（或 procfs 抽风）。回 0% 会被当成"负载真的很低"，
    // 所以只能报"没值"，让调用方省略字段。
    if (totalDelta <= 0 || idleDelta < 0 || idleDelta > totalDelta) {
        return false;
    }
    const double busy = 1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta);
    double percent = busy * 100.0;
    if (percent < 0.0) {
        percent = 0.0;
    }
    if (percent > 100.0) {
        percent = 100.0;
    }
    *outPercent = percent;
    return true;
}

qint64 memTotalMb()
{
    const QString content = readOneFile(QStringLiteral("/proc/meminfo"));
    const QRegularExpression re(QStringLiteral("MemTotal:\\s+(\\d+)\\s+kB"));
    const QRegularExpressionMatch m = re.match(content);
    return m.hasMatch() ? m.captured(1).toLongLong() / 1024 : 0;
}

qint64 memUsedMb()
{
    const QString content = readOneFile(QStringLiteral("/proc/meminfo"));
    const QRegularExpression totalRe(QStringLiteral("MemTotal:\\s+(\\d+)\\s+kB"));
    // MemAvailable 是 3.14+ 内核才有的；缺了就退到 free+buffers+cached 的老算法。
    const QRegularExpression availRe(QStringLiteral("MemAvailable:\\s+(\\d+)\\s+kB"));
    const QRegularExpressionMatch total = totalRe.match(content);
    if (!total.hasMatch()) {
        return 0;
    }
    const qint64 totalKb = total.captured(1).toLongLong();
    const QRegularExpressionMatch avail = availRe.match(content);
    const qint64 availKb = avail.hasMatch() ? avail.captured(1).toLongLong() : -1;
    if (availKb < 0) {
        return 0;
    }
    return (totalKb - availKb) / 1024;
}

bool readBattery(int *percent, bool *charging)
{
    static const QString kBase = QStringLiteral("/sys/class/power_supply/bat/");
    // present 是"这格子里到底有没有电池"的判据；没读到该键就当没有这个来源，
    // 而不是拿 capacity 的残值报一个假电量。
    const QString present = readOneFile(kBase + QStringLiteral("present"));
    if (present.trimmed() != QLatin1String("1")) {
        return false;
    }
    bool ok = false;
    const int value = readOneFile(kBase + QStringLiteral("capacity")).toInt(&ok);
    if (!ok || value < 0 || value > 100) {
        return false;
    }
    if (percent) {
        *percent = value;
    }
    if (charging) {
        *charging = (readOneFile(kBase + QStringLiteral("status"))
                         .compare(QStringLiteral("Charging"), Qt::CaseInsensitive) == 0);
    }
    return true;
}

bool readSocTempC(double *outC)
{
    if (!outC) {
        return false;
    }
    // ⚠ /sys/class/thermal/thermal_zone* 是**符号链接**：`QDir::Dirs | QDir::NoSymLinks`
    //   在本机列回来是空表（实测 filter=9 → 空，filter=24583 → 8 个 zone 全在），
    //   于是温度项会静默变成"没来源"。必须用 AllEntries。
    const QStringList zones = QDir(QStringLiteral("/sys/class/thermal"))
                                  .entryList(QStringList{QStringLiteral("thermal_zone*")},
                                             QDir::AllEntries | QDir::NoDotAndDotDot);
    if (zones.isEmpty()) {
        return false;
    }
    // 本机 8 个 zone 里第 7 个是 bat(25.0℃)：按下标取会取到电池温度，
    // 所以按 type 找 soc-thermal，实在找不到再用第一个能读到毫度的。
    QString fallback;
    for (const QString &zone : zones) {
        const QString dir = QStringLiteral("/sys/class/thermal/") + zone + QLatin1Char('/');
        const QString type = readOneFile(dir + QStringLiteral("type"));
        const QString raw = readOneFile(dir + QStringLiteral("temp"));
        if (raw.isEmpty()) {
            continue;
        }
        if (type.compare(QStringLiteral("soc-thermal"), Qt::CaseInsensitive) == 0) {
            *outC = raw.toDouble() / 1000.0;
            return true;
        }
        if (fallback.isEmpty() && type.compare(QStringLiteral("bat"), Qt::CaseInsensitive) != 0) {
            fallback = raw;
        }
    }
    if (fallback.isEmpty()) {
        return false;
    }
    *outC = fallback.toDouble() / 1000.0;
    return true;
}

bool readNpuLoadPercent(double *outPercent)
{
    if (!outPercent) {
        return false;
    }
    // 真正的按核负载在 debugfs（uid 1000 打不开，见头文件第 1 条）。
    const QString content = readOneFile(QStringLiteral("/sys/kernel/debug/rknpu/load"));
    if (content.isEmpty()) {
        return false;
    }
    // 形如 "Npu load: 37%, 1000MHz"（各核一行），取最大值当整机使用率。
    const QRegularExpression re(QStringLiteral("(\\d+)\\s*%"));
    double best = -1.0;
    QRegularExpressionMatchIterator it = re.globalMatch(content);
    while (it.hasNext()) {
        best = qMax(best, it.next().captured(1).toDouble());
    }
    if (best < 0.0) {
        return false;
    }
    *outPercent = best;
    return true;
}

bool readUptimeSec(qint64 *outSec)
{
    if (!outSec) {
        return false;
    }
    const QString content = readOneFile(QStringLiteral("/proc/uptime"));
    if (content.isEmpty()) {
        return false;
    }
    bool ok = false;
    const double seconds = content.section(QLatin1Char(' '), 0, 0).toDouble(&ok);
    if (!ok || seconds < 0.0) {
        return false;
    }
    *outSec = static_cast<qint64>(seconds);
    return true;
}

QString osPrettyName()
{
    const QString content = readOneFile(QStringLiteral("/etc/os-release"));
    const QRegularExpression re(
        QStringLiteral("PRETTY_NAME=\"?([^\"\n]+)\"?"),
        QRegularExpression::MultilineOption);
    const QRegularExpressionMatch m = re.match(content);
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

DeviceSampler::DeviceSampler(QString dataPath)
    : dataPath_(std::move(dataPath))
{
    if (dataPath_.isEmpty()) {
        // RuntimePaths 给的是随 cwd 的相对路径，绝对化后才能交给 QStorageInfo 判分区。
        const QString abs = QFileInfo(RuntimePaths::alarmsDir()).absoluteFilePath();
        dataPath_ = abs.isEmpty() ? QStringLiteral("/") : abs;
    }
}

void DeviceSampler::seed()
{
    readCpuTicks(&prevIdleMs_, &prevTotalMs_);
}

DeviceStats DeviceSampler::sample()
{
    DeviceStats stats;

    qint64 idleMs = -1;
    qint64 totalMs = -1;
    if (readCpuTicks(&idleMs, &totalMs)) {
        double percent = 0.0;
        if (prevTotalMs_ > 0 && cpuPercentFromTicks(prevIdleMs_, prevTotalMs_, idleMs, totalMs,
                                                    &percent)) {
            stats.hasCpu = true;
            stats.cpuUsagePercent = percent;
        }
        prevIdleMs_ = idleMs;
        prevTotalMs_ = totalMs;
    }

    const qint64 total = memTotalMb();
    const qint64 used = memUsedMb();
    if (total > 0 && used >= 0) {
        stats.hasMemory = true;
        stats.memTotalMb = total;
        stats.memUsedMb = used;
    }

    int batteryPercent = 0;
    bool charging = false;
    if (readBattery(&batteryPercent, &charging)) {
        stats.hasBattery = true;
        stats.batteryPercent = batteryPercent;
        stats.batteryCharging = charging;
    }

    double tempC = 0.0;
    if (readSocTempC(&tempC)) {
        stats.hasTemperature = true;
        stats.temperatureC = tempC;
    }

    double npu = 0.0;
    if (readNpuLoadPercent(&npu)) {
        stats.hasNpu = true;
        stats.npuUsagePercent = npu;
    }

    const QStorageInfo info(dataPath_);
    if (info.isValid() && info.bytesTotal() > 0) {
        stats.hasDisk = true;
        stats.diskTotalGb = static_cast<double>(info.bytesTotal()) / kBytesPerGb;
        stats.diskUsedGb = static_cast<double>(info.bytesTotal() - info.bytesAvailable()) / kBytesPerGb;
    }

    qint64 uptimeSec = 0;
    if (readUptimeSec(&uptimeSec)) {
        stats.hasUptime = true;
        stats.uptimeSec = uptimeSec;
    }

    stats.systemVersion = osPrettyName();
    return stats;
}

} // namespace sysinfo
