#include "ui/SettingsWidgets.h"

#include "i18n/Retranslate.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QButtonGroup>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QStyleHints>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

/** @brief 判断当前调色板属于深色主题，用于选择卡片底色与描边的明暗方向。 */
bool isDarkPalette(const QPalette &palette)
{
    const QColor window = palette.color(QPalette::Window);
    return window.lightness() < 128;
}

/** @brief 按当前主题返回控件“未激活轨道”颜色。 */
QColor trackColor(const QPalette &palette)
{
    return isDarkPalette(palette) ? QColor(QStringLiteral("#3a4257"))
                                  : QColor(QStringLiteral("#d3dade"));
}

} // namespace

namespace ShelliumUi {

QColor accentColor()
{
    // 与 MainWindow::applyModernAppearance() 的 @ACCENT@ 保持同一取值。这里重复声明而不是
    // 反向依赖 MainWindow，是为了让设置控件族可以被不含主窗口的测试目标单独构建。
    return QColor(QStringLiteral("#2bd981"));
}

QColor warningColor(const bool dark)
{
    return dark ? QColor(QStringLiteral("#e2a94f")) : QColor(QStringLiteral("#b8791f"));
}

QColor warningTintColor(const bool dark)
{
    return dark ? QColor(QStringLiteral("#3a2f18")) : QColor(QStringLiteral("#faefdc"));
}

QString standaloneDialogStyleSheet(const bool dark)
{
    // 颜色取值逐条对应 MainWindow::applyModernAppearance() 里的同名占位符；这里是那份
    // 全局样式表面向单个对话框的最小子集，只保留卡片、输入控件、按钮与页脚这几类。
    QString styleSheet = QStringLiteral(R"QSS(
        QDialog {
            background: @WORKSPACE@;
            color: @TEXT@;
        }
        QLabel {
            color: @TEXT@;
        }
        QLineEdit {
            color: @TEXT@;
            background: @SURFACE@;
            selection-color: @TEXT@;
            selection-background-color: @SELECTION@;
            border: 1px solid @BORDER@;
            border-radius: 8px;
            min-height: 28px;
            padding: 3px 8px;
        }
        QLineEdit:focus {
            border: 1px solid @ACCENT@;
        }
        QPushButton {
            color: @TEXT@;
            background: @SURFACE@;
            border: 1px solid @BORDER@;
            border-radius: 8px;
            min-height: 28px;
            padding: 4px 11px;
        }
        QPushButton:hover {
            background: @RAISED@;
            border-color: @MUTED@;
        }
        QPushButton:pressed {
            background: @SELECTION@;
            border-color: @ACCENT@;
        }
        QPushButton[primaryAction="true"] {
            color: #0c2f24;
            background: @ACCENT@;
            border-color: @ACCENT@;
            font-weight: 700;
        }
        QPushButton[primaryAction="true"]:hover {
            background: #45e394;
            border-color: #45e394;
        }
        QPushButton[destructiveAction="true"] {
            color: #ffffff;
            background: @DANGER@;
            border-color: @DANGER@;
            font-weight: 700;
        }
        QPushButton[destructiveAction="true"]:hover {
            background: @DANGER@;
            border-color: @DANGER@;
        }
        QPushButton[destructiveAction="true"]:disabled {
            color: @MUTED@;
            background: @SIDEBAR@;
            border-color: @BORDER@;
        }
        QToolButton[advancedDisclosure="true"] {
            color: @MUTED@;
            background: transparent;
            border: 0;
            min-height: 24px;
            padding: 2px 0;
            text-align: left;
        }
        QToolButton[advancedDisclosure="true"]:hover,
        QToolButton[advancedDisclosure="true"]:checked {
            color: @TEXT@;
            background: transparent;
        }
        QFrame#SettingsCard {
            background: @SURFACE@;
            border: 1px solid @BORDER@;
            border-radius: 14px;
        }
        QLabel#SettingsCardTitle {
            color: @TEXT@;
            font-size: 14px;
            font-weight: 700;
        }
        QLabel#SettingsRowLabel {
            color: @TEXT@;
        }
        QLabel#SettingsRowHint {
            color: @MUTED@;
            font-size: 12px;
        }
        QWidget#SettingsFooter {
            background: @SIDEBAR@;
            border-top: 1px solid @BORDER@;
        }
    )QSS");
    styleSheet.replace(QStringLiteral("@WORKSPACE@"),
                       dark ? QStringLiteral("#101525") : QStringLiteral("#edf1f4"));
    styleSheet.replace(QStringLiteral("@SIDEBAR@"),
                       dark ? QStringLiteral("#191f31") : QStringLiteral("#f5f7f8"));
    styleSheet.replace(QStringLiteral("@SURFACE@"),
                       dark ? QStringLiteral("#21283b") : QStringLiteral("#ffffff"));
    styleSheet.replace(QStringLiteral("@RAISED@"),
                       dark ? QStringLiteral("#293147") : QStringLiteral("#f0f3f5"));
    styleSheet.replace(QStringLiteral("@BORDER@"),
                       dark ? QStringLiteral("#30384f") : QStringLiteral("#d9e0e4"));
    styleSheet.replace(QStringLiteral("@TEXT@"),
                       dark ? QStringLiteral("#f0f3fa") : QStringLiteral("#1b2230"));
    styleSheet.replace(QStringLiteral("@MUTED@"),
                       dark ? QStringLiteral("#929bb3") : QStringLiteral("#71808d"));
    styleSheet.replace(QStringLiteral("@SELECTION@"),
                       dark ? QStringLiteral("#24413d") : QStringLiteral("#dff6ec"));
    styleSheet.replace(QStringLiteral("@ACCENT@"), accentColor().name());
    styleSheet.replace(QStringLiteral("@DANGER@"),
                       dark ? QStringLiteral("#ef776e") : QStringLiteral("#d84a41"));
    // 保险库会在 MainWindow 创建前使用本样式。追加专用片段而不是把选择器散落到基础规则中，
    // 可确保应用运行期间由全局样式复用时，得到与首次启动完全一致的安全操作视觉层级。
    return styleSheet + vaultDialogStyleSheet(dark) + hostChainDialogStyleSheet(dark);
}

