#include "terminal/TerminalSearchBar.h"

#include "i18n/Retranslate.h"
#include "ui/UiIcons.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>

namespace {

/** @brief 无命中时输入框的警示边框色；终端主题无论深浅都能看清。 */
const QColor kNotFoundBorder(QStringLiteral("#e5484d"));

} // namespace

TerminalSearchBar::TerminalSearchBar(QWidget *parent)
    : QWidget(parent)
    , m_input(new QLineEdit(this))
    , m_resultLabel(new QLabel(this))
    , m_previousButton(new QToolButton(this))
    , m_nextButton(new QToolButton(this))
    , m_closeButton(new QToolButton(this))
{
    setObjectName(QStringLiteral("terminalSearchBar"));
    // 样式表背景只有在 WA_StyledBackground 下才会绘制到普通 QWidget 上。
    setAttribute(Qt::WA_StyledBackground, true);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 4, 6, 4);
    layout->setSpacing(4);

    m_input->setObjectName(QStringLiteral("terminalSearchInput"));
    m_input->setClearButtonEnabled(true);
    m_input->setMinimumWidth(160);
    m_input->setMaximumWidth(360);
    m_input->installEventFilter(this);

    m_resultLabel->setObjectName(QStringLiteral("terminalSearchResult"));
    m_resultLabel->setMinimumWidth(72);

    for (QToolButton *button : {m_previousButton, m_nextButton, m_closeButton}) {
        button->setAutoRaise(true);
        button->setIconSize(QSize(16, 16));
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setStyleSheet(UiIcons::sessionActionStyleSheet());
    }
    m_previousButton->setObjectName(QStringLiteral("terminalSearchPrevious"));
    m_nextButton->setObjectName(QStringLiteral("terminalSearchNext"));
    m_closeButton->setObjectName(QStringLiteral("terminalSearchClose"));

    layout->addWidget(m_input, 1);
    layout->addWidget(m_resultLabel);
    layout->addWidget(m_previousButton);
    layout->addWidget(m_nextButton);
    layout->addStretch(0);
    layout->addWidget(m_closeButton);

    connect(m_input, &QLineEdit::textChanged, this, [this](const QString &text) {
        emit searchRequested(text, TerminalSearchStep::Refine);
    });
    connect(m_previousButton, &QToolButton::clicked, this, [this]() {
        emit searchRequested(m_input->text(), TerminalSearchStep::Previous);
    });
    connect(m_nextButton, &QToolButton::clicked, this, [this]() {
        emit searchRequested(m_input->text(), TerminalSearchStep::Next);
    });
    connect(m_closeButton, &QToolButton::clicked, this, &TerminalSearchBar::closeRequested);

    i18n::onRetranslate(this, [this] {
        m_input->setPlaceholderText(tr("Find in terminal"));
        m_input->setAccessibleName(tr("Find in terminal"));
        m_previousButton->setToolTip(tr("Previous match (Enter)"));
        m_previousButton->setAccessibleName(tr("Previous match"));
        m_nextButton->setToolTip(tr("Next match (Shift+Enter)"));
        m_nextButton->setAccessibleName(tr("Next match"));
        m_closeButton->setToolTip(tr("Close search (Esc)"));
        m_closeButton->setAccessibleName(tr("Close search"));
        updateResultPresentation();
    });
    applyStyle();
    hide();
}

void TerminalSearchBar::activate(const QString &initialText)
{
    const bool replaceText = !initialText.isEmpty() && initialText != m_input->text();
    show();
    m_input->setFocus(Qt::ShortcutFocusReason);
    if (replaceText) {
        // setText 会触发 textChanged，从而按新内容查找一次。
        m_input->setText(initialText);
    } else if (!m_input->text().isEmpty()) {
        // 重新打开时终端可能已经刷出新内容，旧计数不再可信，按现有文本重查一次。
        emit searchRequested(m_input->text(), TerminalSearchStep::Refine);
    }
    m_input->selectAll();
}

QString TerminalSearchBar::text() const
{
    return m_input->text();
}

void TerminalSearchBar::setResult(const TerminalSearchResult &result)
{
    m_result = result;
    updateResultPresentation();
}

void TerminalSearchBar::setColors(const QColor &background,
                                  const QColor &foreground,
                                  const QColor &inputBackground)
{
    m_background = background;
    m_foreground = foreground;
    m_inputBackground = inputBackground;
    applyStyle();
}

bool TerminalSearchBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const int key = keyEvent->key();
        if (key == Qt::Key_Escape) {
            emit closeRequested();
            return true;
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            const TerminalSearchStep step = keyEvent->modifiers().testFlag(Qt::ShiftModifier)
                ? TerminalSearchStep::Next
                : TerminalSearchStep::Previous;
            emit searchRequested(m_input->text(), step);
            return true;
        }
        // 平台习惯的"查找下一个 / 上一个"（macOS ⌘G / ⇧⌘G，其他平台 F3 / Shift+F3）
        // 与两个按钮一一对应。
        if (keyEvent->matches(QKeySequence::FindNext)) {
            emit searchRequested(m_input->text(), TerminalSearchStep::Next);
            return true;
        }
        if (keyEvent->matches(QKeySequence::FindPrevious)) {
            emit searchRequested(m_input->text(), TerminalSearchStep::Previous);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TerminalSearchBar::updateResultPresentation()
{
    const bool hasText = !m_input->text().isEmpty();
    const bool hasMatches = m_result.total > 0;
    QString text;
    if (hasText && !hasMatches) {
        text = tr("No results");
    } else if (hasMatches) {
        const QString current = m_result.current > 0 ? QString::number(m_result.current)
                                                     : QStringLiteral("?");
        // 一次性多参数替换：链式 arg 会把第一个参数里出现的 %2 再替换一遍。
        text = m_result.totalCapped
            ? tr("%1 of %2+").arg(current, QString::number(m_result.total))
            : tr("%1 of %2").arg(current, QString::number(m_result.total));
    }
    m_resultLabel->setText(text);
    m_previousButton->setEnabled(hasMatches);
    m_nextButton->setEnabled(hasMatches);

    const bool notFound = hasText && !hasMatches;
    if (notFound != m_notFound) {
        m_notFound = notFound;
        applyStyle();
    }
}

void TerminalSearchBar::applyStyle()
{
    QColor border = m_foreground;
    border.setAlpha(70);
    const QColor inputBorder = m_notFound ? kNotFoundBorder : border;
    QColor focusBorder = m_foreground;
    focusBorder.setAlpha(150);
    setStyleSheet(
        QStringLiteral(
            "QWidget#terminalSearchBar { background: %1; color: %2; }"
            "QLabel#terminalSearchResult { color: %2; }"
            "QLineEdit#terminalSearchInput { background: %3; color: %2;"
            " border: 1px solid %4; border-radius: 5px; padding: 2px 6px; }"
            "QLineEdit#terminalSearchInput:focus { border-color: %5; }")
            .arg(m_background.name(),
                 m_foreground.name(),
                 m_inputBackground.name(),
                 inputBorder.name(QColor::HexArgb),
                 (m_notFound ? kNotFoundBorder : focusBorder).name(QColor::HexArgb)));

    // 自绘图标的线条色不受样式表影响，换主题时必须重新生成。
    m_previousButton->setIcon(UiIcons::symbol(this, UiIcons::Symbol::ChevronUp, m_foreground));
    m_nextButton->setIcon(UiIcons::symbol(this, UiIcons::Symbol::ChevronDown, m_foreground));
    m_closeButton->setIcon(UiIcons::symbol(this, UiIcons::Symbol::Close, m_foreground));
}
