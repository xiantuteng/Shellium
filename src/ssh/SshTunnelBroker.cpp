#include "ssh/SshTunnelBroker.h"

#include "ssh/SshSessionStack.h"

#include <QMutexLocker>

SshTunnelBroker &SshTunnelBroker::instance()
{
    static SshTunnelBroker broker;
    return broker;
}

void SshTunnelBroker::registerRoute(const QString &routeId,
                                    SshSessionStack *stack,
                                    const QString &layerId,
                                    const ConnectionTarget &target,
                                    const ConnectionTarget &relayOrigin)
{
    if (routeId.isEmpty() || stack == nullptr || !target.isValid()) {
        return;
    }
    QMutexLocker locker(&m_mutex);
    m_routes.insert(routeId, {stack, layerId, target, relayOrigin});
}

void SshTunnelBroker::unregisterRoute(const QString &routeId)
{
    QMutexLocker locker(&m_mutex);
    m_routes.remove(routeId);
}

NativeSocketHandle SshTunnelBroker::openSocket(const QString &routeId, QString *errorMessage)
{
    Route route;
    {
        QMutexLocker locker(&m_mutex);
        const auto iterator = m_routes.constFind(routeId);
        if (iterator == m_routes.cend() || iterator->stack.isNull()) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("嵌套 SSH 的外层会话已经关闭");
            }
            return InvalidNativeSocket;
        }
        route = *iterator;
    }
    // 不在持有 Broker 锁时等待工作线程，否则标签页关闭时 unregisterRoute 会形成锁反转。
    return route.stack->openForwardSocket(route.layerId, route.target.host, route.target.port,
                                          errorMessage);
}

bool SshTunnelBroker::remoteSftpRelay(const QString &routeId,
                                      ConnectionTarget *relayOrigin,
                                      ConnectionTarget *target)
{
    if (relayOrigin == nullptr || target == nullptr) {
        return false;
    }

    QMutexLocker locker(&m_mutex);
    const auto iterator = m_routes.constFind(routeId);
    if (iterator == m_routes.cend() || iterator->stack.isNull()
        || !iterator->relayOrigin.isValid() || !iterator->target.isValid()) {
        return false;
    }
    // ConnectionTarget 中的密码只存在于当前进程内存；这里按值复制给单次 SFTP 后台任务，
    // 不写日志或数据库。任务结束时 SftpConnection 析构并释放对应 libssh 会话。
    *relayOrigin = iterator->relayOrigin;
    *target = iterator->target;
    return true;
}
