#pragma once

#include "settings/TerminalSettings.h"

#include <QMainWindow>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QStringList>
#include <QVector>

class ApplicationDatabase;
class AiAssistantWidget;
class AiSettingsService;
// ConnectionTarget 的真实定义是结构体。MSVC 会把 class/struct 关键字编码进模板参数的
// 修饰名，前置声明若写成 class 会使 QList<ConnectionTarget> 的调用方与实现方符号不一致；
// 统一为 struct 可同时保持 Windows、macOS 和 Linux 三个平台的 ABI 声明一致。
struct ConnectionTarget;
class CredentialVault;
class HostManagerWidget;
class LocalizationService;
class PortForwardManagerWidget;
class ProductivityModel;
class ProductivityWidget;
class QuickCommandPopup;
struct TerminalSettings;
class QAction;
class QEvent;
class QLabel;
class QMessageBox;
class QMenu;
class QObject;
class QPoint;
class QResizeEvent;
class QShowEvent;
class QSplitter;
class QStackedWidget;
class QTabWidget;
class QTimer;
class QToolButton;
class QVariantAnimation;
class QWidget;
class SessionTabBar;
class SessionTabStrip;
class SessionTabDropOverlay;
class TerminalPane;
class TerminalSettingsService;
class TerminalWorkspace;
class TerminalWorkspaceGroup;
class UpdateChecker;
class WorkspaceSidebarWidget;

/**
 * @brief Shellium 主窗口，协调标签页、终端和连接对话框。
 *
 * 每个 TerminalWorkspace 在标签页内部持有自己的远程文件管理器；主窗口不持有或转发
 * SSH/SFTP 会话状态，从而保证不同终端标签之间完全隔离。
 */
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    /**
     * @brief 创建主窗口并接入已初始化的持久化数据库。
     * @param database 应用级数据库，生命周期由 main() 管理并覆盖整个窗口。
     * @param parent Qt 父对象。
     */
    explicit MainWindow(ApplicationDatabase *database,
                        CredentialVault *credentialVault,
                        LocalizationService *localization = nullptr,
                        QWidget *parent = nullptr);
    ~MainWindow() override;

    /**
     * @brief 构建标题栏标签的右键菜单。
     * @param index 被右键的标签索引；小于 0 表示点在空白处，只保留"新建终端标签"。
     * @param parent 菜单的父控件，由调用方决定生命周期。
     * @return 已装配好全部动作的菜单，调用方负责弹出或直接触发其中的动作。
     *
     * 菜单构建与弹出分离，用例才能在不进入 exec() 阻塞事件循环的前提下触发同一批动作。
     */
    [[nodiscard]] QMenu *createSessionTabMenu(int index, QWidget *parent);

    /**
     * @brief 打开堡垒机通过 SecureCRT 命令行交来的一次性会话。
     * @param target 已解析的目标。登录票据在 loginUsername 中，本函数不写入主机库。
     *
     * 每次点击都新建标签，和 SecureCRT 新开一个会话页一致。已保存主机的连接历史不记录
     * 这次票据，避免过期票据留在磁盘上。
     */
    void openSecureCrtSession(const ConnectionTarget &target);

protected:
    /**
     * @brief 观察全应用的人机输入并刷新保险库闲置计时器。
     *
     * 只处理键盘、鼠标、滚轮和触摸等真实活动；绘制、网络与定时器事件不能延长解锁时间，
     * 否则一个持续输出日志的无人值守终端会让保险库永不自动锁定。
     */
    bool eventFilter(QObject *watched, QEvent *event) override;

    /**
     * @brief 窗口首次显示后配置 macOS 原生透明标题栏。
     *
     * Cocoa 的 NSWindow 在 QWidget 构造阶段还不一定存在，因此必须延迟到 showEvent；配置
     * 失败时不会改变功能，并会在后续显示事件安全重试。
     */
    void showEvent(QShowEvent *event) override;

    /**
     * @brief 在窗口最大化、恢复或全屏状态变化后重新对齐 macOS 原生标题按钮。
     * @param event Qt 窗口状态变化事件；其他 changeEvent 类型只交给基类处理。
     *
     * AppKit 可能在状态转换期间重新布局红黄绿按钮，因此需要等待当前事件完成后再次校正，
     * 否则按钮会短暂或永久回到系统默认短标题栏的纵向位置。
     */
    void changeEvent(QEvent *event) override;

    /**
     * @brief 在原生窗口尺寸变化后维持交通灯相对自定义标题栏的垂直位置。
     * @param event Qt 窗口尺寸变化事件。
     *
     * macOS 的标准缩放动画会产生连续尺寸事件，却不保证对应 Qt::WindowStateChange；在这里
     * 做幂等对齐可覆盖双击放大、绿色按钮缩放以及普通边缘调整三条路径。
     */
    void resizeEvent(QResizeEvent *event) override;

