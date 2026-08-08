#pragma once

#include "core/HostRecord.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QString>

#include <optional>

class ApplicationDatabase;

/**
 * @brief 使用 libsodium Argon2id 与 XChaCha20-Poly1305 实现的内存凭据保险库。
 *
 * 主密码只用于派生 256 位会话密钥，不被保存。每条密文使用独立随机 nonce 并包含认证标签，
 * 可同时防止内容泄露和密文篡改。锁定时会使用 sodium_memzero 清除密钥内存。
 */
class Vault final {
public:
    Vault();
    ~Vault();

    Vault(const Vault &) = delete;
    Vault &operator=(const Vault &) = delete;

    /**
     * @brief 用主密码解锁保险库。
     * @param masterPassword 用户主密码。
     * @param salt 已存在保险库的盐；为空时生成新盐。
     * @param errorMessage 失败原因输出，可为空。
     * @return 派生成功返回 true。
     */
    bool unlock(const QString &masterPassword,
                const QByteArray &salt = {},
                QString *errorMessage = nullptr);

    /** @brief 清除会话密钥并进入锁定状态。 */
    void lock();

    /** @brief 判断当前是否持有可用会话密钥。 */
    [[nodiscard]] bool isUnlocked() const;

    /** @brief 返回当前密文格式使用的公开算法标识。 */
    [[nodiscard]] static QString cipherAlgorithm();

    /** @brief 返回当前密钥派生算法的公开标识。 */
    [[nodiscard]] static QString kdfAlgorithm();

    /** @brief 返回 XChaCha20-Poly1305 nonce 的固定字节数。 */
    [[nodiscard]] static qsizetype nonceSize();

    /** @brief 返回认证标签的固定字节数，用于数据库层校验密文最小长度。 */
    [[nodiscard]] static qsizetype authenticationTagSize();

    /** @brief 返回当前 Argon2id 的运算次数参数，供凭据元数据持久化。 */
    [[nodiscard]] static qint64 kdfOperationsLimit();

    /** @brief 返回当前 Argon2id 内存参数，单位 KiB。 */
    [[nodiscard]] static qint64 kdfMemoryKib();

    /** @brief 返回创建保险库时生成或解锁时采用的公开盐值。 */
    [[nodiscard]] QByteArray salt() const;

    /**
     * @brief 生成用于校验主密码的密文块，应当与盐一同持久化。
     * @param errorMessage 失败原因输出，可为空。
     * @return 校验块；保险库锁定时返回空。
     *
     * 只在首次创建保险库时调用一次。之所以需要它：错误的主密码同样能派生出一把合法的
     * 密钥，unlock 无从判断对错，用户要等到第一次使用凭据才会看到一串莫名其妙的解密失败。
     */
    [[nodiscard]] QByteArray createVerifier(QString *errorMessage = nullptr) const;

    /**
     * @brief 用当前会话密钥校验主密码是否正确。
     * @param verifier 之前由 createVerifier 生成并持久化的校验块。
     * @return 主密码正确返回 true；密码错误或校验块被篡改返回 false。
     *
     * 刻意不接受空校验块：那代表保险库尚未初始化，调用方应当走创建流程而不是解锁流程，
     * 把两者混为一谈会让损坏的数据被静默当成"新保险库"，进而覆盖掉用户已有的凭据。
     */
    [[nodiscard]] bool verifyPassword(const QByteArray &verifier) const;

    /** @brief 加密敏感字节，返回 nonce + 认证密文；锁定时返回空。 */
    [[nodiscard]] QByteArray encrypt(const QByteArray &plainText,
                                     QString *errorMessage = nullptr) const;

    /** @brief 验证并解密 nonce + 密文；篡改或密码错误时返回空。 */
    [[nodiscard]] QByteArray decrypt(const QByteArray &encrypted,
                                     QString *errorMessage = nullptr) const;

    /**
     * @brief 立即清除可变字节数组中的敏感内容并释放逻辑长度。
     * @param sensitiveData 需要擦除的密码、私钥或解密结果。
     *
     * QByteArray 的隐式共享意味着调用方应保证传入的是唯一持有的可变副本；若此前已经复制，
     * 其他副本仍需分别擦除。公开此方法是为了让 UI 在把解密结果交给 SSH 后及时清理临时缓冲。
     */
    static void secureErase(QByteArray &sensitiveData);

private:
    QByteArray m_key;
    QByteArray m_salt;
};

/**
 * @brief 把 Vault 密码学能力与 ApplicationDatabase 持久化组合成可直接供 UI 使用的服务。
 *
 * 该类负责保险库完整生命周期：首次创建时生成盐和校验块，后续启动校验主密码，保存时只把
 * 密文写入数据库，读取时校验全部算法元数据，并支持由界面定时器驱动的闲置自动锁定。
 * 它不缓存任何解密后的凭据；调用 loadSecret() 得到的明文所有权交给调用方，使用完必须调用
 * Vault::secureErase()。数据库对象必须比本服务存活更久。
 */
class CredentialVault final {
public:
    /** @brief 保险库可观察状态，供首次启动和解锁界面决定展示哪个流程。 */
    enum class State {
        Uninitialized, ///< 数据库中没有保险库配置，应进入创建主密码流程。
        Locked,        ///< 配置完整，但当前进程未持有会话密钥。
        Unlocked,      ///< 当前进程持有通过校验的会话密钥。
        Corrupt        ///< 配置查询失败、字段缺失或算法版本不受支持。
    };

    /**
     * @brief 构造凭据保险库服务。
     * @param database 已完成 initialize() 的应用数据库，生命周期必须覆盖本对象。
     */
    explicit CredentialVault(ApplicationDatabase &database);

