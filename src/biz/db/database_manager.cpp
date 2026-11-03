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
#include <QSqlQuery>
#include <QSqlError>
#include <QSet>
#include <QDateTime>
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
            status          TEXT NOT NULL DEFAULT 'pending',   -- pending / rectified / false_alarm。⚠ `rectified` 是本机"已确认"的语义 = **我看过了**，不是"已处置"；v1 只有 PENDING/DISPOSED/FALSE_ALARM 三值，所以出站映射成 `PENDING + readAt`（B3 的落地口径）
            dispose_result  TEXT,                              -- 处置措施文本（v1 的 dispose.measure）。**全仓零写入方**：AlarmDAO::dispose() 有实现无调用，等 /api/alarms/{id}/dispose
            dispose_user_id INTEGER,                           -- 处置人 ID。⚠ 类型已与 v1 冲突：对端的 personId 是字符串工号（"05-0101"/"P-001"），处置闭环落地时要改成 TEXT，别在 INTEGER 上硬塞
            dispose_user_name TEXT,                            -- 处置人/误报人姓名（同 dispose_result，无写入方）
            dispose_time    TEXT,                              -- 处置时间（同 alarm_time 口径；v1 的 disposedAt，终态时必填）
            dispose_photo   TEXT,                              -- 现场整改照片**单张**路径。v1 允许 0~5 张（photoUrls 数组）→ 要么换成 JSON 数组列，要么加子表，现在这列装不下
            remark          TEXT,                              -- 补充说明（v1 的 dispose.remark）；AlarmDAO::updateRemark() 有实现，写入方待处置表单
            read_time       TEXT,                              -- **最早**已读时间。markRead() 现在保证"保留最早 + 必推 update_time"（含墙钟回拨），但调用方还没有：v1 的 POST /alarms/{id}/read 未实现
            create_time     TEXT NOT NULL,                     -- 入库时间。建行那一刻由 makeAlarmRecord 写死（现等于 alarm_time）；早期版本留空、靠 DAO 兜底当前时间，所以老行可能是 NULL
            update_time     TEXT,                              -- 最后变更时间：状态/误报/已读/备注/图片/片段每条 UPDATE 都推它（`alarm_dao.cpp:785/811/840/869/900/923/944/965`）。⚠ **v1 的断线补漏游标就是它**（`AlarmDAO::queryUpdatedSince` 按 `>= 游标` 过滤），所以建行即写不能留空——空串会让刚产生的报警被增量整批漏掉。实测两份库的空串行数差两个量级：`data/idge.db` 1992/3432 为空，`build/build_rk3588_linux/data/idge.db` 只有 136/38294 为空（片段回填顺带 bump 了绝大多数行）→ `COALESCE(NULLIF(update_time,''),alarm_time)` 这条兜底在哪份库上真正生效，取决于跑的是哪一个
            worker_name     TEXT,                              -- 施工负责人姓名，扫码登录会话定死后在**产生这一刻**盖上（不等到显示/上行再回查）。报警列表那一列与"按负责人"筛选用它——为这个才单独成列，省掉每行解析 JSON
            construction_info TEXT                             -- 整份施工信息 JSON（compact 原样，含电话/工号/工作内容/construction_id）。详情框逐键渲染与上行 data["construction"] 都从它出，**加字段不用改表**。v1 的 taskId 也取这里的 construction_id（是否等同 v1 施工计划 ID 待甲方确认，报告 B2）
        );
    )";

    if (!executeSQL(createAlarms)) {
        return false;
    }

    // 补列必须夹在建表与建索引之间：CREATE TABLE IF NOT EXISTS 对已部署的老库是
    // 空操作，新加的施工信息两列只有靠这里 ALTER 才存在；而下面两条按新列建的索引
    // 早于 ALTER 就会失败（老库上实测 "no such column: worker_name"）。
    // 失败不致命（只影响报警带施工信息，不影响其余功能），所以只告警不返回 false。
    if (!ensureColumns(QStringLiteral("alarms"),
                       {QStringLiteral("worker_name TEXT"),
                        QStringLiteral("construction_info TEXT")})) {
        qWarning() << "Alarms construction columns not fully ensured";
    }

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
