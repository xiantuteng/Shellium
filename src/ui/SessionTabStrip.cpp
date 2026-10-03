#include "ui/SessionTabStrip.h"

#include "ui/SessionTabBar.h"

#include <QEasingCurve>
#include <QEvent>
#include <QMetaMethod>
#include <QResizeEvent>
#include <QStyle>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>

namespace {

/** @brief 外部箭头每点击一次滚动的像素；按住时随自动重复连续累加。 */
constexpr int kArrowScrollStep = 96;
/** @brief 普通鼠标滚轮每格（angleDelta 120）折算的像素。 */
constexpr int kWheelPixelsPerNotch = 60;
/** @brief Qt 定义的一格标准滚轮增量。 */
constexpr int kWheelNotch = 120;
/** @brief 箭头平滑滚动的上限时长；按住箭头时每段动画都会被下一次点击续接。 */
constexpr int kMaximumScrollAnimationDuration = 160;

} // namespace

SessionTabStrip::SessionTabStrip(QWidget *parent)
    : QWidget(parent)
    , m_tabBar(new SessionTabBar(this))
{
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    setFixedHeight(m_tabBar->height());
    m_tabBar->move(0, 0);
    m_tabBar->installEventFilter(this);

    connect(m_tabBar,
            &SessionTabBar::tabLayoutChanged,
            this,
            &SessionTabStrip::handleTabLayoutChanged);
    connect(m_tabBar,
            &QTabBar::currentChanged,
            this,
            &SessionTabStrip::ensureTabVisible);
    syncTabBarGeometry();
}

SessionTabBar *SessionTabStrip::tabBar() const
{
    return m_tabBar;
}

int SessionTabStrip::scrollOffset() const
{
    return m_scrollOffset;
}

int SessionTabStrip::maximumScrollOffset() const
{
    return std::max(0, contentWidth() - width());
}

void SessionTabStrip::scrollByPixels(const int pixels, const bool animated)
{
    const int duration = animated
        ? std::min(style()->styleHint(QStyle::SH_Widget_Animation_Duration, nullptr, this),
                   kMaximumScrollAnimationDuration)
        : 0;
    if (duration <= 0) {
        // 系统"减少动态效果"会把动画时长设为 0，此时与滚轮一样直接落位。
        stopScrollAnimation();
        setScrollOffset(m_scrollOffset + pixels);
        return;
    }

    const bool running = m_scrollAnimation != nullptr
        && m_scrollAnimation->state() == QAbstractAnimation::Running;
    const int base = running ? m_scrollTarget : m_scrollOffset;
    const int target = std::clamp(base + pixels, 0, maximumScrollOffset());
    if (target == m_scrollOffset) {
        stopScrollAnimation();
        return;
    }
    m_scrollTarget = target;

    if (m_scrollAnimation == nullptr) {
        m_scrollAnimation = new QVariantAnimation(this);
        m_scrollAnimation->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_scrollAnimation,
                &QVariantAnimation::valueChanged,
                this,
                [this](const QVariant &value) { setScrollOffset(value.toInt()); });
    }
    // 从当前屏幕位置续接新终点，按住箭头时的多段动画首尾相连，不会回跳。
    m_scrollAnimation->stop();
    m_scrollAnimation->setStartValue(m_scrollOffset);
    m_scrollAnimation->setEndValue(target);
    m_scrollAnimation->setDuration(duration);
    m_scrollAnimation->start();
}

void SessionTabStrip::scrollTabsLeft()
{
    scrollByPixels(-kArrowScrollStep, true);
}

void SessionTabStrip::scrollTabsRight()
{
    scrollByPixels(kArrowScrollStep, true);
}

QSize SessionTabStrip::sizeHint() const
{
    return {contentWidth(), m_tabBar->height()};
}

QSize SessionTabStrip::minimumSizeHint() const
{
    return {0, m_tabBar->height()};
}

void SessionTabStrip::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    syncTabBarGeometry();
    // 视口变宽后当前标签可能已整体可见，变窄后则可能被挤出右缘；与 QTabBar 原生行为
    // 一致，缩放窗口时始终保证当前标签在视野内。
    ensureTabVisible(m_tabBar->currentIndex());
}

bool SessionTabStrip::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_tabBar && event->type() == QEvent::Wheel) {
        return handleWheel(static_cast<QWheelEvent *>(event));
    }
    return QWidget::eventFilter(watched, event);
}

void SessionTabStrip::connectNotify(const QMetaMethod &signal)
{
    QWidget::connectNotify(signal);
    if (signal == QMetaMethod::fromSignal(&SessionTabStrip::scrollStateChanged)) {
        emit scrollStateChanged(m_scrollable, m_canScrollLeft, m_canScrollRight);
    }
}

