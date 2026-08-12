#include "ui/VaultDialog.h"

#include "security/Vault.h"
#include "ui/SettingsWidgets.h"
#include "ui/UiIcons.h"

#include <QColor>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QStyle>
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
                       state == CredentialVault::State::Uninitialized
                           ? Operation::Create
                           : Operation::Unlock,
                       allowCancel,
                       parent);
    return dialog.exec() == QDialog::Accepted;
}

bool VaultDialog::enablePasswordProtection(CredentialVault *vault, QWidget *parent)
{
    if (vault == nullptr) {
        QMessageBox::critical(parent, QObject::tr("保险箱不可用"),
                              QObject::tr("应用没有初始化凭据保险箱服务。"));
        return false;
    }
    QString stateError;
    if (vault->state(&stateError) != CredentialVault::State::Uninitialized) {
        QMessageBox::critical(
            parent,
            QObject::tr("无法启用主密码保护"),
            stateError.isEmpty() ? QObject::tr("保险箱当前状态不允许重复启用主密码保护。")
                                 : stateError);
        return false;
    }
    VaultDialog dialog(vault, Operation::Enable, true, parent);
    return dialog.exec() == QDialog::Accepted;
}

bool VaultDialog::disablePasswordProtection(CredentialVault *vault, QWidget *parent)
{
    if (vault == nullptr) {
        QMessageBox::critical(parent, QObject::tr("保险箱不可用"),
                              QObject::tr("应用没有初始化凭据保险箱服务。"));
        return false;
    }
    QString stateError;
    const CredentialVault::State state = vault->state(&stateError);
    if (state == CredentialVault::State::Corrupt
        || state == CredentialVault::State::Uninitialized) {
        QMessageBox::critical(
            parent,
            QObject::tr("无法关闭主密码保护"),
            stateError.isEmpty() ? QObject::tr("保险箱没有可供验证的主密码配置。")
                                 : stateError);
        return false;
    }
    // 不因 State::Unlocked 跳过对话框：关闭保护是安全降级，必须重新验证用户本次输入。
    VaultDialog dialog(vault, Operation::Disable, true, parent);
    return dialog.exec() == QDialog::Accepted;
}

