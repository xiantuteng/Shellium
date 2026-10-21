#include "ai/AiSettings.h"
#include "ai/AiSettingsService.h"
#include "ai/LocalAgentCli.h"
#include "settings/BundledFonts.h"
#include "settings/TerminalSettings.h"
#include "security/Vault.h"
#include "storage/ApplicationDatabase.h"
#include "ui/ConnectionDialog.h"
#include "ui/OpenSourceLicensesDialog.h"
#include "ui/SettingsDialog.h"
#include "i18n/LocalizationService.h"
#include "ui/SettingsWidgets.h"
#include "update/UpdateChecker.h"
#include "update/UpdatePrompt.h"

#include <QLabel>

#include <algorithm>
#include <cmath>

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFrame>
#include <QGridLayout>
#include <QLineEdit>
#include <QScopeGuard>
#include <QSet>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTest>
#include <QTemporaryDir>
#include <QTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSharedPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBrowser>
#include <QToolButton>

namespace {

/**
 * @brief 只应答 GET /models 的极简网关，用来驱动设置页的"刷新模型列表"。
 *
 * 手写而不是引入 mock HTTP 依赖：刷新按钮只发一个不带请求体的 GET，读到请求头结束的空行
 * 就可以应答并关闭连接，不需要支持长连接、分块编码或任何其他路径。
 */
class MiniModelServer final : public QObject {
    Q_OBJECT

public:
    explicit MiniModelServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (m_server.hasPendingConnections()) {
                QTcpSocket *socket = m_server.nextPendingConnection();
                auto buffer = QSharedPointer<QByteArray>::create();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer]() {
                    buffer->append(socket->readAll());
                    if (!buffer->contains("\r\n\r\n")) {
                        return;
                    }
                    requestPaths.append(QString::fromUtf8(buffer->split(' ').value(1)));
                    const QByteArray response =
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                    socket->write(response);
                    socket->flush();
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
            }
        });
        m_server.listen(QHostAddress::LocalHost);
    }

    [[nodiscard]] bool isListening() const { return m_server.isListening(); }
    [[nodiscard]] QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
    }

    QByteArray body = QByteArrayLiteral(R"({"data":[{"id":"alpha-1"},{"id":"beta-2"}]})");
    QStringList requestPaths;

private:
    QTcpServer m_server;
};

} // namespace

/**
 * @brief 覆盖设置界面改版后的控件接线：步进器、开关、配色色板与分类导航。
 *
 * 改版把复选框换成 ToggleSwitch、把 spin box 换成带加减按钮的步进器、把配色下拉框换成色板
 * 卡。这些控件都不是 Qt 原生表单控件，一旦某个控件忘记接线，界面看上去完全正常但设置读不
 * 出来也存不进去，因此这里直接从“操作控件 → editedSettings()/数据库”这条链路上验证。
 */
class SettingsUiTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void stepperButtonsDriveSpinValues();
    void togglesRoundTripBooleanSettings();
    /** @brief 主密码保护开关必须分别通过设置新密码和验证当前密码后才能保存。 */
    void vaultProtectionToggleRequiresPasswordInBothDirections();
    /** @brief 关闭保护时执行忘记密码重置，必须同步自动保存设置并保留主机。 */
    void forgottenVaultPasswordResetSynchronizesSettings();
    void reconnectToggleGatesDependentRows();
    /** @brief 语言下拉必须列出候选语言，并把选择即时写入设置服务、重开时回显。 */
    void languageSelectorPersistsAndRoundTrips();
    /** @brief 密度即时入库、服务重载及外部修改回显，默认值仍为宽松。 */
    void sidebarDensityPersistsAndSynchronizes();
    /** @brief 双击侧边栏分隔条的确认提示开关可即时保存，并在设置重开后回显。 */
    void sidebarDoubleClickPromptPersistsAndSynchronizes();
    /** @brief 设置窗口打开时切换语言，卡片标题/行文案/分段控件必须就地热切换。 */
    void settingsContentRetranslatesLiveWithoutReopen();
    void colorSchemeSwatchesAreExclusiveAndPersist();
    void navigationSelectsMatchingPage();
    /** @brief 设置页不再显示保存/取消，修改必须即时入库且关闭窗口不得回滚。 */
    void changesPersistImmediatelyAndCloseDoesNotRollback();
    /** @brief 滚动历史内存上限必须能在界面上修改、立即落盘，并在重新加载后保持。 */
    void scrollbackMemoryCapPersistsAndReloads();
    /** @brief 历史最短命令长度默认 8，可在界面修改为 0（全部保存）并在重新加载后保持。 */
    void historyMinCommandLengthPersistsAndReloads();
    /** @brief 恢复默认必须和普通单项修改一样自动持久化。 */
    void restoreDefaultsPersistsImmediately();
    void connectionDialogToggleRoundTripsLegacyHmac();
    void everyThemePresetGetsUniqueSwatchAndPalette();
    /** @brief 参考图里那几套主题必须使用与其上游一致的 16 色，不能只对上背景色。 */
    void referenceThemesCarryTheirPublishedAnsiPalette();
    void colorSchemeFilterKeepsVisibleSwatchesPacked();
    void unknownColorSchemeIdFallsBackToDefault();
    void legacyTermiusColorSchemeIdMigratesToShellium();
    /** @brief 旧库 files.toggle=Ctrl+Shift+F 时必须让位给搜索主机，且不得牵连其他自定义键。 */
    void legacyFilesToggleShortcutYieldsToHostSearch();
    /** @brief Windows/Linux 上仍是旧默认值的快捷键迁到统一的 Ctrl+Shift 方案，自定义键保留。 */
    void previousShortcutDefaultsMigrateToUnifiedScheme();
    void colorSchemeSwitchKeepsApplicationTextColors();
    void fontComboOffersRecommendedFamiliesAndKeepsMissingOne();
    /** @brief 内置字体必须真的注册进 QFontDatabase，并排在字体下拉框最前面。 */
    void bundledFontsResolveToThemselvesAndLeadTheFontCombo();
    /** @brief AI 连接配置的长文本输入框必须占用卡片整行宽度，不能退回默认窄尺寸。 */
    void aiConnectionFieldsFillCardWidth();
    /** @brief 刷新按钮要把服务端模型填进下拉框，同时不覆盖用户已经手填的模型名。 */
    void aiModelRefreshFillsComboWithoutDiscardingManualEntry();
    /** @brief 模型名和思考开关都要能存进 AiSettingsService 并在重开设置页时回显。 */
    void aiModelAndThinkingToggleRoundTrip();
    /**
     * @brief 模型来源切到本地 CLI 后只显示 CLI 卡，CLI 选择、路径、模型与执行策略都能
     *        持久化并在重开时回显。
     */
    void aiLocalCliAndExecutionPolicyRoundTrip();
    /** @brief “关于”页必须展示应用版本、版权声明，并提供可在外部浏览器打开的官网链接。 */
    void aboutPageShowsVersionCopyrightAndWebsite();
    /**
     * @brief 开源组件清单必须覆盖全部直接依赖与内置字体，每条都能读到许可证全文，
     *        资源里也不能有无人引用的许可证文件（说明清单与资源已经分叉）。
     */
    void openSourceNoticesCoverEveryDistributedComponent();
    /** @brief 关于页的按钮要打开许可证对话框，并展示 LGPL 声明与许可证要求的署名。 */
    void aboutPageOpensOpenSourceLicenses();
    /**
     * @brief 自动检查开关即时入库；跳过的版本在关于页可见、可清除，并且修改其他设置时
     *        不会被冲掉（editedSettings() 从空白快照构建，这个字段没有控件）。
     */
    void updateSettingsPersistAndSkippedVersionSurvivesOtherEdits();
    /** @brief 关于页“立即检查”发现新版本后弹出提示，点“跳过此版本”要把版本号写入设置。 */
    void manualUpdateCheckPromptsAndSkipRecordsVersion();

private:
    /** @brief 返回步进器里指定角色（increase/decrease）的按钮。 */
    static QToolButton *stepperButton(QAbstractSpinBox *spinBox, const QString &role);

    /** @brief 创建一个绑定内存数据库的设置服务，测试之间互不影响。 */
    std::unique_ptr<TerminalSettingsService> makeService();

    std::unique_ptr<ApplicationDatabase> m_database;
};

void SettingsUiTest::initTestCase()
{
    m_database = std::make_unique<ApplicationDatabase>(ApplicationDatabase::inMemoryPath());
    QString error;
    QVERIFY2(m_database->initialize(&error), qPrintable(error));
}

std::unique_ptr<TerminalSettingsService> SettingsUiTest::makeService()
{
    auto service = std::make_unique<TerminalSettingsService>(m_database.get());
    QString error;
    // QVERIFY 只能用在返回 void 的槽里，这里用 qFatal 直接失败：设置服务初始化不成功时后续
    // 断言都没有意义。
    if (!service->save(TerminalSettings{}.normalized(), &error)) {
        qFatal("无法写入测试用默认设置：%s", qPrintable(error));
    }
    return service;
}

QToolButton *SettingsUiTest::stepperButton(QAbstractSpinBox *spinBox, const QString &role)
{
    if (spinBox == nullptr || spinBox->parentWidget() == nullptr) {
        return nullptr;
    }
    const QList<QToolButton *> buttons =
        spinBox->parentWidget()->findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly);
    for (QToolButton *button : buttons) {
        if (button->property("stepperRole").toString() == role) {
            return button;
        }
    }
    return nullptr;
}

void SettingsUiTest::stepperButtonsDriveSpinValues()
{
    auto service = makeService();
    SettingsDialog dialog(service.get());

    auto *fontSizeSpin = dialog.findChild<QSpinBox *>(QStringLiteral("terminalFontSizeSpin"));
    QVERIFY(fontSizeSpin != nullptr);
    const int originalSize = fontSizeSpin->value();

    QToolButton *increase = stepperButton(fontSizeSpin, QStringLiteral("increase"));
    QToolButton *decrease = stepperButton(fontSizeSpin, QStringLiteral("decrease"));
    QVERIFY(increase != nullptr);
    QVERIFY(decrease != nullptr);

    increase->click();
    increase->click();
    QCOMPARE(fontSizeSpin->value(), originalSize + 2);
    QCOMPARE(dialog.editedSettings().fontPointSize, originalSize + 2);

    decrease->click();
    QCOMPARE(dialog.editedSettings().fontPointSize, originalSize + 1);

    // 浮点步进器共用同一实现，单步长度由 QDoubleSpinBox 自己决定，这里确认包装没有吃掉它。
    auto *letterSpacingSpin =
        dialog.findChild<QDoubleSpinBox *>(QStringLiteral("terminalLetterSpacingSpin"));
    QVERIFY(letterSpacingSpin != nullptr);
    const double originalSpacing = letterSpacingSpin->value();
    stepperButton(letterSpacingSpin, QStringLiteral("increase"))->click();
    QCOMPARE(letterSpacingSpin->value(), originalSpacing + letterSpacingSpin->singleStep());
    QCOMPARE(dialog.editedSettings().letterSpacing, letterSpacingSpin->value());
}

