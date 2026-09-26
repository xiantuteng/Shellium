#include "ui/SettingsDialog.h"

#include "ai/AiClient.h"
#include "ai/AiSettings.h"
#include "ai/AiSettingsService.h"
#include "i18n/LocalizationService.h"
#include "i18n/Retranslate.h"
#include "settings/BundledFonts.h"
#include "settings/TerminalSettings.h"
#include "security/Vault.h"
#include "ui/SettingsWidgets.h"
#include "ui/UiIcons.h"
#include "ui/VaultDialog.h"

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
#include <QToolButton>
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
    ShortcutPage,
    AiPage
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
    : SettingsDialog(service, nullptr, nullptr, parent)
{
}

SettingsDialog::SettingsDialog(TerminalSettingsService *service,
                               CredentialVault *credentialVault,
                               QWidget *parent)
    : SettingsDialog(service, credentialVault, nullptr, parent)
{
}

SettingsDialog::SettingsDialog(TerminalSettingsService *service,
                               CredentialVault *credentialVault,
                               AiSettingsService *aiSettingsService,
                               QWidget *parent)
    : QDialog(parent)
    , m_service(service)
    , m_persistedSettings(service == nullptr ? TerminalSettings{}.normalized()
                                             : service->currentSettings())
    , m_navigationList(new QListWidget(this))
    , m_pages(new QStackedWidget(this))
    , m_languageSelector(new QComboBox(this))
    , m_themeSelector(new SegmentedControl(this))
    , m_sidebarDensitySelector(new SegmentedControl(this))
    , m_sidebarDoubleClickPromptToggle(new ToggleSwitch(this))
    , m_fontCombo(new QComboBox(this))
    , m_fontSizeSpin(new QSpinBox(this))
    , m_lineHeightSpin(new QSpinBox(this))
    , m_letterSpacingSpin(new QDoubleSpinBox(this))
    , m_cursorShapeSelector(new SegmentedControl(this))
    , m_cursorBlinkToggle(new ToggleSwitch(this))
    , m_scrollbackSpin(new QSpinBox(this))
    , m_scrollbackMaxMegabytesSpin(new QSpinBox(this))
    , m_audibleBellToggle(new ToggleSwitch(this))
    , m_commandAutoCompletionToggle(new ToggleSwitch(this))
    , m_multiLinePasteConfirmationToggle(new ToggleSwitch(this))
    , m_automaticReconnectToggle(new ToggleSwitch(this))
    , m_vaultPasswordEnabledToggle(new ToggleSwitch(this))
    , m_reconnectMaxAttemptsSpin(new QSpinBox(this))
    , m_reconnectBaseDelaySpin(new QSpinBox(this))
    , m_previewLabel(new QLabel(this))
    , m_credentialVault(credentialVault)
    , m_aiSettingsService(aiSettingsService)
    , m_aiTestClient(new AiClient(this))
{
    Q_ASSERT(m_service != nullptr);
    UiIcons::modernizePopup(m_fontCombo);
    setObjectName(QStringLiteral("SettingsDialog"));
    setWindowTitle(tr("Settings"));
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
    auto *sidebarTitle = new QLabel(tr("Settings"), sidebar);
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
    m_pages->addWidget(wrapInScrollArea(buildAiPage()));
    buildNavigation();

    auto *footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("SettingsFooter"));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(20, 12, 20, 12);
    footerLayout->setSpacing(10);
    auto *restoreButton = new QPushButton(tr("Restore defaults"), footer);
    restoreButton->setObjectName(QStringLiteral("settingsRestoreDefaults"));
    auto *closeButton = new QPushButton(tr("Close"), footer);
    closeButton->setObjectName(QStringLiteral("settingsClose"));
    i18n::onRetranslate(this, [this, sidebarTitle, restoreButton, closeButton] {
        setWindowTitle(tr("Settings"));
        sidebarTitle->setText(tr("Settings"));
        restoreButton->setText(tr("Restore defaults"));
        closeButton->setText(tr("Close"));
    });
    footerLayout->addWidget(restoreButton);
    footerLayout->addStretch(1);
    footerLayout->addWidget(closeButton);
    rootLayout->addWidget(footer);

    // 自动保存后窗口不再持有“待提交草稿”。关闭按钮只结束对话框，Esc 触发的 reject() 也
    // 使用 QDialog 默认行为，二者都不会撤销已经成功写入数据库的设置。
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(restoreButton, &QPushButton::clicked, this, [this]() {
        populateControls(TerminalSettings{}.normalized());
        // “恢复默认”是一次明确的批量操作，使用服务的完整设置事务一次提交，避免逐字段写入
        // 时应用短暂处于半默认状态；若涉及关闭主密码保护，persistChanges() 会照常要求验证。
        persistChanges();
    });

    connect(m_languageSelector, &QComboBox::currentIndexChanged,
            this, [this](int) { persistChanges(); });
    connect(m_sidebarDensitySelector, &SegmentedControl::currentDataChanged,
            this, [this](int) { persistChanges(); });
    connect(m_themeSelector, &SegmentedControl::currentDataChanged,
            this, [this](int) { persistChanges(); });
    connect(m_cursorShapeSelector, &SegmentedControl::currentDataChanged,
            this, [this](int) { persistChanges(); });
    connect(m_fontCombo, &QComboBox::currentTextChanged,
            this, [this](const QString &) { persistChanges(); });
    connect(m_fontSizeSpin, &QSpinBox::valueChanged, this, [this](int) { persistChanges(); });
    connect(m_lineHeightSpin, &QSpinBox::valueChanged, this, [this](int) { persistChanges(); });
    connect(m_letterSpacingSpin, &QDoubleSpinBox::valueChanged,
            this, [this](double) { persistChanges(); });
    connect(m_scrollbackSpin, &QSpinBox::valueChanged, this, [this](int) { persistChanges(); });
    connect(m_scrollbackMaxMegabytesSpin, &QSpinBox::valueChanged,
            this, [this](int) { persistChanges(); });
    connect(m_reconnectMaxAttemptsSpin, &QSpinBox::valueChanged,
            this, [this](int) { persistChanges(); });
    connect(m_reconnectBaseDelaySpin, &QSpinBox::valueChanged,
            this, [this](int) { persistChanges(); });

    // 先更新重连依赖项的可用状态，再执行持久化。若保存失败，populateControls() 触发的嵌套
    // toggled 信号会把行状态恢复为持久值；此连接顺序可避免外层旧参数在回滚完成后再次覆盖它。
    connect(m_automaticReconnectToggle, &ToggleSwitch::toggled, this, [this](const bool enabled) {
        for (QWidget *row : std::as_const(m_reconnectDependentRows)) {
            row->setEnabled(enabled);
        }
    });
    for (ToggleSwitch *toggle :
         {m_cursorBlinkToggle,
         m_audibleBellToggle,
         m_commandAutoCompletionToggle,
         m_multiLinePasteConfirmationToggle,
         m_automaticReconnectToggle,
         m_sidebarDoubleClickPromptToggle}) {
        connect(toggle, &ToggleSwitch::toggled, this, [this](bool) { persistChanges(); });
    }
    // 主密码保护不是普通布尔值：开关改变后立即进入专用验证/迁移流程；只有流程完成且设置
    // 快照写入成功，控件才保持新状态。用户关闭密码对话框时会自动回到原持久值。
    connect(m_vaultPasswordEnabledToggle,
            &ToggleSwitch::toggled,
            this,
            [this](bool) { persistChanges(); });
    for (QKeySequenceEdit *editor : std::as_const(m_shortcutEditors)) {
        connect(editor, &QKeySequenceEdit::keySequenceChanged,
                this, [this](const QKeySequence &) { persistChanges(); });
    }

    // 菜单及其他设置窗口共享服务；外部广播只刷新快照，populateControls 的保护位阻止回写。
    connect(m_service, &TerminalSettingsService::settingsChanged, this,
            [this](const TerminalSettings &settings) {
                m_persistedSettings = settings;
                populateControls(settings);
                updatePreviewCard(settings);
            });
    populateControls(m_persistedSettings);
    updatePreviewCard(m_persistedSettings);
}

