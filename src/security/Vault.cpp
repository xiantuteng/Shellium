#include "security/Vault.h"

#include "storage/ApplicationDatabase.h"

#include <sodium.h>

#include <QUuid>

#include <mutex>

namespace {

/** @brief 确保 libsodium 全局初始化在进程内只执行一次。 */
bool initializeSodium()
{
    static std::once_flag once;
    static bool initialized = false;
    std::call_once(once, []() { initialized = sodium_init() >= 0; });
    return initialized;
}

/**
 * @brief 校验块的明文内容。
 *
 * 取固定常量即可：它不含任何机密，作用只是给出一段"用正确密钥才能解开"的密文。
 * 带上格式版本，将来若更换 KDF 参数或密文布局，可以据此识别旧数据并平滑迁移。
 */
constexpr char kVerifierPlainText[] = "shellium-vault-v1";

} // namespace

Vault::Vault()
{
    initializeSodium();
}

Vault::~Vault()
{
    lock();
}

bool Vault::unlock(const QString &masterPassword,
                   const QByteArray &salt,
                   QString *errorMessage)
{
    lock();
    if (!initializeSodium()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("libsodium 初始化失败");
        }
        return false;
    }
    if (masterPassword.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("主密码不能为空");
        }
        return false;
    }

    if (salt.isEmpty()) {
        m_salt.resize(crypto_pwhash_SALTBYTES);
        randombytes_buf(m_salt.data(), static_cast<size_t>(m_salt.size()));
    } else if (salt.size() == crypto_pwhash_SALTBYTES) {
        m_salt = salt;
    } else {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("保险库盐值长度无效");
        }
        return false;
    }

    m_key.resize(crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
    QByteArray encodedPassword = masterPassword.toUtf8();
    const int derivationResult = crypto_pwhash(
        reinterpret_cast<unsigned char *>(m_key.data()),
        static_cast<unsigned long long>(m_key.size()),
        encodedPassword.constData(),
        static_cast<unsigned long long>(encodedPassword.size()),
        reinterpret_cast<const unsigned char *>(m_salt.constData()),
        crypto_pwhash_OPSLIMIT_INTERACTIVE,
        crypto_pwhash_MEMLIMIT_INTERACTIVE,
        crypto_pwhash_ALG_ARGON2ID13);
    // QString::toUtf8() 会产生一份包含完整主密码的临时缓冲。无论派生成功与否都立即擦除，
    // 避免它一直留到 QByteArray 分配器复用这块堆内存为止。
    secureErase(encodedPassword);
    if (derivationResult != 0) {
        lock();
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("内存不足，无法派生保险库密钥");
        }
        return false;
    }
    return true;
}

void Vault::lock()
{
    if (!m_key.isEmpty()) {
        sodium_memzero(m_key.data(), static_cast<size_t>(m_key.size()));
        m_key.clear();
    }
}

bool Vault::isUnlocked() const
{
    return m_key.size() == crypto_aead_xchacha20poly1305_ietf_KEYBYTES;
}

QString Vault::cipherAlgorithm()
{
    return QStringLiteral("xchacha20-poly1305-v1");
}

QString Vault::kdfAlgorithm()
{
    return QStringLiteral("argon2id13");
}

qsizetype Vault::nonceSize()
{
    return crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
}

qsizetype Vault::authenticationTagSize()
{
    return crypto_aead_xchacha20poly1305_ietf_ABYTES;
}

qint64 Vault::kdfOperationsLimit()
{
    return static_cast<qint64>(crypto_pwhash_OPSLIMIT_INTERACTIVE);
}

qint64 Vault::kdfMemoryKib()
{
    return static_cast<qint64>(crypto_pwhash_MEMLIMIT_INTERACTIVE / 1024U);
}

QByteArray Vault::salt() const
{
    return m_salt;
}

QByteArray Vault::createVerifier(QString *errorMessage) const
{
    return encrypt(QByteArray(kVerifierPlainText), errorMessage);
}

