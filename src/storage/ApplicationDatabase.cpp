#include "storage/ApplicationDatabase.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <QVariant>

namespace {

/// 当前代码要求的 schema 版本。每次结构变更必须递增，并在 upgradeSchema 中补齐对应分支。
constexpr int kSchemaVersion = 2;

/// 内存数据库的 SQLite 特殊路径。
const QString kMemoryPath = QStringLiteral(":memory:");

/// 列表字段在单个 TEXT 列中的分隔符，与 OpenSSH -J 的跳板链写法保持一致。
constexpr QChar kListSeparator = QLatin1Char(',');

/// LIKE 模式的转义字符，随查询以 ESCAPE 子句声明。
constexpr QChar kLikeEscape = QLatin1Char('\\');

/**
 * @brief 把时间统一序列化为带毫秒的 UTC ISO8601 文本。
 *
 * 统一到 UTC 是为了让文本排序等价于时间排序：历史记录靠 ORDER BY 字符串取“最近 N 条”，
 * 一旦混入不同偏移量的本地时间，跨时区或夏令时切换后排序就会错乱。
 */
QVariant serializeTime(const QDateTime &value)
{
    if (!value.isValid()) {
        return {};
    }
    return value.toUTC().toString(Qt::ISODateWithMs);
}

QDateTime deserializeTime(const QVariant &value)
{
    if (value.isNull()) {
        return {};
    }
    return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
}

/// 空字符串一律存 NULL：parent_id 上有外键约束，空串会被当成一个并不存在的父节点。
QVariant nullableText(const QString &value)
{
    if (value.isEmpty()) {
        return {};
    }
    return value;
}

QString joinList(const QStringList &values)
{
    return values.join(kListSeparator);
}

QStringList splitList(const QVariant &value)
{
    const QString text = value.toString();
    if (text.isEmpty()) {
        return {};
    }
    return text.split(kListSeparator, Qt::SkipEmptyParts);
}

/**
 * @brief 转义用户输入中的 LIKE 通配符。
 *
 * 用户搜索 "db_1" 时期待的是字面下划线；不转义的话 _ 会匹配任意字符，把无关主机也搜出来。
 */
QString escapeLikePattern(const QString &raw)
{
    QString escaped;
    escaped.reserve(raw.size() * 2);
    for (const QChar character : raw) {
        if (character == kLikeEscape || character == QLatin1Char('%')
            || character == QLatin1Char('_')) {
            escaped.append(kLikeEscape);
        }
        escaped.append(character);
    }
    return escaped;
}

/// 记录查询失败原因，同时把 SQL 语句一并带上，避免只看到 "near ...: syntax error"。
void reportQueryError(const QSqlQuery &query, QString *errorMessage)
{
    if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("%1（SQL：%2）")
                            .arg(query.lastError().text(), query.executedQuery().isEmpty()
                                                               ? query.lastQuery()
                                                               : query.executedQuery());
    }
}

HostRecord readHostRow(const QSqlQuery &query)
{
    HostRecord record;
    record.id = query.value(QStringLiteral("id")).toString();
    record.parentId = query.value(QStringLiteral("parent_id")).toString();
    record.kind = query.value(QStringLiteral("kind")).toInt() == static_cast<int>(HostNodeKind::Folder)
        ? HostNodeKind::Folder
        : HostNodeKind::Host;
    record.name = query.value(QStringLiteral("name")).toString();
    record.host = query.value(QStringLiteral("host")).toString();
    const QVariant port = query.value(QStringLiteral("port"));
    if (!port.isNull()) {
        record.port = static_cast<quint16>(port.toUInt());
    }
    record.username = query.value(QStringLiteral("username")).toString();
    record.credentialId = query.value(QStringLiteral("credential_id")).toString();
    record.identityFile = query.value(QStringLiteral("identity_file")).toString();
    record.tags = splitList(query.value(QStringLiteral("tags")));
    record.proxyJumps = splitList(query.value(QStringLiteral("proxy_jump")));
    record.allowLegacyHmacSha1 = query.value(QStringLiteral("legacy_hmac")).toInt() != 0;
    record.sortOrder = query.value(QStringLiteral("sort_order")).toInt();
    record.createdAt = deserializeTime(query.value(QStringLiteral("created_at")));
    record.updatedAt = deserializeTime(query.value(QStringLiteral("updated_at")));
    return record;
}

