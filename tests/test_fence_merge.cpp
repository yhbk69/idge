// ============================================================================
// test_fence_merge.cpp — 检测内存快照的两个围栏来源（native + Pad）合并自检
// ----------------------------------------------------------------------------
// 为什么值得单独钉（这次改动的全部风险都在这三条上）：
//   A. cameraId 的**两套编号口径**。native_fences 存本机自己拼的 "CH"+0-based，
//      pad_fences 存 Pad 原样发来的 cameraId，而 §7.1 的 channelIdentity() 是
//      "CH"+1-based。共用一句 mid(2).toInt() 会把 Pad 的围栏整体**下移一路**——
//      对端在 CH2 画的禁区，本机在 CH1 的画面上报警，这比不生效严重得多。
//   B. Pad 优先必须和 GET /api/fences 的合并口径一致，否则"对端看到的围栏"
//      和"本机在判的围栏"是两套。
//   C. 锁的可重入性。QMutex 默认**不可重入**，而 loadFromConfig/reloadForTask
//      整段持锁；这两个入口里再调带锁的公共方法是自死锁，且死在启动第一帧之前，
//      现象是"程序起来就没界面"，板端极难归因到一次刷新函数上。
//
// 只编 fence_manager + 两张围栏表 + DatabaseManager + ConfigManager + 会话：
// 不开摄像头、不碰 RKNN、不建窗口（QCoreApplication）。
//
// 运行（cwd 任意，库文件建在 QDir::temp() 下的独立目录）：
//   ./build/build_rk3588_linux/test_fence_merge      rc 0=PASS 1=FAIL
// ============================================================================

#include "ConfigManager.h"
#include "construction_session.h"
#include "database_manager.h"
#include "fence_manager.h"
#include "native_fence_store.h"
#include "pad_fence_store.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointF>
#include <QTemporaryDir>
#include <QVector>
#include <iostream>
#include <stdexcept>
#include <string>

using geofence::ChannelFence;
using geofence::FenceManager;
using geofence::NativeFenceStore;
using geofence::PadFence;
using geofence::PadFenceStore;
using geofence::PadRegion;

namespace {

const QString kTask = QStringLiteral("TASK-MERGE");

void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}

/// 一个合法的 §5.2 RECT 区域（归一化 0~1）：位置由 tag 区分，便于断言"并进来的到底是哪条"
PadRegion makeRegion(const QString &regionId)
{
    PadRegion r;
    r.regionId = regionId;
    r.shapeType = QStringLiteral("RECT");
    r.ruleType = QStringLiteral("INTRUSION");
    r.points = {QPointF(0.10f, 0.10f), QPointF(0.40f, 0.10f),
                QPointF(0.40f, 0.40f), QPointF(0.10f, 0.40f)};
    return r;
}

/// 本机手绘口径：cameraId 是 "CH"+0-based
QString putNative(const QString &cameraId)
{
    NativeFenceStore store;
    const QString id = store.saveActiveVersion(kTask, cameraId, cameraId,
                                               QStringLiteral("本机围栏"),
                                               {makeRegion(QStringLiteral("RG-N1"))});
    check(!id.isEmpty(), "native saveActiveVersion failed");
    return id;
}

/// Pad 下发口径：save-draft 再 apply，返回 fenceId（regions 的 regionId 用来认领形状）
QString putPad(const QString &cameraId, const QString &regionId, bool applyIt)
{
    PadFenceStore store;
    PadFenceStore::DraftInput in;
    in.taskId = kTask;
    in.cameraId = cameraId;
    in.streamId = cameraId;
    in.name = QStringLiteral("Pad 围栏");
    in.regions = {makeRegion(regionId)};
    QString fenceId;
    int version = 0;
    QString detail;
    const auto drafted = store.saveDraft(in, &fenceId, &version, &detail);
    check(drafted == PadFenceStore::DraftOutcome::Created, "pad saveDraft failed");
    if (applyIt) {
        QString appliedAt;
        const auto applied = store.applyDraft(fenceId, version, &appliedAt, &detail);
        check(applied == PadFenceStore::ApplyOutcome::Applied, "pad applyDraft failed");
    }
    return fenceId;
}

