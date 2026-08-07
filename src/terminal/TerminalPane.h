#pragma once

#include "core/ConnectionTarget.h"

#include <QVector>
#include <QStringList>
#include <QWidget>

class QLabel;
class LocalShellWorker;
class SshTerminalWorker;
class TerminalView;

/**
 * @brief 一个可独立连接、关闭并被分屏管理的终端面板。
 *
 * 面板通过 libssh 工作线程建立交互式 PTY，支持密码、私钥、Agent、known_hosts 和跳板链。
 * 它同时观察用户提交的 ssh/exit 命令，维护当前面板内的服务器连接链，供远程文件管理器
 * 自由选择外层或内层服务器。
 */
class TerminalPane final : public QWidget {
    Q_OBJECT

public:
    explicit TerminalPane(QWidget *parent = nullptr);
    ~TerminalPane() override;

    /** @brief 使用目标配置启动新的 SSH 工作线程；已有会话会先被安全终止。 */
    void connectToHost(const ConnectionTarget &target);

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
    /** @brief 用户在当前分屏中激活了该面板。 */
    void activated(TerminalPane *pane);

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

    QLabel *m_headerLabel = nullptr;
    TerminalView *m_terminalView = nullptr;
    SshTerminalWorker *m_worker = nullptr;
    LocalShellWorker *m_localWorker = nullptr;
    QVector<ConnectionTarget> m_targetChain;
    bool m_shellReady = false;

    /**
     * @brief 当前 SSH 工作线程是否至少成功建立过一次交互式 Shell。
     *
     * 只有该状态为 true 后发生的断开才自动退回本地 zsh；连接阶段失败、主动切换会话和
     * 析构清理都保持 false，避免错误启动新的本地终端。
     */
    bool m_remoteSessionEstablished = false;

    /**
     * @brief 当前分屏的会话代次，用于丢弃旧工作线程延迟到达的断开回调。
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
};
