#pragma once

#include <QHash>
#include <QWidget>

class QComboBox;
class QGridLayout;
class QLabel;
class QListWidget;
class QStackedWidget;
class TerminalWorkspace;
class WorkspaceTerminalFrame;

/**
 * @brief 在一个顶层标签内承载多个正在运行的终端会话。
 *
 * 该容器只改变既有 TerminalWorkspace 的父子关系和可见布局，不复制或重连 SSH 会话。
 * 每个成员仍保留自己的终端缓冲、连接栈、AI 上下文和文件管理器状态；铺开显示时仅暂时
 * 禁止文件管理器，切回单终端切换模式后即可继续使用。
 */
class TerminalWorkspaceGroup final : public QWidget {
    Q_OBJECT

public:
    /** @brief 工作空间的四种显示布局。前三种同时显示多个终端，Switch 每次铺满一个终端。 */
    enum class LayoutMode {
        Horizontal,
        Vertical,
        Grid,
        Switch
    };
    Q_ENUM(LayoutMode)

    /** @brief 创建空工作空间容器；至少加入两个终端后才应作为顶层标签展示。 */
    explicit TerminalWorkspaceGroup(QWidget *parent = nullptr);

    /**
     * @brief 把一个已存在的终端会话加入工作空间。
     * @param workspace 待接管的终端控件；所有权转移给本容器，但会话不会重建。
     * @param title 终端在工作空间侧栏中的显示名称；空值会回退为“终端”。
     */
    void addTerminal(TerminalWorkspace *workspace, const QString &title);

    /**
     * @brief 从工作空间移除终端但不销毁它。
     * @param workspace 待移除终端。
     * @return 成功移除返回 true；不属于本工作空间时返回 false。
     */
    bool takeTerminal(TerminalWorkspace *workspace);

    /** @brief 返回工作空间内全部终端，顺序与布局及侧栏缩略图一致。 */
    [[nodiscard]] QList<TerminalWorkspace *> terminals() const;

    /** @brief 判断指定终端是否属于本工作空间。 */
    [[nodiscard]] bool contains(TerminalWorkspace *workspace) const;

    /** @brief 返回当前选中并接收工作空间级动作的终端。 */
    [[nodiscard]] TerminalWorkspace *activeTerminal() const;

    /** @brief 选择一个成员终端；非成员指针会被忽略。 */
    void setActiveTerminal(TerminalWorkspace *workspace);

    /** @brief 返回指定成员在侧栏中的名称；非成员返回空字符串。 */
    [[nodiscard]] QString terminalTitle(TerminalWorkspace *workspace) const;

    /** @brief 返回当前显示布局。 */
    [[nodiscard]] LayoutMode layoutMode() const;

    /**
     * @brief 切换工作空间布局并重新排列既有会话。
     * @param mode 新布局。
     *
     * 水平、垂直和网格会同时显示多个终端，因此会强制收起并禁用远程文件管理；Switch
     * 每次只显示选中终端，允许该终端恢复自己的文件管理器。
     */
    void setLayoutMode(LayoutMode mode);

signals:
    /** @brief 当前终端变化，侧栏缩略图、AI 助手和终端动作应同步跟随。 */
    void activeTerminalChanged(TerminalWorkspace *workspace);

    /** @brief 成员增删或名称变化，工作空间侧栏需要重建缩略图列表。 */
    void terminalsChanged();

    /** @brief 显示模式发生变化。 */
    void layoutModeChanged(TerminalWorkspaceGroup::LayoutMode mode);

private:
    /** @brief 根据 m_layoutMode 重新设置每个终端框的网格位置和可见性。 */
    void rebuildLayout();

    /** @brief 刷新选中/未选中边框，并确保切换模式只展示当前成员。 */
    void refreshSelection();

    QWidget *m_content = nullptr;
    QGridLayout *m_grid = nullptr;
    QList<TerminalWorkspace *> m_terminals;
    QHash<TerminalWorkspace *, WorkspaceTerminalFrame *> m_frames;
    QHash<TerminalWorkspace *, QString> m_titles;
    TerminalWorkspace *m_activeTerminal = nullptr;
    LayoutMode m_layoutMode = LayoutMode::Grid;
};

/**
 * @brief 导航轨“工作空间”页面，展示当前工作空间的布局选择和终端缩略图列表。
 *
 * 页面不拥有工作空间；MainWindow 在顶层标签切换时替换观察对象。列表项采用终端预览卡片
 * 图标与标题，点击即可切换右侧铺满终端，也可在铺开布局中改变实线选中框。
 */
class WorkspaceSidebarWidget final : public QWidget {
    Q_OBJECT

public:
    /** @brief 创建工作空间侧栏。 */
    explicit WorkspaceSidebarWidget(QWidget *parent = nullptr);

    /** @brief 切换当前观察的工作空间；传空会显示使用提示并禁用布局选择。 */
    void setWorkspace(TerminalWorkspaceGroup *workspace);

protected:
    /** @brief 语言热切换时重译常驻模式选项与空态提示。 */
    void changeEvent(QEvent *event) override;

private:
    /** @brief 重新生成布局下拉框文案，并保持当前枚举值不变。 */
    void retranslateUi();

    /** @brief 从当前工作空间重建终端缩略图列表和选中项。 */
    void refreshTerminals();

    TerminalWorkspaceGroup *m_workspace = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QListWidget *m_terminalList = nullptr;
    QLabel *m_emptyHint = nullptr;
};

