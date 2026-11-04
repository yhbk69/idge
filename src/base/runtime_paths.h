#pragma once

/**
 * @file runtime_paths.h
 * @brief base 域 —— 运行时数据路径的唯一事实源（header-only，全部 inline）
 *
 * 所属模块：src/base（被 media/ai/biz/ui 各域直接包含，本文件不依赖任何上层）。
 * 核心职责：集中定义 config.json / idge.db / backups/ / alarms/ / roll_call_data/
 *           这些运行时产物的路径，以及旧布局的一次性迁移。
 *
 * 使用约定（新增运行时产物时必须遵守，理由见 base/README.md）：
 *   - 要加新路径 → 在本文件加一个 inline 常量，**禁止**在业务代码里写路径字面量。
 *     散落的字面量会让"数据到底落在哪"无法一处回答，而迁移逻辑也就无从覆盖它。
 *   - 本文件全是相对路径，所以一切都随 cwd 变（见下面那块"约定"注释），
 *     需要绝对路径的调用方（QStorageInfo、内嵌 http 服务的 rootPath）必须自己
 *     QFileInfo::absoluteFilePath() 之后再传出去——两边都有踩过坑的记录，
 *     分别见 src/base/sysinfo/README.md 与 src/media/server/README.md。
 */
// ============================================================================
// runtime_paths.h — 运行时数据路径唯一事实源
// ----------------------------------------------------------------------------
// 约定：config.json / idge.db / backups/ / alarms/ 等运行时产物统一落在
// **进程当前工作目录**下的 data/ 目录（下面全是相对路径，没有一个绝对路径）。
// ⚠ 这个"当前工作目录"取决于怎么启动：`run.sh` 会先 cd 进
//   build/build_rk3588_linux 再执行 ./idge，所以经 run.sh 跑起来时，真正读写的
//   是 build/build_rk3588_linux/data/，**不是源码根的 data/**；两处 config.json
//   是彼此独立的两份，改错地方就"改了没生效"。想固定就显式 cd 到项目根再启动。
//   （model/ 与 3rdparty/ 由 CMake 在构建目录挂了软链回源码根，不受此影响；
//    roll_call_data 另走 IDGE_WORKSPACE，见 rollCallDir()。）
// 旧版布局（散落在根目录）由 migrateLegacy() 在 main() 最前一次性迁移。
// ============================================================================

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace RuntimePaths {

// 以下五个都是"data/ 下的固定成员"，返回相对路径、永不返回空、不做存在性检查。
// 调用方需要绝对路径或需要 mkdir 时自己处理（alarmsDir() 的按天子目录由
// AlarmManager/解码侧的报警落盘分支负责创建，这里不建目录）。
inline QString dataRoot()   { return QStringLiteral("data"); }
inline QString configFile() { return dataRoot() + QStringLiteral("/config.json"); }
inline QString database()   { return dataRoot() + QStringLiteral("/idge.db"); }
inline QString backupsDir() { return dataRoot() + QStringLiteral("/backups"); }
inline QString alarmsDir()  { return dataRoot() + QStringLiteral("/alarms"); }

/**
 * @brief 某天备份库的完整路径
 * @param date 日期串，格式由调用方决定——现网两处调用点都用 "yyyyMMdd"
 *             （main.cpp 的自动备份），与本仓报警媒体按天目录同一口径
 * @return 相对路径 data/backups/idge_<date>.db；不保证父目录存在
 */
inline QString backupFile(const QString &date)
{
    return backupsDir() + QStringLiteral("/idge_") + date + QStringLiteral(".db");
}

// ws 为 IDGE_WORKSPACE（缺省回退当前工作目录），空串按 cwd 处理
// ⚠ 这是本文件里唯一**不**跟着 data/ 走相对口径的成员：点名数据允许被挪到
//   工作区绝对路径下（frmmain.cpp 读环境变量后转成 QString 传进来），
//   所以同一个进程里 "报警媒体在 cwd 的 data/ 下、点名库在 ws 的 data/ 下" 是合法的，
//   排障时不要按"都在 data/"去推。ws 传空 = 与其它产物同根。
inline QString rollCallDir(const QString &ws = QString())
{
    return (ws.isEmpty() ? dataRoot() : ws + QLatin1Char('/') + dataRoot())
           + QStringLiteral("/roll_call_data");
}
inline QString rollCallDb(const QString &ws = QString())
{
    return rollCallDir(ws) + QStringLiteral("/roll_call.db");
}

// 仓库自带的测试图（设置页「模型测试」的默认输入）。
// 与上面几个不同：它在 assets/ 下而**不是** data/ 下——assets/、model/、3rdparty/
// 都由 CMake 在构建目录挂了软链回源码根（实测 build/build_rk3588_linux/assets 存在），
// 所以这一条是相对路径里少数"两个 cwd 下都指向同一份文件"的；产物类路径不要学它。
// 文件可能不存在（安装树只搬库不搬 assets），调用方必须自查 exists()
// —— frmmain.cpp 的模型测试就是先 QFileInfo::exists 再弹警告。
inline QString assetsTestImage() { return QStringLiteral("assets/test/test.jpg"); }

// 兼容历史 DB 记录：旧报警截图路径以根目录 "alarms/..." 形式存库，
// 目录整体迁入 data/ 后旧记录在此重写前缀，新记录直接存 data/ 相对路径
/**
 * @param stored 数据库里存的原样字符串（可为空）
 * @return 需要补前缀时返回 data/alarms/...，否则**原样返回**
 * @note 判据是"以 alarms/ 开头 **且** 当前 cwd 下该相对路径不存在"，因此结果依赖 cwd：
 *       同一条记录在源码根目录下能直接命中、在构建目录下就会被改写成 data/ 前缀。
 *       这是刻意的（哪条能用就用哪条），但也意味着本函数不能用来"规范化路径"，
 *       拿它的返回值再判等或再比较目录都会漂。
 */
inline QString resolveStoredPath(const QString &stored)
{
    if (stored.startsWith(QStringLiteral("alarms/")) && !QFileInfo::exists(stored))
        return dataRoot() + QLatin1Char('/') + stored;
    return stored;
}

// 一次性迁移：根目录存在旧布局产物且 data/ 下无同名项时整体挪入 data/。
// 必须在任何数据库/配置打开之前调用（main() 第一行）。
/**
 * @brief 把旧布局（散落在仓库根）的运行时产物一次性搬进 data/
 * @note 前置条件：必须在 ConfigManager::load() 与任何 sqlite 打开 idge.db **之前**
 *       调用一次；晚调用就等于对着旧位置写、新位置读，表现是"配置/报警莫名其妙是空的"。
 * @note kFiles 里带上 idge.db-wal / idge.db-shm 不是凑数：SQLite 的 WAL 模式下
 *       -wal 里可能还压着未合并的事务，只搬 .db 不搬 -wal 会让这份 WAL
 *       在下次打开时被重放到另一份库上。三者必须同批移动。
 * @note 失败是**静默**的：QFile::rename 的返回值没有被检查。同名目标已存在时
 *       （即 data/ 下已经有那份文件）刻意跳过、不覆盖现场数据；但跨文件系统
 *       重命名失败（例如根目录与 data/ 不在同一挂载点）也会被同样跳过，
 *       表现就是"东西还在老地方"。要确认真的搬完了得看目录，不能靠返回值。
 * @note roll_call_data 的目标走 rollCallDir()（受 IDGE_WORKSPACE 影响），
 *       与其余几项不同根；末尾的三个 mkpath 顺带把 data/ 本身建出来。
 */
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
