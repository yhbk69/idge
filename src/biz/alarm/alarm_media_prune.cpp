// ============================================================================
// alarm_media_prune.cpp — 见头文件那段（为什么要能注入、为什么要保缩略图）
// ============================================================================
#include "alarm_media_prune.h"

#include <QDir>
#include <QFile>
#include <QFileInfoList>
#include <QStringList>

namespace alarm_media {

namespace {

qint64 dirBytes(const QString &path)
{
    qint64 total = 0;
    const QFileInfoList files = QDir(path).entryInfoList(QDir::Files);
    for (const QFileInfo &fi : files)
        total += fi.size();
    return total;
}

/// 一天目录里的"大文件"（整幅抓拍图、裸流片段、mp4 副本）删掉，sidecar 缩略图留下。
/// 返回释放的字节数与实际删掉的个数（删不动的——正被写、权限——不计入，
/// 调用方按释放量判断有没有落回上限，第二遍兜底自然会把它们连目录一起收掉）。
void stripBigFiles(const QString &path, qint64 *freed, int *removed, int *kept)
{
    const QFileInfoList files = QDir(path).entryInfoList(QDir::Files);
    for (const QFileInfo &fi : files) {
        if (isThumbnailSidecar(fi.fileName())) {
            ++(*kept);
            continue;
        }
        // ⚠ 尺寸必须在删除**之前**取：这里的 QFileInfo 是 entryInfoList 造出来的惰性对象，
        //   文件没了再 size() 回 0。离线用例实测到写反的后果——每一档都"释放了 0 字节"，
        //   于是容量循环一路判不到落回上限，直接走到兜底那一遍把整天目录（连缩略图）
        //   全删了：这次改动的目的正是保住缩略图，写反等于白改还多删。
        const qint64 size = fi.size();
        if (QFile::remove(fi.absoluteFilePath())) {
            *freed += size;
            ++(*removed);
        }
    }
}

}  // namespace

PruneResult pruneDayDirs(const QString &rootPath, int daysToKeep, qint64 maxBytes,
                         const QDate &today)
{
    PruneResult result;
    QDir root(rootPath);
    if (!root.exists())
        return result;

    QStringList dayDirs;                      // 目录名（yyyyMMdd），从旧到新
    const QFileInfoList entries = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &fi : entries) {
        if (QDate::fromString(fi.fileName(), QStringLiteral("yyyyMMdd")).isValid())
            dayDirs << fi.fileName();
    }
    dayDirs.sort();                           // yyyyMMdd 字典序即时间序
    result.dayDirsScanned = dayDirs.size();

    // ── 闸门 1：过期归档，整目录删 ────────────────────────────────────
    const QDate cutoff = today.addDays(-qMax(1, daysToKeep));
    while (!dayDirs.isEmpty() && QDate::fromString(dayDirs.first(), QStringLiteral("yyyyMMdd")) < cutoff) {
        const QString path = root.absoluteFilePath(dayDirs.takeFirst());
        const qint64 freed = dirBytes(path);
        if (QDir(path).removeRecursively()) {
            result.bytesFreed += freed;
            ++result.dayDirsRemoved;
        }
    }

    if (maxBytes <= 0)
        return result;

    // ── 闸门 2：容量上限。第一遍只端大文件，缩略图留着 ─────────────────
    qint64 total = 0;
    for (const QString &name : dayDirs)
        total += dirBytes(root.absoluteFilePath(name));

    for (int i = 0; i < dayDirs.size() && total > maxBytes; ++i) {
        qint64 freed = 0;
        int removed = 0;
        stripBigFiles(root.absoluteFilePath(dayDirs[i]), &freed, &removed,
                      &result.thumbnailsKept);
        total -= freed;
        result.bytesFreed += freed;
        result.bigFilesRemoved += removed;
    }

    // ── 闸门 2 兜底：连缩略图都还超上限，从最旧开始整目录删 ────────────
    // 上限在这里是硬约束：分区写满会拖垮落库与检测，比"看不见三周前的截图"严重得多。
    for (int i = 0; i < dayDirs.size() && total > maxBytes; ++i) {
        const QString path = root.absoluteFilePath(dayDirs[i]);
        const qint64 remain = dirBytes(path);
        if (remain <= 0) {
            QDir(path).removeRecursively();   // 已经空了（或只剩删不掉的），顺手收掉
            continue;
        }
        if (QDir(path).removeRecursively()) {
            total -= remain;
            result.bytesFreed += remain;
            ++result.dayDirsRemoved;
        }
        // 删不动就停：上限再超也不能在这里打转，下一轮 cleanOldData 还会来试
    }

    return result;
}

}  // namespace alarm_media