    CredentialVault(const CredentialVault &) = delete;
    CredentialVault &operator=(const CredentialVault &) = delete;

    /**
     * @brief 检查数据库配置和内存密钥后返回当前状态。
     * @param errorMessage 状态为 Corrupt 时接收具体原因；可为空。
     */
    [[nodiscard]] State state(QString *errorMessage = nullptr) const;

    /**
     * @brief 首次创建保险库并原子持久化公开盐值和校验块。
     * @param masterPassword 用户设置的主密码，不会写入数据库。
     * @param errorMessage 失败原因输出；可为空。
     * @return 创建成功且保持解锁状态返回 true。
     *
     * 已存在配置或配置损坏时一律拒绝覆盖，避免用新主密码使已有密文永久不可恢复。
     */
    bool initialize(const QString &masterPassword, QString *errorMessage = nullptr);

    /**
     * @brief 使用持久化盐值派生密钥并验证主密码。
     * @return 校验成功返回 true；错误密码、未初始化或元数据损坏均返回 false。
     */
    bool unlock(const QString &masterPassword, QString *errorMessage = nullptr);

    /** @brief 清除会话密钥并进入锁定状态。 */
    void lock();

    /** @brief 判断当前是否持有已通过校验的会话密钥。 */
    [[nodiscard]] bool isUnlocked() const;

    /**
     * @brief 设置闲置自动锁定阈值。
     * @param timeoutMilliseconds 大于 0 时启用；0 或负数表示禁用自动锁定。
     */
    void setIdleTimeoutMs(qint64 timeoutMilliseconds);

    /**
     * @brief 记录一次用户或凭据访问活动。
     * @param monotonicMilliseconds 测试可传入确定的单调时钟值；负数使用内部时钟。
     */
    void recordActivity(qint64 monotonicMilliseconds = -1);

    /**
     * @brief 判断给定时刻是否已达到闲置锁定阈值，但不改变状态。
     * @param monotonicMilliseconds 测试可传入确定值；负数使用内部时钟。
     */
    [[nodiscard]] bool hasIdleTimedOut(qint64 monotonicMilliseconds = -1) const;

    /**
     * @brief 达到闲置阈值时清除密钥。
     * @return 本次调用确实从解锁转为锁定返回 true，否则返回 false。
     */
    bool lockIfIdle(qint64 monotonicMilliseconds = -1);

    /**
     * @brief 加密并保存一条凭据。
     * @param id 稳定凭据 id，供 HostRecord::credentialId 引用。
     * @param name 用户可见名称。
     * @param type 凭据用途。
     * @param secret 待加密明文；调用方仍负责擦除自己的输入缓冲。
     * @param errorMessage 失败原因输出；可为空。
     */
    bool saveSecret(const QString &id,
                    const QString &name,
                    CredentialType type,
                    const QByteArray &secret,
                    QString *errorMessage = nullptr);

    /**
     * @brief 读取、验证并解密一条凭据。
     * @return 成功返回明文字节；不存在、锁定或数据损坏返回 std::nullopt。
     *
     * 返回值包含敏感数据，调用方使用完后必须对其中的 QByteArray 调用 secureErase()。
     */
    [[nodiscard]] std::optional<QByteArray> loadSecret(const QString &id,
                                                       QString *errorMessage = nullptr);

    /**
     * @brief 根据主机的 credentialId 解密凭据并填入一次性连接目标。
     * @param host 已解析继承关系的主机记录；credentialId 为空时视为无需凭据并返回 true。
     * @param target 待发起连接的进程内目标；成功后 password 字段持有密码或私钥口令。
     * @param errorMessage 失败原因输出；可为空。
     * @return 无凭据或成功填充返回 true；凭据不存在、损坏或类型不适用时返回 false。
     *
     * 当前 ConnectionTarget 只能承载密码/私钥口令，不能承载私钥正文或通用令牌，因此这两类
     * 凭据会明确失败。私钥文件仍由 HostRecord::identityFile 指向本地路径。
     */
    bool applyHostCredential(const HostRecord &host,
                             ConnectionTarget &target,
                             QString *errorMessage = nullptr);

    /**
     * @brief 把连接配置中的密码加密保存，并原子更新主机的 credentialId 引用。
     * @param host 待更新的持久化主机；成功后其 credentialId 会同步为新建或复用的 id。
     * @param connectionConfiguration ConnectionDialog 等界面产生的连接配置。
     * @param credentialName 可选用户可见名称；为空时根据主机名与用途自动生成。
     * @param errorMessage 失败原因输出；可为空。
     * @return 凭据密文与主机引用全部提交成功返回 true，任一步失败都会回滚。
     *
     * host 已有 credentialId 时执行更新；为空时自动创建随机 id。identityFile 非空时把
     * password 解释为私钥口令，否则解释为登录密码。函数不会把明文复制进 HostRecord。
     */
    bool saveConnectionPassword(HostRecord &host,
                                const ConnectionTarget &connectionConfiguration,
                                const QString &credentialName = {},
                                QString *errorMessage = nullptr);

private:
    /** @brief 把负数哨兵转换为内部单调时钟，非负测试值保持原样。 */
    [[nodiscard]] qint64 normalizedMonotonicTime(qint64 supplied) const;

    ApplicationDatabase &m_database;
    Vault m_vault;
    QElapsedTimer m_monotonicClock;
    qint64 m_idleTimeoutMs = 15 * 60 * 1000; ///< 默认 15 分钟，UI 可通过设置覆盖。
    qint64 m_lastActivityMs = 0;
};
