// ============================================================================
// database_manager.cpp - 数据库管理器实现
// ============================================================================
//
// 作用：
//   管理 SQLite 数据库连接、初始化、表创建和维护。
//   是整个数据库层的核心组件，提供线程安全的数据库访问。
//
// 架构：
//   ┌─────────────────────────────────────────────────────────────┐
//   │                    DatabaseManager (单例)                   │
//   │   - initialize(): 打开SQLite连接 + 启用WAL模式 + 建表        │
//   │   - database(): 返回"当前线程"的 QSqlDatabase 供 DAO 使用    │
//   │   - vacuum/backup/clean: 数据库维护功能                      │
//   └─────────────────────────────────────────────────────────────┘
//         │                    │                    │
//         ▼                    ▼                    ▼
//   ┌───────────┐      ┌──────────────┐      ┌──────────────┐
//   │ AlarmDAO  │      │ DetectionDAO │      │   其他DAO    │
//   │ 报警表CRUD │      │  检测表CRUD   │      │  扩展用      │
//   └───────────┘      └──────────────┘      └──────────────┘
//
// ============================================================================

#include "database_manager.h"
#include "construction_session.h"   // 施工计划身份的唯一取值判据（task_id 回填复用，别在这儿抄键名）
#include <QSqlQuery>
#include <QSqlError>
#include <QSet>
#include <QDateTime>
#include <QElapsedTimer>   // 回填耗时打日志用：这段跑在 GUI 线程的启动路径上
#include <QFileInfo>
#include <QFile>
#include <QDebug>

// 获取单例实例
DatabaseManager &DatabaseManager::instance()
{
    static DatabaseManager mgr;
    return mgr;
}

// 构造函数
DatabaseManager::DatabaseManager(QObject *parent)
    : QObject(parent)
{
}

// 析构函数
DatabaseManager::~DatabaseManager()
{
    close();
}

// ============================================================================
// initialize: 初始化数据库
// ============================================================================
// 流程：
//   1. 规范化路径、确保数据目录存在
//   2. 取得（或按新路径重建）调用线程的连接并打开 SQLite
//   3. 启用 WAL 模式（文件级持久设置，提高并发性能）
//   4. 创建表结构 + 一次性单位迁移
//
// 注意：其他线程的连接在各自首次调用 database() 时惰性创建，
//       每个连接自带 foreign_keys/busy_timeout 等连接级 PRAGMA。
// ============================================================================
bool DatabaseManager::initialize(const QString &dbPath)
{
    QMutexLocker lock(&mutex_);

    // 设置数据库路径
    dbPath_ = dbPath;

    // 如果是相对路径，使用当前目录
    if (!QDir::isAbsolutePath(dbPath_)) {
        dbPath_ = QDir::currentPath() + "/" + dbPath_;
    }

    // 确保目录存在
    QFileInfo fileInfo(dbPath_);
    QDir dir = fileInfo.absoluteDir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    // 打开当前线程的连接（路径变化时自动重建；连接级 PRAGMA 在其中设置）
    QSqlDatabase db = currentConnection();
    if (!db.isOpen()) {
        QString error = db.lastError().text();
        qCritical() << "Failed to open database:" << error;
        emit errorOccurred(error);
        return false;
    }

    qInfo() << "Database opened:" << dbPath_;

    // 启用 WAL 模式（Write-Ahead Logging）：文件级持久设置，只需设置一次
    QSqlQuery query(db);
    if (!query.exec("PRAGMA journal_mode=WAL;")) {
        qWarning() << "Failed to set WAL mode:" << query.lastError().text();
    }
    // 必须 finish()：这条 PRAGMA 会**返回一行结果**，语句因此保持活动。带着活动语句进到
    // createTables() 里，后面任何 commit() 都会失败（实测报
    // "cannot commit transaction - SQL statements in progress"），而失败后的 rollback
    // 会把同连接上刚执行的 ALTER TABLE 一起吞掉——老库升级当场废掉且只在日志里留两行。
    query.finish();

    // 创建表结构
    if (!createTables()) {
        qCritical() << "Failed to create tables";
        return false;
    }

    // 补列已挪进 createTables()：它必须发生在按新列建索引之前。

    // 一次性单位迁移：旧版本把 detections.timestamp 写成了 epoch 纳秒，
    // 而查询/清理按毫秒约定（纳秒值恒大于毫秒 cutoff，导致永不清删）。
    // 现写入口已统一换算为毫秒；这里把存量纳秒行原地换算（epoch 纳秒
    // 约 1.7e18、毫秒约 1.7e12，以 1e15 为不可能混淆的分界）。条件在
    // 迁移完成后自然失效，重复执行为无害空操作。
    {
        QSqlQuery migrate(db);
        if (!migrate.exec("UPDATE detections SET timestamp = timestamp / 1000000 "
                          "WHERE timestamp >= 1000000000000000")) {
            qWarning() << "Detections timestamp unit migration failed:"
                       << migrate.lastError().text();
        } else if (migrate.numRowsAffected() > 0) {
            qInfo() << "Migrated" << migrate.numRowsAffected()
                    << "legacy ns detection records to ms";
        }
    }

    initialized_ = true;
    emit databaseOpened();
    return true;
}

// ============================================================================
// close: 关闭数据库
// ============================================================================
// Qt 规定连接只能由创建线程访问，故此处只回收调用线程（通常为主线程）的
// 连接；其他线程的连接在各自线程退出时由 QThreadStorage 自动释放。
// ============================================================================
void DatabaseManager::close()
{
    QMutexLocker lock(&mutex_);
    if (!initialized_) {
        return;
    }
    initialized_ = false;
    if (threadConn_.hasLocalData()) {
        threadConn_.localData().clear();
    }
    qInfo() << "Database closed";
    emit databaseClosed();
}

// ============================================================================
// isOpen: 数据库是否已初始化（连接按线程划分，可用性以 database() 结果为准）
// ============================================================================
bool DatabaseManager::isOpen() const
{
    QMutexLocker lock(&mutex_);
    return initialized_;
}

