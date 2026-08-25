#pragma once

#include "core/ConnectionTarget.h"
#include "terminal/TerminalEchoFilter.h"

#include <QColor>
#include <QHash>
#include <QSet>
#include <QVector>
#include <QStringList>
#include <QWidget>

class ApplicationDatabase;
class QLabel;
class LocalShellWorker;
class SshSessionStack;
class TerminalView;
class QTimer;
class QToolButton;
struct TerminalSettings;

/**
 * @brief 一个标签页内的终端面板，可独立连接和关闭。
 *
 * 面板通过 libssh 工作线程建立交互式 PTY，支持密码、私钥、Agent、known_hosts 和跳板链。
 * 它维护本标签页内的服务器连接链，供远程文件管理器自由选择外层或内层服务器。
 */
class TerminalPane final : public QWidget {
    Q_OBJECT

public:
    explicit TerminalPane(QWidget *parent = nullptr);
    ~TerminalPane() override;

    /** @brief 使用目标配置启动新的 SSH 工作线程；已有会话会先被安全终止。 */
    void connectToHost(const ConnectionTarget &target);

    /**
     * @brief 按受管主机链启动新的 SSH 工作线程。
     * @param connectionPlan 从本机第一跳到最终交互目标的有序列表；中间节点仅用作跳板。
     *
     * 自动重连会保留整条计划，而不是只重连最后一个内网地址；否则断线后会绕过跳板从
     * 本机误连同名私网主机，属于必须在接口边界避免的安全问题。
     */
    void connectToHosts(const QList<ConnectionTarget> &connectionPlan);

    /**
     * @brief 设置下一次受管主机连接是否需要后台识别远端发行版。
     * @param enabled true 表示认证成功后读取一次 /etc/os-release，false 表示跳过。
     *
     * 这是一次性连接参数而不是持久化偏好：MainWindow 只有在数据库尚未存有该主机的发行版
     * 时才开启它，connectToHosts() 读取后立即清除，避免之后的临时连接或自动重连重复探测。
     */
    void setRemoteDistributionDetectionEnabled(bool enabled);

    /**
     * @brief 在当前连接之上由本程序亲自建立一层嵌套会话，终端随后切换到该层。
     * @param target 内层服务器；host/port 由当前栈顶服务器的视角解析。
     *
     * 与用户在终端里手打 ssh 不同，这条路径建立的层是确定事实：层数、身份、退出时机
     * 都由本程序掌握，不依赖对终端输出的任何解析。
     *
     * @param outerShimSuspended 该层是否来自接管用户手打的 ssh。为真时外层 Shell 里的 shim
     *        正挂起等待，本层结束时必须放行它；从菜单发起的建层没有这样的等待方。
     */
    void connectNestedHost(const ConnectionTarget &target, bool outerShimSuspended = false);

    /** @brief 是否已有可用于承载嵌套连接的已就绪远程会话。 */
    [[nodiscard]] bool canPushNestedLayer() const;

    /** @brief 在当前面板中启动本地登录式 zsh，用于本地运维和终端功能测试。 */
    void openLocalShell(const QString &shellPath = QStringLiteral("/bin/zsh"));

    /** @brief 返回该面板中已识别的外层到内层服务器链。 */
    [[nodiscard]] QList<ConnectionTarget> targets() const;

    /** @brief 返回连接链最内层目标；尚未连接时返回无效目标。 */
    [[nodiscard]] ConnectionTarget activeTarget() const;

    /**
     * @brief 返回当前活动层最近一次由 Shell 集成上报的工作目录。
     * @return 该层尚未上报过 cwd（shim 未就绪、推测层没有 shim 或提示符还未刷新过一次）
     *         时返回空字符串，调用方必须视为"未知"而不是根目录。
     *
     * 与 remoteWorkingDirectoryChanged 信号只广播"变化"不同，本方法提供拖拽上传等需要在
     * 任意时刻同步查询"此刻在哪"的场景，不必先订阅信号再等待下一次提示符刷新。
     */
    [[nodiscard]] QString activeWorkingDirectory() const;

    /**
     * @brief 返回当前会话赖以建立的受管连接计划；本地 Shell 或尚未连接时为空。
     *
     * 与 targets() 不同，这里保留受管跳板层、剔除推测层，因此可以原样交给另一个面板
     * 重新拨号——复制标签正是靠它避免再走一遍主机链解析与保险库解锁。
     */
    [[nodiscard]] QList<ConnectionTarget> connectionPlan() const;