QString vaultDialogStyleSheet(const bool dark)
{
    // 颜色逐项对应设计文档中的 accent-tint、danger、warn 与 warn-tint。警示边框使用不透明
    // 的预混合色，是因为 Qt QSS 对 rgba 边框在不同平台样式引擎上的抗锯齿结果并不一致。
    const QString workspace = dark ? QStringLiteral("#101525") : QStringLiteral("#edf1f4");
    const QString text = dark ? QStringLiteral("#f0f3fa") : QStringLiteral("#1b2230");
    const QString muted = dark ? QStringLiteral("#929bb3") : QStringLiteral("#71808d");
    const QString accentTint = dark ? QStringLiteral("#173729") : QStringLiteral("#e2f6ec");
    const QString danger = dark ? QStringLiteral("#ef776e") : QStringLiteral("#d84a41");
    const QString warning = dark ? QStringLiteral("#e2a94f") : QStringLiteral("#b8791f");
    const QString warningTint = dark ? QStringLiteral("#3a2f18") : QStringLiteral("#faefdc");
    const QString warningBorder = dark ? QStringLiteral("#765827") : QStringLiteral("#e1c08e");

    QString styleSheet = QStringLiteral(R"QSS(
        QDialog#VaultDialog,
        QWidget#VaultBody {
            background: @WORKSPACE@;
            color: @TEXT@;
        }
        QLabel#VaultSecurityBadge {
            background: @ACCENT_TINT@;
            border: 0;
            border-radius: 12px;
        }
        QLabel#VaultHeading {
            color: @TEXT@;
            font-size: 15px;
            font-weight: 700;
        }
        QLabel#VaultIntroduction {
            color: @MUTED@;
            font-size: 12px;
        }
        QLabel#VaultFieldLabel {
            color: @TEXT@;
            font-size: 13px;
            font-weight: 600;
        }
        QLineEdit[validationError="true"],
        QLineEdit[validationError="true"]:focus {
            border: 1px solid @DANGER@;
        }
        QLabel#VaultErrorLabel {
            color: @DANGER@;
            font-size: 12px;
        }
        QFrame#VaultWarningNotice {
            color: @TEXT@;
            background: @WARNING_TINT@;
            border: 1px solid @WARNING_BORDER@;
            border-radius: 8px;
        }
        QLabel#VaultWarningIcon {
            color: @WARNING@;
            background: transparent;
            border: 0;
        }
        QLabel#VaultWarningText {
            color: @TEXT@;
            background: transparent;
            border: 0;
            font-size: 12px;
        }
    )QSS");
    styleSheet.replace(QStringLiteral("@WORKSPACE@"), workspace);
    styleSheet.replace(QStringLiteral("@TEXT@"), text);
    styleSheet.replace(QStringLiteral("@MUTED@"), muted);
    styleSheet.replace(QStringLiteral("@ACCENT_TINT@"), accentTint);
    styleSheet.replace(QStringLiteral("@DANGER@"), danger);
    styleSheet.replace(QStringLiteral("@WARNING@"), warning);
    styleSheet.replace(QStringLiteral("@WARNING_TINT@"), warningTint);
    styleSheet.replace(QStringLiteral("@WARNING_BORDER@"), warningBorder);
    return styleSheet;
}