bool Vault::verifyPassword(const QByteArray &verifier) const
{
    if (verifier.isEmpty()) {
        // 空校验块代表保险库尚未初始化。此处返回 false 而不是"通过"，避免调用方把损坏或
        // 缺失的数据当成新保险库，进而用一把新密钥覆盖掉用户已有的凭据。
        return false;
    }
    // sodium_memcmp 是常数时间比较：普通的 == 会随不匹配位置提前返回，泄漏可用于逐字节
    // 猜测的时序信息。这里比较的虽是公开常量，但保持一致的写法可以避免以后被照搬到真正
    // 敏感的比较上。
    QByteArray plainText = decrypt(verifier);
    const QByteArray expected(kVerifierPlainText);
    if (plainText.size() != expected.size()) {
        secureErase(plainText);
        return false;
    }
    const bool matches = sodium_memcmp(plainText.constData(),
                                       expected.constData(),
                                       static_cast<size_t>(expected.size())) == 0;
    secureErase(plainText);
    return matches;
}

QByteArray Vault::encrypt(const QByteArray &plainText, QString *errorMessage) const
{
    if (!isUnlocked()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("保险库尚未解锁");
        }
        return {};
    }

    QByteArray nonce(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, Qt::Uninitialized);
    randombytes_buf(nonce.data(), static_cast<size_t>(nonce.size()));
    QByteArray cipher(plainText.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES,
                      Qt::Uninitialized);
    unsigned long long cipherLength = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            reinterpret_cast<unsigned char *>(cipher.data()),
            &cipherLength,
            reinterpret_cast<const unsigned char *>(plainText.constData()),
            static_cast<unsigned long long>(plainText.size()),
            nullptr,
            0,
            nullptr,
            reinterpret_cast<const unsigned char *>(nonce.constData()),
            reinterpret_cast<const unsigned char *>(m_key.constData())) != 0) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("凭据加密失败");
        }
        return {};
    }
    cipher.resize(static_cast<qsizetype>(cipherLength));
    return nonce + cipher;
}

QByteArray Vault::decrypt(const QByteArray &encrypted, QString *errorMessage) const
{
    if (!isUnlocked()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("保险库尚未解锁");
        }
        return {};
    }
    constexpr qsizetype nonceLength = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    if (encrypted.size() < nonceLength + crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("保险库密文格式无效");
        }
        return {};
    }

    const QByteArray nonce = encrypted.first(nonceLength);
    const QByteArray cipher = encrypted.sliced(nonceLength);
    QByteArray plainText(cipher.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES,
                         Qt::Uninitialized);
    unsigned long long plainLength = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char *>(plainText.data()),
            &plainLength,
            nullptr,
            reinterpret_cast<const unsigned char *>(cipher.constData()),
            static_cast<unsigned long long>(cipher.size()),
            nullptr,
            0,
            reinterpret_cast<const unsigned char *>(nonce.constData()),
            reinterpret_cast<const unsigned char *>(m_key.constData())) != 0) {
        // 即使认证失败，libsodium 也可能已经触碰输出缓冲；返回前统一清零，避免把任何
        // 未验证字节残留在堆上。
        secureErase(plainText);
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("主密码错误或密文已被篡改");
        }
        return {};
    }
    plainText.resize(static_cast<qsizetype>(plainLength));
    return plainText;
}

void Vault::secureErase(QByteArray &sensitiveData)
{
    if (!sensitiveData.isEmpty()) {
        sodium_memzero(sensitiveData.data(), static_cast<size_t>(sensitiveData.size()));
        sensitiveData.clear();
    }
}

CredentialVault::CredentialVault(ApplicationDatabase &database)
    : m_database(database)
{
    // QElapsedTimer 使用单调时钟，不受用户调整系统时间或 NTP 校时影响，适合安全超时判断。
    m_monotonicClock.start();
    m_lastActivityMs = m_monotonicClock.elapsed();
}

