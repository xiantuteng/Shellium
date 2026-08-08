#include "security/SshKeyManager.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <libssh/libssh.h>

namespace {

/** @brief RAII 包装 libssh 密钥，确保所有失败分支都释放原生对象。 */
class ScopedSshKey final {
public:
    ~ScopedSshKey() { SSH_KEY_FREE(value); }
    ssh_key value = nullptr;
};

/** @brief RAII 包装由 libssh 分配的字符串，并在释放前清除私钥正文。 */
class ScopedSensitiveString final {
public:
    ~ScopedSensitiveString()
    {
        if (value != nullptr) {
            volatile char *cursor = value;
            while (*cursor != '\0') {
                *cursor++ = '\0';
            }
            ssh_string_free_char(value);
        }
    }
    char *value = nullptr;
};

/** @brief 普通公钥 Base64 字符串不敏感，仅需保证由匹配的 libssh 释放函数回收。 */
class ScopedPublicString final {
public:
    ~ScopedPublicString() { SSH_STRING_FREE_CHAR(value); }
    char *value = nullptr;
};

/** @brief 把 QString 口令转成可擦除缓冲，析构时覆盖全部字节。 */
class ScopedPassphrase final {
public:
    explicit ScopedPassphrase(const QString &passphrase)
        : bytes(passphrase.toUtf8())
    {
    }
    ~ScopedPassphrase()
    {
        volatile char *cursor = bytes.data();
        for (qsizetype index = 0; index < bytes.size(); ++index) {
            cursor[index] = '\0';
        }
        bytes.clear();
    }
    [[nodiscard]] const char *nullableData() const
    {
        return bytes.isEmpty() ? nullptr : bytes.constData();
    }
    QByteArray bytes;
};

/** @brief libssh 请求口令时只记录事实并中止，用于区分加密文件与普通格式错误。 */
int detectPassphrasePrompt(const char *, char *, size_t, int, int, void *userData)
{
    if (userData != nullptr) {
        *static_cast<bool *>(userData) = true;
    }
    return -1;
}

enum ssh_keytypes_e nativeType(const SshKeyManager::Algorithm algorithm)
{
    switch (algorithm) {
    case SshKeyManager::Algorithm::Ed25519:
        return SSH_KEYTYPE_ED25519;
    case SshKeyManager::Algorithm::Rsa:
        return SSH_KEYTYPE_RSA;
    case SshKeyManager::Algorithm::EcdsaP256:
        return SSH_KEYTYPE_ECDSA_P256;
    }
    return SSH_KEYTYPE_UNKNOWN;
}

SshKeyManager::Algorithm portableType(const enum ssh_keytypes_e type)
{
    switch (type) {
    case SSH_KEYTYPE_RSA:
        return SshKeyManager::Algorithm::Rsa;
    case SSH_KEYTYPE_ECDSA:
    case SSH_KEYTYPE_ECDSA_P256:
    case SSH_KEYTYPE_ECDSA_P384:
    case SSH_KEYTYPE_ECDSA_P521:
        return SshKeyManager::Algorithm::EcdsaP256;
    default:
        return SshKeyManager::Algorithm::Ed25519;
    }
}

/**
 * @brief 从私钥构造 OpenSSH 单行公钥。
 * @return 成功返回非空字符串；失败返回空并写入错误。
 */
QString publicKeyLine(const ssh_key privateKey, const QString &comment, QString *errorMessage)
{
    ScopedSshKey publicKey;
    if (ssh_pki_export_privkey_to_pubkey(privateKey, &publicKey.value) != SSH_OK
        || publicKey.value == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("无法从私钥导出公钥");
        }
        return {};
    }
    ScopedPublicString base64;
    if (ssh_pki_export_pubkey_base64(publicKey.value, &base64.value) != SSH_OK
        || base64.value == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("无法编码 OpenSSH 公钥");
        }
        return {};
    }
    const char *typeName = ssh_key_type_to_char(ssh_key_type(publicKey.value));
    if (typeName == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("libssh 无法识别生成的公钥类型");
        }
        return {};
    }
    QString line = QString::fromLatin1(typeName) + QLatin1Char(' ')
        + QString::fromLatin1(base64.value);
    if (!comment.trimmed().isEmpty()) {
        line += QLatin1Char(' ') + comment.trimmed();
    }
    return line;
}