QString hostChainDialogStyleSheet(const bool dark)
{
    // 颜色与 MainWindow::applyModernAppearance() 的同名主题变量严格一致。单独生成具体值
    // 而不是在局部样式中依赖未替换占位符，确保独立测试和主窗口全局样式都能直接复用。
    const QString workspace = dark ? QStringLiteral("#101525") : QStringLiteral("#edf1f4");
    const QString sidebar = dark ? QStringLiteral("#191f31") : QStringLiteral("#f5f7f8");
    const QString surface = dark ? QStringLiteral("#21283b") : QStringLiteral("#ffffff");
    const QString raised = dark ? QStringLiteral("#293147") : QStringLiteral("#f0f3f5");
    const QString border = dark ? QStringLiteral("#30384f") : QStringLiteral("#d9e0e4");
    const QString text = dark ? QStringLiteral("#f0f3fa") : QStringLiteral("#1b2230");
    const QString muted = dark ? QStringLiteral("#929bb3") : QStringLiteral("#71808d");
    const QString selection = dark ? QStringLiteral("#24413d") : QStringLiteral("#dff6ec");
    const QString danger = dark ? QStringLiteral("#ef776e") : QStringLiteral("#d84a41");

    QString styleSheet = QStringLiteral(R"QSS(
        QDialog#HostChainDialog,
        QDialog#HostPickerDialog,
        QWidget#HostChainBody,
        QWidget#HostPickerBody {
            color: @TEXT@;
            background: @WORKSPACE@;
        }
        QLabel#HostChainIntroduction,
        QLabel#HostPickerSubtitle {
            color: @MUTED@;
            font-size: 12px;
        }
        QLineEdit#hostChainSearchEdit {
            color: @TEXT@;
            background: @SURFACE@;
            border: 1px solid @BORDER@;
            border-radius: 9px;
            min-height: 30px;
            padding: 4px 10px;
        }
        QLineEdit#hostChainSearchEdit:focus {
            border-color: @ACCENT@;
        }
        QListWidget#hostChainList,
        QListWidget#hostChainCandidateList {
            color: @TEXT@;
            background: @SURFACE@;
            border: 1px solid @BORDER@;
            border-radius: 11px;
            outline: 0;
            padding: 0;
        }
        QLabel#hostChainRouteArrow {
            color: @MUTED@;
            background: transparent;
        }
        QFrame#hostChainTargetCard {
            color: @TEXT@;
            background: @SELECTION@;
            border: 1px dashed @ACCENT@;
            border-radius: 11px;
        }
        QLabel#hostChainTargetLabel {
            color: @ACCENT_DARK@;
            background: transparent;
            font-size: 11px;
            font-weight: 700;
        }
        QLabel#hostChainTargetDetail {
            color: @MUTED@;
            background: transparent;
            font-size: 12px;
        }
        QWidget#HostChainFooter,
        QWidget#HostPickerFooter {
            color: @TEXT@;
            background: @SIDEBAR@;
            border-top: 1px solid @BORDER@;
        }
        QPushButton[hostChainDanger="true"] {
            color: @DANGER@;
            background: transparent;
            border: 0;
            font-weight: 600;
            padding-left: 0;
            padding-right: 0;
        }
        QPushButton[hostChainDanger="true"]:hover {
            color: @DANGER@;
            background: @RAISED@;
        }
        QPushButton[hostChainDanger="true"]:disabled {
            color: @MUTED@;
            background: transparent;
        }
    )QSS");
    styleSheet.replace(QStringLiteral("@WORKSPACE@"), workspace);
    styleSheet.replace(QStringLiteral("@SIDEBAR@"), sidebar);
    styleSheet.replace(QStringLiteral("@SURFACE@"), surface);
    styleSheet.replace(QStringLiteral("@RAISED@"), raised);
    styleSheet.replace(QStringLiteral("@BORDER@"), border);
    styleSheet.replace(QStringLiteral("@TEXT@"), text);
    styleSheet.replace(QStringLiteral("@MUTED@"), muted);
    styleSheet.replace(QStringLiteral("@SELECTION@"), selection);
    styleSheet.replace(QStringLiteral("@ACCENT_DARK@"), ShelliumUi::accentColor().darker(140).name());
    styleSheet.replace(QStringLiteral("@ACCENT@"), ShelliumUi::accentColor().name());
    styleSheet.replace(QStringLiteral("@DANGER@"), danger);
    return styleSheet;
}

