#pragma once

#include "core/HostRecord.h"

#include <QList>
#include <QSqlDatabase>
#include <QString>

#include <functional>
#include <optional>

/**
 * @brief 管理 Shellium SQLite 数据库连接、事务、版本化建表与全部持久化读写。
 *
 * 每个实例使用独立 Qt SQL 连接名，测试可指向临时文件或内存数据库；生产环境默认存储在
 * 系统应用数据目录。数据库只保存结构化配置，凭据正文必须先由 Vault 加密，禁止明文写入。
 *
 * 读接口一律区分“查询失败”与“没有数据”：返回 std::optional 的接口在失败时同时写入
 * errorMessage，返回列表的接口通过 ok 出参报告失败。二者混淆会让上层把一次磁盘故障
 * 当成“用户还没有添加主机”，进而在下一次保存时覆盖掉尚存的数据。
 */
class ApplicationDatabase final {
public:
    explicit ApplicationDatabase(QString databasePath = {});
    ~ApplicationDatabase();

    ApplicationDatabase(const ApplicationDatabase &) = delete;
    ApplicationDatabase &operator=(const ApplicationDatabase &) = delete;

    /**
     * @brief 返回代表“纯内存数据库”的路径常量。
     *
     * 单元测试用它可以完全避免触碰用户真实数据文件，也不会在磁盘留下残留。
     */
    [[nodiscard]] static QString inMemoryPath();

    /** @brief 当前代码要求的 schema 版本号。 */
    [[nodiscard]] static int expectedSchemaVersion();

    /**
     * @brief 打开数据库并执行全部幂等迁移。
     * @param errorMessage 失败时接收可读错误；可为空。
     * @return 初始化成功返回 true。
     */
    bool initialize(QString *errorMessage = nullptr);

    /** @brief 返回已打开的 Qt SQL 连接，供诊断与集成测试使用。 */
    [[nodiscard]] QSqlDatabase connection() const;

    /** @brief 返回实际数据库文件路径，便于备份和问题诊断。 */
    [[nodiscard]] QString databasePath() const;

    /**
     * @brief 读取数据库中记录的 schema 版本。
     * @param errorMessage 失败时接收可读错误；可为空。
     * @return 读取成功返回版本号，失败返回 std::nullopt。
     */
    [[nodiscard]] std::optional<int> schemaVersion(QString *errorMessage = nullptr) const;

    // ---------------------------------------------------------------- 主机与分组

    /**
     * @brief 按 id 插入或整体更新一条主机 / 分组记录。
     * @param record 待写入记录；id 为空时视为参数错误。createdAt / updatedAt 为空则自动填充。
     * @param errorMessage 失败时接收可读错误；可为空。
     * @return 写入并提交成功返回 true。
     */
    bool saveHost(const HostRecord &record, QString *errorMessage = nullptr);

    /**
     * @brief 按 id 读取单条记录。
     * @return 存在返回记录；不存在返回 std::nullopt 且不设置 errorMessage。
     */
    [[nodiscard]] std::optional<HostRecord> findHost(const QString &id,
                                                     QString *errorMessage = nullptr) const;

    /**
     * @brief 删除记录；子节点由外键 ON DELETE CASCADE 一并删除。
     * @param removedCount 可选出参，接收实际删除的直接匹配行数（0 表示 id 不存在）。
     * @return 语句执行并提交成功返回 true；id 不存在同样算成功。
     */
    bool removeHost(const QString &id, int *removedCount = nullptr, QString *errorMessage = nullptr);

    /**
     * @brief 列出某个分组下的直接子节点，按 sortOrder、name 排序。
     * @param parentId 空字符串表示根层级。
     * @param ok 可选出参；查询失败置 false，空分组返回空列表但置 true。
     */
    [[nodiscard]] QList<HostRecord> listHostsInGroup(const QString &parentId,
                                                     bool *ok = nullptr,
                                                     QString *errorMessage = nullptr) const;

    /**
     * @brief 按名称或地址模糊搜索。
     * @param keyword 关键字；其中的 %、_、\ 会被转义，避免用户输入被当成通配符。
     */
    [[nodiscard]] QList<HostRecord> searchHosts(const QString &keyword,
                                                bool *ok = nullptr,
                                                QString *errorMessage = nullptr) const;

    /** @brief 按精确标签筛选主机（标签本身不做模糊匹配）。 */
    [[nodiscard]] QList<HostRecord> hostsWithTag(const QString &tag,
                                                 bool *ok = nullptr,
                                                 QString *errorMessage = nullptr) const;

    /**
     * @brief 沿父分组链补全未设置字段，返回可直接用于连接的有效配置。
     *
     * 这是“分组继承”的落地点：只有未显式设置的字段才向上取值，已设置字段永远优先。
     * 标签取自身与全部祖先的并集，以便按分组标签筛选也能命中其中的主机。
     * @return id 不存在返回 std::nullopt 且不设置 errorMessage。
     */
    [[nodiscard]] std::optional<HostRecord> resolveHost(const QString &id,
                                                        QString *errorMessage = nullptr) const;

    // ---------------------------------------------------------------- 凭据