private:
    /**
     * @brief 创建更新检查器：启动后延迟检查一次、之后每天一次，并接入菜单的手动检查。
     * @param checkForUpdatesAction “帮助 → 检查更新”动作。
     *
     * 自动检查只在发现未被跳过的新版本时弹出提示，失败和“已是最新”都保持安静；手动检查
     * 无论结果都给出反馈，且忽略跳过记录。
     */
    void setupUpdateChecks(QAction *checkForUpdatesAction);

    /**
     * @brief 按当前最大化状态刷新 Windows/Linux 标题栏的最大化或还原图标。
     *
     * 无边框窗口不再由系统绘制标题按钮；窗口状态可能来自按钮、双击标题栏、快捷键或桌面
     * 环境，因此统一在状态变化后读取真实状态，避免按钮图标与窗口实际状态不一致。
     */
    void refreshWindowMaximizeIcon();

    /** @brief 打开连接对话框，并在当前标签页的终端中启动 SSH。 */
    void connectCurrentPane();

    /**
     * @brief 经当前标签页已连接的服务器，由本程序亲自建立下一层嵌套连接。
     *
     * 与用户在终端里手打 ssh 的区别在于：这样建立的层是确定事实，退出时机同样确定，
     * 文件管理器无需对终端输出做任何猜测。
     */
    void connectNestedFromCurrentPane();

    /**
     * @brief 从主机树或最近连接列表连接一条持久化主机记录。
     * @param hostId 数据库稳定主键，用于连接历史与凭据引用。
     * @param connectionPlan 已解析的受管连接计划；为空时由主窗口按 hostId 重新解析。新建
     *        主机后首次连接时，最终层还可能带进程内临时密码。
     * @param openInNewTab true 时先新建标签页再连接（双击主机），而不是复用当前标签页。
     */
    void connectSavedHost(const QString &hostId,
                          const QList<ConnectionTarget> &connectionPlan,
                          bool openInNewTab = false);

    /**
     * @brief 由"通过主机链连接"对话框手动指定跳板发起连接。
     * @param hostId 最终目标主机的稳定 id。
     * @param jumpHostId 用户手动选中的跳板主机稳定 id；只有本次连接握手成功后才会被记为
     *        该目标主机"最近连接"的跳板，供下次打开对话框时列在最前。
     * @param connectionPlan 已拼接好跳板与目标两段的完整连接计划。
     * @param openInNewTab true 时新建标签页，与 connectSavedHost() 语义一致。
     *
     * 内部只是给 connectSavedHost() 暂存 jumpHostId 后原样转发，复用其全部凭据解锁与
     * 标签页创建逻辑，避免维护第二套几乎相同的连接流程。
     */
    void connectSavedHostViaChain(const QString &hostId,
                                  const QString &jumpHostId,
                                  const QList<ConnectionTarget> &connectionPlan,
                                  bool openInNewTab);

    /**
     * @brief 连接某个主机分组下的全部主机，并把它们放进同一个新建工作空间标签。
     * @param groupId 数据库中该分组的稳定主键。
     * @param groupName 分组名称，直接作为新工作空间标签的标题。
     *
     * 依次复用 connectSavedHost() 为每台主机新建独立标签（保留其保险库解锁、历史记录等
     * 既有行为），再仿照拖拽合并路径把这些标签重新挂进同一个 TerminalWorkspaceGroup，
     * 默认以切换模式打开并把侧栏切到“工作空间”页。
     */
    void connectHostGroupInWorkspace(const QString &groupId, const QString &groupName);

    /** @brief 根连接认证成功后写入进行中的成功历史。 */
    void recordConnectionSucceeded(TerminalPane *pane, const ConnectionTarget &target);

    /** @brief 根连接认证前失败后写入带错误原因的完整历史。 */
    void recordConnectionFailed(TerminalPane *pane,
                                const ConnectionTarget &target,
                                const QString &message);

    /** @brief 已成功根连接断开后补齐结束时间和持续时长。 */
    void recordConnectionDisconnected(TerminalPane *pane,
                                      const ConnectionTarget &target,
                                      int exitStatus);

    /** @brief 确保保险库已解锁，供连接已保存凭据和安全设置操作复用。 */
    bool ensureVaultUnlocked();

    /** @brief 创建一个拥有独立终端与文件管理器的新标签页，并统一连接状态信号。 */
    TerminalWorkspace *createWorkspaceTab(const QString &title);

    /**
     * @brief 构建"没有任何标签页"时占据整个工作区的品牌空态页面。
     * @return 由调用方加入工作区栈的空态控件。
     *
     * 启动时不再预先创建一个空终端：真正的默认状态是"还没有会话"。用一个独立页面表达
     * 它，比留下一个连 Shell 都没启动的空标签更诚实，也避免用户误以为那是可用终端。
     */
    QWidget *createWorkspacePlaceholder();

    /**
     * @brief 按当前标签数量在品牌空态页与终端标签页之间切换工作区栈。
     *
     * 所有增删标签的路径都必须调用它；否则关闭最后一个标签后，QTabWidget 会留下一块
     * 没有页面的空白区域，看起来像界面渲染失败。
     */
    void updateWorkspacePlaceholder();

    /**
     * @brief 为一个终端标签配置其专属 AI 助手及命令路由。
     * @param assistant 归属于该标签的 AI 助手实例，不可为空。
     * @param pane 同一标签内的终端面板，不可为空；助手终端上下文始终从它读取。
     *
     * 每个标签使用独立助手实例，才能保证模型异步回复、输出捕获和最近十次会话在切换标签
     * 后仍留在原标签。命令也直接路由到绑定 pane，避免用户点击后恰逢标签切换而发错终端。
     */
    void configureAiAssistant(AiAssistantWidget *assistant, TerminalPane *pane);

    /**
     * @brief 关闭指定会话标签并同步标题栏标签模型。
     * @param index 待关闭标签索引。
     * @param allowLast 保留参数：关闭最后一个标签现在会回到品牌空态页，两种取值行为相同。
     *
     * 标题栏 QTabBar 只是中央 QTabWidget 的视觉镜像，所有删除都必须经过这里，避免两个
     * 控件的索引错位后把连接、文件管理器或保存工作区操作路由到错误会话。
     */
    void closeWorkspaceTab(int index, bool allowLast = false);

    /**
     * @brief 同时更新中央工作区与标题栏镜像中的标签文字。
     * @param index 标签索引。
     * @param title 经过调用方校验的非空显示名称。
     */
    void setWorkspaceTabText(int index, const QString &title);

    /**
     * @brief 在新标签中再打开一份与指定标签相同的会话。
     * @param index 被复制的标签索引。
     *
     * 直接复用源面板已解析好的整条受管计划（含逐跳凭据），因此复制远程标签不会再次触发
     * 主机链解析和保险库解锁；源标签跑的是本地 Shell 时，副本同样只是本地 Shell。
     */
    void duplicateWorkspaceTab(int index);

    /**
     * @brief 显示或隐藏左侧单一内容侧栏。
     * @param visible true 表示显示当前侧栏页面，false 表示仅保留窄图标导航轨。
     *
     * 通过动画修改 QSplitter 两个子区域的实际尺寸。动画期间不立即隐藏侧栏面板，避免
     * QWidget 的显隐变化让布局瞬间跳变；内容保持展开宽度，在裁剪容器内向左滑出或
     * 向右滑入。收起完成后才隐藏，反向请求从当前宽度继续，无返回值或 IO 错误。
     */
    void setSidebarVisible(bool visible);

    /**
     * @brief 切换左侧侧栏中的功能页面并确保侧栏可见。
     * @param pageIndex 0 为主机、1 为效率工具、2 为端口转发、3 为 AI、4 为工作空间；
     *        越界值会被忽略。
     *
     * 页面采用一个 QStackedWidget 而不是多个 Dock。这样导航轨、标题和正文始终共享同一
     * 对齐体系，窗口缩放时不会出现 Dock 标题条、拖动把手和页签反复挤压终端的问题。
     */
    void showSidebarPage(int pageIndex);

    /**
     * @brief 让端口转发页的主机选择器跟随当前终端标签所连的持久化主机。
     *
     * 本地 Shell、临时连接或主机已删除时不改动选择器，保留用户上次手动挑选的主机。
     */
    void syncForwardingHostToActiveTerminal();

    /** @brief 侧栏各页的标题文案，随当前语言实时求值（不能缓存进 static）。 */
    QString sidebarPageTitle(int pageIndex) const;

    /**
     * @brief 读取上次持久化的侧栏宽度并应用到分隔器。
     *
     * 读取失败或数值越界都退回默认宽度，而不是把损坏值原样交给 QSplitter：一个过小或
     * 过大的宽度会让侧栏或终端在启动后完全无法使用，用户也没有入口修正它。
     */
    void restoreSidebarWidth();

    /** @brief 应用当前侧栏密度的几何尺寸；仅更新 UI，无 IO 或返回值。 */
    void applySidebarDensity();

    /** @brief 当前侧栏密度；缺省为宽松，使用独立设置键持久化。 */
    SidebarDensity m_sidebarDensity = SidebarDensity::Relaxed;

    /** @brief 导航栏等紧凑相关布局把精简视同紧凑，只有主机树本身区分这两档。 */
    [[nodiscard]] bool compactLayout() const
    {
        return m_sidebarDensity != SidebarDensity::Relaxed;
    }

    /**
     * @brief 把分隔器当前的侧栏宽度写入数据库设置。
     *
     * 只在侧栏可见且宽度不小于最小值时写入。侧栏被收起时 QSplitter 会把它的尺寸压成 0，
     * 若照单保存，下次启动恢复出来的就是一条看不见的侧栏。
     */
    void persistSidebarWidth();

    /**
     * @brief 根据当前设置应用窗口级现代化视觉系统。
     * @param settings 已规范化的终端设置快照。
     *
     * 该方法只负责窗口框架、通用表单和列表的颜色/间距；终端正文仍由 TerminalPane 使用
     * 自己的终端调色板，避免 QSS 覆盖 ANSI 渲染颜色。
     */
    void applyModernAppearance(const TerminalSettings &settings);

    /**
     * @brief 在标题栏标签的右键位置打开会话菜单。
     * @param position 相对于标题栏 QTabBar 的局部坐标。
     */
    void showSessionTabContextMenu(const QPoint &position);

    /**
     * @brief 记录一条运行信息。
     *
     * 界面上不再有任何对应的提示区：消息只写入 ActivityLog 的日志文件，供事后排查。
     * @param message 面向用户的简短说明；空文本无操作。
     * @param timeoutMilliseconds 历史参数，界面提示条移除后已被忽略，仅保留调用点兼容。
     */
    void showTransientMessage(const QString &message, int timeoutMilliseconds = 5000);

    /** @brief 返回当前标签页的终端工作区。 */
    [[nodiscard]] TerminalWorkspace *activeWorkspace() const;

    /** @brief 返回当前顶层标签中的多终端工作空间；当前为单终端标签时返回空。 */
    [[nodiscard]] TerminalWorkspaceGroup *activeWorkspaceGroup() const;

    /**
     * @brief 把一个单终端顶层标签合并到目标终端或既有工作空间标签。
     * @param sourceIndex 被拖动的单终端顶层索引。
     * @param targetIndex 用户在拖动前选中的接收标签索引。
     * @param globalDropPosition 鼠标释放全局坐标，用于精确命中终端页面或工作空间侧栏。
     * @param droppedOnTargetTab true 表示直接落在目标标签；false 表示需要识别页面/侧栏落点。
     *
     * 合并只重挂现有 QWidget，不重启终端进程或 SSH 连接；AI 页面同时进入嵌套栈，确保
     * 工作空间内切换选中终端后仍显示对应会话的独立聊天记录。
     */
    void mergeTerminalTabs(int sourceIndex,
                           int targetIndex,
                           const QPoint &globalDropPosition,
                           bool droppedOnTargetTab);

    /**
     * @brief 接上工作空间成员“拖回标题栏”的手势：拖动中预告插入位置，松手后移出工作空间。
     * @param group 新建的工作空间；每个工作空间只应调用一次。
     */
    void connectWorkspaceDragOut(TerminalWorkspaceGroup *group);

    /**
     * @brief 把工作空间成员移出并恢复为独立顶层标签，会话不重建。
     * @param member 工作空间中的终端；不属于任何工作空间时忽略。
     * @param insertIndex 新标签在标题栏中的插入位置（按移出前的索引）。
     *
     * AI 页面随终端一起回到顶层栈；工作空间因此变空时整体关闭。
     */
    void detachTerminalFromWorkspace(TerminalWorkspace *member, int insertIndex);

    /** @brief 查找某个终端所属的顶层标签索引，兼容单终端与工作空间成员。 */
    [[nodiscard]] int topLevelIndexForWorkspace(TerminalWorkspace *workspace) const;

    /** @brief 根据当前顶层页面更新工作空间侧栏观察对象。 */
    void refreshWorkspaceSidebar();

    /** @brief 收集所有已打开标签的终端面板；返回值不转移所有权。 */
    [[nodiscard]] QList<TerminalPane *> allTerminalPanes() const;

    /**
     * @brief 根据当前可交互标签刷新效率工具的主机选择器，并把当前标签同步给 AI 助手。
     *
     * 两者共用同一个"当前标签变化"信号源：效率工具需要全部已连接标签的列表，AI 助手只
     * 需要当前激活的单个标签，因此在同一个方法里一并处理，不新增第二条几乎相同的信号
     * 连接链。
     */
    void refreshProductivityTargets();

    /** @brief 把设置服务中的快捷键映射到实际 QAction。 */
    void applyConfiguredShortcuts();

    /** @brief 弹出查找文本输入框并在当前终端中循环查找。 */
    void searchCurrentTerminal();

    /**
     * @brief 打开可搜索命令面板并触发选中的当前可用 QAction。
     *
     * 面板在每次打开时从窗口实际动作树采集条目，因此插件式新增菜单动作后无需维护第二份
     * 命令注册表；菜单容器、分隔符、不可见和禁用动作不会出现在结果中。外观与 Cmd+K 快速
     * 面板一致（同一个 QuickCommandPopup 的 Actions 模式），快捷键在行尾右对齐。
     */
    void openCommandPalette();

    /**
     * @brief 在当前终端上方弹出全部会话标签的切换列表。
     *
     * 复用 QuickCommandPopup 的 Tabs 模式：列出全部标签并初始选中当前标签，Cmd+1..9
     * 直接切到可见列表的第 1..9 项。标签溢出时标题栏"+"左侧的按钮触发它。
     */
    void openTabSwitcher();

    /**
     * @brief 在当前终端上方弹出命令片段快速面板（默认 Cmd+K）。
     *
     * 按 1..9 直接在弹出时的那个终端执行对应片段；没有活动终端时只提示，不弹面板。
     */
    void openQuickSnippets();

    /**
     * @brief 在当前终端上方弹出该会话主机的命令历史快速面板（默认 Cmd+J）。
     *
     * 历史按最近执行倒序、按命令去重；主机取当前活动层的稳定 id，本地 Shell 为空。
     */
    void openQuickHistory();

    /**
     * @brief 两个快速面板的共同入口：定位终端、弹出面板并把选中命令送回同一终端。
     * @param snippets true 为片段面板，false 为历史面板。
     */
    void openQuickCommandPopup(bool snippets);

    /** @brief 返回三个快速面板共用的 QuickCommandPopup，首次调用时创建并接好信号。 */
    QuickCommandPopup *ensureQuickCommandPopup();

    /** @brief 将当前终端纯文本通过 QSaveFile 原子写入用户选择的文件。 */
    void exportCurrentTerminal();

    /**
     * @brief 把当前全部标签的名称、保存主机 id 和文件管理显隐状态保存为工作区。
     *
     * 临时连接没有可重建的凭据引用，因此会拒绝保存并提示用户先把主机加入主机树；本地
     * Shell 标签可以保存为空 hostId，恢复时重新打开本地登录 Shell。
     */
    void saveCurrentWorkspace();

    /** @brief 选择并严格校验一个工作区，然后用其中的标签组合替换当前全部标签。 */
    void openSavedWorkspace();

    /** @brief 选择并删除一个持久化工作区，不影响当前已经打开的终端会话。 */
    void removeSavedWorkspace();

    /** @brief 将命令路由到指定主机 id 对应的全部已打开终端；空列表表示当前终端。 */
    void executeOnTargets(const QString &command, const QStringList &targetIds);


    TerminalWorkspace *m_workspace = nullptr;
    QTabWidget *m_tabs = nullptr;
    /**
     * @brief 工作区区域的二选一容器：品牌空态页或终端标签页。
     *
     * 空态不是"隐藏 QTabWidget"，而是一个真实页面。这样分隔器的尺寸、最小宽度和拖动
     * 行为在有无标签时完全一致，用户新建第一个标签时终端区不会突然跳一次宽度。
     */
    QStackedWidget *m_workspaceStack = nullptr;
    /** @brief 没有任何会话标签时显示的产品图标与名称页面。 */
    QWidget *m_workspacePlaceholder = nullptr;
    /** @brief 拖动会话标签离开标题栏时，覆盖在中央工作区之上的落点提示层。 */
    SessionTabDropOverlay *m_tabDropOverlay = nullptr;
    /** @brief 融入窗口标题栏的可交互会话标签，内容与 m_tabs 严格镜像。 */
    SessionTabBar *m_titleTabBar = nullptr;
    /** @brief 承载 m_titleTabBar 的裁剪视口，负责按像素滚动标签。 */
    SessionTabStrip *m_titleTabStrip = nullptr;
    /**
     * @brief 标签栏两侧的外部滚动箭头。
     *
     * 溢出时它们使用 retainSizeWhenHidden 占位，防止显隐反过来改变溢出判定而抖动；一个
     * 标签都没有时这份占位没有意义，必须连同标签栏一起彻底让出宽度。
     */
    QToolButton *m_tabScrollLeftButton = nullptr;
    QToolButton *m_tabScrollRightButton = nullptr;
    /**
     * @brief "+"左侧的标签切换列表按钮，只在标签溢出时显示。
     *
     * 它不保留隐藏占位：显示时视口变窄只会更溢出，隐藏时视口变宽只会更不溢出，显隐与
     * 溢出判定之间没有反馈回路。
     */
    QToolButton *m_tabSwitcherButton = nullptr;
    /** @brief 与原生或无边框窗口标题区合并的 52 像素会话导航容器。 */
    QWidget *m_titleChrome = nullptr;
    /**
     * @brief Windows/Linux 无边框标题栏中的最大化/还原按钮。
     *
     * macOS 继续使用原生交通灯按钮，因此该指针在 macOS 上保持为空。
     */
    QToolButton *m_windowMaximizeButton = nullptr;
    /** @brief 始终保留的窄图标导航轨，负责选择侧栏功能。 */
    QWidget *m_navigationRail = nullptr;
    /** @brief 可整体收起的单一侧栏容器。 */
    QWidget *m_sidebarPanel = nullptr;
    /**
     * @brief 连接侧栏与终端工作区的水平分隔器。
     *
     * 分隔器把侧栏最右边框变成可拖拽命中区域，同时保留终端区的最小可用宽度；隐藏侧栏
     * 时 Qt 会自动隐藏对应把手，再次展开时恢复用户上一次拖动后的宽度。
     */
    QSplitter *m_sidebarSplitter = nullptr;
    /**
     * @brief 侧栏宽度落库的防抖定时器。
     *
     * 分隔器为实时拖动，splitterMoved 会在一次拖动中发出上百次；逐次写库既拖慢拖动手感，
     * 也在磁盘上留下大量无意义事务。定时器只在拖动停下后写一次最终宽度。
     */
    QTimer *m_sidebarWidthPersistTimer = nullptr;
    /**
     * @brief 当前侧栏展开/收起动画；为空表示没有进行中的尺寸过渡。
     *
     * 使用单独的 QVariantAnimation 而不是阻塞式循环，是为了让终端、网络连接和窗口重绘在
     * 动画期间继续处理；新的显隐请求会停止旧动画并从当前实际宽度继续，避免反向操作跳变。
     */
    QVariantAnimation *m_sidebarAnimation = nullptr;
    /** @brief 最近一次有效的展开宽度，收起后用于恢复用户拖动过的宽度。 */
    int m_sidebarExpandedWidth = 0;
    /** @brief 当前显隐动画的目标状态，区别于收起动画结束前面板仍暂时可见的物理状态。 */
    bool m_sidebarTargetVisible = true;
    /** @brief 本次按下的导航按钮在按下前是否已是当前页；决定松开时是收起还是切页。 */
    bool m_navigationPressedActivePage = false;
    /** @brief 承载主机、效率工具、端口转发、AI 和工作空间五个互斥页面。 */
    QStackedWidget *m_sidebarStack = nullptr;
    /** @brief 当前侧栏页面标题。 */
    QLabel *m_sidebarTitle = nullptr;
    /** @brief 主机页标题后的数量摘要；切换到其他侧栏页面时隐藏。 */
    QLabel *m_hostCountLabel = nullptr;
    /** @brief 主机页提供、挂在侧栏标题行右侧的动作条；仅在主机页可见。 */
    QWidget *m_hostHeaderActions = nullptr;
    /** @brief 效率工具页的标题行动作条；仅在效率工具页可见，归 ProductivityWidget 管理逻辑。 */
    QWidget *m_productivityHeaderActions = nullptr;
    /** @brief 紧跟侧栏标题的效率工具视图切换器（片段 / 历史图标）；只在效率工具页显示。 */
    QWidget *m_productivityViewSwitcher = nullptr;
    /** @brief 导航轨上与三个侧栏页面一一对应的互斥按钮。 */
    QList<QToolButton *> m_navigationButtons;
    /** @brief 标题区及导航轨中需要随主题重新着色的图标按钮。 */
    QList<QToolButton *> m_chromeButtons;
    ApplicationDatabase *m_database = nullptr;
    CredentialVault *m_credentialVault = nullptr;
    HostManagerWidget *m_hostManager = nullptr;
    PortForwardManagerWidget *m_portForwardManager = nullptr;
    ProductivityModel *m_productivityModel = nullptr;
    ProductivityWidget *m_productivityWidget = nullptr;
    /** @brief Cmd+K / Cmd+J 共用的快速命令面板；首次使用时创建，由本窗口拥有。 */
    QuickCommandPopup *m_quickCommandPopup = nullptr;
    /** @brief 快速面板弹出时的目标终端；面板关闭前标签被关掉时自动置空。 */
    QPointer<TerminalPane> m_quickCommandPane;
    /** @brief 命令面板本次展示的动作，下标与面板条目一一对应；动作销毁时自动置空。 */
    QList<QPointer<QAction>> m_commandPaletteActions;
    /** @brief 导航轨“工作空间”页面，展示当前组的布局选择与终端缩略图。 */
    WorkspaceSidebarWidget *m_workspaceSidebar = nullptr;
    AiSettingsService *m_aiSettingsService = nullptr;
    /**
     * @brief 与终端 QTabWidget 索引严格同步的 AI 助手页面栈。
     *
     * 栈中每个页面都是一个独立 AiAssistantWidget；新建、移动、关闭终端标签时必须执行
     * 相同的插入、移动、删除操作，currentIndex 也必须同步，防止显示其他标签的聊天记录。
     */
    QStackedWidget *m_aiAssistantStack = nullptr;
    TerminalSettingsService *m_settingsService = nullptr;
    QTimer *m_vaultIdleTimer = nullptr;

    /** @brief 启动后首次自动检查更新的延迟。 */
    static constexpr int kStartupUpdateCheckDelayMs = 8000;
    /** @brief 窗口长期打开时的自动检查周期：一天。 */
    static constexpr int kPeriodicUpdateCheckIntervalMs = 24 * 60 * 60 * 1000;
    /** @brief 后台与菜单“检查更新”共用的检查器。 */
    UpdateChecker *m_updateChecker = nullptr;
    QTimer *m_updateCheckTimer = nullptr;
    /** @brief 当前打开的更新提示框；关闭后自动置空，用于避免重复弹出。 */
    QPointer<QMessageBox> m_updatePrompt;

    /** @brief 标题栏最左侧的侧栏显隐动作。 */
    QAction *m_sidebarToggleAction = nullptr;
    /** @brief 当前侧栏页索引，用于收起后恢复到用户最后使用的功能。 */
    int m_activeSidebarPage = 0;
    /** @brief 防止重复桥接 macOS NSWindow；false 时 showEvent 会继续安全重试。 */
    bool m_macWindowChromeConfigured = false;

    /** @brief 正在连接但尚未认证成功的面板与持久化主机 id。 */
    QHash<TerminalPane *, QString> m_pendingHistoryHosts;

    /** @brief 连接按钮触发时的 UTC 时间，成功和失败历史都以此为起点。 */
    QHash<TerminalPane *, QDateTime> m_pendingHistoryStartedAt;

    /** @brief 已成功连接且尚未写入断开时间的面板与连接历史行 id。 */
    QHash<TerminalPane *, qint64> m_openHistoryRows;

    /**
     * @brief 正在连接、且由"通过主机链连接"对话框手动指定了跳板的面板与该跳板 id。
     *
     * 只有认证成功后才会据此把跳板记为目标主机的"最近连接"；失败或面板销毁时直接
     * 丢弃，绝不能把未验证成功的跳板选择当作历史结果持久化。
     */
    QHash<TerminalPane *, QString> m_pendingChainJumpHosts;

    /**
     * @brief connectSavedHostViaChain() 到 connectSavedHost() 之间暂存跳板 id 的临时变量。
     *
     * connectSavedHost() 从发起连接到拿到目标 TerminalPane* 之间要经过标签页创建、凭据
     * 解锁等多步同步调用，中途没有让出事件循环的机会，因此可以安全地用单个成员而不是
     * 按面板建表暂存；调用结束后立即清空，不会跨越两次连接请求。
     */
    QString m_pendingChainJumpHostId;

    /**
     * @brief 每个终端面板最近一次连接状态，取值对齐 SessionTabBar::ConnectionState。
     *
     * 顶层标签的状态点按标签索引维护，工作空间侧栏需要按成员逐个展示，因此单独按面板记录；
     * 面板销毁时清除。缺省视为 Unknown。
     */
    QHash<TerminalPane *, int> m_paneConnectionState;

    /** @brief 稳定设置动作 id 到主窗口 QAction 的映射，设置变化时统一更新快捷键。 */
    QHash<QString, QAction *> m_configurableActions;

    /**
     * @brief 语言切换后需要重设文案的菜单项与菜单标题及其源字符串。
     *
     * 菜单栏是常驻控件，切换语言时 Qt 会向本窗口投递 QEvent::LanguageChange，此处据此逐条
     * 用当前翻译器重译。按需弹出的对话框不登记：它们下次打开时会以新语言重新构造。
     */
    struct TranslatableUiEntry {
        QAction *action = nullptr;
        QMenu *menu = nullptr;
        const char *text = nullptr;
        const char *toolTip = nullptr;
    };
    QVector<TranslatableUiEntry> m_translatableUi;

    /** @brief 界面语言服务；可为空（如单元测试），此时仅保留被动重译能力。 */
    LocalizationService *m_localization = nullptr;

    /** @brief 记录一个菜单动作及其源文案，供语言切换时重译；同时设置初始文案。 */
    void registerTranslatable(QAction *action,
                              const char *sourceText,
                              const char *sourceToolTip = nullptr);

    /** @brief 记录一个菜单及其源标题，供语言切换时重译；同时设置初始标题。 */
    void registerTranslatable(QMenu *menu, const char *sourceTitle);

    /** @brief 用当前翻译器重设所有已登记的常驻菜单文案。 */
    void retranslateUi();

    /**
     * @brief 把设置里的语言偏好应用到运行中的翻译器。
     * @param storedLanguage TerminalSettings::language 的原始值（"system" 或具体码）。
     *
     * 仅当解析后的具体语言与当前语言不同才切换：切换会触发 QEvent::LanguageChange 让菜单
     * 就地重译，并首次弹出一次“部分界面需重开或重启后完全生效”的提示。m_localization
     * 为空时静默跳过。
     */
    void applyLanguageSetting(const QString &storedLanguage);
};
