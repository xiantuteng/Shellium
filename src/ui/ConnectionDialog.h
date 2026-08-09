#pragma once

#include "core/ConnectionTarget.h"

#include <QDialog>

class QLineEdit;
class QSpinBox;
class ToggleSwitch;

/**
 * @brief 收集一次 SSH 终端及其 SFTP 文件管理所需的连接参数。
 *
 * 密码字段可填写账户密码或私钥口令，只保留在 ConnectionTarget 的进程内副本中。
 */
class ConnectionDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ConnectionDialog(QWidget *parent = nullptr);

    /** @brief 使用当前终端目标预填表单，方便重连同一服务器。 */
    void setInitialTarget(const ConnectionTarget &target);

    /** @brief 返回用户确认后的连接配置；调用前应确保对话框已接受。 */
    [[nodiscard]] ConnectionTarget target() const;

protected:
    /** @brief 校验主机与端口等必填项，通过后才允许关闭对话框。 */
    void accept() override;

private:
    /** @brief 打开私钥文件选择器并写入路径输入框。 */
    void browseIdentityFile();

    QLineEdit *m_displayNameEdit = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QLineEdit *m_usernameEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QLineEdit *m_identityEdit = nullptr;
    /** @brief 旧服务器 hmac-sha1 兼容开关；语义与旧版复选框完全一致。 */
    ToggleSwitch *m_legacyHmacToggle = nullptr;
    ConnectionTarget m_baseTarget;
};
