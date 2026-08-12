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

    /**
     * @brief 请求用户设置并确认新主密码，然后启用保护并迁移已有凭据。
     * @param vault 应用级保险箱服务，不可为空。
     * @param parent Qt 父窗口；通常为设置对话框。
     * @return 用户完成密码设置与迁移时返回 true；取消或失败返回 false。
     *
     * 与 ensureUnlocked() 的首次初始化不同，本入口明确表达“从本机透明加密升级为主密码
     * 保护”，因此提交时调用 CredentialVault::enablePasswordProtection() 执行全量迁移。
     */
    static bool enablePasswordProtection(CredentialVault *vault,
                                         QWidget *parent = nullptr);

    /**
     * @brief 请求用户重新输入当前主密码，然后迁移凭据并清除保险箱密码配置。
     * @param vault 应用级保险箱服务，不可为空。
     * @param parent Qt 父窗口；通常为设置对话框。
     * @return 密码验证、凭据迁移和配置清除全部成功时返回 true。
     *
     * 即使当前会话已解锁也始终弹出验证框，避免临时接触已解锁电脑的人直接关闭保护。
     */
    static bool disablePasswordProtection(CredentialVault *vault,
                                          QWidget *parent = nullptr);

protected:
    /** @brief 校验两次输入并调用保险库服务；失败时保持对话框打开并展示具体原因。 */
    void accept() override;

private:
    /** @brief 对话框执行的安全操作；不同操作决定文案、字段数量和最终服务调用。 */
    enum class Operation {
        Create,  ///< 首次启动创建空保险箱，不涉及已有透明凭据迁移。
        Unlock,  ///< 解锁已经存在的保险箱。
        Enable,  ///< 从透明本机模式启用主密码保护并迁移全部凭据。
        Disable  ///< 重新验证当前主密码、降级凭据保护并删除保险箱配置。
    };

    /**
     * @brief 创建指定模式的对话框。
     * @param vault 保险库服务。
     * @param operation 当前要执行的安全操作。
     * @param allowCancel 是否显示取消按钮。
     * @param parent Qt 父窗口。
     */
    explicit VaultDialog(CredentialVault *vault,
                         Operation operation,
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
    Operation m_operation = Operation::Unlock;
    QLineEdit *m_passwordEdit = nullptr;
    QLineEdit *m_confirmationEdit = nullptr;
    QLabel *m_passwordErrorLabel = nullptr;
    QLabel *m_confirmationErrorLabel = nullptr;
};
