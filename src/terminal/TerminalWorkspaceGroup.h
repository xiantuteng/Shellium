#pragma once

#include <QHash>
#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;
class QSplitter;
class QVBoxLayout;
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

    /** @brief 分屏拖动过程中根据全局坐标算出停靠目标与边；target 为空表示最外层。 */
    struct DockTargetHit {
        WorkspaceTerminalFrame *targetFrame = nullptr;
        DockEdge edge = DockEdge::Right;
        QRect zoneInContent;
        bool valid = false;
    };
    [[nodiscard]] DockTargetHit hitTestDock(const QPoint &globalPosition) const;

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
    LayoutMode m_layoutMode = LayoutMode::Tiled;
};

/**
 * @brief 导航轨“工作空间”页面：分屏 / 切换二态开关 + 终端缩略图列表。
 *
 * 页面不拥有工作空间；MainWindow 在顶层标签切换时替换观察对象。
 */
class WorkspaceSidebarWidget final : public QWidget {
    Q_OBJECT

public:
    /** @brief 创建工作空间侧栏。 */
    explicit WorkspaceSidebarWidget(QWidget *parent = nullptr);

    /** @brief 切换当前观察的工作空间；传空会显示使用提示并禁用模式开关。 */
    void setWorkspace(TerminalWorkspaceGroup *workspace);

protected:
    /** @brief 语言热切换时重译常驻开关文案与空态提示。 */
    void changeEvent(QEvent *event) override;

private:
    /** @brief 重新生成模式开关文案，并保持当前状态不变。 */
    void retranslateUi();

    /** @brief 从当前工作空间重建终端缩略图列表和选中项。 */
    void refreshTerminals();

    TerminalWorkspaceGroup *m_workspace = nullptr;
    QPushButton *m_modeToggle = nullptr;
    QListWidget *m_terminalList = nullptr;
    QLabel *m_emptyHint = nullptr;
};