CredentialRecord readCredentialRow(const QSqlQuery &query)
{
    CredentialRecord record;
    record.id = query.value(QStringLiteral("id")).toString();
    record.name = query.value(QStringLiteral("name")).toString();
    record.type = static_cast<CredentialType>(query.value(QStringLiteral("type")).toInt());
    record.encryptedPayload = query.value(QStringLiteral("encrypted_payload")).toByteArray();
    record.cipher = query.value(QStringLiteral("cipher")).toString();
    record.nonce = query.value(QStringLiteral("nonce")).toByteArray();
    record.kdfAlgorithm = query.value(QStringLiteral("kdf_algorithm")).toString();
    record.kdfSalt = query.value(QStringLiteral("kdf_salt")).toByteArray();
    record.kdfIterations = query.value(QStringLiteral("kdf_iterations")).toLongLong();
    record.kdfMemoryKib = query.value(QStringLiteral("kdf_memory_kib")).toLongLong();
    record.kdfParallelism = query.value(QStringLiteral("kdf_parallelism")).toLongLong();
    record.createdAt = deserializeTime(query.value(QStringLiteral("created_at")));
    record.updatedAt = deserializeTime(query.value(QStringLiteral("updated_at")));
    return record;
}

/// 供列表接口统一设置 ok 出参，避免每处都写一遍空指针判断。
void setFlag(bool *flag, const bool value)
{
    if (flag != nullptr) {
        *flag = value;
    }
}

} // namespace

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

QString ApplicationDatabase::inMemoryPath()
{
    return kMemoryPath;
}

int ApplicationDatabase::expectedSchemaVersion()
{
    return kSchemaVersion;
}

bool ApplicationDatabase::initialize(QString *errorMessage)
{
    // 内存数据库没有目录，也不应该在用户数据区留下任何痕迹，因此跳过建目录。
    if (m_databasePath != kMemoryPath) {
        const QFileInfo databaseInfo(m_databasePath);
        if (!QDir().mkpath(databaseInfo.absolutePath())) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("无法创建数据库目录：%1").arg(databaseInfo.absolutePath());
            }
            return false;
        }
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

    if (!createBaseSchema(errorMessage)) {
        return false;
    }
    return upgradeSchema(errorMessage);
}

bool ApplicationDatabase::createBaseSchema(QString *errorMessage)
{
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

bool ApplicationDatabase::upgradeSchema(QString *errorMessage)
{
    const std::optional<int> currentVersion = schemaVersion(errorMessage);
    if (!currentVersion.has_value()) {
        return false;
    }
    if (*currentVersion >= kSchemaVersion) {
        return true;
    }
    if (*currentVersion < 1) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("数据库 schema 版本异常：%1").arg(*currentVersion);
        }
        return false;
    }

    if (!m_database.transaction()) {
        if (errorMessage != nullptr) {
            *errorMessage = m_database.lastError().text();
        }
        return false;
    }

    QStringList statements;
    if (*currentVersion < 2) {
        // 版本 2：凭据补齐解密所需的公开元数据，连接历史补齐时长列。
        // 只用 ADD COLUMN 而不重建表，既保留已有行，也不会丢失外键与索引定义。
        statements << QStringLiteral("ALTER TABLE credentials ADD COLUMN cipher TEXT")
                   << QStringLiteral("ALTER TABLE credentials ADD COLUMN nonce BLOB")
                   << QStringLiteral("ALTER TABLE credentials ADD COLUMN kdf_algorithm TEXT")
                   << QStringLiteral("ALTER TABLE credentials ADD COLUMN kdf_salt BLOB")
                   << QStringLiteral("ALTER TABLE credentials ADD COLUMN kdf_iterations INTEGER NOT NULL DEFAULT 0")
                   << QStringLiteral("ALTER TABLE credentials ADD COLUMN kdf_memory_kib INTEGER NOT NULL DEFAULT 0")
                   << QStringLiteral("ALTER TABLE credentials ADD COLUMN kdf_parallelism INTEGER NOT NULL DEFAULT 0")
                   << QStringLiteral("ALTER TABLE connection_history ADD COLUMN duration_ms INTEGER")
                   << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_connection_history_time "
                                     "ON connection_history(connected_at DESC)")
                   << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_hosts_name ON hosts(name)");
    }
    statements << QStringLiteral("UPDATE schema_version SET version = %1").arg(kSchemaVersion);

    for (const QString &statement : statements) {
        if (!executeMigration(statement, errorMessage)) {
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

std::optional<int> ApplicationDatabase::schemaVersion(QString *errorMessage) const
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT version FROM schema_version LIMIT 1"))) {
        reportQueryError(query, errorMessage);
        return std::nullopt;
    }
    if (!query.next()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("schema_version 表为空，数据库可能已损坏");
        }
        return std::nullopt;
    }
    return query.value(0).toInt();
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

