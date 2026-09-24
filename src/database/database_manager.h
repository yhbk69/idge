#ifndef DATABASE_MANAGER_H
#define DATABASE_MANAGER_H

// ============================================================================
// database_manager.h - 数据库管理器
// ============================================================================
//
// 作用：
//   管理 SQLite 数据库连接、初始化、表创建和维护。
//   是整个数据库层的核心组件。
//
// 主要职责：
//   1. 初始化数据库连接（单例模式）
//   2. 自动创建表结构
//   3. 提供数据库维护功能（压缩、备份、清理）
//   4. 线程安全的数据库访问：database() 返回"当前线程自己的"命名连接，
//      解码线程写库与 GUI 线程查询各用各的连接，经 WAL + busy_timeout 并发
//
// 使用方式：
//   // 初始化
//   DatabaseManager::instance().initialize("/data/idge/idge.db");
//
//   // 获取数据库连接
//   QSqlDatabase db = DatabaseManager::instance().database();
//
// ============================================================================

#include <QObject>
#include <QString>
#include <QSqlDatabase>
#include <QMutex>
#include <QDir>
#include <QThreadStorage>
#include <QSharedPointer>

class DatabaseManager : public QObject
{
    Q_OBJECT
public:
    // 获取单例实例
    static DatabaseManager &instance();

    // 初始化数据库
    // dbPath: 数据库文件路径（默认 "idge.db"）
    bool initialize(const QString &dbPath = "idge.db");

    // 关闭数据库
    void close();

    // 数据库是否已打开
    bool isOpen() const;

    // 获取数据库连接（用于DAO操作）
    QSqlDatabase database() const;

    // ========================================================================
    // 数据库维护
    // ========================================================================

    // 压缩数据库（回收空间）
    bool vacuum();

    // 备份数据库
    bool backup(const QString &backupPath);

    // 获取数据库文件大小（字节）
    int64_t databaseSize() const;

    // ========================================================================
    // 数据清理
    // ========================================================================

    // 清理N天前的检测数据，返回删除的行数
    int cleanOldDetections(int daysToKeep);

    // 清理N天前的报警数据，返回删除的行数
    int cleanOldAlarms(int daysToKeep);

    // ========================================================================
    // 配置
    // ========================================================================

    // 设置数据保留天数（默认30天）
    void setRetentionDays(int days);
    int retentionDays() const { return retentionDays_; }

    // 设置数据目录
    void setDataDir(const QString &dir);
    QString dataDir() const { return dataDir_; }

signals:
    // 数据库已打开
    void databaseOpened();

    // 数据库已关闭
    void databaseClosed();

    // 发生错误
    void errorOccurred(const QString &error);

private:
    explicit DatabaseManager(QObject *parent = nullptr);
    ~DatabaseManager();

    // 禁止拷贝
    DatabaseManager(const DatabaseManager &) = delete;
    DatabaseManager &operator=(const DatabaseManager &) = delete;

    // 创建所有表
    bool createTables();

    // 执行SQL语句（调用线程自己的连接；须在持有 mutex_ 的路径上调用）
    bool executeSQL(const QString &sql);

    // 线程连接包装：Qt 规定一个 QSqlDatabase 连接只能由创建它的线程使用。
    // 检测写入跑在解码线程、查询/处置/清理跑在 GUI 线程，故每个线程持有
    // 一个独立命名连接（QThreadStorage 绑定线程生命周期）：线程退出时析构
    // 自动 close + removeDatabase；dbPath_ 变更（重新 initialize）时自动重建。
    struct ThreadConnection {
        QString name;       // QSqlDatabase 注册名（全局唯一，带序号）
        QString path;       // 该连接对应的数据库文件路径
        QSqlDatabase db;
        ~ThreadConnection()
        {
            if (name.isEmpty())
                return;
            if (db.isOpen())
                db.close();
            db = QSqlDatabase();                // 先释放本地引用，避免 removeDatabase 告警
            QSqlDatabase::removeDatabase(name);
        }
    };

    // 取得（必要时惰性创建）当前线程的数据库连接；须在持有 mutex_ 时调用
    QSqlDatabase currentConnection() const;

    mutable QMutex mutex_;      // 互斥锁（保护连接注册表与下列元数据；不覆盖 SQL 执行期）
    mutable QThreadStorage<QSharedPointer<ThreadConnection>> threadConn_;  // 每线程一个连接
    mutable quint64 connSeq_ = 0;   // 连接名序号，保证注册名全局唯一
    bool initialized_ = false;  // initialize() 成功后置位；close()/重新初始化复位
    QString dbPath_;            // 数据库文件路径
    QString dataDir_;           // 数据目录
    int retentionDays_ = 30;    // 数据保留天数
};

#endif // DATABASE_MANAGER_H
