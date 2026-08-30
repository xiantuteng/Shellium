#include "ui/SessionTabBar.h"

#include "ui/MacWindowChrome.h"

#include <QAbstractButton>
#include <QApplication>
#include <QEasingCurve>
#include <QEvent>
#include <QFontMetrics>
#include <QLabel>
#include <QMetaMethod>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cstdlib>

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

/** @brief 标签让位动画的默认上限时长；接近 Qt 原生 250ms，但反馈稍显利落。 */
constexpr int kMaximumTabMoveAnimationDuration = 220;
/** @brief 极短落位仍保留的最小时长，避免小幅拖动在松手时直接闪到终点。 */
constexpr int kMinimumTabSettleAnimationDuration = 80;

/**
 * @brief 根据系统样式配置计算标签换位动画时长。
 * @param widget 用于读取当前平台样式提示的控件。
 * @return 0 表示系统明确关闭控件动画；否则返回 140~220 毫秒的可感知时长。
 *
 * Qt 会通过 SH_Widget_Animation_Duration 统一表达平台动画偏好，包括“减少动态效果”等
 * 辅助功能设置。尊重 0 值可以避免强行播放动画；非零值限制在紧凑区间，则既能看清方向，
 * 又不会让连续拖过多个标签时产生明显滞后。
 */
[[nodiscard]] int tabMoveAnimationDuration(const QWidget *widget)
{
    const int styleDuration = widget->style()->styleHint(
        QStyle::SH_Widget_Animation_Duration, nullptr, widget);
    if (styleDuration <= 0) {
        return 0;
    }
    return std::clamp(styleDuration, 140, kMaximumTabMoveAnimationDuration);
}

/**
 * @brief 计算任意可视标签矩形中的关闭按钮区域。
 * @param targetTab 已包含拖拽或动画偏移的标签矩形。
 * @return 相对于标签栏的 18×18 像素命中区域。
 *
 * 绘制动画标签时不能再调用基于逻辑 tabRect() 的 closeButtonRect(index)，否则叉号会留在
 * 终点而胶囊仍在移动。把几何计算抽成无状态函数可确保静态与动画两条路径完全一致。
 */
[[nodiscard]] QRect closeButtonRectForTab(const QRect &targetTab)
{
    // 不使用 QRect::center()：整数矩形的 center 会向下取整（高 42 的标签得到 20 而不是
    // 21），再叠加“减去半边长”的写法，关闭图标会稳定偏上 1~2 像素。
    constexpr int closeButtonSize = 18;
    return QRect(targetTab.right() - 29,
                 targetTab.top() + (targetTab.height() - closeButtonSize) / 2,
                 closeButtonSize,
                 closeButtonSize);
}

/**
 * @brief 连接状态点的固定颜色；标题区背景恒定深色（不跟随应用深浅主题），因此不需要
 * 像内容区控件那样按主题分支取色，直接用饱和色即可保证可读性。
 */
[[nodiscard]] QColor connectionStateDotColor(const SessionTabBar::ConnectionState state)
{
    switch (state) {
    case SessionTabBar::ConnectionState::Connected:
        return QColor(QStringLiteral("#2bd981"));
    case SessionTabBar::ConnectionState::Failed:
        return QColor(QStringLiteral("#e5484d"));
    case SessionTabBar::ConnectionState::Connecting:
        return QColor(QStringLiteral("#e2a94f"));
    case SessionTabBar::ConnectionState::Unknown:
    default:
        return QColor();
    }
}

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

    // QTabBar::moveTab() 会在内部列表完成重排后同步发出 tabMoved。使用直属连接可以保证
    // 基类 mouseMoveEvent 返回前稳定标识已经切换到新顺序，随后计算的动画起终点不会错配。
    connect(this,
            &QTabBar::tabMoved,
            this,
            &SessionTabBar::handleTabMoved,
            Qt::DirectConnection);
    syncScrollButtons();
}

void SessionTabBar::setTabConnectionState(const int index, const ConnectionState state)
{
    const quint64 tabId = tabIdAt(index);
    if (tabId == 0) {
        return;
    }
    if (state == ConnectionState::Unknown) {
        m_tabConnectionStates.remove(tabId);
    } else {
        m_tabConnectionStates.insert(tabId, state);
    }
    update();
}

