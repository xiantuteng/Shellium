#include "terminal/TerminalWorkspaceGroup.h"

#include "terminal/TerminalPane.h"
#include "terminal/TerminalWorkspace.h"
#include "ui/HostDistributionIcon.h"
#include "ui/UiIcons.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

namespace {

/** @brief 会话状态栏容器的对象名，见 TerminalPane 构造。分屏拖动以它作为把手。 */
const char *const kSessionHeaderObjectName = "terminalSessionStatusBar";

/**
 * @brief 取一个成员终端当前的背景色，作为工作空间容器与卡片的底色。
 *
 * 工作空间的空隙、内衬和卡片底必须与终端会话背景保持一致，否则深色终端四周会出现一圈
 * 突兀的浅色（应用在浅色主题下的 QPalette::Window/Base）。终端主题色写在 TerminalView 的
 * QPalette::Base 上；读不到时回退到与标题栏一致的深色，而不是应用窗口色。
 */
[[nodiscard]] QColor terminalChromeBackground(const TerminalWorkspace *workspace)
{
    if (workspace != nullptr) {
        if (const auto *view =
                workspace->findChild<QPlainTextEdit *>(QStringLiteral("TerminalView"))) {
            const QColor base = view->palette().color(QPalette::Base);
            if (base.isValid()) {
                return base;
            }
        }
    }
    return QColor(QStringLiteral("#1c2333"));
}

/** @brief 判断某个控件是否处在终端会话状态栏内（用于识别分屏拖动把手）。 */
[[nodiscard]] bool isWithinSessionHeader(const QObject *object)
{
    for (const QObject *node = object; node != nullptr; node = node->parent()) {
        if (node->objectName() == QLatin1String(kSessionHeaderObjectName)) {
            return true;
        }
    }
    return false;
}

} // namespace

/**
 * @brief 为工作空间中的单个终端绘制三态边框，并把会话状态栏变成分屏拖动把手。
 *
 * 事件过滤器安装到终端的全部既有子控件上：点击终端正文/滚动条选中该成员；在会话状态栏
 * 上按下并拖动则发起分屏停靠拖动（浮动快照跟随光标，落点由 TerminalWorkspaceGroup 高亮）。
 */
class WorkspaceTerminalFrame final : public QFrame {
public:
    explicit WorkspaceTerminalFrame(TerminalWorkspace *workspace, QWidget *parent)
        : QFrame(parent)
        , m_workspace(workspace)
    {
        setObjectName(QStringLiteral("WorkspaceTerminalFrame"));
        setMouseTracking(true);
        auto *layout = new QVBoxLayout(this);
        // 卡片底色与边框绘制在控件内部，留 6px 让圆角卡片的描边和内衬在终端四周透出，
        // 不覆盖终端文字与滚动条命中区域。
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(0);
        layout->addWidget(workspace);
        // 动态属性为自动化和主题扩展提供稳定、无需了解内部子类的状态入口；实际边框仍由
        // paintEvent 绘制，避免样式表在高 DPI 下把虚线圆角栅格化得参差不齐。
        setProperty("workspaceSelected", false);
        setProperty("workspaceHovered", false);
        installInputFilters();
    }

    /** @brief 返回框内的终端，用于把鼠标事件映射回工作空间成员。 */
    [[nodiscard]] TerminalWorkspace *workspace() const { return m_workspace; }

    /** @brief 设置选中态；选中优先于悬停并立即重绘。 */
    void setSelected(const bool selected)
    {
        if (m_selected == selected) {
            return;
        }
        m_selected = selected;
        setProperty("workspaceSelected", selected);
        update();
    }

    /** @brief 注册选择回调，避免内部展示类暴露到公共头文件的 Qt 信号元数据。 */
    void setSelectionCallback(std::function<void()> callback)
    {
        m_selectionCallback = std::move(callback);
    }

