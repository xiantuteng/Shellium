#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;
class QSplitter;
class QToolButton;
class QVBoxLayout;
class TerminalPane;
class TerminalWorkspace;
class WorkspaceDockOverlay;
class WorkspaceTerminalFrame;

/**
 * @brief 在一个顶层标签内承载多个正在运行的终端会话。
 *
 * 该容器只改变既有 TerminalWorkspace 的父子关系和可见布局，不复制或重连 SSH 会话。
 * 每个成员仍保留自己的终端缓冲、连接栈、AI 上下文和文件管理器状态。
 *
 * 分屏（Tiled）模式下成员挂在一棵递归 QSplitter 树里，用户可以拖动某个终端顶部的会话
 * 状态栏把它停靠到另一终端的上/下/左/右半区，或工作空间整体的四条外边缘；切换（Switch）
 * 模式每次只铺满当前成员并恢复其远程文件管理能力。停靠布局仅在当前会话内有效。
 */
class TerminalWorkspaceGroup final : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief 工作空间的两种显示模式。
     *
     * Tiled：递归 QSplitter 树，同时显示多个终端，禁用远程文件管理；
     * Switch：只铺满当前成员，侧栏缩略图驱动切换，恢复远程文件管理。
     */
    enum class LayoutMode {
        Tiled,
        Switch
    };
    Q_ENUM(LayoutMode)

    /** @brief 停靠目标边：决定新分块出现在参照物的哪一侧。 */
    enum class DockEdge {
        Left,
        Right,
        Top,
        Bottom
    };
    Q_ENUM(DockEdge)

    /** @brief 创建空工作空间容器；至少加入两个终端后才应作为顶层标签展示。 */
    explicit TerminalWorkspaceGroup(QWidget *parent = nullptr);

    /**
     * @brief 把一个已存在的终端会话加入工作空间。
     * @param workspace 待接管的终端控件；所有权转移给本容器，但会话不会重建。
     * @param title 终端在工作空间侧栏中的显示名称；空值会回退为“终端”。
     *
     * 新成员追加为分屏树根 QSplitter 的等分兄弟，用户之后可自行拖动重排。
     */
    void addTerminal(TerminalWorkspace *workspace, const QString &title);

    /**
     * @brief 从工作空间移除终端但不销毁它。
     * @param workspace 待移除终端。
     * @return 成功移除返回 true；不属于本工作空间时返回 false。
     */
    bool takeTerminal(TerminalWorkspace *workspace);

    /**
     * @brief 把一个成员停靠到另一成员（或整个工作空间）的指定边。
     * @param source 被移动的成员。
     * @param target 参照成员；传空表示停靠到分屏树最外层。
     * @param edge 新分块相对参照物出现的方向。
     *
     * 只重挂 QWidget 与 QSplitter 节点，不重启终端或 SSH。方向与现有 QSplitter 一致时
     * 直接插入兄弟槽，否则新建一层反向 QSplitter；source 原父节点若只剩一个子控件会被解包。
     */
    void dockTerminal(TerminalWorkspace *source, TerminalWorkspace *target, DockEdge edge);

    /**
     * @brief 从标签栏把一个终端拖入本工作空间时的落点预览（与内部拖动同一套高亮）。
     *
     * MainWindow 在 SessionTabBar 的拖出信号里驱动这三个方法：begin 时亮出高亮层，move 时
     * 按全局坐标刷新目标半区，end 时收起高亮但保留最后一次命中结果，供随后的合并读取。
     */
    void beginExternalDropHint();
    void updateExternalDropHint(const QPoint &globalPosition);
    void endExternalDropHint();

    /**
     * @brief 取出并清空最近一次外部落点命中。
     * @param target 命中的参照成员；停靠到最外层时写入 nullptr。
     * @param edge 停靠方向。
     * @return 有有效命中返回 true；否则返回 false 且不改写出参。
     */
    bool consumeExternalDropTarget(TerminalWorkspace **target, DockEdge *edge);

    /** @brief 返回工作空间内全部终端，顺序与侧栏缩略图一致（与分屏树结构无关）。 */
    [[nodiscard]] QList<TerminalWorkspace *> terminals() const;

    /** @brief 判断指定终端是否属于本工作空间。 */
    [[nodiscard]] bool contains(TerminalWorkspace *workspace) const;

    /** @brief 返回当前选中并接收工作空间级动作的终端。 */
    [[nodiscard]] TerminalWorkspace *activeTerminal() const;

    /** @brief 选择一个成员终端；非成员指针会被忽略。 */
    void setActiveTerminal(TerminalWorkspace *workspace);

    /** @brief 返回指定成员在侧栏中的名称；非成员返回空字符串。 */
    [[nodiscard]] QString terminalTitle(TerminalWorkspace *workspace) const;

    /** @brief 返回当前显示模式。 */
    [[nodiscard]] LayoutMode layoutMode() const;

    /** @brief 是否已开启工作空间内实时输入广播。 */
    [[nodiscard]] bool isBroadcastEnabled() const;

    /**
     * @brief 开关工作空间内实时输入广播。
     *
     * 开启后，任意成员终端里键入 / 粘贴 / 输入法提交的原始字节会被同步写入其他所有成员的
     * PTY，因此各终端的操作完全一致。接收端不再次广播，不会形成回路。
     */
    void setBroadcastEnabled(bool enabled);

    /**
     * @brief 切换分屏 / 切换模式并重新排列既有会话。
     * @param mode 新模式。
     */
    void setLayoutMode(LayoutMode mode);

