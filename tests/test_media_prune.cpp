// ============================================================================
// test_media_prune.cpp — 报警媒体按天目录裁剪的离线自检
// ----------------------------------------------------------------------------
// 覆盖五条契约（判据本体在 src/biz/alarm/alarm_media_prune.cpp）：
//   1) 过期归档整目录删：连缩略图一起没（那时 DB 行同一天也没了，留着没人指向的文件是白占盘）
//   2) 保留期内的目录一个字节都不动（尤其不许"顺手清一下老图"）
//   3) 超上限时**只端大文件、保住 *_thumb.jpg**，且一落回上限就停手
//      —— 这条就是"列表里那条报警还在、点开却无截图"的本体：报警行留 30 天，
//         容量闸门原来一两轮就把整天目录端走了
//   4) 兜底：连缩略图本身都超上限时改回整目录删（分区写满会拖垮落库与检测，
//      上限是硬约束，优先级高于"看得见三周前的截图"）
//   5) 认不出 yyyyMMdd 的目录一律不碰（现场手工放的东西、dispose/ 这类非按天子目录）
//
// 只链 Qt5::Core + alarm_media_prune.cpp：不建窗口、不碰库、不读 config，
// 所以能在板子上把"过期 + 超上限"这两种组合一次跑完。
// 仓库根运行：./build/build_rk3588_linux/test_media_prune，rc 0=PASS 1=FAIL
// ============================================================================

#include "alarm_media_prune.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QStringList>
#include <iostream>
#include <stdexcept>

using alarm_media::PruneResult;
using alarm_media::pruneDayDirs;

namespace {

const QDate kToday(2026, 11, 7);       // "今天"固定住：过期判据不能依赖墙钟

void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}

/// 一天目录：root/yyyyMMdd，里面放 nBig 个大文件（szBig 字节）+ nThumb 张缩略图
void makeDayDir(const QString &root, const QDate &day, int nBig, qint64 szBig, int nThumb)
{
    const QString name = day.toString(QStringLiteral("yyyyMMdd"));
    const QString path = QDir(root).filePath(name);
    QDir().mkpath(path);
    for (int i = 0; i < nBig; ++i) {
        QFile f(QDir(path).filePath(QStringLiteral("ch1_person_%1.jpg").arg(i, 2, 10, QChar('0'))));
        f.open(QIODevice::WriteOnly);
        f.write(QByteArray(int(szBig), char('x')));
        f.close();
    }
    for (int i = 0; i < nThumb; ++i) {
        QFile f(QDir(path).filePath(QStringLiteral("ch1_person_%1_thumb.jpg").arg(i, 2, 10, QChar('0'))));
        f.open(QIODevice::WriteOnly);
        f.write(QByteArray(1024, char('t')));
        f.close();
    }
}

int fileCount(const QString &path)
{
    return QDir(path).entryList(QDir::Files).size();
}

int thumbCount(const QString &path)
{
    int n = 0;
    const QStringList names = QDir(path).entryList(QDir::Files);
    for (const QString &name : names) {
        if (alarm_media::isThumbnailSidecar(name))
            ++n;
    }
    return n;
}

