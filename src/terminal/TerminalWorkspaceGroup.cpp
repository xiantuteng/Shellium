#include "terminal/TerminalWorkspaceGroup.h"

#include "terminal/TerminalWorkspace.h"

#include <QComboBox>
#include <QCursor>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

/**
 * @brief 为工作空间中的单个终端绘制选中、悬停和未选中三态边框。
 *
 * 未选中态不绘制任何边框；悬停态绘制虚线圆角矩形；选中态绘制实线圆角矩形。事件过滤
 * 安装到终端的全部既有子控件上，因此点击终端正文、滚动条或会话标题都能选中同一成员。
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
        // 边框绘制在控件内部，预留 4px 可避免实线或虚线覆盖终端文字和滚动条命中区域。
        layout->setContentsMargins(4, 4, 4, 4);
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

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        Q_UNUSED(watched)
        if (event->type() == QEvent::MouseButtonPress) {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton && m_selectionCallback) {
                m_selectionCallback();
            }
        } else if (event->type() == QEvent::Enter || event->type() == QEvent::MouseMove) {
            if (!m_hovered) {
                m_hovered = true;
                setProperty("workspaceHovered", true);
                update();
            }
        } else if (event->type() == QEvent::Leave) {
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
        }
        return false;
    }

    void paintEvent(QPaintEvent *event) override
    {
        QFrame::paintEvent(event);
        if (!m_selected && !m_hovered) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor color = palette().color(QPalette::Highlight);
        color.setAlpha(m_selected ? 230 : 190);
        painter.setPen(QPen(color,
                            m_selected ? 2.0 : 1.5,
                            m_selected ? Qt::SolidLine : Qt::DashLine,
                            Qt::RoundCap,
                            Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5), 9.0, 9.0);
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

    TerminalWorkspace *m_workspace = nullptr;
    std::function<void()> m_selectionCallback;
    bool m_selected = false;
    bool m_hovered = false;
};

TerminalWorkspaceGroup::TerminalWorkspaceGroup(QWidget *parent)
    : QWidget(parent)
    , m_content(new QWidget(this))
    , m_grid(new QGridLayout(m_content))
{
    setObjectName(QStringLiteral("TerminalWorkspaceGroup"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_content);
    m_grid->setContentsMargins(4, 4, 4, 4);
    m_grid->setHorizontalSpacing(4);
    m_grid->setVerticalSpacing(4);
}

void TerminalWorkspaceGroup::addTerminal(TerminalWorkspace *workspace, const QString &title)
{
    if (workspace == nullptr || contains(workspace)) {
        return;
    }
    auto *frame = new WorkspaceTerminalFrame(workspace, m_content);
    frame->setSelectionCallback([this, workspace]() { setActiveTerminal(workspace); });
    // 从 QTabWidget 移出的非当前页会带着显式 hidden 状态。仅重设父对象并加入布局不会
    // 自动清除该状态，结果是新拖入的终端虽然已在模型中，网格内却是空白的，也无法
    // 再作为后续拖放目标。在框架已完成重挂后显式 show()；切换模式仍会由下方
    // refreshSelection() 按活动成员隐藏整个 frame，不会因此把多个终端同时露出。
    workspace->show();
    m_terminals.append(workspace);
    m_frames.insert(workspace, frame);
    m_titles.insert(workspace, title.trimmed().isEmpty() ? tr("Terminal") : title.trimmed());
    if (m_activeTerminal == nullptr) {
        m_activeTerminal = workspace;
    }
    rebuildLayout();
    emit terminalsChanged();
}

bool TerminalWorkspaceGroup::takeTerminal(TerminalWorkspace *workspace)
{
    const int index = m_terminals.indexOf(workspace);
    if (index < 0) {
        return false;
    }
    WorkspaceTerminalFrame *frame = m_frames.take(workspace);
    m_grid->removeWidget(frame);
    workspace->setParent(this);
    workspace->show();
    frame->deleteLater();
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
    for (WorkspaceTerminalFrame *frame : std::as_const(m_frames)) {
        m_grid->removeWidget(frame);
    }

    const bool switchMode = m_layoutMode == LayoutMode::Switch;
    const int terminalCount = m_terminals.size();
    const int gridColumns = terminalCount <= 1
        ? 1
        : static_cast<int>(std::ceil(std::sqrt(static_cast<double>(terminalCount))));
    for (int index = 0; index < terminalCount; ++index) {
        TerminalWorkspace *workspace = m_terminals.at(index);
        WorkspaceTerminalFrame *frame = m_frames.value(workspace);
        int row = 0;
        int column = 0;
        switch (m_layoutMode) {
        case LayoutMode::Horizontal:
            column = index;
            break;
        case LayoutMode::Vertical:
            row = index;
            break;
        case LayoutMode::Grid:
            row = index / gridColumns;
            column = index % gridColumns;
            break;
        case LayoutMode::Switch:
            // 所有成员占同一网格单元，但只显示当前项；这种写法不会在切换时重建控件或会话。
            break;
        }
        m_grid->addWidget(frame, row, column);
        workspace->setFileManagerAllowed(switchMode);
    }
    refreshSelection();
}

void TerminalWorkspaceGroup::refreshSelection()
{
    const bool switchMode = m_layoutMode == LayoutMode::Switch;
    for (TerminalWorkspace *workspace : std::as_const(m_terminals)) {
        WorkspaceTerminalFrame *frame = m_frames.value(workspace);
        frame->setSelected(workspace == m_activeTerminal);
        frame->setVisible(!switchMode || workspace == m_activeTerminal);
    }
}

WorkspaceSidebarWidget::WorkspaceSidebarWidget(QWidget *parent)
    : QWidget(parent)
    , m_modeCombo(new QComboBox(this))
    , m_terminalList(new QListWidget(this))
    , m_emptyHint(new QLabel(this))
{
    setObjectName(QStringLiteral("WorkspaceSidebar"));
    m_modeCombo->setObjectName(QStringLiteral("WorkspaceLayoutMode"));
    m_terminalList->setObjectName(QStringLiteral("WorkspaceTerminalThumbnails"));
    m_terminalList->setIconSize(QSize(52, 34));
    m_terminalList->setSpacing(4);
    m_emptyHint->setObjectName(QStringLiteral("WorkspaceSidebarEmptyHint"));
    m_emptyHint->setWordWrap(true);
    m_emptyHint->setAlignment(Qt::AlignCenter);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(m_modeCombo);
    layout->addWidget(m_terminalList, 1);
    layout->addWidget(m_emptyHint, 1);
    retranslateUi();
    setWorkspace(nullptr);

    connect(m_modeCombo,
            &QComboBox::currentIndexChanged,
            this,
            [this](const int index) {
                if (m_workspace == nullptr || index < 0) {
                    return;
                }
                m_workspace->setLayoutMode(static_cast<TerminalWorkspaceGroup::LayoutMode>(
                    m_modeCombo->itemData(index).toInt()));
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
    }
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
    const int previousMode = m_modeCombo->currentData().toInt();
    const QSignalBlocker blocker(m_modeCombo);
    m_modeCombo->clear();
    m_modeCombo->addItem(tr("Horizontal"), static_cast<int>(TerminalWorkspaceGroup::LayoutMode::Horizontal));
    m_modeCombo->addItem(tr("Vertical"), static_cast<int>(TerminalWorkspaceGroup::LayoutMode::Vertical));
    m_modeCombo->addItem(tr("Grid"), static_cast<int>(TerminalWorkspaceGroup::LayoutMode::Grid));
    m_modeCombo->addItem(tr("Switch"), static_cast<int>(TerminalWorkspaceGroup::LayoutMode::Switch));
    const int restoredIndex = m_modeCombo->findData(previousMode);
    m_modeCombo->setCurrentIndex(restoredIndex >= 0 ? restoredIndex : 2);
    m_emptyHint->setText(tr("Drag a terminal tab into the current terminal page or this sidebar to create or extend a workspace."));
}

void WorkspaceSidebarWidget::refreshTerminals()
{
    const QSignalBlocker listBlocker(m_terminalList);
    m_terminalList->clear();
    const bool hasWorkspace = m_workspace != nullptr;
    m_modeCombo->setEnabled(hasWorkspace);
    m_terminalList->setVisible(hasWorkspace);
    m_emptyHint->setVisible(!hasWorkspace);
    if (!hasWorkspace) {
        return;
    }

    {
        const QSignalBlocker comboBlocker(m_modeCombo);
        const int modeIndex = m_modeCombo->findData(static_cast<int>(m_workspace->layoutMode()));
        m_modeCombo->setCurrentIndex(modeIndex);
    }
    const QList<TerminalWorkspace *> terminals = m_workspace->terminals();
    for (int index = 0; index < terminals.size(); ++index) {
        TerminalWorkspace *workspace = terminals.at(index);
        // 缩略图使用小型终端屏幕轮廓，而不抓取真实终端像素。真实截图可能包含密码、令牌等
        // 敏感输出，也会因隐藏页面没有渲染而变成空白；语义预览既稳定又不会复制敏感内容。
        QPixmap preview(52, 34);
        preview.fill(Qt::transparent);
        QPainter painter(&preview);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(palette().color(QPalette::Mid), 1.2));
        painter.setBrush(palette().color(QPalette::Base));
        painter.drawRoundedRect(QRectF(1.0, 1.0, 50.0, 32.0), 4.0, 4.0);
        painter.setPen(QPen(palette().color(QPalette::Highlight), 1.4));
        painter.drawLine(QPointF(8.0, 11.0), QPointF(13.0, 16.0));
        painter.drawLine(QPointF(13.0, 16.0), QPointF(8.0, 21.0));
        painter.drawLine(QPointF(18.0, 21.0), QPointF(34.0, 21.0));
        auto *item = new QListWidgetItem(QIcon(preview), m_workspace->terminalTitle(workspace));
        item->setSizeHint(QSize(0, 48));
        m_terminalList->addItem(item);
        if (workspace == m_workspace->activeTerminal()) {
            m_terminalList->setCurrentRow(index);
        }
    }
}
