#include "storage/ApplicationDatabase.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <QVariant>

namespace {

/// 当前代码要求的 schema 版本。每次结构变更必须递增，并在 upgradeSchema 中补齐对应分支。
constexpr int kSchemaVersion = 6;

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

/**
 * @brief 从规范化关联表读取一台主机的有序前置链路。
 * @param database 当前 ApplicationDatabase 持有的连接。
 * @param hostId 链路所属的目标主机 id。
 * @param chain 成功时接收按 hop_index 排序的跳板机 id。
 * @param errorMessage 查询失败原因；可为空。
 * @return 查询成功返回 true，空链同样属于成功。
 */
bool readManagedHostChain(const QSqlDatabase &database,
                          const QString &hostId,
                          QStringList *chain,
                          QString *errorMessage)
{
    Q_ASSERT(chain != nullptr);
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT jump_host_id FROM host_chain_members "
        "WHERE owner_host_id = :owner_host_id ORDER BY hop_index"));
    query.bindValue(QStringLiteral(":owner_host_id"), hostId);
    if (!query.exec()) {
        reportQueryError(query, errorMessage);
        return false;
    }
    QStringList result;
    while (query.next()) {
        result.append(query.value(0).toString());
    }
    *chain = result;
    return true;
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

/**
 * @brief 将 snippets 查询的当前行转换为强类型记录。
 *
 * 旧 schema 没有单独的 kind 列，而是用 command=NULL 表示分组。转换集中在这里可以避免
 * 各查询接口分别猜测节点类型，也为未来迁移到显式 kind 列保留单一修改点。
 */
SnippetRecord readSnippetRow(const QSqlQuery &query)
{
    SnippetRecord record;
    record.id = query.value(QStringLiteral("id")).toString();
    record.parentId = query.value(QStringLiteral("parent_id")).toString();
    record.name = query.value(QStringLiteral("name")).toString();
    const QVariant command = query.value(QStringLiteral("command"));
    record.kind = command.isNull() ? SnippetNodeKind::Folder : SnippetNodeKind::Snippet;
    record.command = command.toString();
    record.description = query.value(QStringLiteral("description")).toString();
    record.sortOrder = query.value(QStringLiteral("sort_order")).toInt();
    record.updatedAt = deserializeTime(query.value(QStringLiteral("updated_at")));
    return record;
}

/** @brief 把 port_forwards 查询当前行转换为领域模型。 */
PortForwardRule readPortForwardRow(const QSqlQuery &query)
{
    PortForwardRule rule;
    rule.id = query.value(QStringLiteral("id")).toString();
    rule.hostId = query.value(QStringLiteral("host_id")).toString();
    rule.name = query.value(QStringLiteral("name")).toString();
    rule.type = static_cast<PortForwardType>(query.value(QStringLiteral("type")).toInt());
    rule.bindHost = query.value(QStringLiteral("bind_host")).toString();
    rule.bindPort = static_cast<quint16>(query.value(QStringLiteral("bind_port")).toUInt());
    rule.targetHost = query.value(QStringLiteral("target_host")).toString();
    rule.targetPort = static_cast<quint16>(query.value(QStringLiteral("target_port")).toUInt());
    rule.autoStart = query.value(QStringLiteral("auto_start")).toInt() != 0;
    return rule;
}