void SettingsUiTest::togglesRoundTripBooleanSettings()
{
    auto service = makeService();
    SettingsDialog dialog(service.get());

    auto *bellToggle = dialog.findChild<ToggleSwitch *>(QStringLiteral("audibleBellToggle"));
    auto *blinkToggle = dialog.findChild<ToggleSwitch *>(QStringLiteral("cursorBlinkToggle"));
    auto *completionToggle =
        dialog.findChild<ToggleSwitch *>(QStringLiteral("commandAutoCompletionToggle"));
    auto *pasteConfirmToggle =
        dialog.findChild<ToggleSwitch *>(QStringLiteral("multiLinePasteConfirmationToggle"));
    auto *vaultPasswordToggle =
        dialog.findChild<ToggleSwitch *>(QStringLiteral("vaultPasswordEnabledToggle"));
    QVERIFY(pasteConfirmToggle != nullptr);
    QVERIFY(bellToggle != nullptr);
    QVERIFY(blinkToggle != nullptr);
    QVERIFY(completionToggle != nullptr);
    QVERIFY(vaultPasswordToggle != nullptr);
    // 打开对话框时开关必须反映已持久化的值，而不是一律显示默认关闭。
    QCOMPARE(bellToggle->isChecked(), service->currentSettings().audibleBell);
    QCOMPARE(blinkToggle->isChecked(), service->currentSettings().cursorBlink);
    // 命令自动提示属于可能打断既有输入习惯的新功能，首次启动和恢复默认都必须保持关闭。
    QVERIFY(!service->currentSettings().commandAutoCompletion);
    QVERIFY(!completionToggle->isChecked());
    // 多行粘贴确认默认关闭，即直接粘贴；这是与其他终端一致的开箱行为。
    QVERIFY(!service->currentSettings().multiLinePasteConfirmation);
    QVERIFY(!pasteConfirmToggle->isChecked());
    QVERIFY(!vaultPasswordToggle->isChecked());

    // 点击开关会立即保存并改写服务里的运行设置，因此期望值必须取点击前的快照，不能在
    // 第一次点击后再把 currentSettings() 当作原值。
    const bool originalBell = service->currentSettings().audibleBell;
    const bool originalBlink = service->currentSettings().cursorBlink;
    bellToggle->click();
    blinkToggle->click();
    completionToggle->click();
    pasteConfirmToggle->click();
    QCOMPARE(dialog.editedSettings().audibleBell, !originalBell);
    QCOMPARE(dialog.editedSettings().cursorBlink, !originalBlink);
    QVERIFY(dialog.editedSettings().commandAutoCompletion);
    QVERIFY(dialog.editedSettings().multiLinePasteConfirmation);
    // 该用例使用不带 CredentialVault 的兼容构造入口，因此不触发需要密码交互的安全开关。
    // 默认关闭值仍必须正确显示并持久化，真正的双向密码迁移由 CredentialVault 专项用例覆盖。
    QVERIFY(!dialog.editedSettings().vaultPasswordEnabled);

    const bool expectedBell = !originalBell;
    QCOMPARE(service->currentSettings().audibleBell, expectedBell);
    QCOMPARE(m_database->setting(QStringLiteral("terminal/audible_bell")).value_or(QString()),
             expectedBell ? QStringLiteral("1") : QStringLiteral("0"));
    QCOMPARE(m_database->setting(QStringLiteral("terminal/command_auto_completion"))
                 .value_or(QString()),
             QStringLiteral("1"));
    QCOMPARE(m_database->setting(QStringLiteral("terminal/multi_line_paste_confirmation"))
                 .value_or(QString()),
             QStringLiteral("1"));
    QCOMPARE(m_database->setting(QStringLiteral("security/vault_password_enabled"))
                 .value_or(QString()),
             QStringLiteral("0"));
}

void SettingsUiTest::vaultProtectionToggleRequiresPasswordInBothDirections()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    ApplicationDatabase database(
        temporaryDirectory.filePath(QStringLiteral("settings-vault-toggle.db")));
    QString error;
    QVERIFY2(database.initialize(&error), qPrintable(error));
    TerminalSettingsService service(&database);
    QVERIFY2(service.load(&error), qPrintable(error));
    CredentialVault vault(database);
    const QString masterPassword = QStringLiteral("设置页双向验证主密码-2026");

    SettingsDialog enablingDialog(&service, &vault, nullptr);
    auto *enableToggle = enablingDialog.findChild<ToggleSwitch *>(
        QStringLiteral("vaultPasswordEnabledToggle"));
    QVERIFY(enableToggle != nullptr);
    QString interactionFailure;
    // 自动保存把安全验证移动到了开关点击当下，因此必须在 click() 前排队填写模态密码窗口。
    QTimer::singleShot(0, [&interactionFailure, &masterPassword]() {
        QWidget *dialog = QApplication::activeModalWidget();
        auto *passwordEdit = dialog == nullptr ? nullptr : dialog->findChild<QLineEdit *>(
            QStringLiteral("vaultPasswordEdit"));
        auto *confirmationEdit = dialog == nullptr ? nullptr : dialog->findChild<QLineEdit *>(
            QStringLiteral("vaultConfirmationEdit"));
        auto *confirmButton = dialog == nullptr ? nullptr : dialog->findChild<QAbstractButton *>(
            QStringLiteral("vaultConfirmButton"));
        if (passwordEdit == nullptr || confirmationEdit == nullptr || confirmButton == nullptr) {
            interactionFailure = QStringLiteral("启用主密码保护时没有要求输入并确认新密码");
            if (dialog != nullptr) {
                dialog->close();
            }
            return;
        }
        passwordEdit->setText(masterPassword);
        confirmationEdit->setText(masterPassword);
        confirmButton->click();
    });
    enableToggle->click();
    QVERIFY2(interactionFailure.isEmpty(), qPrintable(interactionFailure));
    QVERIFY(enablingDialog.editedSettings().vaultPasswordEnabled);
    QVERIFY(vault.passwordProtectionEnabled());
    QVERIFY(service.currentSettings().vaultPasswordEnabled);

    SettingsDialog disablingDialog(&service, &vault, nullptr);
    auto *disableToggle = disablingDialog.findChild<ToggleSwitch *>(
        QStringLiteral("vaultPasswordEnabledToggle"));
    QVERIFY(disableToggle != nullptr);
    QVERIFY(disableToggle->isChecked());
    interactionFailure.clear();
    QTimer::singleShot(0, [&interactionFailure, &masterPassword]() {
        QWidget *dialog = QApplication::activeModalWidget();
        auto *passwordEdit = dialog == nullptr ? nullptr : dialog->findChild<QLineEdit *>(
            QStringLiteral("vaultPasswordEdit"));
        const auto *confirmationEdit = dialog == nullptr ? nullptr
                                                         : dialog->findChild<QLineEdit *>(
                                                               QStringLiteral(
                                                                   "vaultConfirmationEdit"));
        auto *confirmButton = dialog == nullptr ? nullptr : dialog->findChild<QAbstractButton *>(
            QStringLiteral("vaultConfirmButton"));
        if (passwordEdit == nullptr || confirmationEdit != nullptr || confirmButton == nullptr) {
            interactionFailure = QStringLiteral("关闭主密码保护时没有要求单独验证当前密码");
            if (dialog != nullptr) {
                dialog->close();
            }
            return;
        }
        passwordEdit->setText(masterPassword);
        confirmButton->click();
    });
    disableToggle->click();
    QVERIFY2(interactionFailure.isEmpty(), qPrintable(interactionFailure));
    QVERIFY(!vault.passwordProtectionEnabled());
    QVERIFY(!service.currentSettings().vaultPasswordEnabled);
    QVERIFY(!database.vaultConfiguration(&error).has_value());
}

void SettingsUiTest::forgottenVaultPasswordResetSynchronizesSettings()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    ApplicationDatabase database(
        temporaryDirectory.filePath(QStringLiteral("settings-forgotten-vault-reset.db")));
    QString error;
    QVERIFY2(database.initialize(&error), qPrintable(error));

    CredentialVault vault(database);
    HostRecord host;
    host.id = QStringLiteral("settings-reset-host");
    host.name = QStringLiteral("设置页重置后保留的主机");
    host.host = QStringLiteral("203.0.113.26");
    host.identityFile = QStringLiteral("/tmp/preserved_id_ed25519");
    ConnectionTarget target = host.toConnectionTarget();
    target.password = QStringLiteral("重置后删除的口令");
    QVERIFY2(vault.saveConnectionPassword(host, target, {}, &error), qPrintable(error));
    QVERIFY2(vault.enablePasswordProtection(
                 QStringLiteral("设置页模拟遗忘的主密码-2026"), &error),
             qPrintable(error));
    vault.lock();

    // 设置服务在保险箱启用后加载，确保对话框初始开关与数据库中的真实安全状态一致。
    TerminalSettingsService service(&database);
    QVERIFY2(service.load(&error), qPrintable(error));
    QVERIFY(service.currentSettings().vaultPasswordEnabled);

    SettingsDialog dialog(&service, &vault, nullptr);
    auto *vaultToggle = dialog.findChild<ToggleSwitch *>(
        QStringLiteral("vaultPasswordEnabledToggle"));
    QVERIFY(vaultToggle != nullptr);
    QVERIFY(vaultToggle->isChecked());

    QString interactionFailure;
    QTimer::singleShot(0, [&interactionFailure]() {
        QWidget *disableDialog = QApplication::activeModalWidget();
        auto *forgotButton = disableDialog == nullptr ? nullptr
                                                      : disableDialog->findChild<QAbstractButton *>(
                                                            QStringLiteral(
                                                                "vaultForgotPasswordButton"));
        if (forgotButton == nullptr) {
            interactionFailure = QStringLiteral("关闭主密码保护窗口缺少忘记密码入口");
            if (disableDialog != nullptr) {
                disableDialog->close();
            }
            return;
        }
        QTimer::singleShot(0, [&interactionFailure]() {
            QWidget *confirmationDialog = QApplication::activeModalWidget();
            auto *confirmationEdit = confirmationDialog == nullptr ? nullptr
                                                                    : confirmationDialog
                                                                          ->findChild<QLineEdit *>(
                                                                              QStringLiteral(
                                                                                  "vaultResetConfirmationEdit"));
            auto *resetButton = confirmationDialog == nullptr ? nullptr
                                                               : confirmationDialog
                                                                     ->findChild<QAbstractButton *>(
                                                                         QStringLiteral(
                                                                             "vaultResetConfirmButton"));
            if (confirmationEdit == nullptr || resetButton == nullptr) {
                interactionFailure = QStringLiteral("设置页重置没有要求输入确认短语");
                if (confirmationDialog != nullptr) {
                    confirmationDialog->close();
                }
                return;
            }
            confirmationEdit->setText(QStringLiteral("Reset vault"));
            resetButton->click();
        });
        forgotButton->click();
    });
    vaultToggle->click();

    QVERIFY2(interactionFailure.isEmpty(), qPrintable(interactionFailure));
    QVERIFY(!vault.passwordProtectionEnabled());
    QVERIFY(!vaultToggle->isChecked());
    QVERIFY(!service.currentSettings().vaultPasswordEnabled);
    QCOMPARE(database.setting(QStringLiteral("security/vault_password_enabled"), &error)
                 .value_or(QString()),
             QStringLiteral("0"));
    const std::optional<HostRecord> preservedHost = database.findHost(host.id, &error);
    QVERIFY2(preservedHost.has_value(), qPrintable(error));
    QVERIFY(preservedHost->credentialId.isEmpty());
    QCOMPARE(preservedHost->host, host.host);
    QCOMPARE(preservedHost->identityFile, host.identityFile);
}

void SettingsUiTest::reconnectToggleGatesDependentRows()
{
    auto service = makeService();
    SettingsDialog dialog(service.get());

    auto *reconnectToggle =
        dialog.findChild<ToggleSwitch *>(QStringLiteral("automaticReconnectToggle"));
    auto *attemptsSpin = dialog.findChild<QSpinBox *>(QStringLiteral("reconnectMaxAttemptsSpin"));
    QVERIFY(reconnectToggle != nullptr);
    QVERIFY(attemptsSpin != nullptr);
    QVERIFY(reconnectToggle->isChecked());
    QVERIFY(attemptsSpin->isEnabled());

    reconnectToggle->click();
    QVERIFY(!dialog.editedSettings().automaticReconnect);
    // 步进器的加减按钮与数值输入必须整体置灰，否则关闭自动重连后仍能改一个无效的重试次数。
    QVERIFY(!attemptsSpin->isEnabled());
    QVERIFY(!stepperButton(attemptsSpin, QStringLiteral("increase"))->isEnabled());

    reconnectToggle->click();
    QVERIFY(attemptsSpin->isEnabled());
    QVERIFY(stepperButton(attemptsSpin, QStringLiteral("increase"))->isEnabled());
}

void SettingsUiTest::sidebarDensityPersistsAndSynchronizes()
{
    auto service = makeService();
    SettingsDialog dialog(service.get());
    auto *selector = dialog.findChild<SegmentedControl *>(QStringLiteral("sidebarDensitySelector"));
    QVERIFY(selector != nullptr);
    QCOMPARE(selector->currentData(), 0);
    QSignalSpy changes(service.get(), &TerminalSettingsService::settingsChanged);
    selector->setCurrentData(1);
    QCOMPARE(service->currentSettings().sidebarDensity, SidebarDensity::Compact);
    QCOMPARE(changes.count(), 1);
    QCOMPARE(m_database->setting(QStringLiteral("layout/sidebar_compact")).value_or(QString()),
             QStringLiteral("compact"));

    // 精简是第三档，选择后同样要落库并广播。
    selector->setCurrentData(2);
    QCOMPARE(service->currentSettings().sidebarDensity, SidebarDensity::Minimal);
    QCOMPARE(changes.count(), 2);
    QCOMPARE(m_database->setting(QStringLiteral("layout/sidebar_compact")).value_or(QString()),
             QStringLiteral("minimal"));

    // 新服务从数据库重载，覆盖重新打开设置的回显。
    TerminalSettingsService reloaded(m_database.get());
    QString error;
    QVERIFY2(reloaded.load(&error), qPrintable(error));
    QCOMPARE(reloaded.currentSettings().sidebarDensity, SidebarDensity::Minimal);
    SettingsDialog reopened(&reloaded);
    QCOMPARE(reopened.findChild<SegmentedControl *>(QStringLiteral("sidebarDensitySelector"))->currentData(), 2);

    // 模拟菜单通过同一服务修改；控件应同步且不再次触发保存。
    TerminalSettings relaxed = service->currentSettings();
    relaxed.sidebarDensity = SidebarDensity::Relaxed;
    QVERIFY2(service->save(relaxed, &error), qPrintable(error));
    QCOMPARE(selector->currentData(), 0);
    QCOMPARE(changes.count(), 3);

    // 旧版本菜单只写过布尔值 "1"；精简模式上线后仍要能读出紧凑，不能因未知格式回退宽松。
    QString legacyError;
    QVERIFY2(m_database->setSetting(QStringLiteral("layout/sidebar_compact"), QStringLiteral("1"),
                                    &legacyError),
             qPrintable(legacyError));
    TerminalSettingsService legacyReloaded(m_database.get());
    QVERIFY2(legacyReloaded.load(&error), qPrintable(error));
    QCOMPARE(legacyReloaded.currentSettings().sidebarDensity, SidebarDensity::Compact);
}