bool ApplicationDatabase::runInTransaction(const std::function<bool(QString *)> &work,
                                           QString *errorMessage)
{
    if (!m_database.isOpen()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("数据库尚未初始化");
        }
        return false;
    }

    // 已处于外层事务时直接参与其中：外层负责最终提交或回滚，这里再 BEGIN 会被 SQLite 拒绝。
    if (m_transactionDepth > 0) {
        QString innerError;
        if (work(&innerError)) {
            return true;
        }
        if (errorMessage != nullptr) {
            *errorMessage = innerError;
        }
        return false;
    }

    if (!m_database.transaction()) {
        if (errorMessage != nullptr) {
            *errorMessage = m_database.lastError().text();
        }
        return false;
    }

    ++m_transactionDepth;
    QString innerError;
    const bool succeeded = work(&innerError);
    --m_transactionDepth;

    if (!succeeded) {
        m_database.rollback();
        if (errorMessage != nullptr) {
            *errorMessage = innerError.isEmpty() ? QStringLiteral("事务被调用方放弃") : innerError;
        }
        return false;
    }
    if (!m_database.commit()) {
        const QString commitError = m_database.lastError().text();
        m_database.rollback();
        if (errorMessage != nullptr) {
            *errorMessage = commitError;
        }
        return false;
    }
    return true;
}

// -------------------------------------------------------------------- 主机与分组

bool ApplicationDatabase::saveHost(const HostRecord &record, QString *errorMessage)
{
    if (record.id.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("主机记录缺少 id");
        }
        return false;
    }
    if (record.name.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("主机记录缺少名称");
        }
        return false;
    }
    if (!record.parentId.isEmpty() && record.parentId == record.id) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("主机不能把自己作为父分组");
        }
        return false;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime createdAt = record.createdAt.isValid() ? record.createdAt : now;
    const QDateTime updatedAt = record.updatedAt.isValid() ? record.updatedAt : now;

    return runInTransaction([&](QString *innerError) {
        QSqlQuery query(m_database);
        // 用 upsert 而不是先 DELETE 再 INSERT：后者会触发 ON DELETE CASCADE，
        // 更新一个分组时会连带删光它下面的所有主机。
        query.prepare(QStringLiteral(
            "INSERT INTO hosts (id, parent_id, kind, name, host, port, username, credential_id, "
            "identity_file, tags, proxy_jump, legacy_hmac, sort_order, created_at, updated_at) "
            "VALUES (:id, :parent_id, :kind, :name, :host, :port, :username, :credential_id, "
            ":identity_file, :tags, :proxy_jump, :legacy_hmac, :sort_order, :created_at, :updated_at) "
            "ON CONFLICT(id) DO UPDATE SET parent_id = excluded.parent_id, kind = excluded.kind, "
            "name = excluded.name, host = excluded.host, port = excluded.port, "
            "username = excluded.username, credential_id = excluded.credential_id, "
            "identity_file = excluded.identity_file, tags = excluded.tags, "
            "proxy_jump = excluded.proxy_jump, legacy_hmac = excluded.legacy_hmac, "
            "sort_order = excluded.sort_order, updated_at = excluded.updated_at"));
        query.bindValue(QStringLiteral(":id"), record.id);
        query.bindValue(QStringLiteral(":parent_id"), nullableText(record.parentId));
        query.bindValue(QStringLiteral(":kind"), static_cast<int>(record.kind));
        query.bindValue(QStringLiteral(":name"), record.name);
        query.bindValue(QStringLiteral(":host"), record.host);
        query.bindValue(QStringLiteral(":port"),
                        record.port.has_value() ? QVariant(static_cast<uint>(*record.port)) : QVariant());
        query.bindValue(QStringLiteral(":username"), record.username);
        query.bindValue(QStringLiteral(":credential_id"), record.credentialId);
        query.bindValue(QStringLiteral(":identity_file"), record.identityFile);
        query.bindValue(QStringLiteral(":tags"), joinList(record.tags));
        query.bindValue(QStringLiteral(":proxy_jump"), joinList(record.proxyJumps));
        query.bindValue(QStringLiteral(":legacy_hmac"), record.allowLegacyHmacSha1 ? 1 : 0);
        query.bindValue(QStringLiteral(":sort_order"), record.sortOrder);
        query.bindValue(QStringLiteral(":created_at"), serializeTime(createdAt));
        query.bindValue(QStringLiteral(":updated_at"), serializeTime(updatedAt));
        if (!query.exec()) {
            reportQueryError(query, innerError);
            return false;
        }
        return true;
    }, errorMessage);
}