/** @brief 把 workspaces 查询当前行转换为不解释布局内容的持久化记录。 */
WorkspaceRecord readWorkspaceRow(const QSqlQuery &query)
{
    WorkspaceRecord record;
    record.id = query.value(QStringLiteral("id")).toString();
    record.name = query.value(QStringLiteral("name")).toString();
    record.layoutJson = query.value(QStringLiteral("layout_json")).toString();
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
        // 主机链使用独立关联表保存稳定引用和顺序。jump_host_id 采用 RESTRICT：若仍有目标
        // 依赖一台跳板机，删除操作必须明确失败，不能静默把内网主机退化成本机直连。
        QStringLiteral("CREATE TABLE IF NOT EXISTS host_chain_members ("
                       "owner_host_id TEXT NOT NULL REFERENCES hosts(id) ON DELETE CASCADE, "
                       "hop_index INTEGER NOT NULL CHECK(hop_index >= 0), "
                       "jump_host_id TEXT NOT NULL REFERENCES hosts(id) ON DELETE RESTRICT, "
                       "PRIMARY KEY(owner_host_id, hop_index), "
                       "UNIQUE(owner_host_id, jump_host_id))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_host_chain_jump "
                       "ON host_chain_members(jump_host_id)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS credentials ("
                       "id TEXT PRIMARY KEY, name TEXT NOT NULL, type INTEGER NOT NULL, "
                       "encrypted_payload BLOB NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS snippets ("
                       "id TEXT PRIMARY KEY, parent_id TEXT, name TEXT NOT NULL, command TEXT, "
                       "description TEXT, sort_order INTEGER NOT NULL DEFAULT 0, updated_at TEXT NOT NULL)"),
        // 父分组列表是 Snippet 侧栏最频繁的查询；组合索引同时覆盖父级筛选和稳定排序。
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_snippets_parent "
                       "ON snippets(parent_id, sort_order, name)"),
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
    if (*currentVersion < 3) {
        // 版本 3：新增全局保险库公开配置。固定 id=1 并通过 CHECK 约束保证表中永远只有
        // 一行，避免两个不同盐值同时存在时调用方无法判断应该用哪一个派生密钥。
        statements << QStringLiteral(
            "CREATE TABLE vault_metadata ("
            "id INTEGER PRIMARY KEY CHECK(id = 1), "
            "format_version INTEGER NOT NULL, kdf_algorithm TEXT NOT NULL, "
            "salt BLOB NOT NULL, verifier BLOB NOT NULL, "
            "created_at TEXT NOT NULL, updated_at TEXT NOT NULL)");
    }
    if (*currentVersion < 4) {
        // 版本 4：旧 port_forwards 只是占位表，没有外键和字段约束。SQLite 不能直接为既有
        // 表补外键，因此在同一事务中重建；只迁移仍能匹配 hosts 的合法规则，孤儿占位数据
        // 不应在连接后被误启动。字段 CHECK 把损坏尽量拦在数据库边界。
        statements << QStringLiteral("ALTER TABLE port_forwards RENAME TO port_forwards_v3")
                   << QStringLiteral(
                          "CREATE TABLE port_forwards ("
                          "id TEXT PRIMARY KEY, "
                          "host_id TEXT NOT NULL REFERENCES hosts(id) ON DELETE CASCADE, "
                          "name TEXT NOT NULL, type INTEGER NOT NULL CHECK(type BETWEEN 0 AND 2), "
                          "bind_host TEXT NOT NULL, bind_port INTEGER NOT NULL CHECK(bind_port BETWEEN 1 AND 65535), "
                          "target_host TEXT, target_port INTEGER CHECK(target_port BETWEEN 0 AND 65535), "
                          "auto_start INTEGER NOT NULL DEFAULT 0 CHECK(auto_start IN (0, 1)))")
                   << QStringLiteral(
                          "INSERT INTO port_forwards(id, host_id, name, type, bind_host, bind_port, "
                          "target_host, target_port, auto_start) "
                          "SELECT p.id, p.host_id, p.name, p.type, COALESCE(NULLIF(p.bind_host, ''), '127.0.0.1'), "
                          "p.bind_port, p.target_host, COALESCE(p.target_port, 0), p.auto_start "
                          "FROM port_forwards_v3 p JOIN hosts h ON h.id = p.host_id "
                          "WHERE p.type BETWEEN 0 AND 2 AND p.bind_port BETWEEN 1 AND 65535 "
                          "AND (p.type = 2 OR (p.target_host IS NOT NULL AND p.target_host <> '' "
                          "AND p.target_port BETWEEN 1 AND 65535))")
                   << QStringLiteral("DROP TABLE port_forwards_v3")
                   << QStringLiteral("CREATE INDEX idx_port_forwards_host "
                                     "ON port_forwards(host_id, name)");
    }
    if (*currentVersion < 5) {
        // 版本 5：把受管主机链规范化保存。旧版本的 proxy_jump 是自由文本，无法安全映射到
        // 本机主机 id，因此原样保留在 hosts.proxy_jump 中，不进行猜测式自动转换。
        statements << QStringLiteral(
                          "CREATE TABLE IF NOT EXISTS host_chain_members ("
                          "owner_host_id TEXT NOT NULL REFERENCES hosts(id) ON DELETE CASCADE, "
                          "hop_index INTEGER NOT NULL CHECK(hop_index >= 0), "
                          "jump_host_id TEXT NOT NULL REFERENCES hosts(id) ON DELETE RESTRICT, "
                          "PRIMARY KEY(owner_host_id, hop_index), "
                          "UNIQUE(owner_host_id, jump_host_id))")
                   << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_host_chain_jump "
                                     "ON host_chain_members(jump_host_id)");
    }
    if (*currentVersion < 6) {
        // 版本 6：历史“复制主机”会原样复制 credential_id，导致若干主机共同更新同一条密文。
        // 临时映射表为除每组第一条之外的每个直接引用生成独立 id，再复制完全相同的密文和
        // 元数据。全过程不解密凭据，也不会改变任一主机当前密码；任一步失败由外层事务整体
        // 回滚。父分组继承不会在 hosts 子记录上写 credential_id，因此不会被误拆分。
        statements << QStringLiteral(
                          "CREATE TEMP TABLE credential_reference_splits ("
                          "host_id TEXT PRIMARY KEY, old_credential_id TEXT NOT NULL, "
                          "new_credential_id TEXT NOT NULL UNIQUE)")
                   << QStringLiteral(
                          "INSERT INTO credential_reference_splits(host_id, old_credential_id, "
                          "new_credential_id) "
                          "SELECT h.id, h.credential_id, "
                          "'credential-' || lower(hex(randomblob(16))) "
                          "FROM hosts h JOIN credentials c ON c.id = h.credential_id "
                          "WHERE h.credential_id IS NOT NULL AND h.credential_id <> '' "
                          "AND h.id <> (SELECT MIN(first_host.id) FROM hosts first_host "
                          "WHERE first_host.credential_id = h.credential_id)")
                   << QStringLiteral(
                          "INSERT INTO credentials(id, name, type, encrypted_payload, cipher, nonce, "
                          "kdf_algorithm, kdf_salt, kdf_iterations, kdf_memory_kib, kdf_parallelism, "
                          "created_at, updated_at) "
                          "SELECT splits.new_credential_id, credentials.name, credentials.type, "
                          "credentials.encrypted_payload, credentials.cipher, credentials.nonce, "
                          "credentials.kdf_algorithm, credentials.kdf_salt, "
                          "credentials.kdf_iterations, credentials.kdf_memory_kib, "
                          "credentials.kdf_parallelism, credentials.created_at, credentials.updated_at "
                          "FROM credential_reference_splits splits "
                          "JOIN credentials ON credentials.id = splits.old_credential_id")
                   << QStringLiteral(
                          "UPDATE hosts SET credential_id = ("
                          "SELECT splits.new_credential_id FROM credential_reference_splits splits "
                          "WHERE splits.host_id = hosts.id) "
                          "WHERE id IN (SELECT host_id FROM credential_reference_splits)")
                   << QStringLiteral("DROP TABLE credential_reference_splits")
                   << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_hosts_credential "
                                     "ON hosts(credential_id)");
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
    if (record.kind != HostNodeKind::Host && !record.hostChain.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("分组不能配置主机链");
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

        // 链路与主机记录必须原子更新。先清空旧成员再写入新顺序，任一引用无效时外层事务
        // 会整体回滚，数据库不会留下 mode 已更新而链路只有一半的中间状态。
        QSqlQuery clearChain(m_database);
        clearChain.prepare(QStringLiteral(
            "DELETE FROM host_chain_members WHERE owner_host_id = :owner_host_id"));
        clearChain.bindValue(QStringLiteral(":owner_host_id"), record.id);
        if (!clearChain.exec()) {
            reportQueryError(clearChain, innerError);
            return false;
        }

        QSet<QString> uniqueJumpIds;
        for (qsizetype index = 0; index < record.hostChain.size(); ++index) {
            const QString jumpId = record.hostChain.at(index).trimmed();
            if (jumpId.isEmpty() || jumpId == record.id || uniqueJumpIds.contains(jumpId)) {
                *innerError = jumpId == record.id
                    ? QStringLiteral("主机链不能包含目标主机自身")
                    : QStringLiteral("主机链包含空引用或重复主机：%1").arg(jumpId);
                return false;
            }
            uniqueJumpIds.insert(jumpId);

            QSqlQuery verifyJump(m_database);
            verifyJump.prepare(QStringLiteral("SELECT kind FROM hosts WHERE id = :id"));
            verifyJump.bindValue(QStringLiteral(":id"), jumpId);
            if (!verifyJump.exec()) {
                reportQueryError(verifyJump, innerError);
                return false;
            }
            if (!verifyJump.next()) {
                *innerError = QStringLiteral("主机链引用了不存在的主机：%1").arg(jumpId);
                return false;
            }
            if (verifyJump.value(0).toInt() != static_cast<int>(HostNodeKind::Host)) {
                *innerError = QStringLiteral("主机链不能使用分组节点：%1").arg(jumpId);
                return false;
            }

            QSqlQuery insertHop(m_database);
            insertHop.prepare(QStringLiteral(
                "INSERT INTO host_chain_members(owner_host_id, hop_index, jump_host_id) "
                "VALUES (:owner_host_id, :hop_index, :jump_host_id)"));
            insertHop.bindValue(QStringLiteral(":owner_host_id"), record.id);
            insertHop.bindValue(QStringLiteral(":hop_index"), index);
            insertHop.bindValue(QStringLiteral(":jump_host_id"), jumpId);
            if (!insertHop.exec()) {
                reportQueryError(insertHop, innerError);
                return false;
            }
        }

        // 写入后通过与正式连接完全相同的解析器校验跨主机环路。把校验放在事务内可让一次
        // A→B、B→A 的编辑安全回滚，而不是把损坏图留给下一次连接才发现。
        QString chainError;
        if (record.kind == HostNodeKind::Host
            && resolveConnectionChain(record.id, &chainError).isEmpty()) {
            *innerError = chainError.isEmpty() ? QStringLiteral("无法解析主机链") : chainError;
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
    HostRecord record = readHostRow(query);
    if (!readManagedHostChain(m_database, record.id, &record.hostChain, errorMessage)) {
        return std::nullopt;
    }
    return record;
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
    for (HostRecord &record : records) {
        if (!readManagedHostChain(m_database, record.id, &record.hostChain, errorMessage)) {
            return {};
        }
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
    for (HostRecord &record : records) {
        if (!readManagedHostChain(m_database, record.id, &record.hostChain, errorMessage)) {
            return {};
        }
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
    for (HostRecord &record : records) {
        if (!readManagedHostChain(m_database, record.id, &record.hostChain, errorMessage)) {
            return {};
        }
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

QList<HostRecord> ApplicationDatabase::resolveConnectionChain(
    const QString &id, QString *errorMessage) const
{
    QList<HostRecord> plan;
    QSet<QString> activePath;
    QSet<QString> appended;

    // 链路成员可以复用另一台主机已经配置的前置链，因此解析必须递归。activePath 用于给
    // 真正的环路提供精确错误，appended 则拒绝从不同分支重复抵达同一主机的歧义路径。
    std::function<bool(const QString &)> appendHost = [&](const QString &hostId) {
        if (activePath.contains(hostId)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("主机链存在环路，重复进入主机：%1").arg(hostId);
            }
            return false;
        }
        if (appended.contains(hostId)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("主机链重复经过同一主机：%1").arg(hostId);
            }
            return false;
        }

        QString lookupError;
        const std::optional<HostRecord> raw = findHost(hostId, &lookupError);
        if (!raw.has_value()) {
            if (errorMessage != nullptr) {
                *errorMessage = lookupError.isEmpty()
                    ? QStringLiteral("主机链引用不存在：%1").arg(hostId)
                    : lookupError;
            }
            return false;
        }
        if (raw->kind != HostNodeKind::Host) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("主机链节点不是可连接主机：%1").arg(raw->name);
            }
            return false;
        }

        activePath.insert(hostId);
        for (const QString &jumpId : raw->hostChain) {
            if (!appendHost(jumpId)) {
                return false;
            }
        }
        activePath.remove(hostId);

        const std::optional<HostRecord> resolved = resolveHost(hostId, &lookupError);
        if (!resolved.has_value()) {
            if (errorMessage != nullptr) {
                *errorMessage = lookupError.isEmpty()
                    ? QStringLiteral("无法解析主机配置：%1").arg(hostId)
                    : lookupError;
            }
            return false;
        }
        HostRecord hop = *resolved;
        // 返回值已经是扁平的实际连接顺序。清掉结构字段可防止调用方误把它再次展开。
        hop.hostChain.clear();
        plan.append(hop);
        appended.insert(hostId);
        return true;
    };

    if (!appendHost(id)) {
        return {};
    }
    return plan;
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
    // 仅检查 payload 非空无法阻止调用方把口令原文塞进 BLOB。数据库层虽然不能自行验证
    // AEAD 标签，但可以强制要求当前 XChaCha20/Argon2 封装的全部公开元数据和最小长度，
    // 把最常见的“绕过 Vault 直接保存”错误挡在落盘之前。
    constexpr qsizetype xChaChaNonceBytes = 24;
    constexpr qsizetype poly1305TagBytes = 16;
    constexpr qsizetype argon2SaltBytes = 16;
    const bool supportedCipher = record.cipher == QStringLiteral("xchacha20-poly1305-v1");
    const bool passwordKdf = record.kdfAlgorithm == QStringLiteral("argon2id13");
    const bool localKeyKdf = record.kdfAlgorithm == QStringLiteral("local-key-v1");
    if (record.encryptedPayload.size() < poly1305TagBytes || !supportedCipher
        || record.nonce.size() != xChaChaNonceBytes || (!passwordKdf && !localKeyKdf)
        || record.kdfSalt.size() != argon2SaltBytes || record.kdfIterations <= 0
        || (passwordKdf && record.kdfMemoryKib <= 0)
        || (localKeyKdf && record.kdfMemoryKib != 0) || record.kdfParallelism <= 0) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral(
                "凭据必须携带完整的 XChaCha20-Poly1305 与受支持密钥元数据，禁止写入明文");
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

int ApplicationDatabase::credentialReferenceCount(const QString &credentialId,
                                                  bool *ok,
                                                  QString *errorMessage) const
{
    setFlag(ok, false);
    if (credentialId.trimmed().isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("统计凭据引用时 credentialId 不能为空");
        }
        return 0;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM hosts WHERE credential_id = :credential_id"));
    query.bindValue(QStringLiteral(":credential_id"), credentialId);
    if (!query.exec() || !query.next()) {
        reportQueryError(query, errorMessage);
        return 0;
    }
    setFlag(ok, true);
    return query.value(0).toInt();
}

// ---------------------------------------------------------------- 保险库公开配置

bool ApplicationDatabase::saveVaultConfiguration(const VaultConfiguration &configuration,
                                                  QString *errorMessage)
{
    if (configuration.formatVersion != 1) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("不支持的保险库格式版本：%1")
                                .arg(configuration.formatVersion);
        }
        return false;
    }
    if (configuration.kdfAlgorithm.trimmed().isEmpty() || configuration.salt.isEmpty()
        || configuration.verifier.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("保险库配置缺少 KDF、盐值或主密码校验块");
        }
        return false;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime createdAt = configuration.createdAt.isValid() ? configuration.createdAt : now;
    const QDateTime updatedAt = configuration.updatedAt.isValid() ? configuration.updatedAt : now;
    return runInTransaction([&](QString *innerError) {
        QSqlQuery query(m_database);
        // upsert 仅用于以后安全迁移 KDF 参数；日常首次创建流程会在服务层拒绝覆盖已有配置。
        query.prepare(QStringLiteral(
            "INSERT INTO vault_metadata (id, format_version, kdf_algorithm, salt, verifier, "
            "created_at, updated_at) VALUES (1, :format_version, :kdf_algorithm, :salt, "
            ":verifier, :created_at, :updated_at) "
            "ON CONFLICT(id) DO UPDATE SET format_version = excluded.format_version, "
            "kdf_algorithm = excluded.kdf_algorithm, salt = excluded.salt, "
            "verifier = excluded.verifier, updated_at = excluded.updated_at"));
        query.bindValue(QStringLiteral(":format_version"), configuration.formatVersion);
        query.bindValue(QStringLiteral(":kdf_algorithm"), configuration.kdfAlgorithm);
        query.bindValue(QStringLiteral(":salt"), configuration.salt);
        query.bindValue(QStringLiteral(":verifier"), configuration.verifier);
        query.bindValue(QStringLiteral(":created_at"), serializeTime(createdAt));
        query.bindValue(QStringLiteral(":updated_at"), serializeTime(updatedAt));
        if (!query.exec()) {
            reportQueryError(query, innerError);
            return false;
        }
        return true;
    }, errorMessage);
}

