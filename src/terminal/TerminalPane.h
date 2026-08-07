#pragma once

#include "core/ConnectionTarget.h"

#include <QVector>
#include <QStringList>
#include <QWidget>

class QLabel;
class LocalShellWorker;
class SshSessionStack;
class TerminalView;

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
     * @brief 在当前连接之上由本程序亲自建立一层嵌套会话，终端随后切换到该层。
     * @param target 内层服务器；host/port 由当前栈顶服务器的视角解析。
     *
     * 与用户在终端里手打 ssh 不同，这条路径建立的层是确定事实：层数、身份、退出时机
     * 都由本程序掌握，不依赖对终端输出的任何解析。
     */
    void connectNestedHost(const ConnectionTarget &target);

    /** @brief 是否已有可用于承载嵌套连接的已就绪远程会话。 */
    [[nodiscard]] bool canPushNestedLayer() const;

    /** @brief 在当前面板中启动本地登录式 zsh，用于本地运维和终端功能测试。 */
    void openLocalShell(const QString &shellPath = QStringLiteral("/bin/zsh"));

    /** @brief 返回该面板中已识别的外层到内层服务器链。 */
    [[nodiscard]] QList<ConnectionTarget> targets() const;

    /** @brief 返回连接链最内层目标；尚未连接时返回无效目标。 */
    [[nodiscard]] ConnectionTarget activeTarget() const;

    /** @brief 判断当前远程或本地终端是否已经完成启动并可交互。 */
    [[nodiscard]] bool isConnected() const;

    /** @brief 把键盘焦点交给终端显示区。 */
    void focusTerminal();

signals:
    /** @brief 嵌套 SSH 进入或退出导致可选服务器链发生变化。 */
    void targetsChanged(TerminalPane *pane);

    /** @brief 连接状态文字发生变化，供状态栏展示。 */
    void statusMessage(const QString &message);

private slots:
    /** @brief 根据用户提交的命令更新嵌套服务器链。 */
    void inspectSubmittedCommand(const QString &command);

private:
    /** @brief 停止并销毁旧工作线程，供重连和析构共用。 */
    void stopWorker();

    /** @brief 按当前所在层刷新标题，并把推测层显式标注出来。 */
    void refreshHeaderLabel();

    /** @brief 向当前栈顶的远端 Shell 注入 ssh 接管函数。 */
    void injectShellIntegration();

    /**
     * @brief 处理远端 shim 上报的事件，决定是否接管这次 ssh。
     * @param payload VtermBridge 交出的 OSC 载荷。
     *
     * 远端 shim 正阻塞等待回执，因此本方法的每条分支都必须给出答复，否则用户终端会挂住。
     */
    void handleShellIntegrationEvent(const QByteArray &payload);

    /**
     * @brief 撤销尚未确认的嵌套推测层，用于 ssh 失败或被用户取消。
     * @param reason 供状态栏展示的简短原因。
     *
     * 失败的 ssh 如果不回滚，就会在连接链上留下一个用户从未真正进入的幽灵层；文件管理器
     * 随后会把它当作可操作的服务器，而它往往和真实层同名，界面上根本分辨不出来。
     */
    void rollBackPendingNestedTarget(const QString &reason);

    QLabel *m_headerLabel = nullptr;
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

    QStringList m_tunnelRouteIds;

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
     * @brief 是否向远端 Shell 注入 ssh 接管函数。
     *
     * 注入会覆盖用户在该会话中的 ssh 命令，属于对远端环境的可见改动，因此保留开关。
     * 关闭后嵌套识别退回命令解析的降级路径，推测层会被显式标注为不确定。
     */
    bool m_shellIntegrationEnabled = true;

    /**
     * @brief 当前层的密码提示语；非空表示正在等待用户输入密码。
     *
     * 用户在提示处直接回车时据此原样再问一遍，与 OpenSSH 行为一致。
     */
    QString m_pendingCredentialPrompt;
};