std::optional<HostRecord> ApplicationDatabase::findHost(const QString &id,
                                                        QString *errorMessage) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT * FROM hosts WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        reportQueryError(query, errorMessage);
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    return readHostRow(query);
}

bool ApplicationDatabase::removeHost(const QString &id, int *removedCount, QString *errorMessage)
{
    if (removedCount != nullptr) {
        *removedCount = 0;
    }
    return runInTransaction([&](QString *innerError) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("DELETE FROM hosts WHERE id = :id"));
        query.bindValue(QStringLiteral(":id"), id);
        if (!query.exec()) {
            reportQueryError(query, innerError);
            return false;
        }
        if (removedCount != nullptr) {
            *removedCount = query.numRowsAffected();
        }
        return true;
    }, errorMessage);
}

QList<HostRecord> ApplicationDatabase::listHostsInGroup(const QString &parentId,
                                                        bool *ok,
                                                        QString *errorMessage) const
{
    setFlag(ok, false);
    QSqlQuery query(m_database);
    if (parentId.isEmpty()) {
        query.prepare(QStringLiteral(
            "SELECT * FROM hosts WHERE parent_id IS NULL ORDER BY sort_order, name"));
    } else {
        query.prepare(QStringLiteral(
            "SELECT * FROM hosts WHERE parent_id = :parent_id ORDER BY sort_order, name"));
        query.bindValue(QStringLiteral(":parent_id"), parentId);
    }
    if (!query.exec()) {
        reportQueryError(query, errorMessage);
        return {};
    }

    QList<HostRecord> records;
    while (query.next()) {
        records.append(readHostRow(query));
    }
    setFlag(ok, true);
    return records;
}

QList<HostRecord> ApplicationDatabase::searchHosts(const QString &keyword,
                                                   bool *ok,
                                                   QString *errorMessage) const
{
    setFlag(ok, false);
    const QString pattern = QStringLiteral("%%%1%%").arg(escapeLikePattern(keyword.trimmed()));

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT * FROM hosts WHERE name LIKE :name_pattern ESCAPE '\\' "
        "OR host LIKE :host_pattern ESCAPE '\\' ORDER BY kind, sort_order, name"));
    // 两个占位符绑定同一个值：Qt 各驱动对重复命名占位符的支持并不一致，重名会静默漏绑。
    query.bindValue(QStringLiteral(":name_pattern"), pattern);
    query.bindValue(QStringLiteral(":host_pattern"), pattern);
    if (!query.exec()) {
        reportQueryError(query, errorMessage);
        return {};
    }

    QList<HostRecord> records;
    while (query.next()) {
        records.append(readHostRow(query));
    }
    setFlag(ok, true);
    return records;
}

QList<HostRecord> ApplicationDatabase::hostsWithTag(const QString &tag,
                                                    bool *ok,
                                                    QString *errorMessage) const
{
    setFlag(ok, false);
    if (tag.trimmed().isEmpty()) {
        setFlag(ok, true);
        return {};
    }

    // 标签以逗号连接存于单列。两侧补上哨兵逗号后再匹配 ",tag," ，可避免 "prod" 命中 "production"。
    const QString pattern = QStringLiteral("%,%1,%").arg(escapeLikePattern(tag.trimmed()));
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT * FROM hosts WHERE ',' || IFNULL(tags, '') || ',' LIKE :pattern ESCAPE '\\' "
        "ORDER BY sort_order, name"));
    query.bindValue(QStringLiteral(":pattern"), pattern);
    if (!query.exec()) {
        reportQueryError(query, errorMessage);
        return {};
    }

    QList<HostRecord> records;
    while (query.next()) {
        records.append(readHostRow(query));
    }
    setFlag(ok, true);
    return records;
}

