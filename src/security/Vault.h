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

private:
    QByteArray m_key;
    QByteArray m_salt;
};
