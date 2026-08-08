#include "ui/SessionTabBar.h"

#include "ui/MacWindowChrome.h"

#include <QEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QStyleOptionTab>
#include <QWindow>

#include <algorithm>

namespace {

/** @brief 单个会话标签允许占用的最小宽度，保证短标题仍有稳定点击区域。 */
constexpr int kMinimumTabWidth = 132;
/** @brief 单个会话标签允许占用的最大宽度，防止长主机名挤走全部其他标签。 */
constexpr int kMaximumTabWidth = 238;
/** @brief 胶囊标签固定高度；标题区上下各保留 5 像素呼吸空间。 */
constexpr int kTabHeight = 42;

} // namespace

SessionTabBar::SessionTabBar(QWidget *parent)
    : QTabBar(parent)
{
    setDocumentMode(true);
    setMovable(true);
    // 关闭按钮由本类按悬停状态绘制。若同时开启原生按钮，样式引擎会额外保留一份空位。
    setTabsClosable(false);
    setExpanding(false);
    setUsesScrollButtons(true);
    setElideMode(Qt::ElideRight);
    setMouseTracking(true);
    setDrawBase(false);
    setIconSize(QSize(18, 18));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(kTabHeight);
}

void SessionTabBar::setVisualColors(const QColor &textColor,
                                    const QColor &selectedTextColor,
                                    const QColor &hoverColor,
                                    const QColor &selectedColor,
                                    const QColor &accentColor)
{
    m_textColor = textColor;
    m_selectedTextColor = selectedTextColor;
    m_hoverColor = hoverColor;
    m_selectedColor = selectedColor;
    m_accentColor = accentColor;
    update();
}

QSize SessionTabBar::tabSizeHint(const int index) const
{
    if (index < 0 || index >= count()) {
        return QSize(kMinimumTabWidth, kTabHeight);
    }

    const QFontMetrics metrics(font());
    // 52 像素包含左右留白、终端图标与关闭按钮。关闭按钮只在需要时绘制，但始终预留
    // 空间可防止鼠标滑入标签时文字突然抖动或重新省略。
    const int contentWidth = metrics.horizontalAdvance(tabText(index)) + 68;
    return QSize(std::clamp(contentWidth, kMinimumTabWidth, kMaximumTabWidth), kTabHeight);
}

void SessionTabBar::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    for (int index = 0; index < count(); ++index) {
        const QRect rawRect = tabRect(index);
        if (!rawRect.intersects(rect())) {
            continue;
        }

        const bool selected = index == currentIndex();
        const bool hovered = index == m_hoveredIndex;
        // 标签之间保留 3 像素缝隙，圆角底色再向上下收缩，避免视觉上重新连成传统页签。
        const QRectF pillRect = QRectF(rawRect).adjusted(3.0, 4.0, -3.0, -4.0);
        if (selected || hovered) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(selected ? m_selectedColor : m_hoverColor);
            painter.drawRoundedRect(pillRect, 10.0, 10.0);
        }

        const QColor foreground = selected ? m_selectedTextColor : m_textColor;
        painter.setPen(QPen(foreground, 1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);

        // 自绘 17 像素终端图标可以与关闭图标保持同一线宽，且不依赖操作系统图标主题。
        const QRectF terminalRect(rawRect.left() + 15.0, rawRect.center().y() - 8.0, 17.0, 16.0);
        painter.drawRoundedRect(terminalRect, 3.0, 3.0);
        painter.drawLine(QPointF(terminalRect.left() + 4.0, terminalRect.top() + 5.0),
                         QPointF(terminalRect.left() + 7.0, terminalRect.top() + 8.0));
        painter.drawLine(QPointF(terminalRect.left() + 7.0, terminalRect.top() + 8.0),
                         QPointF(terminalRect.left() + 4.0, terminalRect.top() + 11.0));
        painter.drawLine(QPointF(terminalRect.left() + 9.5, terminalRect.top() + 11.0),
                         QPointF(terminalRect.left() + 13.0, terminalRect.top() + 11.0));

        if (selected) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_accentColor);
            painter.drawEllipse(QPointF(terminalRect.right() - 1.0, terminalRect.top() + 1.0),
                                2.2,
                                2.2);
        }

        const bool showCloseButton = selected || hovered;
        const int textRight = showCloseButton ? closeButtonRect(index).left() - 7
                                              : rawRect.right() - 13;
        const QRect textRect(rawRect.left() + 42,
                             rawRect.top(),
                             std::max(0, textRight - rawRect.left() - 42),
                             rawRect.height());
        painter.setPen(foreground);
        painter.setFont(font());
        const QString visibleTitle = painter.fontMetrics().elidedText(
            tabText(index), Qt::ElideRight, textRect.width());
        painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, visibleTitle);

        if (showCloseButton) {
            const QRect closeRect = closeButtonRect(index);
            if (closeRect.contains(mapFromGlobal(QCursor::pos()))) {
                QColor closeBackground = foreground;
                closeBackground.setAlpha(30);
                painter.setPen(Qt::NoPen);
                painter.setBrush(closeBackground);
                painter.drawEllipse(closeRect.adjusted(2, 2, -2, -2));
            }
            painter.setPen(QPen(foreground, 1.7, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(closeRect.center() + QPoint(-3, -3),
                             closeRect.center() + QPoint(3, 3));
            painter.drawLine(closeRect.center() + QPoint(3, -3),
                             closeRect.center() + QPoint(-3, 3));
        }
    }
}