    /** @brief 注册分屏拖动三阶段回调（开始 / 移动 / 结束），参数为鼠标全局坐标。 */
    void setDockDragCallbacks(std::function<void()> begin,
                              std::function<void(const QPoint &)> move,
                              std::function<void(const QPoint &)> end)
    {
        m_dockDragBegin = std::move(begin);
        m_dockDragMove = std::move(move);
        m_dockDragEnd = std::move(end);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() != Qt::LeftButton) {
                break;
            }
            if (m_selectionCallback) {
                m_selectionCallback();
            }
            // 会话状态栏（非按钮区域）按下即进入“可能拖动”；正文/滚动条按下只做选中。
            const bool onHeaderHandle = isWithinSessionHeader(watched)
                && qobject_cast<QAbstractButton *>(watched) == nullptr;
            if (onHeaderHandle && m_dockDragBegin) {
                m_dockPressGlobal = mouseEvent->globalPosition().toPoint();
                m_dockPressed = true;
                m_dockDragging = false;
            }
            break;
        }
        case QEvent::MouseMove: {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (m_dockPressed && mouseEvent->buttons().testFlag(Qt::LeftButton)) {
                const QPoint global = mouseEvent->globalPosition().toPoint();
                if (!m_dockDragging
                    && (global - m_dockPressGlobal).manhattanLength()
                        > QApplication::startDragDistance()) {
                    m_dockDragging = true;
                    createDragGhost();
                    if (m_dockDragBegin) {
                        m_dockDragBegin();
                    }
                }
                if (m_dockDragging) {
                    moveDragGhost(global);
                    if (m_dockDragMove) {
                        m_dockDragMove(global);
                    }
                    return true;
                }
                break;
            }
            if (!m_hovered) {
                m_hovered = true;
                setProperty("workspaceHovered", true);
                update();
            }
            break;
        }
        case QEvent::Enter:
            if (!m_hovered) {
                m_hovered = true;
                setProperty("workspaceHovered", true);
                update();
            }
            break;
        case QEvent::MouseButtonRelease: {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton && m_dockPressed) {
                const bool wasDragging = m_dockDragging;
                const QPoint global = mouseEvent->globalPosition().toPoint();
                m_dockPressed = false;
                m_dockDragging = false;
                destroyDragGhost();
                if (wasDragging) {
                    if (m_dockDragEnd) {
                        m_dockDragEnd(global);
                    }
                    return true;
                }
            }
            break;
        }
        case QEvent::Leave: {
            // 子控件之间移动会连续产生 Leave/Enter。延迟到本轮事件结束后检查全局指针是否
            // 真的离开整个框，避免虚线在终端正文与滚动条之间闪烁。
            QPointer<WorkspaceTerminalFrame> guard(this);
            QTimer::singleShot(0, this, [guard]() {
                if (guard.isNull()) {
                    return;
                }
                const QPoint localPosition = guard->mapFromGlobal(QCursor::pos());
                const bool hovered = guard->rect().contains(localPosition);
                if (guard->m_hovered != hovered) {
                    guard->m_hovered = hovered;
                    guard->setProperty("workspaceHovered", hovered);
                    guard->update();
                }
            });
            break;
        }
        default:
            break;
        }
        return false;
    }

    void paintEvent(QPaintEvent *event) override
    {
        QFrame::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF cardRect = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
        constexpr qreal radius = 10.0;

        // 圆角卡片底：始终绘制，让每个终端都是一块独立的深色卡片而不是贴边裸终端。
        // 终端控件本身不透明，只有 6px 内衬和四个圆角会露出这层底色——因此必须取终端自己
        // 的背景色，浅色主题下的 QPalette::Base 会在深色终端四周留下突兀的白边。
        painter.setPen(Qt::NoPen);
        painter.setBrush(terminalChromeBackground(m_workspace));
        painter.drawRoundedRect(cardRect, radius, radius);

        // 三态边框：未选中淡实线、Hover 虚线、选中高对比实线；三者都画，避免切换时卡片
        // 尺寸/描边突然出现或消失造成的跳动。
        QColor borderColor;
        qreal borderWidth = 1.0;
        Qt::PenStyle borderStyle = Qt::SolidLine;
        if (m_selected) {
            borderColor = palette().color(QPalette::Highlight);
            borderColor.setAlpha(230);
            borderWidth = 2.0;
        } else if (m_hovered) {
            borderColor = palette().color(QPalette::Highlight);
            borderColor.setAlpha(165);
            borderWidth = 1.4;
            borderStyle = Qt::DashLine;
        } else {
            borderColor = palette().color(QPalette::Mid);
            borderColor.setAlpha(150);
        }
        painter.setPen(QPen(borderColor, borderWidth, borderStyle, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(cardRect, radius, radius);
    }

private:
    /** @brief 给当前终端子树安装只观察、不消费事件的过滤器。 */
    void installInputFilters()
    {
        m_workspace->installEventFilter(this);
        const QList<QWidget *> children = m_workspace->findChildren<QWidget *>();
        for (QWidget *child : children) {
            child->setMouseTracking(true);
            child->installEventFilter(this);
        }
    }

    /** @brief 创建跟随光标的浮动快照：无边框、鼠标穿透、不抢焦点的顶层控件。 */
    void createDragGhost()
    {
        if (!m_dragGhost.isNull()) {
            return;
        }
        const QPixmap shot = grab().scaled(width() / 2,
                                           height() / 2,
                                           Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
        auto *ghost = new QLabel(nullptr,
                                 Qt::ToolTip | Qt::FramelessWindowHint
                                     | Qt::WindowTransparentForInput
                                     | Qt::NoDropShadowWindowHint);
        ghost->setObjectName(QStringLiteral("WorkspaceDockDragGhost"));
        ghost->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        ghost->setAttribute(Qt::WA_ShowWithoutActivating, true);
        ghost->setAttribute(Qt::WA_TranslucentBackground, true);
        ghost->setWindowOpacity(0.85);
        ghost->setPixmap(shot);
        ghost->resize(shot.size());
        m_dragGhost = ghost;
    }

    void moveDragGhost(const QPoint &globalPosition)
    {
        if (m_dragGhost.isNull()) {
            return;
        }
        m_dragGhost->move(globalPosition.x() - m_dragGhost->width() / 2,
                          globalPosition.y() + 10);
        if (!m_dragGhost->isVisible()) {
            m_dragGhost->show();
            m_dragGhost->raise();
        }
    }

    void destroyDragGhost()
    {
        if (!m_dragGhost.isNull()) {
            m_dragGhost->deleteLater();
            m_dragGhost.clear();
        }
    }

    TerminalWorkspace *m_workspace = nullptr;
    std::function<void()> m_selectionCallback;
    std::function<void()> m_dockDragBegin;
    std::function<void(const QPoint &)> m_dockDragMove;
    std::function<void(const QPoint &)> m_dockDragEnd;
    QPointer<QWidget> m_dragGhost;
    QPoint m_dockPressGlobal;
    bool m_dockPressed = false;
    bool m_dockDragging = false;
    bool m_selected = false;
    bool m_hovered = false;
};

/**
 * @brief 分屏拖动时覆盖在工作空间内容区之上的落点高亮层。
 *
 * 平时隐藏、对鼠标透明；拖动期间由 TerminalWorkspaceGroup 显示并置顶，按命中的目标半区
 * 填一块半透明高亮，明确告诉用户松手后新分块会落在哪。
 */
class WorkspaceDockOverlay final : public QWidget {
public:
    explicit WorkspaceDockOverlay(QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("WorkspaceDockOverlay"));
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        hide();
    }

    /** @brief 设置当前高亮矩形（本控件局部坐标）；空矩形表示无有效落点。 */
    void setZone(const QRect &zone)
    {
        if (m_zone == zone) {
            return;
        }
        m_zone = zone;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (m_zone.isEmpty()) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor accent = palette().color(QPalette::Highlight);
        QColor fill = accent;
        fill.setAlpha(70);
        painter.setBrush(fill);
        accent.setAlpha(220);
        painter.setPen(QPen(accent, 2.0));
        painter.drawRoundedRect(QRectF(m_zone).adjusted(1.5, 1.5, -1.5, -1.5), 8.0, 8.0);
    }

private:
    QRect m_zone;
};

TerminalWorkspaceGroup::TerminalWorkspaceGroup(QWidget *parent)
    : QWidget(parent)
    , m_content(new QWidget(this))
{
    setObjectName(QStringLiteral("TerminalWorkspaceGroup"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_content);

    m_contentLayout = new QVBoxLayout(m_content);
    m_contentLayout->setContentsMargins(8, 8, 8, 8);
    m_contentLayout->setSpacing(0);
    m_splitRoot = makeSplitter(Qt::Horizontal);
    m_splitRoot->setObjectName(QStringLiteral("WorkspaceSplitRoot"));
    m_contentLayout->addWidget(m_splitRoot);

    m_dockOverlay = new WorkspaceDockOverlay(m_content);
}

QSplitter *TerminalWorkspaceGroup::makeSplitter(const Qt::Orientation orientation)
{
    auto *splitter = new QSplitter(orientation, m_content);
    splitter->setObjectName(QStringLiteral("WorkspaceSplitNode"));
    splitter->setChildrenCollapsible(false);
    splitter->setOpaqueResize(true);
    splitter->setHandleWidth(8);
    return splitter;
}

void TerminalWorkspaceGroup::equalizeSplitter(QSplitter *splitter)
{
    if (splitter == nullptr) {
        return;
    }
    const int count = splitter->count();
    if (count <= 1) {
        return;
    }
    const int extent = splitter->orientation() == Qt::Horizontal ? splitter->width()
                                                                 : splitter->height();
    const int each = std::max(1, extent / count);
    QList<int> sizes;
    sizes.reserve(count);
    for (int index = 0; index < count; ++index) {
        sizes.append(each);
    }
    splitter->setSizes(sizes);
}

void TerminalWorkspaceGroup::pruneSplitter(QSplitter *splitter)
{
    if (splitter == nullptr || splitter == m_splitRoot) {
        return;
    }
    auto *grandParent = qobject_cast<QSplitter *>(splitter->parentWidget());
    if (splitter->count() == 0) {
        splitter->setParent(nullptr);
        splitter->deleteLater();
        pruneSplitter(grandParent);
        return;
    }
    if (splitter->count() == 1 && grandParent != nullptr) {
        QWidget *only = splitter->widget(0);
        const int index = grandParent->indexOf(splitter);
        const QList<int> sizes = grandParent->sizes();
        grandParent->insertWidget(index, only); // reparents `only` out of `splitter`
        splitter->setParent(nullptr);
        splitter->deleteLater();
        grandParent->setSizes(sizes);
        pruneSplitter(grandParent);
    }
}

void TerminalWorkspaceGroup::addTerminal(TerminalWorkspace *workspace, const QString &title)
{
    if (workspace == nullptr || contains(workspace)) {
        return;
    }
    auto *frame = new WorkspaceTerminalFrame(workspace, m_content);
    frame->setSelectionCallback([this, workspace]() { setActiveTerminal(workspace); });
    frame->setDockDragCallbacks(
        [this, workspace]() { beginDockDrag(workspace); },
        [this](const QPoint &global) { updateDockDrag(global); },
        [this](const QPoint &global) { endDockDrag(global); });
    // 从 QTabWidget 移出的非当前页会带着显式 hidden 状态。仅重设父对象不会自动清除它，
    // 结果是新拖入的终端虽已在模型中、树里却是空白。重挂完成后显式 show()；切换模式仍会
    // 由 refreshSelection() 按活动成员隐藏整个 frame。
    workspace->show();
    m_terminals.append(workspace);
    m_frames.insert(workspace, frame);
    m_titles.insert(workspace, title.trimmed().isEmpty() ? tr("Terminal") : title.trimmed());
    if (m_activeTerminal == nullptr) {
        m_activeTerminal = workspace;
    }
    m_splitRoot->addWidget(frame);
    equalizeSplitter(m_splitRoot);
    installMemberControls(workspace);
    connect(workspace->activePane(), &TerminalPane::broadcastSourceInput,
            this, &TerminalWorkspaceGroup::relayBroadcastInput, Qt::UniqueConnection);
    rebuildLayout();
    emit terminalsChanged();
}

void TerminalWorkspaceGroup::installMemberControls(TerminalWorkspace *workspace)
{
    TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
    if (pane == nullptr) {
        return;
    }
    QWidget *slot = pane->headerExtrasContainer();
    auto *slotLayout = qobject_cast<QHBoxLayout *>(slot->layout());
    if (slotLayout == nullptr) {
        return;
    }
    // 图标线条色与文件管理按钮完全一致（随终端主题走），读不到时回退浅灰。
    QColor iconColor = pane->headerIconColor();
    if (!iconColor.isValid()) {
        iconColor = QColor(QStringLiteral("#e6e6e6"));
    }
    // 无外边框：悬停只轻微提亮底色，选中（广播开启）填一块浅色圆角底，和参考图一致。
    const QString buttonStyle = UiIcons::sessionActionStyleSheet();
    const auto makeButton = [&](const UiIcons::Symbol symbol,
                                const QString &objectName,
                                const QString &tooltip,
                                const bool checkable) {
        auto *button = new QToolButton(slot);
        button->setObjectName(objectName);
        button->setAutoRaise(true);
        button->setCheckable(checkable);
        button->setIconSize(QSize(16, 16));
        button->setIcon(UiIcons::symbol(slot, symbol, iconColor));
        button->setToolTip(tooltip);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(buttonStyle);
        slotLayout->addWidget(button);
        return button;
    };

    auto *broadcastButton = makeButton(
        UiIcons::Symbol::Broadcast,
        QStringLiteral("workspaceBroadcastToggle"),
        tr("Broadcast input to every terminal in this workspace"),
        true);
    {
        const QSignalBlocker blocker(broadcastButton);
        broadcastButton->setChecked(m_broadcastEnabled);
    }
    connect(broadcastButton, &QToolButton::toggled, this,
            [this](const bool on) { setBroadcastEnabled(on); });
    m_broadcastButtons.append(broadcastButton);

    auto *modeButton = makeButton(
        UiIcons::Symbol::Workspace,
        QStringLiteral("workspaceModeToggle"),
        tr("Toggle between tiled split view and one-terminal-at-a-time view"),
        false);
    connect(modeButton, &QToolButton::clicked, this, [this]() {
        setLayoutMode(m_layoutMode == LayoutMode::Tiled ? LayoutMode::Switch
                                                        : LayoutMode::Tiled);
    });

    auto *closeButton = makeButton(
        UiIcons::Symbol::Close,
        QStringLiteral("workspaceMemberClose"),
        tr("Close this terminal"),
        false);
    connect(closeButton, &QToolButton::clicked, workspace,
            [workspace]() { workspace->requestClose(); });
}

bool TerminalWorkspaceGroup::isBroadcastEnabled() const
{
    return m_broadcastEnabled;
}

void TerminalWorkspaceGroup::setBroadcastEnabled(const bool enabled)
{
    if (m_broadcastEnabled == enabled) {
        return;
    }
    m_broadcastEnabled = enabled;
    for (QToolButton *button : std::as_const(m_broadcastButtons)) {
        if (button == nullptr) {
            continue;
        }
        const QSignalBlocker blocker(button);
        button->setChecked(enabled);
    }
    emit broadcastEnabledChanged(enabled);
}

void TerminalWorkspaceGroup::relayBroadcastInput(TerminalPane *source, const QByteArray &bytes)
{
    if (!m_broadcastEnabled || source == nullptr || bytes.isEmpty()) {
        return;
    }
    for (TerminalWorkspace *workspace : std::as_const(m_terminals)) {
        TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
        if (pane != nullptr && pane != source) {
            // sendInputBytes 不再次发出 broadcastSourceInput，因此不会 A→B→A 回环。
            pane->sendInputBytes(bytes);
        }
    }
}

bool TerminalWorkspaceGroup::takeTerminal(TerminalWorkspace *workspace)
{
    const int index = m_terminals.indexOf(workspace);
    if (index < 0) {
        return false;
    }
    WorkspaceTerminalFrame *frame = m_frames.take(workspace);
    auto *oldParent = qobject_cast<QSplitter *>(frame->parentWidget());
    // 终端会以独立标签的形式继续存在：必须撤掉工作空间挂上的标题栏按钮与广播接线，
    // 否则一个已离开工作空间的终端仍会把输入广播进来、标题栏也残留三个按钮。
    if (TerminalPane *pane = workspace->activePane()) {
        disconnect(pane, &TerminalPane::broadcastSourceInput, this, nullptr);
        m_broadcastButtons.removeIf([pane](QToolButton *button) {
            return button == nullptr || pane->isAncestorOf(button);
        });
        pane->clearHeaderExtras();
    }
    // 回到独立标签：恢复远程文件管理入口与标准最小宽度（分屏时被放宽到 180）。
    workspace->setFileManagerAllowed(true);
    // 先把终端还给调用方，再销毁承载它的 frame；顺序反了会把仍在运行的会话一起删掉。
    workspace->setParent(this);
    workspace->show();
    frame->setParent(nullptr);
    frame->deleteLater();
    pruneSplitter(oldParent);
    m_terminals.removeAt(index);
    m_titles.remove(workspace);
    if (m_activeTerminal == workspace) {
        m_activeTerminal = m_terminals.isEmpty()
            ? nullptr
            : m_terminals.at(std::min<qsizetype>(index, m_terminals.size() - 1));
        emit activeTerminalChanged(m_activeTerminal);
    }
    rebuildLayout();
    emit terminalsChanged();
    return true;
}

void TerminalWorkspaceGroup::dockTerminal(TerminalWorkspace *source,
                                          TerminalWorkspace *target,
                                          const DockEdge edge)
{
    WorkspaceTerminalFrame *sourceFrame = m_frames.value(source);
    WorkspaceTerminalFrame *targetFrame = target == nullptr ? nullptr : m_frames.value(target);
    if (sourceFrame == nullptr || sourceFrame == targetFrame) {
        return;
    }

    // 1. 从当前父节点摘除，随后收拾旧父。
    auto *oldParent = qobject_cast<QSplitter *>(sourceFrame->parentWidget());
    sourceFrame->setParent(nullptr);

    const bool wantHorizontal = edge == DockEdge::Left || edge == DockEdge::Right;
    const Qt::Orientation want = wantHorizontal ? Qt::Horizontal : Qt::Vertical;
    const bool sourceBefore = edge == DockEdge::Left || edge == DockEdge::Top;

    if (targetFrame == nullptr) {
        // 停靠到最外层：始终作用在根节点上，根节点本身永不被替换，只在需要反向时向内嵌套。
        if (m_splitRoot->count() <= 1) {
            m_splitRoot->setOrientation(want);
            m_splitRoot->insertWidget(sourceBefore ? 0 : m_splitRoot->count(), sourceFrame);
        } else if (m_splitRoot->orientation() == want) {
            m_splitRoot->insertWidget(sourceBefore ? 0 : m_splitRoot->count(), sourceFrame);
        } else {
            QSplitter *inner = makeSplitter(m_splitRoot->orientation());
            while (m_splitRoot->count() > 0) {
                inner->addWidget(m_splitRoot->widget(0));
            }
            m_splitRoot->setOrientation(want);
            m_splitRoot->addWidget(inner);
            m_splitRoot->insertWidget(sourceBefore ? 0 : 1, sourceFrame);
            equalizeSplitter(inner);
        }
        equalizeSplitter(m_splitRoot);
    } else {
        auto *parent = qobject_cast<QSplitter *>(targetFrame->parentWidget());
        if (parent == nullptr) {
            // 理论上不会发生：每个 frame 都挂在某个 QSplitter 下。安全回退到根。
            m_splitRoot->addWidget(sourceFrame);
            equalizeSplitter(m_splitRoot);
        } else if (parent->orientation() == want) {
            const int targetIndex = parent->indexOf(targetFrame);
            parent->insertWidget(sourceBefore ? targetIndex : targetIndex + 1, sourceFrame);
            equalizeSplitter(parent);
        } else {
            const int targetIndex = parent->indexOf(targetFrame);
            const QList<int> parentSizes = parent->sizes();
            QSplitter *nested = makeSplitter(want);
            parent->insertWidget(targetIndex, nested);
            nested->addWidget(targetFrame); // reparents targetFrame out of `parent`
            nested->insertWidget(sourceBefore ? 0 : 1, sourceFrame);
            if (parentSizes.size() == parent->count()) {
                parent->setSizes(parentSizes);
            }
            equalizeSplitter(nested);
        }
    }

    pruneSplitter(oldParent);
    sourceFrame->show();
    setActiveTerminal(source);
    rebuildLayout();
    emit terminalsChanged();
}

QList<TerminalWorkspace *> TerminalWorkspaceGroup::terminals() const
{
    return m_terminals;
}

bool TerminalWorkspaceGroup::contains(TerminalWorkspace *workspace) const
{
    return workspace != nullptr && m_frames.contains(workspace);
}

TerminalWorkspace *TerminalWorkspaceGroup::activeTerminal() const
{
    return m_activeTerminal;
}

void TerminalWorkspaceGroup::setActiveTerminal(TerminalWorkspace *workspace)
{
    if (!contains(workspace) || m_activeTerminal == workspace) {
        return;
    }
    m_activeTerminal = workspace;
    refreshSelection();
    emit activeTerminalChanged(workspace);
}

QString TerminalWorkspaceGroup::terminalTitle(TerminalWorkspace *workspace) const
{
    return m_titles.value(workspace);
}

TerminalWorkspaceGroup::LayoutMode TerminalWorkspaceGroup::layoutMode() const
{
    return m_layoutMode;
}

void TerminalWorkspaceGroup::setLayoutMode(const LayoutMode mode)
{
    if (m_layoutMode == mode) {
        return;
    }
    m_layoutMode = mode;
    rebuildLayout();
    emit layoutModeChanged(mode);
}

void TerminalWorkspaceGroup::rebuildLayout()
{
    // 容器与内容区跟随终端会话背景一起变深，让分块之间的空隙、外边距和标题栏融为一体，
    // 而不是露出应用在浅色主题下的窗口色。
    const QColor chrome = terminalChromeBackground(m_terminals.value(0));
    for (QWidget *surface : {static_cast<QWidget *>(this), m_content}) {
        QPalette surfacePalette = surface->palette();
        surfacePalette.setColor(QPalette::Window, chrome);
        surface->setPalette(surfacePalette);
        surface->setAutoFillBackground(true);
    }

    restyleSplitterHandles(chrome);

    const bool switchMode = m_layoutMode == LayoutMode::Switch;
    for (TerminalWorkspace *workspace : std::as_const(m_terminals)) {
        // 分屏模式禁用远程文件管理（多块并列时目标歧义、单元格尺寸被破坏）；切换模式恢复。
        workspace->setFileManagerAllowed(switchMode);
    }
    refreshSelection();
}

void TerminalWorkspaceGroup::restyleSplitterHandles(const QColor &chromeColor)
{
    // QSplitter 分隔条默认由样式引擎绘制成一条浅色细线；分屏里那是终端之间唯一的浅色噪点。
    // 直接把 handle 背景刷成终端会话底色，分隔条就“消失”成一条同色缝隙。
    const QString handleStyle =
        QStringLiteral("QSplitter::handle { background-color: %1; }")
            .arg(chromeColor.name(QColor::HexRgb));
    const QList<QSplitter *> splitters = findChildren<QSplitter *>();
    for (QSplitter *splitter : splitters) {
        const QString name = splitter->objectName();
        if (name == QLatin1String("WorkspaceSplitRoot")
            || name == QLatin1String("WorkspaceSplitNode")) {
            splitter->setStyleSheet(handleStyle);
        }
    }
}

void TerminalWorkspaceGroup::beginExternalDropHint()
{
    m_externalHit = {};
    m_externalDropActive = true;
    m_dockDragSource = nullptr;
    m_dockOverlay->setZone({});
    m_dockOverlay->setGeometry(m_content->rect());
    m_dockOverlay->show();
    m_dockOverlay->raise();
}

void TerminalWorkspaceGroup::updateExternalDropHint(const QPoint &globalPosition)
{
    if (!m_externalDropActive) {
        return;
    }
    m_externalHit = hitTestDock(globalPosition);
    m_dockOverlay->setGeometry(m_content->rect());
    m_dockOverlay->raise();
    m_dockOverlay->setZone(m_externalHit.valid ? m_externalHit.zoneInContent : QRect());
}

void TerminalWorkspaceGroup::endExternalDropHint()
{
    // 只收起高亮，保留 m_externalHit 供随后的合并读取（consumeExternalDropTarget 会清空）。
    m_externalDropActive = false;
    m_dockOverlay->setZone({});
    m_dockOverlay->hide();
}

bool TerminalWorkspaceGroup::consumeExternalDropTarget(TerminalWorkspace **target, DockEdge *edge)
{
    const DockTargetHit hit = m_externalHit;
    m_externalHit = {};
    if (!hit.valid) {
        return false;
    }
    if (target != nullptr) {
        *target = hit.targetFrame == nullptr ? nullptr : hit.targetFrame->workspace();
    }
    if (edge != nullptr) {
        *edge = hit.edge;
    }
    return true;
}

void TerminalWorkspaceGroup::refreshSelection()
{
    const bool switchMode = m_layoutMode == LayoutMode::Switch;
    for (TerminalWorkspace *workspace : std::as_const(m_terminals)) {
        WorkspaceTerminalFrame *frame = m_frames.value(workspace);
        if (frame == nullptr) {
            continue;
        }
        frame->setSelected(workspace == m_activeTerminal);
        // 切换模式：QSplitter 对隐藏子控件自动收起 handle 并把空间给唯一可见项，
        // 无需重新 reparent，切回分屏只是再次全部 setVisible(true)。
        frame->setVisible(!switchMode || workspace == m_activeTerminal);
    }
}

TerminalWorkspaceGroup::DockTargetHit
TerminalWorkspaceGroup::hitTestDock(const QPoint &globalPosition) const
{
    DockTargetHit hit;
    if (m_content == nullptr) {
        return hit;
    }
    const QPoint contentPoint = m_content->mapFromGlobal(globalPosition);
    const QRect contentRect = m_content->rect();
    if (!contentRect.contains(contentPoint)) {
        return hit;
    }

    // 靠近内容区边缘的窄带优先解释为“停靠到最外层”。
    constexpr int kOuterBand = 28;
    if (contentPoint.x() <= contentRect.left() + kOuterBand) {
        hit = {nullptr, DockEdge::Left, QRect(contentRect.topLeft(),
                                              QSize(contentRect.width() / 2, contentRect.height())),
               true};
        return hit;
    }
    if (contentPoint.x() >= contentRect.right() - kOuterBand) {
        hit = {nullptr, DockEdge::Right,
               QRect(contentRect.left() + contentRect.width() / 2, contentRect.top(),
                     contentRect.width() / 2, contentRect.height()),
               true};
        return hit;
    }
    if (contentPoint.y() <= contentRect.top() + kOuterBand) {
        hit = {nullptr, DockEdge::Top,
               QRect(contentRect.topLeft(), QSize(contentRect.width(), contentRect.height() / 2)),
               true};
        return hit;
    }
    if (contentPoint.y() >= contentRect.bottom() - kOuterBand) {
        hit = {nullptr, DockEdge::Bottom,
               QRect(contentRect.left(), contentRect.top() + contentRect.height() / 2,
                     contentRect.width(), contentRect.height() / 2),
               true};
        return hit;
    }

    // 否则命中光标所在的可见成员，按象限判定边。
    for (TerminalWorkspace *workspace : m_terminals) {
        WorkspaceTerminalFrame *frame = m_frames.value(workspace);
        if (frame == nullptr || !frame->isVisible() || workspace == m_dockDragSource) {
            continue;
        }
        const QPoint framePoint = frame->mapFromGlobal(globalPosition);
        if (!frame->rect().contains(framePoint)) {
            continue;
        }
        const qreal nx = static_cast<qreal>(framePoint.x()) / std::max(1, frame->width()) - 0.5;
        const qreal ny = static_cast<qreal>(framePoint.y()) / std::max(1, frame->height()) - 0.5;
        DockEdge edge = DockEdge::Right;
        if (std::abs(nx) >= std::abs(ny)) {
            edge = nx < 0 ? DockEdge::Left : DockEdge::Right;
        } else {
            edge = ny < 0 ? DockEdge::Top : DockEdge::Bottom;
        }

        const QRect frameInContent(frame->mapTo(m_content, QPoint(0, 0)), frame->size());
        QRect zone = frameInContent;
        switch (edge) {
        case DockEdge::Left:
            zone.setWidth(frameInContent.width() / 2);
            break;
        case DockEdge::Right:
            zone.setLeft(frameInContent.left() + frameInContent.width() / 2);
            break;
        case DockEdge::Top:
            zone.setHeight(frameInContent.height() / 2);
            break;
        case DockEdge::Bottom:
            zone.setTop(frameInContent.top() + frameInContent.height() / 2);
            break;
        }
        hit = {frame, edge, zone, true};
        return hit;
    }
    return hit;
}

void TerminalWorkspaceGroup::beginDockDrag(TerminalWorkspace *source)
{
    if (m_layoutMode != LayoutMode::Tiled || !contains(source)) {
        return;
    }
    m_dockDragSource = source;
    m_dockOverlay->setZone({});
    m_dockOverlay->setGeometry(m_content->rect());
    m_dockOverlay->show();
    m_dockOverlay->raise();
}

void TerminalWorkspaceGroup::updateDockDrag(const QPoint &globalPosition)
{
    if (m_dockDragSource == nullptr) {
        return;
    }
    const DockTargetHit hit = hitTestDock(globalPosition);
    m_dockOverlay->setZone(hit.valid ? hit.zoneInContent : QRect());
}

void TerminalWorkspaceGroup::endDockDrag(const QPoint &globalPosition)
{
    TerminalWorkspace *source = m_dockDragSource;
    m_dockDragSource = nullptr;
    m_dockOverlay->setZone({});
    m_dockOverlay->hide();
    if (source == nullptr) {
        return;
    }
    const DockTargetHit hit = hitTestDock(globalPosition);
    if (!hit.valid) {
        return;
    }
    TerminalWorkspace *target =
        hit.targetFrame == nullptr ? nullptr : hit.targetFrame->workspace();
    if (target == source) {
        return;
    }
    dockTerminal(source, target, hit.edge);
}

namespace {

/** @brief 侧栏终端行的自定义数据角色。 */
constexpr int kRowSubtitleRole = Qt::UserRole + 1;
constexpr int kRowDistroRole = Qt::UserRole + 2;
constexpr int kRowStateRole = Qt::UserRole + 3;

/** @brief 连接状态点颜色，语义与 SessionTabBar::ConnectionState 一致；Unknown 不画点。 */
[[nodiscard]] QColor connectionDotColor(const int state)
{
    switch (state) {
    case 1: // Connecting
        return QColor(QStringLiteral("#f59e0b"));
    case 2: // Connected
        return QColor(QStringLiteral("#22c55e"));
    case 3: // Failed
        return QColor(QStringLiteral("#ef4444"));
    default:
        return {};
    }
}

/**
 * @brief Termius 风格的工作空间终端行委托。
 *
 * 每行左侧发行版 Logo、主标题（同名多开带序号）、"ssh, user" 次级行，右侧连接图标与状态点；
 * 选中行圆角实心高亮，悬停行浅色高亮且左侧图标处浮出关闭按钮。关闭点击通过 onCloseClicked
 * 回调上报，样式与文件内其他匿名类保持“注入回调而非信号”的一致做法。
 */
class WorkspaceTerminalRowDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    std::function<void(int)> onCloseClicked;

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem & /*option*/,
                                 const QModelIndex & /*index*/) const override
    {
        return {0, 54};
    }

    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
        const QRect card = option.rect.adjusted(6, 3, -6, -3);
        if (selected) {
            QColor fill = option.palette.color(QPalette::Highlight);
            fill.setAlpha(64);
            painter->setPen(Qt::NoPen);
            painter->setBrush(fill);
            painter->drawRoundedRect(card, 10, 10);
        } else if (hovered) {
            QColor fill = option.palette.color(QPalette::Text);
            fill.setAlpha(20);
            painter->setPen(Qt::NoPen);
            painter->setBrush(fill);
            painter->drawRoundedRect(card, 10, 10);
        }

        const QWidget *host = option.widget;
        const QColor textColor = option.palette.color(QPalette::Text);
        QColor mutedColor = textColor;
        mutedColor.setAlpha(140);
        const int centerY = option.rect.center().y();
        const QRect glyphRect(option.rect.left() + 14, centerY - 10, 20, 20);

        if (hovered && host != nullptr) {
            UiIcons::symbol(host, UiIcons::Symbol::Close, textColor, QSize(18, 18))
                .paint(painter, glyphRect, Qt::AlignCenter);
        } else {
            QIcon distroIcon =
                HostDistributionIcon::iconFor(index.data(kRowDistroRole).toString());
            if (distroIcon.isNull() && host != nullptr) {
                distroIcon =
                    UiIcons::symbol(host, UiIcons::Symbol::HostItem, textColor, QSize(18, 18));
            }
            distroIcon.paint(painter, glyphRect, Qt::AlignCenter);
        }

        const int textLeft = option.rect.left() + 44;
        const int textRight = option.rect.right() - 40;
        const int textWidth = std::max(0, textRight - textLeft);
        const QString subtitle = index.data(kRowSubtitleRole).toString();

        QFont titleFont = option.font;
        titleFont.setPixelSize(13);
        titleFont.setWeight(QFont::DemiBold);
        painter->setFont(titleFont);
        painter->setPen(textColor);
        const QFontMetrics titleMetrics(titleFont);
        const QString title = titleMetrics.elidedText(
            index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle, textWidth);
        const QRect titleRect(textLeft,
                              subtitle.isEmpty() ? option.rect.top() : option.rect.top() + 8,
                              textWidth,
                              subtitle.isEmpty() ? option.rect.height() : 22);
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, title);

        if (!subtitle.isEmpty()) {
            QFont subFont = option.font;
            subFont.setPixelSize(11);
            painter->setFont(subFont);
            painter->setPen(mutedColor);
            const QFontMetrics subMetrics(subFont);
            const QString subText =
                subMetrics.elidedText(subtitle, Qt::ElideRight, textWidth);
            painter->drawText(QRect(textLeft, centerY, textWidth, 20),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              subText);
        }

        if (host != nullptr) {
            UiIcons::symbol(host, UiIcons::Symbol::Connect, mutedColor, QSize(15, 15))
                .paint(painter,
                       QRect(option.rect.right() - 34, centerY - 8, 16, 16),
                       Qt::AlignCenter);
        }
        const QColor dot = connectionDotColor(index.data(kRowStateRole).toInt());
        if (dot.isValid()) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(dot);
            painter->drawEllipse(QPointF(option.rect.right() - 15, centerY), 3.5, 3.5);
        }

        painter->restore();
    }

