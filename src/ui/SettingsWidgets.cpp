#include "ui/SettingsWidgets.h"

#include <QAbstractSpinBox>
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
        auto *titleLabel = new QLabel(title, this);
        titleLabel->setObjectName(QStringLiteral("SettingsCardTitle"));
        m_layout->addWidget(titleLabel);
    }
}

QWidget *SettingsCard::addRow(const QString &label, QWidget *field, const QString &hint)
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
    if (!hint.isEmpty()) {
        auto *hintWidget = new QLabel(hint, textColumn);
        hintWidget->setObjectName(QStringLiteral("SettingsRowHint"));
        hintWidget->setWordWrap(true);
        textLayout->addWidget(hintWidget);
    }

    field->setParent(row);
    rowLayout->addWidget(textColumn, 1);
    rowLayout->addWidget(field, 0, Qt::AlignRight | Qt::AlignVCenter);
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