signals:
    /** @brief 当前终端变化，侧栏缩略图、AI 助手和终端动作应同步跟随。 */
    void activeTerminalChanged(TerminalWorkspace *workspace);

    /** @brief 成员增删、名称或停靠位置变化，工作空间侧栏需要重建缩略图列表。 */
    void terminalsChanged();

    /** @brief 显示模式发生变化。 */
    void layoutModeChanged(TerminalWorkspaceGroup::LayoutMode mode);

    /** @brief 广播开关状态发生变化。 */
    void broadcastEnabledChanged(bool enabled);

    /**
     * @brief 成员会话状态栏拖动进行中（分屏与切换模式都会发出）。
     *
     * MainWindow 据此在标题栏预告“拖回标签栏移出工作空间”的插入位置。
     */
    void memberDragMoved(TerminalWorkspace *source, const QPoint &globalPosition);

    /**
     * @brief 成员拖动在给定全局坐标释放。
     *
     * 在组内停靠之前发出；接收方若要把成员移出工作空间，必须排队执行（发出时仍处于成员
     * 自身的事件过滤器里），本组发现成员已不在组内时不会再做停靠。
     */
    void memberDragReleased(TerminalWorkspace *source, const QPoint &globalPosition);

private:
    /** @brief 按当前模式刷新容器底色、成员可见性与远程文件管理开关。 */
    void rebuildLayout();

    /** @brief 刷新选中/未选中边框，并确保切换模式只展示当前成员。 */
    void refreshSelection();

    /** @brief 创建一个统一配置的内部 QSplitter 节点。 */
    [[nodiscard]] QSplitter *makeSplitter(Qt::Orientation orientation);

    /** @brief 把 splitter 各子控件尺寸设为等分。 */
    void equalizeSplitter(QSplitter *splitter);

    /**
     * @brief 摘除一个成员后收拾其原父节点：只剩一个子控件就把它提升到祖父并删掉这层，
     *        空节点直接删掉；根节点永不删除。会向上级联。
     */
    void pruneSplitter(QSplitter *splitter);

    /** @brief 给一个成员的会话状态栏挂上广播 / 布局切换 / 关闭三个按钮并接线。 */
    void installMemberControls(TerminalWorkspace *workspace);

    /** @brief 把 source 的原始输入同步写入其他所有成员（广播开启时）。 */
    void relayBroadcastInput(TerminalPane *source, const QByteArray &bytes);

    /** @brief 分屏拖动过程中根据全局坐标算出停靠目标与边；target 为空表示最外层。 */
    struct DockTargetHit {
        WorkspaceTerminalFrame *targetFrame = nullptr;
        DockEdge edge = DockEdge::Right;
        QRect zoneInContent;
        bool valid = false;
    };
    [[nodiscard]] DockTargetHit hitTestDock(const QPoint &globalPosition) const;

    /** @brief 把分屏树里所有 QSplitter 的分隔条刷成终端背景色，消除浅色分隔条。 */
    void restyleSplitterHandles(const QColor &chromeColor);

    /** @brief 成员会话状态栏拖动的三个阶段回调，注入给每个 WorkspaceTerminalFrame。 */
    void beginDockDrag(TerminalWorkspace *source);
    void updateDockDrag(const QPoint &globalPosition);
    void endDockDrag(const QPoint &globalPosition);

    QWidget *m_content = nullptr;
    QVBoxLayout *m_contentLayout = nullptr;
    QSplitter *m_splitRoot = nullptr;
    WorkspaceDockOverlay *m_dockOverlay = nullptr;
    QList<TerminalWorkspace *> m_terminals;
    QHash<TerminalWorkspace *, WorkspaceTerminalFrame *> m_frames;
    QHash<TerminalWorkspace *, QString> m_titles;
    TerminalWorkspace *m_activeTerminal = nullptr;
    TerminalWorkspace *m_dockDragSource = nullptr;
    DockTargetHit m_externalHit;
    bool m_externalDropActive = false;
    bool m_broadcastEnabled = false;
    /** @brief 各成员标题栏里的广播按钮，开关状态整体同步。 */
    QList<QToolButton *> m_broadcastButtons;
    LayoutMode m_layoutMode = LayoutMode::Tiled;
};