std::optional<HostRecord> ApplicationDatabase::resolveHost(const QString &id,
                                                           QString *errorMessage) const
{
    QString lookupError;
    std::optional<HostRecord> resolved = findHost(id, &lookupError);
    if (!resolved.has_value()) {
        if (!lookupError.isEmpty() && errorMessage != nullptr) {
            *errorMessage = lookupError;
        }
        return std::nullopt;
    }

    QSet<QString> visited{resolved->id};
    QString parentId = resolved->parentId;
    while (!parentId.isEmpty()) {
        // 数据损坏或导入错误可能造成父子成环；不设防会让界面在启动时直接死循环。
        if (visited.contains(parentId)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("主机分组存在环：%1").arg(parentId);
            }
            return std::nullopt;
        }
        visited.insert(parentId);

        const std::optional<HostRecord> ancestor = findHost(parentId, &lookupError);
        if (!ancestor.has_value()) {
            if (!lookupError.isEmpty()) {
                if (errorMessage != nullptr) {
                    *errorMessage = lookupError;
                }
                return std::nullopt;
            }
            break; // 父节点已不存在（外键关闭时可能出现），当作到达根层级处理。
        }

        if (!resolved->port.has_value()) {
            resolved->port = ancestor->port;
        }
        if (resolved->username.isEmpty()) {
            resolved->username = ancestor->username;
        }
        if (resolved->credentialId.isEmpty()) {
            resolved->credentialId = ancestor->credentialId;
        }
        if (resolved->identityFile.isEmpty()) {
            resolved->identityFile = ancestor->identityFile;
        }
        if (resolved->proxyJumps.isEmpty()) {
            resolved->proxyJumps = ancestor->proxyJumps;
        }
        for (const QString &tag : ancestor->tags) {
            if (!resolved->tags.contains(tag)) {
                resolved->tags.append(tag);
            }
        }
        parentId = ancestor->parentId;
    }
    return resolved;
}

// -------------------------------------------------------------------- 凭据

bool ApplicationDatabase::saveCredential(const CredentialRecord &record, QString *errorMessage)
{
    if (record.id.isEmpty() || record.name.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("凭据记录缺少 id 或名称");
        }
        return false;
    }
    // 这两项为空说明调用方绕过了 Vault：与其把疑似明文写进磁盘，不如让保存直接失败。
    if (record.encryptedPayload.isEmpty() || record.cipher.trimmed().isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("凭据必须携带密文与加密算法标识，禁止写入明文");
        }
        return false;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime createdAt = record.createdAt.isValid() ? record.createdAt : now;
    const QDateTime updatedAt = record.updatedAt.isValid() ? record.updatedAt : now;

    return runInTransaction([&](QString *innerError) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral(
            "INSERT INTO credentials (id, name, type, encrypted_payload, cipher, nonce, "
            "kdf_algorithm, kdf_salt, kdf_iterations, kdf_memory_kib, kdf_parallelism, "
            "created_at, updated_at) "
            "VALUES (:id, :name, :type, :payload, :cipher, :nonce, :kdf_algorithm, :kdf_salt, "
            ":kdf_iterations, :kdf_memory_kib, :kdf_parallelism, :created_at, :updated_at) "
            "ON CONFLICT(id) DO UPDATE SET name = excluded.name, type = excluded.type, "
            "encrypted_payload = excluded.encrypted_payload, cipher = excluded.cipher, "
            "nonce = excluded.nonce, kdf_algorithm = excluded.kdf_algorithm, "
            "kdf_salt = excluded.kdf_salt, kdf_iterations = excluded.kdf_iterations, "
            "kdf_memory_kib = excluded.kdf_memory_kib, kdf_parallelism = excluded.kdf_parallelism, "
            "updated_at = excluded.updated_at"));
        query.bindValue(QStringLiteral(":id"), record.id);
        query.bindValue(QStringLiteral(":name"), record.name);
        query.bindValue(QStringLiteral(":type"), static_cast<int>(record.type));
        query.bindValue(QStringLiteral(":payload"), record.encryptedPayload);
        query.bindValue(QStringLiteral(":cipher"), record.cipher);
        query.bindValue(QStringLiteral(":nonce"), record.nonce);
        query.bindValue(QStringLiteral(":kdf_algorithm"), record.kdfAlgorithm);
        query.bindValue(QStringLiteral(":kdf_salt"), record.kdfSalt);
        query.bindValue(QStringLiteral(":kdf_iterations"), record.kdfIterations);
        query.bindValue(QStringLiteral(":kdf_memory_kib"), record.kdfMemoryKib);
        query.bindValue(QStringLiteral(":kdf_parallelism"), record.kdfParallelism);
        query.bindValue(QStringLiteral(":created_at"), serializeTime(createdAt));
        query.bindValue(QStringLiteral(":updated_at"), serializeTime(updatedAt));
        if (!query.exec()) {
            reportQueryError(query, innerError);
            return false;
        }
        return true;
    }, errorMessage);
}

