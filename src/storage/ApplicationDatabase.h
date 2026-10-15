#pragma once

#include "core/HostRecord.h"
#include "forwarding/PortForwardRule.h"

#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>

#include <functional>
#include <optional>

/**
 * @brief 一个可持久化工作区的名称与版本化布局 JSON。
 *
 * 数据库层不解释 layoutJson：布局属于应用层协议，当前版本保存标签名称、持久化主机 id 和
 * 文件管理器显隐状态。把 JSON 作为不透明字段可让后续加入窗口几何或模板参数时保持表结构
 * 稳定；应用层读取时仍必须校验版本和每个字段，不能盲目信任磁盘内容。
 */
struct WorkspaceRecord {
    QString id;         ///< 稳定 UUID；保存同名工作区不会隐式覆盖其他 id。
    QString name;       ///< 用户可见名称，去除首尾空白后不得为空。
    QString layoutJson; ///< 应用层生成的紧凑 JSON，不得为空且必须是合法 JSON 文档。
    QDateTime updatedAt;
};

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

    /**
     * @brief 把目标主机及其受管主机链解析为从本机到最终目标的有序连接计划。
     * @param id 最终目标主机的稳定 id。
     * @param errorMessage 缺失引用、分组被当作跳板、重复节点或环路的可读说明。
     * @return 成功时返回至少包含最终目标的一组已完成分组继承解析的主机；失败返回空列表。
     *
     * 每个链路成员也可以拥有自己的前置链路，本方法会递归展开并拒绝任何重复节点。拒绝
     * 重复而不是静默去重，是因为同一主机在路径中出现两次通常代表配置顺序已经不可信，
     * 继续连接可能把后续内网地址交给错误的服务器解析。
     */
    [[nodiscard]] QList<HostRecord> resolveConnectionChain(
        const QString &id, QString *errorMessage = nullptr) const;

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

    /**
     * @brief 统计直接引用某条凭据的主机/分组记录数量。
     * @param credentialId 待统计的稳定凭据 id；空值视为参数错误。
     * @param ok 查询成功时置 true；没有引用时返回 0 且置 true。
     * @param errorMessage 参数或查询失败时接收原因；可为空。
     * @return hosts.credential_id 与指定 id 相等的记录数；失败返回 0。
     *
     * 这里只统计数据库中的直接引用，不展开父分组继承。继承本身是有意共享父分组凭据；
     * 真正危险的是多条独立记录持有同一 credential_id，编辑其中一条会覆盖其他记录。
     */
    [[nodiscard]] int credentialReferenceCount(const QString &credentialId,
                                               bool *ok = nullptr,
                                               QString *errorMessage = nullptr) const;

    // ------------------------------------------------------------ 保险库公开配置

    /**
     * @brief 原子保存保险库盐值和主密码校验块。
     *
     * 表中只允许存在一条配置。调用方必须先完成密钥派生与校验块加密，再调用本方法；
     * 主密码和会话密钥永远不会进入数据库层。缺少任一关键字段都会被拒绝，防止半写入的
     * 初始化数据让后续启动既不能创建也不能解锁。
     * @param configuration 待持久化的公开配置。
     * @param errorMessage 失败时接收可读错误；可为空。
     * @return 写入并提交成功返回 true。
     */
    bool saveVaultConfiguration(const VaultConfiguration &configuration,
                                QString *errorMessage = nullptr);

    /**
     * @brief 删除保险箱的主密码公开配置。
     * @param errorMessage 删除或事务提交失败时接收可读原因；可为空。
     * @return 删除语句成功提交时返回 true；配置原本不存在也视为成功。
     *
     * 本方法只删除盐值和认证校验块，不处理 credentials 表。调用方必须先在同一个外层事务
     * 中把所有主密码凭据迁移到新的保护方式，才能调用本方法；否则旧密文会永久失去解密
     * 所需的派生参数。runInTransaction() 支持嵌套，因此 CredentialVault 可以原子组合迁移与
     * 删除操作，任何一步失败都会恢复原保险箱。
     */
    bool removeVaultConfiguration(QString *errorMessage = nullptr);

    /**
     * @brief 在忘记主密码时不可逆地重置整个凭据保险箱。
     * @param errorMessage 任一清理语句或事务提交失败时接收可读原因；可为空。
     * @return 主机凭据引用、全部凭据密文、保险箱元数据和旧本机密钥全部原子清除后返回 true。
     *
     * 该操作只清理无法再解密的认证材料：hosts 表中的地址、账号、端口、私钥文件路径、
     * 分组与主机链均原样保留。所有删除必须处在同一个事务中，避免出现主机仍引用已删除密文，
     * 或密文已删除但应用下次启动仍要求旧主密码的半重置状态。
     */
    bool resetCredentialVault(QString *errorMessage = nullptr);

    /**
     * @brief 读取唯一的保险库公开配置。
     * @param errorMessage 查询失败或字段损坏时接收原因；可为空。
     * @return 尚未初始化时返回 std::nullopt 且不设置错误；记录完整时返回配置。
     *
     * 空盐值、空校验块、未知格式版本都视为数据损坏，而不是“尚未初始化”。这一边界可以
     * 避免应用静默创建新保险库并使已有凭据永久不可解密。
     */
    [[nodiscard]] std::optional<VaultConfiguration>
    vaultConfiguration(QString *errorMessage = nullptr) const;

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

    /** @brief 按时间倒序取全部主机（含本地 Shell）最近 N 条命令。 */
    [[nodiscard]] QList<CommandHistoryEntry> recentCommandsAcrossHosts(
        int limit, bool *ok = nullptr, QString *errorMessage = nullptr) const;

    // ---------------------------------------------------------------- Snippets

    /**
     * @brief 新增或整体更新一条 Snippet / 分组记录。
     * @param record 待保存记录；id、name 不能为空，可执行片段还必须包含非空 command。
     * @param errorMessage 失败时接收参数校验、父节点或 SQL 错误；可为空。
     * @return 校验、写入和事务提交全部成功时返回 true。
     *
     * 父节点必须是已存在的分组；把分组移动到自己的后代下会被拒绝。更新使用 UPSERT 而
     * 不是 REPLACE，避免 SQLite 先删除旧行后让子节点短暂失去父级。
     */
    bool saveSnippet(const SnippetRecord &record, QString *errorMessage = nullptr);

    /**
     * @brief 按稳定 id 读取一条 Snippet 或分组。
     * @return 存在时返回完整记录；不存在返回 std::nullopt 且不设置 errorMessage。
     */
    [[nodiscard]] std::optional<SnippetRecord> findSnippet(
        const QString &id,
        QString *errorMessage = nullptr) const;

    /**
     * @brief 列出某个分组的直接子节点。
     * @param parentId 空表示根层级。
     * @param ok 查询成功时置 true；空分组同样属于成功。
     * @param errorMessage 查询失败时接收可读错误；可为空。
     * @return 按 sortOrder、name 排序的直接子节点，不递归展开。
     */
    [[nodiscard]] QList<SnippetRecord> listSnippetsInGroup(
        const QString &parentId,
        bool *ok = nullptr,
        QString *errorMessage = nullptr) const;

    /**
     * @brief 删除 Snippet；删除分组时递归删除全部后代。
     * @param id 目标稳定 id。
     * @param removedCount 可选出参，接收本次删除的目标及后代总行数。
     * @param errorMessage 失败时接收可读错误；可为空。
     * @return SQL 成功执行并提交时返回 true；id 不存在也返回 true 且删除数为 0。
     */
    bool removeSnippet(const QString &id,
                       int *removedCount = nullptr,
                       QString *errorMessage = nullptr);

    // --------------------------------------------------------------- 端口转发规则

    /**
     * @brief 插入或整体更新一条端口转发规则。
     * @param rule 必须通过 PortForwardRule::isValid()，且 hostId 必须指向可连接主机节点。
     * @param errorMessage 失败时接收字段、外键或 SQL 错误；可传 nullptr。
     * @return 写入并提交成功返回 true。
     *
     * hostId 绑定持久化 hosts 主键，而不是一次会话里的地址字符串。这样主机改名或修改地址
     * 后规则仍属于同一主机，根连接生命周期也能无歧义地触发 autoStart。
     */
    bool savePortForward(const PortForwardRule &rule, QString *errorMessage = nullptr);

    /** @brief 按稳定 id 读取规则；不存在返回 std::nullopt 且不设置错误。 */
    [[nodiscard]] std::optional<PortForwardRule>
    findPortForward(const QString &id, QString *errorMessage = nullptr) const;

    /**
     * @brief 列出某台持久化主机的全部规则，按名称排序。
     * @param hostId hosts 表主键；空字符串不会退化成“列出全部”，而是返回参数错误。
     */
    [[nodiscard]] QList<PortForwardRule> listPortForwards(const QString &hostId,
                                                          bool *ok = nullptr,
                                                          QString *errorMessage = nullptr) const;

    /**
     * @brief 一次查询返回每台主机保存的端口转发规则数量。
     * @param ok 可选出参；查询成功（包括完全没有规则）置 true，失败置 false。
     * @param errorMessage 查询失败时接收数据库错误；可为空。
     * @return hostId 到规则数量的映射；没有规则的主机不出现在映射中，调用方应按 0 处理。
     *
     * 主机树会同时展示大量节点，集中聚合可避免为每台主机分别执行一次 COUNT 查询而形成
     * N+1 数据库访问；主机数量增加后刷新耗时仍保持为一次顺序扫描。
     */
    [[nodiscard]] QHash<QString, int>
    portForwardCounts(bool *ok = nullptr, QString *errorMessage = nullptr) const;

    /** @brief 删除规则；id 不存在仍算成功，并通过 removedCount 返回 0。 */
    bool removePortForward(const QString &id,
                           int *removedCount = nullptr,
                           QString *errorMessage = nullptr);

    // ---------------------------------------------------------------- 工作区

    /**
     * @brief 新增或整体更新一个工作区。
     * @param record id、name 和 layoutJson 必须有效；layoutJson 会解析一次以拒绝损坏数据。
     * @param errorMessage 参数、JSON 或 SQL 错误输出；可为空。
     * @return 写入并提交成功返回 true。
     */
    bool saveWorkspace(const WorkspaceRecord &record, QString *errorMessage = nullptr);

    /** @brief 按稳定 id 读取工作区；不存在返回空且不设置错误。 */
    [[nodiscard]] std::optional<WorkspaceRecord>
    findWorkspace(const QString &id, QString *errorMessage = nullptr) const;

    /**
     * @brief 列出全部工作区，最近更新的排在前面。
     * @param ok 查询成功时置 true，空列表也是成功。
     * @param errorMessage 查询失败时接收原因；可为空。
     */
    [[nodiscard]] QList<WorkspaceRecord> listWorkspaces(
        bool *ok = nullptr,
        QString *errorMessage = nullptr) const;

    /** @brief 删除工作区；不存在也返回成功，并通过 removedCount 返回 0。 */
    bool removeWorkspace(const QString &id,
                         int *removedCount = nullptr,
                         QString *errorMessage = nullptr);

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
