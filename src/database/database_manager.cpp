// ============================================================================
// database_manager.cpp - 数据库管理器实现
// ============================================================================
//
// 作用：
//   管理 SQLite 数据库连接、初始化、表创建和维护。
//
// ============================================================================

#include "database_manager.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QDateTime>
#include <QFileInfo>
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
//   1. 确保数据目录存在
//   2. 打开 SQLite 数据库连接
//   3. 启用 WAL 模式（提高并发性能）
//   4. 创建表结构
//
// ============================================================================
bool DatabaseManager::initialize(const QString &dbPath)
{
    QMutexLocker lock(&mutex_);

    // 如果已经打开，先关闭
    if (db_.isOpen()) {
        db_.close();
    }

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

    // 打开数据库连接
    db_ = QSqlDatabase::addDatabase("QSQLITE");
    db_.setDatabaseName(dbPath_);

    if (!db_.open()) {
        QString error = db_.lastError().text();
        qCritical() << "Failed to open database:" << error;
        emit errorOccurred(error);
        return false;
    }

    qInfo() << "Database opened:" << dbPath_;

    // 启用 WAL 模式（Write-Ahead Logging，提高并发性能）
    QSqlQuery query(db_);
    if (!query.exec("PRAGMA journal_mode=WAL;")) {
        qWarning() << "Failed to set WAL mode:" << query.lastError().text();
    }

    // 启用外键约束
    if (!query.exec("PRAGMA foreign_keys=ON;")) {
        qWarning() << "Failed to enable foreign keys:" << query.lastError().text();
    }

    // 设置 busy timeout（5秒）
    if (!query.exec("PRAGMA busy_timeout=5000;")) {
        qWarning() << "Failed to set busy timeout:" << query.lastError().text();
    }

    // 创建表结构
    if (!createTables()) {
        qCritical() << "Failed to create tables";
        db_.close();
        return false;
    }

    emit databaseOpened();
    return true;
}

// ============================================================================
// close: 关闭数据库
// ============================================================================
void DatabaseManager::close()
{
    QMutexLocker lock(&mutex_);
    if (db_.isOpen()) {
        db_.close();
        qInfo() << "Database closed";
        emit databaseClosed();
    }
}

// ============================================================================
// isOpen: 数据库是否已打开
// ============================================================================
bool DatabaseManager::isOpen() const
{
    QMutexLocker lock(&mutex_);
    return db_.isOpen();
}

// ============================================================================
// database: 获取数据库连接
// ============================================================================
QSqlDatabase DatabaseManager::database() const
{
    QMutexLocker lock(&mutex_);
    return db_;
}

// ============================================================================
// createTables: 创建所有表
// ============================================================================
bool DatabaseManager::createTables()
{
    // 1. 创建检测信息表
    QString createDetections = R"(
        CREATE TABLE IF NOT EXISTS detections (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp       INTEGER NOT NULL,
            channel         INTEGER NOT NULL,
            frame_id        INTEGER,
            class_id        INTEGER NOT NULL,
            class_name      TEXT NOT NULL,
            confidence      REAL NOT NULL,
            bbox_left       INTEGER NOT NULL,
            bbox_top        INTEGER NOT NULL,
            bbox_right      INTEGER NOT NULL,
            bbox_bottom     INTEGER NOT NULL,
            created_at      DATETIME DEFAULT CURRENT_TIMESTAMP
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
    QString createAlarms = R"(
        CREATE TABLE IF NOT EXISTS alarms (
            id              TEXT PRIMARY KEY,
            alarm_type      TEXT NOT NULL,
            alarm_level     INTEGER NOT NULL DEFAULT 3,
            alarm_time      TEXT NOT NULL,
            channel         INTEGER NOT NULL,
            class_id        INTEGER NOT NULL,
            class_name      TEXT NOT NULL,
            confidence      REAL NOT NULL,
            image_path      TEXT,
            video_path      TEXT,
            status          TEXT NOT NULL DEFAULT 'pending',
            dispose_result  TEXT,
            dispose_user_id INTEGER,
            dispose_user_name TEXT,
            dispose_time    TEXT,
            dispose_photo   TEXT,
            remark          TEXT,
            read_time       TEXT,
            create_time     TEXT NOT NULL,
            update_time     TEXT
        );
    )";

    if (!executeSQL(createAlarms)) {
        return false;
    }

    // 创建报警表索引
    executeSQL("CREATE INDEX IF NOT EXISTS idx_alarms_type ON alarms(alarm_type);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_alarms_level ON alarms(alarm_level);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_alarms_status ON alarms(status);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_alarms_time ON alarms(alarm_time);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_alarms_channel ON alarms(channel);");

    qInfo() << "Database tables created successfully";
    return true;
}

// ============================================================================
// executeSQL: 执行SQL语句
// ============================================================================
bool DatabaseManager::executeSQL(const QString &sql)
{
    QSqlQuery query(db_);
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
    if (!db_.isOpen()) {
        return false;
    }

    QSqlQuery query(db_);
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
    if (!db_.isOpen()) {
        return false;
    }

    // 确保备份目录存在
    QFileInfo fileInfo(backupPath);
    QDir dir = fileInfo.absoluteDir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    // 使用 SQLite 的备份 API
    QSqlQuery query(db_);
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
int DatabaseManager::cleanOldDetections(int daysToKeep)
{
    QMutexLocker lock(&mutex_);
    if (!db_.isOpen()) {
        return 0;
    }

    // 计算截止时间戳（毫秒）
    QDateTime cutoff = QDateTime::currentDateTime().addDays(-daysToKeep);
    long cutoffMs = cutoff.toMSecsSinceEpoch();

    QSqlQuery query(db_);
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
int DatabaseManager::cleanOldAlarms(int daysToKeep)
{
    QMutexLocker lock(&mutex_);
    if (!db_.isOpen()) {
        return 0;
    }

    // 计算截止时间戳（毫秒）
    QDateTime cutoff = QDateTime::currentDateTime().addDays(-daysToKeep);
    long cutoffMs = cutoff.toMSecsSinceEpoch();

    QSqlQuery query(db_);
    query.prepare("DELETE FROM alarms WHERE timestamp < :cutoff");
    query.bindValue(":cutoff", static_cast<qlonglong>(cutoffMs));

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