std::optional<CredentialRecord> ApplicationDatabase::findCredential(const QString &id,
                                                                    QString *errorMessage) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT * FROM credentials WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        reportQueryError(query, errorMessage);
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    return readCredentialRow(query);
}

bool ApplicationDatabase::removeCredential(const QString &id,
                                           int *removedCount,
                                           QString *errorMessage)
{
    if (removedCount != nullptr) {
        *removedCount = 0;
    }
    return runInTransaction([&](QString *innerError) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("DELETE FROM credentials WHERE id = :id"));
        query.bindValue(QStringLiteral(":id"), id);
        if (!query.exec()) {
            reportQueryError(query, innerError);
            return false;
        }
        if (removedCount != nullptr) {
            *removedCount = query.numRowsAffected();
        }
        return true;
    }, errorMessage);
}

QList<CredentialRecord> ApplicationDatabase::listCredentials(bool *ok, QString *errorMessage) const
{
    setFlag(ok, false);
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT * FROM credentials ORDER BY name"))) {
        reportQueryError(query, errorMessage);
        return {};
    }
    QList<CredentialRecord> records;
    while (query.next()) {
        records.append(readCredentialRow(query));
    }
    setFlag(ok, true);
    return records;
}

// -------------------------------------------------------------------- 连接历史

std::optional<qint64> ApplicationDatabase::appendConnectionHistory(const ConnectionHistoryEntry &entry,
                                                                   QString *errorMessage)
{
    const QDateTime connectedAt = entry.connectedAt.isValid() ? entry.connectedAt
                                                              : QDateTime::currentDateTimeUtc();
    std::optional<qint64> durationMs = entry.durationMs;
    if (!durationMs.has_value() && entry.disconnectedAt.has_value()
        && entry.disconnectedAt->isValid()) {
        durationMs = connectedAt.msecsTo(*entry.disconnectedAt);
    }

    qint64 insertedId = 0;
    const bool succeeded = runInTransaction([&](QString *innerError) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral(
            "INSERT INTO connection_history (host_id, connected_at, disconnected_at, duration_ms, "
            "result, error) VALUES (:host_id, :connected_at, :disconnected_at, :duration_ms, "
            ":result, :error)"));
        query.bindValue(QStringLiteral(":host_id"), nullableText(entry.hostId));
        query.bindValue(QStringLiteral(":connected_at"), serializeTime(connectedAt));
        query.bindValue(QStringLiteral(":disconnected_at"),
                        entry.disconnectedAt.has_value() ? serializeTime(*entry.disconnectedAt)
                                                         : QVariant());
        query.bindValue(QStringLiteral(":duration_ms"),
                        durationMs.has_value() ? QVariant(*durationMs) : QVariant());
        query.bindValue(QStringLiteral(":result"), entry.succeeded ? 1 : 0);
        query.bindValue(QStringLiteral(":error"), nullableText(entry.error));
        if (!query.exec()) {
            reportQueryError(query, innerError);
            return false;
        }
        insertedId = query.lastInsertId().toLongLong();
        return true;
    }, errorMessage);

    if (!succeeded) {
        return std::nullopt;
    }
    return insertedId;
}