/** @brief 使用 QSaveFile 写入一个此前不存在的文件，并固定 POSIX 权限。 */
bool writeNewFileAtomically(const QString &path,
                            const QByteArray &content,
                            const QFileDevice::Permissions permissions,
                            QString *errorMessage)
{
    if (QFileInfo::exists(path)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("目标文件已存在，拒绝覆盖：%1").arg(path);
        }
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("无法创建文件 %1：%2").arg(path, file.errorString());
        }
        return false;
    }
    if (!file.setPermissions(permissions)) {
        file.cancelWriting();
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("无法设置安全文件权限：%1").arg(path);
        }
        return false;
    }
    if (file.write(content) != content.size()) {
        file.cancelWriting();
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("写入文件失败 %1：%2").arg(path, file.errorString());
        }
        return false;
    }
    if (!file.commit()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("原子提交文件失败 %1：%2").arg(path, file.errorString());
        }
        return false;
    }
    return true;
}

} // namespace

QList<SshKeyManager::Algorithm> SshKeyManager::supportedAlgorithms()
{
    QList<Algorithm> algorithms;
    for (const Algorithm algorithm : {Algorithm::Ed25519, Algorithm::Rsa, Algorithm::EcdsaP256}) {
        ScopedSshKey probe;
        const int parameter = algorithm == Algorithm::Rsa ? 2048
            : algorithm == Algorithm::EcdsaP256 ? 256 : 0;
        if (ssh_pki_generate(nativeType(algorithm), parameter, &probe.value) == SSH_OK
            && probe.value != nullptr) {
            algorithms.append(algorithm);
        }
    }
    return algorithms;
}

QString SshKeyManager::algorithmName(const Algorithm algorithm)
{
    switch (algorithm) {
    case Algorithm::Ed25519:
        return QStringLiteral("Ed25519（推荐）");
    case Algorithm::Rsa:
        return QStringLiteral("RSA");
    case Algorithm::EcdsaP256:
        return QStringLiteral("ECDSA P-256");
    }
    return QStringLiteral("未知算法");
}

