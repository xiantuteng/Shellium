#include "ui/SessionTabBar.h"

#include "ui/MacWindowChrome.h"

#include <QAbstractButton>
#include <QEvent>
#include <QFontMetrics>
#include <QMetaMethod>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QStyleOptionTab>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

#include <algorithm>

namespace {

/** @brief 单个会话标签允许占用的最小宽度，保证短标题仍有稳定点击区域。 */
constexpr int kMinimumTabWidth = 132;
/** @brief 单个会话标签允许占用的最大宽度，防止长主机名挤走全部其他标签。 */
constexpr int kMaximumTabWidth = 238;
/** @brief 胶囊标签固定高度；标题区上下各保留 5 像素呼吸空间。 */
constexpr int kTabHeight = 42;

/**
 * @brief QTabBar 内置滚动按钮的固定对象名，自 Qt 4 起保持稳定。
 *
 * Qt 未公开滚动偏移量的读写接口，但为这两个按钮设置了稳定对象名；借助它们即可在不
 * 复制 QTabBar 私有布局逻辑的前提下把滚动动作交给外部按钮触发。
 */
const char *const kInternalScrollLeftName = "ScrollLeftButton";
const char *const kInternalScrollRightName = "ScrollRightButton";

/** @brief 内置滚动按钮移出可视区域后的横坐标；子控件会被父控件裁剪而不可见。 */
constexpr int kHiddenScrollButtonX = -400;

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

    // 内置滚动按钮必须保留：它们持有 QTabBar 唯一可用的滚动入口。这里只把它们移出可视
    // 区域，改由标题栏中左右分置的外部按钮转发点击。
    m_internalScrollLeft = findChild<QAbstractButton *>(
        QLatin1String(kInternalScrollLeftName));
    m_internalScrollRight = findChild<QAbstractButton *>(
        QLatin1String(kInternalScrollRightName));
    if (m_internalScrollLeft != nullptr && m_internalScrollRight != nullptr) {
        m_internalScrollLeft->installEventFilter(this);
        m_internalScrollRight->installEventFilter(this);
    }
    syncScrollButtons();
}

bool SessionTabBar::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == m_internalScrollLeft || watched == m_internalScrollRight)
        && (event->type() == QEvent::EnabledChange
            || event->type() == QEvent::Show
            || event->type() == QEvent::Hide)) {
        syncScrollButtons();
    }
    return QTabBar::eventFilter(watched, event);
}

void SessionTabBar::scrollTabsLeft()
{
    if (m_internalScrollLeft != nullptr && m_internalScrollLeft->isEnabled()) {
        m_internalScrollLeft->click();
    }
}

void SessionTabBar::scrollTabsRight()
{
    if (m_internalScrollRight != nullptr && m_internalScrollRight->isEnabled()) {
        m_internalScrollRight->click();
    }
}

void SessionTabBar::syncScrollButtons()
{
    if (m_internalScrollLeft == nullptr || m_internalScrollRight == nullptr) {
        return;
    }
    // QTabBar 每次重新布局都会把两个按钮摆回右下角，因此每次布局后都要重新移开。
    m_internalScrollLeft->move(kHiddenScrollButtonX, 0);
    m_internalScrollRight->move(kHiddenScrollButtonX, 0);

    // Qt 用可见性表达"标签是否溢出"，用可用性表达"该方向是否还有内容"。
    const bool scrollable = !m_internalScrollLeft->isHidden();
    const bool canScrollLeft = scrollable && m_internalScrollLeft->isEnabled();
    const bool canScrollRight = scrollable && m_internalScrollRight->isEnabled();
    if (scrollable == m_scrollable
        && canScrollLeft == m_canScrollLeft
        && canScrollRight == m_canScrollRight) {
        // 布局在窗口缩放期间会被反复触发；只在状态真正变化时通知外部，避免无谓的重绘，
        // 也避免外部按钮反复更新再反过来触发本控件重新布局。
        return;
    }
    m_scrollable = scrollable;
    m_canScrollLeft = canScrollLeft;
    m_canScrollRight = canScrollRight;
    emit scrollStateChanged(scrollable, canScrollLeft, canScrollRight);
}

void SessionTabBar::connectNotify(const QMetaMethod &signal)
{
    QTabBar::connectNotify(signal);
    if (signal == QMetaMethod::fromSignal(&SessionTabBar::scrollStateChanged)) {
        // 见头文件中的说明：状态可能早于本次连接就已经稳定，之后不会再有变化触发
        // syncScrollButtons() 重新发出信号，新监听者会永远学不到当前真实状态。补发一次
        // 缓存值即可，对已连接的旧监听者只是收到一次数值相同的重复信号。
        emit scrollStateChanged(m_scrollable, m_canScrollLeft, m_canScrollRight);
    }
}