bool ApplicationDatabase::finishConnectionHistory(const qint64 id,
                                                  const QDateTime &disconnectedAt,
                                                  bool *updated,
                                                  QString *errorMessage)
{
    setFlag(updated, false);
    if (!disconnectedAt.isValid()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("断开时间无效");
        }
        return false;
    }

    return runInTransaction([&](QString *innerError) {
        // 时长由数据库按已存的 connected_at 计算：调用方通常拿不到当初写入的那个时间点，
        // 让它自行相减会在跨进程重启后算出负数或荒谬的巨大值。
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral(
            "UPDATE connection_history SET disconnected_at = :disconnected_at, "
            "duration_ms = CAST((julianday(:duration_reference) - julianday(connected_at)) "
            "* 86400000.0 AS INTEGER) WHERE id = :id"));
        query.bindValue(QStringLiteral(":disconnected_at"), serializeTime(disconnectedAt));
        query.bindValue(QStringLiteral(":duration_reference"), serializeTime(disconnectedAt));
        query.bindValue(QStringLiteral(":id"), id);
        if (!query.exec()) {
            reportQueryError(query, innerError);
            return false;
        }
        setFlag(updated, query.numRowsAffected() > 0);
        return true;
    }, errorMessage);
}

QList<ConnectionHistoryEntry> ApplicationDatabase::recentConnections(const int limit,
                                                                     const QString &hostId,
                                                                     bool *ok,
                                                                     QString *errorMessage) const
{
    setFlag(ok, false);
    if (limit <= 0) {
        setFlag(ok, true);
        return {};
    }

    QSqlQuery query(m_database);
    // id 作为次级排序键：同一毫秒内的多条记录若只按时间排序，返回顺序取决于存储引擎，
    // “最近一条”会变得不稳定。
    if (hostId.isEmpty()) {
        query.prepare(QStringLiteral(
            "SELECT * FROM connection_history ORDER BY connected_at DESC, id DESC LIMIT :limit"));
    } else {
        query.prepare(QStringLiteral(
            "SELECT * FROM connection_history WHERE host_id = :host_id "
            "ORDER BY connected_at DESC, id DESC LIMIT :limit"));
        query.bindValue(QStringLiteral(":host_id"), hostId);
    }
    query.bindValue(QStringLiteral(":limit"), limit);
    if (!query.exec()) {
        reportQueryError(query, errorMessage);
        return {};
    }

    QList<ConnectionHistoryEntry> entries;
    while (query.next()) {
        ConnectionHistoryEntry entry;
        entry.id = query.value(QStringLiteral("id")).toLongLong();
        entry.hostId = query.value(QStringLiteral("host_id")).toString();
        entry.connectedAt = deserializeTime(query.value(QStringLiteral("connected_at")));
        const QVariant disconnectedAt = query.value(QStringLiteral("disconnected_at"));
        if (!disconnectedAt.isNull()) {
            entry.disconnectedAt = deserializeTime(disconnectedAt);
        }
        const QVariant durationMs = query.value(QStringLiteral("duration_ms"));
        if (!durationMs.isNull()) {
            entry.durationMs = durationMs.toLongLong();
        }
        entry.succeeded = query.value(QStringLiteral("result")).toInt() != 0;
        entry.error = query.value(QStringLiteral("error")).toString();
        entries.append(entry);
    }
    setFlag(ok, true);
    return entries;
}

// -------------------------------------------------------------------- 命令历史

std::optional<qint64> ApplicationDatabase::appendCommandHistory(const CommandHistoryEntry &entry,
                                                                QString *errorMessage)
{
    if (entry.command.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("命令历史记录不能为空");
        }
        return std::nullopt;
    }
    const QDateTime executedAt = entry.executedAt.isValid() ? entry.executedAt
                                                            : QDateTime::currentDateTimeUtc();

    qint64 insertedId = 0;
    const bool succeeded = runInTransaction([&](QString *innerError) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral(
            "INSERT INTO command_history (host_id, command, executed_at, exit_code) "
            "VALUES (:host_id, :command, :executed_at, :exit_code)"));
        query.bindValue(QStringLiteral(":host_id"), nullableText(entry.hostId));
        query.bindValue(QStringLiteral(":command"), entry.command);
        query.bindValue(QStringLiteral(":executed_at"), serializeTime(executedAt));
        query.bindValue(QStringLiteral(":exit_code"),
                        entry.exitCode.has_value() ? QVariant(*entry.exitCode) : QVariant());
        if (!query.exec()) {
            reportQueryError(query, innerError);
            return false;
        }
        insertedId = query.lastInsertId().toLongLong();
        return true;
    }, errorMessage);

    if (!succeeded) {
        return std::nullopt;
    }
    return insertedId;
}