VaultDialog::VaultDialog(CredentialVault *vault,
                         const Operation operation,
                         const bool allowCancel,
                         QWidget *parent)
    : QDialog(parent)
    , m_vault(vault)
    , m_operation(operation)
    , m_passwordEdit(new QLineEdit(this))
    , m_confirmationEdit(
          operation == Operation::Create || operation == Operation::Enable
              ? new QLineEdit(this)
              : nullptr)
    , m_passwordErrorLabel(new QLabel(this))
    , m_confirmationErrorLabel(m_confirmationEdit != nullptr ? new QLabel(this) : nullptr)
{
    const bool creating = operation == Operation::Create || operation == Operation::Enable;
    // 保险库沿用全应用的卡片和独立页脚，但主密码属于安全入口，不再套用普通设置页的
    // “左说明右控件”版式。居中的锁形徽标和说明先建立操作语境，字段再纵向排列，用户在
    // 第一次阅读时即可区分这是安全操作，而不是一般连接参数表单。
    setObjectName(QStringLiteral("VaultDialog"));
    const QString windowTitle = operation == Operation::Enable
        ? tr("启用主密码保护")
        : operation == Operation::Disable
            ? tr("关闭主密码保护")
            : creating ? tr("创建 Shellium 保险库") : tr("解锁 Shellium 保险库");
    setWindowTitle(windowTitle);
    setMinimumWidth(400);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    // 启动阶段本对话框先于 MainWindow 出现，全局样式表还没铺开，因此这里补一份等效样式；
    // 主窗口运行期间则由全局样式按用户选择的应用主题接管，避免错误地跟随系统外观。
    ShelliumUi::applyStandaloneDialogStyle(this);

    const QString introduction = operation == Operation::Enable
        ? tr("请设置新的主密码。已有主机凭据将安全迁移为主密码保护。")
        : operation == Operation::Disable
            ? tr("请输入当前主密码确认关闭。已有凭据将迁移为本机加密，随后清除主密码配置。")
            : creating
                ? tr("请设置主密码。Shellium 使用 Argon2id 派生密钥，并以 "
                     "XChaCha20-Poly1305 加密保存的 SSH 凭据。")
                : tr("保险库已锁定。请输入主密码以使用已保存的 SSH 凭据。");
    m_passwordEdit->setObjectName(QStringLiteral("vaultPasswordEdit"));
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(creating ? tr("至少 8 个字符") : tr("输入主密码"));
    m_passwordEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (m_confirmationEdit != nullptr) {
        m_confirmationEdit->setObjectName(QStringLiteral("vaultConfirmationEdit"));
        m_confirmationEdit->setEchoMode(QLineEdit::Password);
        m_confirmationEdit->setPlaceholderText(tr("再次输入以确认"));
        m_confirmationEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    // 每个字段各自保留错误标签，使错误短句始终紧贴实际出错的输入框。只用一张卡片底部的
    // 公共错误标签时，创建模式下用户还需要反向判断究竟是主密码还是确认密码有问题。
    const auto configureErrorLabel = [](QLabel *label) {
        if (label == nullptr) {
            return;
        }
        label->setObjectName(QStringLiteral("VaultErrorLabel"));
        label->setWordWrap(true);
        label->setVisible(false);
    };
    configureErrorLabel(m_passwordErrorLabel);
    configureErrorLabel(m_confirmationErrorLabel);

    auto *card = new SettingsCard(QString(), this);
    card->setProperty("vaultCredentialCard", true);
    const auto addPasswordField = [card](const QString &labelText,
                                         QLineEdit *edit,
                                         QLabel *errorLabel) {
        Q_ASSERT(edit != nullptr);
        Q_ASSERT(errorLabel != nullptr);
        auto *field = new QWidget(card);
        auto *fieldLayout = new QVBoxLayout(field);
        fieldLayout->setContentsMargins(0, 0, 0, 0);
        fieldLayout->setSpacing(5);
        auto *fieldLabel = new QLabel(labelText, field);
        fieldLabel->setObjectName(QStringLiteral("VaultFieldLabel"));
        fieldLayout->addWidget(fieldLabel);
        fieldLayout->addWidget(edit);
        fieldLayout->addWidget(errorLabel);
        card->addFullWidthWidget(field);
    };
    addPasswordField(tr("主密码"), m_passwordEdit, m_passwordErrorLabel);
    if (m_confirmationEdit != nullptr) {
        addPasswordField(tr("确认主密码"),
                         m_confirmationEdit,
                         m_confirmationErrorLabel);
    }

    if (creating) {
        // “主密码无法找回”属于一次性且不可逆的安全提醒，不能继续埋在普通说明文字末尾。
        // 独立警示框不会阻断输入，却能在用户提交前形成清晰的视觉停顿。
        auto *warningNotice = new QFrame(card);
        warningNotice->setObjectName(QStringLiteral("VaultWarningNotice"));
        auto *warningLayout = new QHBoxLayout(warningNotice);
        warningLayout->setContentsMargins(10, 9, 10, 9);
        warningLayout->setSpacing(8);
        auto *warningIcon = new QLabel(warningNotice);
        warningIcon->setObjectName(QStringLiteral("VaultWarningIcon"));
        warningIcon->setFixedSize(16, 16);
        warningIcon->setAlignment(Qt::AlignCenter);
        // 使用跨深浅背景都可辨认的中间琥珀色；提示框底色和边框仍由当前应用主题的 QSS
        // 精确控制，因此系统外观与应用主题相反时也不会选错整套颜色。
        warningIcon->setPixmap(
            UiIcons::symbol(this,
                            UiIcons::Symbol::Warning,
                            QColor(QStringLiteral("#d39a36")),
                            QSize(15, 15))
                .pixmap(QSize(15, 15)));
        auto *warningText = new QLabel(
            tr("主密码无法找回，请妥善保管。丢失后已保存的 SSH 凭据将无法解密。"),
            warningNotice);
        warningText->setObjectName(QStringLiteral("VaultWarningText"));
        warningText->setWordWrap(true);
        warningLayout->addWidget(warningIcon, 0, Qt::AlignTop);
        warningLayout->addWidget(warningText, 1);
        card->addFullWidthWidget(warningNotice);
    }

    auto *footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("SettingsFooter"));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(20, 12, 20, 12);
    footerLayout->setSpacing(10);
    const QString confirmText = operation == Operation::Enable
        ? tr("启用保护")
        : operation == Operation::Disable
            ? tr("确认关闭")
            : creating ? tr("创建保险库") : tr("解锁");
    auto *confirmButton = new QPushButton(confirmText, footer);
    confirmButton->setObjectName(QStringLiteral("vaultConfirmButton"));
    confirmButton->setProperty("primaryAction", true);
    confirmButton->setDefault(true);
    if (allowCancel) {
        auto *cancelButton = new QPushButton(tr("取消"), footer);
        cancelButton->setObjectName(QStringLiteral("vaultCancelButton"));
        footerLayout->addWidget(cancelButton);
        connect(cancelButton, &QPushButton::clicked, this, &VaultDialog::reject);
    }
    // 取消位于左侧、主操作位于右侧；首次创建不允许取消时不创建按钮，让辅助技术也不会
    // 遇到一个永远不可用的伪入口。
    footerLayout->addStretch(1);
    footerLayout->addWidget(confirmButton);

    auto *body = new QWidget(this);
    body->setObjectName(QStringLiteral("VaultBody"));
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(20, 18, 20, 18);
    bodyLayout->setSpacing(0);

    auto *securityBadge = new QLabel(body);
    securityBadge->setObjectName(QStringLiteral("VaultSecurityBadge"));
    securityBadge->setFixedSize(44, 44);
    securityBadge->setAlignment(Qt::AlignCenter);
    securityBadge->setPixmap(
        UiIcons::symbol(this,
                        UiIcons::Symbol::Lock,
                        ShelliumUi::accentColor(),
                        QSize(22, 22))
            .pixmap(QSize(22, 22)));
    const QString headingText = operation == Operation::Enable
        ? tr("启用主密码保护")
        : operation == Operation::Disable
            ? tr("关闭主密码保护")
            : creating ? tr("创建保险库") : tr("解锁保险库");
    auto *heading = new QLabel(headingText, body);
    heading->setObjectName(QStringLiteral("VaultHeading"));
    heading->setAlignment(Qt::AlignCenter);
    auto *introductionLabel = new QLabel(introduction, body);
    introductionLabel->setObjectName(QStringLiteral("VaultIntroduction"));
    introductionLabel->setAlignment(Qt::AlignCenter);
    introductionLabel->setWordWrap(true);

    bodyLayout->addWidget(securityBadge, 0, Qt::AlignHCenter);
    bodyLayout->addSpacing(14);
    bodyLayout->addWidget(heading);
    bodyLayout->addSpacing(4);
    bodyLayout->addWidget(introductionLabel);
    bodyLayout->addSpacing(16);
    bodyLayout->addWidget(card);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(body, 1);
    layout->addWidget(footer);

    connect(confirmButton, &QPushButton::clicked, this, &VaultDialog::accept);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &VaultDialog::accept);
    connect(m_passwordEdit, &QLineEdit::textEdited, this, [this](const QString &) {
        clearInlineError();
    });
    if (m_confirmationEdit != nullptr) {
        connect(m_confirmationEdit, &QLineEdit::returnPressed, this, &VaultDialog::accept);
        connect(m_confirmationEdit, &QLineEdit::textEdited, this, [this](const QString &) {
            clearInlineError();
        });
    }
    m_passwordEdit->setFocus();
}