protected:
    bool editorEvent(QEvent *event,
                     QAbstractItemModel * /*model*/,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        if (event->type() != QEvent::MouseButtonPress
            && event->type() != QEvent::MouseButtonRelease) {
            return false;
        }
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() != Qt::LeftButton) {
            return false;
        }
        const int centerY = option.rect.center().y();
        const QRect closeHit(option.rect.left() + 8, centerY - 14, 32, 28);
        if (!closeHit.contains(mouseEvent->pos())) {
            return false;
        }
        // 命中关闭区域时吞掉按下事件，避免顺带切换选中行；释放时才真正触发关闭。
        if (event->type() == QEvent::MouseButtonRelease && onCloseClicked) {
            onCloseClicked(index.row());
        }
        return true;
    }
};

} // namespace

WorkspaceSidebarWidget::WorkspaceSidebarWidget(QWidget *parent)
    : QWidget(parent)
    , m_title(new QLabel(this))
    , m_layoutToggle(new QToolButton(this))
    , m_broadcastToggle(new QToolButton(this))
    , m_headerRow(new QWidget(this))
    , m_terminalList(new QListWidget(this))
    , m_emptyHint(new QLabel(this))
{
    setObjectName(QStringLiteral("WorkspaceSidebar"));
    m_title->setObjectName(QStringLiteral("WorkspaceSidebarTitle"));

    const QColor windowText = palette().color(QPalette::WindowText);
    for (QToolButton *button : {m_layoutToggle, m_broadcastToggle}) {
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Qt::PointingHandCursor);
        button->setProperty("compactIconButton", true);
    }
    m_layoutToggle->setObjectName(QStringLiteral("WorkspaceLayoutToggle"));
    m_broadcastToggle->setObjectName(QStringLiteral("WorkspaceBroadcastToggle"));
    UiIcons::decorateButton(
        m_layoutToggle,
        UiIcons::symbol(this, UiIcons::Symbol::Workspace, windowText, QSize(16, 16)));
    UiIcons::decorateButton(
        m_broadcastToggle,
        UiIcons::symbol(this, UiIcons::Symbol::Broadcast, windowText, QSize(16, 16)));

    m_terminalList->setObjectName(QStringLiteral("WorkspaceTerminalThumbnails"));
    m_terminalList->setFrameShape(QFrame::NoFrame);
    m_terminalList->setMouseTracking(true);
    m_terminalList->setUniformItemSizes(true);
    m_terminalList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_terminalList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_terminalList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    auto *delegate = new WorkspaceTerminalRowDelegate(m_terminalList);
    delegate->onCloseClicked = [this](const int row) {
        if (m_workspace != nullptr && row >= 0 && row < m_workspace->terminals().size()) {
            emit closeTerminalRequested(m_workspace->terminals().at(row));
        }
    };
    m_terminalList->setItemDelegate(delegate);

    m_emptyHint->setObjectName(QStringLiteral("WorkspaceSidebarEmptyHint"));
    m_emptyHint->setWordWrap(true);
    m_emptyHint->setAlignment(Qt::AlignCenter);

    auto *headerLayout = new QHBoxLayout(m_headerRow);
    headerLayout->setContentsMargins(2, 0, 0, 0);
    headerLayout->setSpacing(2);
    headerLayout->addWidget(m_title);
    headerLayout->addStretch(1);
    headerLayout->addWidget(m_layoutToggle);
    headerLayout->addWidget(m_broadcastToggle);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(m_headerRow);
    layout->addWidget(m_terminalList, 1);
    layout->addWidget(m_emptyHint, 1);
    retranslateUi();
    setWorkspace(nullptr);

    connect(m_layoutToggle, &QToolButton::toggled, this, [this](const bool checked) {
        if (m_workspace != nullptr) {
            m_workspace->setLayoutMode(checked ? TerminalWorkspaceGroup::LayoutMode::Switch
                                               : TerminalWorkspaceGroup::LayoutMode::Tiled);
        }
        retranslateUi();
    });
    connect(m_broadcastToggle, &QToolButton::toggled, this, [this](const bool checked) {
        if (m_workspace != nullptr) {
            m_workspace->setBroadcastEnabled(checked);
        }
    });
    connect(m_terminalList,
            &QListWidget::currentRowChanged,
            this,
            [this](const int row) {
                if (m_workspace == nullptr || row < 0 || row >= m_workspace->terminals().size()) {
                    return;
                }
                m_workspace->setActiveTerminal(m_workspace->terminals().at(row));
            });
}

