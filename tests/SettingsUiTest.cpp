#include "ai/AiSettings.h"
#include "ai/AiSettingsService.h"
#include "settings/BundledFonts.h"
#include "settings/TerminalSettings.h"
#include "security/Vault.h"
#include "storage/ApplicationDatabase.h"
#include "ui/ConnectionDialog.h"
#include "ui/SettingsDialog.h"
#include "i18n/LocalizationService.h"
#include "ui/SettingsWidgets.h"

#include <QLabel>

#include <QAbstractSpinBox>
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGridLayout>
#include <QLineEdit>
#include <QSet>
#include <QListWidget>
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
    /** @brief 设置窗口打开时切换语言，卡片标题/行文案/分段控件必须就地热切换。 */
    void settingsContentRetranslatesLiveWithoutReopen();
    void colorSchemeSwatchesAreExclusiveAndPersist();
    void navigationSelectsMatchingPage();
    /** @brief 设置页不再显示保存/取消，修改必须即时入库且关闭窗口不得回滚。 */
    void changesPersistImmediatelyAndCloseDoesNotRollback();
    /** @brief 恢复默认必须和普通单项修改一样自动持久化。 */
    void restoreDefaultsPersistsImmediately();
    void connectionDialogToggleRoundTripsLegacyHmac();
    void everyThemePresetGetsUniqueSwatchAndPalette();
    /** @brief 参考图里那几套主题必须使用与其上游一致的 16 色，不能只对上背景色。 */
    void referenceThemesCarryTheirPublishedAnsiPalette();
    void colorSchemeFilterKeepsVisibleSwatchesPacked();
    void unknownColorSchemeIdFallsBackToDefault();
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

void SettingsUiTest::languageSelectorPersistsAndRoundTrips()
{
    auto service = makeService();
    QCOMPARE(service->currentSettings().language, QStringLiteral("system"));

    {
        SettingsDialog dialog(service.get());
        auto *selector = dialog.findChild<QComboBox *>(QStringLiteral("languageSelector"));
        QVERIFY(selector != nullptr);
        QVERIFY(selector->count() >= 2);
        // 首项固定是“跟随系统”。
        QCOMPARE(selector->itemData(0).toString(), QStringLiteral("system"));

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

    QVERIFY(localization.apply(QStringLiteral("en")));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LanguageChange);
    QVERIFY(hasTitle(QStringLiteral("Interface language")));
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

void SettingsUiTest::restoreDefaultsPersistsImmediately()
{
    auto service = makeService();
    TerminalSettings customized = service->currentSettings();
    customized.fontPointSize = 23;
    customized.audibleBell = !TerminalSettings{}.normalized().audibleBell;
    QString error;
    QVERIFY2(service->save(customized, &error), qPrintable(error));

    SettingsDialog dialog(service.get());
    auto *restoreButton = dialog.findChild<QAbstractButton *>(
        QStringLiteral("settingsRestoreDefaults"));
    QVERIFY(restoreButton != nullptr);
    restoreButton->click();

    const TerminalSettings defaults = TerminalSettings{}.normalized();
    QCOMPARE(service->currentSettings().fontPointSize, defaults.fontPointSize);
    QCOMPARE(service->currentSettings().audibleBell, defaults.audibleBell);
    QCOMPARE(m_database->setting(QStringLiteral("terminal/font_size")).value_or(QString()),
             QString::number(defaults.fontPointSize));
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
    QCOMPARE(presets.first().id, QStringLiteral("default"));

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
        {"termius_dark", 4, "#015fff"},
        {"termius_dark", 12, "#015fff"},
        {"termius_light", 4, "#2f6fd0"},
    };
    for (const Expectation &expectation : expectations) {
        TerminalSettings probe;
        probe.colorSchemeId = QString::fromLatin1(expectation.id);
        const TerminalPalette palette = probe.normalized().effectivePalette();
        QCOMPARE(palette.ansi.value(expectation.index),
                 QColor(QLatin1String(expectation.hex)));
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
    navigation->setCurrentRow(navigation->count() - 1);
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

QTEST_MAIN(SettingsUiTest)
#include "SettingsUiTest.moc"