/**
 * @brief 侧栏中单个终端行的展示数据，由 MainWindow 组装后推给 WorkspaceSidebarWidget。
 *
 * 侧栏本身没有数据库和每会话连接状态，因此主机名、发行版、连接状态都在此结构里下发；
 * 缺省（空列表或数量不匹配）时侧栏回退到 TerminalWorkspaceGroup::terminalTitle()。
 */
struct WorkspaceSidebarRow {
    /** @brief 主标题：优先显示名/主机名，回退到工作空间成员标题。 */
    QString title;
    /** @brief 次级行，如 "ssh, root"；空表示不画第二行。 */
    QString subtitle;
    /** @brief /etc/os-release 的规范化 ID，可空（未探测到时用通用主机图标）。 */
    QString distributionId;
    /** @brief 连接状态，取值对齐 SessionTabBar::ConnectionState（0=Unknown…3=Failed）。 */
    int connectionState = 0;
};

/**
 * @brief 导航轨“工作空间”页面：Termius 风格的终端列表 + 布局 / 广播开关。
 *
 * 顶部一行是「Terminals · N」标题加两个图标按钮（布局模式、输入广播）；下方每行显示发行版
 * 图标、主机名（同主机多开带序号）、"ssh, user" 次级行、连接状态点，悬停浮出关闭按钮。
 * 页面不拥有工作空间；MainWindow 在顶层标签切换时替换观察对象并下发每行数据。
 */
class WorkspaceSidebarWidget final : public QWidget {
    Q_OBJECT

public:
    /** @brief 创建工作空间侧栏。 */
    explicit WorkspaceSidebarWidget(QWidget *parent = nullptr);

    /** @brief 切换当前观察的工作空间；传空会显示使用提示并禁用模式开关。 */
    void setWorkspace(TerminalWorkspaceGroup *workspace);

    /**
     * @brief 下发每行展示数据；行序与 TerminalWorkspaceGroup::terminals() 一一对应。
     * @param rows 空或数量与成员数不符时忽略，回退到成员标题文本。
     */
    void setMemberRows(const QList<WorkspaceSidebarRow> &rows);

signals:
    /** @brief 用户在某行点了关闭按钮，请求把该终端移出工作空间。 */
    void closeTerminalRequested(TerminalWorkspace *workspace);

protected:
    /** @brief 语言热切换时重译常驻开关文案与空态提示。 */
    void changeEvent(QEvent *event) override;

private:
    /** @brief 重新生成模式开关文案，并保持当前状态不变。 */
    void retranslateUi();

    /** @brief 从当前工作空间重建终端缩略图列表和选中项。 */
    void refreshTerminals();

    TerminalWorkspaceGroup *m_workspace = nullptr;
    QLabel *m_title = nullptr;
    QToolButton *m_layoutToggle = nullptr;
    QToolButton *m_broadcastToggle = nullptr;
    QWidget *m_headerRow = nullptr;
    QListWidget *m_terminalList = nullptr;
    QLabel *m_emptyHint = nullptr;
    QList<WorkspaceSidebarRow> m_rows;
};