bool ApplicationDatabase::removeVaultConfiguration(QString *errorMessage)
{
    return runInTransaction([&](QString *innerError) {
        QSqlQuery query(m_database);
        // vault_metadata 通过固定主键保证最多一行。这里仍按 id 精确删除，而不是无条件清表，
        // 既表达数据模型约束，也避免未来表中加入其他格式元数据后被旧客户端误删。
        query.prepare(QStringLiteral("DELETE FROM vault_metadata WHERE id = 1"));
        if (!query.exec()) {
            reportQueryError(query, innerError);
            return false;
        }
        return true;
    }, errorMessage);
}

std::optional<VaultConfiguration>
ApplicationDatabase::vaultConfiguration(QString *errorMessage) const
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT * FROM vault_metadata WHERE id = 1"))) {
        reportQueryError(query, errorMessage);
        return std::nullopt;
    }
    if (!query.next()) {
        // 没有记录是正常的首次启动状态，不能把它报告为数据库错误。
        return std::nullopt;
    }

    VaultConfiguration configuration;
    configuration.formatVersion = query.value(QStringLiteral("format_version")).toInt();
    configuration.kdfAlgorithm = query.value(QStringLiteral("kdf_algorithm")).toString();
    configuration.salt = query.value(QStringLiteral("salt")).toByteArray();
    configuration.verifier = query.value(QStringLiteral("verifier")).toByteArray();
    configuration.createdAt = deserializeTime(query.value(QStringLiteral("created_at")));
    configuration.updatedAt = deserializeTime(query.value(QStringLiteral("updated_at")));

    // 这里做结构完整性校验，而不是把空字段交给 Vault。否则上层无法区分“主密码错误”和
    // “磁盘上的初始化数据被截断”，也可能错误地向用户反复索要一个永远不可能成功的密码。
    if (configuration.formatVersion != 1 || configuration.kdfAlgorithm.trimmed().isEmpty()
        || configuration.salt.isEmpty() || configuration.verifier.isEmpty()
        || !configuration.createdAt.isValid() || !configuration.updatedAt.isValid()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("保险库公开配置不完整或格式版本不受支持");
        }
        return std::nullopt;
    }
    return configuration;
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