// ============================================================================
// currentConnection: 取得（必要时惰性创建）当前线程的数据库连接
// ============================================================================
// 前置条件：持有 mutex_（保护 connSeq_ / threadConn_ 槽位 / dbPath_ 快照）。
// 每个线程一个命名连接："idge_conn_<seq>"，序号保证注册名全局唯一，
// 线程 ID 被系统复用也不会拿到别人的连接。
// 连接级 PRAGMA（foreign_keys、busy_timeout）不随连接共享，逐连接设置；
// journal_mode=WAL 是文件级持久设置，由 initialize() 负责。
// ============================================================================
QSqlDatabase DatabaseManager::currentConnection() const
{
    if (threadConn_.hasLocalData()) {
        QSharedPointer<ThreadConnection> &slot = threadConn_.localData();
        if (slot && slot->path == dbPath_ && slot->db.isOpen()) {
            return slot->db;
        }
        if (slot) {
            slot.clear();   // 在本线程析构旧连接（close + removeDatabase）
        }
    }

    const QString name = QStringLiteral("idge_conn_%1").arg(++connSeq_);
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
    db.setDatabaseName(dbPath_);

    if (!db.open()) {
        qCritical() << "Failed to open thread database connection:" << db.lastError().text();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
        return QSqlDatabase();
    }

    QSqlQuery pragma(db);
    if (!pragma.exec("PRAGMA foreign_keys=ON;")) {
        qWarning() << "Failed to enable foreign keys:" << pragma.lastError().text();
    }
    if (!pragma.exec("PRAGMA busy_timeout=5000;")) {
        qWarning() << "Failed to set busy timeout:" << pragma.lastError().text();
    }

    QSharedPointer<ThreadConnection> tc(new ThreadConnection());
    tc->name = name;
    tc->path = dbPath_;
    tc->db = db;
    if (threadConn_.hasLocalData()) {
        threadConn_.localData() = tc;
    } else {
        threadConn_.setLocalData(tc);
    }
    return db;
}

// ============================================================================
// database: 获取当前线程的数据库连接
// ============================================================================
QSqlDatabase DatabaseManager::database() const
{
    QMutexLocker lock(&mutex_);
    if (!initialized_) {
        return QSqlDatabase();
    }
    return currentConnection();
}

