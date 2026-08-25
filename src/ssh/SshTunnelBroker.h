#pragma once

#include "core/ConnectionTarget.h"

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
     * @param routeId 进程内唯一的路由标识。
     * @param stack 当前终端持有的 SSH 会话栈。
     * @param layerId 要抵达的那一层的目标 id；空表示沿用栈顶（旧的命令解析路径）。
     * @param target 文件管理器最终需要访问的内层服务器。
     * @param relayOrigin 原会话无法中继时，可重新连接并在其上执行远端 OpenSSH 的外层服务器。
     *
     * 记录的是"目的地"而不是"从哪出发"：转发必须由目标层的**下一层外层**发起，而层的
     * 序号会随压栈退栈变化，交给栈自己解析比在界面侧记账可靠。
     */
    void registerRoute(const QString &routeId,
                       SshSessionStack *stack,
                       const QString &layerId,
                       const ConnectionTarget &target,
                       const ConnectionTarget &relayOrigin = {});

    /** @brief 删除路由；已建立的 socket 由各自 SFTP 会话负责关闭。 */
    void unregisterRoute(const QString &routeId);

    /**
     * @brief 打开一端连接内层 SSH、另一端交给 libssh 的 socket。
     * @return 成功返回 fd，调用方取得所有权；失败返回 -1。
     */
    int openSocket(const QString &routeId, QString *errorMessage);

    /**
     * @brief 查询使用远端 OpenSSH 启动 SFTP 子系统所需的两端配置。
     * @param routeId 已注册的嵌套路由标识。
     * @param relayOrigin 输出需要重新认证的外层服务器；不可为空。
     * @param target 输出由外层 OpenSSH 连接的内层服务器；不可为空。
     * @return 路由仍有效且同时具备两端配置时返回 true。
     *
     * 该兜底用于私钥只存在于外层服务器的场景：本机 libssh 无法读取远端私钥，因此必须
     * 在外层执行 `ssh -s <target> sftp`，让远端 OpenSSH 使用它自己的 ~/.ssh 密钥完成认证。
     */
    bool remoteSftpRelay(const QString &routeId,
                         ConnectionTarget *relayOrigin,
                         ConnectionTarget *target);

private:
    struct Route {
        QPointer<SshSessionStack> stack;
        QString layerId;
        ConnectionTarget target;
        ConnectionTarget relayOrigin;
    };

    SshTunnelBroker() = default;
    QMutex m_mutex;
    QHash<QString, Route> m_routes;
};