// -------------------------------------------------------------------- Snippets

bool ApplicationDatabase::saveSnippet(const SnippetRecord &record, QString *errorMessage)
{
    const QString normalizedName = record.name.trimmed();
    if (record.id.trimmed().isEmpty() || normalizedName.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Snippet 的 id 和名称不能为空");
        }
        return false;
    }
    if (record.kind == SnippetNodeKind::Snippet && record.command.trimmed().isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("可执行 Snippet 的命令不能为空");
        }
        return false;
    }
    if (record.parentId == record.id) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Snippet 分组不能以自身作为父分组");
        }
        return false;
    }

    return runInTransaction([&](QString *innerError) {
        if (!record.parentId.isEmpty()) {
            QSqlQuery parentQuery(m_database);
            parentQuery.prepare(QStringLiteral("SELECT command FROM snippets WHERE id = :id"));
            parentQuery.bindValue(QStringLiteral(":id"), record.parentId);
            if (!parentQuery.exec()) {
                reportQueryError(parentQuery, innerError);
                return false;
            }
            if (!parentQuery.next()) {
                *innerError = QStringLiteral("父 Snippet 分组不存在：%1").arg(record.parentId);
                return false;
            }
            if (!parentQuery.value(0).isNull()) {
                *innerError = QStringLiteral("Snippet 只能放入分组，不能放入另一条可执行片段");
                return false;
            }

            // 从候选父节点一路向上找祖先。若能遇到当前记录，说明本次移动会形成环；使用
            // UNION 去重而非 UNION ALL，使已被旧版本数据污染的环也不会让查询无限递归。
            QSqlQuery cycleQuery(m_database);
            cycleQuery.prepare(QStringLiteral(
                "WITH RECURSIVE ancestors(id, parent_id) AS ("
                " SELECT id, parent_id FROM snippets WHERE id = :parent_id"
                " UNION SELECT s.id, s.parent_id FROM snippets s"
                " JOIN ancestors a ON s.id = a.parent_id"
                ") SELECT 1 FROM ancestors WHERE id = :id LIMIT 1"));
            cycleQuery.bindValue(QStringLiteral(":parent_id"), record.parentId);
            cycleQuery.bindValue(QStringLiteral(":id"), record.id);
            if (!cycleQuery.exec()) {
                reportQueryError(cycleQuery, innerError);
                return false;
            }
            if (cycleQuery.next()) {
                *innerError = QStringLiteral("不能把 Snippet 分组移动到自己的后代中");
                return false;
            }
        }

        if (record.kind == SnippetNodeKind::Snippet) {
            // 有子节点的分组不能直接改成片段，否则已有子节点会指向一个不可容纳子节点的
            // 叶子。要求用户先移动或删除子节点，避免树结构静默损坏。
            QSqlQuery childrenQuery(m_database);
            childrenQuery.prepare(
                QStringLiteral("SELECT 1 FROM snippets WHERE parent_id = :id LIMIT 1"));
            childrenQuery.bindValue(QStringLiteral(":id"), record.id);
            if (!childrenQuery.exec()) {
                reportQueryError(childrenQuery, innerError);
                return false;
            }
            if (childrenQuery.next()) {
                *innerError = QStringLiteral("含有子节点的分组不能直接改为可执行 Snippet");
                return false;
            }
        }

        const QDateTime updatedAt = record.updatedAt.isValid()
            ? record.updatedAt
            : QDateTime::currentDateTimeUtc();
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral(
            "INSERT INTO snippets "
            "(id, parent_id, name, command, description, sort_order, updated_at) "
            "VALUES (:id, :parent_id, :name, :command, :description, :sort_order, :updated_at) "
            "ON CONFLICT(id) DO UPDATE SET "
            "parent_id = excluded.parent_id, name = excluded.name, command = excluded.command, "
            "description = excluded.description, sort_order = excluded.sort_order, "
            "updated_at = excluded.updated_at"));
        query.bindValue(QStringLiteral(":id"), record.id);
        query.bindValue(QStringLiteral(":parent_id"), nullableText(record.parentId));
        query.bindValue(QStringLiteral(":name"), normalizedName);
        query.bindValue(QStringLiteral(":command"),
                        record.kind == SnippetNodeKind::Folder
                            ? QVariant()
                            : QVariant(record.command));
        query.bindValue(QStringLiteral(":description"), nullableText(record.description));
        query.bindValue(QStringLiteral(":sort_order"), record.sortOrder);
        query.bindValue(QStringLiteral(":updated_at"), serializeTime(updatedAt));
        if (!query.exec()) {
            reportQueryError(query, innerError);
            return false;
        }
        return true;
    }, errorMessage);
}