void VaultDialog::showInlineError(const QString &message, QLineEdit *focusTarget)
{
    clearInlineError();
    if (message.isEmpty()) {
        return;
    }

    // 默认把服务端解锁/创建失败归到主密码字段；两次输入不一致时调用方会明确传入确认框，
    // 因而错误文案与红色描边始终落在真正需要用户修正的位置。
    QLineEdit *errorEdit = focusTarget != nullptr ? focusTarget : m_passwordEdit;
    QLabel *errorLabel = errorEdit == m_confirmationEdit
        ? m_confirmationErrorLabel
        : m_passwordErrorLabel;
    Q_ASSERT(errorEdit != nullptr);
    Q_ASSERT(errorLabel != nullptr);
    errorLabel->setText(message);
    errorLabel->setVisible(true);
    errorEdit->setProperty("validationError", true);
    // Qt 不会因为动态属性改变而自动重算既有 QSS。显式重新 polish 只刷新当前输入框，既能
    // 立即显示红色描边，也避免对整棵对话框对象树做代价更高且可能引起尺寸抖动的重绘。
    errorEdit->style()->unpolish(errorEdit);
    errorEdit->style()->polish(errorEdit);
    errorEdit->update();
    if (focusTarget != nullptr) {
        focusTarget->setFocus();
        focusTarget->selectAll();
    }
}

