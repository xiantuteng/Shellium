#include "security/Vault.h"

#include <sodium.h>

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
    const QByteArray encodedPassword = masterPassword.toUtf8();
    if (crypto_pwhash(reinterpret_cast<unsigned char *>(m_key.data()),
                      static_cast<unsigned long long>(m_key.size()),
                      encodedPassword.constData(),
                      static_cast<unsigned long long>(encodedPassword.size()),
                      reinterpret_cast<const unsigned char *>(m_salt.constData()),
                      crypto_pwhash_OPSLIMIT_INTERACTIVE,
                      crypto_pwhash_MEMLIMIT_INTERACTIVE,
                      crypto_pwhash_ALG_ARGON2ID13) != 0) {
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

QByteArray Vault::salt() const
{
    return m_salt;
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
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("主密码错误或密文已被篡改");
        }
        return {};
    }
    plainText.resize(static_cast<qsizetype>(plainLength));
    return plainText;
}
