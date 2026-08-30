#include "ui/SshKeyManagerDialog.h"

#include "security/SshKeyManager.h"
#include "ui/UiIcons.h"

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
    , m_copyPublicKeyButton(new QPushButton(tr("Copy public key"), this))
{
    setWindowTitle(tr("SSH Key Management"));
    resize(720, 520);
    UiIcons::modernizePopup(m_algorithmCombo);
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
    auto *generationBrowseButton = new QPushButton(tr("Select…"), generationPage);
    generationPathRow->addWidget(generationBrowseButton);
    m_generationPassphraseEdit->setEchoMode(QLineEdit::Password);
    m_generationPassphraseConfirmationEdit->setEchoMode(QLineEdit::Password);
    m_generationPassphraseEdit->setPlaceholderText(tr("May be left empty; a passphrase is recommended for long-lived keys"));
    m_generationPassphraseConfirmationEdit->setPlaceholderText(tr("Enter the same passphrase again"));
    m_rsaBitsSpin->setRange(2048, 4096);
    m_rsaBitsSpin->setSingleStep(1024);
    m_rsaBitsSpin->setValue(3072);
    generationForm->addRow(tr("Algorithm:"), m_algorithmCombo);
    generationForm->addRow(tr("Private key path:"), generationPathRow);
    generationForm->addRow(tr("Protection passphrase:"), m_generationPassphraseEdit);
    generationForm->addRow(tr("Confirm passphrase:"), m_generationPassphraseConfirmationEdit);
    generationForm->addRow(tr("Public key comment:"), m_commentEdit);
    generationForm->addRow(tr("RSA bits:"), m_rsaBitsSpin);
    generationLayout->addLayout(generationForm);
    auto *generationNotice = new QLabel(
        tr("Security note: Shellium never overwrites an existing key; the private key is saved atomically with permissions readable and writable only by the current user, and the passphrase is not written to the database. Back up your private key yourself."),
        generationPage);
    generationNotice->setWordWrap(true);
    generationLayout->addWidget(generationNotice);
    auto *generateButton = new QPushButton(tr("Generate key pair"), generationPage);
    generationLayout->addWidget(generateButton, 0, Qt::AlignRight);
    m_generationStatusLabel->setWordWrap(true);
    generationLayout->addWidget(m_generationStatusLabel);
    generationLayout->addStretch(1);
    tabs->addTab(generationPage, tr("Generate"));

    auto *importPage = new QWidget(tabs);
    auto *importLayout = new QVBoxLayout(importPage);
    auto *importForm = new QFormLayout;
    auto *importPathRow = new QHBoxLayout;
    importPathRow->addWidget(m_importPathEdit, 1);
    auto *importBrowseButton = new QPushButton(tr("Select…"), importPage);
    importPathRow->addWidget(importBrowseButton);
    m_importPassphraseEdit->setEchoMode(QLineEdit::Password);
    m_importPassphraseEdit->setPlaceholderText(tr("Required for an encrypted private key; used only during this check"));
    importForm->addRow(tr("Existing private key:"), importPathRow);
    importForm->addRow(tr("Protection passphrase:"), m_importPassphraseEdit);
    importLayout->addLayout(importForm);
    auto *inspectButton = new QPushButton(tr("Check and extract the public key"), importPage);
    importLayout->addWidget(inspectButton, 0, Qt::AlignRight);
    m_importStatusLabel->setWordWrap(true);
    importLayout->addWidget(m_importStatusLabel);
    m_publicKeyEdit->setReadOnly(true);
    m_publicKeyEdit->setPlaceholderText(tr("After verification succeeds, the authorized_keys-format public key is shown here"));
    importLayout->addWidget(m_publicKeyEdit, 1);
    m_copyPublicKeyButton->setEnabled(false);
    importLayout->addWidget(m_copyPublicKeyButton, 0, Qt::AlignRight);
    tabs->addTab(importPage, tr("Check / import"));

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
        this, tr("Choose where to save the new private key"), m_generationPathEdit->text().trimmed());
    if (!path.isEmpty()) {
        m_generationPathEdit->setText(path);
    }
}

void SshKeyManagerDialog::generateKeyPair()
{
    const QString privatePath = m_generationPathEdit->text().trimmed();
    if (privatePath.isEmpty()) {
        QMessageBox::warning(this, tr("Missing save path"), tr("Choose where to save the new private key."));
        return;
    }
    if (m_generationPassphraseEdit->text()
        != m_generationPassphraseConfirmationEdit->text()) {
        QMessageBox::warning(this, tr("Passphrases do not match"), tr("The two protection passphrases do not match."));
        return;
    }
    if (m_algorithmCombo->currentIndex() < 0) {
        QMessageBox::critical(this,
                              tr("No algorithms available"),
                              tr("The current libssh build does not support generating the planned SSH key algorithm."));
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
        QMessageBox::critical(this, tr("Failed to generate key"), error);
        return;
    }

    // 生成完成后立刻清除两个口令控件，缩短敏感文本在 Qt 对象内存中的存活时间。
    m_generationPassphraseEdit->clear();
    m_generationPassphraseConfirmationEdit->clear();
    m_generationStatusLabel->setText(
        tr("Generated a %1 key.\nPrivate key: %2\nPublic key: %3")
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
        this, tr("Select an existing SSH private key"), suggested, tr("Private key files (*)"));
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
        QMessageBox::warning(this, tr("The file does not exist"), tr("Select an existing SSH private key file."));
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
        m_importStatusLabel->setText(tr("This private key is encrypted; enter the passphrase and check again."));
        m_importPassphraseEdit->setFocus();
        return;
    }
    if (status != SshKeyManager::ImportStatus::Valid) {
        QMessageBox::critical(this,
                              tr("The private key is unavailable"),
                              error.isEmpty() ? tr("The private key format is unsupported, the file is corrupt, or the passphrase is wrong.") : error);
        return;
    }

    m_importStatusLabel->setText(
        tr("Verification passed: %1%2. You can select this file in the 'Private key' field of the host editor.")
            .arg(SshKeyManager::algorithmName(info.algorithm),
                 info.encrypted ? tr(", passphrase-protected") : QString()));
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

