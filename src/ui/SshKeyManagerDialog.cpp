#include "ui/SshKeyManagerDialog.h"

#include "security/SshKeyManager.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {

/**
 * @brief 为新私钥给出可预期的默认路径。
 * @return 用户主目录下 `.ssh/shellium_ed25519`；主目录不可用时退回空路径。
 *
 * 这里只提供建议而不自动创建目录：创建密钥属于安全敏感写操作，最终路径必须由用户在
 * 保存对话框中明确确认，且 SshKeyManager 会拒绝覆盖任何现有文件。
 */
QString defaultPrivateKeyPath()
{
    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    return home.isEmpty() ? QString() : home + QStringLiteral("/.ssh/shellium_ed25519");
}

} // namespace

SshKeyManagerDialog::SshKeyManagerDialog(QWidget *parent)
    : QDialog(parent)
    , m_algorithmCombo(new QComboBox(this))
    , m_generationPathEdit(new QLineEdit(defaultPrivateKeyPath(), this))
    , m_generationPassphraseEdit(new QLineEdit(this))
    , m_generationPassphraseConfirmationEdit(new QLineEdit(this))
    , m_commentEdit(new QLineEdit(QStringLiteral("shellium"), this))
    , m_rsaBitsSpin(new QSpinBox(this))
    , m_generationStatusLabel(new QLabel(this))
    , m_importPathEdit(new QLineEdit(this))
    , m_importPassphraseEdit(new QLineEdit(this))
    , m_importStatusLabel(new QLabel(this))
    , m_publicKeyEdit(new QPlainTextEdit(this))
    , m_copyPublicKeyButton(new QPushButton(tr("复制公钥"), this))
{
    setWindowTitle(tr("SSH 密钥管理"));
    resize(720, 520);
    auto *rootLayout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    rootLayout->addWidget(tabs, 1);

    auto *generationPage = new QWidget(tabs);
    auto *generationLayout = new QVBoxLayout(generationPage);
    auto *generationForm = new QFormLayout;
    for (const SshKeyManager::Algorithm algorithm : SshKeyManager::supportedAlgorithms()) {
        m_algorithmCombo->addItem(SshKeyManager::algorithmName(algorithm),
                                  static_cast<int>(algorithm));
    }
    auto *generationPathRow = new QHBoxLayout;
    generationPathRow->addWidget(m_generationPathEdit, 1);
    auto *generationBrowseButton = new QPushButton(tr("选择…"), generationPage);
    generationPathRow->addWidget(generationBrowseButton);
    m_generationPassphraseEdit->setEchoMode(QLineEdit::Password);
    m_generationPassphraseConfirmationEdit->setEchoMode(QLineEdit::Password);
    m_generationPassphraseEdit->setPlaceholderText(tr("可留空；建议为长期密钥设置口令"));
    m_generationPassphraseConfirmationEdit->setPlaceholderText(tr("再次输入相同口令"));
    m_rsaBitsSpin->setRange(2048, 4096);
    m_rsaBitsSpin->setSingleStep(1024);
    m_rsaBitsSpin->setValue(3072);
    generationForm->addRow(tr("算法："), m_algorithmCombo);
    generationForm->addRow(tr("私钥路径："), generationPathRow);
    generationForm->addRow(tr("保护口令："), m_generationPassphraseEdit);
    generationForm->addRow(tr("确认口令："), m_generationPassphraseConfirmationEdit);
    generationForm->addRow(tr("公钥注释："), m_commentEdit);
    generationForm->addRow(tr("RSA 位数："), m_rsaBitsSpin);
    generationLayout->addLayout(generationForm);
    auto *generationNotice = new QLabel(
        tr("安全说明：Shellium 不会覆盖已有密钥；私钥以仅当前用户可读写的权限原子保存，"
           "口令不会写入数据库。请自行备份私钥。"),
        generationPage);
    generationNotice->setWordWrap(true);
    generationLayout->addWidget(generationNotice);
    auto *generateButton = new QPushButton(tr("生成密钥对"), generationPage);
    generationLayout->addWidget(generateButton, 0, Qt::AlignRight);
    m_generationStatusLabel->setWordWrap(true);
    generationLayout->addWidget(m_generationStatusLabel);
    generationLayout->addStretch(1);
    tabs->addTab(generationPage, tr("生成"));

    auto *importPage = new QWidget(tabs);
    auto *importLayout = new QVBoxLayout(importPage);
    auto *importForm = new QFormLayout;
    auto *importPathRow = new QHBoxLayout;
    importPathRow->addWidget(m_importPathEdit, 1);
    auto *importBrowseButton = new QPushButton(tr("选择…"), importPage);
    importPathRow->addWidget(importBrowseButton);
    m_importPassphraseEdit->setEchoMode(QLineEdit::Password);
    m_importPassphraseEdit->setPlaceholderText(tr("加密私钥需要；只在本次检查期间使用"));
    importForm->addRow(tr("已有私钥："), importPathRow);
    importForm->addRow(tr("保护口令："), m_importPassphraseEdit);
    importLayout->addLayout(importForm);
    auto *inspectButton = new QPushButton(tr("检查并提取公钥"), importPage);
    importLayout->addWidget(inspectButton, 0, Qt::AlignRight);
    m_importStatusLabel->setWordWrap(true);
    importLayout->addWidget(m_importStatusLabel);
    m_publicKeyEdit->setReadOnly(true);
    m_publicKeyEdit->setPlaceholderText(tr("校验成功后在此显示 authorized_keys 格式公钥"));
    importLayout->addWidget(m_publicKeyEdit, 1);
    m_copyPublicKeyButton->setEnabled(false);
    importLayout->addWidget(m_copyPublicKeyButton, 0, Qt::AlignRight);
    tabs->addTab(importPage, tr("检查 / 导入"));

    auto *closeButtons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    rootLayout->addWidget(closeButtons);
    connect(closeButtons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(generationBrowseButton, &QPushButton::clicked,
            this, &SshKeyManagerDialog::browseGenerationPath);
    connect(generateButton, &QPushButton::clicked,
            this, &SshKeyManagerDialog::generateKeyPair);
    connect(importBrowseButton, &QPushButton::clicked,
            this, &SshKeyManagerDialog::browseImportPath);
    connect(inspectButton, &QPushButton::clicked,
            this, &SshKeyManagerDialog::inspectPrivateKey);
    connect(m_copyPublicKeyButton, &QPushButton::clicked,
            this, &SshKeyManagerDialog::copyPublicKey);
    connect(m_algorithmCombo, &QComboBox::currentIndexChanged,
            this, [this](const int) { refreshAlgorithmControls(); });
    refreshAlgorithmControls();
}

void SshKeyManagerDialog::browseGenerationPath()
{
    const QString path = QFileDialog::getSaveFileName(
        this, tr("选择新私钥保存位置"), m_generationPathEdit->text().trimmed());
    if (!path.isEmpty()) {
        m_generationPathEdit->setText(path);
    }
}

void SshKeyManagerDialog::generateKeyPair()
{
    const QString privatePath = m_generationPathEdit->text().trimmed();
    if (privatePath.isEmpty()) {
        QMessageBox::warning(this, tr("缺少保存路径"), tr("请选择新私钥的保存位置。"));
        return;
    }
    if (m_generationPassphraseEdit->text()
        != m_generationPassphraseConfirmationEdit->text()) {
        QMessageBox::warning(this, tr("口令不一致"), tr("两次输入的保护口令不一致。"));
        return;
    }
    if (m_algorithmCombo->currentIndex() < 0) {
        QMessageBox::critical(this,
                              tr("没有可用算法"),
                              tr("当前 libssh 构建不支持生成规划中的 SSH 密钥算法。"));
        return;
    }

    const auto algorithm = static_cast<SshKeyManager::Algorithm>(
        m_algorithmCombo->currentData().toInt());
    SshKeyManager::KeyInfo info;
    QString error;
    if (!SshKeyManager::generate(algorithm,
                                 privatePath,
                                 m_generationPassphraseEdit->text(),
                                 m_commentEdit->text(),
                                 m_rsaBitsSpin->value(),
                                 &info,
                                 &error)) {
        QMessageBox::critical(this, tr("生成密钥失败"), error);
        return;
    }

    // 生成完成后立刻清除两个口令控件，缩短敏感文本在 Qt 对象内存中的存活时间。
    m_generationPassphraseEdit->clear();
    m_generationPassphraseConfirmationEdit->clear();
    m_generationStatusLabel->setText(
        tr("已生成 %1 密钥。\n私钥：%2\n公钥：%3")
            .arg(SshKeyManager::algorithmName(info.algorithm),
                 info.privateKeyPath,
                 info.publicKeyPath));
    m_publicKeyEdit->setPlainText(info.publicKeyLine);
    m_copyPublicKeyButton->setEnabled(!info.publicKeyLine.isEmpty());
}

void SshKeyManagerDialog::browseImportPath()
{
    const QString suggested = m_importPathEdit->text().trimmed().isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
              + QStringLiteral("/.ssh")
        : m_importPathEdit->text().trimmed();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("选择已有 SSH 私钥"), suggested, tr("私钥文件 (*)"));
    if (!path.isEmpty()) {
        m_importPathEdit->setText(path);
        m_publicKeyEdit->clear();
        m_copyPublicKeyButton->setEnabled(false);
    }
}

