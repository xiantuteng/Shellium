#include "ui/SettingsDialog.h"

#include "settings/TerminalSettings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

SettingsDialog::SettingsDialog(TerminalSettingsService *service, QWidget *parent)
    : QDialog(parent)
    , m_service(service)
    , m_originalSettings(service == nullptr ? TerminalSettings{}.normalized()
                                            : service->currentSettings())
    , m_themeCombo(new QComboBox(this))
    , m_colorSchemeCombo(new QComboBox(this))
    , m_fontCombo(new QFontComboBox(this))
    , m_fontSizeSpin(new QSpinBox(this))
    , m_lineHeightSpin(new QSpinBox(this))
    , m_letterSpacingSpin(new QDoubleSpinBox(this))
    , m_cursorShapeCombo(new QComboBox(this))
    , m_cursorBlinkCheck(new QCheckBox(tr("闪烁光标"), this))
    , m_scrollbackSpin(new QSpinBox(this))
    , m_audibleBellCheck(new QCheckBox(tr("收到 BEL 时播放系统提示音"), this))
    , m_automaticReconnectCheck(
          new QCheckBox(tr("已建立 SSH 会话因网络异常断开时自动重连"), this))
    , m_reconnectMaxAttemptsSpin(new QSpinBox(this))
    , m_reconnectBaseDelaySpin(new QSpinBox(this))
    , m_previewLabel(new QLabel(this))
{
    Q_ASSERT(m_service != nullptr);
    setWindowTitle(tr("Shellium 设置"));
    resize(660, 640);

    auto *rootLayout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    rootLayout->addWidget(tabs, 1);

    auto *appearancePage = new QWidget(tabs);
    auto *appearanceLayout = new QVBoxLayout(appearancePage);
    auto *appearanceForm = new QFormLayout;

    m_themeCombo->setObjectName(QStringLiteral("themeCombo"));
    m_themeCombo->addItem(tr("跟随系统"), static_cast<int>(ThemeMode::FollowSystem));
    m_themeCombo->addItem(tr("浅色"), static_cast<int>(ThemeMode::Light));
    m_themeCombo->addItem(tr("深色"), static_cast<int>(ThemeMode::Dark));

    m_colorSchemeCombo->setObjectName(QStringLiteral("colorSchemeCombo"));
    m_colorSchemeCombo->addItem(tr("默认（跟随主题）"),
                                static_cast<int>(TerminalColorScheme::Default));
    m_colorSchemeCombo->addItem(QStringLiteral("Solarized Dark"),
                                static_cast<int>(TerminalColorScheme::SolarizedDark));
    m_colorSchemeCombo->addItem(QStringLiteral("Dracula"),
                                static_cast<int>(TerminalColorScheme::Dracula));
    m_colorSchemeCombo->addItem(QStringLiteral("Monokai"),
                                static_cast<int>(TerminalColorScheme::Monokai));
    m_colorSchemeCombo->addItem(QStringLiteral("Nord"),
                                static_cast<int>(TerminalColorScheme::Nord));

    m_fontCombo->setObjectName(QStringLiteral("terminalFontCombo"));
    m_fontCombo->setFontFilters(QFontComboBox::MonospacedFonts);
    m_fontSizeSpin->setObjectName(QStringLiteral("terminalFontSizeSpin"));
    m_fontSizeSpin->setRange(6, 72);
    m_fontSizeSpin->setSuffix(tr(" pt"));
    m_lineHeightSpin->setRange(80, 250);
    m_lineHeightSpin->setSuffix(QStringLiteral("%"));
    m_letterSpacingSpin->setRange(-2.0, 10.0);
    m_letterSpacingSpin->setDecimals(1);
    m_letterSpacingSpin->setSingleStep(0.5);
    m_letterSpacingSpin->setSuffix(tr(" px"));

    m_cursorShapeCombo->addItem(tr("块状"), static_cast<int>(TerminalCursorShape::Block));
    m_cursorShapeCombo->addItem(tr("下划线"),
                                static_cast<int>(TerminalCursorShape::Underline));
    m_cursorShapeCombo->addItem(tr("竖线"), static_cast<int>(TerminalCursorShape::IBeam));

    m_scrollbackSpin->setRange(100, 1000000);
    m_scrollbackSpin->setSingleStep(1000);
    m_scrollbackSpin->setSuffix(tr(" 行"));
    m_scrollbackSpin->setToolTip(
        tr("减小该值会立即丢弃超过上限的最旧历史；已经丢弃的内容无法恢复。"));

    appearanceForm->addRow(tr("主题："), m_themeCombo);
    appearanceForm->addRow(tr("终端配色："), m_colorSchemeCombo);
    appearanceForm->addRow(tr("等宽字体："), m_fontCombo);
    appearanceForm->addRow(tr("字号："), m_fontSizeSpin);
    appearanceForm->addRow(tr("行高："), m_lineHeightSpin);
    appearanceForm->addRow(tr("字符间距："), m_letterSpacingSpin);
    appearanceForm->addRow(tr("光标形态："), m_cursorShapeCombo);
    appearanceForm->addRow(QString(), m_cursorBlinkCheck);
    appearanceForm->addRow(tr("滚动缓冲："), m_scrollbackSpin);
    appearanceForm->addRow(QString(), m_audibleBellCheck);
    appearanceLayout->addLayout(appearanceForm);

    auto *previewGroup = new QGroupBox(tr("实时预览"), appearancePage);
    auto *previewLayout = new QVBoxLayout(previewGroup);
    m_previewLabel->setObjectName(QStringLiteral("terminalSettingsPreview"));
    m_previewLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_previewLabel->setMinimumHeight(130);
    m_previewLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_previewLabel->setText(QStringLiteral(
        "deploy@server:~$ ssh app@10.0.0.8\n"
        "Shellium · UTF-8 · 256 色 / TrueColor\n"
        "https://shellium.example/docs"));
    previewLayout->addWidget(m_previewLabel);
    appearanceLayout->addWidget(previewGroup);
    appearanceLayout->addStretch(1);
    tabs->addTab(appearancePage, tr("外观与终端"));

    auto *connectionPage = new QWidget(tabs);
    auto *connectionLayout = new QVBoxLayout(connectionPage);
    auto *connectionForm = new QFormLayout;
    m_reconnectMaxAttemptsSpin->setRange(0, 10);
    m_reconnectMaxAttemptsSpin->setSuffix(tr(" 次"));
    m_reconnectMaxAttemptsSpin->setToolTip(
        tr("仅重试已经成功建立过、且未收到远端退出状态的异常断线；0 表示不重试。"));
    m_reconnectBaseDelaySpin->setRange(1, 60);
    m_reconnectBaseDelaySpin->setSuffix(tr(" 秒"));
    m_reconnectBaseDelaySpin->setToolTip(
        tr("后续尝试使用 2 倍指数退避，最长等待 60 秒，避免网络故障时高频重连。"));
    connectionForm->addRow(QString(), m_automaticReconnectCheck);
    connectionForm->addRow(tr("最多重试："), m_reconnectMaxAttemptsSpin);
    connectionForm->addRow(tr("首轮等待："), m_reconnectBaseDelaySpin);
    connectionLayout->addLayout(connectionForm);
    auto *reconnectNotice = new QLabel(
        tr("正常执行 exit、主动切换连接、首次认证失败或主机密钥拒绝均不会自动重连。"),
        connectionPage);
    reconnectNotice->setWordWrap(true);
    connectionLayout->addWidget(reconnectNotice);
    connectionLayout->addStretch(1);
    tabs->addTab(connectionPage, tr("连接恢复"));

    // 快捷键数量可能继续增加，放进滚动区避免小屏幕上确定/取消按钮被挤出窗口。
    auto *shortcutPage = new QWidget(tabs);
    auto *shortcutPageLayout = new QVBoxLayout(shortcutPage);
    auto *shortcutScroll = new QScrollArea(shortcutPage);
    shortcutScroll->setWidgetResizable(true);
    auto *shortcutContent = new QWidget(shortcutScroll);
    auto *shortcutForm = new QFormLayout(shortcutContent);
    for (const QString &id : TerminalSettings::shortcutIds()) {
        auto *editor = new QKeySequenceEdit(shortcutContent);
        editor->setObjectName(QStringLiteral("shortcut_%1").arg(id));
        editor->setClearButtonEnabled(true);
        editor->setMaximumSequenceLength(1);
        editor->setToolTip(tr("留空表示取消该动作的快捷键；同一快捷键不能分配给两个动作。"));
        shortcutForm->addRow(TerminalSettings::shortcutDisplayName(id) + tr("："), editor);
        m_shortcutEditors.insert(id, editor);
    }
    shortcutScroll->setWidget(shortcutContent);
    shortcutPageLayout->addWidget(shortcutScroll);
    tabs->addTab(shortcutPage, tr("快捷键"));

    auto *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults,
        this);
    buttonBox->button(QDialogButtonBox::Ok)->setText(tr("保存"));
    buttonBox->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    buttonBox->button(QDialogButtonBox::RestoreDefaults)->setText(tr("恢复默认"));
    rootLayout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    connect(buttonBox->button(QDialogButtonBox::RestoreDefaults),
            &QPushButton::clicked,
            this,
            [this]() {
                populateControls(TerminalSettings{}.normalized());
                previewChanges();
            });

    const auto connectPreview = [this](QObject *control, const char *signal) {
        // 旧式字符串连接仅用于把不同控件签名统一路由到无参数预览方法；所有信号均是 Qt
        // 公共 API，并在连接失败时由调试构建输出明确告警。
        connect(control, signal, this, SLOT(previewChanges()));
    };
    connectPreview(m_themeCombo, SIGNAL(currentIndexChanged(int)));
    connectPreview(m_colorSchemeCombo, SIGNAL(currentIndexChanged(int)));
    connectPreview(m_fontCombo, SIGNAL(currentFontChanged(QFont)));
    connectPreview(m_fontSizeSpin, SIGNAL(valueChanged(int)));
    connectPreview(m_lineHeightSpin, SIGNAL(valueChanged(int)));
    connectPreview(m_letterSpacingSpin, SIGNAL(valueChanged(double)));
    connectPreview(m_cursorShapeCombo, SIGNAL(currentIndexChanged(int)));
    connectPreview(m_cursorBlinkCheck, SIGNAL(toggled(bool)));
    connectPreview(m_scrollbackSpin, SIGNAL(valueChanged(int)));
    connectPreview(m_audibleBellCheck, SIGNAL(toggled(bool)));
    connectPreview(m_automaticReconnectCheck, SIGNAL(toggled(bool)));
    connectPreview(m_reconnectMaxAttemptsSpin, SIGNAL(valueChanged(int)));
    connectPreview(m_reconnectBaseDelaySpin, SIGNAL(valueChanged(int)));
    connect(m_automaticReconnectCheck, &QCheckBox::toggled, this, [this](const bool enabled) {
        m_reconnectMaxAttemptsSpin->setEnabled(enabled);
        m_reconnectBaseDelaySpin->setEnabled(enabled);
    });
    for (QKeySequenceEdit *editor : std::as_const(m_shortcutEditors)) {
        connect(editor, &QKeySequenceEdit::keySequenceChanged,
                this, [this](const QKeySequence &) { previewChanges(); });
    }

    populateControls(m_originalSettings);
    updatePreviewCard(m_originalSettings);
}

