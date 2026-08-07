#include "storage/ApplicationDatabase.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

ApplicationDatabase::ApplicationDatabase(QString databasePath)
    : m_databasePath(std::move(databasePath))
    , m_connectionName(QStringLiteral("shellium-%1")
                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
    if (m_databasePath.isEmpty()) {
        const QString dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        m_databasePath = QDir(dataDirectory).filePath(QStringLiteral("shellium.db"));
    }
}

ApplicationDatabase::~ApplicationDatabase()
{
    if (m_database.isValid()) {
        m_database.close();
        m_database = {};
    }
    // removeDatabase 前必须释放所有 QSqlDatabase 值副本，否则 Qt 会留下悬挂连接警告。
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool ApplicationDatabase::initialize(QString *errorMessage)
{
    const QFileInfo databaseInfo(m_databasePath);
    if (!QDir().mkpath(databaseInfo.absolutePath())) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("无法创建数据库目录：%1").arg(databaseInfo.absolutePath());
        }
        return false;
    }

    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(m_databasePath);
    if (!m_database.open()) {
        if (errorMessage != nullptr) {
            *errorMessage = m_database.lastError().text();
        }
        return false;
    }

    // WAL 允许界面读取和后台历史写入并发进行；外键与 busy_timeout 防止静默孤儿数据和瞬时锁失败。
    const QStringList pragmas = {
        QStringLiteral("PRAGMA journal_mode=WAL"),
        QStringLiteral("PRAGMA foreign_keys=ON"),
        QStringLiteral("PRAGMA busy_timeout=5000")
    };
    for (const QString &pragma : pragmas) {
        if (!executeMigration(pragma, errorMessage)) {
            return false;
        }
    }

    if (!m_database.transaction()) {
        if (errorMessage != nullptr) {
            *errorMessage = m_database.lastError().text();
        }
        return false;
    }

    // 所有表均使用 IF NOT EXISTS，保证首次安装和旧版本升级重复执行时安全。
    const QStringList migrations = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL)"),
        QStringLiteral("INSERT INTO schema_version(version) SELECT 1 WHERE NOT EXISTS "
                       "(SELECT 1 FROM schema_version)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS hosts ("
                       "id TEXT PRIMARY KEY, parent_id TEXT REFERENCES hosts(id) ON DELETE CASCADE, "
                       "kind INTEGER NOT NULL, name TEXT NOT NULL, host TEXT, port INTEGER, username TEXT, "
                       "credential_id TEXT, identity_file TEXT, tags TEXT, proxy_jump TEXT, "
                       "legacy_hmac INTEGER NOT NULL DEFAULT 0, sort_order INTEGER NOT NULL DEFAULT 0, "
                       "created_at TEXT NOT NULL, updated_at TEXT NOT NULL)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_hosts_parent ON hosts(parent_id, sort_order)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS credentials ("
                       "id TEXT PRIMARY KEY, name TEXT NOT NULL, type INTEGER NOT NULL, "
                       "encrypted_payload BLOB NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS snippets ("
                       "id TEXT PRIMARY KEY, parent_id TEXT, name TEXT NOT NULL, command TEXT, "
                       "description TEXT, sort_order INTEGER NOT NULL DEFAULT 0, updated_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS command_history ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT, host_id TEXT, command TEXT NOT NULL, "
                       "executed_at TEXT NOT NULL, exit_code INTEGER)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_command_history_host "
                       "ON command_history(host_id, executed_at DESC)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS connection_history ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT, host_id TEXT, connected_at TEXT NOT NULL, "
                       "disconnected_at TEXT, result INTEGER, error TEXT)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS port_forwards ("
                       "id TEXT PRIMARY KEY, host_id TEXT, name TEXT NOT NULL, type INTEGER NOT NULL, "
                       "bind_host TEXT, bind_port INTEGER, target_host TEXT, target_port INTEGER, "
                       "auto_start INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS workspaces ("
                       "id TEXT PRIMARY KEY, name TEXT NOT NULL, layout_json TEXT NOT NULL, updated_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS plugin_state ("
                       "plugin_id TEXT PRIMARY KEY, enabled INTEGER NOT NULL, config_json TEXT NOT NULL)")
    };
    for (const QString &migration : migrations) {
        if (!executeMigration(migration, errorMessage)) {
            m_database.rollback();
            return false;
        }
    }
    if (!m_database.commit()) {
        if (errorMessage != nullptr) {
            *errorMessage = m_database.lastError().text();
        }
        return false;
    }
    return true;
}

QSqlDatabase ApplicationDatabase::connection() const
{
    return m_database;
}

QString ApplicationDatabase::databasePath() const
{
    return m_databasePath;
}

bool ApplicationDatabase::executeMigration(const QString &sql, QString *errorMessage)
{
    QSqlQuery query(m_database);
    if (query.exec(sql)) {
        return true;
    }
    if (errorMessage != nullptr) {
        *errorMessage = query.lastError().text();
    }
    return false;
}