std::optional<SnippetRecord> ApplicationDatabase::findSnippet(const QString &id,
                                                               QString *errorMessage) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT * FROM snippets WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        reportQueryError(query, errorMessage);
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    return readSnippetRow(query);
}

QList<SnippetRecord> ApplicationDatabase::listSnippetsInGroup(const QString &parentId,
                                                              bool *ok,
                                                              QString *errorMessage) const
{
    setFlag(ok, false);
    QSqlQuery query(m_database);
    if (parentId.isEmpty()) {
        query.prepare(QStringLiteral(
            "SELECT * FROM snippets WHERE parent_id IS NULL ORDER BY sort_order, name, id"));
    } else {
        query.prepare(QStringLiteral(
            "SELECT * FROM snippets WHERE parent_id = :parent_id ORDER BY sort_order, name, id"));
        query.bindValue(QStringLiteral(":parent_id"), parentId);
    }
    if (!query.exec()) {
        reportQueryError(query, errorMessage);
        return {};
    }

    QList<SnippetRecord> records;
    while (query.next()) {
        records.append(readSnippetRow(query));
    }
    setFlag(ok, true);
    return records;
}

bool ApplicationDatabase::removeSnippet(const QString &id,
                                        int *removedCount,
                                        QString *errorMessage)
{
    if (removedCount != nullptr) {
        *removedCount = 0;
    }
    if (id.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("待删除 Snippet 的 id 不能为空");
        }
        return false;
    }

    int count = 0;
    const bool succeeded = runInTransaction([&](QString *innerError) {
        // 先在同一事务里统计，再递归删除。DELETE 的 numRowsAffected 在不同 SQLite 驱动
        // 版本对 WITH RECURSIVE 的返回不一致，显式 COUNT 才能稳定报告删除数量。
        const QString descendantsCte = QStringLiteral(
            "WITH RECURSIVE descendants(id) AS ("
            " SELECT id FROM snippets WHERE id = :id"
            " UNION SELECT s.id FROM snippets s JOIN descendants d ON s.parent_id = d.id"
            ") ");
        QSqlQuery countQuery(m_database);
        countQuery.prepare(descendantsCte
                           + QStringLiteral("SELECT COUNT(*) FROM descendants"));
        countQuery.bindValue(QStringLiteral(":id"), id);
        if (!countQuery.exec() || !countQuery.next()) {
            reportQueryError(countQuery, innerError);
            return false;
        }
        count = countQuery.value(0).toInt();

        QSqlQuery deleteQuery(m_database);
        deleteQuery.prepare(descendantsCte
                            + QStringLiteral("DELETE FROM snippets WHERE id IN "
                                             "(SELECT id FROM descendants)"));
        deleteQuery.bindValue(QStringLiteral(":id"), id);
        if (!deleteQuery.exec()) {
            reportQueryError(deleteQuery, innerError);
            return false;
        }
        return true;
    }, errorMessage);

    if (succeeded && removedCount != nullptr) {
        *removedCount = count;
    }
    return succeeded;
}

