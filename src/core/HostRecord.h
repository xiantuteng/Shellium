#pragma once

#include "core/ConnectionTarget.h"

#include <QByteArray>
#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QStringList>

#include <optional>

/**
 * @brief 主机树节点的种类。
 *
 * 分组和主机共用一张表，因为“分组继承”要求两者位于同一棵父子链上：如果分组独立成表，
 * 每次解析继承都要跨表递归，而且无法表达“分组下再放分组”的任意深度嵌套。
 */
enum class HostNodeKind {
    Folder = 0, ///< 仅用于组织结构的分组节点，本身不可连接。
    Host = 1    ///< 可连接的主机节点。
};

/**
 * @brief 持久化的主机 / 分组记录，可与 ConnectionTarget 互转。
 *
 * 可继承字段（port、username、identityFile、credentialId、proxyJumps）刻意使用
 * std::optional 或“空即未设置”的语义，用来区分“显式设为某值”和“沿用父分组配置”。
 * 若这些字段直接使用带默认值的普通类型，子主机就无法表达“端口跟随分组”，一旦分组端口
 * 变更，所有子主机都要手工同步。
 */
struct HostRecord {
    QString id;
    QString parentId; ///< 空表示位于根层级。
    HostNodeKind kind = HostNodeKind::Host;
    QString name;
    QString host;

    std::optional<quint16> port;  ///< 未设置时继承父分组，最终缺省为 22。
    QString username;             ///< 空字符串表示继承父分组。
    QString credentialId;         ///< 空字符串表示继承父分组。
    QString identityFile;         ///< 空字符串表示继承父分组。
    QStringList proxyJumps;       ///< 空列表表示继承父分组的跳板链。
    QStringList tags;             ///< 与祖先分组的标签取并集，用于按标签筛选。

    /**
     * @brief 是否显式允许旧版 hmac-sha1。
     *
     * 该字段刻意不参与继承：降低算法安全基线必须是针对某一台服务器的知情决定，
     * 若能从分组继承，用户可能在毫不知情的情况下让整组主机都退回弱 MAC。
     */
    bool allowLegacyHmacSha1 = false;

    int sortOrder = 0;
    QDateTime createdAt;
    QDateTime updatedAt;

    /**
     * @brief 转换为可直接发起连接的目标。
     * @return 未设置端口时回落到 22；不含任何密码字段。
     */
    [[nodiscard]] ConnectionTarget toConnectionTarget() const
    {
        ConnectionTarget target;
        target.id = id;
        target.displayName = name;
        target.host = host;
        target.username = username;
        target.port = port.value_or(quint16{22});
        target.identityFile = identityFile;
        target.proxyJumps = proxyJumps;
        target.parentId = parentId;
        target.allowLegacyHmacSha1 = allowLegacyHmacSha1;
        // 来自数据库的记录是用户保存的配置，不是从终端输出推测出来的，因此可标为 Confirmed。
        target.confidence = ConnectionTarget::Confidence::Confirmed;
        return target;
    }

    /**
     * @brief 由连接目标构造可持久化记录。
     * @param target 来源目标；其 password 与进程内字段一律被丢弃。
     * @return 类型为 Host 的记录，id 与时间戳需由调用方补全。
     */
    [[nodiscard]] static HostRecord fromConnectionTarget(const ConnectionTarget &target)
    {
        HostRecord record;
        record.id = target.id;
        record.kind = HostNodeKind::Host;
        record.name = target.displayName.isEmpty() ? target.host : target.displayName;
        record.host = target.host;
        record.username = target.username;
        record.port = target.port;
        record.identityFile = target.identityFile;
        record.proxyJumps = target.proxyJumps;
        record.parentId = target.parentId;
        record.allowLegacyHmacSha1 = target.allowLegacyHmacSha1;
        // password / tunnelRouteId / interactiveAuthenticationPending 是进程内状态，
        // 落库会把明文密码写进磁盘并让隧道 id 跨进程失效，因此在此显式丢弃。
        return record;
    }
};

/** @brief 凭据的用途分类，决定 Vault 解密后如何解释明文。 */
enum class CredentialType {
    Password = 0,    ///< 登录密码。
    PrivateKey = 1,  ///< SSH 私钥正文。
    Passphrase = 2,  ///< 私钥保护口令。
    Token = 3        ///< 其他令牌类机密。
};

/**
 * @brief 凭据记录：只承载密文及其解密所需的公开元数据。
 *
 * 数据库层不做加密也不接受明文。KDF 参数与 nonce 必须随密文一起存储，否则更换 Argon2
 * 代价参数后，旧密文将永远无法被重新派生出的密钥解开。
 */