CredentialVault::State CredentialVault::state(QString *errorMessage) const
{
    QString configurationError;
    const std::optional<VaultConfiguration> configuration =
        m_database.vaultConfiguration(&configurationError);
    if (!configurationError.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = configurationError;
        }
        return State::Corrupt;
    }
    if (!configuration.has_value()) {
        bool credentialsReadable = false;
        QString credentialsError;
        const QList<CredentialRecord> existingCredentials =
            m_database.listCredentials(&credentialsReadable, &credentialsError);
        if (!credentialsReadable || !existingCredentials.isEmpty()) {
            if (errorMessage != nullptr) {
                *errorMessage = !credentialsReadable
                    ? credentialsError
                    : QStringLiteral("数据库已有加密凭据，但缺少保险库盐值和主密码校验块");
            }
            // 老版本或截断数据库可能留下 credentials 行却丢失全局校验块。将其当成首次
            // 启动会生成新密钥并覆盖引用，因此必须停在可诊断、不可写的损坏状态。
            return State::Corrupt;
        }
        return State::Uninitialized;
    }

    // 数据库层检查“非空”，服务层进一步检查算法和精确长度。分层校验让数据库保持与
    // 密码库实现解耦，同时确保损坏的元数据绝不会被误报成普通锁定状态。
    if (configuration->formatVersion != 1
        || configuration->kdfAlgorithm != Vault::kdfAlgorithm()
        || configuration->salt.size() != crypto_pwhash_SALTBYTES
        || configuration->verifier.size()
               < Vault::nonceSize() + Vault::authenticationTagSize()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("保险库算法标识、盐值或校验块格式已损坏");
        }
        return State::Corrupt;
    }

    // 若数据库在运行期间被外部替换，内存中的旧密钥不得继续被视为已解锁。即使盐值相同，
    // 也要重新认证 verifier，才能发现校验块被同长度数据替换的非结构性损坏。
    if (m_vault.isUnlocked() && m_vault.salt() == configuration->salt) {
        if (m_vault.verifyPassword(configuration->verifier)) {
            return State::Unlocked;
        }
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("保险库校验块无法通过当前会话密钥认证");
        }
        return State::Corrupt;
    }
    return State::Locked;
}

bool CredentialVault::initialize(const QString &masterPassword, QString *errorMessage)
{
    QString stateError;
    const State currentState = state(&stateError);
    if (currentState != State::Uninitialized) {
        if (errorMessage != nullptr) {
            *errorMessage = currentState == State::Corrupt
                ? stateError
                : QStringLiteral("保险库已经初始化，禁止覆盖现有主密码配置");
        }
        return false;
    }

    QString cryptoError;
    if (!m_vault.unlock(masterPassword, {}, &cryptoError)) {
        if (errorMessage != nullptr) {
            *errorMessage = cryptoError;
        }
        return false;
    }
    const QByteArray verifier = m_vault.createVerifier(&cryptoError);
    if (verifier.isEmpty()) {
        m_vault.lock();
        if (errorMessage != nullptr) {
            *errorMessage = cryptoError.isEmpty() ? QStringLiteral("无法创建主密码校验块")
                                                  : cryptoError;
        }
        return false;
    }

    VaultConfiguration configuration;
    configuration.formatVersion = 1;
    configuration.kdfAlgorithm = Vault::kdfAlgorithm();
    configuration.salt = m_vault.salt();
    configuration.verifier = verifier;
    QString databaseError;
    if (!m_database.saveVaultConfiguration(configuration, &databaseError)) {
        m_vault.lock();
        if (errorMessage != nullptr) {
            *errorMessage = databaseError;
        }
        return false;
    }

    recordActivity();
    return true;
}

bool CredentialVault::unlock(const QString &masterPassword, QString *errorMessage)
{
    // 每次尝试前先丢弃旧密钥。这样即使数据库配置刚刚损坏，失败的重新解锁也不会让调用方
    // 继续使用一次历史成功留下的密钥。
    m_vault.lock();

    QString currentStateError;
    const State currentState = state(&currentStateError);
    if (currentState != State::Locked) {
        if (errorMessage != nullptr) {
            *errorMessage = currentState == State::Uninitialized
                ? QStringLiteral("保险库尚未初始化")
                : currentStateError.isEmpty() ? QStringLiteral("保险库配置状态异常")
                                              : currentStateError;
        }
        return false;
    }

    QString configurationError;
    const std::optional<VaultConfiguration> configuration =
        m_database.vaultConfiguration(&configurationError);
    if (!configuration.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = configurationError.isEmpty() ? QStringLiteral("保险库尚未初始化")
                                                         : configurationError;
        }
        return false;
    }
    if (configuration->formatVersion != 1
        || configuration->kdfAlgorithm != Vault::kdfAlgorithm()
        || configuration->salt.size() != crypto_pwhash_SALTBYTES
        || configuration->verifier.size()
               < Vault::nonceSize() + Vault::authenticationTagSize()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("保险库公开配置已损坏或算法版本不受支持");
        }
        return false;
    }

    QString cryptoError;
    if (!m_vault.unlock(masterPassword, configuration->salt, &cryptoError)) {
        if (errorMessage != nullptr) {
            *errorMessage = cryptoError;
        }
        return false;
    }
    if (!m_vault.verifyPassword(configuration->verifier)) {
        m_vault.lock();
        if (errorMessage != nullptr) {
            // 不区分“密码错误”和“校验块被篡改”，避免向攻击者泄漏可用于判断数据库状态的
            // 额外信息；诊断界面可以通过 state() 单独发现明显的结构损坏。
            *errorMessage = QStringLiteral("主密码错误或保险库校验块已损坏");
        }
        return false;
    }

    recordActivity();
    return true;
}