    /** @brief 判断当前远程或本地终端是否已经完成启动并可交互。 */
    [[nodiscard]] bool isConnected() const;

    /**
     * @brief 进入"等待用户决定"的断线恢复状态：终端只接受 r（重连）和 x（关闭）。
     * @param target 断线前的根连接目标；按 r 时按原受管计划整链重连。
     * @param reason 已经展示给用户的断线原因，作为提示的第一行再复述一次。
     *
     * 自动重连耗尽后不再擅自启动本地 zsh。本地 Shell 与刚刚断开的服务器毫无关系，却会
     * 顶掉屏幕上的远端输出，用户下一条命令很可能就打在了本机上；把决定权交回用户，既
     * 保留了现场，也保留了"再试一次"这个最可能的意图。
     */
    void beginManualRecovery(const ConnectionTarget &target, const QString &reason);

    /** @brief 当前是否停在断线后的手动恢复提示上（等待 r 或 x）。 */
    [[nodiscard]] bool isAwaitingManualRecovery() const;

    /** @brief 把键盘焦点交给终端显示区。 */
    void focusTerminal();

    /**
     * @brief 返回当前根连接的受控 SSH 会话栈，供端口转发运行时使用。
     * @return 当前根会话栈对象仍存在时返回非空；尚未创建或已清理时返回 nullptr。
     *
     * 返回值不转移所有权。调用方只能使用 SshSessionStack 明确标注为跨线程安全的转发接口，
     * 不得直接访问 libssh 句柄。该方法在断线信号中仍返回准确对象，是为了让管理器按对象
     * 身份注销旧运行时；真正启动新转发仍只能发生在 rootConnectionSucceeded 之后。
     */
    [[nodiscard]] SshSessionStack *forwardingSessionStack() const;

    /**
     * @brief 注入命令历史数据库并立即加载当前主机的补全候选。
     * @param database 已初始化数据库，生命周期必须覆盖本面板；传 nullptr 可关闭持久化。
     *
     * TerminalPane 不拥有数据库。显式注入而不是使用全局单例，可让测试使用隔离的内存
     * 数据库，也避免终端层隐式打开第二条 SQLite 连接。
     */
    void setCommandHistoryDatabase(ApplicationDatabase *database);

    /** @brief 把应用级终端设置应用到视图和面板标题栏。 */
    void applyTerminalSettings(const TerminalSettings &settings);

    /** @brief 在终端输出中查找文本；到达边界时由视图自动环回。 */
    bool findTerminalText(const QString &text, bool backward = false);

    /** @brief 返回当前终端的纯文本输出，供主窗口原子导出。 */
    [[nodiscard]] QString terminalPlainText() const;

    /**
     * @brief 同步会话顶部远程文件按钮的选中状态。
     * @param visible true 表示本标签的文件管理区正在显示，false 表示已收起。
     *
     * 该方法只更新按钮展示，不直接操作分隔器；真正的显隐状态由所属
     * TerminalWorkspace 持有，从而保证每个标签页互不干扰。
     */
    void setFileManagerVisible(bool visible);

    /**
     * @brief 在会话状态栏、紧邻服务器名之后，短暂展示一条状态文字。
     * @param message 待展示的文字；空字符串立即清空当前显示。
     *
     * 这条消息只与本标签页有关（如"正在读取主目录…"），因此展示位置就在产生它的会话
     * 旁边，而不是窗口标题栏的全局提示区——后者用于设置、保险库这类与具体标签页无关的
     * 应用级消息，两者混在一起会让用户分不清一条提示到底说的是哪个会话。
     */
    void showTransientStatus(const QString &message);

    /**
     * @brief 在与 showTransientStatus 相同的位置展示一次文件传输结果，并带上成功/失败图标。
     * @param success true 显示绿色对勾，false 显示红色警示。
     * @param message 传输结果文字，例如"上传 a.txt → /root"；空字符串立即清空当前显示。
     *
     * 沿用 showTransientStatus 的 8 秒自动消失定时器，只是额外在文字前渲染一个图标——
     * 拖拽上传、右键菜单上传、面板自身的上传按钮触发的传输，最终都会经这里展示结果，
     * 因此三者的自动消失时机天然保持一致，不需要分别维护。
     */
    void showTransientTransferResult(bool success, const QString &message);

public slots:
    /**
     * @brief 向当前 SSH 或本地 PTY 写入原始字节，不产生新的广播源信号。
     * @param bytes 已按终端协议编码的数据。
     *
     * 广播路由必须调用该槽，而不能重新注入 TerminalView::inputBytes，否则多个已开启广播
     * 的面板会互相回送并形成无限循环。
     */
    void sendInputBytes(const QByteArray &bytes);