    /**
     * @brief 插入或更新凭据。
     *
     * 只接受密文：encryptedPayload 或 cipher 为空时直接失败，防止调用方误把明文口令
     * 当作 payload 写入。加密与解密由 Vault 负责，本层永远看不到明文。
     */
    bool saveCredential(const CredentialRecord &record, QString *errorMessage = nullptr);

    /** @brief 按 id 读取凭据；不存在返回 std::nullopt 且不设置 errorMessage。 */
    [[nodiscard]] std::optional<CredentialRecord> findCredential(const QString &id,
                                                                 QString *errorMessage = nullptr) const;

    /** @brief 删除凭据。 */
    bool removeCredential(const QString &id,
                          int *removedCount = nullptr,
                          QString *errorMessage = nullptr);

    /** @brief 按名称列出全部凭据。 */
    [[nodiscard]] QList<CredentialRecord> listCredentials(bool *ok = nullptr,
                                                          QString *errorMessage = nullptr) const;

    // ---------------------------------------------------------------- 连接历史

    /**
     * @brief 追加一条连接记录。
     * @return 成功返回数据库分配的自增 id，失败返回 std::nullopt。
     */
    std::optional<qint64> appendConnectionHistory(const ConnectionHistoryEntry &entry,
                                                  QString *errorMessage = nullptr);

    /**
     * @brief 为已存在的连接记录补上断开时间，并据此换算时长。
     * @param id appendConnectionHistory 返回的自增 id。
     * @param updated 可选出参；id 不存在时置 false，但函数本身仍返回 true。
     */
    bool finishConnectionHistory(qint64 id,
                                 const QDateTime &disconnectedAt,
                                 bool *updated = nullptr,
                                 QString *errorMessage = nullptr);

    /**
     * @brief 按时间倒序取最近 N 条连接记录。
     * @param limit 上限；小于等于 0 时返回空列表并置 ok 为 true。
     * @param hostId 非空时只取该主机的记录。
     */
    [[nodiscard]] QList<ConnectionHistoryEntry> recentConnections(int limit,
                                                                  const QString &hostId = {},
                                                                  bool *ok = nullptr,
                                                                  QString *errorMessage = nullptr) const;

    // ---------------------------------------------------------------- 命令历史

    /** @brief 追加一条命令记录，返回自增 id。 */
    std::optional<qint64> appendCommandHistory(const CommandHistoryEntry &entry,
                                               QString *errorMessage = nullptr);

    /** @brief 按时间倒序取某主机最近 N 条命令；hostId 为空表示本地 Shell。 */
    [[nodiscard]] QList<CommandHistoryEntry> recentCommands(const QString &hostId,
                                                            int limit,
                                                            bool *ok = nullptr,
                                                            QString *errorMessage = nullptr) const;

    // ---------------------------------------------------------------- 设置项

    /** @brief 写入字符串设置项。 */
    bool setSetting(const QString &key, const QString &value, QString *errorMessage = nullptr);

    /** @brief 读取字符串设置项；键不存在返回 std::nullopt 且不设置 errorMessage。 */
    [[nodiscard]] std::optional<QString> setting(const QString &key,
                                                 QString *errorMessage = nullptr) const;

    /** @brief 删除设置项。 */
    bool removeSetting(const QString &key, QString *errorMessage = nullptr);

    /** @brief 以 0/1 写入布尔设置项。 */
    bool setBoolSetting(const QString &key, bool value, QString *errorMessage = nullptr);

    /**
     * @brief 读取布尔设置项。
     * @return 键不存在或存储值无法解析为布尔时返回 std::nullopt；后者会写入 errorMessage，
     *         以便与“用户从未设置过”区分开。
     */
    [[nodiscard]] std::optional<bool> boolSetting(const QString &key,
                                                  QString *errorMessage = nullptr) const;

    /** @brief 写入整数设置项。 */
    bool setIntSetting(const QString &key, int value, QString *errorMessage = nullptr);

    /** @brief 读取整数设置项；无法解析时返回 std::nullopt 并写入 errorMessage。 */
    [[nodiscard]] std::optional<int> intSetting(const QString &key,
                                                QString *errorMessage = nullptr) const;

    /**
     * @brief 在单个事务中执行一组写操作，任一步失败即整体回滚。
     * @param work 返回 false 表示放弃本次事务；可通过其 QString* 参数给出原因。
     * @return 全部成功并提交返回 true。
     */
    bool runInTransaction(const std::function<bool(QString *)> &work, QString *errorMessage = nullptr);

private:
    /** @brief 在当前事务中执行单条无参数迁移 SQL。 */
    bool executeMigration(const QString &sql, QString *errorMessage);

    /** @brief 建立初始表结构（schema 版本 1）。 */
    bool createBaseSchema(QString *errorMessage);

    /** @brief 逐版本升级到 expectedSchemaVersion()，全过程处于单个事务中。 */
    bool upgradeSchema(QString *errorMessage);

    QString m_databasePath;
    QString m_connectionName;
    QSqlDatabase m_database;

    /**
     * @brief 当前嵌套的事务层数。
     *
     * SQLite 不支持嵌套 BEGIN。上层若想把若干个已自带事务的写接口合并成一次原子操作，
     * 只有记录深度、让内层复用外层事务，才能既保持单个接口可独立调用，又允许组合。
     */
    int m_transactionDepth = 0;
};