void CredentialVault::lock()
{
    m_vault.lock();
}

bool CredentialVault::isUnlocked() const
{
    // 不能只看内存 key 长度：数据库可能在运行时被替换。state() 会同时核验当前公开配置。
    return state() == State::Unlocked;
}

void CredentialVault::setIdleTimeoutMs(const qint64 timeoutMilliseconds)
{
    m_idleTimeoutMs = timeoutMilliseconds;
}

qint64 CredentialVault::normalizedMonotonicTime(const qint64 supplied) const
{
    return supplied >= 0 ? supplied : m_monotonicClock.elapsed();
}

void CredentialVault::recordActivity(const qint64 monotonicMilliseconds)
{
    m_lastActivityMs = normalizedMonotonicTime(monotonicMilliseconds);
}

bool CredentialVault::hasIdleTimedOut(const qint64 monotonicMilliseconds) const
{
    if (!isUnlocked() || m_idleTimeoutMs <= 0) {
        return false;
    }
    const qint64 now = normalizedMonotonicTime(monotonicMilliseconds);
    // 防御测试时钟或极少见的单调计时器重置：负间隔不应触发立即锁定。
    return now >= m_lastActivityMs && now - m_lastActivityMs >= m_idleTimeoutMs;
}

bool CredentialVault::lockIfIdle(const qint64 monotonicMilliseconds)
{
    if (!hasIdleTimedOut(monotonicMilliseconds)) {
        return false;
    }
    lock();
    return true;
}

bool CredentialVault::saveSecret(const QString &id,
                                 const QString &name,
                                 const CredentialType type,
                                 const QByteArray &secret,
                                 QString *errorMessage)
{
    if (!isUnlocked()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("保险库尚未解锁");
        }
        return false;
    }
    if (id.trimmed().isEmpty() || name.trimmed().isEmpty() || secret.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("凭据 id、名称和秘密内容均不能为空");
        }
        return false;
    }

    QString cryptoError;
    QByteArray encrypted = m_vault.encrypt(secret, &cryptoError);
    if (encrypted.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = cryptoError;
        }
        return false;
    }

    CredentialRecord record;
    record.id = id;
    record.name = name;
    record.type = type;
    record.nonce = encrypted.first(Vault::nonceSize());
    record.encryptedPayload = encrypted.sliced(Vault::nonceSize());
    record.cipher = Vault::cipherAlgorithm();
    record.kdfAlgorithm = Vault::kdfAlgorithm();
    record.kdfSalt = m_vault.salt();
    record.kdfIterations = Vault::kdfOperationsLimit();
    record.kdfMemoryKib = Vault::kdfMemoryKib();
    record.kdfParallelism = 1;

    QString databaseError;
    const bool saved = m_database.saveCredential(record, &databaseError);
    // encrypted 虽不是明文，但及时释放它可以缩短同一凭据多份密文缓冲在内存中的驻留时间。
    encrypted.clear();
    if (!saved) {
        if (errorMessage != nullptr) {
            *errorMessage = databaseError;
        }
        return false;
    }
    recordActivity();
    return true;
}