// ============================================================================
// createTables: 创建所有表（程序启动时自动调用）
// ============================================================================
// 使用 CREATE TABLE IF NOT EXISTS，重复运行不会报错。
//
// 表结构：
//   1. detections: AI检测结果表
//      - 记录每帧的检测结果（类别、置信度、位置框）
//      - 用于统计分析，不直接展示给用户
//
//   2. alarms: 报警信息表
//      - 记录触发报警的检测结果（含处置状态）
//      - 用于报警列表展示、统计、处置跟踪
//
// 索引设计：
//   - 按时间查询：idx_alarms_time, idx_detections_timestamp
//   - 按通道查询：idx_alarms_channel, idx_detections_channel
//   - 按状态筛选：idx_alarms_status（快速查未处理报警）
//   - 按类别统计：idx_alarms_type, idx_detections_class
//
// ============================================================================
bool DatabaseManager::createTables()
{
    // 1. 创建检测信息表
    //    ⚠ 只有**触发了报警**的那一条检测会落这张表（AlarmManager::storeDetectionDetail），
    //      总开关是 config.json 的 database.storeDetections。它不是全量帧明细，别当抽样表用。
    QString createDetections = R"(
        CREATE TABLE IF NOT EXISTS detections (
            id              INTEGER PRIMARY KEY AUTOINCREMENT, -- 自增主键，无业务含义
            timestamp       INTEGER NOT NULL,                  -- 帧时间戳，**毫秒**（推理结果 time 的 epoch 纳秒 /1e6）。与 alarms.alarm_time 的文本口径不同，跨表比时间要先换算，别直接字符串比较
            channel         INTEGER NOT NULL,                  -- 视频通道编号，0-based（0~3）。对外出数一律 +1，与 alarms.channel 同一口径
            frame_id        INTEGER,                           -- ⚠ 存的是**级联槽位下标**，不是帧号：storeDetectionDetail 把 slot 塞进 singleResult.id 透传到这里。作用是"这个框是本通道第几个模型检出的"
            class_id        INTEGER NOT NULL,                  -- 类别在**该槽位模型** labels.txt 里的行号。只在同槽位内有意义：不同模型的同一 id 是不同类别
            class_name      TEXT NOT NULL,                     -- labels.txt 的行内容（英文，如 person / un_helmet / cigarette），与 alarms.class_name 同源
            confidence      REAL NOT NULL,                     -- 置信度 0.0~1.0（取自推理结果 prop，不是百分数）
            bbox_left       INTEGER NOT NULL,                  -- 检测框左边界，**原始帧像素**坐标（不是归一化、也不是模型输入 640 空间）
            bbox_top        INTEGER NOT NULL,                  -- 检测框上边界，原始帧像素
            bbox_right      INTEGER NOT NULL,                  -- 检测框右边界，原始帧像素
            bbox_bottom     INTEGER NOT NULL,                  -- 检测框下边界，原始帧像素（= 人的"脚点"所在行，围栏判定用的就是它）
            created_at      DATETIME DEFAULT CURRENT_TIMESTAMP -- ⚠ SQLite 默认值是 **UTC**，且格式是 "yyyy-MM-dd HH:mm:ss"（空格分隔）：与本表的 timestamp、alarms.alarm_time 都差 8 小时且分隔符不同，别拿它做时间过滤，只是插入痕迹
        );
    )";

    if (!executeSQL(createDetections)) {
        return false;
    }

    // 创建检测表索引
    executeSQL("CREATE INDEX IF NOT EXISTS idx_detections_timestamp ON detections(timestamp);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_detections_channel ON detections(channel);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_detections_class ON detections(class_name);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_detections_channel_time ON detections(channel, timestamp);");

    // 2. 创建报警信息表
    //    列的读写口径全在这里注释；两份建表（本文件与 sql/idge_init.sql）必须同步改。
    //    时间列一律是**定宽本地时间文本** "yyyy-MM-ddTHH:mm:ss"（无时区偏移），
    //    因为所有时间过滤都靠字典序 BETWEEN（判据与实测见 alarm_dao.cpp 的
    //    buildFilterWhere()，:440 那段注释）；
    //    对外要带偏移的 ISO-8601 在出门口补（src/biz/pad/pad_wire.cpp），不动库。
    QString createAlarms = R"(
        CREATE TABLE IF NOT EXISTS alarms (
            id              TEXT PRIMARY KEY,                  -- 32 位十六进制 UUID（去花括号去横线，makeAlarmRecord 生成）。报警的对外身份：v1 的 alarmId、抓拍文件名对齐、UPDATE 的 WHERE 都靠它，绝不复用
            alarm_type      TEXT NOT NULL,                     -- 现在只产出三种值：detection / fence_in（闯入禁区）/ fence_out（离岗）。⚠ 历史值 `fence`（不带后缀）才是多数：实测两份库 `data/idge.db` 是 fence 933 / detection 2499、**fence_in/fence_out 零行**，`build/build_rk3588_linux/data/idge.db`（run.sh 实际加载那份）是 fence 25862 / detection 10682 / fence_in 1739 / fence_out 11。下游按 "fence" **前缀**认三代（`alarm_dao.cpp:480`、`:1243`、`alarm_list_widget.cpp:778`），精确匹配 fence_in/fence_out 的新代码都会漏掉老行——`pad_wire.cpp:148` 已经踩了（审查报告 §八 P2-4）
            alarm_level     INTEGER NOT NULL DEFAULT 3,        -- 报警级别：1 紧急 / 2 重要 / 3 一般 / 4 提示。当前硬编码只有两档产出：普通检测=3、围栏=2；1/4 级没有产出路径（级别映射表未做）。v1 按 int 传，level 1/2 触发 Pad 全屏强提醒
            alarm_time      TEXT NOT NULL,                     -- 报警**发生**时间，取自推理结果的帧时刻（异常时回落当前墙钟）。定宽无偏移，见上面那段口径
            channel         INTEGER NOT NULL,                  -- 视频通道编号，0-based（0~3）；对外出数是 1-based，UI 列名"通道N"也是 +1
            class_id        INTEGER NOT NULL,                  -- 触发报警的类别在**该槽位模型** labels.txt 里的行号；跨模型不可比，去重限流用的是它不是 class_name
            class_name      TEXT NOT NULL,                     -- labels.txt 的行内容（英文，如 un_helmet / person / cigarette）。三处靠它：报警判定（槽位的 alarm_class 比对）、抓拍/片段文件名、Pad 出站类型表（src/biz/pad/pad_wire.cpp 的 kTable）
            confidence      REAL NOT NULL,                     -- 置信度 0.0~1.0（推理结果 prop）
            image_path      TEXT,                              -- 抓拍图**相对路径** data/alarms/yyyyMMdd/ch{N+1}_{类别}_{HHmmss_zzz}.jpg。入库前就填好（解码线程 deliverAlarms 先写 imagePath 再 storeAndNotify），所以报警一落地就有图可对外。⚠ 老行可能是迁移前的 "alarms/..." 前缀，读取时过 RuntimePaths::resolveStoredPath 重写
            video_path      TEXT,                              -- insert 时**必为空**：片段要"报警后约 2 秒"才由 ClipWriter 写出来，事后 updateVideoPath 回填。存的是 Annex-B 裸流（.h265/.h264）；网页/Pad 要播的同名 .mp4 副本不入库（入库会让本地详情框找不到那段视频）
            status          TEXT NOT NULL DEFAULT 'pending',   -- pending / rectified / disposed / false_alarm。⚠ `rectified` 是本机"已确认"的语义 = **我看过了**，不是"已处置"；`disposed` 只由 AlarmDAO::dispose() 写，是 v1 §3.10 里 DISPOSED 的唯一来源。出站映射：disposed→DISPOSED、false_alarm→FALSE_ALARM、其余（含 rectified）→PENDING
            dispose_result  TEXT,                              -- 处置措施文本（v1 的 dispose.measure）。写入方：`POST /api/alarms/{id}/dispose`（Pad）→ AlarmManager::disposeAlarmById → AlarmDAO::dispose；本机详情框的处置表单走同一条路
            dispose_user_id INTEGER,                           -- 处置人 ID。⚠ 类型已与 v1 冲突：对端的 personId 是字符串工号（"05-0101"/"P-001"），所以原文落在下面的 dispose_person_id，这一列只在工号整体是整数时才顺带写
            dispose_user_name TEXT,                            -- 处置人/误报人姓名（写入方同 dispose_result）
            dispose_person_id TEXT,                            -- 处置人/误报人的**工号原文**（v1 的 dispose.personId，形如 "05-0101"/"P-001"）。为什么另开一列而不是塞进 dispose_user_id：那是 INTEGER，装不下带横线/字母的工号，硬转数字就会静默变成 0（口径确认单 N5 的落地选择：保原文、旧列照旧写数字档）。老库由 ensureColumns 补这一列
            dispose_time    TEXT,                              -- 处置时间（同 alarm_time 口径；v1 的 disposedAt，终态时必填）
            dispose_photo   TEXT,                              -- 处置整改照片路径的 **JSON 数组文本**（0~5 个相对路径，v1 的 dispose.photoUrls）。⚠ 老行可能是**单张裸路径**（这一列原本的口径就是单张），所以读侧两种形状都要认：以 '[' 开头按数组解，否则当单元素（pad_wire.cpp 的 photoUrls 就是这么出的）。写入方是 POST /api/alarms/{id}/dispose
            remark          TEXT,                              -- 补充说明（v1 的 dispose.remark）；AlarmDAO::updateRemark() 有实现，写入方待处置表单
            read_time       TEXT,                              -- **最早**已读时间。markRead() 现在保证"保留最早 + 必推 update_time"（含墙钟回拨），但调用方还没有：v1 的 POST /alarms/{id}/read 未实现
            create_time     TEXT NOT NULL,                     -- 入库时间。建行那一刻由 makeAlarmRecord 写死（现等于 alarm_time）；早期版本留空、靠 DAO 兜底当前时间，所以老行可能是 NULL
            update_time     TEXT,                              -- 最后变更时间：状态/误报/已读/备注/图片/片段每条 UPDATE 都推它（`alarm_dao.cpp:785/811/840/869/900/923/944/965`）。⚠ **v1 的断线补漏游标就是它**（`AlarmDAO::queryUpdatedSince` 按 `>= 游标` 过滤），所以建行即写不能留空——空串会让刚产生的报警被增量整批漏掉。实测两份库的空串行数差两个量级：`data/idge.db` 1992/3432 为空，`build/build_rk3588_linux/data/idge.db` 只有 136/38294 为空（片段回填顺带 bump 了绝大多数行）→ `COALESCE(NULLIF(update_time,''),alarm_time)` 这条兜底在哪份库上真正生效，取决于跑的是哪一个
            worker_name     TEXT,                              -- 施工负责人姓名，扫码登录会话定死后在**产生这一刻**盖上（不等到显示/上行再回查）。报警列表那一列与"按负责人"筛选用它——为这个才单独成列，省掉每行解析 JSON
            construction_info TEXT,                            -- 整份施工信息 JSON（compact 原样；2026-11-04 起是 qr-login 的 person/plan 嵌套形状，历史行仍是扁平 construction_id/worker_name 形状，两代都读得回）。详情框逐键渲染与上行 data["construction"] 都从它出，**加字段不用改表**
            task_id           TEXT                             -- 施工计划 ID ＝ v1 的 taskId（B2 于 2026-11-04 定案：就是扫码载荷里的施工计划 ID（qr-login 起是 plan.taskId，历史行是 construction_id），对外统一叫 taskId）。写库时从会话快照直接落这一列；Pad 的 sync 过滤、列表「按施工计划」筛选、facets 候选值全部走这条**等值**判据。以前是 `instr(construction_info,'"construction_id":"X"')` 的子串匹配，依赖 QJsonDocument::Compact 的序列化形状（键序变了、多了空格、将来换成 pretty 就静默查不到），所以把它升成一等列。老行由启动时一次性回填（backfillAlarmTaskIds）。⚠ **这一列的不变式是「永不为 NULL」**：没有计划身份的行落空串 `''`。写入侧靠 INSERT 里的 `IFNULL(:task_id, '')` 兜住——Qt 把空 QString 当无值，直接绑就落 NULL，而关机时 RAM 窗口整批回写会把已回填好的行打回 NULL，于是回填每开机重跑一次（实测每次 1000 行）
            rule            TEXT,                              -- 触发规则中文名（v1 §13.2.1 的 `rule`）。取值只认 src/biz/alarm/alarm_labels.cpp 那一张表，与 Pad 出门口同源；历史行为空、不回填
            model_version   TEXT,                              -- 产出这条报警的模型版本，形如 `yolo11n-ppe@2026-10-21`（库 id + meta.json 的 version，与 §7.1 的 model_version 同口径）。检测报警取该槽位模型，围栏报警取检出该目标的那个槽位
            fence_id        TEXT,                              -- 围栏报警：当时生效的围栏主行 id（本机手绘 NFENCE-*，Pad 下发 FENCE-*）；检测报警为空
            fence_version   INTEGER,                           -- 围栏报警：当时生效的围栏版本号（每次编辑 +1）。v1 的 `fenceVersion` 是字符串 "V2"，那层 "V" 前缀在出门口补，库里只存数字
            region_id       TEXT,                              -- 围栏报警：命中区域的 regionId（RG-<8hex>，形状级、跨版本稳定）。⚠ 离岗（fence_out）没有单一责任形状，这一列**恒空**，不是漏写
            dispose_option_id INTEGER                         -- 处置选项 id（1..4，见 pad_http_alarms.cpp 的选项表）。与 dispose_result 的 label 同源：措辞以后还会改，统计与对账要认 id 而不是认中文
        );
    )";

    if (!executeSQL(createAlarms)) {
        return false;
    }

    // 补列必须夹在建表与建索引之间：CREATE TABLE IF NOT EXISTS 对已部署的老库是
    // 空操作，新加的施工信息两列只有靠这里 ALTER 才存在；而下面两条按新列建的索引
    // 早于 ALTER 就会失败（老库上实测 "no such column: worker_name"）。
    // ⚠ 但"失败只告警"这个尺度 2026-11-06 之后已经不成立了：AlarmDAO 的两条 INSERT
    //   （insertAlarm / insertAlarms）**硬编码**了下面这些列名，少一列的后果不是"这条
    //   报警少带施工信息"，而是**每一条报警都插不进去**——RAM 窗口里看着一切正常，
    //   关机就整批没了。所以这里补完必须回读一次，缺列就按初始化失败处理（见下）。
    const QStringList alarmExtraColumnDefs = {
        QStringLiteral("worker_name TEXT"),
        QStringLiteral("construction_info TEXT"),
        QStringLiteral("task_id TEXT"),
        QStringLiteral("dispose_person_id TEXT"),
        QStringLiteral("rule TEXT"),
        QStringLiteral("model_version TEXT"),
        QStringLiteral("fence_id TEXT"),
        QStringLiteral("fence_version INTEGER"),
        QStringLiteral("region_id TEXT"),
        QStringLiteral("dispose_option_id INTEGER")};
    if (!ensureColumns(QStringLiteral("alarms"), alarmExtraColumnDefs)) {
        qWarning() << "Alarms construction columns not fully ensured";
    }

    QStringList alarmExtraColumnNames;
    for (const QString &def : alarmExtraColumnDefs)
        alarmExtraColumnNames << def.section(QLatin1Char(' '), 0, 0);
    const QStringList alarmsAbsent = missingColumns(QStringLiteral("alarms"), alarmExtraColumnNames);
    if (!alarmsAbsent.isEmpty()) {
        qCritical().noquote()
            << "alarms 表缺列：" << alarmsAbsent.join(", ")
            << "。AlarmDAO 的 INSERT 硬编码了这些列名，继续跑等于**每一条报警都写不进库**，"
               "所以数据库初始化按失败处理——请查上面几行 Add column failed 的原因"
               "（磁盘满/库被别的进程占着/库文件坏了都会走这条）。";
        return false;
    }
    // 老行的 task_id 回填必须在建 idx_alarms_task_id 之前跑完：几万行逐条 UPDATE 之后
    // 再建索引，比"先建索引再逐条 UPDATE（每条都要维护索引）"省一整个来回。
    backfillAlarmTaskIds();

    // 创建报警表索引
    executeSQL("CREATE INDEX IF NOT EXISTS idx_alarms_type ON alarms(alarm_type);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_alarms_level ON alarms(alarm_level);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_alarms_status ON alarms(status);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_alarms_time ON alarms(alarm_time);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_alarms_channel ON alarms(channel);");
    // 下面两条是检索改造带来的：单列索引只能挑一个用，之后 ORDER BY alarm_time DESC
    // 要再建临时 B 树排序。实测 35383 行的库上「通道 + 时间档」翻页 34ms、
    // 「按负责人」28ms，都在 GUI 线程上；复合索引让这两条退化成几万分之一秒的量级。
    executeSQL("CREATE INDEX IF NOT EXISTS idx_alarms_channel_time ON alarms(channel, alarm_time);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_alarms_worker ON alarms(worker_name);");
    // v1 的 sync 补漏每次都按 taskId 过滤（Pad 每重连一次就跑一遍），没索引就是几万行全表扫。
    executeSQL("CREATE INDEX IF NOT EXISTS idx_alarms_task_id ON alarms(task_id);");

    // 3. Pad 接口 v1 的 HTTP 面三张表（§2.5 幂等表 + §5 围栏两张）
    //    与 alarms/detections 不同，这三张**只被 biz/pad 与 biz/geofence 读写**，
    //    界面一行都不读它们；把它们的口径写死在这里，是为了让"库里有几张表"仍能一处回答。
    //    时间列同一套定宽无偏移文本（与 alarms 一致，见上面那段口径）。
    QString createPadRequestCache = R"(
        CREATE TABLE IF NOT EXISTS pad_request_cache (
            request_id    TEXT PRIMARY KEY,                    -- §2.5 的幂等键：Pad 每次写操作生成的 UUID。**只按它命中，不比较请求内容**（规约明写"不比较请求内容，不重复创建/修改"）
            route         TEXT NOT NULL,                       -- 首次命中的 "POST /api/..."，只为排障；命中判据里没有它
            http_status   INTEGER NOT NULL,                    -- 首次回复的 HTTP 状态码（含 4xx/5xx —— 规约说"返回第一次的结果"，错误也是结果）
            content_type  TEXT,                                -- 首次回复的 Content-Type，空 = 用 application/json
            body          TEXT NOT NULL,                       -- §2.4 信封的**原样 JSON 文本**，重放时一字不改地写回去
            created_at    TEXT NOT NULL                        -- 建行时刻，定宽无偏移；清理按它做字典序比较（>= 24 小时前的删）
        );
    )";
    if (!executeSQL(createPadRequestCache)) {
        return false;
    }

    QString createPadFences = R"(
        CREATE TABLE IF NOT EXISTS pad_fences (
            fence_id    TEXT PRIMARY KEY,                      -- §5.2 由本机分配且**永不复用**（FENCE-<自增号>）；唯一键是 (task_id, camera_id)
            task_id     TEXT NOT NULL,                         -- 施工计划 ID（与 alarms.task_id 同物）
            camera_id   TEXT NOT NULL,                         -- Pad 侧的摄像头 ID（本机口径见 src/biz/pad/README.md 的 §6 那节）
            stream_id   TEXT NOT NULL,                         -- 视频流 ID
            created_at  TEXT NOT NULL,                         -- 围栏建行时刻
            updated_at  TEXT NOT NULL                          -- 最后一次任何版本变更的时刻
        );
    )";
    if (!executeSQL(createPadFences)) {
        return false;
    }
    // 同一 taskId + cameraId 一个围栏（§5.1）——靠唯一索引兜，不靠代码里"先查再插"：
    // 两处并发写时后者会直接被 SQLite 挡下，而不是造出两条同键围栏。
    executeSQL("CREATE UNIQUE INDEX IF NOT EXISTS idx_pad_fences_task_camera ON pad_fences(task_id, camera_id);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_pad_fences_task ON pad_fences(task_id);");

    // 版本行。**不存 active_version/draft_version**：那两列与 status 是同一件事的两个影子，
    // 存了就会出现"改 status 忘推影子"的不一致；出数时按 status 现查（§13.4.1 那两个字段）。
    QString createPadFenceVersions = R"(
        CREATE TABLE IF NOT EXISTS pad_fence_versions (
            fence_id      TEXT NOT NULL,                       -- → pad_fences.fence_id。⚠ 本库连接没开 PRAGMA foreign_keys（见 business_db_manager.h 同款说明），级联由删除路径手工兜
            version       INTEGER NOT NULL,                    -- 从 1 起；编辑草稿不变号，复制/新建才 max+1（§5.2）
            status        TEXT NOT NULL,                       -- ACTIVE / DRAFT / DISABLED；同一围栏最多一条 ACTIVE + 一条 DRAFT，由写路径判
            name          TEXT NOT NULL,                       -- 围栏名称（Pad 保存时默认取摄像头名）
            regions_json  TEXT NOT NULL,                       -- §5.2 的 regions 数组**原样 JSON 文本**：regionId 由 Pad 生成、本机只透传，所以存原文而不是拆开重建
            plan_json     TEXT NOT NULL,                       -- 保存那一刻的施工计划快照（§13.4.4），不随时间变
            owner_json    TEXT NOT NULL,                       -- 保存那一刻的负责人快照（§13.4.5），owner.id 必填
            frame_path    TEXT,                                -- 合成生效画面 PNG 的相对路径 data/alarms/fence-frames/<fenceId>/v<n>.png；**只有 ACTIVE/DISABLED 可能有值，DRAFT 恒为 NULL**（出 URL 前还要求文件真在）
            applied_at    TEXT,                                -- 下发生效时刻；草稿为 NULL
            created_at    TEXT NOT NULL,                       -- 版本建行时刻
            updated_at    TEXT NOT NULL,                       -- 版本最后变更时刻（编辑草稿就推它，§13.4.2）
            PRIMARY KEY (fence_id, version)
        );
    )";
    if (!executeSQL(createPadFenceVersions)) {
        return false;
    }
    // GET /fences?taskId= 先按 task 捞围栏再逐条取版本；这条按围栏取 ACTIVE/DRAFT 是热路径
    executeSQL("CREATE INDEX IF NOT EXISTS idx_pad_fence_versions_fence ON pad_fence_versions(fence_id, status);");

    // ── 本机围栏（驱动检测的那套）：按 taskId + cameraId(=CHn) 组织，多版本留历史 ──
    // 与 pad_fences 同构但更简单：没有 DRAFT，每次编辑建一条新 ACTIVE、旧 ACTIVE→DISABLED，
    // regionId 由本机分配并稳定跟踪到形状级。详见 src/biz/geofence/native_fence_store.h。
    QString createNativeFences = R"(
        CREATE TABLE IF NOT EXISTS native_fences (
            fence_id    TEXT PRIMARY KEY,                      -- NFENCE-<自增号>，永不复用
            task_id     TEXT NOT NULL,                         -- 登录态=当前施工 taskId；未登录时空串（但未登录不允许画，故实际只有登录态数据）
            camera_id   TEXT NOT NULL,                         -- CH0 / CH1 / CH2 / CH3（=通道号）
            stream_id   TEXT NOT NULL DEFAULT '',              -- 通道对应流 id（可空）
            created_at  TEXT NOT NULL,
            updated_at  TEXT NOT NULL
        );
    )";
    if (!executeSQL(createNativeFences)) {
        return false;
    }
    executeSQL("CREATE UNIQUE INDEX IF NOT EXISTS idx_native_fences_task_camera ON native_fences(task_id, camera_id);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_native_fences_task ON native_fences(task_id);");

    QString createNativeFenceVersions = R"(
        CREATE TABLE IF NOT EXISTS native_fence_versions (
            fence_id      TEXT NOT NULL,                       -- → native_fences.fence_id
            version       INTEGER NOT NULL,                    -- 从 1 起，每次编辑 max+1
            status        TEXT NOT NULL,                       -- ACTIVE / DISABLED（本机无 DRAFT，画完即生效）
            name          TEXT NOT NULL,                       -- "通道N围栏"
            regions_json  TEXT NOT NULL,                       -- §5 regions 数组 JSON（regionId 本机生成、形状级稳定）
            frame_path    TEXT,
            applied_at    TEXT,
            created_at    TEXT NOT NULL,
            updated_at    TEXT NOT NULL,
            PRIMARY KEY (fence_id, version)
        );
    )";
    if (!executeSQL(createNativeFenceVersions)) {
        return false;
    }
    executeSQL("CREATE INDEX IF NOT EXISTS idx_native_fence_versions_fence ON native_fence_versions(fence_id, status);");

    qInfo() << "Database tables created successfully";
    return true;
}