// -------------------------------------------------------------------- 端口转发规则

bool ApplicationDatabase::savePortForward(const PortForwardRule &rule, QString *errorMessage)
{
    QString validationError;
    if (!rule.isValid(&validationError)) {
        if (errorMessage != nullptr) {
            *errorMessage = validationError;
        }
        return false;
    }

    return runInTransaction([&](QString *innerError) {
        // 除外键存在性外还要拒绝分组节点：分组没有可建立 SSH 会话的地址，给它保存规则只会
        // 形成永远无法触发的配置。校验放在同一事务内，避免查询后主机恰好被并发删除。
        QSqlQuery hostQuery(m_database);
        hostQuery.prepare(QStringLiteral("SELECT kind FROM hosts WHERE id = :host_id"));
        hostQuery.bindValue(QStringLiteral(":host_id"), rule.hostId);
        if (!hostQuery.exec()) {
            reportQueryError(hostQuery, innerError);
            return false;
        }
        if (!hostQuery.next()) {
            *innerError = QStringLiteral("端口转发绑定的主机不存在：%1").arg(rule.hostId);
            return false;
        }
        if (hostQuery.value(0).toInt() != static_cast<int>(HostNodeKind::Host)) {
            *innerError = QStringLiteral("端口转发不能绑定到主机分组");
            return false;
        }

        QSqlQuery query(m_database);
        query.prepare(QStringLiteral(
            "INSERT INTO port_forwards "
            "(id, host_id, name, type, bind_host, bind_port, target_host, target_port, auto_start) "
            "VALUES (:id, :host_id, :name, :type, :bind_host, :bind_port, :target_host, "
            ":target_port, :auto_start) "
            "ON CONFLICT(id) DO UPDATE SET host_id = excluded.host_id, name = excluded.name, "
            "type = excluded.type, bind_host = excluded.bind_host, bind_port = excluded.bind_port, "
            "target_host = excluded.target_host, target_port = excluded.target_port, "
            "auto_start = excluded.auto_start"));
        query.bindValue(QStringLiteral(":id"), rule.id);
        query.bindValue(QStringLiteral(":host_id"), rule.hostId);
        query.bindValue(QStringLiteral(":name"), rule.name.trimmed());
        query.bindValue(QStringLiteral(":type"), static_cast<int>(rule.type));
        query.bindValue(QStringLiteral(":bind_host"), rule.bindHost.trimmed());
        query.bindValue(QStringLiteral(":bind_port"), rule.bindPort);
        query.bindValue(QStringLiteral(":target_host"),
                        rule.type == PortForwardType::Dynamic
                            ? QVariant()
                            : QVariant(rule.targetHost.trimmed()));
        query.bindValue(QStringLiteral(":target_port"),
                        rule.type == PortForwardType::Dynamic
                            ? QVariant(0)
                            : QVariant(rule.targetPort));
        query.bindValue(QStringLiteral(":auto_start"), rule.autoStart ? 1 : 0);
        if (!query.exec()) {
            reportQueryError(query, innerError);
            return false;
        }
        return true;
    }, errorMessage);
}