TerminalSettings SettingsDialog::editedSettings() const
{
    TerminalSettings settings;
    settings.themeMode = static_cast<ThemeMode>(m_themeCombo->currentData().toInt());
    settings.colorScheme =
        static_cast<TerminalColorScheme>(m_colorSchemeCombo->currentData().toInt());
    settings.fontFamily = m_fontCombo->currentFont().family();
    settings.fontPointSize = m_fontSizeSpin->value();
    settings.lineHeightPercent = m_lineHeightSpin->value();
    settings.letterSpacing = m_letterSpacingSpin->value();
    settings.cursorShape = static_cast<TerminalCursorShape>(
        m_cursorShapeCombo->currentData().toInt());
    settings.cursorBlink = m_cursorBlinkCheck->isChecked();
    settings.scrollbackLines = m_scrollbackSpin->value();
    settings.audibleBell = m_audibleBellCheck->isChecked();
    settings.automaticReconnect = m_automaticReconnectCheck->isChecked();
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
    m_themeCombo->setCurrentIndex(m_themeCombo->findData(static_cast<int>(normalized.themeMode)));
    m_colorSchemeCombo->setCurrentIndex(
        m_colorSchemeCombo->findData(static_cast<int>(normalized.colorScheme)));

    const QFont font = normalized.fontFamily.isEmpty()
        ? QFontDatabase::systemFont(QFontDatabase::FixedFont)
        : QFont(normalized.fontFamily);
    m_fontCombo->setCurrentFont(font);
    m_fontSizeSpin->setValue(normalized.fontPointSize);
    m_lineHeightSpin->setValue(normalized.lineHeightPercent);
    m_letterSpacingSpin->setValue(normalized.letterSpacing);
    m_cursorShapeCombo->setCurrentIndex(
        m_cursorShapeCombo->findData(static_cast<int>(normalized.cursorShape)));
    m_cursorBlinkCheck->setChecked(normalized.cursorBlink);
    m_scrollbackSpin->setValue(normalized.scrollbackLines);
    m_audibleBellCheck->setChecked(normalized.audibleBell);
    m_automaticReconnectCheck->setChecked(normalized.automaticReconnect);
    m_reconnectMaxAttemptsSpin->setValue(normalized.reconnectMaxAttempts);
    m_reconnectBaseDelaySpin->setValue(normalized.reconnectBaseDelaySeconds);
    m_reconnectMaxAttemptsSpin->setEnabled(normalized.automaticReconnect);
    m_reconnectBaseDelaySpin->setEnabled(normalized.automaticReconnect);
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
    updatePreviewCard(settings);
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
        QStringLiteral("QLabel { background: %1; color: %2; padding: 12px; "
                       "line-height: %3%; border: 1px solid %4; }")
            .arg(palette.background.name(),
                 palette.foreground.name(),
                 QString::number(normalized.lineHeightPercent),
                 palette.selection.name()));
}