// ============================================================================
// ensureColumns: 给已存在的表补列（本仓唯一的迁移手段）
// ============================================================================
// 为什么必须存在：两份 CREATE TABLE 都是 IF NOT EXISTS，已部署的库不会自动长列，
// 只改建表语句 ⇒ 老库上 INSERT 直接 "no such column" 静默失败。
// 判断用 PRAGMA table_info（内建 SQLite 3.49.1 支持），列名取第 2 个字段。
// 只加可空、无默认、无外键的列，所以 ALTER 在已有数据上永远合法。
// 调用方须持有 mutex_（initialize() 内部就是这样）。
// ============================================================================
bool DatabaseManager::ensureColumns(const QString &table, const QStringList &columnDefs)
{
    QSqlDatabase db = currentConnection();
    if (!db.isOpen()) {
        return false;
    }

    QSet<QString> existing;
    {
        QSqlQuery pragma(db);
        // table 是代码里的字面量（alarms/detections），不来自用户输入，无注入面
        if (!pragma.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
            qWarning() << "PRAGMA table_info failed for" << table << ":"
                       << pragma.lastError().text();
            return false;
        }
        while (pragma.next()) {
            existing.insert(pragma.value(1).toString().toLower());
        }
    }

    bool ok = true;
    for (const QString &def : columnDefs) {
        const QString name = def.section(QLatin1Char(' '), 0, 0).toLower();
        if (name.isEmpty() || existing.contains(name)) {
            continue;   // 已有该列：幂等，第二次启动就是走这条
        }
        QSqlQuery alter(db);
        const QString sql = QStringLiteral("ALTER TABLE %1 ADD COLUMN %2").arg(table, def);
        if (!alter.exec(sql)) {
            qWarning() << "Add column failed:" << sql << ":" << alter.lastError().text();
            ok = false;
            continue;
        }
        qInfo() << "Migrated:" << sql;
    }
    return ok;
}