qint64 totalBytes(const QString &root)
{
    qint64 total = 0;
    const QStringList dirs = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &d : dirs) {
        const QStringList files = QDir(QDir(root).filePath(d)).entryList(QDir::Files);
        for (const QString &f : files)
            total += QFileInfo(QDir(QDir(root).filePath(d)).filePath(f)).size();
    }
    return total;
}

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    int rc = 1;
    QTemporaryDir workspace;   // 见下面 include
    if (!workspace.isValid())
        return 1;
    const QString root = QDir(workspace.path()).filePath(QStringLiteral("alarms"));
    QDir().mkpath(root);

    try {
        // ====================================================================
        // 契约 1+2：过期整目录删、保留期内一根手指都不碰
        // daysToKeep=30、今天 11-07 ⇒ cutoff = 10-08；10-01 过期、11-01 在内
        // ====================================================================
        makeDayDir(root, QDate(2026, 10, 1), 3, 100 * 1024, 3);
        makeDayDir(root, QDate(2026, 11, 1), 2, 100 * 1024, 2);
        const QString oldDir = QDir(root).filePath(QStringLiteral("20261001"));
        const QString keepDir = QDir(root).filePath(QStringLiteral("20261101"));

        PruneResult r = pruneDayDirs(root, 30, 4LL * 1024 * 1024 * 1024, kToday);
        check(!QDir(oldDir).exists(), "1) 过期那天该整目录没掉（连缩略图）");
        check(r.dayDirsRemoved == 1, "1) 只删过期那一天的目录");
        check(QDir(keepDir).exists() && fileCount(keepDir) == 4,
              "2) 保留期内的目录一个字节都不动");

        // ====================================================================
        // 契约 3：超上限只端大文件、保住缩略图，且**一落回上限就停手**
        // 三天各 200KB 大文件 + 2 张 1KB 缩略图，上限 450KB（总 606KB）：
        // 端掉最旧那天的 200KB 就已经落回 406KB ≤ 450KB ⇒ 后两天必须整幅还在。
        // "停手"这半边判据和"端大文件"同样重要：真到上限边界时多端一天就是
        // 多丢一天的取证，而那天本来是不该动的。
        // ====================================================================
        QDir(root).removeRecursively();
        QDir().mkpath(root);
        makeDayDir(root, QDate(2026, 11, 3), 2, 100 * 1024, 2);
        makeDayDir(root, QDate(2026, 11, 4), 2, 100 * 1024, 2);
        makeDayDir(root, QDate(2026, 11, 5), 2, 100 * 1024, 2);
        const QString d3 = QDir(root).filePath(QStringLiteral("20261103"));
        const QString d4 = QDir(root).filePath(QStringLiteral("20261104"));
        const QString d5 = QDir(root).filePath(QStringLiteral("20261105"));

        r = pruneDayDirs(root, 30, 450 * 1024, kToday);
check(QDir(d3).exists(), "3) 超上限那天目录本身不许消失（只是瘦身）");
        check(thumbCount(d3) == 2 && fileCount(d3) == 2,
              "3) 最旧那天只端大文件，两张缩略图都留着");
        check(fileCount(d4) == 4 && fileCount(d5) == 4,
              "3) 落回上限之后就停手：后两天的整幅抓拍一张不许少");
        check(r.bigFilesRemoved == 2 && r.thumbnailsKept == 2,
              "3) 统计口径：端掉 2 个大文件、保住 2 张缩略图");
        check(r.dayDirsRemoved == 0, "3) 这一档不该整目录删任何一天");

        // ====================================================================
        // 契约 4：兜底。上限压到"连缩略图都放不下"⇒ 回到整目录删
        // 上一轮残留 406KB（d3 已是 2KB 缩略图，d4/d5 各 202KB），上限 3KB：
        // 第一遍把 d4/d5 也瘦成缩略图（6KB）仍超，第二遍才从最旧开始连目录端掉。
        // 这条存在的意义是"上限是硬约束"：分区写满会拖垮落库与检测，
        // 优先级高于"看得见三周前的截图"。
        // ====================================================================
        r = pruneDayDirs(root, 30, 3 * 1024, kToday);
        check(r.dayDirsRemoved >= 1, "4) 连缩略图都超上限时兜底整目录删");
        check(totalBytes(root) <= 3 * 1024, "4) 兜底之后落回上限内（上限是硬约束）");

        // ====================================================================
        // 契约 5：非按天命名的目录不碰
        // dispose/<alarmId>/ 是处置照片的落盘位置（pad_http_alarms），名字就不是 yyyyMMdd；
        // 现场手工放的 backup-2026 之类同理。
        // ====================================================================
        QDir(root).removeRecursively();
        QDir().mkpath(root);
        makeDayDir(root, QDate(2026, 9, 1), 2, 100 * 1024, 2);      // 过期，会被闸门 1 收掉
        const QString dispose = QDir(root).filePath(QStringLiteral("dispose"));
        QDir().mkpath(QDir(dispose).filePath(QStringLiteral("aa0001")));
        QFile hand(QDir(dispose).filePath(QStringLiteral("keep.jpg")));
        hand.open(QIODevice::WriteOnly);
        hand.write(QByteArray(500 * 1024, char('p')));
        hand.close();

        r = pruneDayDirs(root, 30, 1024, kToday);
        check(QDir(dispose).exists() && QFile::exists(hand.fileName()),
              "5) dispose/ 这种非按天目录一个字节都不动（上限再紧也不碰）");
        check(!QDir(QDir(root).filePath(QStringLiteral("20260901"))).exists(),
              "5) 同轮里按天的过期目录照旧收掉");

        // ====================================================================
        // 契约 5b：上限 <=0 = 不设容量闸门（只有过期那一档生效）
        // ====================================================================
        QDir(root).removeRecursively();
        QDir().mkpath(root);
        makeDayDir(root, QDate(2026, 11, 6), 2, 100 * 1024, 2);
        r = pruneDayDirs(root, 30, 0, kToday);
        check(r.bigFilesRemoved == 0 && r.dayDirsRemoved == 0,
              "5b) maxBytes<=0 时容量闸门不参与，只走过期判据");

        rc = 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << std::endl;
        rc = 1;
    }

    std::cout << (rc == 0 ? "PASS media_prune" : "FAIL media_prune") << std::endl;
    return rc;
}