void applyStandaloneDialogStyle(QWidget *dialog)
{
    Q_ASSERT(dialog != nullptr);
    if (qApp == nullptr || !qApp->styleSheet().isEmpty()) {
        return;
    }

    // 全局样式表为空说明 MainWindow 还没建立视觉系统，此时只能按系统外观选明暗；Qt 未
    // 声明配色方案的平台再退回调色板亮度，与 TerminalSettings::effectiveThemeMode() 的
    // "跟随系统"判定同源。
    Qt::ColorScheme scheme = Qt::ColorScheme::Unknown;
    if (QGuiApplication::instance() != nullptr) {
        scheme = QGuiApplication::styleHints()->colorScheme();
    }
    const bool dark = scheme == Qt::ColorScheme::Dark
        || (scheme == Qt::ColorScheme::Unknown && isDarkPalette(QGuiApplication::palette()));
    dialog->setStyleSheet(standaloneDialogStyleSheet(dark));
}

} // namespace ShelliumUi

ToggleSwitch::ToggleSwitch(QWidget *parent)
    : QAbstractButton(parent)
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
}

QSize ToggleSwitch::sizeHint() const
{
    return {44, 24};
}

void ToggleSwitch::setKnobPosition(const qreal position)
{
    m_knobPosition = qBound(0.0, position, 1.0);
    update();
}

void ToggleSwitch::checkStateSet()
{
    QAbstractButton::checkStateSet();
    animateToCurrentState();
}

void ToggleSwitch::nextCheckState()
{
    QAbstractButton::nextCheckState();
    animateToCurrentState();
}

void ToggleSwitch::animateToCurrentState()
{
    const qreal target = isChecked() ? 1.0 : 0.0;
    if (!isVisible()) {
        // 控件尚未显示时（例如 populateControls 首次填充）直接落位，避免打开设置页时看到
        // 一排开关同时从关滑到开的无意义动画。
        setKnobPosition(target);
        return;
    }
    auto *animation = new QPropertyAnimation(this, "knobPosition", this);
    animation->setDuration(130);
    animation->setStartValue(m_knobPosition);
    animation->setEndValue(target);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void ToggleSwitch::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF track(0.5, (height() - 22) / 2.0 + 0.5, 42.0, 21.0);
    const qreal radius = track.height() / 2.0;
    QColor active = ShelliumUi::accentColor();
    if (!isEnabled()) {
        active.setAlpha(110);
    }
    QColor inactive = trackColor(palette());
    if (!isEnabled()) {
        inactive.setAlpha(110);
    }

    // 轨道颜色在开与关之间按滑块位置插值，滑动过程中的颜色变化与位移完全同步。
    const QColor blended(
        static_cast<int>(inactive.red() + (active.red() - inactive.red()) * m_knobPosition),
        static_cast<int>(inactive.green() + (active.green() - inactive.green()) * m_knobPosition),
        static_cast<int>(inactive.blue() + (active.blue() - inactive.blue()) * m_knobPosition));
    painter.setPen(Qt::NoPen);
    painter.setBrush(blended);
    painter.drawRoundedRect(track, radius, radius);

    const qreal knobDiameter = track.height() - 4.0;
    const qreal knobLeft = track.left() + 2.0 + (track.width() - 4.0 - knobDiameter) * m_knobPosition;
    const QRectF knob(knobLeft, track.top() + 2.0, knobDiameter, knobDiameter);
    painter.setBrush(QColor(QStringLiteral("#ffffff")));
    painter.drawEllipse(knob);

    if (hasFocus()) {
        QColor focusRing = ShelliumUi::accentColor();
        focusRing.setAlpha(150);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(focusRing, 2.0));
        painter.drawRoundedRect(track.adjusted(-1.5, -1.5, 1.5, 1.5), radius + 1.5, radius + 1.5);
    }
}