void SessionTabBar::mousePressEvent(QMouseEvent *event)
{
    const bool pressedEmptyChrome = event->button() == Qt::LeftButton
        && tabAt(event->position().toPoint()) < 0;
    if (pressedEmptyChrome && window() != nullptr) {
        // macOS 优先消费当前 NSEvent，以获得与系统标题栏一致的拖动行为；其他平台或原生
        // 句柄尚未就绪时再回退到 Qt 的系统移动接口，不能自行累加鼠标坐标。
        bool moveStarted = beginMacWindowDrag(window());
        if (!moveStarted && window()->windowHandle() != nullptr) {
            moveStarted = window()->windowHandle()->startSystemMove();
        }
        if (moveStarted) {
            event->accept();
            return;
        }
    }

    // 标签内部必须继续进入基类状态机，否则 QTabBar 无法完成标签选中和拖动排序。
    QTabBar::mousePressEvent(event);
}

void SessionTabBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    const bool doubleClickedEmptyChrome = event->button() == Qt::LeftButton
        && tabAt(event->position().toPoint()) < 0;
    if (doubleClickedEmptyChrome && window() != nullptr) {
        if (!toggleMacWindowZoom(window())) {
            // 非 macOS 平台或 Cocoa 句柄短暂不可用时保留 Qt 语义，避免标题栏手势完全失效。
            window()->isMaximized() ? window()->showNormal() : window()->showMaximized();
        }
        event->accept();
        return;
    }

    QTabBar::mouseDoubleClickEvent(event);
}

void SessionTabBar::mouseMoveEvent(QMouseEvent *event)
{
    const int hoveredIndex = tabAt(event->position().toPoint());
    if (m_hoveredIndex != hoveredIndex) {
        m_hoveredIndex = hoveredIndex;
        update();
    }
    // QTabBar 仍需接收移动事件以维持原生拖动排序和滚动按钮状态。
    QTabBar::mouseMoveEvent(event);
}

void SessionTabBar::leaveEvent(QEvent *event)
{
    m_hoveredIndex = -1;
    update();
    QTabBar::leaveEvent(event);
}

void SessionTabBar::mouseReleaseEvent(QMouseEvent *event)
{
    const int index = tabAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton
        && index >= 0
        && (index == currentIndex() || index == m_hoveredIndex)
        && closeButtonRect(index).contains(event->position().toPoint())) {
        event->accept();
        emit tabCloseRequested(index);
        return;
    }
    QTabBar::mouseReleaseEvent(event);
}

QRect SessionTabBar::closeButtonRect(const int index) const
{
    if (index < 0 || index >= count()) {
        return {};
    }
    const QRect targetTab = tabRect(index);
    return QRect(targetTab.right() - 29, targetTab.center().y() - 9, 18, 18);
}
