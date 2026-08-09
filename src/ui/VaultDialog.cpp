#include "ui/VaultDialog.h"

#include "security/Vault.h"
#include "ui/SettingsWidgets.h"

#include <QHBoxLayout>
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
    , m_errorLabel(new QLabel(this))
    , m_passwordEdit(new QLineEdit(this))
    , m_confirmationEdit(new QLineEdit(this))
{
    // 版式与连接对话框、设置窗口一致：卡片承载“左说明右控件”的行，底部是一条独立页脚。
    // 启动阶段本对话框先于 MainWindow 出现，全局样式表还没铺开，因此这里补一份等效样式。
    setObjectName(QStringLiteral("VaultDialog"));
    setWindowTitle(creating ? tr("创建 Shellium 保险库") : tr("解锁 Shellium 保险库"));
    setMinimumWidth(480);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    ShelliumUi::applyStandaloneDialogStyle(this);

    const QString introduction = creating
        ? tr("请设置主密码。Shellium 使用 Argon2id 派生密钥，并以 XChaCha20-Poly1305 "
             "加密保存的 SSH 凭据。主密码无法找回，请妥善保管。")
        : tr("保险库已锁定。请输入主密码以使用已保存的 SSH 凭据。");
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(tr("输入主密码"));
    m_passwordEdit->setMinimumWidth(240);
    m_confirmationEdit->setEchoMode(QLineEdit::Password);
    m_confirmationEdit->setPlaceholderText(tr("再次输入以确认"));
    m_confirmationEdit->setMinimumWidth(240);

    // 校验失败与解密失败都在卡片内联提示：再叠一层 QMessageBox 会遮住刚输错的那个输入框，
    // 用户还得先关掉弹窗才能重试。
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setVisible(false);
    m_errorLabel->setStyleSheet(QStringLiteral("color: #e5484d; font-size: 12px;"));

    auto *card = new SettingsCard(creating ? tr("创建保险库") : tr("解锁保险库"), this);
    card->addNote(introduction);
    card->addRow(tr("主密码"), m_passwordEdit);
    if (creating) {
        card->addRow(tr("确认主密码"), m_confirmationEdit);
    } else {
        // 解锁模式不需要确认框，但它已经以本对话框为父对象创建。父子关系只决定归属，
        // 不决定位置：未被任何布局接管的控件会停在 (0,0) 并按 sizeHint 绘制，表现为左上角
        // 一个盖住说明文字的空白输入框，而且仍能被 Tab 选中并接收键入的主密码。
        // 显式隐藏即可让它退出绘制与焦点链，同时保持 accept() 中的无条件 clear() 有效。
        m_confirmationEdit->setVisible(false);
    }
    card->addFullWidthWidget(m_errorLabel);

    auto *footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("SettingsFooter"));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(20, 12, 20, 12);
    footerLayout->setSpacing(10);
    auto *confirmButton = new QPushButton(creating ? tr("创建保险库") : tr("解锁"), footer);
    confirmButton->setProperty("primaryAction", true);
    confirmButton->setDefault(true);
    footerLayout->addStretch(1);
    if (allowCancel) {
        auto *cancelButton = new QPushButton(tr("取消"), footer);
        footerLayout->addWidget(cancelButton);
        connect(cancelButton, &QPushButton::clicked, this, &VaultDialog::reject);
    }
    footerLayout->addWidget(confirmButton);

    auto *body = new QWidget(this);
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(22, 20, 22, 18);
    bodyLayout->setSpacing(16);
    bodyLayout->addWidget(card);
    bodyLayout->addStretch(1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(body, 1);
    layout->addWidget(footer);

    connect(confirmButton, &QPushButton::clicked, this, &VaultDialog::accept);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &VaultDialog::accept);
    connect(m_confirmationEdit, &QLineEdit::returnPressed, this, &VaultDialog::accept);
    m_passwordEdit->setFocus();
}

void VaultDialog::showInlineError(const QString &message, QLineEdit *focusTarget)
{
    m_errorLabel->setText(message);
    m_errorLabel->setVisible(!message.isEmpty());
    if (focusTarget != nullptr) {
        focusTarget->setFocus();
        focusTarget->selectAll();
    }
}

void VaultDialog::accept()
{
    const QString password = m_passwordEdit->text();
    if (password.isEmpty()) {
        showInlineError(tr("请输入主密码。"), m_passwordEdit);
        return;
    }
    if (m_creating && password.size() < 8) {
        showInlineError(tr("主密码至少需要 8 个字符，以降低离线猜测风险。"), m_passwordEdit);
        return;
    }
    if (m_creating && password != m_confirmationEdit->text()) {
        m_confirmationEdit->clear();
        showInlineError(tr("两次输入的主密码不一致，请重新确认。"), m_confirmationEdit);
        return;
    }
    showInlineError(QString());

    QString error;
    const bool succeeded = m_creating ? m_vault->initialize(password, &error)
                                      : m_vault->unlock(password, &error);
    // QLineEdit 内部存储无法获得操作系统级不可复制保证，但在完成派生后立即清空控件，仍能
    // 显著缩短主密码留在常规 UI 对象中的时间。
    m_passwordEdit->clear();
    m_confirmationEdit->clear();
    if (!succeeded) {
        showInlineError(m_creating ? tr("创建保险库失败：%1").arg(error)
                                   : tr("无法解锁保险库：%1").arg(error),
                        m_passwordEdit);
        return;
    }

    m_vault->recordActivity();
    QDialog::accept();
}