bool SessionTabStrip::handleWheel(QWheelEvent *event)
{
    if (!m_scrollable) {
        // 没有溢出内容时交还标签栏的默认处理，不吞掉本该继续传递的滚轮事件。
        return false;
    }

    // 触控板的横向手势优先：x 分量非零时 y 通常只是噪声，二者相加会让同一次手势被
    // 同时按两个方向计入。
    const QPoint pixelDelta = event->pixelDelta();
    int pixels = 0;
    if (!pixelDelta.isNull()) {
        pixels = pixelDelta.x() != 0 ? pixelDelta.x() : pixelDelta.y();
    } else {
        const QPoint angleDelta = event->angleDelta();
        const int angle = angleDelta.x() != 0 ? angleDelta.x() : angleDelta.y();
        // 高精度滚轮会送来远小于 120 的增量；保留余数才能让这些小增量累积成像素，
        // 而不是每次都被整除截断为 0。
        const int scaled = angle * kWheelPixelsPerNotch + m_wheelAngleRemainder;
        pixels = scaled / kWheelNotch;
        m_wheelAngleRemainder = scaled % kWheelNotch;
    }

    // 向上 / 向左滚（正值）把内容往右带，让更靠左的标签出现，即减小滚动偏移。
    stopScrollAnimation();
    setScrollOffset(m_scrollOffset - pixels);
    event->accept();
    return true;
}

void SessionTabStrip::setScrollOffset(const int offset)
{
    const int clamped = std::clamp(offset, 0, maximumScrollOffset());
    if (clamped != m_scrollOffset) {
        m_scrollOffset = clamped;
        m_tabBar->move(-m_scrollOffset, 0);
    }
    syncScrollState();
}

void SessionTabStrip::syncTabBarGeometry()
{
    // 标签栏至少铺满视口：视口比标签总宽更宽时（布局尚未收缩的瞬间），空白处仍属于
    // 标签栏，窗口拖动与双击放大手势保持可用。
    const int barWidth = std::max(contentWidth(), width());
    if (m_tabBar->width() != barWidth) {
        m_tabBar->resize(barWidth, m_tabBar->height());
    }
    const int clamped = std::clamp(m_scrollOffset, 0, maximumScrollOffset());
    if (clamped != m_scrollOffset) {
        stopScrollAnimation();
    }
    m_scrollOffset = clamped;
    m_tabBar->move(-m_scrollOffset, 0);
    syncScrollState();
}

void SessionTabStrip::handleTabLayoutChanged()
{
    // 本回调位于 QTabBar 的布局过程内部，此刻改变标签栏尺寸会重入它的布局。这里只同步
    // 通知外层布局"建议宽度变了"（外层据此可立即重新分配宽度），标签栏本身的几何
    // 推迟到事件循环中、或外层布局调整视口尺寸时再摆放。
    updateGeometry();
    if (!m_geometrySyncPending) {
        m_geometrySyncPending = true;
        QMetaObject::invokeMethod(
            this,
            [this]() {
                m_geometrySyncPending = false;
                syncTabBarGeometry();
                ensureTabVisible(m_tabBar->currentIndex());
            },
            Qt::QueuedConnection);
    }
}

void SessionTabStrip::ensureTabVisible(const int index)
{
    if (index < 0 || index >= m_tabBar->count()) {
        return;
    }
    // 当前标签切换可能发生在标签栏尚未按新内容加宽之前，先同步几何，避免用旧宽度收敛
    // 偏移而滚不到最后一个标签。
    syncTabBarGeometry();
    const QRect tab = m_tabBar->tabRect(index);
    int offset = m_scrollOffset;
    if (tab.left() < offset) {
        offset = tab.left();
    } else if (tab.right() + 1 > offset + width()) {
        offset = tab.right() + 1 - width();
    }
    if (offset != m_scrollOffset) {
        stopScrollAnimation();
        setScrollOffset(offset);
    }
}

void SessionTabStrip::stopScrollAnimation()
{
    if (m_scrollAnimation != nullptr) {
        m_scrollAnimation->stop();
    }
    m_scrollTarget = m_scrollOffset;
}

void SessionTabStrip::syncScrollState()
{
    if (m_emittingScrollState) {
        // 监听者在处理信号时会显示/隐藏外部箭头，而显示子控件会让 Qt 同步激活父布局，
        // 进而改变本视口宽度并回到这里。若此时嵌套发出新状态，监听者会在 setVisible(true)
        // 尚未返回时执行 setVisible(false)，箭头留下"已隐藏却仍标记为可见"的矛盾状态。
        // 因此只记下需要重算，等外层发射完成后再统一发出最终状态。
        m_scrollStateDirty = true;
        return;
    }
    do {
        m_scrollStateDirty = false;
        const int maximum = maximumScrollOffset();
        const bool scrollable = maximum > 0;
        const bool canScrollLeft = scrollable && m_scrollOffset > 0;
        const bool canScrollRight = scrollable && m_scrollOffset < maximum;
        if (scrollable == m_scrollable
            && canScrollLeft == m_canScrollLeft
            && canScrollRight == m_canScrollRight) {
            continue;
        }
        m_scrollable = scrollable;
        m_canScrollLeft = canScrollLeft;
        m_canScrollRight = canScrollRight;
        m_emittingScrollState = true;
        emit scrollStateChanged(scrollable, canScrollLeft, canScrollRight);
        m_emittingScrollState = false;
    } while (m_scrollStateDirty);
}

int SessionTabStrip::contentWidth() const
{
    return m_tabBar->count() == 0 ? 0 : m_tabBar->sizeHint().width();
}
