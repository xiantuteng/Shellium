#pragma once

#include <QHash>
#include <QMutex>
#include <QPointer>
#include <QString>

class SshSessionStack;

/**
 * @brief 连接嵌套目标与已认证外层 SSH 会话的进程内路由表。
 *
 * SFTP 后台任务只持有不可解引用的 routeId；Broker 使用 QPointer 验证终端会话栈工作线程仍存活，
 * 从而避免在分屏关闭后访问悬挂指针。
 */
class SshTunnelBroker final {
public:
    /** @brief 返回进程内唯一 Broker。 */
    static SshTunnelBroker &instance();

    /** @brief 注册一个从现有 SSH 会话到内层主机的路由。 */
    void registerRoute(const QString &routeId,
                       SshSessionStack *stack,
                       const QString &targetHost,
                       quint16 targetPort);

    /** @brief 删除路由；已建立的 socket 由各自 SFTP 会话负责关闭。 */
    void unregisterRoute(const QString &routeId);

    /**
     * @brief 打开一端连接内层 SSH、另一端交给 libssh 的 socket。
     * @return 成功返回 fd，调用方取得所有权；失败返回 -1。
     */
    int openSocket(const QString &routeId, QString *errorMessage);

private:
    struct Route {
        QPointer<SshSessionStack> stack;
        QString targetHost;
        quint16 targetPort = 22;
    };

    SshTunnelBroker() = default;
    QMutex m_mutex;
    QHash<QString, Route> m_routes;
};