std::optional<QByteArray> CredentialVault::loadSecret(const QString &id,
                                                      QString *errorMessage)
{
    if (!isUnlocked()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("保险库尚未解锁");
        }
        return std::nullopt;
    }

    QString databaseError;
    const std::optional<CredentialRecord> record = m_database.findCredential(id, &databaseError);
    if (!record.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = databaseError.isEmpty() ? QStringLiteral("指定凭据不存在") : databaseError;
        }
        return std::nullopt;
    }

    // 所有公开元数据都必须与当前已解锁保险库一致。只检查密文认证标签不够：攻击者若把
    // 另一套保险库的合法记录替换进来，错误应明确归因于元数据不匹配而不是让解密模糊失败。
    if (record->cipher != Vault::cipherAlgorithm()
        || record->nonce.size() != Vault::nonceSize()
        || record->encryptedPayload.size() < Vault::authenticationTagSize()
        || record->kdfAlgorithm != Vault::kdfAlgorithm()
        || record->kdfSalt != m_vault.salt()
        || record->kdfIterations != Vault::kdfOperationsLimit()
        || record->kdfMemoryKib != Vault::kdfMemoryKib()
        || record->kdfParallelism != 1) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("凭据加密元数据不完整、已损坏或不属于当前保险库");
        }
        return std::nullopt;
    }

    const QByteArray encrypted = record->nonce + record->encryptedPayload;
    QString cryptoError;
    QByteArray plainText = m_vault.decrypt(encrypted, &cryptoError);
    if (plainText.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = cryptoError.isEmpty() ? QStringLiteral("凭据解密失败") : cryptoError;
        }
        return std::nullopt;
    }
    recordActivity();
    return plainText;
}

bool CredentialVault::applyHostCredential(const HostRecord &host,
                                          ConnectionTarget &target,
                                          QString *errorMessage)
{
    if (host.credentialId.isEmpty()) {
        // 主机明确没有引用凭据是合法配置，例如完全依赖 ssh-agent 的连接，不应弹出错误。
        return true;
    }

    QString lookupError;
    const std::optional<CredentialRecord> record =
        m_database.findCredential(host.credentialId, &lookupError);
    if (!record.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = lookupError.isEmpty() ? QStringLiteral("主机引用的凭据不存在")
                                                  : lookupError;
        }
        return false;
    }
    if (record->type != CredentialType::Password
        && record->type != CredentialType::Passphrase) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral(
                "当前连接目标只支持密码或私钥口令，不能直接使用私钥正文/令牌凭据");
        }
        return false;
    }

    std::optional<QByteArray> secret = loadSecret(host.credentialId, errorMessage);
    if (!secret.has_value()) {
        return false;
    }
    // ConnectionTarget 的 password 是现有 SSH API 的进程内传递格式。完成 QString 转换后
    // 立即擦除中间 QByteArray，避免同一口令同时以两种编码留在堆上。
    target.password = QString::fromUtf8(*secret);
    Vault::secureErase(*secret);
    return true;
}

bool CredentialVault::saveConnectionPassword(HostRecord &host,
                                             const ConnectionTarget &connectionConfiguration,
                                             const QString &credentialName,
                                             QString *errorMessage)
{
    if (host.id.trimmed().isEmpty() || host.name.trimmed().isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("保存连接凭据前必须先为主机分配 id 和名称");
        }
        return false;
    }
    if (connectionConfiguration.password.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("连接配置中没有可保存的密码或私钥口令");
        }
        return false;
    }

    HostRecord updatedHost = host;
    if (updatedHost.credentialId.isEmpty()) {
        updatedHost.credentialId = QStringLiteral("credential-%1")
                                       .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    const bool isPassphrase = !connectionConfiguration.identityFile.isEmpty();
    const CredentialType type = isPassphrase ? CredentialType::Passphrase
                                             : CredentialType::Password;
    const QString effectiveName = credentialName.trimmed().isEmpty()
        ? QStringLiteral("%1（%2）")
              .arg(updatedHost.name,
                   isPassphrase ? QStringLiteral("私钥口令") : QStringLiteral("登录密码"))
        : credentialName.trimmed();

    QByteArray secret = connectionConfiguration.password.toUtf8();
    QString transactionError;
    const bool saved = m_database.runInTransaction([&](QString *innerError) {
        if (!saveSecret(updatedHost.credentialId, effectiveName, type, secret, innerError)) {
            return false;
        }
        if (!m_database.saveHost(updatedHost, innerError)) {
            return false;
        }
        return true;
    }, &transactionError);
    // 无论事务成功或回滚，UTF-8 临时密码都必须立即擦除；调用方 ConnectionTarget 中的
    // QString 仍由调用方在连接结束后负责清理其生命周期。
    Vault::secureErase(secret);

    if (!saved) {
        if (errorMessage != nullptr) {
            *errorMessage = transactionError;
        }
        return false;
    }
    host = updatedHost;
    return true;
}