void SettingsUiTest::sidebarDoubleClickPromptPersistsAndSynchronizes()
{
    auto service = makeService();
    SettingsDialog dialog(service.get());
    auto *promptToggle =
        dialog.findChild<ToggleSwitch *>(QStringLiteral("sidebarDoubleClickPromptToggle"));
    QVERIFY(promptToggle != nullptr);
    QVERIFY(promptToggle->isChecked());
    QVERIFY(service->currentSettings().sidebarDoubleClickPrompt);

    promptToggle->click();
    QVERIFY(!dialog.editedSettings().sidebarDoubleClickPrompt);
    QVERIFY(!service->currentSettings().sidebarDoubleClickPrompt);
    QCOMPARE(m_database->intSetting(QStringLiteral("layout/sidebar_double_click_prompt"))
                 .value_or(-1),
             0);

    // 新建服务并重新打开设置，验证该开关不是只停留在当前对话框的控件状态中。
    TerminalSettingsService reloaded(m_database.get());
    QString error;
    QVERIFY2(reloaded.load(&error), qPrintable(error));
    QVERIFY(!reloaded.currentSettings().sidebarDoubleClickPrompt);
    SettingsDialog reopened(&reloaded);
    auto *reopenedToggle = reopened.findChild<ToggleSwitch *>(
        QStringLiteral("sidebarDoubleClickPromptToggle"));
    QVERIFY(reopenedToggle != nullptr);
    QVERIFY(!reopenedToggle->isChecked());

    // 通过同一服务改回开启，覆盖“误选不再提示后在设置中恢复”的入口。
    TerminalSettings restored = reloaded.currentSettings();
    restored.sidebarDoubleClickPrompt = true;
    QVERIFY2(reloaded.save(restored, &error), qPrintable(error));
    QVERIFY(reopenedToggle->isChecked());
}

void SettingsUiTest::languageSelectorPersistsAndRoundTrips()
{
    auto service = makeService();
    QCOMPARE(service->currentSettings().language, QStringLiteral("system"));

    {
        SettingsDialog dialog(service.get());
        auto *selector = dialog.findChild<QComboBox *>(QStringLiteral("languageSelector"));
        QVERIFY(selector != nullptr);
        QVERIFY(selector->count() >= 2);
        // 语言顺序是面向用户的稳定约定：系统跟随固定置顶，中文和英文紧随其后，日语固定置底。
        // 这里直接检查控件数据而不是显示文字，避免当前测试语言或翻译状态影响顺序断言。
        const QStringList expectedLeadingAndTrailingCodes = {
            QStringLiteral("system"), QStringLiteral("zh_CN"), QStringLiteral("zh_TW"),
            QStringLiteral("en"),
        };
        QCOMPARE(selector->count(), LocalizationService::availableLanguages().size());
        for (qsizetype index = 0; index < expectedLeadingAndTrailingCodes.size(); ++index) {
            QCOMPARE(selector->itemData(static_cast<int>(index)).toString(),
                     expectedLeadingAndTrailingCodes.at(index));
        }
        QCOMPARE(selector->itemData(selector->count() - 1).toString(), QStringLiteral("ja"));

        // modernizePopup() 应在构造阶段移除 Qt 弹出容器的 StyledPanel；否则即使内部列表有
        // 圆角，容器仍会在外侧画出一圈直角边框。保留该断言可防止语言下拉遗漏统一处理。
        QAbstractItemView *popupView = selector->view();
        QVERIFY(popupView != nullptr);
        auto *popupFrame = qobject_cast<QFrame *>(popupView->window());
        QVERIFY(popupFrame != nullptr);
        QCOMPARE(popupFrame->frameShape(), QFrame::NoFrame);
        QVERIFY(popupFrame->testAttribute(Qt::WA_TranslucentBackground));

        // 实际弹出会触发 Qt 对容器的样式刷新，单纯检查构造结果无法覆盖用户看到的外框。
        // 连续两次刷新样式并展开，确保首次显示和主题切换后都不会恢复直角框架。
        dialog.show();
        for (const QString &surface : {QStringLiteral("white"), QStringLiteral("#20252b")}) {
            dialog.setStyleSheet(QStringLiteral(
                "QComboBox QAbstractItemView { background: %1; border: 1px solid gray; "
                "border-radius: 10px; padding: 6px; }").arg(surface));
            selector->showPopup();
            QApplication::processEvents();
            QVERIFY(popupFrame->isVisible());
            // QFrame::NoFrame 只禁止 Qt 内容绘制；原生弹出窗口还需明确禁用系统窗口装饰，
            // 否则 macOS 仍可能在圆角列表之外绘制黑色直角外框。
            QVERIFY(popupFrame->windowFlags().testFlag(Qt::FramelessWindowHint));
            QVERIFY(popupFrame->windowFlags().testFlag(Qt::NoDropShadowWindowHint));
            QCOMPARE(popupFrame->windowType(), Qt::Popup);
            QCOMPARE(popupFrame->frameShape(), QFrame::NoFrame);
            selector->hidePopup();
        }

        const int englishIndex = selector->findData(QStringLiteral("en"));
        QVERIFY(englishIndex >= 0);
        selector->setCurrentIndex(englishIndex);
        QCOMPARE(dialog.editedSettings().language, QStringLiteral("en"));
    }

    QCOMPARE(service->currentSettings().language, QStringLiteral("en"));

    SettingsDialog reopened(service.get());
    auto *reopenedSelector =
        reopened.findChild<QComboBox *>(QStringLiteral("languageSelector"));
    QVERIFY(reopenedSelector != nullptr);
    QCOMPARE(reopenedSelector->currentData().toString(), QStringLiteral("en"));

    QString error;
    QVERIFY2(service->save(TerminalSettings{}.normalized(), &error), qPrintable(error));
}

void SettingsUiTest::settingsContentRetranslatesLiveWithoutReopen()
{
    auto service = makeService();
    LocalizationService localization;
    QVERIFY(localization.apply(QStringLiteral("en")));

    SettingsDialog dialog(service.get());
    const QList<QLabel *> titles =
        dialog.findChildren<QLabel *>(QStringLiteral("SettingsCardTitle"));
    QVERIFY(!titles.isEmpty());
    const auto hasTitle = [&titles](const QString &text) {
        for (const QLabel *label : titles) {
            if (label->text() == text) {
                return true;
            }
        }
        return false;
    };
    QVERIFY2(hasTitle(QStringLiteral("Interface language")), "英文源语言下应有英文卡片标题");

    QVERIFY(localization.apply(QStringLiteral("zh_CN")));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LanguageChange);

    // 卡片标题、行文案、分段控件都必须就地切换，无需重开设置窗口。
    QVERIFY2(hasTitle(QString::fromUtf8("界面语言")), "设置窗口打开状态下卡片标题未热切换");
    QVERIFY2(hasTitle(QString::fromUtf8("应用主题")), "设置窗口打开状态下卡片标题未热切换");
    auto *themeSelector = dialog.findChild<SegmentedControl *>(QStringLiteral("themeSelector"));
    QVERIFY(themeSelector != nullptr);
    bool followSystemInChinese = false;
    for (const QAbstractButton *button : themeSelector->findChildren<QAbstractButton *>()) {
        followSystemInChinese =
            followSystemInChinese || button->text() == QString::fromUtf8("跟随系统");
    }
    QVERIFY2(followSystemInChinese, "分段控件文案未热切换");
    // 密度分段也必须在原控件上重译，并保留当前数值选择。
    auto *densitySelector = dialog.findChild<SegmentedControl *>(QStringLiteral("sidebarDensitySelector"));
    QVERIFY(densitySelector != nullptr);
    QStringList densityLabels;
    for (const auto *button : densitySelector->findChildren<QAbstractButton *>()) {
        densityLabels.append(button->text());
    }
    QVERIFY(densityLabels.contains(QStringLiteral("紧凑")));
    QVERIFY(densityLabels.contains(QStringLiteral("宽松")));

    // QSpinBox 后缀是一次性 setSuffix，必须显式登记进热切换闭包。
    auto *scrollbackSpin = dialog.findChild<QSpinBox *>(QStringLiteral("scrollbackSpin"));
    QVERIFY(scrollbackSpin != nullptr);
    QVERIFY2(!scrollbackSpin->suffix().trimmed().isEmpty()
                 && scrollbackSpin->suffix() != QStringLiteral(" lines"),
             QString::fromUtf8("滚动历史 spin 的后缀未随语言热切换：'%1'")
                 .arg(scrollbackSpin->suffix())
                 .toUtf8());
    auto *retriesSpin =
        dialog.findChild<QSpinBox *>(QStringLiteral("reconnectMaxAttemptsSpin"));
    QVERIFY(retriesSpin != nullptr);
    QVERIFY2(retriesSpin->suffix() != QStringLiteral(" times"),
             "重连次数 spin 的后缀未随语言热切换");

    // AI 设置页的宽行标签与「测试连接 / 保存」按钮同样必须就地热切换。
    auto *aiSave = dialog.findChild<QPushButton *>(QStringLiteral("settingsAiSave"));
    auto *aiTest = dialog.findChild<QPushButton *>(QStringLiteral("settingsAiTest"));
    QVERIFY(aiSave != nullptr);
    QVERIFY(aiTest != nullptr);
    QVERIFY2(aiSave->text() != QStringLiteral("Save"), "AI 页“保存”按钮未热切换");
    QVERIFY2(aiTest->text() != QStringLiteral("Test connection"),
             "AI 页“测试连接”按钮未热切换");
    bool aiServiceUrlLabelTranslated = false;
    bool shortcutActionNameTranslated = false;
    for (const QLabel *label :
         dialog.findChildren<QLabel *>(QStringLiteral("SettingsRowLabel"))) {
        if (label->text() == QString::fromUtf8("服务地址")) {
            aiServiceUrlLabelTranslated = true;
        }
        if (label->text() == QString::fromUtf8("连接服务器")) {
            shortcutActionNameTranslated = true;
        }
        QVERIFY2(label->text() != QStringLiteral("Service URL"),
                 "AI 页宽行标签未热切换");
        QVERIFY2(label->text() != QStringLiteral("Connect to Server"),
                 "快捷键设置页动作名未热切换（shortcutDisplayName 被 static 缓存冻结）");
    }
    QVERIFY2(aiServiceUrlLabelTranslated, "AI 页“服务地址”宽行标签未热切换");
    QVERIFY2(shortcutActionNameTranslated, "快捷键设置页动作名未热切换");

    // 配色色板的“默认”标题（themePresets() 里唯一被翻译的名字）必须实时重译。
    auto *defaultSwatch =
        dialog.findChild<ThemeSwatchButton *>(QStringLiteral("colorSchemeSwatch_default"));
    QVERIFY(defaultSwatch != nullptr);
    QCOMPARE(defaultSwatch->text(), QString::fromUtf8("默认"));

    QVERIFY(localization.apply(QStringLiteral("en")));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LanguageChange);
    QVERIFY(hasTitle(QStringLiteral("Interface language")));
    QCOMPARE(scrollbackSpin->suffix(), QStringLiteral(" lines"));
    QCOMPARE(aiSave->text(), QStringLiteral("Save"));
}