std::optional<PortForwardRule>
ApplicationDatabase::findPortForward(const QString &id, QString *errorMessage) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT * FROM port_forwards WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        reportQueryError(query, errorMessage);
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    return readPortForwardRow(query);
}

QList<PortForwardRule> ApplicationDatabase::listPortForwards(const QString &hostId,
                                                             bool *ok,
                                                             QString *errorMessage) const
{
    setFlag(ok, false);
    if (hostId.trimmed().isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("列出端口转发时 hostId 不能为空");
        }
        return {};
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT * FROM port_forwards WHERE host_id = :host_id "
        "ORDER BY name COLLATE NOCASE, id"));
    query.bindValue(QStringLiteral(":host_id"), hostId);
    if (!query.exec()) {
        reportQueryError(query, errorMessage);
        return {};
    }
    QList<PortForwardRule> rules;
    while (query.next()) {
        rules.append(readPortForwardRow(query));
    }
    setFlag(ok, true);
    return rules;
}

bool ApplicationDatabase::removePortForward(const QString &id,
                                            int *removedCount,
                                            QString *errorMessage)
{
    return runInTransaction([&](QString *innerError) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("DELETE FROM port_forwards WHERE id = :id"));
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

// ---------------------------------------------------------------------- 工作区

bool ApplicationDatabase::saveWorkspace(const WorkspaceRecord &record,
                                         QString *errorMessage)
{
    const QString id = record.id.trimmed();
    const QString name = record.name.trimmed();
    QJsonParseError parseError;
    const QJsonDocument layout = QJsonDocument::fromJson(record.layoutJson.toUtf8(), &parseError);
    if (id.isEmpty() || name.isEmpty() || layout.isNull()) {
        if (errorMessage != nullptr) {
            *errorMessage = layout.isNull() && !record.layoutJson.trimmed().isEmpty()
                ? QStringLiteral("工作区布局 JSON 无效：%1").arg(parseError.errorString())
                : QStringLiteral("工作区 id、名称和布局不能为空");
        }
        return false;
    }
    const QDateTime updatedAt = record.updatedAt.isValid()
        ? record.updatedAt
        : QDateTime::currentDateTimeUtc();
    return runInTransaction([&](QString *innerError) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral(
            "INSERT INTO workspaces (id, name, layout_json, updated_at) "
            "VALUES (:id, :name, :layout_json, :updated_at) "
            "ON CONFLICT(id) DO UPDATE SET name = excluded.name, "
            "layout_json = excluded.layout_json, updated_at = excluded.updated_at"));
        query.bindValue(QStringLiteral(":id"), id);
        query.bindValue(QStringLiteral(":name"), name);
        // 重新压缩序列化，避免同一布局因无意义空白产生不同磁盘内容和导出差异。
        query.bindValue(QStringLiteral(":layout_json"),
                        QString::fromUtf8(layout.toJson(QJsonDocument::Compact)));
        query.bindValue(QStringLiteral(":updated_at"), serializeTime(updatedAt));
        if (!query.exec()) {
            reportQueryError(query, innerError);
            return false;
        }
        return true;
    }, errorMessage);
}