void SettingsDialog::buildNavigation()
{
    m_navigationList->setObjectName(QStringLiteral("SettingsNav"));
    m_navigationList->setFrameShape(QFrame::NoFrame);
    m_navigationList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navigationList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_navigationList->setUniformItemSizes(true);
    for (int i = 0; i < 5; ++i) {
        m_navigationList->addItem(QString());
    }
    i18n::onRetranslate(this, [this] {
        const QStringList entries{tr("Appearance"), tr("Terminal"),
                                  tr("Connections & Security"), tr("Shortcut"),
                                  tr("AI Assistant")};
        for (int row = 0; row < entries.size() && row < m_navigationList->count(); ++row) {
            m_navigationList->item(row)->setText(entries.at(row));
        }
    });
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
    const auto appendFamily = [this](const QString &family) {
        QFont itemFont(family);
        itemFont.setPointSize(12);
        m_fontCombo->addItem(family, family);
        m_fontCombo->setItemData(m_fontCombo->count() - 1, itemFont, Qt::FontRole);
    };

    // 内置字体排在最前，并且不与本机安装求交集：它们随程序分发，一定可用。这里主动注册一次
    // 而不是假设 main() 已经注册过，是为了让只构造对话框的界面测试也走同一条真实路径。
    const QStringList bundled = BundledFonts::registerAll();
    for (const QString &family : bundled) {
        appendFamily(family);
    }

    const QStringList installed = QFontDatabase::families();
    QStringList recommended;
    for (const QString &family : TerminalSettings::recommendedFontFamilies()) {
        // 内置字体注册后同样会出现在 QFontDatabase 里，不排除就会在两个分组各出现一次。
        if (installed.contains(family) && !bundled.contains(family)) {
            recommended.append(family);
        }
    }
    if (!recommended.isEmpty() && !bundled.isEmpty()) {
        m_fontCombo->insertSeparator(m_fontCombo->count());
    }
    for (const QString &family : std::as_const(recommended)) {
        appendFamily(family);
    }

    QStringList others;
    for (const QString &family : installed) {
        if (!bundled.contains(family) && !recommended.contains(family)
            && isLikelyMonospaceFamily(family)) {
            others.append(family);
        }
    }
    others.sort(Qt::CaseInsensitive);
    if (!others.isEmpty() && (!recommended.isEmpty() || !bundled.isEmpty())) {
        // 分隔条目：不可选，只用来区分“内置/推荐编程字体”和本机其余等宽字体。
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

    m_languageSelector->setObjectName(QStringLiteral("languageSelector"));
    // 语言下拉的列表是独立的顶层弹出窗口；仅靠 QSS 的 border-radius 会让内部列表圆角化，
    // 但 Qt 默认的弹出容器仍会绘制一圈直角 StyledPanel 边框。复用统一的弹出层处理，关闭
    // 外层容器装饰并保留内部列表的圆角边框，从而避免四角出现截图中的黑色直角框。
    UiIcons::modernizePopup(m_languageSelector);
    if (m_languageSelector->count() == 0) {
        for (const LanguageOption &option : LocalizationService::availableLanguages()) {
            m_languageSelector->addItem(option.nativeName, option.code);
        }
    }
    auto *languageCard = new SettingsCard([this] { return tr("Interface language"); }, page);
    languageCard->addRow([this] { return tr("Display language"); },
                         m_languageSelector,
                         [this] { return tr("The whole interface switches immediately. Terminal "
                                            "output that has already scrolled by keeps its "
                                            "original language."); });
    layout->addWidget(languageCard);
    // “跟随系统”是唯一需要随语言变化的下拉项文案（其余是各语言自身的写法）。
    i18n::onRetranslate(m_languageSelector, [this] {
        const int systemIndex = m_languageSelector->findData(QStringLiteral("system"));
        if (systemIndex >= 0) {
            m_languageSelector->setItemText(systemIndex, LocalizationService::availableLanguages()
                                                            .constFirst()
                                                            .nativeName);
        }
    });

    m_themeSelector->setObjectName(QStringLiteral("themeSelector"));
    m_themeSelector->addSegment([this] { return tr("Follow System"); }, static_cast<int>(ThemeMode::FollowSystem));
    m_themeSelector->addSegment([this] { return tr("Light"); }, static_cast<int>(ThemeMode::Light));
    m_themeSelector->addSegment([this] { return tr("Dark"); }, static_cast<int>(ThemeMode::Dark));

    auto *themeCard = new SettingsCard([this] { return tr("App theme"); }, page);
    themeCard->addRow([this] { return tr("Interface appearance"); },
                      m_themeSelector,
                      [this] { return tr("'Follow system' syncs automatically when the operating "
                                         "system switches between light and dark appearance."); });
    layout->addWidget(themeCard);

    // 分段文案使用延迟翻译回调，沿用 SettingsWidgets 的热切换机制，无需重新打开设置。
    m_sidebarDensitySelector->setObjectName(QStringLiteral("sidebarDensitySelector"));
    m_sidebarDensitySelector->addSegment([this] { return tr("Relaxed"); }, 0);
    m_sidebarDensitySelector->addSegment([this] { return tr("Compact"); }, 1);
    m_sidebarDensitySelector->addSegment([this] { return tr("Minimal"); }, 2);
    themeCard->addRow([this] { return tr("Sidebar density"); }, m_sidebarDensitySelector);

    m_sidebarDoubleClickPromptToggle->setObjectName(QStringLiteral("sidebarDoubleClickPromptToggle"));
    themeCard->addRow([this] { return tr("Confirm before hiding the sidebar"); },
                      m_sidebarDoubleClickPromptToggle,
                      [this] {
                          return tr("Ask before hiding the sidebar when you double-click its divider.");
                      });

    auto *schemeCard = new SettingsCard([this] { return tr("Terminal color scheme"); }, page);
    m_colorSchemeFilter = new QLineEdit(schemeCard);
    m_colorSchemeFilter->setObjectName(QStringLiteral("colorSchemeFilter"));
    m_colorSchemeFilter->setClearButtonEnabled(true);
    m_colorSchemeFilter->setPlaceholderText(tr("Search color schemes, e.g. Nord, Dark, Gruvbox"));
    schemeCard->addFullWidthWidget(m_colorSchemeFilter);

    m_colorSchemeGrid = new QWidget(schemeCard);
    auto *gridLayout = new QGridLayout(m_colorSchemeGrid);
    gridLayout->setContentsMargins(0, 0, 0, 0);
    gridLayout->setHorizontalSpacing(12);
    gridLayout->setVerticalSpacing(10);

    for (const TerminalThemePreset &preset : TerminalSettings::themePresets()) {
        const bool isDefault = preset.id == QLatin1String("default");
        const TerminalPalette palette = isDefault
            ? paletteForScheme(m_persistedSettings, preset.id)
            : preset.palette;
        auto *swatch = new ThemeSwatchButton(preset.name,
                                            isDefault ? tr("Follow app theme")
                                                      : (preset.dark ? tr("Dark") : tr("Light")),
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
            persistChanges();
        });
    }
    relayoutColorSchemeGrid(QString());

    // 色板标题（“Default”会翻译，其余是品牌名保持原样）和「跟随应用主题 / 深色 / 浅色」
    // 副标题都是构造期一次性写入的，必须显式登记热切换。
    i18n::onRetranslate(this, [this] {
        for (const TerminalThemePreset &preset : TerminalSettings::themePresets()) {
            ThemeSwatchButton *swatch = m_colorSchemeSwatches.value(preset.id);
            if (swatch == nullptr) {
                continue;
            }
            const bool isDefault = preset.id == QLatin1String("default");
            // themePresets() 是 static 缓存，其中唯一被翻译的 "Default" 名字会冻结在首次
            // 求值时的语言，这里按同一 QObject 上下文实时重译；其余是品牌名，原样使用。
            swatch->setText(isDefault
                                ? QCoreApplication::translate("QObject", "Default")
                                : preset.name);
            swatch->setSubtitle(isDefault ? tr("Follow app theme")
                                          : (preset.dark ? tr("Dark") : tr("Light")));
        }
    });
    schemeCard->addFullWidthWidget(m_colorSchemeGrid);
    m_colorSchemeEmptyLabel = schemeCard->addNote([this] { return tr("No matching color schemes."); });
    m_colorSchemeEmptyLabel->setVisible(false);
    connect(m_colorSchemeFilter, &QLineEdit::textChanged,
            this, [this](const QString &text) { relayoutColorSchemeGrid(text); });
    layout->addWidget(schemeCard);

    auto *previewCard = new SettingsCard([this] { return tr("Live preview"); }, page);
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

    // 卡片标题、行文案、分段控件已由 provider 版接口自动重译；这里补上非 SettingsCard 承载
    // 的那几处：配色搜索框占位符、色板卡副标题。
    i18n::onRetranslate(this, [this] {
        m_colorSchemeFilter->setPlaceholderText(
            tr("Search color schemes, e.g. Nord, Dark, Gruvbox"));
        for (const TerminalThemePreset &preset : TerminalSettings::themePresets()) {
            if (ThemeSwatchButton *swatch = m_colorSchemeSwatches.value(preset.id)) {
                swatch->setSubtitle(preset.id == QLatin1String("default")
                                        ? tr("Follow app theme")
                                        : (preset.dark ? tr("Dark") : tr("Light")));
            }
        }
    });

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

    auto *fontCard = new SettingsCard([this] { return tr("Font"); }, page);
    fontCard->addRow([this] { return tr("Monospace font"); }, m_fontCombo);
    fontCard->addRow([this] { return tr("Font size"); }, new SettingsStepper(m_fontSizeSpin, fontCard));
    fontCard->addRow([this] { return tr("Line height"); }, new SettingsStepper(m_lineHeightSpin, fontCard));
    fontCard->addRow([this] { return tr("Letter spacing"); }, new SettingsStepper(m_letterSpacingSpin, fontCard));
    layout->addWidget(fontCard);

    m_cursorShapeSelector->setObjectName(QStringLiteral("cursorShapeSelector"));
    m_cursorShapeSelector->addSegment([this] { return tr("Block"); }, static_cast<int>(TerminalCursorShape::Block));
    m_cursorShapeSelector->addSegment([this] { return tr("Underline"); },
                                      static_cast<int>(TerminalCursorShape::Underline));
    m_cursorShapeSelector->addSegment([this] { return tr("Bar"); }, static_cast<int>(TerminalCursorShape::IBeam));
    m_cursorBlinkToggle->setObjectName(QStringLiteral("cursorBlinkToggle"));

    auto *cursorCard = new SettingsCard([this] { return tr("Cursor"); }, page);
    cursorCard->addRow([this] { return tr("Shape"); }, m_cursorShapeSelector);
    cursorCard->addRow([this] { return tr("Blinking cursor"); }, m_cursorBlinkToggle);
    layout->addWidget(cursorCard);

    m_scrollbackSpin->setObjectName(QStringLiteral("scrollbackSpin"));
    m_scrollbackSpin->setRange(100, 1000000);
    m_scrollbackSpin->setSingleStep(1000);
    m_scrollbackSpin->setMinimumWidth(120);
    m_scrollbackMaxMegabytesSpin->setObjectName(QStringLiteral("scrollbackMaxMegabytesSpin"));
    m_scrollbackMaxMegabytesSpin->setRange(1, 512);
    m_scrollbackMaxMegabytesSpin->setSingleStep(1);
    m_scrollbackMaxMegabytesSpin->setMinimumWidth(120);

    // QSpinBox 的后缀是一次性 setSuffix，语言切换时不会自己更新；显式登记进热切换闭包，
    // 否则切到别的语言后数值旁仍残留上一次语言的“行 / px”。
    i18n::onRetranslate(this, [this] {
        m_letterSpacingSpin->setSuffix(tr(" px"));
        m_scrollbackSpin->setSuffix(tr(" lines"));
        m_scrollbackMaxMegabytesSpin->setSuffix(tr(" MB"));
    });
    m_audibleBellToggle->setObjectName(QStringLiteral("audibleBellToggle"));
    m_commandAutoCompletionToggle->setObjectName(
        QStringLiteral("commandAutoCompletionToggle"));
    m_multiLinePasteConfirmationToggle->setObjectName(
        QStringLiteral("multiLinePasteConfirmationToggle"));

    auto *sessionCard = new SettingsCard([this] { return tr("Session"); }, page);
    sessionCard->addRow([this] { return tr("Scrollback"); },
                        new SettingsStepper(m_scrollbackSpin, sessionCard),
                        [this] { return tr("Lowering this value immediately discards the oldest history beyond the limit; discarded content cannot be recovered."); });
    sessionCard->addRow([this] { return tr("Scrollback memory cap"); },
                        new SettingsStepper(m_scrollbackMaxMegabytesSpin, sessionCard),
                        [this] { return tr("Upper bound on the memory one session's scrollback may use. The oldest lines are discarded once either this or the line limit is reached; only very long lines or very large line limits come close to it."); });
    sessionCard->addRow([this] { return tr("Alert sound"); },
                        m_audibleBellToggle,
                        [this] { return tr("Play the system alert sound when a terminal BEL byte is received."); });
    sessionCard->addRow(
        [this] { return tr("Command-line autosuggestions"); },
        m_commandAutoCompletionToggle,
        [this] { return tr("After you type at least two characters, suggestions are shown automatically from the current host's command history; off by default."); });
    sessionCard->addRow(
        [this] { return tr("Multi-line paste confirmation"); },
        m_multiLinePasteConfirmationToggle,
        [this] { return tr("Before pasting multi-line content, show the full content and ask for confirmation; off by default, i.e. paste directly. Confirmation is still forced when the content contains invisible characters that could inject terminal control sequences."); });
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
    m_reconnectBaseDelaySpin->setObjectName(QStringLiteral("reconnectBaseDelaySpin"));
    m_reconnectBaseDelaySpin->setRange(1, 60);
    // 后缀同样必须随语言热切换，见 buildTerminalPage 的同类闭包。
    i18n::onRetranslate(this, [this] {
        m_reconnectMaxAttemptsSpin->setSuffix(tr(" times"));
        m_reconnectBaseDelaySpin->setSuffix(tr(" s"));
    });

    m_vaultPasswordEnabledToggle->setObjectName(
        QStringLiteral("vaultPasswordEnabledToggle"));
    auto *vaultCard = new SettingsCard([this] { return tr("Credential vault"); }, page);
    vaultCard->addRow(
        [this] { return tr("Protect credentials with a master password"); },
        m_vaultPasswordEnabledToggle,
        [this] { return tr("Both enabling and disabling require a password; once enabled you can unlock and view plaintext on the host editor page."); });
    vaultCard->addNote(
        [this] { return tr("When disabling, the current master password is verified first, then credentials are migrated to local encryption and the master password configuration is cleared. Host passwords are still used automatically for connections, but their plaintext cannot be viewed in the UI and they no longer have master password protection."); });
    layout->addWidget(vaultCard);

    auto *card = new SettingsCard([this] { return tr("Automatic reconnect"); }, page);
    card->addRow([this] { return tr("Reconnect automatically on network errors"); },
                 m_automaticReconnectToggle,
                 [this] { return tr("Only retry abnormal disconnects that were previously established successfully and received no remote exit status."); });
    // 步进器容器需要整行置灰，因此记录 addRow 返回的控件而不是内部 spin box。
    m_reconnectDependentRows.append(
        card->addRow([this] { return tr("Max retries"); }, new SettingsStepper(m_reconnectMaxAttemptsSpin, card)));
    m_reconnectDependentRows.append(
        card->addRow([this] { return tr("Initial wait"); },
                     new SettingsStepper(m_reconnectBaseDelaySpin, card),
                     [this] { return tr("Later attempts use 2× exponential backoff, up to 60 seconds."); }));
    card->addNote([this] { return tr("A normal exit, switching connections yourself, a first-time authentication failure, or a host key rejection never trigger an automatic reconnect."); });
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

    auto *card = new SettingsCard([this] { return tr("Global shortcuts"); }, page);
    for (const QString &id : TerminalSettings::shortcutIds()) {
        auto *editor = new QKeySequenceEdit(card);
        editor->setObjectName(QStringLiteral("shortcut_%1").arg(id));
        editor->setClearButtonEnabled(true);
        editor->setMaximumSequenceLength(1);
        editor->setMinimumWidth(180);
        editor->setMaximumWidth(220);
        card->addRow([id] { return TerminalSettings::shortcutDisplayName(id); }, editor);
        m_shortcutEditors.insert(id, editor);
    }
    card->addNote([this] { return tr("Leaving it empty removes the shortcut for this action; the same shortcut cannot be assigned to two actions."); });
    layout->addWidget(card);

    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildAiPage()
{
    auto *page = new QWidget(m_pages);
    page->setObjectName(QStringLiteral("SettingsPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 22, 24, 22);
    layout->setSpacing(16);

    auto *card = new SettingsCard([this] { return tr("LLM connection"); }, page);
    m_aiEndpointEdit = new QLineEdit(card);
    m_aiEndpointEdit->setObjectName(QStringLiteral("aiEndpointEdit"));
    m_aiEndpointEdit->setPlaceholderText(QStringLiteral("https://your-gateway/v1"));
    // 服务地址、API Key 和模型名称都可能显著长于普通设置值，因此使用上下排列的宽表单行。
    // 输入框会占满卡片内容区并随窗口伸缩，用户无需把光标放进窄框后逐字符横向滚动确认配置。
    card->addWideRow(
        [this] { return tr("Service URL"); },
        m_aiEndpointEdit,
        [this] { return tr("Base URL of a gateway compatible with the OpenAI Chat Completions protocol, without /chat/completions."); });

    m_aiApiKeyEdit = new QLineEdit(card);
    m_aiApiKeyEdit->setObjectName(QStringLiteral("aiApiKeyEdit"));
    m_aiApiKeyEdit->setEchoMode(QLineEdit::Password);
    m_aiApiKeyEdit->setPlaceholderText(tr("Empty means the gateway requires no authentication"));
    card->addWideRow(
        [this] { return tr("API Key"); },
        m_aiApiKeyEdit,
        [this] { return tr("Sent with each request as a Bearer token, stored encrypted in the credential vault."); });

    // 模型名称行 = 可编辑下拉框 + 刷新按钮：网关列表拉得到就下拉选，拉不到（网关不实现
    // /models、或者密钥还没填）也不能挡住用户手填一个模型名直接用。
    auto *modelRow = new QWidget(card);
    modelRow->setObjectName(QStringLiteral("aiModelRow"));
    auto *modelLayout = new QHBoxLayout(modelRow);
    modelLayout->setContentsMargins(0, 0, 0, 0);
    modelLayout->setSpacing(8);
    m_aiModelCombo = new QComboBox(modelRow);
    UiIcons::modernizePopup(m_aiModelCombo);
    m_aiModelCombo->setObjectName(QStringLiteral("aiModelCombo"));
    m_aiModelCombo->setEditable(true);
    // 不插入历史条目：下拉列表只应呈现服务端此刻真实提供的模型，用户敲进去的临时名字混在
    // 其中会让"这个模型到底存不存在"无法分辨。
    m_aiModelCombo->setInsertPolicy(QComboBox::NoInsert);
    m_aiModelCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_aiModelCombo->lineEdit()->setObjectName(QStringLiteral("aiModelEdit"));
    m_aiModelCombo->lineEdit()->setPlaceholderText(
        QStringLiteral("gpt-4o-mini / deepseek-chat / ..."));
    m_aiModelRefreshButton = new QToolButton(modelRow);
    m_aiModelRefreshButton->setObjectName(QStringLiteral("aiModelRefreshButton"));
    m_aiModelRefreshButton->setText(QStringLiteral("⟳"));
    // 复用步进器按钮的视觉：设置页里所有"输入框旁边的小方按钮"共用一套圆角和描边，
    // 不为刷新单独再造一种样式。
    m_aiModelRefreshButton->setProperty("stepperButton", true);
    m_aiModelRefreshButton->setCursor(Qt::PointingHandCursor);
    m_aiModelRefreshButton->setFocusPolicy(Qt::NoFocus);
    m_aiModelRefreshButton->setFixedSize(32, 32);
    m_aiModelRefreshButton->setToolTip(tr("Fetch the model list from the server"));
    m_aiModelRefreshButton->setAccessibleName(tr("Refresh model list"));
    modelLayout->addWidget(m_aiModelCombo, 1);
    modelLayout->addWidget(m_aiModelRefreshButton, 0);
    card->addWideRow(
        [this] { return tr("Model name"); },
        modelRow,
        [this] { return tr("You can type it manually, or click the refresh button on the right to fetch the server model list and pick from the dropdown."); });

    m_aiThinkingToggle = new ToggleSwitch(card);
    m_aiThinkingToggle->setObjectName(QStringLiteral("aiThinkingToggle"));
    card->addRow(
        [this] { return tr("Enable thinking"); },
        m_aiThinkingToggle,
        [this] { return tr("Ask the model to reason before answering; answers are more reliable but slower. Only effective on gateways that support thinking mode; when off, the request body omits this field."); });

    m_aiStatusLabel = new QLabel(card);
    m_aiStatusLabel->setWordWrap(true);
    m_aiStatusLabel->setObjectName(QStringLiteral("SettingsRowHint"));
    card->addFullWidthWidget(m_aiStatusLabel);

    auto *buttonRow = new QWidget(card);
    auto *buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(0, 4, 0, 0);
    m_aiTestButton = new QPushButton(buttonRow);
    m_aiTestButton->setObjectName(QStringLiteral("settingsAiTest"));
    m_aiSaveButton = new QPushButton(buttonRow);
    m_aiSaveButton->setObjectName(QStringLiteral("settingsAiSave"));
    buttonLayout->addWidget(m_aiTestButton);
    buttonLayout->addWidget(m_aiSaveButton);
    buttonLayout->addStretch(1);
    card->addFullWidthWidget(buttonRow);
    layout->addWidget(card);

    // 这两个按钮不是 SettingsCard 的托管行，文案要自己登记热切换；刷新按钮的 Tooltip/
    // 无障碍名同理。
    i18n::onRetranslate(this, [this] {
        m_aiTestButton->setText(tr("Test connection"));
        m_aiSaveButton->setText(tr("Save"));
        m_aiModelRefreshButton->setToolTip(tr("Fetch the model list from the server"));
        m_aiModelRefreshButton->setAccessibleName(tr("Refresh model list"));
    });

    card->addNote(
        [this] { return tr("Unlike other settings, changes to the address and key are not saved character by character; click 'Save', so that every keystroke does not trigger an encrypted write."); });

    const bool available = m_aiSettingsService != nullptr;
    for (QWidget *field : {static_cast<QWidget *>(m_aiEndpointEdit),
                           static_cast<QWidget *>(m_aiApiKeyEdit),
                           static_cast<QWidget *>(m_aiModelCombo),
                           static_cast<QWidget *>(m_aiModelRefreshButton),
                           static_cast<QWidget *>(m_aiThinkingToggle),
                           static_cast<QWidget *>(m_aiTestButton),
                           static_cast<QWidget *>(m_aiSaveButton)}) {
        field->setEnabled(available);
    }
    if (available) {
        const AiSettings current = m_aiSettingsService->currentSettings();
        m_aiEndpointEdit->setText(current.endpointUrl);
        m_aiApiKeyEdit->setText(current.apiKey);
        // 已保存的模型名此刻还不在候选列表里（列表要联网才拉得到），直接写进编辑框，
        // 保证打开设置页看到的就是正在生效的配置。
        m_aiModelCombo->setCurrentText(current.model);
        m_aiThinkingToggle->setChecked(current.thinkingEnabled);
    } else {
        m_aiStatusLabel->setText(tr("The AI assistant configuration service is unavailable"));
    }

    connect(m_aiTestButton, &QPushButton::clicked, this, [this]() {
        if (m_aiSettingsService == nullptr) {
            return;
        }
        AiSettings probe;
        probe.endpointUrl = m_aiEndpointEdit->text();
        probe.apiKey = m_aiApiKeyEdit->text();
        probe.model = m_aiModelCombo->currentText();
        m_aiTestButton->setEnabled(false);
        m_aiStatusLabel->setStyleSheet(QString());
        m_aiStatusLabel->setText(tr("Testing connection…"));
        m_aiTestClient->testConnection(probe);
    });
    connect(m_aiModelRefreshButton, &QToolButton::clicked, this, [this]() {
        if (m_aiSettingsService == nullptr) {
            return;
        }
        AiSettings probe;
        // 用界面上当前填的地址和密钥，而不是已保存的那份：用户往往是先改地址再来刷新
        // 列表，要求"先保存才能刷新"只会逼着人保存一份还没验证过的配置。
        probe.endpointUrl = m_aiEndpointEdit->text();
        probe.apiKey = m_aiApiKeyEdit->text();
        m_aiModelRefreshButton->setEnabled(false);
        m_aiStatusLabel->setStyleSheet(QString());
        m_aiStatusLabel->setText(tr("Fetching model list…"));
        m_aiTestClient->fetchModels(probe);
    });
    connect(m_aiTestClient, &AiClient::modelsFetched, this,
            [this](const bool ok, const QStringList &models, const QString &message) {
                m_aiModelRefreshButton->setEnabled(true);
                m_aiStatusLabel->setStyleSheet(
                    ok ? QString() : QStringLiteral("color: #d84a41;"));
                m_aiStatusLabel->setText(message);
                if (!ok) {
                    return;
                }
                // 重填候选列表不能顺手改掉用户已选的模型：先记住当前文本，替换列表后原样
                // 写回去，哪怕这个名字不在新列表里也保留——网关少列一个模型不代表它不可用。
                const QString selected = m_aiModelCombo->currentText();
                m_aiModelCombo->clear();
                m_aiModelCombo->addItems(models);
                m_aiModelCombo->setCurrentText(selected);
            });
    connect(m_aiTestClient, &AiClient::testConnectionFinished, this,
            [this](const bool ok, const QString &message) {
                m_aiTestButton->setEnabled(true);
                m_aiStatusLabel->setStyleSheet(
                    ok ? QString() : QStringLiteral("color: #d84a41;"));
                m_aiStatusLabel->setText(message);
            });
    connect(m_aiSaveButton, &QPushButton::clicked, this, &SettingsDialog::persistAiSettings);

    layout->addStretch(1);
    return page;
}

void SettingsDialog::persistAiSettings()
{
    if (m_aiSettingsService == nullptr) {
        return;
    }
    AiSettings settings;
    settings.endpointUrl = m_aiEndpointEdit->text().trimmed();
    settings.apiKey = m_aiApiKeyEdit->text();
    settings.model = m_aiModelCombo->currentText().trimmed();
    settings.thinkingEnabled = m_aiThinkingToggle->isChecked();

    QString errorMessage;
    if (m_aiSettingsService->save(settings, &errorMessage)) {
        m_aiStatusLabel->setStyleSheet(QString());
        m_aiStatusLabel->setText(tr("Saved"));
    } else {
        m_aiStatusLabel->setStyleSheet(QStringLiteral("color: #d84a41;"));
        m_aiStatusLabel->setText(tr("Save failed: %1").arg(errorMessage));
    }
}

TerminalSettings SettingsDialog::editedSettings() const
{
    TerminalSettings settings;
    const QString languageCode = m_languageSelector->currentData().toString();
    if (!languageCode.isEmpty()) {
        settings.language = languageCode;
    }
    settings.themeMode = static_cast<ThemeMode>(m_themeSelector->currentData());
    switch (m_sidebarDensitySelector->currentData()) {
    case 1:
        settings.sidebarDensity = SidebarDensity::Compact;
        break;
    case 2:
        settings.sidebarDensity = SidebarDensity::Minimal;
        break;
    default:
        settings.sidebarDensity = SidebarDensity::Relaxed;
        break;
    }
    settings.sidebarDoubleClickPrompt = m_sidebarDoubleClickPromptToggle->isChecked();
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
    settings.scrollbackMaxMegabytes = m_scrollbackMaxMegabytesSpin->value();
    settings.audibleBell = m_audibleBellToggle->isChecked();
    settings.commandAutoCompletion = m_commandAutoCompletionToggle->isChecked();
    settings.multiLinePasteConfirmation = m_multiLinePasteConfirmationToggle->isChecked();
    settings.automaticReconnect = m_automaticReconnectToggle->isChecked();
    settings.vaultPasswordEnabled = m_vaultPasswordEnabledToggle->isChecked();
    settings.reconnectMaxAttempts = m_reconnectMaxAttemptsSpin->value();
    settings.reconnectBaseDelaySeconds = m_reconnectBaseDelaySpin->value();
    for (auto iterator = m_shortcutEditors.cbegin(); iterator != m_shortcutEditors.cend(); ++iterator) {
        settings.shortcuts.insert(iterator.key(), iterator.value()->keySequence());
    }
    return settings.normalized();
}

void SettingsDialog::persistChanges()
{
    if (m_populating) {
        return;
    }

    const TerminalSettings candidate = editedSettings();
    const bool passwordProtectionChanged =
        candidate.vaultPasswordEnabled != m_persistedSettings.vaultPasswordEnabled;
    if (passwordProtectionChanged) {
        if (m_credentialVault == nullptr) {
            // 兼容构造入口没有保险箱服务，不能仅保存一个无法落实的安全开关。先恢复界面再
            // 提示，保证模态消息框背后的控件已经反映真实状态。
            populateControls(m_persistedSettings);
            updatePreviewCard(m_persistedSettings);
            QMessageBox::warning(this,
                                 tr("The vault is unavailable"),
                                 tr("The current window is not connected to a credential vault service; master password protection cannot be changed."));
            return;
        }

        // 两个方向都通过专用对话框完成。启用要求输入两次新密码；关闭即使保险箱已解锁也
        // 重新要求当前密码，并在服务层原子迁移全部凭据后清除主密码配置。
        const bool transitionSucceeded = candidate.vaultPasswordEnabled
            ? VaultDialog::enablePasswordProtection(m_credentialVault, this)
            : VaultDialog::disablePasswordProtection(m_credentialVault, this);
        if (!transitionSucceeded) {
            if (!m_credentialVault->passwordProtectionEnabled()) {
                // 关闭保护窗口中的“忘记密码”会完成不可逆重置，并以 Rejected 结束原密码操作。
                // 这不是普通取消：必须把自动保存基准和服务快照同步为关闭，否则下次修改外观
                // 会把旧 true 连同整份设置重新写回数据库。
                TerminalSettings resetSettings = m_service->currentSettings();
                resetSettings.vaultPasswordEnabled = false;
                QString synchronizationError;
                if (!m_service->save(resetSettings, &synchronizationError)) {
                    m_service->applyTransient(resetSettings);
                    QMessageBox::warning(
                        this,
                        tr("The vault has been reset"),
                        tr("Credentials were cleared, but syncing the settings state failed: %1").arg(synchronizationError));
                }
                m_persistedSettings = m_service->currentSettings();
                populateControls(m_persistedSettings);
                refreshDefaultSwatchPreview(m_persistedSettings);
                updatePreviewCard(m_persistedSettings);
                return;
            }
            // 用户取消验证或迁移失败时，保险箱事务自身不会留下半迁移数据；同步恢复开关，避免
            // 界面看起来已经启用/关闭而实际安全策略没有变化。
            populateControls(m_persistedSettings);
            refreshDefaultSwatchPreview(m_persistedSettings);
            updatePreviewCard(m_persistedSettings);
            return;
        }
    }

    QString error;
    if (!m_service->save(candidate, &error)) {
        if (passwordProtectionChanged) {
            // 凭据迁移已经在独立安全事务中提交，不能为了恢复一个设置键而反向迁移且再次索要
            // 密码。迁移事务自身已持久化安全开关，因此只推进该字段；恢复同批次其他普通字段，
            // 避免“恢复默认”等批量操作让界面把未落库的值误记为成功快照。
            TerminalSettings synchronizedSettings = m_persistedSettings;
            synchronizedSettings.vaultPasswordEnabled = candidate.vaultPasswordEnabled;
            m_persistedSettings = synchronizedSettings;
            populateControls(m_persistedSettings);
            m_service->applyTransient(m_persistedSettings);
            refreshDefaultSwatchPreview(m_persistedSettings);
            updatePreviewCard(m_persistedSettings);
            QMessageBox::critical(
                this,
                tr("Settings were not saved completely"),
                tr("Master password protection was toggled, but writing other settings failed: %1\nUnsaved changes have been reverted.")
                    .arg(error));
            return;
        }

        // 普通配置没有额外副作用，失败时安全地恢复到上一份持久快照。恢复期间 m_populating
        // 会屏蔽控件信号，防止回滚动作再次发起保存。
        populateControls(m_persistedSettings);
        refreshDefaultSwatchPreview(m_persistedSettings);
        updatePreviewCard(m_persistedSettings);
        QMessageBox::warning(this, tr("Could not save settings"), error);
        return;
    }

    // 服务会在 save() 成功后规范化并广播；以服务快照作为下一次失败回滚基准，避免界面自行
    // 推断规范化结果。即使控件连续快速修改，每次成功提交都会推进这一基准。
    m_persistedSettings = m_service->currentSettings();
    refreshDefaultSwatchPreview(m_persistedSettings);
    updatePreviewCard(m_persistedSettings);
}

void SettingsDialog::populateControls(const TerminalSettings &settings)
{
    m_populating = true;
    const TerminalSettings normalized = settings.normalized();
    const int languageIndex = m_languageSelector->findData(normalized.language);
    m_languageSelector->setCurrentIndex(languageIndex >= 0 ? languageIndex : 0);
    m_themeSelector->setCurrentData(static_cast<int>(normalized.themeMode));
    m_sidebarDensitySelector->setCurrentData(static_cast<int>(normalized.sidebarDensity));
    m_sidebarDoubleClickPromptToggle->setChecked(normalized.sidebarDoubleClickPrompt);
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
        m_fontCombo->addItem(tr("%1 (not installed)").arg(desiredFamily), desiredFamily);
        fontIndex = m_fontCombo->count() - 1;
    }
    m_fontCombo->setCurrentIndex(std::max(fontIndex, 0));
    m_fontSizeSpin->setValue(normalized.fontPointSize);
    m_lineHeightSpin->setValue(normalized.lineHeightPercent);
    m_letterSpacingSpin->setValue(normalized.letterSpacing);
    m_cursorShapeSelector->setCurrentData(static_cast<int>(normalized.cursorShape));
    m_cursorBlinkToggle->setChecked(normalized.cursorBlink);
    m_scrollbackSpin->setValue(normalized.scrollbackLines);
    m_scrollbackMaxMegabytesSpin->setValue(normalized.scrollbackMaxMegabytes);
    m_audibleBellToggle->setChecked(normalized.audibleBell);
    m_commandAutoCompletionToggle->setChecked(normalized.commandAutoCompletion);
    m_multiLinePasteConfirmationToggle->setChecked(normalized.multiLinePasteConfirmation);
    m_automaticReconnectToggle->setChecked(normalized.automaticReconnect);
    m_vaultPasswordEnabledToggle->setChecked(normalized.vaultPasswordEnabled);
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