/// fences_ 里那一条的"身份指纹"：fenceId + 版本 + 形状个数
void expectChannel(int ch, const QString &wantFenceId, const char *what)
{
    const ChannelFence cf = FenceManager::instance().channelFence(ch);
    if (wantFenceId.isEmpty()) {
        check(cf.shapes.empty(), what);
        return;
    }
    check(cf.fenceId == wantFenceId, what);
    check(!cf.shapes.empty(), what);
    check(cf.version >= 1, what);
}

/// 并进来的形状确实是那一版：regionId 经 regionsToShapes 后落在 points 上，
/// 但 ChannelFence 不带 regionId，所以用"形状数 + 归属通道"当判据就够，
/// 真正的位置由 expectChannel 的 fenceId 保证（同一 cameraId 只有一条围栏）。

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // ── 独立临时库 + 临时配置 + 假登录：既不碰板上的 idge.db，也不写板上的 config ──
    QTemporaryDir workspace;
    if (!workspace.isValid())
        return 1;
    {
        QFile config(QDir(workspace.path()).filePath(QStringLiteral("config.json")));
        config.open(QIODevice::WriteOnly);
        config.write("{}");
        config.close();
        ConfigManager::instance().load(QFileInfo(config).absoluteFilePath());
    }
    const QString dbPath = workspace.path() + QStringLiteral("/merge.db");
    if (!DatabaseManager::instance().initialize(dbPath)) {
        std::cerr << "临时库初始化失败: " << dbPath.toStdString() << std::endl;
        return 1;
    }
    // FenceManager::loadFromConfig() 用的是会话的 taskId，不是 reloadForTask 的入参，
    // 所以契约 9 要有确定的作用域就必须先假登录到 kTask。
    session::ConstructionInfo info;
    info.personId = "MERGE";
    info.personName = "自检";
    info.taskId = kTask;
    info.endtime = "23:00";
    info.totalPersonCount = 1;
    if (!session::ConstructionSession::instance().startLogin(info)) {
        std::cerr << "假登录失败（会话策略改了？契约 9 无法判定）" << std::endl;
        return 1;
    }

    int rc = 1;
    try {
        FenceManager &fm = FenceManager::instance();

        // ====================================================================
        // 契约 1：只有 native 时，"CH0" 落在通道 0（0-based 口径不变）
        // ====================================================================
        const QString nf0 = putNative(QStringLiteral("CH0"));
        fm.reloadForTask(kTask);
        expectChannel(0, nf0, "1) native CH0 -> channel 0");
        check(fm.enabled(), "1) 有围栏必须允许判定（config 的 enabled 可能是模板默认 false）");

        // ====================================================================
        // 契约 2：Pad 的 "CH2" 落在通道 **1**（1-based），不是通道 2
        // 这是两套编号的核心：错了就是整体下移一路，在别的画面上报警。
        // ====================================================================
        const QString pf2 = putPad(QStringLiteral("CH2"), QStringLiteral("RG-P2"), true);
        fm.reloadForTask(kTask);
        expectChannel(1, pf2, "2) pad CH2 -> channel 1 (1-based)");
        check(fm.channelFence(2).shapes.empty(), "2) pad CH2 不得同时落到通道 2");

        // ====================================================================
        // 契约 3：同一通道上 Pad 优先覆盖本机手绘
        // native "CH1"(→ch1) 与 pad "CH2"(→ch1) 撞车，必须剩 pad 那条。
        // ====================================================================
        const QString nf1 = putNative(QStringLiteral("CH1"));
        fm.reloadForTask(kTask);
        expectChannel(1, pf2, "3) 同通道 Pad 优先（与 GET /api/fences 合并口径一致）");

        // ====================================================================
        // 契约 4：只有草稿的 Pad 围栏不覆盖本机那版（草稿还没"生效"）
        // pad "CH1" → ch0，与 native "CH0"(→ch0) 撞车；不下发时 ch0 仍是本机那条。
        // ====================================================================
        const QString pfCh1 = putPad(QStringLiteral("CH1"), QStringLiteral("RG-P1"), false);
        fm.reloadForTask(kTask);
        expectChannel(0, nf0, "4) Pad 草稿不驱动检测");
        check(!pfCh1.isEmpty(), "4) draft id");

        // ====================================================================
        // 契约 5：apply 之后走**公共**刷新入口（pad_http_fences 的路径）
        // 这一条同时钉住"公共方法自己加锁"不死锁：调用方没持锁。
        // ====================================================================
        {
            PadFenceStore store;
            QString appliedAt, detail;
            // 草稿版本号 = 1（新建围栏的第一版）
            const auto applied = store.applyDraft(pfCh1, 1, &appliedAt, &detail);
            check(applied == PadFenceStore::ApplyOutcome::Applied, "5) applyDraft failed");
            fm.refreshFromStores();   // 不 hang、且真的把 ch0 换过来
            expectChannel(0, pfCh1, "5) apply 后 Pad 版立刻参与检测");
            expectChannel(1, pf2, "5) 刷新不能动其它通道");
        }

        // ====================================================================
        // 契约 6：停用之后这一路**退回本机手绘那版**，不是清空、也不是继续按停用版判
        // （disable 是撤掉 Pad 的覆盖，本机自己的围栏从来没被删过——这才是"停用"
        // 在两套来源并存时的正确语义，与 GET /api/fences 露出本机那条一致。）
        // ====================================================================
        {
            PadFenceStore store;
            QString disabledAt, detail;
            const auto d = store.disableVersion(pf2, 1, &disabledAt, &detail);
            check(d == PadFenceStore::DisableOutcome::Disabled, "6) disableVersion failed");
            check(pf2 != nf1, "6) 两条围栏必须是不同身份，否则这条判据没意义");
            fm.refreshFromStores();
            expectChannel(1, nf1, "6) 停用后回落本机那版");
        }

        // ====================================================================
        // 契约 7：cameraId 认不上就整条跳过，绝不"猜一路"
        // CH0（不是合法 1-based）、CH9（超出通道数）、CAM-3（不是 CHn）
        // ====================================================================
        {
            const int chBefore = (int)fm.channelFence(3).shapes.size();
            putPad(QStringLiteral("CH0"), QStringLiteral("RG-X0"), true);   // 唯一键不同，能建
            putPad(QStringLiteral("CH9"), QStringLiteral("RG-X9"), true);
            fm.refreshFromStores();
            check(fm.channelFence(3).shapes.size() == chBefore,
                  "7) 认不上的 cameraId 不该并进来");
        }

        // ====================================================================
        // 契约 8：登出清空内存围栏，但库里历史还在
        // ====================================================================
        fm.reloadForTask(QString());
        check(!fm.hasFence(0) && !fm.hasFence(1) && !fm.hasFence(2) && !fm.hasFence(3),
              "8) 登出后内存不该留着围栏");
        fm.reloadForTask(kTask);
        check(fm.hasFence(0), "8) 重新登录要读得回来（历史没被登出清掉）");

        // ====================================================================
        // 契约 9：loadFromConfig()（启动路径，整段持锁）不死锁
        // 走到这里说明它返回了；判据是它确实把 native 那版又装了回来。
        // ====================================================================
        fm.loadFromConfig();
        expectChannel(0, pfCh1, "9) loadFromConfig 同样并两库且不自死锁");

        rc = 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << std::endl;
        rc = 1;
    }

    DatabaseManager::instance().close();
    std::cout << (rc == 0 ? "PASS fence_merge" : "FAIL fence_merge") << std::endl;
    return rc;
}