void SettingsUiTest::colorSchemeSwatchesAreExclusiveAndPersist()
{
    auto service = makeService();
    const QString draculaName = QStringLiteral("colorSchemeSwatch_dracula");
    const QString defaultName = QStringLiteral("colorSchemeSwatch_default");

    {
        SettingsDialog dialog(service.get());
        auto *defaultSwatch = dialog.findChild<ThemeSwatchButton *>(defaultName);
        auto *draculaSwatch = dialog.findChild<ThemeSwatchButton *>(draculaName);
        QVERIFY(defaultSwatch != nullptr);
        QVERIFY(draculaSwatch != nullptr);
        QVERIFY(defaultSwatch->isChecked());

        draculaSwatch->click();
        // 色板卡不属于同一个 QButtonGroup，互斥完全靠对话框自己维护；漏掉取消上一张卡会让
        // editedSettings() 依赖 QHash 的遍历顺序，从而随机返回旧配色。
        QVERIFY(draculaSwatch->isChecked());
        QVERIFY(!defaultSwatch->isChecked());
        QCOMPARE(dialog.editedSettings().colorSchemeId, QStringLiteral("dracula"));
    }

    QCOMPARE(service->currentSettings().colorSchemeId, QStringLiteral("dracula"));

    SettingsDialog reopened(service.get());
    QVERIFY(reopened.findChild<ThemeSwatchButton *>(draculaName)->isChecked());
    QVERIFY(!reopened.findChild<ThemeSwatchButton *>(defaultName)->isChecked());

    QString error;
    QVERIFY2(service->save(TerminalSettings{}.normalized(), &error), qPrintable(error));
}

void SettingsUiTest::navigationSelectsMatchingPage()
{
    auto service = makeService();
    SettingsDialog dialog(service.get());

    auto *navigation = dialog.findChild<QListWidget *>(QStringLiteral("SettingsNav"));
    auto *pages = dialog.findChild<QStackedWidget *>(QStringLiteral("SettingsPages"));
    QVERIFY(navigation != nullptr);
    QVERIFY(pages != nullptr);
    QCOMPARE(navigation->count(), pages->count());
    QCOMPARE(pages->currentIndex(), 0);

    for (int row = navigation->count() - 1; row >= 0; --row) {
        navigation->setCurrentRow(row);
        QCOMPARE(pages->currentIndex(), row);
    }
}

void SettingsUiTest::changesPersistImmediatelyAndCloseDoesNotRollback()
{
    auto service = makeService();
    const TerminalSettings before = service->currentSettings();

    SettingsDialog dialog(service.get());
    // 自动保存模式只保留语义明确的关闭按钮；继续显示保存或取消会让用户误以为设置仍是草稿。
    QVERIFY(dialog.findChild<QAbstractButton *>(QStringLiteral("settingsSave")) == nullptr);
    QVERIFY(dialog.findChild<QAbstractButton *>(QStringLiteral("settingsCancel")) == nullptr);
    QVERIFY(dialog.findChild<QAbstractButton *>(QStringLiteral("settingsClose")) != nullptr);

    QSignalSpy changeSpy(service.get(), &TerminalSettingsService::settingsChanged);
    stepperButton(dialog.findChild<QSpinBox *>(QStringLiteral("terminalFontSizeSpin")),
                  QStringLiteral("increase"))
        ->click();
    QVERIFY(changeSpy.count() > 0);
    QCOMPARE(service->currentSettings().fontPointSize, before.fontPointSize + 1);
    QCOMPARE(m_database->setting(QStringLiteral("terminal/font_size")).value_or(QString()),
             QString::number(before.fontPointSize + 1));

    // Esc/窗口关闭仍会调用 reject()，但已自动保存的值不允许再恢复为打开窗口时的旧快照。
    dialog.reject();
    QCOMPARE(service->currentSettings().fontPointSize, before.fontPointSize + 1);
    QCOMPARE(m_database->setting(QStringLiteral("terminal/font_size")).value_or(QString()),
             QString::number(before.fontPointSize + 1));
}

void SettingsUiTest::scrollbackMemoryCapPersistsAndReloads()
{
    // 紧凑历史格式下 10000 行只占几 MB，默认值不必再为省内存而牺牲可回溯的历史；这里同时
    // 锁住两个默认值，防止有人再用"砍历史"换内存。
    const TerminalSettings defaults = TerminalSettings{}.normalized();
    QCOMPARE(defaults.scrollbackLines, 10000);
    QCOMPARE(defaults.scrollbackMaxMegabytes, 32);

    auto service = makeService();
    SettingsDialog dialog(service.get());
    auto *capSpin = dialog.findChild<QSpinBox *>(QStringLiteral("scrollbackMaxMegabytesSpin"));
    QVERIFY(capSpin != nullptr);
    QCOMPARE(capSpin->value(), 32);

    capSpin->setValue(48);
    QCOMPARE(service->currentSettings().scrollbackMaxMegabytes, 48);
    QCOMPARE(m_database->setting(QStringLiteral("terminal/scrollback_max_megabytes"))
                 .value_or(QString()),
             QStringLiteral("48"));

    TerminalSettingsService reloaded(m_database.get());
    QString error;
    QVERIFY2(reloaded.load(&error), qPrintable(error));
    QCOMPARE(reloaded.currentSettings().scrollbackMaxMegabytes, 48);

    // 越界值按 [1, 512] 夹取，不能把 0 当成"无上限"写进去。
    TerminalSettings outOfRange = reloaded.currentSettings();
    outOfRange.scrollbackMaxMegabytes = 0;
    QCOMPARE(outOfRange.normalized().scrollbackMaxMegabytes, 1);
    outOfRange.scrollbackMaxMegabytes = 100000;
    QCOMPARE(outOfRange.normalized().scrollbackMaxMegabytes, 512);
}

void SettingsUiTest::historyMinCommandLengthPersistsAndReloads()
{
    QCOMPARE(TerminalSettings{}.normalized().historyMinCommandLength, 8);

    auto service = makeService();
    SettingsDialog dialog(service.get());
    auto *spin = dialog.findChild<QSpinBox *>(QStringLiteral("historyMinCommandLengthSpin"));
    QVERIFY(spin != nullptr);
    QCOMPARE(spin->value(), 8);
    QCOMPARE(spin->minimum(), 0);

    spin->setValue(0);
    QCOMPARE(service->currentSettings().historyMinCommandLength, 0);
    QCOMPARE(m_database->setting(QStringLiteral("terminal/history_min_command_length"))
                 .value_or(QString()),
             QStringLiteral("0"));

    TerminalSettingsService reloaded(m_database.get());
    QString error;
    QVERIFY2(reloaded.load(&error), qPrintable(error));
    QCOMPARE(reloaded.currentSettings().historyMinCommandLength, 0);

    TerminalSettings outOfRange = reloaded.currentSettings();
    outOfRange.historyMinCommandLength = -5;
    QCOMPARE(outOfRange.normalized().historyMinCommandLength, 0);
    outOfRange.historyMinCommandLength = 100000;
    QCOMPARE(outOfRange.normalized().historyMinCommandLength, 256);
}

void SettingsUiTest::restoreDefaultsPersistsImmediately()
{
    auto service = makeService();
    TerminalSettings customized = service->currentSettings();
    customized.fontPointSize = 23;
    // 密度也应参与整份默认快照恢复，不能遗留菜单单独保存的紧凑值。
    customized.sidebarDensity = SidebarDensity::Compact;
    customized.audibleBell = !TerminalSettings{}.normalized().audibleBell;
    customized.automaticUpdateCheck = false;
    customized.skippedUpdateVersion = QStringLiteral("9.9.9");
    QString error;
    QVERIFY2(service->save(customized, &error), qPrintable(error));

    SettingsDialog dialog(service.get());
    auto *restoreButton = dialog.findChild<QAbstractButton *>(
        QStringLiteral("settingsRestoreDefaults"));
    QVERIFY(restoreButton != nullptr);
    restoreButton->click();

    const TerminalSettings defaults = TerminalSettings{}.normalized();
    QCOMPARE(service->currentSettings().fontPointSize, defaults.fontPointSize);
    QCOMPARE(service->currentSettings().sidebarDensity, SidebarDensity::Relaxed);
    QCOMPARE(service->currentSettings().audibleBell, defaults.audibleBell);
    QCOMPARE(m_database->setting(QStringLiteral("terminal/font_size")).value_or(QString()),
             QString::number(defaults.fontPointSize));
    // 跳过的版本没有控件，恢复默认仍必须清掉，否则用户无法通过恢复默认撤销“跳过此版本”。
    QVERIFY(service->currentSettings().automaticUpdateCheck);
    QVERIFY(service->currentSettings().skippedUpdateVersion.isEmpty());
    QCOMPARE(m_database->setting(QStringLiteral("update/skipped_version")).value_or(QStringLiteral("missing")),
             QString());
}

void SettingsUiTest::connectionDialogToggleRoundTripsLegacyHmac()
{
    ConnectionDialog dialog;
    ConnectionTarget target;
    target.host = QStringLiteral("10.0.0.8");
    target.username = QStringLiteral("deploy");
    target.port = 2222;
    target.allowLegacyHmacSha1 = true;
    dialog.setInitialTarget(target);

    auto *legacyToggle = dialog.findChild<ToggleSwitch *>();
    QVERIFY(legacyToggle != nullptr);
    QVERIFY(legacyToggle->isChecked());
    QCOMPARE(dialog.target().port, quint16{2222});
    QVERIFY(dialog.target().allowLegacyHmacSha1);

    legacyToggle->click();
    QVERIFY(!dialog.target().allowLegacyHmacSha1);
}

void SettingsUiTest::everyThemePresetGetsUniqueSwatchAndPalette()
{
    auto service = makeService();
    SettingsDialog dialog(service.get());

    const QList<TerminalThemePreset> &presets = TerminalSettings::themePresets();
    QVERIFY(presets.size() >= 60);
    // Shellium 自有配色排在色板最前，其后紧跟“默认”。
    QCOMPARE(presets.at(0).id, QStringLiteral("shellium_dark"));
    QCOMPARE(presets.at(1).id, QStringLiteral("shellium_light"));
    QCOMPARE(presets.at(2).id, QStringLiteral("default"));

    QSet<QString> seenIds;
    QSet<QString> seenNames;
    for (const TerminalThemePreset &preset : presets) {
        // id 重复会让持久化与色板互斥同时错乱：两张卡共用一个 objectName，选中态互相覆盖。
        QVERIFY2(!seenIds.contains(preset.id), qPrintable(preset.id));
        QVERIFY2(!seenNames.contains(preset.name), qPrintable(preset.name));
        seenIds.insert(preset.id);
        seenNames.insert(preset.name);

        auto *swatch = dialog.findChild<ThemeSwatchButton *>(
            QStringLiteral("colorSchemeSwatch_%1").arg(preset.id));
        QVERIFY2(swatch != nullptr, qPrintable(preset.id));

        // 每套预设都必须给出四种有效颜色，且背景与前景不能相同——否则终端里字就看不见了。
        TerminalSettings probe = service->currentSettings();
        probe.colorSchemeId = preset.id;
        const TerminalPalette palette = probe.normalized().effectivePalette();
        QVERIFY2(palette.background.isValid() && palette.foreground.isValid()
                     && palette.cursor.isValid() && palette.selection.isValid()
                     && palette.link.isValid(),
                 qPrintable(preset.id));
        QVERIFY2(palette.background != palette.foreground, qPrintable(preset.id));
        // 16 色表必须齐全：远端用 SGR 30-37/90-97 着色的输出（`ls`、grep、vim）在缺表时会
        // 退回 libvterm 的 VGA 值，深色主题上的蓝色目录名几乎看不见。
        QCOMPARE(palette.ansi.size(), 16);
        for (int index = 0; index < palette.ansi.size(); ++index) {
            const QColor &color = palette.ansi.at(index);
            const QString where = QStringLiteral("%1 ansi[%2]").arg(preset.id).arg(index);
            QVERIFY2(color.isValid(), qPrintable(where));
            // 0 号（黑）在深色主题上贴着背景是正常的，其余各号与背景完全相同则意味着该号
            // 色的输出会整段消失。
            if (index == 0 || index == 8) {
                continue;
            }
            QVERIFY2(color != palette.background, qPrintable(where));
        }
        // 除“默认”外，预设颜色不随主题模式改变。
        if (preset.id != QLatin1String("default")) {
            probe.themeMode = ThemeMode::Light;
            QCOMPARE(probe.normalized().effectivePalette().background, palette.background);
        }
    }
}