SettingsStepper::SettingsStepper(QAbstractSpinBox *spinBox, QWidget *parent)
    : QWidget(parent)
    , m_spinBox(spinBox)
{
    Q_ASSERT(m_spinBox != nullptr);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    const auto makeButton = [this](const QString &glyph, const QString &role) {
        auto *button = new QToolButton(this);
        button->setText(glyph);
        button->setProperty("stepperButton", true);
        button->setProperty("stepperRole", role);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setAutoRepeat(true);
        button->setFixedSize(30, 30);
        return button;
    };

    auto *decrease = makeButton(QStringLiteral("−"), QStringLiteral("decrease"));
    auto *increase = makeButton(QStringLiteral("+"), QStringLiteral("increase"));

    // 隐藏原生上下箭头：一个数值只保留一套增减入口，否则窄行里会出现两组尺寸不同的箭头。
    m_spinBox->setParent(this);
    m_spinBox->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_spinBox->setAlignment(Qt::AlignCenter);
    m_spinBox->setProperty("stepperValue", true);
    m_spinBox->setMinimumWidth(76);

    layout->addWidget(decrease);
    layout->addWidget(m_spinBox);
    layout->addWidget(increase);

    connect(decrease, &QToolButton::clicked, m_spinBox, &QAbstractSpinBox::stepDown);
    connect(increase, &QToolButton::clicked, m_spinBox, &QAbstractSpinBox::stepUp);
    connect(m_spinBox, &QAbstractSpinBox::editingFinished, this, [this]() {
        m_spinBox->interpretText();
    });
}

SegmentedControl::SegmentedControl(QWidget *parent)
    : QWidget(parent)
    , m_group(new QButtonGroup(this))
{
    m_group->setExclusive(true);
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(4);

    connect(m_group, &QButtonGroup::idToggled, this, [this](const int id, const bool checked) {
        if (checked) {
            emit currentDataChanged(id);
        }
    });
}

void SegmentedControl::addSegment(const QString &text, const int data)
{
    auto *button = new QPushButton(text, this);
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setProperty("segmentButton", true);
    button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    if (m_group->buttons().isEmpty()) {
        button->setChecked(true);
    }
    m_layout->addWidget(button);
    m_group->addButton(button, data);
}

void SegmentedControl::addSegment(std::function<QString()> text, const int data)
{
    addSegment(text ? text() : QString(), data);
    if (QAbstractButton *button = m_group->button(data); button != nullptr && text) {
        i18n::onRetranslate(button,
                            [button, fn = std::move(text)] { button->setText(fn()); });
    }
}

void SegmentedControl::setSegmentText(const int data, const QString &text)
{
    if (QAbstractButton *button = m_group->button(data)) {
        button->setText(text);
    }
}

int SegmentedControl::currentData() const
{
    return m_group->checkedId();
}

void SegmentedControl::setCurrentData(const int data)
{
    QAbstractButton *button = m_group->button(data);
    if (button == nullptr || button->isChecked()) {
        return;
    }
    button->setChecked(true);
}