void SshKeyManagerDialog::inspectPrivateKey()
{
    const QString privatePath = m_importPathEdit->text().trimmed();
    if (privatePath.isEmpty() || !QFileInfo::exists(privatePath)) {
        QMessageBox::warning(this, tr("文件不存在"), tr("请选择一个存在的 SSH 私钥文件。"));
        return;
    }

    SshKeyManager::KeyInfo info;
    QString error;
    const SshKeyManager::ImportStatus status = SshKeyManager::inspectPrivateKey(
        privatePath, m_importPassphraseEdit->text(), &info, &error);
    // 每次尝试后都清除口令；如果输错，用户明确重输比在控件中长期保留更安全。
    m_importPassphraseEdit->clear();
    m_publicKeyEdit->clear();
    m_copyPublicKeyButton->setEnabled(false);
    if (status == SshKeyManager::ImportStatus::PassphraseRequired) {
        m_importStatusLabel->setText(tr("该私钥已加密，请输入保护口令后重新检查。"));
        m_importPassphraseEdit->setFocus();
        return;
    }
    if (status != SshKeyManager::ImportStatus::Valid) {
        QMessageBox::critical(this,
                              tr("私钥不可用"),
                              error.isEmpty() ? tr("私钥格式不支持、文件损坏或口令错误。") : error);
        return;
    }

    m_importStatusLabel->setText(
        tr("校验通过：%1%2。可在主机编辑器的“私钥”字段选择此文件。")
            .arg(SshKeyManager::algorithmName(info.algorithm),
                 info.encrypted ? tr("，已使用口令保护") : QString()));
    m_publicKeyEdit->setPlainText(info.publicKeyLine);
    m_copyPublicKeyButton->setEnabled(!info.publicKeyLine.isEmpty());
}

void SshKeyManagerDialog::copyPublicKey() const
{
    const QString publicKey = m_publicKeyEdit->toPlainText().trimmed();
    if (!publicKey.isEmpty() && QApplication::clipboard() != nullptr) {
        QApplication::clipboard()->setText(publicKey);
    }
}

void SshKeyManagerDialog::refreshAlgorithmControls()
{
    const bool rsaSelected = m_algorithmCombo->currentIndex() >= 0
        && static_cast<SshKeyManager::Algorithm>(m_algorithmCombo->currentData().toInt())
            == SshKeyManager::Algorithm::Rsa;
    m_rsaBitsSpin->setEnabled(rsaSelected);
}