void SettingsUiTest::referenceThemesCarryTheirPublishedAnsiPalette()
{
    // 逐色核对过的主题必须和上游（或参考截图）完全一致：这些值一旦被"顺手调亮一点"，
    // 用户看到的就不再是他选的那套配色。抽查每套主题最容易被看出来的几号色。
    struct Expectation {
        const char *id;
        int index;
        const char *hex;
    };
    static const Expectation expectations[] = {
        {"kanagawa_wave", 4, "#7e9cd8"},   // 目录名用的 ANSI 蓝
        {"kanagawa_wave", 2, "#76946a"},
        {"kanagawa_dragon", 1, "#c4746e"},
        {"kanagawa_lotus", 4, "#4d699b"},
        {"flexoki_dark", 4, "#4385be"},
        {"flexoki_dark", 1, "#d14d41"},
        {"flexoki_light", 4, "#205ea6"},
        // `ll` 可能通过普通蓝或粗体亮蓝呈现目录名，两个色槽必须
        // 都保持为用户指定的 #015fff，避免远端配置差异改变目录名颜色。
        {"shellium_dark", 4, "#015fff"},
        {"shellium_dark", 12, "#015fff"},
        {"shellium_light", 4, "#2f6fd0"},
        {"dracula", 4, "#bd93f9"},
        {"nord", 4, "#81a1c1"},
        {"gruvbox_dark", 4, "#458588"},
        {"solarized_dark", 4, "#268bd2"},
        {"tokyo_night", 4, "#7aa2f7"},
        {"catppuccin_mocha", 4, "#89b4fa"},
    };
    for (const Expectation &expectation : expectations) {
        TerminalSettings probe;
        probe.colorSchemeId = QString::fromLatin1(expectation.id);
        const TerminalPalette palette = probe.normalized().effectivePalette();
        QCOMPARE(palette.ansi.value(expectation.index),
                 QColor(QLatin1String(expectation.hex)));
    }

    // Shellium 自有配色是自拟的，没有上游可对照，因此直接按对比度把关：正文相对背景至少
    // 7:1，ANSI 彩色（红绿黄蓝品青及其亮色）至少 3:1。旧版深色正文用的是高饱和品牌绿，
    // 整屏绿字久看刺眼，正文因此必须是近乎无彩的中性色。
    const auto luminance = [](const QColor &color) {
        const auto channel = [](const qreal value) {
            return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF())
            + 0.0722 * channel(color.blueF());
    };
    const auto contrast = [&luminance](const QColor &first, const QColor &second) {
        const qreal a = luminance(first);
        const qreal b = luminance(second);
        return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
    };
    for (const char *id : {"shellium_dark", "shellium_light"}) {
        TerminalSettings shellium;
        shellium.colorSchemeId = QString::fromLatin1(id);
        const TerminalPalette palette = shellium.normalized().effectivePalette();
        QVERIFY2(contrast(palette.foreground, palette.background) >= 7.0, id);
        // 用通道极差（色度）而不是 HSV 饱和度衡量：深灰的 HSV 饱和度会被放大，而旧版的
        // #3ecf8f 色度约 0.57，远超中性色。
        const QColor fg = palette.foreground;
        const int chroma = std::max({fg.red(), fg.green(), fg.blue()})
            - std::min({fg.red(), fg.green(), fg.blue()});
        QVERIFY2(chroma < 40, id);
        for (const int index : {1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14}) {
            QVERIFY2(contrast(palette.ansi.at(index), palette.background) >= 3.0,
                     qPrintable(QStringLiteral("%1 ansi[%2]").arg(QLatin1String(id)).arg(index)));
        }
    }

    // 默认（跟随明暗）的深色调色板同样对着参考截图定过 4 号蓝：`ls` 的目录名用的就是它，
    // 退回偏暗或偏灰的蓝正是"终端文字看不清"最先被看出来的地方。
    TerminalSettings builtin;
    builtin.themeMode = ThemeMode::Dark;
    QCOMPARE(builtin.normalized().effectivePalette().ansi.value(4),
             QColor(QStringLiteral("#015fff")));

    // Hacker 系是单色主题：参考图里连目录名都只是同一色系的另一档亮度，出现互补色就说明
    // 有人把通用调色板塞了进来。
    TerminalSettings hacker;
    hacker.colorSchemeId = QStringLiteral("hacker_green");
    for (const QColor &color : hacker.normalized().effectivePalette().ansi) {
        QVERIFY2(color.green() >= color.red() && color.green() >= color.blue(),
                 qPrintable(color.name()));
    }
}

void SettingsUiTest::colorSchemeFilterKeepsVisibleSwatchesPacked()
{
    auto service = makeService();
    SettingsDialog dialog(service.get());
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    auto *filter = dialog.findChild<QLineEdit *>(QStringLiteral("colorSchemeFilter"));
    QVERIFY(filter != nullptr);
    auto *gruvboxDark =
        dialog.findChild<ThemeSwatchButton *>(QStringLiteral("colorSchemeSwatch_gruvbox_dark"));
    auto *dracula =
        dialog.findChild<ThemeSwatchButton *>(QStringLiteral("colorSchemeSwatch_dracula"));
    QVERIFY(gruvboxDark != nullptr);
    QVERIFY(dracula != nullptr);

    filter->setText(QStringLiteral("gruv"));
    QVERIFY(gruvboxDark->isVisible());
    QVERIFY(!dracula->isVisible());
    // 过滤后可见卡片必须从第一行第一列重新排布；只 hide 不重排会在网格里留下大片空洞。
    auto *grid = qobject_cast<QGridLayout *>(gruvboxDark->parentWidget()->layout());
    QVERIFY(grid != nullptr);
    const int index = grid->indexOf(gruvboxDark);
    QVERIFY(index >= 0);
    int row = -1;
    int column = -1;
    int rowSpan = 0;
    int columnSpan = 0;
    grid->getItemPosition(index, &row, &column, &rowSpan, &columnSpan);
    QCOMPARE(row, 0);
    QCOMPARE(column, 0);

    filter->setText(QStringLiteral("绝不存在的配色"));
    QVERIFY(!gruvboxDark->isVisible());

    filter->clear();
    QVERIFY(gruvboxDark->isVisible());
    QVERIFY(dracula->isVisible());
}

void SettingsUiTest::unknownColorSchemeIdFallsBackToDefault()
{
    // normalized() 自己就必须挡住未知 id：设置结构会被直接构造后传给终端（例如
    // applyTransient 预览、测试与将来的导入配置路径），这些路径不经过 load() 的校验。
    TerminalSettings handmade;
    handmade.colorSchemeId = QStringLiteral("no_such_theme");
    QCOMPARE(handmade.normalized().colorSchemeId, QStringLiteral("default"));
    QVERIFY(handmade.normalized().effectivePalette().foreground.isValid());

    auto service = makeService();
    QString error;
    // 手工改库或旧版本可能留下一个已经删掉的配色 id；此时必须回退默认，而不是让终端拿到
    // 一套空颜色（背景与前景都是无效 QColor，渲染出来是纯黑或纯白）。
    QVERIFY(m_database->setSetting(QStringLiteral("appearance/color_scheme"),
                                   QStringLiteral("no_such_theme"),
                                   &error));
    QVERIFY2(service->load(&error), qPrintable(error));
    QCOMPARE(service->currentSettings().colorSchemeId, QStringLiteral("default"));
    QVERIFY(service->currentSettings().effectivePalette().background.isValid());

    SettingsDialog dialog(service.get());
    QVERIFY(dialog.findChild<ThemeSwatchButton *>(QStringLiteral("colorSchemeSwatch_default"))
                ->isChecked());
}

void SettingsUiTest::legacyTermiusColorSchemeIdMigratesToShellium()
{
    // 0.2.5 及更早版本把 Shellium 配色存为 termius_*。改名后若不迁移，未知 id 校验会把
    // 这些用户静默打回“默认”，等于丢了他们选好的配色。
    TerminalSettings handmade;
    handmade.colorSchemeId = QStringLiteral("termius_dark");
    QCOMPARE(handmade.normalized().colorSchemeId, QStringLiteral("shellium_dark"));
    handmade.colorSchemeId = QStringLiteral("termius_light");
    QCOMPARE(handmade.normalized().colorSchemeId, QStringLiteral("shellium_light"));

    auto service = makeService();
    QString error;
    QVERIFY(m_database->setSetting(QStringLiteral("appearance/color_scheme"),
                                   QStringLiteral("termius_dark"),
                                   &error));
    QVERIFY2(service->load(&error), qPrintable(error));
    QCOMPARE(service->currentSettings().colorSchemeId, QStringLiteral("shellium_dark"));

    SettingsDialog dialog(service.get());
    QVERIFY(dialog.findChild<ThemeSwatchButton *>(QStringLiteral("colorSchemeSwatch_shellium_dark"))
                ->isChecked());
    // 旧 id 只用于迁移，不能再作为一张独立的色板卡出现。
    QVERIFY(dialog.findChild<ThemeSwatchButton *>(QStringLiteral("colorSchemeSwatch_termius_dark"))
            == nullptr);
}

void SettingsUiTest::legacyFilesToggleShortcutYieldsToHostSearch()
{
    const QString hostSearchKey = QStringLiteral("shortcut/hosts.search");
    const QString filesToggleKey = QStringLiteral("shortcut/files.toggle");
    const QString newTabKey = QStringLiteral("shortcut/tab.new");
    const QString lockKey = QStringLiteral("shortcut/vault.lock");
    const QKeySequence hostSearchDefault =
        TerminalSettings::defaultShortcuts().value(QStringLiteral("hosts.search"));
    auto service = makeService();
    QString error;

    // 旧版本整表写库：files.toggle 存着旧默认 Ctrl+Shift+F，库里还没有 hosts.search。
    QVERIFY(m_database->removeSetting(hostSearchKey, &error));
    QVERIFY(m_database->setSetting(filesToggleKey, QStringLiteral("Ctrl+Shift+F"), &error));
    QVERIFY(m_database->setSetting(newTabKey, QStringLiteral("Ctrl+Alt+T"), &error));
    QVERIFY2(service->load(&error), qPrintable(error));
    QCOMPARE(service->shortcut(QStringLiteral("hosts.search")), hostSearchDefault);
    QCOMPARE(service->shortcut(QStringLiteral("files.toggle")),
             QKeySequence(QStringLiteral("Ctrl+Shift+O")));
    QVERIFY2(service->shortcut(QStringLiteral("tab.new")) == QKeySequence(QStringLiteral("Ctrl+Alt+T")),
             "迁移不能把用户的其他自定义快捷键打回默认");

    // 用户已把新默认键 Ctrl+Shift+O 占给别的动作：files.toggle 留空，而不是制造冲突。
    QVERIFY(m_database->removeSetting(hostSearchKey, &error));
    QVERIFY(m_database->setSetting(filesToggleKey, QStringLiteral("Ctrl+Shift+F"), &error));
    QVERIFY(m_database->setSetting(lockKey, QStringLiteral("Ctrl+Shift+O"), &error));
    QVERIFY2(service->load(&error), qPrintable(error));
    QVERIFY(service->shortcut(QStringLiteral("files.toggle")).isEmpty());
    QCOMPARE(service->shortcut(QStringLiteral("vault.lock")),
             QKeySequence(QStringLiteral("Ctrl+Shift+O")));
    QCOMPARE(service->shortcut(QStringLiteral("tab.new")), QKeySequence(QStringLiteral("Ctrl+Alt+T")));

    // 用户把搜索主机的默认键自定义给了别的动作：搜索主机留空，原有绑定保持不变。
    QVERIFY(m_database->removeSetting(hostSearchKey, &error));
    QVERIFY(m_database->setSetting(filesToggleKey, QStringLiteral("Ctrl+Alt+F"), &error));
    QVERIFY(m_database->setSetting(lockKey, hostSearchDefault.toString(QKeySequence::PortableText),
                                   &error));
    QVERIFY2(service->load(&error), qPrintable(error));
    QVERIFY(service->shortcut(QStringLiteral("hosts.search")).isEmpty());
    QCOMPARE(service->shortcut(QStringLiteral("vault.lock")), hostSearchDefault);
    QCOMPARE(service->shortcut(QStringLiteral("files.toggle")),
             QKeySequence(QStringLiteral("Ctrl+Alt+F")));

    // 后续用例共用同一个数据库，恢复默认设置。
    QVERIFY2(service->save(TerminalSettings{}.normalized(), &error), qPrintable(error));
}