void SessionTabBar::setTabKind(const int index, const TabKind kind)
{
    const quint64 tabId = tabIdAt(index);
    if (tabId == 0) {
        return;
    }
    if (kind == TabKind::Terminal) {
        // 单终端是默认类型，删除显式记录可减少长期打开大量标签时的辅助状态数量。
        m_tabKinds.remove(tabId);
    } else {
        m_tabKinds.insert(tabId, kind);
    }
    update(tabRect(index));
}

SessionTabBar::TabKind SessionTabBar::tabKind(const int index) const
{
    const quint64 tabId = tabIdAt(index);
    return tabId == 0 ? TabKind::Terminal : m_tabKinds.value(tabId, TabKind::Terminal);
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
                                    const QColor &selectedColor)
{
    m_textColor = textColor;
    m_selectedTextColor = selectedTextColor;
    m_hoverColor = hoverColor;
    m_selectedColor = selectedColor;
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
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // 动画每帧会请求整个标签栏更新，但窗口系统仍可能把事件合并为较小区域；显式裁剪可
    // 避免在 Retina 屏幕上重复栅格化所有文字，同时不改变透明子控件的父背景传播行为。
    painter.setClipRegion(event->region());

    // 普通标签先画，当前标签后画，使正在落位的选中胶囊始终位于相邻标签之上。拖拽中的
    // 标签不在逻辑位置绘制，最后再以鼠标位置绘制唯一一份，从根源上消除原位置文字重影。
    for (int index = 0; index < count(); ++index) {
        if (index == currentIndex() || (m_dragActive && tabIdAt(index) == m_pressedTabId)) {
            continue;
        }
        const QRect rawRect = visualTabRect(index);
        if (rawRect.intersects(rect())) {
            const bool hovered = !m_dragActive
                && m_tabAnimationOffsets.value(tabIdAt(index), 0) == 0
                && index == m_hoveredIndex;
            drawSessionTab(painter, index, rawRect, hovered);
        }
    }

    const int selectedIndex = currentIndex();
    if (selectedIndex >= 0
        && selectedIndex < count()
        && !(m_dragActive && tabIdAt(selectedIndex) == m_pressedTabId)) {
        const QRect selectedRect = visualTabRect(selectedIndex);
        if (selectedRect.intersects(rect())) {
            const bool hovered = !m_dragActive
                && m_tabAnimationOffsets.value(tabIdAt(selectedIndex), 0) == 0
                && selectedIndex == m_hoveredIndex;
            drawSessionTab(painter, selectedIndex, selectedRect, hovered);
        }
    }

    if (!m_dragActive) {
        return;
    }

    const int draggedIndex = indexForTabId(m_pressedTabId);
    if (draggedIndex < 0) {
        // 标签可能在拖动期间被业务层关闭；此时不再绘制悬空快照，下一次鼠标事件会完成
        // 基类状态清理。这里保持绘制函数无副作用，避免 paintEvent 中修改模型。
        return;
    }

    QRect draggedRect = tabRect(draggedIndex);
    draggedRect.moveLeft(m_lastDragPosition.x() - m_pressedOffsetWithinTab);
    if (draggedRect.intersects(rect())) {
        drawSessionTab(painter, draggedIndex, draggedRect, false);
    }
}

