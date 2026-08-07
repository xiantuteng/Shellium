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
                                    const QString &targetHost,
                                    const quint16 targetPort)
{
    if (routeId.isEmpty() || stack == nullptr || targetHost.isEmpty()) {
        return;
    }
    QMutexLocker locker(&m_mutex);
    m_routes.insert(routeId, {stack, layerId, targetHost, targetPort});
}

void SshTunnelBroker::unregisterRoute(const QString &routeId)
{
    QMutexLocker locker(&m_mutex);
    m_routes.remove(routeId);
}

int SshTunnelBroker::openSocket(const QString &routeId, QString *errorMessage)
{
    Route route;
    {
        QMutexLocker locker(&m_mutex);
        const auto iterator = m_routes.constFind(routeId);
        if (iterator == m_routes.cend() || iterator->stack.isNull()) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("嵌套 SSH 的外层会话已经关闭");
            }
            return -1;
        }
        route = *iterator;
    }
    // 不在持有 Broker 锁时等待工作线程，否则标签页关闭时 unregisterRoute 会形成锁反转。
    return route.stack->openForwardSocket(route.layerId, route.targetHost, route.targetPort,
                                          errorMessage);
}
