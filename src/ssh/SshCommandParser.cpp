#include "ssh/SshCommandParser.h"

#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

namespace {

/**
 * @brief 判断一个 token 是否是 ssh 可执行程序。
 *
 * 使用 QFileInfo 提取文件名，因此 `/usr/bin/ssh` 与 `ssh` 都能识别；其他名称即使包含
 * “ssh” 字样也不会误判。
 */
bool isSshExecutable(const QString &token)
{
    return QFileInfo(token).fileName() == QStringLiteral("ssh");
}

/** @brief 读取 `-o Key=Value` 中不区分大小写的键值。 */
void applyOpenSshOption(const QString &option,
                        QString &username,
                        quint16 &port,
                        QString &discardedIdentityFile,
                        QStringList &explicitJumps)
{
    const qsizetype separator = option.indexOf(QLatin1Char('='));
    if (separator <= 0) {
        return;
    }

    const QString key = option.left(separator).trimmed().toLower();
    const QString value = option.mid(separator + 1).trimmed();
    if (key == QStringLiteral("user")) {
        username = value;
    } else if (key == QStringLiteral("port")) {
        bool ok = false;
        const uint parsedPort = value.toUInt(&ok);
        if (ok && parsedPort > 0 && parsedPort <= 65535) {
            port = static_cast<quint16>(parsedPort);
        }
    } else if (key == QStringLiteral("identityfile")) {
        discardedIdentityFile = value;
    } else if (key == QStringLiteral("proxyjump")) {
        explicitJumps = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
    }
}

} // namespace

