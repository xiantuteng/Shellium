#pragma once

#include <QString>
#include <QList>

/**
 * @brief SSH 私钥生成、校验和公钥导出的无界面服务。
 *
 * 服务只在调用栈内短暂持有口令，并把私钥直接写入用户选择的本地文件；它既不访问
 * ApplicationDatabase，也不把私钥正文或口令交给持久化层。所有文件写入使用 QSaveFile
 * 原子替换语义，新私钥权限固定为仅所有者可读写。
 */
class SshKeyManager final {
public:
    /** @brief Shellium 支持生成的现代 SSH 密钥类型。 */
    enum class Algorithm {
        Ed25519,   ///< 推荐默认值，密钥短且安全基线高。
        Rsa,       ///< 用于仍不支持 Ed25519 的兼容服务器。
        EcdsaP256  ///< NIST P-256；仅在当前 libssh 构建实际支持时展示。
    };

    /** @brief 已有私钥校验结果，供 UI 决定是否继续询问口令。 */
    enum class ImportStatus {
        Valid,               ///< 私钥有效，给定口令正确或文件未加密。
        PassphraseRequired,  ///< 文件可识别为加密私钥，但本次没有提供口令。
        Invalid              ///< 文件损坏、格式不支持或提供的口令错误。
    };

    /** @brief 生成或导入后返回的非敏感密钥信息。 */
    struct KeyInfo {
        Algorithm algorithm = Algorithm::Ed25519;
        QString privateKeyPath;
        QString publicKeyPath;
        QString publicKeyLine; ///< OpenSSH authorized_keys 格式，不含私钥数据。
        bool encrypted = false;
    };

    /** @brief 返回当前 libssh 构建实际能生成的算法列表。 */
    [[nodiscard]] static QList<Algorithm> supportedAlgorithms();

    /** @brief 返回用户可见的算法名称。 */
    [[nodiscard]] static QString algorithmName(Algorithm algorithm);

    /**
     * @brief 生成密钥对并原子写入 privateKeyPath 与 privateKeyPath + ".pub"。
     * @param algorithm 密钥算法。
     * @param privateKeyPath 私钥目标路径；目标及 .pub 均必须不存在。
     * @param passphrase 可选保护口令；仅在内存中用于导出私钥。
     * @param comment 公钥末尾注释；换行会被拒绝，防止生成额外 authorized_keys 行。
     * @param rsaBits RSA 位数，仅 RSA 使用，允许 2048、3072 或 4096。
     * @param info 成功时接收非敏感结果；可为空。
     * @param errorMessage 失败原因输出；可为空。
     * @return 两个文件均安全落盘返回 true。
     */
    static bool generate(Algorithm algorithm,
                         const QString &privateKeyPath,
                         const QString &passphrase,
                         const QString &comment,
                         int rsaBits,
                         KeyInfo *info = nullptr,
                         QString *errorMessage = nullptr);

    /**
     * @brief 校验已有私钥并导出对应的 OpenSSH 公钥文本。
     * @param privateKeyPath 已有私钥路径；文件不会被复制或修改。
     * @param passphrase 加密私钥口令；为空时可探测是否需要口令。
     * @param info 校验成功时接收密钥类型、公钥与原路径；可为空。
     * @param errorMessage 无效时接收具体原因；PassphraseRequired 也会给出提示。
     * @return Valid、PassphraseRequired 或 Invalid。
     */
    [[nodiscard]] static ImportStatus inspectPrivateKey(const QString &privateKeyPath,
                                                        const QString &passphrase,
                                                        KeyInfo *info = nullptr,
                                                        QString *errorMessage = nullptr);

private:
    SshKeyManager() = delete;
};