    /**
     * @brief 在当前终端执行完整命令，供 Snippet 和历史重放使用。
     * @param command 不带结尾换行的命令。
     *
     * 该入口会同时经过命令历史和嵌套 SSH 识别；它不是简单的 worker 写入快捷方式。
     */
    void executeCommand(const QString &command);

    /**
     * @brief 把命令插入当前终端输入行但不执行，供 AI 助手“插入/修改”动作使用。
     * @param command 不含结尾回车的单行命令。
     *
     * 该入口委托 TerminalView 同步 PTY 字节与当前命令状态，不能用 sendInputBytes() 代替。
     */
    void insertCommandText(const QString &command);

signals:
    /**
     * @brief 用户从会话顶部状态栏请求显示或隐藏远程文件管理区。
     * @param visible true 表示展开，false 表示收起。
     *
     * 按钮不直接查找 MainWindow 的“当前标签”，避免菜单或快捷键切换标签时误操作其他
     * 会话；所属 TerminalWorkspace 会把请求严格路由到自己的文件管理器。
     */
    void fileManagerVisibilityRequested(bool visible);

    /**
     * @brief 根 SSH 栈即将停止，端口转发必须在该对象失效前同步结束网络线程。
     * @param sessionStack 已收到 requestStop、但仍然有效的准确栈对象。
     *
     * 先置停止标志是为了唤醒可能等待建隧道结果的转发线程；信号仍早于对象析构，因此
     * 管理器可以同步结束 Runtime，并安全移除对该栈的全部非拥有引用。
     */
    void forwardingSessionAboutToClose(SshSessionStack *sessionStack);

    /** @brief 嵌套 SSH 进入或退出导致可选服务器链发生变化。 */
    void targetsChanged(TerminalPane *pane);

    /**
     * @brief 当前自持层的远端 Shell 工作目录发生变化。
     * @param stableKey 上报该 cwd 的服务器稳定键（activeTarget().stableKey()）。
     * @param workingDirectory 远端 Shell 提示符刷新时上报的绝对路径。
     *
     * 只有由 Shellium 亲自注入 shim 的自持层才会产生该信号；推测层没有 shim，不会污染
     * 文件管理器的自动跟随。
     */
    void remoteWorkingDirectoryChanged(const QString &stableKey, const QString &workingDirectory);

    /** @brief 连接状态文字发生变化，供主窗口顶部非阻塞提示区展示。 */
    void statusMessage(const QString &message);

    /**
     * @brief 当前终端视图产生了待发送的原始输入，可作为实时广播的源。
     * @param source 产生输入的面板；上层用它排除源自身。
     * @param bytes 键盘、粘贴、输入法或 Snippet 提交产生的原始终端字节。
     *
     * sendInputBytes 不会再次发出本信号，因此广播接收端不会形成回路。
     */
    void broadcastSourceInput(TerminalPane *source, const QByteArray &bytes);

    /**
     * @brief 用户在断线恢复提示中按下 x，要求关闭该终端所在的标签页。
     * @param pane 发出请求的面板；上层据此定位标签索引。
     *
     * 面板不自行 close()：它只是标签页里的一个子控件，销毁自己会留下一个空标签和一份
     * 仍在栈中的 AI 助手页面。关闭动作必须回到统一管理标签生命周期的主窗口。
     */
    void closeRequested(TerminalPane *pane);

    /** @brief 一条非敏感命令成功写入持久历史，效率侧栏可按主机刷新。 */
    void commandHistoryChanged(const QString &hostId);

    /**
     * @brief 根 SSH 会话完成认证并进入可交互状态。
     * @param target 本次根连接使用的完整目标快照。
     *
     * 该信号提供给连接历史等业务层使用；上层不得解析 statusMessage 的本地化文本来判断
     * 成功。嵌套层成功由 targetsChanged 表达，不会重复触发本信号。
     */
    void rootConnectionSucceeded(const ConnectionTarget &target);

