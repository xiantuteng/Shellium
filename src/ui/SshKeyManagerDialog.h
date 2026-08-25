#pragma once

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

/**
 * @brief SSH 密钥生成、已有私钥校验和公钥复制的应用内管理对话框。
 *
 * 对话框不保存私钥正文或口令：生成结果直接由 SshKeyManager 以 0600 权限原子写入用户
 * 选择的路径；导入只校验原文件并展示可公开的 authorized_keys 文本。这样既提供可发现的
 * 密钥管理入口，也不会在 SQLite、日志或长生命周期 QObject 中留下敏感数据副本。
 */
class SshKeyManagerDialog final : public QDialog {
    Q_OBJECT

public:
    /**
     * @brief 创建密钥管理对话框并列出当前 libssh 构建支持的算法。
     * @param parent 父窗口；用于模态层级和生命周期管理。
     */
    explicit SshKeyManagerDialog(QWidget *parent = nullptr);

private:
    /** @brief 选择新私钥的目标路径；文件必须尚不存在，避免误覆盖现有密钥。 */
    void browseGenerationPath();

    /** @brief 校验生成表单并安全写出私钥和 `.pub` 公钥。 */
    void generateKeyPair();

    /** @brief 选择要检查的已有私钥文件，不复制也不修改原文件。 */
    void browseImportPath();

    /**
     * @brief 检查已有私钥；若文件加密且口令为空，会给出可重试提示。
     *
     * 错误口令和损坏/不支持格式在 libssh API 中无法稳定区分，界面只展示统一的安全错误，
     * 不根据失败文本推测密钥内容。
     */
    void inspectPrivateKey();

    /** @brief 把当前展示的公开公钥复制到系统剪贴板；空内容时不执行。 */
    void copyPublicKey() const;

    /** @brief 根据算法切换 RSA 位数控件，避免给 Ed25519/ECDSA 展示无意义参数。 */
    void refreshAlgorithmControls();

    QComboBox *m_algorithmCombo = nullptr;
    QLineEdit *m_generationPathEdit = nullptr;
    QLineEdit *m_generationPassphraseEdit = nullptr;
    QLineEdit *m_generationPassphraseConfirmationEdit = nullptr;
    QLineEdit *m_commentEdit = nullptr;
    QSpinBox *m_rsaBitsSpin = nullptr;
    QLabel *m_generationStatusLabel = nullptr;

    QLineEdit *m_importPathEdit = nullptr;
    QLineEdit *m_importPassphraseEdit = nullptr;
    QLabel *m_importStatusLabel = nullptr;
    QPlainTextEdit *m_publicKeyEdit = nullptr;
    QPushButton *m_copyPublicKeyButton = nullptr;
};