void SettingsUiTest::previousShortcutDefaultsMigrateToUnifiedScheme()
{
#if defined(Q_OS_MACOS)
    QSKIP("macOS 的 Command 快捷键不参与 Ctrl+Shift 统一方案");
#else
    const QString schemeKey = QStringLiteral("shortcut_scheme/version");
    auto service = makeService();
    QString error;
    const auto store = [this, &error](const QString &id, const QString &sequence) {
        return m_database->setSetting(QStringLiteral("shortcut/") + id, sequence, &error);
    };

    // 旧版本整表写入的旧默认值：裸 Ctrl 键与 Ctrl+Alt 键全部迁到 Ctrl+Shift。
    QVERIFY2(service->save(TerminalSettings{}.normalized(), &error), qPrintable(error));
    QVERIFY(m_database->removeSetting(schemeKey, &error));
    QVERIFY(store(QStringLiteral("tab.new"), QStringLiteral("Ctrl+T")));
    QVERIFY(store(QStringLiteral("tab.close"), QStringLiteral("Ctrl+W")));
    QVERIFY(store(QStringLiteral("hosts.search"), QStringLiteral("Ctrl+Shift+F")));
    QVERIFY(store(QStringLiteral("terminal.search"), QStringLiteral("Ctrl+F")));
    QVERIFY(store(QStringLiteral("snippets.quick"), QStringLiteral("Ctrl+Alt+K")));
    QVERIFY(store(QStringLiteral("history.quick"), QStringLiteral("Ctrl+Alt+J")));
    QVERIFY2(service->load(&error), qPrintable(error));
    QCOMPARE(service->currentSettings().shortcuts, TerminalSettings::defaultShortcuts());

    // 用户自定义过的键原样保留；新键被自定义占用时该动作留空，不制造冲突。
    QVERIFY(m_database->removeSetting(schemeKey, &error));
    QVERIFY(store(QStringLiteral("tab.new"), QStringLiteral("Ctrl+Alt+T")));
    QVERIFY(store(QStringLiteral("tab.close"), QStringLiteral("Ctrl+W")));
    QVERIFY(store(QStringLiteral("vault.lock"), QStringLiteral("Ctrl+Shift+W")));
    QVERIFY2(service->load(&error), qPrintable(error));
    QCOMPARE(service->shortcut(QStringLiteral("tab.new")), QKeySequence(QStringLiteral("Ctrl+Alt+T")));
    QVERIFY(service->shortcut(QStringLiteral("tab.close")).isEmpty());
    QCOMPARE(service->shortcut(QStringLiteral("vault.lock")),
             QKeySequence(QStringLiteral("Ctrl+Shift+W")));

    // 保存写入方案版本后，用户再自选旧键位不会被改回。
    TerminalSettings custom = TerminalSettings{}.normalized();
    custom.shortcuts.insert(QStringLiteral("tab.close"), QKeySequence(QStringLiteral("Ctrl+W")));
    QVERIFY2(service->save(custom, &error), qPrintable(error));
    QVERIFY2(service->load(&error), qPrintable(error));
    QCOMPARE(service->shortcut(QStringLiteral("tab.close")), QKeySequence(QStringLiteral("Ctrl+W")));

    QVERIFY2(service->save(TerminalSettings{}.normalized(), &error), qPrintable(error));
#endif
}

void SettingsUiTest::colorSchemeSwitchKeepsApplicationTextColors()
{
    auto service = makeService();
    const QPalette originalApplicationPalette = QApplication::palette();
    service->bindApplicationTheme(qApp);

    TerminalSettings light;
    light.themeMode = ThemeMode::Light;
    light.colorSchemeId = QStringLiteral("default");
    service->applyTransient(light);
    const QPalette before = QApplication::palette();

    TerminalSettings switched = light;
    switched.colorSchemeId = QStringLiteral("hacker_green");
    service->applyTransient(switched);
    const QPalette after = QApplication::palette();

    // 前提：这两套终端配色的前景色确实不同，否则下面的断言恒成立、测不到任何东西。
    QVERIFY(light.normalized().effectivePalette().foreground
            != switched.normalized().effectivePalette().foreground);

    // 终端配色只作用于终端网格。一旦它被掺进应用 QPalette，换配色就会连带改掉设置界面里
    // 所有文字（包括配色卡自己的名称）的颜色，深色终端配色 + 浅色界面会直接看不清。
    QCOMPARE(after.color(QPalette::WindowText), before.color(QPalette::WindowText));
    QCOMPARE(after.color(QPalette::Text), before.color(QPalette::Text));
    QCOMPARE(after.color(QPalette::ButtonText), before.color(QPalette::ButtonText));
    QCOMPARE(after.color(QPalette::Highlight), before.color(QPalette::Highlight));
    QCOMPARE(after.color(QPalette::Link), before.color(QPalette::Link));
    // 浅色界面下的正文必须是深色：这条挡住“文字色跟着深色终端配色变浅”的回归。
    QVERIFY(after.color(QPalette::WindowText).lightnessF() < 0.5);

    QApplication::setPalette(originalApplicationPalette);
}

void SettingsUiTest::fontComboOffersRecommendedFamiliesAndKeepsMissingOne()
{
    auto service = makeService();
    const QStringList bundled = BundledFonts::registerAll();
    const QStringList installed = QFontDatabase::families();
    QStringList expectedRecommended;
    for (const QString &family : TerminalSettings::recommendedFontFamilies()) {
        // 内置字体自成一组排在更前面，不应在推荐组里重复出现。
        if (installed.contains(family) && !bundled.contains(family)) {
            expectedRecommended.append(family);
        }
    }
    QVERIFY2(!expectedRecommended.isEmpty(), "本机没有任何推荐编程字体，用例无法覆盖");

    {
        SettingsDialog dialog(service.get());
        auto *fontCombo = dialog.findChild<QComboBox *>(QStringLiteral("terminalFontCombo"));
        QVERIFY(fontCombo != nullptr);
        // 推荐字体紧跟在内置字体分组和分隔条之后，且顺序与推荐列表一致；否则用户仍要在
        // 几百个字体里翻找。
        const int recommendedOffset = bundled.size() + 1;
        for (int index = 0; index < expectedRecommended.size(); ++index) {
            QCOMPARE(fontCombo->itemData(recommendedOffset + index).toString(),
                     expectedRecommended.at(index));
        }
        QVERIFY(fontCombo->count() > recommendedOffset + expectedRecommended.size());

        // 条目文本与 data 分离：data 才是写进设置的字体族名。
        const QString target = expectedRecommended.last();
        fontCombo->setCurrentIndex(fontCombo->findData(target));
        QCOMPARE(dialog.editedSettings().fontFamily, target);
    }

    // 已保存但本机没安装的字体不能被静默替换成别的字体：重开设置仍要选中原来的名字。
    TerminalSettings missing = service->currentSettings();
    missing.fontFamily = QStringLiteral("Shellium Nonexistent Mono");
    QString error;
    QVERIFY2(service->save(missing, &error), qPrintable(error));

    SettingsDialog reopened(service.get());
    auto *fontCombo = reopened.findChild<QComboBox *>(QStringLiteral("terminalFontCombo"));
    QCOMPARE(fontCombo->currentData().toString(), QStringLiteral("Shellium Nonexistent Mono"));
    QCOMPARE(reopened.editedSettings().fontFamily, QStringLiteral("Shellium Nonexistent Mono"));
}

void SettingsUiTest::bundledFontsResolveToThemselvesAndLeadTheFontCombo()
{
    const QStringList bundled = BundledFonts::registerAll();
    QCOMPARE(bundled, BundledFonts::familyNames());

    // 这几款必须随程序分发：机器上没装时若退回系统字体，字宽会变、终端网格就对不齐了。
    // 名字写死是刻意的——族名（而不是文件名）才是设置里持久化的那个字符串，"Meslo LG S"
    // 之类一旦写错，注册环节会静默跳过，界面上只是少一项，不会有任何报错。
    for (const char *required : {"Source Code Pro", "Fira Mono", "Anonymous Pro", "Ubuntu Mono",
                                 "Droid Sans Mono", "DejaVu Sans Mono", "PT Mono",
                                 "Cascadia Code", "Fira Code", "JetBrains Mono", "Meslo LG S"}) {
        QVERIFY2(bundled.contains(QString::fromLatin1(required)), required);
    }

    const QStringList installed = QFontDatabase::families();
    for (const QString &family : bundled) {
        QVERIFY2(installed.contains(family),
                 qPrintable(QStringLiteral("内置字体未进入字体数据库：%1").arg(family)));
        // 只看 families() 不够：上游的 Medium 文件仍声明自己属于基础族，Qt 会把它并进基础
        // 族，于是 QFont("Fira Mono Medium") 被静默替换成别的字体。QFontInfo 解析出的实际
        // 族名才能挡住这类回归。
        QCOMPARE(QFontInfo(QFont(family)).family(), family);
    }

    // 重复注册必须幂等：设置对话框每次构造都会调用一次，否则同一族会越堆越多。
    QCOMPARE(BundledFonts::registerAll(), bundled);

    auto service = makeService();
    SettingsDialog dialog(service.get());
    auto *fontCombo = dialog.findChild<QComboBox *>(QStringLiteral("terminalFontCombo"));
    QVERIFY(fontCombo != nullptr);
    for (int index = 0; index < bundled.size(); ++index) {
        QCOMPARE(fontCombo->itemData(index).toString(), bundled.at(index));
    }
    // 内置字体组之后必须是分隔条，而不是把系统字体直接接在后面。
    QVERIFY(!fontCombo->itemData(bundled.size()).isValid());

    // 选中的内置字体要能原样写进设置，不带任何展示后缀。
    const QString target = bundled.last();
    fontCombo->setCurrentIndex(fontCombo->findData(target));
    QCOMPARE(dialog.editedSettings().fontFamily, target);
}

void SettingsUiTest::aiConnectionFieldsFillCardWidth()
{
    auto service = makeService();
    SettingsDialog dialog(service.get());
    auto *navigation = dialog.findChild<QListWidget *>(QStringLiteral("SettingsNav"));
    QVERIFY(navigation != nullptr);

    // AI 页不是默认页，必须先切换并显示窗口，Qt 才会为滚动区内的控件计算最终几何尺寸。
    // AI 页之后还有“关于”页，因此按固定行号定位，而不是取最后一行。
    navigation->setCurrentRow(4);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    // 模型名称行现在是"下拉框 + 刷新按钮"的组合控件，宽度由整行容器承担，因此单独校验。
    auto *modelRow = dialog.findChild<QWidget *>(QStringLiteral("aiModelRow"));
    auto *modelCombo = dialog.findChild<QComboBox *>(QStringLiteral("aiModelCombo"));
    auto *refreshButton = dialog.findChild<QToolButton *>(QStringLiteral("aiModelRefreshButton"));
    QVERIFY(modelRow != nullptr);
    QVERIFY(modelCombo != nullptr);
    QVERIFY(refreshButton != nullptr);
    QVERIFY2(modelRow->width() >= modelRow->parentWidget()->contentsRect().width() - 2,
             "模型名称行未占满配置行");
    QVERIFY2(modelCombo->width() >= modelRow->width() - refreshButton->width() - 12,
             "下拉框未吃掉刷新按钮之外的剩余宽度");

    const QList<QString> fieldNames = {QStringLiteral("aiEndpointEdit"),
                                       QStringLiteral("aiApiKeyEdit")};
    for (const QString &fieldName : fieldNames) {
        auto *field = dialog.findChild<QLineEdit *>(fieldName);
        QVERIFY2(field != nullptr, qPrintable(fieldName));
        QVERIFY(field->parentWidget() != nullptr);
        // 宽表单行的布局边距为零，因此输入框应基本等于行容器宽度；保留 2 像素容差，兼容
        // 不同平台样式对控件边框和高 DPI 像素取整造成的微小差异。
        QVERIFY2(field->width() >= field->parentWidget()->contentsRect().width() - 2,
                 qPrintable(QStringLiteral("%1 仍未占满配置行：字段 %2，行 %3")
                                .arg(fieldName)
                                .arg(field->width())
                                .arg(field->parentWidget()->contentsRect().width())));
    }
}