QList<CommandHistoryEntry> ApplicationDatabase::recentCommands(const QString &hostId,
                                                               const int limit,
                                                               bool *ok,
                                                               QString *errorMessage) const
{
    setFlag(ok, false);
    if (limit <= 0) {
        setFlag(ok, true);
        return {};
    }

    QSqlQuery query(m_database);
    if (hostId.isEmpty()) {
        query.prepare(QStringLiteral(
            "SELECT * FROM command_history WHERE host_id IS NULL "
            "ORDER BY executed_at DESC, id DESC LIMIT :limit"));
    } else {
        query.prepare(QStringLiteral(
            "SELECT * FROM command_history WHERE host_id = :host_id "
            "ORDER BY executed_at DESC, id DESC LIMIT :limit"));
        query.bindValue(QStringLiteral(":host_id"), hostId);
    }
    query.bindValue(QStringLiteral(":limit"), limit);
    if (!query.exec()) {
        reportQueryError(query, errorMessage);
        return {};
    }

    QList<CommandHistoryEntry> entries;
    while (query.next()) {
        CommandHistoryEntry entry;
        entry.id = query.value(QStringLiteral("id")).toLongLong();
        entry.hostId = query.value(QStringLiteral("host_id")).toString();
        entry.command = query.value(QStringLiteral("command")).toString();
        entry.executedAt = deserializeTime(query.value(QStringLiteral("executed_at")));
        const QVariant exitCode = query.value(QStringLiteral("exit_code"));
        if (!exitCode.isNull()) {
            entry.exitCode = exitCode.toInt();
        }
        entries.append(entry);
    }
    setFlag(ok, true);
    return entries;
}

// -------------------------------------------------------------------- 设置项

bool ApplicationDatabase::setSetting(const QString &key, const QString &value, QString *errorMessage)
{
    if (key.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("设置项的键不能为空");
        }
        return false;
    }
    return runInTransaction([&](QString *innerError) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral(
            "INSERT INTO settings (key, value) VALUES (:key, :value) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
        query.bindValue(QStringLiteral(":key"), key);
        query.bindValue(QStringLiteral(":value"), value);
        if (!query.exec()) {
            reportQueryError(query, innerError);
            return false;
        }
        return true;
    }, errorMessage);
}

std::optional<QString> ApplicationDatabase::setting(const QString &key, QString *errorMessage) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key = :key"));
    query.bindValue(QStringLiteral(":key"), key);
    if (!query.exec()) {
        reportQueryError(query, errorMessage);
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    return query.value(0).toString();
}

bool ApplicationDatabase::removeSetting(const QString &key, QString *errorMessage)
{
    return runInTransaction([&](QString *innerError) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("DELETE FROM settings WHERE key = :key"));
        query.bindValue(QStringLiteral(":key"), key);
        if (!query.exec()) {
            reportQueryError(query, innerError);
            return false;
        }
        return true;
    }, errorMessage);
}

bool ApplicationDatabase::setBoolSetting(const QString &key, const bool value, QString *errorMessage)
{
    return setSetting(key, value ? QStringLiteral("1") : QStringLiteral("0"), errorMessage);
}

std::optional<bool> ApplicationDatabase::boolSetting(const QString &key, QString *errorMessage) const
{
    const std::optional<QString> raw = setting(key, errorMessage);
    if (!raw.has_value()) {
        return std::nullopt;
    }
    const QString normalized = raw->trimmed().toLower();
    if (normalized == QLatin1String("1") || normalized == QLatin1String("true")) {
        return true;
    }
    if (normalized == QLatin1String("0") || normalized == QLatin1String("false")) {
        return false;
    }
    // 存了别的内容说明数据被外部改坏。默默当成 false 会静默关掉用户开启过的功能，
    // 因此当作错误报告出去，由上层决定是提示还是回退默认值。
    if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("设置项 %1 的值 %2 不是布尔值").arg(key, *raw);
    }
    return std::nullopt;
}

bool ApplicationDatabase::setIntSetting(const QString &key, const int value, QString *errorMessage)
{
    return setSetting(key, QString::number(value), errorMessage);
}

std::optional<int> ApplicationDatabase::intSetting(const QString &key, QString *errorMessage) const
{
    const std::optional<QString> raw = setting(key, errorMessage);
    if (!raw.has_value()) {
        return std::nullopt;
    }
    bool converted = false;
    const int value = raw->trimmed().toInt(&converted);
    if (!converted) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("设置项 %1 的值 %2 不是整数").arg(key, *raw);
        }
        return std::nullopt;
    }
    return value;
}
