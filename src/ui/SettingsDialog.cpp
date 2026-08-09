#include "ui/SettingsDialog.h"

#include "settings/TerminalSettings.h"
#include "ui/SettingsWidgets.h"

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

/**
 * @brief 判断某个字体族是否等宽。
 *
 * QFontDatabase::isFixedPitch() 在 macOS 上对不少真实等宽字体（尤其是用户自行安装、
 * 打了 Nerd Font 补丁的变体）会返回 false，导致“本机其余等宽字体”分组漏掉它们。这里
 * 用字符宽度实测兜底：同一字体里 'i' 与 'W' 的前进宽度相等，基本可以判定为等宽。
 */
bool isLikelyMonospaceFamily(const QString &family)
{
    if (QFontDatabase::isFixedPitch(family)) {
        return true;
    }
    const QFont font(family);
    const QFontMetricsF metrics(font);
    return qFuzzyCompare(metrics.horizontalAdvance(QLatin1Char('i')),
                          metrics.horizontalAdvance(QLatin1Char('W')));
}

/** @brief 左侧导航的分类顺序，索引与页面栈严格一致。 */
enum SettingsPage {
    AppearancePage = 0,
    TerminalPage,
    ConnectionPage,
    ShortcutPage
};

/** @brief 返回指定终端配色在当前主题下的实际颜色，供色板卡真实预览。 */
TerminalPalette paletteForScheme(const TerminalSettings &base, const QString &colorSchemeId)
{
    TerminalSettings probe = base;
    probe.colorSchemeId = colorSchemeId;
    return probe.effectivePalette();
}

} // namespace

