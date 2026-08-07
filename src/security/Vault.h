#pragma once

#include <QByteArray>
#include <QString>

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

    /** @brief 返回创建保险库时生成或解锁时采用的公开盐值。 */
    [[nodiscard]] QByteArray salt() const;

    /** @brief 加密敏感字节，返回 nonce + 认证密文；锁定时返回空。 */
    [[nodiscard]] QByteArray encrypt(const QByteArray &plainText,
                                     QString *errorMessage = nullptr) const;

    /** @brief 验证并解密 nonce + 密文；篡改或密码错误时返回空。 */
    [[nodiscard]] QByteArray decrypt(const QByteArray &encrypted,
                                     QString *errorMessage = nullptr) const;

private:
    QByteArray m_key;
    QByteArray m_salt;
};