// ============================================================================
// missingColumns: 表里**没有**的那几列（ensureColumns 的事后回读）
// ============================================================================
// 为什么单独有这一个函数：ensureColumns 的返回值只说"有没有全加上"，而 createTables
// 要知道的是"最终缺哪些列"——ALTER 失败的原因可能是磁盘满、库被别的进程写占着、库文件
// 坏了，那些都不会自己好转，而硬编码了列名的 INSERT 会**一直**失败。
// 判据朝报错那一侧倒：PRAGMA 读不到（或库没开）时按"全都缺"返回。宁可在启动日志里
// 多一句"初始化失败"，也不要放行一个"每条报警都静默写不进去"的运行期。
// 调用方须持有 mutex_（与 ensureColumns 同口径），table/columnNames 只传代码里的字面量。
// ============================================================================
QStringList DatabaseManager::missingColumns(const QString &table, const QStringList &columnNames)
{
    QStringList absent = columnNames;   // 默认全缺：读不到就一个都不算存在
    QSqlDatabase db = currentConnection();
    if (!db.isOpen()) {
        return absent;
    }

    QSqlQuery pragma(db);
    if (!pragma.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        qWarning() << "PRAGMA table_info failed for" << table << ":"
                   << pragma.lastError().text();
        return absent;
    }
    QSet<QString> existing;
    while (pragma.next())
        existing.insert(pragma.value(1).toString().toLower());

    absent.clear();
    for (const QString &name : columnNames) {
        const QString key = name.trimmed().toLower();
        if (!key.isEmpty() && !existing.contains(key))
            absent << key;
    }
    return absent;
}

