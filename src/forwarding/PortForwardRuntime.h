#pragma once

#include "forwarding/PortForwardRule.h"

#include <QObject>

class QThread;
class SshSessionStack;
class PortForwardWorker;

/**
 * @brief 一条端口转发规则的独立运行时。
 *
 * 每条规则拥有自己的 Qt 网络工作线程，因此本地监听、SOCKS5 协商、TCP 双向搬运和反向
 * 转发本机回连都不会阻塞 GUI。所有 libssh 调用仍通过 SshSessionStack 的线程安全接口
 * 排队到 SSH 工作线程，绝不从网络线程直接触碰 ssh_session。
 *
 * SshSessionStack 为非拥有指针。上层必须在终端销毁或切换根会话前调用 stop() 并销毁本
 * 运行时；TerminalPane::forwardingSessionAboutToClose 专门用于维护这条生命周期边界。
 */
class PortForwardRuntime final : public QObject {
    Q_OBJECT

public:
    /**
     * @brief 创建尚未启动的规则运行时。
     * @param rule 已通过 PortForwardRule::isValid() 的规则快照。
     * @param sessionStack 与规则 hostId 对应的根 SSH 会话栈；生命周期见类说明。
     * @param parent Qt 父对象。
     */
    explicit PortForwardRuntime(const PortForwardRule &rule,
                                SshSessionStack *sessionStack,
                                QObject *parent = nullptr);
    ~PortForwardRuntime() override;

    PortForwardRuntime(const PortForwardRuntime &) = delete;
    PortForwardRuntime &operator=(const PortForwardRuntime &) = delete;

    /** @brief 返回不可变规则快照；编辑规则时应停止旧运行时并创建新实例。 */
    [[nodiscard]] PortForwardRule rule() const { return m_rule; }

    /** @brief 返回 GUI 线程最近收到的统计快照。 */
    [[nodiscard]] PortForwardStatistics statistics() const { return m_statistics; }

public slots:
    /** @brief 异步启动监听或向 SSH 服务器申请远程监听。 */
    void start();

    /** @brief 异步停止监听并关闭该规则的全部活动连接。 */
    void stop();

    /**
     * @brief 接管一条由根 SSH 会话接受的远程转发连接。
     * @param tunnelFd 与 forwarded-tcpip channel 相连的本地 socket fd；调用后无论运行时
     *        是否仍可用，本对象都负责接管或关闭该 fd，调用方不得再次使用。
     *
     * 该入口由 PortForwardManagerWidget 唯一分发，而不是让每个规则同时订阅栈信号。集中
     * 分发可以明确 fd 只有一个所有者，并在规则停止/迁移的竞态窗口及时关闭孤儿连接。
     */
    void acceptRemoteConnection(int tunnelFd);

signals:
    /** @brief 生命周期或流量统计变化；始终投递到本对象所属线程。 */
    void statisticsChanged(const QString &ruleId, const PortForwardStatistics &statistics);

    /** @brief 可展示在状态栏的启动失败、单连接失败或安全限制说明。 */
    void statusMessage(const QString &message);

    /** @brief 工作线程已经停止全部网络活动，可安全销毁运行时。 */
    void stopped(const QString &ruleId);

private:
    PortForwardRule m_rule;
    SshSessionStack *m_sessionStack = nullptr; ///< 非拥有指针；见类说明。
    QThread *m_workerThread = nullptr;
    PortForwardWorker *m_worker = nullptr;
    PortForwardStatistics m_statistics;
};