bool SshKeyManager::generate(const Algorithm algorithm,
                            const QString &privateKeyPath,
                            const QString &passphrase,
                            const QString &comment,
                            const int rsaBits,
                            KeyInfo *info,
                            QString *errorMessage)
{
    if (privateKeyPath.trimmed().isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("私钥目标路径不能为空");
        }
        return false;
    }
    if (comment.contains(QLatin1Char('\n')) || comment.contains(QLatin1Char('\r'))) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("公钥注释不能包含换行符");
        }
        return false;
    }
    if (algorithm == Algorithm::Rsa && rsaBits != 2048 && rsaBits != 3072 && rsaBits != 4096) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("RSA 位数仅支持 2048、3072 或 4096");
        }
        return false;
    }
    const QString publicPath = privateKeyPath + QStringLiteral(".pub");
    if (QFileInfo::exists(privateKeyPath) || QFileInfo::exists(publicPath)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("私钥或公钥目标已存在，拒绝覆盖");
        }
        return false;
    }

    ScopedSshKey privateKey;
    const int parameter = algorithm == Algorithm::Rsa ? rsaBits
        : algorithm == Algorithm::EcdsaP256 ? 256 : 0;
    if (ssh_pki_generate(nativeType(algorithm), parameter, &privateKey.value) != SSH_OK
        || privateKey.value == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("当前 libssh 无法生成 %1 密钥").arg(algorithmName(algorithm));
        }
        return false;
    }

    ScopedPassphrase passphraseBytes(passphrase);
    ScopedSensitiveString privateText;
    if (ssh_pki_export_privkey_base64_format(privateKey.value,
                                             passphraseBytes.nullableData(),
                                             nullptr,
                                             nullptr,
                                             &privateText.value,
                                             SSH_FILE_FORMAT_OPENSSH) != SSH_OK
        || privateText.value == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("无法导出 OpenSSH 私钥");
        }
        return false;
    }
    const QString publicLine = publicKeyLine(privateKey.value, comment, errorMessage);
    if (publicLine.isEmpty()) {
        return false;
    }

    const QByteArray publicContent = publicLine.toUtf8() + '\n';
    if (!writeNewFileAtomically(publicPath,
                                publicContent,
                                QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                    | QFileDevice::ReadGroup | QFileDevice::ReadOther,
                                errorMessage)) {
        return false;
    }
    const QByteArray privateContent(privateText.value);
    if (!writeNewFileAtomically(privateKeyPath,
                                privateContent,
                                QFileDevice::ReadOwner | QFileDevice::WriteOwner,
                                errorMessage)) {
        // 两个目标在调用前均保证不存在，因此这里删除刚创建的公钥属于可恢复失败清理，
        // 不会误删用户原有文件。
        QFile::remove(publicPath);
        return false;
    }

    if (info != nullptr) {
        info->algorithm = algorithm;
        info->privateKeyPath = QFileInfo(privateKeyPath).absoluteFilePath();
        info->publicKeyPath = QFileInfo(publicPath).absoluteFilePath();
        info->publicKeyLine = publicLine;
        info->encrypted = !passphrase.isEmpty();
    }
    return true;
}

SshKeyManager::ImportStatus
SshKeyManager::inspectPrivateKey(const QString &privateKeyPath,
                                const QString &passphrase,
                                KeyInfo *info,
                                QString *errorMessage)
{
    const QFileInfo fileInfo(privateKeyPath);
    if (!fileInfo.exists() || !fileInfo.isFile() || !fileInfo.isReadable()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("私钥文件不存在、不是普通文件或不可读");
        }
        return ImportStatus::Invalid;
    }

    ScopedPassphrase passphraseBytes(passphrase);
    ScopedSshKey privateKey;
    bool prompted = false;
    const QByteArray encodedPath = fileInfo.absoluteFilePath().toUtf8();
    const int result = ssh_pki_import_privkey_file(encodedPath.constData(),
                                                   passphraseBytes.nullableData(),
                                                   passphrase.isEmpty()
                                                       ? detectPassphrasePrompt : nullptr,
                                                   passphrase.isEmpty() ? &prompted : nullptr,
                                                   &privateKey.value);
    if (result != SSH_OK || privateKey.value == nullptr || !ssh_key_is_private(privateKey.value)) {
        if (prompted && passphrase.isEmpty()) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("私钥已加密，需要输入保护口令");
            }
            return ImportStatus::PassphraseRequired;
        }
        if (errorMessage != nullptr) {
            *errorMessage = passphrase.isEmpty()
                ? QStringLiteral("文件不是受支持的 SSH 私钥或内容已损坏")
                : QStringLiteral("私钥口令错误、格式不受支持或内容已损坏");
        }
        return ImportStatus::Invalid;
    }

    const QString publicLine = publicKeyLine(privateKey.value, {}, errorMessage);
    if (publicLine.isEmpty()) {
        return ImportStatus::Invalid;
    }
    if (info != nullptr) {
        info->algorithm = portableType(ssh_key_type(privateKey.value));
        info->privateKeyPath = fileInfo.absoluteFilePath();
        info->publicKeyPath = fileInfo.absoluteFilePath() + QStringLiteral(".pub");
        info->publicKeyLine = publicLine;
        info->encrypted = !passphrase.isEmpty();
    }
    return ImportStatus::Valid;
}
