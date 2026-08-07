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
 * 从而避免在标签页关闭后访问悬挂指针。
 */
class SshTunnelBroker final {
public:
    /** @brief 返回进程内唯一 Broker。 */
    static SshTunnelBroker &instance();

    /**
     * @brief 注册一条通往某一层服务器的路由。
     * @param layerId 要抵达的那一层的目标 id；空表示沿用栈顶（旧的命令解析路径）。
     *
     * 记录的是"目的地"而不是"从哪出发"：转发必须由目标层的**下一层外层**发起，而层的
     * 序号会随压栈退栈变化，交给栈自己解析比在界面侧记账可靠。
     */
    void registerRoute(const QString &routeId,
                       SshSessionStack *stack,
                       const QString &layerId,
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
        QString layerId;
        QString targetHost;
        quint16 targetPort = 22;
    };

    SshTunnelBroker() = default;
    QMutex m_mutex;
    QHash<QString, Route> m_routes;
};