SettingsDialog::SettingsDialog(TerminalSettingsService *service, QWidget *parent)
    : QDialog(parent)
    , m_service(service)
    , m_originalSettings(service == nullptr ? TerminalSettings{}.normalized()
                                            : service->currentSettings())
    , m_navigationList(new QListWidget(this))
    , m_pages(new QStackedWidget(this))
    , m_themeSelector(new SegmentedControl(this))
    , m_fontCombo(new QComboBox(this))
    , m_fontSizeSpin(new QSpinBox(this))
    , m_lineHeightSpin(new QSpinBox(this))
    , m_letterSpacingSpin(new QDoubleSpinBox(this))
    , m_cursorShapeSelector(new SegmentedControl(this))
    , m_cursorBlinkToggle(new ToggleSwitch(this))
    , m_scrollbackSpin(new QSpinBox(this))
    , m_audibleBellToggle(new ToggleSwitch(this))
    , m_automaticReconnectToggle(new ToggleSwitch(this))
    , m_reconnectMaxAttemptsSpin(new QSpinBox(this))
    , m_reconnectBaseDelaySpin(new QSpinBox(this))
    , m_previewLabel(new QLabel(this))
{
    Q_ASSERT(m_service != nullptr);
    setObjectName(QStringLiteral("SettingsDialog"));
    setWindowTitle(tr("设置"));
    resize(880, 660);

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto *bodyRow = new QWidget(this);
    bodyRow->setObjectName(QStringLiteral("SettingsBody"));
    auto *bodyLayout = new QHBoxLayout(bodyRow);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    auto *sidebar = new QWidget(bodyRow);
    sidebar->setObjectName(QStringLiteral("SettingsSidebar"));
    sidebar->setFixedWidth(212);
    auto *sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(12, 18, 12, 14);
    sidebarLayout->setSpacing(10);
    auto *sidebarTitle = new QLabel(tr("设置"), sidebar);
    sidebarTitle->setObjectName(QStringLiteral("SettingsSidebarTitle"));
    sidebarLayout->addWidget(sidebarTitle);
    sidebarLayout->addWidget(m_navigationList, 1);
    // 版本号由 main() 设置；测试或裸 QApplication 下为空，此时只显示产品名，不留孤零零的空格。
    const QString version = QApplication::applicationVersion();
    auto *versionLabel = new QLabel(
        version.isEmpty() ? tr("Shellium") : tr("Shellium %1").arg(version), sidebar);
    versionLabel->setObjectName(QStringLiteral("SettingsRowHint"));
    sidebarLayout->addWidget(versionLabel);

    bodyLayout->addWidget(sidebar);
    bodyLayout->addWidget(m_pages, 1);
    rootLayout->addWidget(bodyRow, 1);

    m_pages->setObjectName(QStringLiteral("SettingsPages"));
    m_pages->addWidget(wrapInScrollArea(buildAppearancePage()));
    m_pages->addWidget(wrapInScrollArea(buildTerminalPage()));
    m_pages->addWidget(wrapInScrollArea(buildConnectionPage()));
    m_pages->addWidget(wrapInScrollArea(buildShortcutPage()));
    buildNavigation();

    auto *footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("SettingsFooter"));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(20, 12, 20, 12);
    footerLayout->setSpacing(10);
    auto *restoreButton = new QPushButton(tr("恢复默认"), footer);
    restoreButton->setObjectName(QStringLiteral("settingsRestoreDefaults"));
    auto *cancelButton = new QPushButton(tr("取消"), footer);
    cancelButton->setObjectName(QStringLiteral("settingsCancel"));
    auto *saveButton = new QPushButton(tr("保存"), footer);
    saveButton->setObjectName(QStringLiteral("settingsSave"));
    saveButton->setProperty("primaryAction", true);
    saveButton->setDefault(true);
    footerLayout->addWidget(restoreButton);
    footerLayout->addStretch(1);
    footerLayout->addWidget(cancelButton);
    footerLayout->addWidget(saveButton);
    rootLayout->addWidget(footer);

    connect(saveButton, &QPushButton::clicked, this, &SettingsDialog::accept);
    connect(cancelButton, &QPushButton::clicked, this, &SettingsDialog::reject);
    connect(restoreButton, &QPushButton::clicked, this, [this]() {
        populateControls(TerminalSettings{}.normalized());
        previewChanges();
    });

    connect(m_themeSelector, &SegmentedControl::currentDataChanged,
            this, [this](int) { previewChanges(); });
    connect(m_cursorShapeSelector, &SegmentedControl::currentDataChanged,
            this, [this](int) { previewChanges(); });
    connect(m_fontCombo, &QComboBox::currentTextChanged,
            this, [this](const QString &) { previewChanges(); });
    connect(m_fontSizeSpin, &QSpinBox::valueChanged, this, [this](int) { previewChanges(); });
    connect(m_lineHeightSpin, &QSpinBox::valueChanged, this, [this](int) { previewChanges(); });
    connect(m_letterSpacingSpin, &QDoubleSpinBox::valueChanged,
            this, [this](double) { previewChanges(); });
    connect(m_scrollbackSpin, &QSpinBox::valueChanged, this, [this](int) { previewChanges(); });
    connect(m_reconnectMaxAttemptsSpin, &QSpinBox::valueChanged,
            this, [this](int) { previewChanges(); });
    connect(m_reconnectBaseDelaySpin, &QSpinBox::valueChanged,
            this, [this](int) { previewChanges(); });
    for (ToggleSwitch *toggle :
         {m_cursorBlinkToggle, m_audibleBellToggle, m_automaticReconnectToggle}) {
        connect(toggle, &ToggleSwitch::toggled, this, [this](bool) { previewChanges(); });
    }
    connect(m_automaticReconnectToggle, &ToggleSwitch::toggled, this, [this](const bool enabled) {
        for (QWidget *row : std::as_const(m_reconnectDependentRows)) {
            row->setEnabled(enabled);
        }
    });
    for (QKeySequenceEdit *editor : std::as_const(m_shortcutEditors)) {
        connect(editor, &QKeySequenceEdit::keySequenceChanged,
                this, [this](const QKeySequence &) { previewChanges(); });
    }

    populateControls(m_originalSettings);
    updatePreviewCard(m_originalSettings);
}

