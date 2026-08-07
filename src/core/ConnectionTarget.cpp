#include "core/ConnectionTarget.h"

bool ConnectionTarget::isValid() const
{
    return !host.trimmed().isEmpty();
}

QString ConnectionTarget::connectionSpecifier(const bool includeDefaultPort) const
{
    // IPv6 地址包含冒号，作为跳板描述的一部分时必须加方括号，避免与端口分隔符混淆。
    QString normalizedHost = host.trimmed();
    if (normalizedHost.contains(QLatin1Char(':')) && !normalizedHost.startsWith(QLatin1Char('['))) {
        normalizedHost = QStringLiteral("[%1]").arg(normalizedHost);
    }

    const QString userPrefix = username.trimmed().isEmpty()
        ? QString()
        : username.trimmed() + QLatin1Char('@');
    const QString portSuffix = includeDefaultPort || port != 22
        ? QStringLiteral(":%1").arg(port)
        : QString();
    return userPrefix + normalizedHost + portSuffix;
}

QString ConnectionTarget::stableKey() const
{
    // 两条跳板链后可能存在相同的内网主机名（例如都叫 db.internal）。把完整跳板链纳入键，
    // 可防止对环境 A 的临时信任被错误复用于环境 B 的同名服务器。
    return QStringLiteral("%1|%2|%3|via=%4")
        .arg(username.trimmed(),
             host.trimmed().toLower(),
             QString::number(port),
             proxyJumps.join(QLatin1Char(',')));
}