// ============================================================================
// backfillAlarmTaskIds: 老行的 task_id 从 construction_info 里补出来
// ============================================================================
// 为什么必须存在：task_id 是 2026-11-04 随 B2 定案新加的列，而 Pad 的 sync 过滤与列表的
// 「按施工计划」筛选都已改成按它**等值**判据。ensureColumns 只能让列存在，值不会自己长出来；
// 老库几万行若不回填，历史报警会一次性从同步结果里消失且零报错——症状与报告 §八 P1-2 同形，
// 只是方向相反。
// 只能在 C++ 侧逐行解：本机 QSQLITE 插件内嵌 SQLite 3.31.1 且没编 JSON1，json_extract 会
// "no such function"。取值判据复用 ConstructionInfo::taskIdFromJsonText（键名只有一处定义）。
// 两步走：① 只对确实带施工信息的 NULL 行逐行解并写值；② 收尾一条 UPDATE 把剩下的 NULL 写成
// 空串。第②步是幂等的关键：QString() 在 Qt 里是 **null** 串，绑进 SQL 就是 NULL，少了这一步
// 未登录的老行会永远留在 `task_id IS NULL` 里，每次开机被重扫重写（实测 3432 行白跑一遍，
// 现场几万行同理）。两步都在"没有任何行要动"时不发语句/不开事务。
// ⚠ 光靠本函数**保不住**幂等：关机时 AlarmManager::syncToDatabase() 把 RAM 窗口（1000 条）整批
// INSERT OR REPLACE 回写，未登录行带的空 QString 又被绑成 NULL，第②步的成果当场被打回去——
// 实测第二次启动于是又打 "0 from JSON + 1000 sealed empty"。所以"这一列永不为 NULL"这条不变式
// 写在 alarm_dao.cpp 两条 INSERT 的值位上（IFNULL(:task_id, '')），回归断言见 test_database 第 5 组。
int DatabaseManager::backfillAlarmTaskIds()
{
    QSqlDatabase db = currentConnection();
    if (!db.isOpen()) {
        return 0;
    }

    QVector<QPair<QString, QString>> pending;   // <id, construction_info>
    {
        QSqlQuery pick(db);
        if (!pick.exec(QStringLiteral("SELECT id, construction_info FROM alarms "
                                      "WHERE task_id IS NULL "
                                      "AND construction_info IS NOT NULL "
                                      "AND construction_info <> ''"))) {
            qWarning() << "Alarm task_id backfill select failed:" << pick.lastError().text();
            return -1;
        }
        while (pick.next()) {
            pending.append(qMakePair(pick.value(0).toString(), pick.value(1).toString()));
        }
        // 带着活动语句进事务，后面 commit 会直接失败（实测踩过两次，见 initialize 里那条 PRAGMA）
        pick.finish();
    }

    // 计时是必要的而不是装饰：这段跑在 GUI 线程的启动路径上，几万行逐条 UPDATE 会直接
    // 体现在"开机后界面卡住多久"上。改这里之前先看这条日志的实测值再决定要不要挪线程。
    QElapsedTimer clock;
    clock.start();

    int filled = 0;
    bool began = false;
    if (!pending.isEmpty()) {
        if (!db.transaction()) {
            qWarning() << "Alarm task_id backfill begin failed:" << db.lastError().text();
            return -1;
        }
        began = true;
        QSqlQuery upd(db);
        upd.prepare(QStringLiteral("UPDATE alarms SET task_id = :task_id WHERE id = :id"));
        for (const QPair<QString, QString> &row : qAsConst(pending)) {
            upd.bindValue(QStringLiteral(":task_id"),
                          session::ConstructionInfo::taskIdFromJsonText(row.second));
            upd.bindValue(QStringLiteral(":id"), row.first);
            if (upd.exec()) {
                ++filled;
            } else {
                qWarning() << "Alarm task_id backfill update failed for" << row.first
                           << ":" << upd.lastError().text();
            }
        }
        upd.finish();   // 最后一条 UPDATE 仍是活动语句，不 finish 就 commit 会失败
    }

    // 收尾：把"确实没有施工计划身份"的行一次性写成空串（判据见文件头第②步）
    int sealed = 0;
    {
        QSqlQuery seal(db);
        if (seal.exec(QStringLiteral("UPDATE alarms SET task_id = '' WHERE task_id IS NULL"))) {
            sealed = seal.numRowsAffected();
        } else {
            qWarning() << "Alarm task_id seal pass failed:" << seal.lastError().text();
        }
        seal.finish();
    }

    if (began && !db.commit()) {
        // **不 rollback**：同一连接上 createTables 刚执行过 ALTER TABLE，SQLite 的 DDL 是事务性的，
        // 一句 rollback 会把刚补出来的列一起吞掉（老库升级路径实测就是这么坏的，后面按新列建
        // 索引跟着报 "no such column"）。回填只碰 NULL 行，提交没成最坏就是下次启动重来一遍。
        qWarning() << "Alarm task_id backfill commit failed, will retry next start:"
                   << db.lastError().text();
        return -1;
    }
    if (filled || sealed) {
        qInfo() << "Alarm task_id backfill:" << filled << "from JSON +" << sealed
                << "sealed empty, in" << clock.elapsed() << "ms";
    }
    return filled + sealed;
}