void SettingsDialog::buildNavigation()
{
    m_navigationList->setObjectName(QStringLiteral("SettingsNav"));
    m_navigationList->setFrameShape(QFrame::NoFrame);
    m_navigationList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navigationList->setUniformItemSizes(true);
    m_navigationList->addItem(tr("外观"));
    m_navigationList->addItem(tr("终端"));
    m_navigationList->addItem(tr("连接恢复"));
    m_navigationList->addItem(tr("快捷键"));
    for (int row = 0; row < m_navigationList->count(); ++row) {
        m_navigationList->item(row)->setSizeHint(QSize(0, 38));
    }
    m_navigationList->setCurrentRow(AppearancePage);
    connect(m_navigationList, &QListWidget::currentRowChanged, m_pages, [this](const int row) {
        if (row >= 0) {
            m_pages->setCurrentIndex(row);
        }
    });
}

QWidget *SettingsDialog::wrapInScrollArea(QWidget *page)
{
    auto *scroll = new QScrollArea(m_pages);
    scroll->setObjectName(QStringLiteral("SettingsScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(page);
    return scroll;
}

void SettingsDialog::relayoutColorSchemeGrid(const QString &filter)
{
    auto *gridLayout = qobject_cast<QGridLayout *>(m_colorSchemeGrid->layout());
    Q_ASSERT(gridLayout != nullptr);
    const QString needle = filter.trimmed();
    int visibleIndex = 0;
    for (const QString &id : std::as_const(m_colorSchemeOrder)) {
        ThemeSwatchButton *swatch = m_colorSchemeSwatches.value(id);
        if (swatch == nullptr) {
            continue;
        }
        const bool matches = needle.isEmpty()
            || swatch->text().contains(needle, Qt::CaseInsensitive)
            || id.contains(needle, Qt::CaseInsensitive);
        gridLayout->removeWidget(swatch);
        swatch->setVisible(matches);
        if (matches) {
            gridLayout->addWidget(swatch, visibleIndex / 2, visibleIndex % 2);
            ++visibleIndex;
        }
    }
    if (m_colorSchemeEmptyLabel != nullptr) {
        m_colorSchemeEmptyLabel->setVisible(visibleIndex == 0);
    }
}

void SettingsDialog::populateFontCombo()
{
    // 每个条目都用它自己的字体渲染，用户不必逐个选中试看效果；data 里存纯字体族名，与显示
    // 文本解耦，因此“（本机未安装）”一类后缀不会写进设置。
    const QStringList installed = QFontDatabase::families();
    const auto appendFamily = [this](const QString &family) {
        QFont itemFont(family);
        itemFont.setPointSize(12);
        m_fontCombo->addItem(family, family);
        m_fontCombo->setItemData(m_fontCombo->count() - 1, itemFont, Qt::FontRole);
    };

    QStringList recommended;
    for (const QString &family : TerminalSettings::recommendedFontFamilies()) {
        if (installed.contains(family)) {
            recommended.append(family);
        }
    }
    for (const QString &family : std::as_const(recommended)) {
        appendFamily(family);
    }

    QStringList others;
    for (const QString &family : installed) {
        if (!recommended.contains(family) && isLikelyMonospaceFamily(family)) {
            others.append(family);
        }
    }
    others.sort(Qt::CaseInsensitive);
    if (!others.isEmpty() && !recommended.isEmpty()) {
        // 分隔条目：不可选，只用来区分“推荐编程字体”和本机其余等宽字体。
        m_fontCombo->insertSeparator(m_fontCombo->count());
    }
    for (const QString &family : std::as_const(others)) {
        appendFamily(family);
    }
}

QWidget *SettingsDialog::buildAppearancePage()
{
    auto *page = new QWidget(m_pages);
    page->setObjectName(QStringLiteral("SettingsPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 22, 24, 22);
    layout->setSpacing(16);

    m_themeSelector->setObjectName(QStringLiteral("themeSelector"));
    m_themeSelector->addSegment(tr("跟随系统"), static_cast<int>(ThemeMode::FollowSystem));
    m_themeSelector->addSegment(tr("浅色"), static_cast<int>(ThemeMode::Light));
    m_themeSelector->addSegment(tr("深色"), static_cast<int>(ThemeMode::Dark));

    auto *themeCard = new SettingsCard(tr("应用主题"), page);
    themeCard->addRow(tr("界面外观"),
                      m_themeSelector,
                      tr("“跟随系统”会在操作系统切换深浅外观时自动同步。"));
    layout->addWidget(themeCard);

    auto *schemeCard = new SettingsCard(tr("终端配色"), page);
    m_colorSchemeFilter = new QLineEdit(schemeCard);
    m_colorSchemeFilter->setObjectName(QStringLiteral("colorSchemeFilter"));
    m_colorSchemeFilter->setClearButtonEnabled(true);
    m_colorSchemeFilter->setPlaceholderText(tr("搜索配色，例如 Nord、Dark、Gruvbox"));
    schemeCard->addFullWidthWidget(m_colorSchemeFilter);

    m_colorSchemeGrid = new QWidget(schemeCard);
    auto *gridLayout = new QGridLayout(m_colorSchemeGrid);
    gridLayout->setContentsMargins(0, 0, 0, 0);
    gridLayout->setHorizontalSpacing(12);
    gridLayout->setVerticalSpacing(10);

    for (const TerminalThemePreset &preset : TerminalSettings::themePresets()) {
        const bool isDefault = preset.id == QLatin1String("default");
        const TerminalPalette palette = isDefault
            ? paletteForScheme(m_originalSettings, preset.id)
            : preset.palette;
        auto *swatch = new ThemeSwatchButton(preset.name,
                                            isDefault ? tr("跟随应用主题")
                                                      : (preset.dark ? tr("深色") : tr("浅色")),
                                            palette.background,
                                            palette.foreground,
                                            palette.cursor,
                                            palette.link,
                                            m_colorSchemeGrid);
        swatch->setObjectName(QStringLiteral("colorSchemeSwatch_%1").arg(preset.id));
        m_colorSchemeSwatches.insert(preset.id, swatch);
        m_colorSchemeOrder.append(preset.id);
        // 色板卡之间互斥但不属于同一个 QButtonGroup：“默认”卡需要在主题变化时单独重绘，
        // 因此这里显式管理选中态，同时避免 QButtonGroup 吃掉“再次点击已选项”的空操作。
        connect(swatch, &ThemeSwatchButton::clicked, this, [this, id = preset.id]() {
            for (auto iterator = m_colorSchemeSwatches.cbegin();
                 iterator != m_colorSchemeSwatches.cend();
                 ++iterator) {
                iterator.value()->setChecked(iterator.key() == id);
            }
            previewChanges();
        });
    }
    relayoutColorSchemeGrid(QString());
    schemeCard->addFullWidthWidget(m_colorSchemeGrid);
    m_colorSchemeEmptyLabel = schemeCard->addNote(tr("没有匹配的配色。"));
    m_colorSchemeEmptyLabel->setVisible(false);
    connect(m_colorSchemeFilter, &QLineEdit::textChanged,
            this, [this](const QString &text) { relayoutColorSchemeGrid(text); });
    layout->addWidget(schemeCard);

    auto *previewCard = new SettingsCard(tr("实时预览"), page);
    m_previewLabel->setObjectName(QStringLiteral("terminalSettingsPreview"));
    m_previewLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_previewLabel->setMinimumHeight(130);
    m_previewLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_previewLabel->setText(QStringLiteral(
        "deploy@server:~$ ssh app@10.0.0.8\n"
        "Shellium · UTF-8 · 256 色 / TrueColor\n"
        "https://shellium.example/docs"));
    previewCard->addFullWidthWidget(m_previewLabel);
    layout->addWidget(previewCard);

    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildTerminalPage()
{
    auto *page = new QWidget(m_pages);
    page->setObjectName(QStringLiteral("SettingsPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 22, 24, 22);
    layout->setSpacing(16);

    m_fontCombo->setObjectName(QStringLiteral("terminalFontCombo"));
    m_fontCombo->setMinimumWidth(260);
    m_fontCombo->setMaxVisibleItems(18);
    populateFontCombo();
    m_fontSizeSpin->setObjectName(QStringLiteral("terminalFontSizeSpin"));
    m_fontSizeSpin->setRange(6, 72);
    m_lineHeightSpin->setObjectName(QStringLiteral("terminalLineHeightSpin"));
    m_lineHeightSpin->setRange(80, 250);
    m_lineHeightSpin->setSuffix(QStringLiteral("%"));
    m_letterSpacingSpin->setObjectName(QStringLiteral("terminalLetterSpacingSpin"));
    m_letterSpacingSpin->setRange(-2.0, 10.0);
    m_letterSpacingSpin->setDecimals(1);
    m_letterSpacingSpin->setSingleStep(0.5);
    m_letterSpacingSpin->setSuffix(tr(" px"));

    auto *fontCard = new SettingsCard(tr("字体"), page);
    fontCard->addRow(tr("等宽字体"), m_fontCombo);
    fontCard->addRow(tr("字号"), new SettingsStepper(m_fontSizeSpin, fontCard));
    fontCard->addRow(tr("行高"), new SettingsStepper(m_lineHeightSpin, fontCard));
    fontCard->addRow(tr("字符间距"), new SettingsStepper(m_letterSpacingSpin, fontCard));
    layout->addWidget(fontCard);

    m_cursorShapeSelector->setObjectName(QStringLiteral("cursorShapeSelector"));
    m_cursorShapeSelector->addSegment(tr("块状"), static_cast<int>(TerminalCursorShape::Block));
    m_cursorShapeSelector->addSegment(tr("下划线"),
                                      static_cast<int>(TerminalCursorShape::Underline));
    m_cursorShapeSelector->addSegment(tr("竖线"), static_cast<int>(TerminalCursorShape::IBeam));
    m_cursorBlinkToggle->setObjectName(QStringLiteral("cursorBlinkToggle"));

    auto *cursorCard = new SettingsCard(tr("光标"), page);
    cursorCard->addRow(tr("形态"), m_cursorShapeSelector);
    cursorCard->addRow(tr("闪烁光标"), m_cursorBlinkToggle);
    layout->addWidget(cursorCard);

    m_scrollbackSpin->setObjectName(QStringLiteral("scrollbackSpin"));
    m_scrollbackSpin->setRange(100, 1000000);
    m_scrollbackSpin->setSingleStep(1000);
    m_scrollbackSpin->setSuffix(tr(" 行"));
    m_scrollbackSpin->setMinimumWidth(120);
    m_audibleBellToggle->setObjectName(QStringLiteral("audibleBellToggle"));

    auto *sessionCard = new SettingsCard(tr("会话"), page);
    sessionCard->addRow(tr("滚动缓冲"),
                        new SettingsStepper(m_scrollbackSpin, sessionCard),
                        tr("减小该值会立即丢弃超过上限的最旧历史；已经丢弃的内容无法恢复。"));
    sessionCard->addRow(tr("提示音"),
                        m_audibleBellToggle,
                        tr("收到终端 BEL 字节时播放系统提示音。"));
    layout->addWidget(sessionCard);

    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildConnectionPage()
{
    auto *page = new QWidget(m_pages);
    page->setObjectName(QStringLiteral("SettingsPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 22, 24, 22);
    layout->setSpacing(16);

    m_automaticReconnectToggle->setObjectName(QStringLiteral("automaticReconnectToggle"));
    m_reconnectMaxAttemptsSpin->setObjectName(QStringLiteral("reconnectMaxAttemptsSpin"));
    m_reconnectMaxAttemptsSpin->setRange(0, 10);
    m_reconnectMaxAttemptsSpin->setSuffix(tr(" 次"));
    m_reconnectBaseDelaySpin->setObjectName(QStringLiteral("reconnectBaseDelaySpin"));
    m_reconnectBaseDelaySpin->setRange(1, 60);
    m_reconnectBaseDelaySpin->setSuffix(tr(" 秒"));

    auto *card = new SettingsCard(tr("自动重连"), page);
    card->addRow(tr("网络异常时自动重连"),
                 m_automaticReconnectToggle,
                 tr("仅重试已经成功建立过、且未收到远端退出状态的异常断线。"));
    // 步进器容器需要整行置灰，因此记录 addRow 返回的控件而不是内部 spin box。
    m_reconnectDependentRows.append(
        card->addRow(tr("最多重试"), new SettingsStepper(m_reconnectMaxAttemptsSpin, card)));
    m_reconnectDependentRows.append(
        card->addRow(tr("首轮等待"),
                     new SettingsStepper(m_reconnectBaseDelaySpin, card),
                     tr("后续尝试使用 2 倍指数退避，最长等待 60 秒。")));
    card->addNote(tr("正常执行 exit、主动切换连接、首次认证失败或主机密钥拒绝均不会自动重连。"));
    layout->addWidget(card);

    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildShortcutPage()
{
    auto *page = new QWidget(m_pages);
    page->setObjectName(QStringLiteral("SettingsPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 22, 24, 22);
    layout->setSpacing(16);

    auto *card = new SettingsCard(tr("全局快捷键"), page);
    for (const QString &id : TerminalSettings::shortcutIds()) {
        auto *editor = new QKeySequenceEdit(card);
        editor->setObjectName(QStringLiteral("shortcut_%1").arg(id));
        editor->setClearButtonEnabled(true);
        editor->setMaximumSequenceLength(1);
        editor->setMinimumWidth(180);
        editor->setMaximumWidth(220);
        card->addRow(TerminalSettings::shortcutDisplayName(id), editor);
        m_shortcutEditors.insert(id, editor);
    }
    card->addNote(tr("留空表示取消该动作的快捷键；同一快捷键不能分配给两个动作。"));
    layout->addWidget(card);

    layout->addStretch(1);
    return page;
}

TerminalSettings SettingsDialog::editedSettings() const
{
    TerminalSettings settings;
    settings.themeMode = static_cast<ThemeMode>(m_themeSelector->currentData());
    settings.colorSchemeId = QStringLiteral("default");
    for (const QString &id : m_colorSchemeOrder) {
        ThemeSwatchButton *swatch = m_colorSchemeSwatches.value(id);
        if (swatch != nullptr && swatch->isChecked()) {
            settings.colorSchemeId = id;
            break;
        }
    }
    settings.fontFamily = m_fontCombo->currentData().toString();
    settings.fontPointSize = m_fontSizeSpin->value();
    settings.lineHeightPercent = m_lineHeightSpin->value();
    settings.letterSpacing = m_letterSpacingSpin->value();
    settings.cursorShape =
        static_cast<TerminalCursorShape>(m_cursorShapeSelector->currentData());
    settings.cursorBlink = m_cursorBlinkToggle->isChecked();
    settings.scrollbackLines = m_scrollbackSpin->value();
    settings.audibleBell = m_audibleBellToggle->isChecked();
    settings.automaticReconnect = m_automaticReconnectToggle->isChecked();
    settings.reconnectMaxAttempts = m_reconnectMaxAttemptsSpin->value();
    settings.reconnectBaseDelaySeconds = m_reconnectBaseDelaySpin->value();
    for (auto iterator = m_shortcutEditors.cbegin(); iterator != m_shortcutEditors.cend(); ++iterator) {
        settings.shortcuts.insert(iterator.key(), iterator.value()->keySequence());
    }
    return settings.normalized();
}

void SettingsDialog::accept()
{
    QString error;
    if (!m_service->save(editedSettings(), &error)) {
        QMessageBox::warning(this, tr("无法保存设置"), error);
        return;
    }
    m_originalSettings = m_service->currentSettings();
    QDialog::accept();
}

void SettingsDialog::reject()
{
    // applyTransient 不写数据库，因此恢复原快照既能让所有工作区回到原外观，也不会触发
    // 没必要的 SQLite 写事务。
    m_service->applyTransient(m_originalSettings);
    QDialog::reject();
}

void SettingsDialog::populateControls(const TerminalSettings &settings)
{
    m_populating = true;
    const TerminalSettings normalized = settings.normalized();
    m_themeSelector->setCurrentData(static_cast<int>(normalized.themeMode));
    for (auto iterator = m_colorSchemeSwatches.cbegin();
         iterator != m_colorSchemeSwatches.cend();
         ++iterator) {
        iterator.value()->setChecked(iterator.key() == normalized.colorSchemeId);
    }
    refreshDefaultSwatchPreview(normalized);

    // 用户显式选过的字体可能已被卸载，或来自另一台机器：此时不静默改成别的字体，而是补一条
    // 标注条目，让用户看到自己选的名字仍然生效（Qt 会按字体回退渲染），也能主动换成本机已有
    // 字体。反之，尚未选过字体（空值）时不该凭空冒出“未安装”——某些平台的默认等宽字体名
    // （如 "monospace"）只是一个别名，本来就不会出现在字体族列表里，直接选第一个可用条目。
    const bool explicitFamily = !normalized.fontFamily.isEmpty();
    const QString desiredFamily = explicitFamily
        ? normalized.fontFamily
        : QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    int fontIndex = m_fontCombo->findData(desiredFamily);
    if (fontIndex < 0 && explicitFamily) {
        m_fontCombo->addItem(tr("%1（本机未安装）").arg(desiredFamily), desiredFamily);
        fontIndex = m_fontCombo->count() - 1;
    }
    m_fontCombo->setCurrentIndex(std::max(fontIndex, 0));
    m_fontSizeSpin->setValue(normalized.fontPointSize);
    m_lineHeightSpin->setValue(normalized.lineHeightPercent);
    m_letterSpacingSpin->setValue(normalized.letterSpacing);
    m_cursorShapeSelector->setCurrentData(static_cast<int>(normalized.cursorShape));
    m_cursorBlinkToggle->setChecked(normalized.cursorBlink);
    m_scrollbackSpin->setValue(normalized.scrollbackLines);
    m_audibleBellToggle->setChecked(normalized.audibleBell);
    m_automaticReconnectToggle->setChecked(normalized.automaticReconnect);
    m_reconnectMaxAttemptsSpin->setValue(normalized.reconnectMaxAttempts);
    m_reconnectBaseDelaySpin->setValue(normalized.reconnectBaseDelaySeconds);
    for (QWidget *row : std::as_const(m_reconnectDependentRows)) {
        row->setEnabled(normalized.automaticReconnect);
    }
    for (auto iterator = m_shortcutEditors.cbegin(); iterator != m_shortcutEditors.cend(); ++iterator) {
        iterator.value()->setKeySequence(normalized.shortcuts.value(iterator.key()));
    }
    m_populating = false;
}

void SettingsDialog::previewChanges()
{
    if (m_populating) {
        return;
    }
    const TerminalSettings settings = editedSettings();
    m_service->applyTransient(settings);
    refreshDefaultSwatchPreview(settings);
    updatePreviewCard(settings);
}

void SettingsDialog::refreshDefaultSwatchPreview(const TerminalSettings &settings)
{
    ThemeSwatchButton *defaultSwatch = m_colorSchemeSwatches.value(QStringLiteral("default"));
    if (defaultSwatch == nullptr) {
        return;
    }
    // “默认”配色是主题模式的函数，切换浅色/深色后必须重绘，否则色板会长期显示另一套颜色。
    const TerminalPalette palette = paletteForScheme(settings, QStringLiteral("default"));
    defaultSwatch->setPreviewColors(palette.background,
                                    palette.foreground,
                                    palette.cursor,
                                    palette.link);
}

void SettingsDialog::updatePreviewCard(const TerminalSettings &settings)
{
    const TerminalSettings normalized = settings.normalized();
    const TerminalPalette palette = normalized.effectivePalette();
    QFont font = normalized.fontFamily.isEmpty()
        ? QFontDatabase::systemFont(QFontDatabase::FixedFont)
        : QFont(normalized.fontFamily);
    font.setPointSize(normalized.fontPointSize);
    font.setLetterSpacing(QFont::AbsoluteSpacing, normalized.letterSpacing);
    m_previewLabel->setFont(font);
    m_previewLabel->setStyleSheet(
        QStringLiteral("QLabel#terminalSettingsPreview { background: %1; color: %2; padding: 14px; "
                       "line-height: %3%; border: 1px solid %4; border-radius: 10px; }")
            .arg(palette.background.name(),
                 palette.foreground.name(),
                 QString::number(normalized.lineHeightPercent),
                 palette.selection.name()));
}
