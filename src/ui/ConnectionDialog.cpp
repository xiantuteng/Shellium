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
    setWindowTitle(tr("连接 SSH 服务器"));
    setMinimumWidth(560);

    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(22);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(tr("可选；不会保存到磁盘"));
    m_identityEdit->setPlaceholderText(tr("可选；留空则使用 SSH Agent/默认密钥"));
    for (QLineEdit *edit : {m_displayNameEdit, m_hostEdit, m_usernameEdit, m_passwordEdit}) {
        edit->setMinimumWidth(260);
    }

    auto *serverCard = new SettingsCard(tr("服务器"), this);
    serverCard->addRow(tr("显示名称"), m_displayNameEdit, tr("留空时自动使用 用户@主机:端口。"));
    serverCard->addRow(tr("主机地址"), m_hostEdit);
    serverCard->addRow(tr("用户名"), m_usernameEdit);
    serverCard->addRow(tr("端口"), new SettingsStepper(m_portSpin, serverCard));

    auto *identityRow = new QWidget(this);
    auto *identityLayout = new QHBoxLayout(identityRow);
    identityLayout->setContentsMargins(0, 0, 0, 0);
    identityLayout->setSpacing(8);
    auto *browseButton = new QPushButton(tr("浏览…"), identityRow);
    m_identityEdit->setMinimumWidth(200);
    identityLayout->addWidget(m_identityEdit, 1);
    identityLayout->addWidget(browseButton);

    auto *authCard = new SettingsCard(tr("认证"), this);
    authCard->addRow(tr("密码/私钥口令"), m_passwordEdit);
    authCard->addRow(tr("私钥文件"), identityRow);
    authCard->addRow(tr("旧版兼容"),
                     m_legacyHmacToggle,
                     tr("兼容仅支持 hmac-sha1 的旧服务器（不推荐）。"));
    authCard->addNote(
        tr("终端通过 libssh 交互式 PTY 连接；文件管理使用同一配置建立独立 SFTP 通道。"));

    auto *footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("SettingsFooter"));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(20, 12, 20, 12);
    footerLayout->setSpacing(10);
    auto *cancelButton = new QPushButton(tr("取消"), footer);
    auto *connectButton = new QPushButton(tr("连接"), footer);
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
        QMessageBox::warning(this, tr("连接信息不完整"), tr("请输入服务器主机名或 IP 地址。"));
        m_hostEdit->setFocus();
        return;
    }
    QDialog::accept();
}

void ConnectionDialog::browseIdentityFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("选择 SSH 私钥文件"));
    if (!path.isEmpty()) {
        m_identityEdit->setText(path);
    }
}