ThemeSwatchButton::ThemeSwatchButton(const QString &title,
                                     const QString &subtitle,
                                     const QColor &background,
                                     const QColor &foreground,
                                     const QColor &cursor,
                                     const QColor &link,
                                     QWidget *parent)
    : QAbstractButton(parent)
    , m_subtitle(subtitle)
    , m_background(background)
    , m_foreground(foreground)
    , m_cursor(cursor)
    , m_link(link)
{
    setText(title);
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize ThemeSwatchButton::sizeHint() const
{
    return {210, 62};
}

void ThemeSwatchButton::setPreviewColors(const QColor &background,
                                         const QColor &foreground,
                                         const QColor &cursor,
                                         const QColor &link)
{
    m_background = background;
    m_foreground = foreground;
    m_cursor = cursor;
    m_link = link;
    update();
}

void ThemeSwatchButton::setSubtitle(const QString &subtitle)
{
    if (m_subtitle == subtitle) {
        return;
    }
    m_subtitle = subtitle;
    update();
}

void ThemeSwatchButton::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const bool dark = isDarkPalette(palette());
    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QColor cardBackground = isChecked()
        ? (dark ? QColor(QStringLiteral("#2a3247")) : QColor(QStringLiteral("#e9eef1")))
        : palette().color(QPalette::Base);
    if (underMouse() && !isChecked()) {
        cardBackground = dark ? QColor(QStringLiteral("#242c40")) : QColor(QStringLiteral("#f2f5f7"));
    }
    painter.setPen(isChecked() ? QPen(ShelliumUi::accentColor(), 1.6)
                               : QPen(trackColor(palette()), 1.0));
    painter.setBrush(cardBackground);
    painter.drawRoundedRect(card, 10, 10);

    // 缩略终端：底色铺满，再画三条不同色的“文本行”，用户可直接判断对比度是否舒适。
    const QRectF preview(card.left() + 10.0, card.top() + 10.0, 46.0, card.height() - 20.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_background);
    painter.drawRoundedRect(preview, 6, 6);

    const QList<QColor> barColors{m_foreground, m_link, m_cursor};
    const QList<qreal> barWidths{0.74, 0.52, 0.62};
    const qreal barHeight = 3.0;
    const qreal barSpacing = (preview.height() - barColors.size() * barHeight) / (barColors.size() + 1);
    for (int index = 0; index < barColors.size(); ++index) {
        QRectF bar(preview.left() + 7.0,
                   preview.top() + barSpacing * (index + 1) + barHeight * index,
                   (preview.width() - 14.0) * barWidths.at(index),
                   barHeight);
        painter.setBrush(barColors.at(index));
        painter.drawRoundedRect(bar, 1.5, 1.5);
    }

    const QRectF textArea(preview.right() + 12.0,
                          card.top(),
                          card.width() - preview.width() - 32.0,
                          card.height());
    QFont titleFont = font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 0.5);
    titleFont.setWeight(isChecked() ? QFont::DemiBold : QFont::Normal);
    painter.setFont(titleFont);
    painter.setPen(isChecked() ? ShelliumUi::accentColor() : palette().color(QPalette::WindowText));
    const bool hasSubtitle = !m_subtitle.isEmpty();
    const QRectF titleRect = hasSubtitle
        ? QRectF(textArea.left(), textArea.top() + 12.0, textArea.width(), 20.0)
        : textArea;
    painter.drawText(titleRect,
                     static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter),
                     fontMetrics().elidedText(text(), Qt::ElideRight,
                                              static_cast<int>(textArea.width())));

    if (hasSubtitle) {
        QFont subtitleFont = font();
        subtitleFont.setPointSizeF(qMax(7.0, subtitleFont.pointSizeF() - 1.5));
        painter.setFont(subtitleFont);
        painter.setPen(palette().color(QPalette::Disabled, QPalette::WindowText));
        painter.drawText(QRectF(textArea.left(), titleRect.bottom(), textArea.width(), 18.0),
                         static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter),
                         m_subtitle);
    }
}

SettingsCard::SettingsCard(const QString &title, QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("SettingsCard"));
    setFrameShape(QFrame::NoFrame);
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(18, 16, 18, 16);
    m_layout->setSpacing(12);

    if (!title.isEmpty()) {
        m_titleLabel = new QLabel(title, this);
        m_titleLabel->setObjectName(QStringLiteral("SettingsCardTitle"));
        m_layout->addWidget(m_titleLabel);
    }
}

SettingsCard::SettingsCard(std::function<QString()> titleProvider, QWidget *parent)
    : SettingsCard(titleProvider ? titleProvider() : QString(), parent)
{
    setTitleProvider(std::move(titleProvider));
}

QWidget *SettingsCard::addRow(const QString &label, QWidget *field, const QString &hint,
                              QLabel **outLabel, QLabel **outHint)
{
    Q_ASSERT(field != nullptr);
    auto *row = new QWidget(this);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(16);

    auto *textColumn = new QWidget(row);
    auto *textLayout = new QVBoxLayout(textColumn);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(2);
    auto *labelWidget = new QLabel(label, textColumn);
    labelWidget->setObjectName(QStringLiteral("SettingsRowLabel"));
    textLayout->addWidget(labelWidget);
    if (outLabel != nullptr) {
        *outLabel = labelWidget;
    }
    if (!hint.isEmpty()) {
        auto *hintWidget = new QLabel(hint, textColumn);
        hintWidget->setObjectName(QStringLiteral("SettingsRowHint"));
        hintWidget->setWordWrap(true);
        textLayout->addWidget(hintWidget);
        if (outHint != nullptr) {
            *outHint = hintWidget;
        }
    }

    field->setParent(row);
    rowLayout->addWidget(textColumn, 1);
    rowLayout->addWidget(field, 0, Qt::AlignRight | Qt::AlignVCenter);
    m_layout->addWidget(row);
    return field;
}

