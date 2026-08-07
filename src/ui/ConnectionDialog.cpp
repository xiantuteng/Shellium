#include "ui/ConnectionDialog.h"

#include <QDialogButtonBox>
#include <QCheckBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
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
    , m_legacyHmacCheck(new QCheckBox(tr("兼容仅支持 hmac-sha1 的旧服务器（不推荐）"), this))
{
    setWindowTitle(tr("连接 SSH 服务器"));
    setMinimumWidth(460);

    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(22);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(tr("可选；不会保存到磁盘"));
    m_identityEdit->setPlaceholderText(tr("可选；留空则使用 SSH Agent/默认密钥"));

    auto *identityRow = new QWidget(this);
    auto *identityLayout = new QHBoxLayout(identityRow);
    identityLayout->setContentsMargins(0, 0, 0, 0);
    auto *browseButton = new QPushButton(tr("浏览…"), identityRow);
    identityLayout->addWidget(m_identityEdit, 1);
    identityLayout->addWidget(browseButton);

    auto *form = new QFormLayout;
    form->addRow(tr("显示名称："), m_displayNameEdit);
    form->addRow(tr("主机地址："), m_hostEdit);
    form->addRow(tr("用户名："), m_usernameEdit);
    form->addRow(tr("端口："), m_portSpin);
    form->addRow(tr("密码/私钥口令："), m_passwordEdit);
    form->addRow(tr("私钥文件："), identityRow);
    form->addRow(tr("旧版兼容："), m_legacyHmacCheck);

    auto *notice = new QLabel(
        tr("终端通过 libssh 交互式 PTY 连接；文件管理使用同一配置建立独立 SFTP 通道。"),
        this);
    notice->setWordWrap(true);
    notice->setStyleSheet(QStringLiteral("QLabel { color: palette(mid); }"));

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(notice);
    layout->addWidget(buttons);

    connect(browseButton, &QPushButton::clicked, this, &ConnectionDialog::browseIdentityFile);
    connect(buttons, &QDialogButtonBox::accepted, this, &ConnectionDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &ConnectionDialog::reject);
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
    m_legacyHmacCheck->setChecked(target.allowLegacyHmacSha1);
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
    result.allowLegacyHmacSha1 = m_legacyHmacCheck->isChecked();
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
