#pragma once

// ============================================================================
// runtime_paths.h — 运行时数据路径唯一事实源
// ----------------------------------------------------------------------------
// 约定：config.json / idge.db / backups/ / alarms/ / roll_call_data/ 等
// 运行时产物统一落在启动目录（run.sh 约定 = 项目根）下的 data/ 目录。
// 全部使用 cwd 相对路径，与项目既有"从项目根启动"约定一致。
// 旧版布局（散落在根目录）由 migrateLegacy() 在 main() 最前一次性迁移。
// ============================================================================

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace RuntimePaths {

inline QString dataRoot()   { return QStringLiteral("data"); }
inline QString configFile() { return dataRoot() + QStringLiteral("/config.json"); }
inline QString database()   { return dataRoot() + QStringLiteral("/idge.db"); }
inline QString backupsDir() { return dataRoot() + QStringLiteral("/backups"); }
inline QString alarmsDir()  { return dataRoot() + QStringLiteral("/alarms"); }

inline QString backupFile(const QString &date)
{
    return backupsDir() + QStringLiteral("/idge_") + date + QStringLiteral(".db");
}

// ws 为 IDGE_WORKSPACE（缺省回退当前工作目录），空串按 cwd 处理
inline QString rollCallDir(const QString &ws = QString())
{
    return (ws.isEmpty() ? dataRoot() : ws + QLatin1Char('/') + dataRoot())
           + QStringLiteral("/roll_call_data");
}
inline QString rollCallDb(const QString &ws = QString())
{
    return rollCallDir(ws) + QStringLiteral("/roll_call.db");
}

inline QString assetsTestImage() { return QStringLiteral("assets/test/test.jpg"); }

// 兼容历史 DB 记录：旧报警截图路径以根目录 "alarms/..." 形式存库，
// 目录整体迁入 data/ 后旧记录在此重写前缀，新记录直接存 data/ 相对路径
inline QString resolveStoredPath(const QString &stored)
{
    if (stored.startsWith(QStringLiteral("alarms/")) && !QFileInfo::exists(stored))
        return dataRoot() + QLatin1Char('/') + stored;
    return stored;
}

// 一次性迁移：根目录存在旧布局产物且 data/ 下无同名项时整体挪入 data/。
// 必须在任何数据库/配置打开之前调用（main() 第一行）。
inline void migrateLegacy()
{
    static const QStringList kFiles = {
        QStringLiteral("config.json"),
        QStringLiteral("idge.db"),
        QStringLiteral("idge.db-wal"),
        QStringLiteral("idge.db-shm"),
    };
    for (const QString &f : kFiles) {
        if (QFileInfo::exists(f) && !QFileInfo::exists(dataRoot() + QLatin1Char('/') + f))
            QFile::rename(f, dataRoot() + QLatin1Char('/') + f);
    }

    static const QStringList kDirs = {
        QStringLiteral("backups"),
        QStringLiteral("alarms"),
        QStringLiteral("roll_call_data"),
    };
    for (const QString &d : kDirs) {
        const QString target = (d == QLatin1String("roll_call_data"))
                                   ? rollCallDir()
                                   : dataRoot() + QLatin1Char('/') + d;
        if (QDir(d).exists() && !QDir(target).exists())
            QFile::rename(d, target);
    }

    QDir().mkpath(backupsDir());
    QDir().mkpath(alarmsDir());
    QDir().mkpath(rollCallDir());
}

} // namespace RuntimePaths