    /**
     * @brief 当前根主机的远端发行版已由后台只读命令识别。
     * @param target 本次成功连接的最终目标快照。
     * @param distributionId /etc/os-release 的规范化 ID。
     * @param versionId /etc/os-release 的 VERSION_ID，可为空。
     *
     * TerminalPane 不直接访问数据库，保证终端层在测试与临时会话中仍可独立复用；主窗口负责
     * 核验 target.id 是否对应保存的主机，再将结果持久化并刷新侧栏。
     */
    void remoteDistributionDetected(const ConnectionTarget &target,
                                    const QString &distributionId,
                                    const QString &versionId);

    /**
     * @brief 异常断线退避结束、即将发起一次自动重连。
     * @param target 与原成功会话相同的根目标快照。
     * @param attempt 当前是本轮第几次尝试，从 1 开始。
     * @param maximumAttempts 当前设置允许的最大次数。
     *
     * 业务层可据此为本次尝试建立独立连接历史；不要从状态栏文本推断重连开始。
     */
    void automaticReconnectStarted(const ConnectionTarget &target,
                                   int attempt,
                                   int maximumAttempts);

    /**
     * @brief 根 SSH 的建立或运行过程发生致命错误。
     * @param target 本次根连接使用的完整目标快照。
     * @param message libssh、主机校验或认证流程返回的可读错误。
     *
     * 建立阶段失败和已连接后的网络/写入错误都会走该信号；主动停止旧会话不会发出失败。
     * 会话代次校验还会丢弃旧工作线程延迟到达的错误，避免污染新连接的历史。
     */
    void rootConnectionFailed(const ConnectionTarget &target, const QString &message);

    /**
     * @brief 已经成功的根 SSH 会话由远端结束。
     * @param target 已结束会话的根连接目标快照。
     * @param exitStatus 远端 Shell 退出码；服务器未发送退出状态或网络异常时为 -1。
     *
     * 主动切换会话和面板析构会先清除 established 状态，因此不会产生伪断开记录；是否把
     * -1 归类为异常由持久化层决定。
     */
    void rootConnectionDisconnected(const ConnectionTarget &target, int exitStatus);

private slots:
    /** @brief 根据用户提交的命令更新嵌套服务器链。 */
    void inspectSubmittedCommand(const QString &command);

private:
    /**
     * @brief 在手动恢复提示状态下解释键盘输入。
     * @param bytes 来自 TerminalView 的原始输入字节。
     *
     * 此时没有任何 PTY 在运行，字节无处可去；不拦截的话按键会被静默丢弃，用户看到的是
     * 一个完全没有反应的终端。
     */
    void handleManualRecoveryInput(const QByteArray &bytes);

    /** @brief 停止并销毁旧工作线程，供重连和析构共用。 */
    void stopWorker();

    /** @brief 按当前所在层刷新标题，并把推测层显式标注出来。 */
    void refreshHeaderLabel();

    /**
     * @brief 处理远端 shim 上报的事件，决定是否接管这次 ssh。
     * @param payload VtermBridge 交出的 OSC 载荷。
     *
     * 远端 shim 正阻塞等待回执，因此本方法的每条分支都必须给出答复，否则用户终端会挂住。
     */
    void handleShellIntegrationEvent(const QByteArray &payload);

    /** @brief 为第 2 层起的自持层注册隧道路由，供文件管理器经外层抵达。 */
    void registerOwnedLayerRoutes();

    /**
     * @brief 接管失败时把用户原本的 ssh 命令交还给远端执行。
     * @param reason 供终端提示的简短原因。
     *
     * 不这样做，用户敲下的命令就凭空消失了：shim 收到 take 便直接返回，而我们又没能建成
     * 连接。他重试多少次都一样，因为每次都会走同一条失败路径。
     */
    void runTakeoverFallback(const QString &reason);

    /**
     * @brief 撤销尚未确认的嵌套推测层，用于 ssh 失败或被用户取消。
     * @param reason 供顶部非阻塞提示区展示的简短原因。
     *
     * 失败的 ssh 如果不回滚，就会在连接链上留下一个用户从未真正进入的幽灵层；文件管理器
     * 随后会把它当作可操作的服务器，而它往往和真实层同名，界面上根本分辨不出来。
     */
    void rollBackPendingNestedTarget(const QString &reason);

    /** @brief 把非凭据命令按提交前所在主机写入历史，并刷新补全候选。 */
    void recordSubmittedCommand(const QString &command);

    /** @brief 从当前主机最近历史生成去重候选并交给 TerminalView。 */
    void refreshCommandCompletions();