void WorkspaceSidebarWidget::setWorkspace(TerminalWorkspaceGroup *workspace)
{
    if (m_workspace == workspace) {
        refreshTerminals();
        return;
    }
    if (m_workspace != nullptr) {
        disconnect(m_workspace, nullptr, this, nullptr);
    }
    m_workspace = workspace;
    m_rows.clear();
    if (m_workspace != nullptr) {
        connect(m_workspace,
                &TerminalWorkspaceGroup::terminalsChanged,
                this,
                &WorkspaceSidebarWidget::refreshTerminals);
        connect(m_workspace,
                &TerminalWorkspaceGroup::activeTerminalChanged,
                this,
                [this](TerminalWorkspace *) { refreshTerminals(); });
        connect(m_workspace,
                &TerminalWorkspaceGroup::layoutModeChanged,
                this,
                [this](TerminalWorkspaceGroup::LayoutMode) { refreshTerminals(); });
        connect(m_workspace,
                &TerminalWorkspaceGroup::broadcastEnabledChanged,
                this,
                [this](bool) { refreshTerminals(); });
    }
    refreshTerminals();
}

void WorkspaceSidebarWidget::setMemberRows(const QList<WorkspaceSidebarRow> &rows)
{
    m_rows = rows;
    refreshTerminals();
}

void WorkspaceSidebarWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
        refreshTerminals();
    }
}

void WorkspaceSidebarWidget::retranslateUi()
{
    const int count = m_workspace == nullptr ? 0 : m_workspace->terminals().size();
    m_title->setText(count > 0 ? QStringLiteral("%1 · %2").arg(tr("Terminals")).arg(count)
                               : tr("Terminals"));
    // 布局按钮 tooltip 表达“当前处于哪种视图”，点一下切到另一种。
    m_layoutToggle->setToolTip(m_layoutToggle->isChecked() ? tr("Switch view") : tr("Tiled view"));
    m_broadcastToggle->setToolTip(tr("Broadcast input to all terminals"));
    m_emptyHint->setText(tr("Drag a terminal tab into the current terminal page or this sidebar to create or extend a workspace."));
}

void WorkspaceSidebarWidget::refreshTerminals()
{
    const QSignalBlocker listBlocker(m_terminalList);
    m_terminalList->clear();
    const bool hasWorkspace = m_workspace != nullptr;
    m_layoutToggle->setEnabled(hasWorkspace);
    m_broadcastToggle->setEnabled(hasWorkspace);
    m_terminalList->setVisible(hasWorkspace);
    m_emptyHint->setVisible(!hasWorkspace);
    if (!hasWorkspace) {
        retranslateUi();
        return;
    }

    {
        const QSignalBlocker toggleBlocker(m_layoutToggle);
        m_layoutToggle->setChecked(m_workspace->layoutMode()
                                   == TerminalWorkspaceGroup::LayoutMode::Switch);
    }
    {
        const QSignalBlocker broadcastBlocker(m_broadcastToggle);
        m_broadcastToggle->setChecked(m_workspace->isBroadcastEnabled());
    }
    retranslateUi();

    const QList<TerminalWorkspace *> terminals = m_workspace->terminals();
    const bool useRows = m_rows.size() == terminals.size();

    // 先统计每个标题出现的总次数：同一主机开了多个终端时，所有同名行都带序号（与 Termius 一致）。
    QStringList titles;
    titles.reserve(terminals.size());
    QHash<QString, int> totals;
    for (int index = 0; index < terminals.size(); ++index) {
        QString title = useRows && !m_rows.at(index).title.isEmpty()
            ? m_rows.at(index).title
            : m_workspace->terminalTitle(terminals.at(index));
        titles.append(title);
        totals[title] += 1;
    }

    QHash<QString, int> running;
    for (int index = 0; index < terminals.size(); ++index) {
        TerminalWorkspace *workspace = terminals.at(index);
        const QString &title = titles.at(index);
        const int ordinal = ++running[title];
        QString display = title;
        if (totals.value(title) > 1) {
            display += QStringLiteral(" (") + QString::number(ordinal) + QStringLiteral(")");
        }

        auto *item = new QListWidgetItem(std::move(display), m_terminalList);
        item->setData(kRowSubtitleRole, useRows ? m_rows.at(index).subtitle : QString());
        item->setData(kRowDistroRole, useRows ? m_rows.at(index).distributionId : QString());
        item->setData(kRowStateRole, useRows ? m_rows.at(index).connectionState : 0);
        if (workspace == m_workspace->activeTerminal()) {
            m_terminalList->setCurrentRow(index);
        }
    }
}
