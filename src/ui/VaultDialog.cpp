#include "ui/VaultDialog.h"

#include "security/Vault.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

bool VaultDialog::ensureUnlocked(CredentialVault *vault,
                                 QWidget *parent,
                                 const bool allowCancel)
{
    if (vault == nullptr) {
        QMessageBox::critical(parent, QObject::tr("保险库不可用"),
                              QObject::tr("应用没有初始化凭据保险库服务。"));
        return false;
    }

    QString stateError;
    const CredentialVault::State state = vault->state(&stateError);
    if (state == CredentialVault::State::Unlocked) {
        vault->recordActivity();
        return true;
    }
    if (state == CredentialVault::State::Corrupt) {
        QMessageBox::critical(parent,
                              QObject::tr("保险库数据损坏"),
                              QObject::tr("无法安全读取本地保险库：%1\n\n"
                                          "为避免覆盖已有凭据，Shellium 不会自动创建新保险库。")
                                  .arg(stateError));
        return false;
    }

    VaultDialog dialog(vault,
                       state == CredentialVault::State::Uninitialized,
                       allowCancel,
                       parent);
    return dialog.exec() == QDialog::Accepted;
}

VaultDialog::VaultDialog(CredentialVault *vault,
                         const bool creating,
                         const bool allowCancel,
                         QWidget *parent)
    : QDialog(parent)
    , m_vault(vault)
    , m_creating(creating)
    , m_messageLabel(new QLabel(this))
    , m_passwordEdit(new QLineEdit(this))
    , m_confirmationEdit(new QLineEdit(this))
{
    setWindowTitle(creating ? tr("创建 Shellium 保险库") : tr("解锁 Shellium 保险库"));
    setMinimumWidth(440);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);

    m_messageLabel->setWordWrap(true);
    m_messageLabel->setText(creating
        ? tr("请设置主密码。Shellium 使用 Argon2id 派生密钥，并以 XChaCha20-Poly1305 "
             "加密保存的 SSH 凭据。主密码无法找回，请妥善保管。")
        : tr("保险库已锁定。请输入主密码以使用已保存的 SSH 凭据。"));
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_confirmationEdit->setEchoMode(QLineEdit::Password);

    auto *form = new QFormLayout;
    form->addRow(tr("主密码："), m_passwordEdit);
    if (creating) {
        form->addRow(tr("确认主密码："), m_confirmationEdit);
    }

    const QDialogButtonBox::StandardButtons buttons = allowCancel
        ? QDialogButtonBox::Ok | QDialogButtonBox::Cancel
        : QDialogButtonBox::Ok;
    auto *buttonBox = new QDialogButtonBox(buttons, this);
    buttonBox->button(QDialogButtonBox::Ok)->setText(creating ? tr("创建保险库") : tr("解锁"));

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_messageLabel);
    layout->addLayout(form);
    layout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, this, &VaultDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &VaultDialog::reject);
    m_passwordEdit->setFocus();
}

void VaultDialog::accept()
{
    const QString password = m_passwordEdit->text();
    if (password.isEmpty()) {
        QMessageBox::warning(this, tr("主密码为空"), tr("请输入主密码。"));
        m_passwordEdit->setFocus();
        return;
    }
    if (m_creating && password.size() < 8) {
        QMessageBox::warning(this,
                             tr("主密码过短"),
                             tr("主密码至少需要 8 个字符，以降低离线猜测风险。"));
        m_passwordEdit->setFocus();
        return;
    }
    if (m_creating && password != m_confirmationEdit->text()) {
        QMessageBox::warning(this, tr("两次输入不一致"), tr("请重新确认主密码。"));
        m_confirmationEdit->clear();
        m_confirmationEdit->setFocus();
        return;
    }

    QString error;
    const bool succeeded = m_creating ? m_vault->initialize(password, &error)
                                      : m_vault->unlock(password, &error);
    // QLineEdit 内部存储无法获得操作系统级不可复制保证，但在完成派生后立即清空控件，仍能
    // 显著缩短主密码留在常规 UI 对象中的时间。
    m_passwordEdit->clear();
    m_confirmationEdit->clear();
    if (!succeeded) {
        QMessageBox::warning(this,
                             m_creating ? tr("创建保险库失败") : tr("无法解锁保险库"),
                             error);
        m_passwordEdit->setFocus();
        return;
    }

    m_vault->recordActivity();
    QDialog::accept();
}