    /**
     * @brief 为异常断线安排下一次有限指数退避重连。
     * @param target 原成功根连接；保留原凭据仅用于本轮恢复。
     * @param sessionGeneration 安排任务时的会话代次，用户切换会话后任务自动失效。
     * @return 已安排下一次尝试返回 true；功能关闭或次数耗尽返回 false，并回退本地 Shell。
     */
    bool scheduleAutomaticReconnect(const ConnectionTarget &target,
                                    quint64 sessionGeneration);
    /** @brief 会话顶部状态栏容器，承载服务器、终端尺寸和文件管理开关。 */
    QWidget *m_headerBar = nullptr;

    /** @brief 当前会话或连接阶段说明，位于状态栏左侧。 */
    QLabel *m_headerLabel = nullptr;

    /**
     * @brief 紧邻服务器名之后的短暂状态文字，如"正在读取主目录…"。
     *
     * 与 m_headerLabel 分开是因为两者生命周期不同：后者反映当前连接阶段，只在阶段真正
     * 变化时更新；前者是转瞬即逝的操作提示，需要独立的清空定时器，不能覆盖或打断
     * 连接状态文字。
     */
    QLabel *m_statusMessageLabel = nullptr;

    /**
     * @brief 紧贴在 m_statusMessageLabel 前面的传输结果图标，平时不占空间。
     *
     * 只有 showTransientTransferResult 会点亮它；showTransientStatus 展示的普通提示
     * （如"正在读取主目录…"）没有明确的成功/失败语义，继续保持纯文字。
     */
    QLabel *m_statusMessageIcon = nullptr;

    /** @brief 清空 m_statusMessageLabel/m_statusMessageIcon 的定时器；每次新消息到达都会重新启动。 */
    QTimer *m_statusMessageTimer = nullptr;

    /** @brief 当前终端编码和列×行尺寸，随 PTY 尺寸信号实时更新。 */
    QLabel *m_terminalMetaLabel = nullptr;

    /** @brief 当前标签专属的远程文件管理显隐按钮，位于状态栏最右侧。 */
    QToolButton *m_fileManagerButton = nullptr;

    /** @brief 状态栏图标线条色，随终端主题更新，供文件管理按钮重绘图标使用。 */
    QColor m_headerIconColor = QColor(QStringLiteral("#e6e6e6"));

    /**
     * @brief 按当前显隐状态重绘远程文件管理按钮图标。
     *
     * 显示与隐藏必须是两个不同的图形，只靠 checked 底色区分在浅色主题下几乎看不出来，
     * 用户无法判断点击后会发生什么。
     */
    void updateFileManagerButtonIcon();

    TerminalView *m_terminalView = nullptr;
    SshSessionStack *m_sessionStack = nullptr;
    LocalShellWorker *m_localWorker = nullptr;
    /**
     * @brief 由本程序亲自建立并认证的会话层，从外层到内层排列。
     *
     * 内容完全由 SshSessionStack::layersChanged 驱动，是连接链的权威部分：层数与身份都是
     * 确定事实，confidence 恒为 Confirmed。
     */
    QVector<ConnectionTarget> m_ownedTargets;

    /**
     * @brief 由解析终端命令推测出的附加层，叠在自持层之上。
     *
     * 这是降级路径：用户绕过嵌套连接入口、直接在终端里手打 ssh 时才会产生。它可能与远端
     * 真实状态不一致，因此 confidence 恒为 Inferred，界面必须把这种不确定性显式呈现。
     * 自持层一旦变化就整体作废——底座变了，叠在上面的推测不再有任何依据。
     */
    QVector<ConnectionTarget> m_inferredTargets;
    bool m_shellReady = false;

    /** @brief 仅作用于下一次 connectToHosts() 的发行版探测开关。 */
    bool m_remoteDistributionDetectionEnabled = false;

    /**
     * @brief 按服务器稳定键缓存的最近已知远端工作目录。
     *
     * remoteWorkingDirectoryChanged 只在变化时广播；这里额外缓存一份是为了让
     * activeWorkingDirectory() 能同步应答"此刻在哪"，供拖拽上传这类不订阅信号、只在
     * 事件发生瞬间查询一次的场景使用。层切换不会清空旧条目，同一层再次成为活动层时
     * 仍可复用上一次上报的位置，直到下一次提示符刷新覆盖它。
     */
    QHash<QString, QString> m_lastKnownCwd;

