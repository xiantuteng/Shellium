#include "ui/ConnectionDialog.h"

#include "ui/SettingsWidgets.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QUuid>
#include <QVBoxLayout>

ConnectionDialog::ConnectionDialog(QWidget *parent)
    : QDialog(parent)
    , m_displayNameEdit(new QLineEdit(this))
    , m_hostEdit(new QLineEdit(this))
    , m_usernameEdit(new QLineEdit(this))
    , m_portSpin(new QSpinBox(this))
    , m_passwordEdit(new QLineEdit(this))
    , m_identityEdit(new QLineEdit(this))
    , m_legacyHmacToggle(new ToggleSwitch(this))
{
    // 与设置窗口共用同一套卡片/行/开关控件族，连接参数与偏好设置因此拥有一致的字段版式：
    // 左侧是字段名与约束说明，右侧是等宽对齐的输入控件。
    setObjectName(QStringLiteral("ConnectionDialog"));
    setWindowTitle(tr("Connect to an SSH server"));
    setMinimumWidth(560);

    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(22);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(tr("Optional; not saved to disk"));
    m_identityEdit->setPlaceholderText(tr("Optional; leave empty to use the SSH agent or default key"));
    for (QLineEdit *edit : {m_displayNameEdit, m_hostEdit, m_usernameEdit, m_passwordEdit}) {
        edit->setMinimumWidth(260);
    }

    auto *serverCard = new SettingsCard(tr("Server"), this);
    serverCard->addRow(tr("Display name"), m_displayNameEdit, tr("When empty, user@host:port is used automatically."));
    serverCard->addRow(tr("Host address"), m_hostEdit);
    serverCard->addRow(tr("Username"), m_usernameEdit);
    serverCard->addRow(tr("Port"), new SettingsStepper(m_portSpin, serverCard));

    auto *identityRow = new QWidget(this);
    auto *identityLayout = new QHBoxLayout(identityRow);
    identityLayout->setContentsMargins(0, 0, 0, 0);
    identityLayout->setSpacing(8);
    auto *browseButton = new QPushButton(tr("Browse…"), identityRow);
    m_identityEdit->setMinimumWidth(200);
    identityLayout->addWidget(m_identityEdit, 1);
    identityLayout->addWidget(browseButton);

    auto *authCard = new SettingsCard(tr("Authentication"), this);
    authCard->addRow(tr("Password / private key passphrase"), m_passwordEdit);
    authCard->addRow(tr("Private key file"), identityRow);
    authCard->addRow(tr("Legacy compatibility"),
                     m_legacyHmacToggle,
                     tr("For legacy servers that only support hmac-sha1 (not recommended)."));
    authCard->addNote(
        tr("The terminal connects over a libssh interactive PTY; the file manager uses the same configuration to open a separate SFTP channel."));

    auto *footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("SettingsFooter"));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(20, 12, 20, 12);
    footerLayout->setSpacing(10);
    auto *cancelButton = new QPushButton(tr("Cancel"), footer);
    auto *connectButton = new QPushButton(tr("Connect"), footer);
    connectButton->setProperty("primaryAction", true);
    connectButton->setDefault(true);
    footerLayout->addStretch(1);
    footerLayout->addWidget(cancelButton);
    footerLayout->addWidget(connectButton);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *body = new QWidget(this);
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(22, 20, 22, 18);
    bodyLayout->setSpacing(16);
    bodyLayout->addWidget(serverCard);
    bodyLayout->addWidget(authCard);
    bodyLayout->addStretch(1);
    layout->addWidget(body, 1);
    layout->addWidget(footer);

    connect(browseButton, &QPushButton::clicked, this, &ConnectionDialog::browseIdentityFile);
    connect(connectButton, &QPushButton::clicked, this, &ConnectionDialog::accept);
    connect(cancelButton, &QPushButton::clicked, this, &ConnectionDialog::reject);
}

void ConnectionDialog::setInitialTarget(const ConnectionTarget &target)
{
    m_baseTarget = target;
    m_displayNameEdit->setText(target.displayName);
    m_hostEdit->setText(target.host);
    m_usernameEdit->setText(target.username);
    m_portSpin->setValue(target.port);
    m_passwordEdit->setText(target.password);
    m_identityEdit->setText(target.identityFile);
    m_legacyHmacToggle->setChecked(target.allowLegacyHmacSha1);
}

ConnectionTarget ConnectionDialog::target() const
{
    ConnectionTarget result = m_baseTarget;
    // 每次由对话框创建的连接都分配新 ID，避免两个标签页共享身份后混淆嵌套父子关系。
    result.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    result.displayName = m_displayNameEdit->text().trimmed();
    result.host = m_hostEdit->text().trimmed();
    result.username = m_usernameEdit->text().trimmed();
    result.port = static_cast<quint16>(m_portSpin->value());
    result.password = m_passwordEdit->text();
    result.identityFile = m_identityEdit->text().trimmed();
    result.allowLegacyHmacSha1 = m_legacyHmacToggle->isChecked();
    result.parentId.clear();
    if (result.displayName.isEmpty()) {
        result.displayName = result.connectionSpecifier();
    }
    return result;
}

void ConnectionDialog::accept()
{
    if (m_hostEdit->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("The connection details are incomplete"), tr("Enter the server host name or IP address."));
        m_hostEdit->setFocus();
        return;
    }
    QDialog::accept();
}

void ConnectionDialog::browseIdentityFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Select SSH private key file"));
    if (!path.isEmpty()) {
        m_identityEdit->setText(path);
    }
}