// ============================================================================
// executeSQL: 执行SQL语句
// ============================================================================
bool DatabaseManager::executeSQL(const QString &sql)
{
    QSqlQuery query(currentConnection());
    if (!query.exec(sql)) {
        QString error = query.lastError().text();
        qWarning() << "SQL execution failed:" << error;
        qWarning() << "SQL:" << sql;
        emit errorOccurred(error);
        return false;
    }
    return true;
}

// ============================================================================
// vacuum: 压缩数据库
// ============================================================================
bool DatabaseManager::vacuum()
{
    QMutexLocker lock(&mutex_);
    QSqlDatabase db = currentConnection();
    if (!initialized_ || !db.isOpen()) {
        return false;
    }

    QSqlQuery query(db);
    if (!query.exec("VACUUM;")) {
        qWarning() << "VACUUM failed:" << query.lastError().text();
        return false;
    }

    qInfo() << "Database vacuumed successfully";
    return true;
}

// ============================================================================
// backup: 备份数据库
// ============================================================================
bool DatabaseManager::backup(const QString &backupPath)
{
    QMutexLocker lock(&mutex_);
    QSqlDatabase db = currentConnection();
    if (!initialized_ || !db.isOpen()) {
        return false;
    }

    // 路径安全校验：防止 SQL 注入（VACUUM INTO 不支持参数化查询）
    // 1. 路径不能为空
    // 2. 不能包含单引号（SQL 字符串分隔符）
    // 3. 不能包含分号（SQL 语句分隔符）
    // 4. 不能包含路径遍历（../）
    if (backupPath.isEmpty() ||
        backupPath.contains('\'') ||
        backupPath.contains(';') ||
        backupPath.contains("..")) {
        qWarning() << "Backup failed: invalid path (security check)";
        return false;
    }

    // 确保备份目录存在
    QFileInfo fileInfo(backupPath);
    QDir dir = fileInfo.absoluteDir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    // VACUUM INTO 要求目标文件不存在，否则报 "output file already exists"。
    // 每日备份按日期命名（backups/idge_yyyyMMdd.db），当天重复触发
    // （启动即备份 + 凌晨定时）属于常态，语义即"覆盖为最新快照"，
    // 因此先删除同名旧备份再执行。
    if (QFile::exists(backupPath)) {
        if (!QFile::remove(backupPath)) {
            qWarning() << "Backup failed: cannot remove existing file" << backupPath;
            return false;
        }
    }

    // 使用 SQLite 的备份 API
    QSqlQuery query(db);
    QString sql = QString("VACUUM INTO '%1';").arg(backupPath);
    if (!query.exec(sql)) {
        qWarning() << "Backup failed:" << query.lastError().text();
        return false;
    }

    qInfo() << "Database backed up to:" << backupPath;
    return true;
}

