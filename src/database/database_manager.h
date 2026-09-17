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
//   4. 线程安全的数据库访问
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

    // 执行SQL语句
    bool executeSQL(const QString &sql);

    mutable QMutex mutex_;      // 互斥锁
    QSqlDatabase db_;           // 数据库连接
    QString dbPath_;            // 数据库文件路径
    QString dataDir_;           // 数据目录
    int retentionDays_ = 30;    // 数据保留天数
};

#endif // DATABASE_MANAGER_H