    /**
     * @brief 当前 SSH 工作线程是否至少成功建立过一次交互式 Shell。
     *
     * 只有该状态为 true 后发生的断开才自动退回本地 zsh；连接阶段失败、主动切换会话和
     * 析构清理都保持 false，避免错误启动新的本地终端。
     */
    bool m_remoteSessionEstablished = false;

    /**
     * @brief 当前终端的会话代次，用于丢弃旧工作线程延迟到达的断开回调。
     *
     * 每次主动连接 SSH 或打开本地 zsh 都递增。自动回退任务执行前必须核对代次，防止
     * 用户已经开始新连接后，旧 SSH 的排队信号又把界面切回本地终端。
     */
    quint64 m_sessionGeneration = 0;

    /** @brief 是否停在断线后的手动恢复提示上；为真时终端只接受 r 和 x。 */
    bool m_awaitingManualRecovery = false;
    /** @brief 手动恢复提示对应的根目标；受管计划为空时作为重连兜底。 */
    ConnectionTarget m_manualRecoveryTarget;

    /** @brief 当前设置是否允许异常网络断线后自动重连。 */
    bool m_automaticReconnectEnabled = true;
    /** @brief 单个断线恢复周期的最大尝试次数。 */
    int m_reconnectMaxAttempts = 3;
    /** @brief 指数退避首轮秒数。 */
    int m_reconnectBaseDelaySeconds = 1;
    /** @brief 当前恢复周期已经发起/安排的尝试次数。 */
    int m_reconnectAttempts = 0;
    /** @brief 是否处于由一次已建立会话异常断线触发的恢复周期。 */
    bool m_reconnectCycleActive = false;
    /** @brief 定时任务取消令牌；用户主动切换会话时递增，使旧 lambda 安全失效。 */
    quint64 m_reconnectToken = 0;
    /** @brief 防止自动重连内部调用 connectToHost 时被误判成用户主动连接。 */
    bool m_automaticReconnectInvocation = false;

    /** @brief 当前逻辑连接的完整受管计划，供异常断线后的整链重连复用。 */
    QList<ConnectionTarget> m_connectionPlan;

    QStringList m_tunnelRouteIds;

    /** @brief 为自持层注册的隧道路由，随层栈变化整体重建。 */
    QStringList m_ownedRouteIds;

    /**
     * @brief 正在等待认证结果的内层目标 ID。
     *
     * 只有用户刚提交了可解析的嵌套 ssh 命令时才设置，避免把 sudo、数据库等无关密码
     * 误用于 SFTP。捕获完成、免密观察窗口结束或终端关闭时立即清空。
     */
    QString m_pendingNestedCredentialTargetId;

    /**
     * @brief 是否应把下一次回车提交的终端输入复用为内层 SFTP 会话密码。
     *
     * 该标志仅由明确的 SSH password 提示触发；密码仍照常发送给远端交互式 ssh，程序
     * 只在内存中取得同一份输入，不要求用户再次输入。
     */
    bool m_captureNextLineAsCredential = false;

    /**
     * @brief 当前层的密码提示语；非空表示正在等待用户输入密码。
     *
     * 用户在提示处直接回车时据此原样再问一遍，与 OpenSSH 行为一致。
     */
    QString m_pendingCredentialPrompt;

    /**
     * @brief 扣除 Shellium 自己写入远端后被回显回来的字节。
     *
     * 注入行与 take/pass 回执都是本程序发出的，用户不该在屏幕上看到它们。
     */
    TerminalEchoFilter m_echoFilter;

    /** @brief 当前在途接管对应的内层目标 id；空表示没有接管在进行。 */
    QString m_takeoverTargetId;

    /** @brief 该次接管拦下的原始 ssh 参数，失败时用于原样补发。 */
    QStringList m_takeoverArguments;

    /** @brief 发起该次接管的外层服务器键，用于记录其转发策略。 */
    QString m_takeoverOuterKey;

    /**
     * @brief 已知禁用 TCP 转发的服务器。
     *
     * 自持层依赖 direct-tcpip；服务器设了 AllowTcpForwarding no 就无法建立。记下来之后
     * 直接放行原生 ssh，不再让用户反复撞同一堵墙。
     */
    QSet<QString> m_forwardingProhibitedHosts;

    /**
     * @brief 非拥有的应用数据库，用于按主机持久化命令历史。
     *
     * 面板析构前数据库必须仍然有效。为空时终端仍可工作，只是不记录历史和提供持久补全。
     */
    ApplicationDatabase *m_commandHistoryDatabase = nullptr;
};