// ============================================================================
// databaseSize: 获取数据库文件大小
// ============================================================================
int64_t DatabaseManager::databaseSize() const
{
    QMutexLocker lock(&mutex_);
    QFileInfo fileInfo(dbPath_);
    if (fileInfo.exists()) {
        return fileInfo.size();
    }
    return 0;
}

// ============================================================================
// cleanOldDetections: 清理N天前的检测数据
// ============================================================================
// 删除 create_time 早于 cutoff 的所有检测记录。
//
// 使用场景：
//   - 定期调用（如每天凌晨），防止数据库无限增长
//   - 默认保留 30 天（可通过 setRetentionDays() 配置）
//
// 返回：实际删除的记录数
//
// ============================================================================
int DatabaseManager::cleanOldDetections(int daysToKeep)
{
    QMutexLocker lock(&mutex_);
    QSqlDatabase db = currentConnection();
    if (!initialized_ || !db.isOpen()) {
        return 0;
    }

    // 计算截止时间戳（毫秒）
    QDateTime cutoff = QDateTime::currentDateTime().addDays(-daysToKeep);
    long cutoffMs = cutoff.toMSecsSinceEpoch();

    QSqlQuery query(db);
    query.prepare("DELETE FROM detections WHERE timestamp < :cutoff");
    query.bindValue(":cutoff", static_cast<qlonglong>(cutoffMs));

    if (!query.exec()) {
        qWarning() << "Clean detections failed:" << query.lastError().text();
        return 0;
    }

    int deleted = query.numRowsAffected();
    if (deleted > 0) {
        qInfo() << "Cleaned" << deleted << "detection records older than" << daysToKeep << "days";
    }
    return deleted;
}

// ============================================================================
// cleanOldAlarms: 清理N天前的报警数据
// ============================================================================
// 删除 create_time 早于 cutoff 的所有报警记录。
//
// 注意：
//   - 删除前应考虑是否需要归档（备份重要报警）
//   - 删除后数据库空间不会立即释放，需要 VACUUM 回收
//
// ============================================================================
int DatabaseManager::cleanOldAlarms(int daysToKeep)
{
    QMutexLocker lock(&mutex_);
    QSqlDatabase db = currentConnection();
    if (!initialized_ || !db.isOpen()) {
        return 0;
    }

    // 计算截止时间（ISO8601格式，与alarms表的create_time字段格式一致）
    QDateTime cutoff = QDateTime::currentDateTime().addDays(-daysToKeep);
    QString cutoffStr = cutoff.toString(Qt::ISODate);

    QSqlQuery query(db);
    query.prepare("DELETE FROM alarms WHERE create_time < :cutoff");
    query.bindValue(":cutoff", cutoffStr);

    if (!query.exec()) {
        qWarning() << "Clean alarms failed:" << query.lastError().text();
        return 0;
    }

    int deleted = query.numRowsAffected();
    if (deleted > 0) {
        qInfo() << "Cleaned" << deleted << "alarm records older than" << daysToKeep << "days";
    }
    return deleted;
}

// ============================================================================
// setRetentionDays: 设置数据保留天数
// ============================================================================
void DatabaseManager::setRetentionDays(int days)
{
    QMutexLocker lock(&mutex_);
    retentionDays_ = qMax(1, days);  // 至少保留1天
}

// ============================================================================
// setDataDir: 设置数据目录
// ============================================================================
void DatabaseManager::setDataDir(const QString &dir)
{
    QMutexLocker lock(&mutex_);
    dataDir_ = dir;

    // 确保目录存在
    QDir d(dir);
    if (!d.exists()) {
        d.mkpath(".");
    }
}