void SettingsUiTest::aiModelRefreshFillsComboWithoutDiscardingManualEntry()
{
    MiniModelServer server;
    QVERIFY(server.isListening());

    auto service = makeService();
    AiSettingsService aiService(nullptr, nullptr);
    SettingsDialog dialog(service.get(), nullptr, &aiService, nullptr);

    auto *endpointEdit = dialog.findChild<QLineEdit *>(QStringLiteral("aiEndpointEdit"));
    auto *modelCombo = dialog.findChild<QComboBox *>(QStringLiteral("aiModelCombo"));
    auto *refreshButton = dialog.findChild<QToolButton *>(QStringLiteral("aiModelRefreshButton"));
    QVERIFY(endpointEdit != nullptr);
    QVERIFY(modelCombo != nullptr);
    QVERIFY(refreshButton != nullptr);
    // 手填必须一直可用：网关不实现 /models 时下拉框永远是空的，此时只剩手填这一条路。
    QVERIFY(modelCombo->isEditable());

    endpointEdit->setText(server.baseUrl());
    modelCombo->setCurrentText(QStringLiteral("my-private-model"));
    refreshButton->click();

    QTRY_COMPARE_WITH_TIMEOUT(modelCombo->count(), 2, 5000);
    QCOMPARE(modelCombo->itemText(0), QStringLiteral("alpha-1"));
    QCOMPARE(modelCombo->itemText(1), QStringLiteral("beta-2"));
    QCOMPARE(server.requestPaths.value(0), QStringLiteral("/models"));
    // 刷新只是补齐候选，不是替用户改配置：正在生效的模型名必须原样留在编辑框里。
    QCOMPARE(modelCombo->currentText(), QStringLiteral("my-private-model"));

    modelCombo->setCurrentIndex(1);
    QCOMPARE(modelCombo->currentText(), QStringLiteral("beta-2"));
}

void SettingsUiTest::aiModelAndThinkingToggleRoundTrip()
{
    auto service = makeService();
    AiSettingsService aiService(m_database.get(), nullptr);
    QString loadError;
    QVERIFY2(aiService.load(&loadError), qPrintable(loadError));
    QVERIFY(!aiService.currentSettings().thinkingEnabled);

    {
        SettingsDialog dialog(service.get(), nullptr, &aiService, nullptr);
        auto *modelCombo = dialog.findChild<QComboBox *>(QStringLiteral("aiModelCombo"));
        auto *thinkingToggle = dialog.findChild<ToggleSwitch *>(QStringLiteral("aiThinkingToggle"));
        auto *saveButton = dialog.findChild<QAbstractButton *>(QStringLiteral("settingsAiSave"));
        QVERIFY(modelCombo != nullptr);
        QVERIFY(thinkingToggle != nullptr);
        QVERIFY(saveButton != nullptr);
        QVERIFY(!thinkingToggle->isChecked());

        modelCombo->setCurrentText(QStringLiteral("thinking-model"));
        thinkingToggle->click();
        saveButton->click();
    }

    QCOMPARE(aiService.currentSettings().model, QStringLiteral("thinking-model"));
    QVERIFY(aiService.currentSettings().thinkingEnabled);

    // 换一个服务实例重新读库，确认开关不是只留在内存快照里。
    AiSettingsService reloaded(m_database.get(), nullptr);
    QVERIFY2(reloaded.load(&loadError), qPrintable(loadError));
    QVERIFY(reloaded.currentSettings().thinkingEnabled);
    QCOMPARE(reloaded.currentSettings().model, QStringLiteral("thinking-model"));

    // 重开设置页必须回显持久化后的状态，而不是每次都从默认值起步。
    SettingsDialog reopened(service.get(), nullptr, &reloaded, nullptr);
    QCOMPARE(reopened.findChild<QComboBox *>(QStringLiteral("aiModelCombo"))->currentText(),
             QStringLiteral("thinking-model"));
    QVERIFY(reopened.findChild<ToggleSwitch *>(QStringLiteral("aiThinkingToggle"))->isChecked());
}

void SettingsUiTest::aiLocalCliAndExecutionPolicyRoundTrip()
{
    auto service = makeService();
    AiSettingsService aiService(m_database.get(), nullptr);
    QString loadError;
    QVERIFY2(aiService.load(&loadError), qPrintable(loadError));
    // 老库里没有这些键：来源保持 HTTP，执行策略落在只读自动，绝不能默认成全自动。
    QCOMPARE(aiService.currentSettings().backend, AiBackend::Http);
    QCOMPARE(aiService.currentSettings().executionPolicy, AiExecutionPolicy::AutoReadOnly);
    QCOMPARE(aiService.currentSettings().maxAutoExecutedSteps, AiSettings::kDefaultMaxAutoSteps);

    {
        SettingsDialog dialog(service.get(), nullptr, &aiService, nullptr);
        auto *backend = dialog.findChild<SegmentedControl *>(QStringLiteral("aiBackendSelector"));
        auto *policy =
            dialog.findChild<SegmentedControl *>(QStringLiteral("aiExecutionPolicySelector"));
        auto *agentCombo = dialog.findChild<QComboBox *>(QStringLiteral("aiCliAgentCombo"));
        auto *pathEdit = dialog.findChild<QLineEdit *>(QStringLiteral("aiCliPathEdit"));
        auto *modelEdit = dialog.findChild<QLineEdit *>(QStringLiteral("aiCliModelEdit"));
        auto *endpointEdit = dialog.findChild<QLineEdit *>(QStringLiteral("aiEndpointEdit"));
        auto *cliSave = dialog.findChild<QAbstractButton *>(QStringLiteral("settingsAiCliSave"));
        auto *stepsSpin = dialog.findChild<QSpinBox *>(QStringLiteral("aiMaxAutoStepsSpin"));
        QVERIFY(backend && policy && agentCombo && pathEdit && modelEdit && endpointEdit && cliSave);
        QVERIFY(stepsSpin != nullptr);
        QCOMPARE(stepsSpin->value(), AiSettings::kDefaultMaxAutoSteps);

        // AI 页不是当前页，isVisibleTo 恒为假；改看所在卡片自身是否被隐藏。
        const auto cardHidden = [](QWidget *field) {
            for (QWidget *widget = field; widget != nullptr; widget = widget->parentWidget()) {
                if (widget->inherits("SettingsCard")) {
                    return widget->isHidden();
                }
            }
            return true;
        };
        // 每个受支持的 CLI 都要列出来，即便本机没装，用户也可以手填路径。
        QCOMPARE(agentCombo->count(), LocalAgentCli::catalog().size());
        QVERIFY(!cardHidden(endpointEdit));
        QVERIFY(cardHidden(pathEdit));

        backend->setCurrentData(static_cast<int>(AiBackend::LocalCli));
        QVERIFY(cardHidden(endpointEdit));
        QVERIFY(!cardHidden(pathEdit));

        agentCombo->setCurrentIndex(agentCombo->findData(QStringLiteral("codex")));
        pathEdit->setText(QStringLiteral("/opt/tools/codex"));
        modelEdit->setText(QStringLiteral("gpt-5-codex"));
        // 执行策略是即时保存的，不依赖点"保存"。
        policy->setCurrentData(static_cast<int>(AiExecutionPolicy::Manual));
        QCOMPARE(aiService.currentSettings().executionPolicy, AiExecutionPolicy::Manual);
        // 即时保存只改策略，不能把尚未点保存的 CLI 字段顺带写进去。
        QCOMPARE(aiService.currentSettings().backend, AiBackend::Http);
        // 步数上限同样即时保存。
        stepsSpin->setValue(35);
        QCOMPARE(aiService.currentSettings().maxAutoExecutedSteps, 35);
        QCOMPARE(aiService.currentSettings().backend, AiBackend::Http);
        cliSave->click();
    }

    AiSettingsService reloaded(m_database.get(), nullptr);
    QVERIFY2(reloaded.load(&loadError), qPrintable(loadError));
    const AiSettings saved = reloaded.currentSettings();
    QCOMPARE(saved.backend, AiBackend::LocalCli);
    QCOMPARE(saved.cliAgentId, QStringLiteral("codex"));
    QCOMPARE(saved.cliExecutablePath, QStringLiteral("/opt/tools/codex"));
    QCOMPARE(saved.cliModel, QStringLiteral("gpt-5-codex"));
    QCOMPARE(saved.executionPolicy, AiExecutionPolicy::Manual);
    QCOMPARE(saved.maxAutoExecutedSteps, 35);
    QVERIFY(saved.isConfigured());

    SettingsDialog reopened(service.get(), nullptr, &reloaded, nullptr);
    QCOMPARE(reopened.findChild<SegmentedControl *>(QStringLiteral("aiBackendSelector"))->currentData(),
             static_cast<int>(AiBackend::LocalCli));
    QCOMPARE(reopened.findChild<QComboBox *>(QStringLiteral("aiCliAgentCombo"))->currentData().toString(),
             QStringLiteral("codex"));
    QCOMPARE(reopened.findChild<QLineEdit *>(QStringLiteral("aiCliPathEdit"))->text(),
             QStringLiteral("/opt/tools/codex"));
    QCOMPARE(reopened.findChild<SegmentedControl *>(QStringLiteral("aiExecutionPolicySelector"))
                 ->currentData(),
             static_cast<int>(AiExecutionPolicy::Manual));
    QCOMPARE(reopened.findChild<QSpinBox *>(QStringLiteral("aiMaxAutoStepsSpin"))->value(), 35);

    // 被手改成越界或非数字的库值不能变成 0 步或无限步，一律回落到缺省值。
    for (const QString &bad : {QStringLiteral("0"), QStringLiteral("100000"), QStringLiteral("abc")}) {
        QVERIFY(m_database->setSetting(QStringLiteral("ai.maxAutoSteps"), bad));
        AiSettingsService corrupted(m_database.get(), nullptr);
        QVERIFY2(corrupted.load(&loadError), qPrintable(loadError));
        QCOMPARE(corrupted.currentSettings().maxAutoExecutedSteps, AiSettings::kDefaultMaxAutoSteps);
    }
}

void SettingsUiTest::aboutPageShowsVersionCopyrightAndWebsite()
{
    const QString previousVersion = QCoreApplication::applicationVersion();
    QCoreApplication::setApplicationVersion(QStringLiteral("9.8.7"));
    const auto restoreVersion = qScopeGuard(
        [previousVersion] { QCoreApplication::setApplicationVersion(previousVersion); });

    LocalizationService localization;
    QVERIFY(localization.apply(QStringLiteral("en")));

    auto service = makeService();
    SettingsDialog dialog(service.get());
    auto *navigation = dialog.findChild<QListWidget *>(QStringLiteral("SettingsNav"));
    auto *pages = dialog.findChild<QStackedWidget *>(QStringLiteral("SettingsPages"));
    QVERIFY(navigation != nullptr);
    QVERIFY(pages != nullptr);

    // “关于”是最后一个分类，导航文字与页面必须一一对应。
    const int aboutRow = navigation->count() - 1;
    QCOMPARE(navigation->item(aboutRow)->text(), QStringLiteral("About"));
    navigation->setCurrentRow(aboutRow);
    QCOMPARE(pages->currentIndex(), aboutRow);
    QWidget *aboutPage = pages->currentWidget();

    auto *versionLabel = aboutPage->findChild<QLabel *>(QStringLiteral("aboutVersionLabel"));
    QVERIFY2(versionLabel != nullptr, "关于页必须位于导航最后一项对应的页面里");
    QCOMPARE(versionLabel->text(), QStringLiteral("9.8.7"));

    auto *copyrightLabel = aboutPage->findChild<QLabel *>(QStringLiteral("aboutCopyrightLabel"));
    QVERIFY(copyrightLabel != nullptr);
    QVERIFY2(copyrightLabel->text().contains(QStringLiteral("2026"))
                 && copyrightLabel->text().contains(QStringLiteral("Shellium")),
             qPrintable(copyrightLabel->text()));

    // 官网链接必须交给系统浏览器打开：只有 openExternalLinks 打开时 QLabel 才会调用
    // QDesktopServices，否则点击只会发出无人连接的 linkActivated 信号。
    auto *websiteLink = aboutPage->findChild<QLabel *>(QStringLiteral("aboutWebsiteLink"));
    QVERIFY(websiteLink != nullptr);
    QVERIFY(websiteLink->openExternalLinks());
    QVERIFY(websiteLink->textInteractionFlags().testFlag(Qt::LinksAccessibleByMouse));
    QVERIFY2(websiteLink->text().contains(QStringLiteral("href=\"https://shellium.net\"")),
             qPrintable(websiteLink->text()));

    // 版本号为空（裸 QApplication）时不能显示空白值。
    QCoreApplication::setApplicationVersion(QString());
    SettingsDialog unversionedDialog(service.get());
    auto *unknownLabel =
        unversionedDialog.findChild<QLabel *>(QStringLiteral("aboutVersionLabel"));
    QVERIFY(unknownLabel != nullptr);
    QCOMPARE(unknownLabel->text(), QStringLiteral("Unknown"));
}