void VaultDialog::clearInlineError()
{
    const auto clearField = [](QLineEdit *edit, QLabel *label) {
        if (edit == nullptr || label == nullptr) {
            return;
        }
        label->clear();
        label->setVisible(false);
        if (!edit->property("validationError").toBool()) {
            return;
        }
        edit->setProperty("validationError", false);
        edit->style()->unpolish(edit);
        edit->style()->polish(edit);
        edit->update();
    };
    clearField(m_passwordEdit, m_passwordErrorLabel);
    clearField(m_confirmationEdit, m_confirmationErrorLabel);
}

void VaultDialog::accept()
{
    const QString password = m_passwordEdit->text();
    if (password.isEmpty()) {
        showInlineError(tr("请输入主密码。"), m_passwordEdit);
        return;
    }
    const bool creating = m_operation == Operation::Create
        || m_operation == Operation::Enable;
    if (creating && password.size() < 8) {
        showInlineError(tr("主密码至少需要 8 个字符，以降低离线猜测风险。"), m_passwordEdit);
        return;
    }
    if (creating && password != m_confirmationEdit->text()) {
        m_confirmationEdit->clear();
        showInlineError(tr("两次输入的主密码不一致，请重新确认。"), m_confirmationEdit);
        return;
    }
    clearInlineError();

    QString error;
    bool succeeded = false;
    switch (m_operation) {
    case Operation::Create:
        succeeded = m_vault->initialize(password, &error);
        break;
    case Operation::Unlock:
        succeeded = m_vault->unlock(password, &error);
        break;
    case Operation::Enable:
        succeeded = m_vault->enablePasswordProtection(password, &error);
        break;
    case Operation::Disable:
        succeeded = m_vault->disablePasswordProtection(password, &error);
        break;
    }
    // QLineEdit 内部存储无法获得操作系统级不可复制保证，但在完成派生后立即清空控件，仍能
    // 显著缩短主密码留在常规 UI 对象中的时间。
    m_passwordEdit->clear();
    if (m_confirmationEdit != nullptr) {
        m_confirmationEdit->clear();
    }
    if (!succeeded) {
        const QString operationError = m_operation == Operation::Enable
            ? tr("无法启用主密码保护：%1").arg(error)
            : m_operation == Operation::Disable
                ? tr("无法关闭主密码保护：%1").arg(error)
                : creating ? tr("创建保险库失败：%1").arg(error)
                           : tr("无法解锁保险库：%1").arg(error);
        showInlineError(operationError,
                        m_passwordEdit);
        return;
    }

    m_vault->recordActivity();
    QDialog::accept();
}