std::optional<WorkspaceRecord>
ApplicationDatabase::findWorkspace(const QString &id, QString *errorMessage) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT * FROM workspaces WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        reportQueryError(query, errorMessage);
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    return readWorkspaceRow(query);
}

QList<WorkspaceRecord> ApplicationDatabase::listWorkspaces(bool *ok,
                                                           QString *errorMessage) const
{
    setFlag(ok, false);
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT * FROM workspaces ORDER BY updated_at DESC, name COLLATE NOCASE, id"))) {
        reportQueryError(query, errorMessage);
        return {};
    }
    QList<WorkspaceRecord> records;
    while (query.next()) {
        records.append(readWorkspaceRow(query));
    }
    setFlag(ok, true);
    return records;
}

bool ApplicationDatabase::removeWorkspace(const QString &id,
                                          int *removedCount,
                                          QString *errorMessage)
{
    return runInTransaction([&](QString *innerError) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("DELETE FROM workspaces WHERE id = :id"));
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

// -------------------------------------------------------------------- 设置项

bool ApplicationDatabase::setSetting(const QString &key, const QString &value, QString *errorMessage)
{
    if (key.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("设置项的键不能为空");
        }
        return false;
    }
    // Qt 的默认 QString 具有“空且为 null”的内部状态。QSqlQuery 会把这种值绑定成 SQL
    // NULL，而不是长度为 0 的 TEXT；但设置表明确使用 NOT NULL，并且“空字体族”“已取消
    // 的快捷键”等业务值本来就需要合法保存为空文本。因此统一在数据库边界把 null QString
    // 规范化为非 null 空串，既保留业务语义，也避免各个设置调用方重复处理 Qt 的这一细节。
    const QString storedValue = value.isNull() ? QStringLiteral("") : value;
    return runInTransaction([&](QString *innerError) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral(
            "INSERT INTO settings (key, value) VALUES (:key, :value) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
        query.bindValue(QStringLiteral(":key"), key);
        query.bindValue(QStringLiteral(":value"), storedValue);
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