void SettingsUiTest::openSourceNoticesCoverEveryDistributedComponent()
{
    const QList<OpenSourceComponent> components = OpenSourceNotices::components();
    QSet<QString> names;
    QSet<QString> referencedFiles;
    for (const OpenSourceComponent &component : components) {
        QVERIFY2(!names.contains(component.name), qPrintable(component.name));
        names.insert(component.name);
        QVERIFY2(!component.license.isEmpty(), qPrintable(component.name));
        QVERIFY2(component.homepage.startsWith(QStringLiteral("http")), qPrintable(component.name));
        QVERIFY2(!component.licenseFiles.isEmpty(), qPrintable(component.name));
        for (const QString &file : component.licenseFiles) {
            QVERIFY2(QFile::exists(OpenSourceNotices::resourcePath(file)), qPrintable(file));
            referencedFiles.insert(file);
        }
        const QString text = OpenSourceNotices::licenseText(component);
        QVERIFY2(text.size() > 100, qPrintable(component.name));
        QVERIFY2(!text.startsWith(QChar(0xFEFF)), qPrintable(component.name));
        if (component.lgpl) {
            QVERIFY2(text.contains(QStringLiteral("GNU LESSER GENERAL PUBLIC LICENSE")),
                     qPrintable(component.name));
        }
    }

    // CMakeLists.txt 里 shellium_resolve_dependency 解析出的每个直接依赖都随安装包分发。
    const QStringList directDependencies{
        QStringLiteral("Qt"),        QStringLiteral("libssh"), QStringLiteral("libvterm"),
        QStringLiteral("libsodium"), QStringLiteral("spdlog"), QStringLiteral("{fmt}"),
        QStringLiteral("minizip"),   QStringLiteral("zlib"),   QStringLiteral("OpenSSL")};
    for (const QString &dependency : directDependencies) {
        QVERIFY2(names.contains(dependency), qPrintable(dependency));
    }
    // OFL/Apache 要求字体随附许可证：每款内置字体都必须有对应条目。
    for (const QString &family : BundledFonts::familyNames()) {
        QVERIFY2(names.contains(family), qPrintable(family));
    }

    // Qt 按 LGPLv3 分发，而 LGPLv3 是 GPLv3 的附加条款，两份全文都必须附带。
    const auto qt = std::find_if(components.cbegin(), components.cend(),
                                 [](const OpenSourceComponent &c) { return c.name == QLatin1String("Qt"); });
    QVERIFY(qt != components.cend());
    QVERIFY(qt->lgpl);
    const QString qtText = OpenSourceNotices::licenseText(*qt);
    QVERIFY(qtText.contains(QStringLiteral("GNU LESSER GENERAL PUBLIC LICENSE")));
    QVERIFY(qtText.contains(QStringLiteral("GNU GENERAL PUBLIC LICENSE")));

    QDirIterator it(QStringLiteral(":/licenses"), QDir::Files, QDirIterator::Subdirectories);
    int bundledFiles = 0;
    while (it.hasNext()) {
        const QString path = it.next();
        const QString relative = path.mid(QStringLiteral(":/licenses/").size());
        QVERIFY2(referencedFiles.contains(relative), qPrintable(path));
        ++bundledFiles;
    }
    QCOMPARE(bundledFiles, referencedFiles.size());
}

void SettingsUiTest::aboutPageOpensOpenSourceLicenses()
{
    LocalizationService localization;
    QVERIFY(localization.apply(QStringLiteral("en")));

    auto service = makeService();
    SettingsDialog dialog(service.get());
    auto *button = dialog.findChild<QPushButton *>(QStringLiteral("aboutOpenSourceLicensesButton"));
    QVERIFY2(button != nullptr, "关于页必须提供开源软件许可入口");
    QCOMPARE(button->text(), QStringLiteral("View licenses"));

    QVERIFY(dialog.findChild<OpenSourceLicensesDialog *>() == nullptr);
    button->click();
    auto *licenses = dialog.findChild<OpenSourceLicensesDialog *>();
    QVERIFY(licenses != nullptr);
    QVERIFY(licenses->isVisible());
    QVERIFY(licenses->testAttribute(Qt::WA_DeleteOnClose));
    QCOMPARE(licenses->windowTitle(), QStringLiteral("Open Source Licenses"));

    auto *list = licenses->findChild<QListWidget *>(QStringLiteral("openSourceComponentList"));
    QVERIFY(list != nullptr);
    QCOMPARE(list->count(), OpenSourceNotices::components().size());

    // 横纵滚动条同时出现会在列表右下角留下空白角块，组件名过长时只能省略显示。
    QCOMPARE(list->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);

    // 顶部声明保持精简：点名动态链接的 LGPL 组件并给出源代码索取方式即可。
    auto *notice = licenses->findChild<QLabel *>(QStringLiteral("openSourceNoticeLabel"));
    QVERIFY(notice != nullptr);
    const QString noticeText = notice->text();
    QVERIFY2(noticeText.contains(QStringLiteral("LGPL")), qPrintable(noticeText));
    QVERIFY2(noticeText.contains(QStringLiteral("Qt, libssh")), qPrintable(noticeText));
    QVERIFY2(noticeText.contains(QStringLiteral("https://shellium.net")), qPrintable(noticeText));
    QVERIFY2(!noticeText.contains(QStringLiteral("FreeType")), qPrintable(noticeText));
    QVERIFY(notice->openExternalLinks());

    auto *view = licenses->findChild<QTextBrowser *>(QStringLiteral("openSourceLicenseView"));
    QVERIFY(view != nullptr);
    QVERIFY(licenses->selectComponent(QStringLiteral("libssh")));
    QVERIFY2(view->toPlainText().contains(QStringLiteral("Linking with OpenSSL")),
             qPrintable(view->toPlainText().left(300)));
    QVERIFY(view->toPlainText().contains(QStringLiteral("https://www.libssh.org/")));
    // FreeType 与 IJG 许可证要求出现在文档里的署名语句，随各自组件的详情页展示。
    QVERIFY(licenses->selectComponent(QStringLiteral("FreeType")));
    QVERIFY(view->toPlainText().contains(
        QStringLiteral("Portions of this software are copyright \u00A9 2026 The FreeType Project")));
    QVERIFY(licenses->selectComponent(QStringLiteral("libjpeg-turbo")));
    QVERIFY(view->toPlainText().contains(
        QStringLiteral("based in part on the work of the Independent JPEG Group")));
    QVERIFY(licenses->selectComponent(QStringLiteral("Meslo LG S")));
    QVERIFY(view->toPlainText().contains(QStringLiteral("Apache License")));
    QVERIFY(!licenses->selectComponent(QStringLiteral("no-such-component")));

    // 按钮与对话框都是常驻控件，切换语言后必须就地重译，而不是停留在英文。
    QVERIFY(localization.apply(QStringLiteral("zh_CN")));
    QCOMPARE(button->text(), QStringLiteral("查看许可证"));
    QCOMPARE(licenses->windowTitle(), QStringLiteral("开源软件许可"));
    QVERIFY(localization.apply(QStringLiteral("en")));

    licenses->close();
}

void SettingsUiTest::updateSettingsPersistAndSkippedVersionSurvivesOtherEdits()
{
    auto service = makeService();
    const auto restoreDefaults = qScopeGuard([&service] {
        TerminalSettings settings = service->currentSettings();
        settings.automaticUpdateCheck = true;
        settings.skippedUpdateVersion.clear();
        service->save(settings, nullptr);
    });
    QVERIFY(service->currentSettings().automaticUpdateCheck);
    QVERIFY(service->currentSettings().skippedUpdateVersion.isEmpty());

    SettingsDialog dialog(service.get());
    // 非当前页在 QStackedWidget 里整体隐藏，先切到“关于”页，可见性断言才有意义。
    auto *navigation = dialog.findChild<QListWidget *>(QStringLiteral("SettingsNav"));
    QVERIFY(navigation != nullptr);
    navigation->setCurrentRow(navigation->count() - 1);
    auto *autoCheckToggle =
        dialog.findChild<ToggleSwitch *>(QStringLiteral("automaticUpdateCheckToggle"));
    auto *skippedLabel =
        dialog.findChild<QLabel *>(QStringLiteral("aboutSkippedUpdateVersionLabel"));
    auto *clearButton =
        dialog.findChild<QPushButton *>(QStringLiteral("aboutClearSkippedUpdateButton"));
    QVERIFY(autoCheckToggle != nullptr);
    QVERIFY(skippedLabel != nullptr);
    QVERIFY(clearButton != nullptr);
    QVERIFY(autoCheckToggle->isChecked());
    // 没有跳过任何版本时整行隐藏。
    QVERIFY(!skippedLabel->isVisibleTo(&dialog));

    autoCheckToggle->click();
    QVERIFY(!service->currentSettings().automaticUpdateCheck);
    QCOMPARE(m_database->intSetting(QStringLiteral("update/automatic_check")).value_or(-1), 0);

    // 更新提示里点“跳过此版本”走的就是这条路径；设置窗口开着时要即时回显。
    QString error;
    QVERIFY2(UpdatePrompt::skipVersion(service.get(), QStringLiteral("0.4.0"), &error),
             qPrintable(error));
    QCOMPARE(skippedLabel->text(), QStringLiteral("0.4.0"));
    QVERIFY(skippedLabel->isVisibleTo(&dialog));

    // 改一项毫不相干的设置，跳过记录必须原样保留。
    auto *bellToggle = dialog.findChild<ToggleSwitch *>(QStringLiteral("audibleBellToggle"));
    QVERIFY(bellToggle != nullptr);
    const bool bellBefore = service->currentSettings().audibleBell;
    bellToggle->click();
    QCOMPARE(service->currentSettings().audibleBell, !bellBefore);
    QCOMPARE(service->currentSettings().skippedUpdateVersion, QStringLiteral("0.4.0"));
    bellToggle->click();

    TerminalSettingsService reloaded(m_database.get());
    QVERIFY2(reloaded.load(&error), qPrintable(error));
    QVERIFY(!reloaded.currentSettings().automaticUpdateCheck);
    QCOMPARE(reloaded.currentSettings().skippedUpdateVersion, QStringLiteral("0.4.0"));

    clearButton->click();
    QVERIFY(service->currentSettings().skippedUpdateVersion.isEmpty());
    QVERIFY(!skippedLabel->isVisibleTo(&dialog));
}

void SettingsUiTest::manualUpdateCheckPromptsAndSkipRecordsVersion()
{
    auto service = makeService();
    const auto restoreDefaults = qScopeGuard([&service] {
        TerminalSettings settings = service->currentSettings();
        settings.skippedUpdateVersion.clear();
        service->save(settings, nullptr);
    });

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString manifestPath = directory.filePath(QStringLiteral("latest.json"));
    QFile manifest(manifestPath);
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(R"({"version":"99.0.0","publishedAt":"2026-11-01T00:00:00Z"})");
    manifest.close();

    SettingsDialog dialog(service.get());
    auto *checker = dialog.findChild<UpdateChecker *>(QStringLiteral("settingsUpdateChecker"));
    auto *checkButton =
        dialog.findChild<QPushButton *>(QStringLiteral("aboutCheckForUpdatesButton"));
    auto *statusLabel = dialog.findChild<QLabel *>(QStringLiteral("aboutUpdateStatusLabel"));
    QVERIFY(checker != nullptr);
    QVERIFY(checkButton != nullptr);
    QVERIFY(statusLabel != nullptr);
    checker->setEndpoint(QUrl::fromLocalFile(manifestPath));
    checker->setCurrentVersion(QStringLiteral("0.3.5"));

    checkButton->click();
    QVERIFY(!checkButton->isEnabled());
    QMessageBox *prompt = nullptr;
    QVERIFY(QTest::qWaitFor([&] {
        prompt = dialog.findChild<QMessageBox *>(QStringLiteral("updatePrompt"));
        return prompt != nullptr;
    }, 5000));
    QVERIFY(checkButton->isEnabled());
    QVERIFY2(statusLabel->text().contains(QStringLiteral("99.0.0")), qPrintable(statusLabel->text()));
    QVERIFY(prompt->text().contains(QStringLiteral("99.0.0")));

    auto *skipButton = prompt->findChild<QPushButton *>(QStringLiteral("updatePromptSkip"));
    QVERIFY(skipButton != nullptr);
    QVERIFY(prompt->findChild<QPushButton *>(QStringLiteral("updatePromptDownload")) != nullptr);
    QVERIFY(prompt->findChild<QPushButton *>(QStringLiteral("updatePromptLater")) != nullptr);
    skipButton->click();
    QCOMPARE(service->currentSettings().skippedUpdateVersion, QStringLiteral("99.0.0"));
    auto *skippedLabel =
        dialog.findChild<QLabel *>(QStringLiteral("aboutSkippedUpdateVersionLabel"));
    QVERIFY(skippedLabel != nullptr);
    QCOMPARE(skippedLabel->text(), QStringLiteral("99.0.0"));
}

QTEST_MAIN(SettingsUiTest)
#include "SettingsUiTest.moc"