void SessionTabBar::wheelEvent(QWheelEvent *event)
{
    if (!m_scrollable) {
        // 没有溢出内容可滚时把事件交还基类/父级，避免吞掉本该继续传递的滚轮事件。
        QTabBar::wheelEvent(event);
        return;
    }

    // 触控板的横向滚动优先：angleDelta().x() 非零时说明用户在做水平手势，此时纵向分量
    // 通常为 0 或噪声，不能相加，否则同一次手势会被同时按两个方向计入累加器。
    const QPoint delta = event->angleDelta();
    const int primaryDelta = delta.x() != 0 ? delta.x() : delta.y();
    if (primaryDelta == 0) {
        event->accept();
        return;
    }

    // 累积到一整格（120，Qt 定义的标准滚轮单位）才触发一次滚动，效果等同于点击一次
    // 外部箭头按钮；直接按 delta 数值滚动会在触控板惯性滚动时让标签"飞"过去。
    m_wheelDeltaAccumulator += primaryDelta;
    constexpr int kWheelStepThreshold = 120;
    while (m_wheelDeltaAccumulator >= kWheelStepThreshold) {
        // 向上滚 / 向左滚（正值）在人类直觉里是把内容往左带，即让更靠左的标签出现，
        // 也就是向左滚动标签。
        scrollTabsLeft();
        m_wheelDeltaAccumulator -= kWheelStepThreshold;
    }
    while (m_wheelDeltaAccumulator <= -kWheelStepThreshold) {
        scrollTabsRight();
        m_wheelDeltaAccumulator += kWheelStepThreshold;
    }
    event->accept();
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

void SessionTabBar::resizeEvent(QResizeEvent *event)
{
    QTabBar::resizeEvent(event);
    syncScrollButtons();
}

void SessionTabBar::tabLayoutChange()
{
    QTabBar::tabLayoutChange();
    syncScrollButtons();
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
        // 竖直定位走浮点中心，避免 QRect::center() 取整后整组图标一起偏上。
        const qreal rowCenterY = QRectF(rawRect).center().y();
        const QRectF terminalRect(rawRect.left() + 15.0, rowCenterY - 8.0, 17.0, 16.0);
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

        // 关闭按钮只在鼠标悬停的标签上出现。当前标签常驻显示关闭图标既是持续的误点风险，
        // 也让标题栏在静止时显得杂乱；标签宽度已按含关闭按钮预留，出现与否不会挤动文字。
        const bool showCloseButton = hovered;
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
            const QRectF closeRect(closeButtonRect(index));
            // "悬停整个标签"（叉号出现）和"精确悬停在叉号上"（即将点击关闭）是两种不同
            // 强度的反馈：前者只让图标出现，后者除了背景圆还要让线条本身变色，否则用户
            // 反馈的"看不出鼠标真的移到关闭按钮上了"就无法成立。
            const bool preciseHover = closeRect.contains(mapFromGlobal(QCursor::pos()));
            if (preciseHover) {
                QColor closeBackground = foreground;
                closeBackground.setAlpha(30);
                painter.setPen(Qt::NoPen);
                painter.setBrush(closeBackground);
                painter.drawEllipse(closeRect.adjusted(2.0, 2.0, -2.0, -2.0));
            }
            // 叉号同样按浮点中心绘制：QRect::center() 会再次向下取整，把已经居中的命中区
            // 与实际画出的图形错开一像素。
            const QPointF closeCenter = closeRect.center();
            const QColor closeStrokeColor = preciseHover ? m_closeHoverColor : foreground;
            painter.setPen(QPen(closeStrokeColor, 1.7, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(closeCenter + QPointF(-3.0, -3.0),
                             closeCenter + QPointF(3.0, 3.0));
            painter.drawLine(closeCenter + QPointF(3.0, -3.0),
                             closeCenter + QPointF(-3.0, 3.0));
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
    // 命中判定必须与绘制条件一致：只有正在悬停、因而真正画出了关闭图标的标签才可点击
    // 关闭，否则当前标签右端会残留一块看不见却能关掉会话的热区。
    if (event->button() == Qt::LeftButton
        && index >= 0
        && index == m_hoveredIndex
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
    // 不使用 QRect::center()：整数矩形的 center 会向下取整（高 42 的标签得到 20 而不是
    // 21），再叠加"减去半边长"的写法，关闭图标就会稳定偏上 1~2 像素。按高度差平分才能
    // 得到真正的垂直居中。
    constexpr int closeButtonSize = 18;
    return QRect(targetTab.right() - 29,
                 targetTab.top() + (targetTab.height() - closeButtonSize) / 2,
                 closeButtonSize,
                 closeButtonSize);
}