QWidget *SettingsCard::addWideRow(const QString &label, QWidget *field, const QString &hint)
{
    Q_ASSERT(field != nullptr);
    auto *row = new QWidget(this);
    auto *rowLayout = new QVBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(6);

    auto *labelWidget = new QLabel(label, row);
    labelWidget->setObjectName(QStringLiteral("SettingsRowLabel"));
    rowLayout->addWidget(labelWidget);
    if (!hint.isEmpty()) {
        auto *hintWidget = new QLabel(hint, row);
        hintWidget->setObjectName(QStringLiteral("SettingsRowHint"));
        hintWidget->setWordWrap(true);
        rowLayout->addWidget(hintWidget);
    }

    field->setParent(row);
    // 不设置固定宽度，而是交给垂直布局填满整行：这样设置窗口放大时能展示更多长内容，
    // 缩小时又会随卡片收窄，不会因为某个像素常量导致水平滚动或挤压页面。
    field->setSizePolicy(QSizePolicy::Expanding, field->sizePolicy().verticalPolicy());
    rowLayout->addWidget(field);
    m_layout->addWidget(row);
    return field;
}

void SettingsCard::addFullWidthWidget(QWidget *widget)
{
    Q_ASSERT(widget != nullptr);
    widget->setParent(this);
    m_layout->addWidget(widget);
}

QLabel *SettingsCard::addNote(const QString &text)
{
    auto *note = new QLabel(text, this);
    note->setObjectName(QStringLiteral("SettingsRowHint"));
    note->setWordWrap(true);
    m_layout->addWidget(note);
    return note;
}

QWidget *SettingsCard::addRow(std::function<QString()> label, QWidget *field,
                              std::function<QString()> hint)
{
    QLabel *labelWidget = nullptr;
    QLabel *hintWidget = nullptr;
    QWidget *row = addRow(label ? label() : QString(), field,
                          hint ? hint() : QString(), &labelWidget, &hintWidget);
    if (labelWidget != nullptr && label) {
        i18n::onRetranslate(labelWidget,
                            [labelWidget, fn = std::move(label)] { labelWidget->setText(fn()); });
    }
    if (hintWidget != nullptr && hint) {
        i18n::onRetranslate(hintWidget,
                            [hintWidget, fn = std::move(hint)] { hintWidget->setText(fn()); });
    }
    return row;
}

QWidget *SettingsCard::addWideRow(std::function<QString()> label, QWidget *field,
                                  std::function<QString()> hint)
{
    QWidget *row = addWideRow(label ? label() : QString(), field, hint ? hint() : QString());
    // addWideRow 的标签/说明用固定 objectName；取回本行内新建的那两个即可挂重译。
    const QList<QLabel *> labels = row->findChildren<QLabel *>();
    for (QLabel *candidate : labels) {
        if (label && candidate->objectName() == QLatin1String("SettingsRowLabel")) {
            i18n::onRetranslate(candidate,
                                [candidate, fn = label] { candidate->setText(fn()); });
        } else if (hint && candidate->objectName() == QLatin1String("SettingsRowHint")) {
            i18n::onRetranslate(candidate,
                                [candidate, fn = hint] { candidate->setText(fn()); });
        }
    }
    return row;
}

QLabel *SettingsCard::addNote(std::function<QString()> text)
{
    QLabel *note = addNote(text ? text() : QString());
    if (text) {
        i18n::onRetranslate(note, [note, fn = std::move(text)] { note->setText(fn()); });
    }
    return note;
}

void SettingsCard::setTitleProvider(std::function<QString()> title)
{
    if (m_titleLabel == nullptr || !title) {
        return;
    }
    QLabel *label = m_titleLabel;
    i18n::onRetranslate(label, [label, fn = std::move(title)] { label->setText(fn()); });
}
