#pragma once

#include <QDialog>

class CredentialVault;
class QLabel;
class QLineEdit;

/**
 * @brief 创建或解锁本地凭据保险库的模态对话框。
 *
 * 对话框直接调用 CredentialVault，主密码仅短暂存在于密码输入控件和派生过程，不会写入
 * ApplicationDatabase。首次创建要求重复输入；后续解锁只显示一个密码框。配置损坏时拒绝
 * 进入创建流程，避免用新密钥覆盖已经存在但暂时不可读的凭据。
 */
class VaultDialog final : public QDialog {
    Q_OBJECT

public:
    /**
     * @brief 确保保险库已经初始化并处于解锁状态。
     * @param vault 应用级保险库服务，生命周期必须覆盖对话框。
     * @param parent Qt 父窗口；应用启动阶段可为空。
     * @param allowCancel 是否允许用户取消。取消时返回 false，由调用方决定退出或放弃操作。
     * @return 已经解锁或用户成功完成创建/解锁时返回 true。
     */
    static bool ensureUnlocked(CredentialVault *vault,
                               QWidget *parent = nullptr,
                               bool allowCancel = true);

protected:
    /** @brief 校验两次输入并调用保险库服务；失败时保持对话框打开并展示具体原因。 */
    void accept() override;

private:
    /**
     * @brief 创建指定模式的对话框。
     * @param vault 保险库服务。
     * @param creating true 表示首次创建，false 表示解锁既有保险库。
     * @param allowCancel 是否显示取消按钮。
     * @param parent Qt 父窗口。
     */
    explicit VaultDialog(CredentialVault *vault,
                         bool creating,
                         bool allowCancel,
                         QWidget *parent = nullptr);

    /**
     * @brief 在卡片内联展示一条错误，并把焦点交回需要重新输入的控件。
     * @param message 错误说明；为空表示清除当前错误。
     * @param focusTarget 需要重新获得焦点的输入框，可为空。
     */
    void showInlineError(const QString &message, QLineEdit *focusTarget = nullptr);

    /**
     * @brief 清除两个密码字段的错误文案和红色描边。
     *
     * 用户开始修正输入后立即清除旧错误，避免已经过时的校验结果继续被误认为当前状态。
     */
    void clearInlineError();

    CredentialVault *m_vault = nullptr;
    bool m_creating = false;
    QLineEdit *m_passwordEdit = nullptr;
    QLineEdit *m_confirmationEdit = nullptr;
    QLabel *m_passwordErrorLabel = nullptr;
    QLabel *m_confirmationErrorLabel = nullptr;
};
