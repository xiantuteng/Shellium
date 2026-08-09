#include "settings/TerminalSettings.h"
#include "storage/ApplicationDatabase.h"
#include "ui/ConnectionDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/SettingsWidgets.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QGridLayout>
#include <QLineEdit>
#include <QSet>
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
    void everyThemePresetGetsUniqueSwatchAndPalette();
    void colorSchemeFilterKeepsVisibleSwatchesPacked();
    void unknownColorSchemeIdFallsBackToDefault();
    void colorSchemeSwitchKeepsApplicationTextColors();
    void fontComboOffersRecommendedFamiliesAndKeepsMissingOne();

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
        dialog.accept();
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
        // 除“默认”外，预设颜色不随主题模式改变。
        if (preset.id != QLatin1String("default")) {
            probe.themeMode = ThemeMode::Light;
            QCOMPARE(probe.normalized().effectivePalette().background, palette.background);
        }
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
    const QStringList installed = QFontDatabase::families();
    QStringList expectedRecommended;
    for (const QString &family : TerminalSettings::recommendedFontFamilies()) {
        if (installed.contains(family)) {
            expectedRecommended.append(family);
        }
    }
    QVERIFY2(!expectedRecommended.isEmpty(), "本机没有任何推荐编程字体，用例无法覆盖");

    {
        SettingsDialog dialog(service.get());
        auto *fontCombo = dialog.findChild<QComboBox *>(QStringLiteral("terminalFontCombo"));
        QVERIFY(fontCombo != nullptr);
        // 推荐字体必须排在最前面，且顺序与推荐列表一致；否则用户仍要在几百个字体里翻找。
        for (int index = 0; index < expectedRecommended.size(); ++index) {
            QCOMPARE(fontCombo->itemData(index).toString(), expectedRecommended.at(index));
        }
        QVERIFY(fontCombo->count() > expectedRecommended.size());

        // 条目文本与 data 分离：data 才是写进设置的字体族名。
        const QString target = expectedRecommended.last();
        fontCombo->setCurrentIndex(fontCombo->findData(target));
        QCOMPARE(dialog.editedSettings().fontFamily, target);
        dialog.accept();
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

QTEST_MAIN(SettingsUiTest)
#include "SettingsUiTest.moc"