SshCommandParser::Result SshCommandParser::parse(const QString &commandLine,
                                                 const ConnectionTarget &currentTarget)
{
    const QString trimmed = commandLine.trimmed();
    if (trimmed == QStringLiteral("Ctrl-D")) {
        return {.action = Action::LeaveServer};
    }
    // 退出还有 `exit 0` 这样带状态码的写法。只认裸 exit 会让链只增不减，越用越偏离真实状态。
    static const QRegularExpression leavePattern(
        QStringLiteral(R"(^(?:exit|logout)(?:\s+\d+)?$)"));
    if (leavePattern.match(trimmed).hasMatch()) {
        return {.action = Action::LeaveServer};
    }

    // QProcess::splitCommand 支持常见引号和反斜杠规则，比按空格切分更适合主机别名、
    // IdentityFile 路径等输入；复杂的管道、变量展开仍交给真正的远端 Shell 处理。
    QStringList tokens = QProcess::splitCommand(trimmed);
    if (tokens.isEmpty()) {
        return {};
    }

    qsizetype index = 0;
    // 允许用户显式写 `command ssh ...`，这是绕过 alias/function 的常见用法。
    if (tokens.value(index) == QStringLiteral("command")) {
        ++index;
    }
    if (index >= tokens.size() || !isSshExecutable(tokens.at(index))) {
        return {};
    }
    ++index;

    QString username;
    // 仍需解析出来，否则它的取值会被误当作目标主机名；但只用于消费，不会传给本机。
    QString discardedIdentityFile;
    QString host;
    quint16 port = 22;
    QStringList explicitJumps;

    // 这些选项会消费下一个 token，但不会改变文件管理连接所需的目标信息。
    static const QSet<QString> optionsWithValue = {
        QStringLiteral("-B"), QStringLiteral("-b"), QStringLiteral("-c"),
        QStringLiteral("-D"), QStringLiteral("-E"), QStringLiteral("-e"),
        QStringLiteral("-F"), QStringLiteral("-I"), QStringLiteral("-L"),
        QStringLiteral("-m"), QStringLiteral("-O"), QStringLiteral("-Q"),
        QStringLiteral("-R"), QStringLiteral("-S"), QStringLiteral("-W"),
        QStringLiteral("-w")
    };

    // 这些开关意味着 ssh 根本不会进入交互式 Shell，用户也就不会"身处"目标服务器：
    // -N 不执行远程命令，-f 退到后台，-W 只做 stdio 转发，-T 禁用 PTY，-O/-Q 查询后即退出。
    // 把它们当作进入服务器会在链上留下永远不会被 exit 弹掉的幽灵层。
    static const QSet<QString> nonInteractiveFlags = {
        QStringLiteral("-N"), QStringLiteral("-f"), QStringLiteral("-T"),
        QStringLiteral("-W"), QStringLiteral("-O"), QStringLiteral("-Q")
    };

    qsizetype hostTokenIndex = -1;
    while (index < tokens.size()) {
        const QString token = tokens.at(index++);
        if (token == QStringLiteral("--")) {
            if (index < tokens.size()) {
                host = tokens.at(index);
                hostTokenIndex = index;
            }
            break;
        }
        if (nonInteractiveFlags.contains(token)) {
            return {};
        }
        if (token == QStringLiteral("-p") && index < tokens.size()) {
            bool ok = false;
            const uint parsedPort = tokens.at(index++).toUInt(&ok);
            if (ok && parsedPort > 0 && parsedPort <= 65535) {
                port = static_cast<quint16>(parsedPort);
            }
            continue;
        }
        if (token == QStringLiteral("-l") && index < tokens.size()) {
            username = tokens.at(index++);
            continue;
        }
        if (token == QStringLiteral("-i") && index < tokens.size()) {
            discardedIdentityFile = tokens.at(index++);
            continue;
        }
        if (token == QStringLiteral("-J") && index < tokens.size()) {
            explicitJumps = tokens.at(index++).split(QLatin1Char(','), Qt::SkipEmptyParts);
            continue;
        }
        if (token == QStringLiteral("-o") && index < tokens.size()) {
            applyOpenSshOption(tokens.at(index++), username, port, discardedIdentityFile,
                               explicitJumps);
            continue;
        }
        if (optionsWithValue.contains(token) && index < tokens.size()) {
            ++index;
            continue;
        }
        if (token.startsWith(QStringLiteral("-p")) && token.size() > 2) {
            bool ok = false;
            const uint parsedPort = token.mid(2).toUInt(&ok);
            if (ok && parsedPort > 0 && parsedPort <= 65535) {
                port = static_cast<quint16>(parsedPort);
            }
            continue;
        }
        if (token.startsWith(QLatin1Char('-'))) {
            // 未知短选项通常是不带参数的开关；保守跳过，避免把它当作目标主机。
            continue;
        }

        host = token;
        hostTokenIndex = index - 1;
        break;
    }

    if (host.isEmpty()) {
        return {};
    }

    // 主机名之后若还有内容，那是要在远端执行的一次性命令（`ssh web01 uptime`）。它执行完
    // 就返回外层 Shell，用户从未进入过那台机器，而且永远不会为它敲 exit——压栈就等于
    // 制造一个不可撤销的幽灵层。宁可少判：用户还能在文件管理器里手动补选。
    if (hostTokenIndex >= 0 && hostTokenIndex + 1 < tokens.size()) {
        return {};
    }

    const qsizetype at = host.lastIndexOf(QLatin1Char('@'));
    if (at > 0) {
        if (username.isEmpty()) {
            username = host.left(at);
        }
        host = host.mid(at + 1);
    }

    ConnectionTarget nested;
    nested.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    nested.host = host;
    nested.username = username.isEmpty() ? currentTarget.username : username;
    nested.port = port;
    // 刻意不传 identityFile：-i 与 -o IdentityFile= 的路径属于**远端**文件系统，而 SFTP
    // 由本机的 libssh 发起。照搬这个路径要么找不到文件，要么更糟——撞上本机一个同名的
    // 无关私钥。留空即可，本机会退回 Agent、默认密钥或询问密码。
    nested.parentId = currentTarget.id;
    nested.displayName = nested.connectionSpecifier();
    // 文件管理器会从本机重新建立“外层 → 内层”的独立 SFTP 连接。旧跳板机若需要
    // hmac-sha1，而嵌套目标丢失该策略，libssh 的 ProxyJump 子会话通常只报告 Socket
    // disconnected。继承兼容标志可保证终端与 SFTP 对同一连接链使用一致协商策略。
    nested.allowLegacyHmacSha1 = currentTarget.allowLegacyHmacSha1;

    nested.proxyJumps = currentTarget.proxyJumps;
    nested.proxyJumps.append(currentTarget.connectionSpecifier(true));
    if (!explicitJumps.isEmpty()) {
        // 嵌套命令中的 -J 是从“当前远端服务器”继续向内的跳板，而文件管理连接从本机发起，
        // 因此不能覆盖外层链；必须追加到当前服务器之后才能复现 A → J → B 的实际路径。
        nested.proxyJumps.append(explicitJumps);
    }

    return {.action = Action::EnterServer, .target = nested};
}