struct CredentialRecord {
    QString id;
    QString name;
    CredentialType type = CredentialType::Password;

    QByteArray encryptedPayload; ///< 认证密文，绝不允许写入明文。
    QString cipher;              ///< 例如 "xchacha20-poly1305-v1"。
    QByteArray nonce;            ///< 每条密文独立的随机 nonce。
    QString kdfAlgorithm;        ///< 例如 "argon2id13"。
    QByteArray kdfSalt;
    qint64 kdfIterations = 0;   ///< Argon2 的 opslimit 或 PBKDF2 轮数。
    qint64 kdfMemoryKib = 0;    ///< Argon2 memlimit，单位 KiB。
    qint64 kdfParallelism = 0;

    QDateTime createdAt;
    QDateTime updatedAt;
};

/**
 * @brief 保险库的全局公开配置，用于在下次启动时重新派生并校验会话密钥。
 *
 * 该结构只包含盐值、算法标识和认证校验块，不包含主密码或派生密钥，因此可以安全写入
 * SQLite。校验块本质上是一段带认证标签的密文：使用错误主密码派生出的密钥无法解开它，
 * 从而让解锁流程在读取真实凭据之前就能明确报告“主密码错误或元数据损坏”。
 */
struct VaultConfiguration {
    int formatVersion = 1;           ///< 密文封装格式版本，供未来迁移算法时区分旧数据。
    QString kdfAlgorithm;            ///< 密钥派生算法，例如 "argon2id13"。
    QByteArray salt;                 ///< Argon2id 的公开随机盐值，不属于秘密。
    QByteArray verifier;             ///< 使用派生密钥加密的固定校验块，禁止为空。
    QDateTime createdAt;             ///< 首次创建时间；更新配置时保持不变。
    QDateTime updatedAt;             ///< 最近一次写入时间。
};

/** @brief 一次连接尝试的历史记录。 */
struct ConnectionHistoryEntry {
    qint64 id = 0; ///< 由数据库分配；0 表示尚未写入。
    QString hostId;
    QDateTime connectedAt;
    std::optional<QDateTime> disconnectedAt; ///< 未设置表示会话仍在进行或从未建立。
    std::optional<qint64> durationMs;        ///< 冗余保存时长，避免统计查询逐行解析时间戳。
    bool succeeded = true;
    QString error;
};

/** @brief 一条在某主机上执行过的命令。 */
struct CommandHistoryEntry {
    qint64 id = 0;
    QString hostId; ///< 空表示本地 Shell。
    QString command;
    QDateTime executedAt;
    std::optional<int> exitCode; ///< 未设置表示退出码未知（例如命令仍在运行）。
};

/**
 * @brief Snippet 树节点的类型。
 *
 * 分组与可执行片段共用一棵树：分组只负责组织，片段保存真正发送到终端的命令。数据库
 * 目前使用 command 是否为 NULL 区分两者，业务层仍使用显式枚举，避免界面把空命令误判
 * 为分组或把分组当作可执行内容。
 */
enum class SnippetNodeKind {
    Folder = 0, ///< 仅用于组织 Snippet 的分组节点，不可执行。
    Snippet = 1 ///< 保存非空命令的可执行片段。
};

/**
 * @brief 一条可持久化的 Snippet 或 Snippet 分组记录。
 *
 * parentId 为空表示根层级；sortOrder 决定同组内顺序。updatedAt 使用 UTC 时间持久化，
 * 便于未来实现同步冲突检测。分组的 command 必须为空，可执行片段的 command 必须非空。
 */
struct SnippetRecord {
    QString id;                         ///< 稳定主键；由调用方生成，保存后不可随名称变化。
    QString parentId;                   ///< 父分组 id；空字符串表示根层级。
    SnippetNodeKind kind = SnippetNodeKind::Snippet;
    QString name;                       ///< 展示名称，同一分组内允许重名。
    QString command;                    ///< 发送给终端的命令；分组必须为空。
    QString description;                ///< 用途、风险或参数说明，可为空。
    int sortOrder = 0;                  ///< 同一父节点内的稳定排序值。
    QDateTime updatedAt;                ///< 最近更新时间；无效值由数据库自动补为当前 UTC。
};

Q_DECLARE_METATYPE(HostRecord)
Q_DECLARE_METATYPE(CredentialRecord)
Q_DECLARE_METATYPE(VaultConfiguration)
Q_DECLARE_METATYPE(CommandHistoryEntry)
Q_DECLARE_METATYPE(SnippetRecord)