void SessionTabBar::drawSessionTab(QPainter &painter,
                                   const int index,
                                   const QRect &rawRect,
                                   const bool hovered) const
{
    if (index < 0 || index >= count() || rawRect.isEmpty()) {
        return;
    }

    const bool selected = index == currentIndex();
    // 标签之间保留 3 像素缝隙，圆角底色再向上下收缩，避免视觉上重新连成传统页签。
    const QRectF pillRect = QRectF(rawRect).adjusted(3.0, 4.0, -3.0, -4.0);
    if (selected || hovered) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(selected ? m_selectedColor : m_hoverColor);
        painter.drawRoundedRect(pillRect, 10.0, 10.0);
    }

    const bool workspaceDropTarget = m_dragActive
        && m_workspaceDropArmed
        && tabIdAt(index) == m_workspaceDropTargetTabId;
    if (workspaceDropTarget) {
        // 拖入目标使用虚线圆角描边，与工作空间内终端 Hover 的视觉语义一致。即使源标签
        // 因 QTabBar 排序已经横向越过目标，稳定标识仍能让高亮留在真正的接收标签上。
        QColor dropColor = m_selectedTextColor;
        dropColor.setAlpha(225);
        painter.setPen(QPen(dropColor,
                            1.7,
                            Qt::DashLine,
                            Qt::RoundCap,
                            Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(pillRect.adjusted(1.0, 1.0, -1.0, -1.0), 9.0, 9.0);
    }

    const QColor foreground = selected ? m_selectedTextColor : m_textColor;
    painter.setPen(QPen(foreground, 1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    // 自绘 17 像素图标可以与关闭图标保持同一线宽，且不依赖操作系统图标主题。工作空间
    // 使用四宫格，单终端继续使用命令窗口，使用户只看轮廓也能区分两类标签。
    // 竖直定位走浮点中心，避免 QRect::center() 取整后整组图标一起偏上。
    const qreal rowCenterY = QRectF(rawRect).center().y();
    const QRectF terminalRect(rawRect.left() + 15.0, rowCenterY - 8.0, 17.0, 16.0);
    if (tabKind(index) == TabKind::Workspace) {
        constexpr qreal tileSize = 6.0;
        constexpr qreal gap = 2.5;
        for (int row = 0; row < 2; ++row) {
            for (int column = 0; column < 2; ++column) {
                painter.drawRoundedRect(
                    QRectF(terminalRect.left() + column * (tileSize + gap),
                           terminalRect.top() + row * (tileSize + gap),
                           tileSize,
                           tileSize),
                    1.4,
                    1.4);
            }
        }
    } else {
        painter.drawRoundedRect(terminalRect, 3.0, 3.0);
        painter.drawLine(QPointF(terminalRect.left() + 4.0, terminalRect.top() + 5.0),
                         QPointF(terminalRect.left() + 7.0, terminalRect.top() + 8.0));
        painter.drawLine(QPointF(terminalRect.left() + 7.0, terminalRect.top() + 8.0),
                         QPointF(terminalRect.left() + 4.0, terminalRect.top() + 11.0));
        painter.drawLine(QPointF(terminalRect.left() + 9.5, terminalRect.top() + 11.0),
                         QPointF(terminalRect.left() + 13.0, terminalRect.top() + 11.0));
    }

    // 连接状态点保留在图标右下角，并且不分选中/未选中都绘制：它表达的是会话连接结果，
    // 与当前标签选择状态无关。选中态已经由胶囊背景和高对比文字完整表达，不再额外绘制
    // 右上角绿点，避免一个没有独立业务含义的装饰元素干扰真正的连接状态。
    const ConnectionState connectionState =
        m_tabConnectionStates.value(tabIdAt(index), ConnectionState::Unknown);
    const QColor stateDotColor = connectionStateDotColor(connectionState);
    if (stateDotColor.isValid()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(stateDotColor);
        painter.drawEllipse(QPointF(terminalRect.right() - 1.0, terminalRect.bottom() - 1.0),
                            2.2,
                            2.2);
    }

    // 关闭按钮只在鼠标悬停的标签上出现。当前标签常驻显示关闭图标既是持续的误点风险，
    // 也让标题栏在静止时显得杂乱；标签宽度已按含关闭按钮预留，出现与否不会挤动文字。
    const bool showCloseButton = hovered;
    const QRect animatedCloseRect = closeButtonRectForTab(rawRect);
    const int textRight = showCloseButton ? animatedCloseRect.left() - 7 : rawRect.right() - 13;
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
        const QRectF closeRect(animatedCloseRect);
        // “悬停整个标签”（叉号出现）和“精确悬停在叉号上”（即将点击关闭）是两种不同
        // 强度的反馈：前者只让图标出现，后者除了背景圆还要让线条本身变色，否则用户
        // 反馈的“看不出鼠标真的移到关闭按钮上了”就无法成立。
        const bool preciseHover = index == m_closeHoveredIndex;
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

void SessionTabBar::mousePressEvent(QMouseEvent *event)
{
    const QPoint position = event->position().toPoint();
    const int pressedIndex = tabAt(position);
    const bool pressedEmptyChrome = event->button() == Qt::LeftButton && pressedIndex < 0;
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

    const int selectedBeforePress = currentIndex();
    if (event->button() == Qt::LeftButton && pressedIndex >= 0) {
        // 记录稳定标识而不是索引：一旦越过相邻标签，QTabBar 会在同一个 mouseMoveEvent 内
        // 改写索引；稳定标识仍能让绘制层找到用户真正抓住的那个标签。
        m_pressedTabId = tabIdAt(pressedIndex);
        // 基类随后会立即选中源标签，因此必须在调用基类前保存此前选中的目标稳定标识。
        m_workspaceDropTargetTabId = selectedBeforePress >= 0
                && selectedBeforePress != pressedIndex
                && tabKind(pressedIndex) == TabKind::Terminal
            ? tabIdAt(selectedBeforePress)
            : 0;
        m_workspaceDropTargetRect = m_workspaceDropTargetTabId == 0
            ? QRect{}
            : tabRect(selectedBeforePress);
        m_dragStartPosition = position;
        m_lastDragPosition = position;
        m_pressedOffsetWithinTab = position.x() - tabRect(pressedIndex).left();
        m_dragActive = false;
    }

    // 标签内部必须继续进入基类状态机，否则 QTabBar 无法完成标签选中和拖动排序。
    const bool deferSourceSelection = event->button() == Qt::LeftButton
        && pressedIndex >= 0
        && selectedBeforePress >= 0
        && selectedBeforePress != pressedIndex;
    if (deferSourceSelection) {
        // QTabBar 默认在按下非当前标签的瞬间就切换页面，这会把用户正要拖入的
        // 接收终端换成源终端。在阻断信号的临界区中仍让基类记录 pressedIndex，随后立即
        // 恢复原选中项；单击所需的真正切换延迟到 mouseReleaseEvent，因而拖动和单击
        // 仍能共用 Qt 的命中、排序和平台鼠标抓取逻辑。
        const QSignalBlocker selectionBlocker(this);
        QTabBar::mousePressEvent(event);
        setCurrentIndex(selectedBeforePress);
        return;
    }
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
    const QPoint position = event->position().toPoint();
    const bool leftButtonHeld = event->buttons().testFlag(Qt::LeftButton);
    const bool crossedDragThreshold = leftButtonHeld
        && m_pressedTabId != 0
        && (position - m_dragStartPosition).manhattanLength()
            > QApplication::startDragDistance();

    // 基类可能在本次调用中同步交换多个标签并发出 tabMoved。先保存每个稳定标签当前的
    // 屏幕矩形，调用返回后再与新逻辑矩形比对，就能从旧位置无缝启动让位动画；若上一段
    // 动画尚未结束，visualTabRect 已包含当前偏移，因此也不会突然跳回旧动画起点。
    QHash<quint64, QRect> visualRectsBeforeMove;
    if (m_dragActive || crossedDragThreshold) {
        for (int index = 0; index < count(); ++index) {
            const quint64 tabId = tabIdAt(index);
            if (tabId != 0) {
                visualRectsBeforeMove.insert(tabId, visualTabRect(index));
            }
        }
    }

    // 首次越过阈值时，Qt 会在基类调用内部创建原生拖拽快照。保存调用前的直属子控件列表，
    // 便于只识别本次新增的快照，避免误隐藏未来可能添加到标签上的业务按钮。
    const QList<QWidget *> childrenBeforeMove =
        findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);

    // QTabBar 仍负责成熟的命中、索引重排、tabMoved 信号和边界处理；本类只替换视觉合成。
    QTabBar::mouseMoveEvent(event);

    if (leftButtonHeld && (m_dragActive || crossedDragThreshold)) {
        m_dragActive = true;
        m_lastDragPosition = position;
        m_hoveredIndex = -1;
        m_closeHoveredIndex = -1;
        hideNativeDragPreview(childrenBeforeMove);

        const int targetIndex = indexForTabId(m_workspaceDropTargetTabId);
        const bool pointerOnTargetTab = targetIndex >= 0
            && (m_workspaceDropTargetRect.contains(position)
                || tabRect(targetIndex).contains(position));
        // 向下离开标签栏代表用户把标签拖进中央终端内容，而不是继续做水平排序。这里只
        // 设置视觉预备态；MainWindow 会在释放时用中央 QTabWidget 的真实全局矩形做最终校验。
        const bool pointerBelowTabBar = position.y() >= height();
        // 指针离开标签栏下沿：意图从"给标签排序"变成"把会话拖进终端区/侧栏"。此时亮出
        // 跟随光标的浮动标签快照并请求 MainWindow 显示落点提示层；真正的合并仍由
        // mouseReleaseEvent 按释放时的全局坐标决定，因此这条路径对自动化用例同样可驱动。
        if (pointerBelowTabBar
            && m_pressedTabId != 0
            && tabKind(indexForTabId(m_pressedTabId)) == TabKind::Terminal) {
            if (!m_externalDragActive) {
                m_externalDragActive = true;
                startExternalDragGhost();
            }
            moveExternalDragGhost(event->globalPosition().toPoint());
        } else if (m_externalDragActive) {
            // 指针又回到标签栏内：撤下浮动快照，恢复普通水平排序预览。
            endExternalDragGhost();
        }
        const bool dropArmed = m_workspaceDropTargetTabId != 0
            && (pointerOnTargetTab || pointerBelowTabBar);
        if (m_workspaceDropArmed != dropArmed) {
            m_workspaceDropArmed = dropArmed;
        }

        const int duration = tabMoveAnimationDuration(this);
        for (int index = 0; index < count(); ++index) {
            const quint64 tabId = tabIdAt(index);
            if (tabId == 0 || tabId == m_pressedTabId
                || !visualRectsBeforeMove.contains(tabId)) {
                continue;
            }

            const QRect finalRect = tabRect(index);
            const QRect previousVisualRect = visualRectsBeforeMove.value(tabId);
            if (previousVisualRect.left() != finalRect.left()) {
                // 相邻标签从拖动前的屏幕坐标缓动到重排后的 tabRect；偏移的正负直接保留
                // 向左/向右让位方向，用户无需等到松手才理解标签将被放到哪里。
                startTabMoveAnimation(tabId,
                                      previousVisualRect.left() - finalRect.left(),
                                      duration);
            }
        }
        update();
        return;
    }

    if (!leftButtonHeld && m_pressedTabId != 0) {
        // 某些窗口系统在鼠标于控件外释放时只会补送一条“不带按键”的移动事件。基类已在
        // 上方清理其 pressedIndex，这里同步丢弃本类手势状态，防止下一次悬停仍被当成拖动。
        resetPointerDragState();
    }

    const int hoveredIndex = tabAt(position);
    // 关闭按钮只在悬停的标签上画得出来，命中判定因此必须绑在同一个标签上，否则鼠标扫过
    // 相邻标签右端时会给一个根本没画出叉号的标签点亮“即将关闭”反馈。
    const int closeHoveredIndex =
        (hoveredIndex >= 0 && closeButtonRect(hoveredIndex).contains(position)) ? hoveredIndex : -1;
    if (m_hoveredIndex != hoveredIndex || m_closeHoveredIndex != closeHoveredIndex) {
        m_hoveredIndex = hoveredIndex;
        m_closeHoveredIndex = closeHoveredIndex;
        update();
    }
}

void SessionTabBar::leaveEvent(QEvent *event)
{
    m_hoveredIndex = -1;
    m_closeHoveredIndex = -1;
    update();
    QTabBar::leaveEvent(event);
}

void SessionTabBar::mouseReleaseEvent(QMouseEvent *event)
{
    const QPoint position = event->position().toPoint();
    const int index = tabAt(position);
    // 命中判定必须与绘制条件一致：只有正在悬停、因而真正画出了关闭图标的标签才可点击
    // 关闭，否则当前标签右端会残留一块看不见却能关掉会话的热区。
    if (event->button() == Qt::LeftButton
        && index >= 0
        && index == m_hoveredIndex
        && m_tabAnimationOffsets.value(tabIdAt(index), 0) == 0
        && closeButtonRect(index).contains(position)) {
        resetPointerDragState();
        event->accept();
        emit tabCloseRequested(index);
        return;
    }

    const bool shouldSettleDraggedTab = event->button() == Qt::LeftButton
        && m_dragActive
        && m_pressedTabId != 0;
    const quint64 draggedTabId = m_pressedTabId;
    const bool shouldActivateClickedTab = event->button() == Qt::LeftButton
        && !m_dragActive
        && draggedTabId != 0;
    const quint64 workspaceDropTargetTabId = m_workspaceDropTargetTabId;
    const QRect workspaceDropTargetRect = m_workspaceDropTargetRect;
    const int floatingLeft = position.x() - m_pressedOffsetWithinTab;

    QTabBar::mouseReleaseEvent(event);

    if (event->button() != Qt::LeftButton) {
        // 非左键释放由基类按默认语义处理，不得清空仍在进行的左键拖拽；例如用户按住左键
        // 时误触辅助鼠标键，标签仍应继续跟随最初的抓取手势。
        return;
    }

    if (shouldActivateClickedTab) {
        // 按下阶段为了区分“点击切换”和“拖入当前终端”而保留了原页面；只有确认
        // 鼠标从未越过拖拽阈值后才在松手时切换，普通标签单击的最终语义不变。
        const int clickedIndex = indexForTabId(draggedTabId);
        if (clickedIndex >= 0) {
            setCurrentIndex(clickedIndex);
        }
    }

    if (shouldSettleDraggedTab) {
        const int draggedIndex = indexForTabId(draggedTabId);
        if (draggedIndex >= 0) {
            const QRect finalRect = tabRect(draggedIndex);
            const int startOffset = floatingLeft - finalRect.left();
            const int baseDuration = tabMoveAnimationDuration(this);
            int settleDuration = 0;
            if (baseDuration > 0 && startOffset != 0) {
                // 距离很短时按比例缩短，但至少保留 80ms；长距离则封顶，既能看清落位方向，
                // 也不会因用户在标签栏外松手而让动画拖得过久。
                settleDuration = std::clamp(
                    (std::abs(startOffset) * baseDuration)
                        / std::max(1, finalRect.width()),
                    kMinimumTabSettleAnimationDuration,
                    baseDuration);
            }
            startTabMoveAnimation(draggedTabId, startOffset, settleDuration);
        }
    }

    // 索引必须在 QTabBar 完成重排后由稳定标识反查。释放点仍命中此前选中的目标胶囊时
    // 才解释为“拖入工作空间”；放在相邻位置或空白处仍只是普通标签排序。
    const int sourceIndex = indexForTabId(draggedTabId);
    const int targetIndex = indexForTabId(workspaceDropTargetTabId);
    const bool droppedOnTarget = shouldSettleDraggedTab
        && targetIndex >= 0
        && (workspaceDropTargetRect.contains(position)
            || tabRect(targetIndex).contains(position));
    const bool droppedBelowTabBar = shouldSettleDraggedTab
        && position.y() >= height();
    const bool validKinds = sourceIndex >= 0
        && targetIndex >= 0
        && tabKind(sourceIndex) == TabKind::Terminal
        && (tabKind(targetIndex) == TabKind::Terminal
            || tabKind(targetIndex) == TabKind::Workspace);

    resetPointerDragState();
    // 松手后恢复正常悬停反馈；若标签仍在落位，paintEvent 会暂时抑制叉号，直到几何稳定。
    m_hoveredIndex = tabAt(position);
    m_closeHoveredIndex = (m_hoveredIndex >= 0
                           && closeButtonRect(m_hoveredIndex).contains(position))
        ? m_hoveredIndex
        : -1;
    update();
    if ((droppedOnTarget || droppedBelowTabBar)
        && validKinds
        && sourceIndex != targetIndex) {
        emit workspaceMergeRequested(sourceIndex,
                                     targetIndex,
                                     event->globalPosition().toPoint(),
                                     droppedOnTarget);
    }
}

void SessionTabBar::tabInserted(const int index)
{
    QTabBar::tabInserted(index);

    // insertTab() 保证 index 位于新列表的有效范围；这里仍做边界收敛，使未来若 Qt 在异常
    // 恢复路径传入尾后索引，稳定标识列表也只会追加而不会触发越界断言。
    const qsizetype safeIndex = std::clamp<qsizetype>(index, 0, m_tabIds.size());
    m_tabIds.insert(safeIndex, m_nextTabId++);
}

void SessionTabBar::tabRemoved(const int index)
{
    if (index >= 0 && index < m_tabIds.size()) {
        const quint64 removedTabId = m_tabIds.at(index);
        stopTabMoveAnimation(removedTabId);
        m_tabIds.removeAt(index);
        m_tabConnectionStates.remove(removedTabId);
        m_tabKinds.remove(removedTabId);
        if (removedTabId == m_pressedTabId) {
            // 业务层可能在拖拽过程中关闭当前会话；立即终止本地手势可防止 paintEvent 继续
            // 查找一个已不存在的稳定标识，同时基类仍会按自身规则清理 pressedIndex。
            resetPointerDragState();
        }
    }

    QTabBar::tabRemoved(index);
}

void SessionTabBar::handleTabMoved(const int from, const int to)
{
    if (from == to
        || from < 0
        || to < 0
        || from >= m_tabIds.size()
        || to >= m_tabIds.size()) {
        return;
    }

    // 信号到达时 QTabBar 的业务列表已完成移动，稳定标识必须使用相同的 take+insert 语义；
    // 不能简单 swap，因为一次快速拖动可能跨过多个索引。
    const quint64 movedTabId = m_tabIds.takeAt(from);
    m_tabIds.insert(to, movedTabId);
}

quint64 SessionTabBar::tabIdAt(const int index) const
{
    return index >= 0 && index < m_tabIds.size() ? m_tabIds.at(index) : 0;
}

int SessionTabBar::indexForTabId(const quint64 tabId) const
{
    return tabId == 0 ? -1 : m_tabIds.indexOf(tabId);
}

QRect SessionTabBar::visualTabRect(const int index) const
{
    if (index < 0 || index >= count()) {
        return {};
    }

    QRect result = tabRect(index);
    result.translate(m_tabAnimationOffsets.value(tabIdAt(index), 0), 0);
    return result;
}

void SessionTabBar::startTabMoveAnimation(const quint64 tabId,
                                          const int startOffset,
                                          const int duration)
{
    if (tabId == 0 || indexForTabId(tabId) < 0) {
        return;
    }

    stopTabMoveAnimation(tabId);
    if (startOffset == 0 || duration <= 0) {
        // duration 为 0 通常来自系统“减少动态效果”设置；直接清除偏移比创建零时长动画
        // 更可靠，也避免 finished 信号在调用栈内同步触发并重入绘制状态清理。
        m_tabAnimationOffsets.remove(tabId);
        update();
        return;
    }

    m_tabAnimationOffsets.insert(tabId, startOffset);
    auto *animation = new QVariantAnimation(this);
    animation->setStartValue(startOffset);
    animation->setEndValue(0);
    animation->setDuration(duration);
    animation->setEasingCurve(QEasingCurve::InOutCubic);

    connect(animation,
            &QVariantAnimation::valueChanged,
            this,
            [this, tabId, animation](const QVariant &value) {
                // 快速跨越多个标签会替换同一标识的旧动画；延迟到达的旧回调不得覆盖新动画
                // 已计算的偏移，因此先核对哈希表中仍登记的是当前对象。
                if (m_tabMoveAnimations.value(tabId) != animation) {
                    return;
                }
                m_tabAnimationOffsets.insert(tabId, value.toInt());
                update();
            });
    connect(animation,
            &QVariantAnimation::finished,
            this,
            [this, tabId, animation]() {
                if (m_tabMoveAnimations.value(tabId) == animation) {
                    m_tabMoveAnimations.remove(tabId);
                    m_tabAnimationOffsets.remove(tabId);
                    update();
                }
                animation->deleteLater();
            });

    m_tabMoveAnimations.insert(tabId, animation);
    animation->start();
}

void SessionTabBar::stopTabMoveAnimation(const quint64 tabId)
{
    QVariantAnimation *animation = m_tabMoveAnimations.take(tabId);
    if (animation != nullptr) {
        // stop() 不会发出 finished；deleteLater() 让可能正位于 valueChanged 调用栈中的对象
        // 安全返回事件循环后再销毁，避免快速连续拖动产生重入删除。
        animation->stop();
        animation->deleteLater();
    }
    m_tabAnimationOffsets.remove(tabId);
}

void SessionTabBar::hideNativeDragPreview(const QList<QWidget *> &childrenBeforeMove)
{
    if (m_nativeDragPreview.isNull()) {
        const QList<QWidget *> childrenAfterMove =
            findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
        for (QWidget *candidate : childrenAfterMove) {
            if (candidate == nullptr
                || childrenBeforeMove.contains(candidate)
                || qobject_cast<QAbstractButton *>(candidate) != nullptr) {
                continue;
            }

            // QTabBar 首次进入 dragInProgress 时唯一新增的非按钮直属子控件就是移动快照。
            // 保存实际指针比匹配私有类名稳定：该类没有公开 API，Qt 可在小版本中改名。
            m_nativeDragPreview = candidate;
            break;
        }
    }

    if (!m_nativeDragPreview.isNull()) {
        // 基类会在每次新拖动开始时重新 show() 同一个快照，所以每一帧基类处理后都隐藏；
        // 隐藏不销毁对象，也不干扰 QTabBar 在释放时运行自己的收尾动画和状态复位。
        m_nativeDragPreview->hide();
    }
}

void SessionTabBar::startExternalDragGhost()
{
    const int draggedIndex = indexForTabId(m_pressedTabId);
    if (draggedIndex < 0 || !m_dragGhost.isNull()) {
        return;
    }

    // 用与静态标签一致的绘制路径渲染快照；hovered=true 借用悬停底色，保证脱离胶囊背景后
    // 快照仍是一块完整的圆角标签而不是悬空文字。
    const QRect tabBox = tabRect(draggedIndex);
    const qreal dpr = devicePixelRatioF();
    QPixmap snapshot(tabBox.size() * dpr);
    snapshot.setDevicePixelRatio(dpr);
    snapshot.fill(Qt::transparent);
    {
        QPainter painter(&snapshot);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.translate(-tabBox.topLeft());
        drawSessionTab(painter, draggedIndex, tabBox, true);
    }

    // 无边框、鼠标穿透的顶层控件：只负责显示快照，不参与命中，也不抢窗口焦点。
    auto *ghost = new QLabel(nullptr,
                             Qt::ToolTip | Qt::FramelessWindowHint
                                 | Qt::WindowTransparentForInput
                                 | Qt::NoDropShadowWindowHint);
    ghost->setObjectName(QStringLiteral("SessionTabDragGhost"));
    ghost->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    ghost->setAttribute(Qt::WA_ShowWithoutActivating, true);
    ghost->setAttribute(Qt::WA_TranslucentBackground, true);
    ghost->setWindowOpacity(0.9);
    ghost->setPixmap(snapshot);
    ghost->resize(tabBox.size());
    m_dragGhost = ghost;

    m_hoveredIndex = -1;
    m_closeHoveredIndex = -1;
    update();
    emit externalDragStarted();
}

void SessionTabBar::moveExternalDragGhost(const QPoint &globalPosition)
{
    if (m_dragGhost.isNull()) {
        return;
    }
    // 快照左边缘对齐最初的抓取位置，纵向让它稍稍位于光标下方，避免完全盖住落点。
    m_dragGhost->move(globalPosition.x() - m_pressedOffsetWithinTab,
                      globalPosition.y() + 6);
    if (!m_dragGhost->isVisible()) {
        m_dragGhost->show();
        m_dragGhost->raise();
    }
}

void SessionTabBar::endExternalDragGhost()
{
    const bool wasActive = m_externalDragActive || !m_dragGhost.isNull();
    m_externalDragActive = false;
    if (!m_dragGhost.isNull()) {
        m_dragGhost->deleteLater();
        m_dragGhost.clear();
    }
    if (wasActive) {
        emit externalDragFinished();
    }
}

void SessionTabBar::resetPointerDragState()
{
    // 撤下浮动快照并通知落点层隐藏；endExternalDragGhost 只在确有拖出手势时才发信号。
    endExternalDragGhost();
    m_dragActive = false;
    m_pressedTabId = 0;
    m_workspaceDropTargetTabId = 0;
    m_workspaceDropTargetRect = {};
    m_workspaceDropArmed = false;
    m_dragStartPosition = {};
    m_pressedOffsetWithinTab = 0;
    m_lastDragPosition = {};
    if (!m_nativeDragPreview.isNull()) {
        m_nativeDragPreview->hide();
    }
}

QRect SessionTabBar::closeButtonRect(const int index) const
{
    if (index < 0 || index >= count()) {
        return {};
    }
    return closeButtonRectForTab(tabRect(index));
}
