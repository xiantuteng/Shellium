#include "settings/TerminalSettings.h"
#include "storage/ApplicationDatabase.h"
#include "ui/ConnectionDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/SettingsWidgets.h"

#include <QAbstractSpinBox>
#include <QDoubleSpinBox>
#include <QListWidget>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTest>
#include <QToolButton>

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
    void reconnectToggleGatesDependentRows();
    void colorSchemeSwatchesAreExclusiveAndPersist();
    void navigationSelectsMatchingPage();
    void rejectRestoresOriginalSettings();
    void connectionDialogToggleRoundTripsLegacyHmac();

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
    QVERIFY(bellToggle != nullptr);
    QVERIFY(blinkToggle != nullptr);
    // 打开对话框时开关必须反映已持久化的值，而不是一律显示默认关闭。
    QCOMPARE(bellToggle->isChecked(), service->currentSettings().audibleBell);
    QCOMPARE(blinkToggle->isChecked(), service->currentSettings().cursorBlink);

    // 点击开关会经 previewChanges() 立即改写服务里的运行设置，因此期望值必须取点击前的
    // 快照，不能再读 service->currentSettings()。
    const bool originalBell = service->currentSettings().audibleBell;
    const bool originalBlink = service->currentSettings().cursorBlink;
    bellToggle->click();
    blinkToggle->click();
    QCOMPARE(dialog.editedSettings().audibleBell, !originalBell);
    QCOMPARE(dialog.editedSettings().cursorBlink, !originalBlink);

    const bool expectedBell = !originalBell;
    dialog.accept();
    QCOMPARE(service->currentSettings().audibleBell, expectedBell);
    QCOMPARE(m_database->setting(QStringLiteral("terminal/audible_bell")).value_or(QString()),
             expectedBell ? QStringLiteral("1") : QStringLiteral("0"));
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

void SettingsUiTest::colorSchemeSwatchesAreExclusiveAndPersist()
{
    auto service = makeService();
    const QString draculaName =
        QStringLiteral("colorSchemeSwatch_%1").arg(static_cast<int>(TerminalColorScheme::Dracula));
    const QString defaultName =
        QStringLiteral("colorSchemeSwatch_%1").arg(static_cast<int>(TerminalColorScheme::Default));

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
        QCOMPARE(dialog.editedSettings().colorScheme, TerminalColorScheme::Dracula);
        dialog.accept();
    }

    QCOMPARE(service->currentSettings().colorScheme, TerminalColorScheme::Dracula);

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

void SettingsUiTest::rejectRestoresOriginalSettings()
{
    auto service = makeService();
    const TerminalSettings before = service->currentSettings();

    SettingsDialog dialog(service.get());
    QSignalSpy changeSpy(service.get(), &TerminalSettingsService::settingsChanged);
    stepperButton(dialog.findChild<QSpinBox *>(QStringLiteral("terminalFontSizeSpin")),
                  QStringLiteral("increase"))
        ->click();
    QVERIFY(changeSpy.count() > 0);
    QCOMPARE(service->currentSettings().fontPointSize, before.fontPointSize + 1);

    dialog.reject();
    QVERIFY(service->currentSettings() == before);
    QCOMPARE(m_database->setting(QStringLiteral("terminal/font_size")).value_or(QString()),
             QString::number(before.fontPointSize));
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

QTEST_MAIN(SettingsUiTest)
#include "SettingsUiTest.moc"
