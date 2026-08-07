#include "ssh/ShellShim.h"

#include <QFileInfo>
#include <QSet>
#include <QUuid>

namespace {

/** @brief Shellium 私有 OSC 编号，与 VtermBridge::oscCallback 中的常量保持一致。 */
constexpr char kOscNumber[] = "6773";

/** @brief argv 分隔符：记录分隔符不会出现在正常命令行参数中。 */
constexpr char kArgumentSeparator = '\x1e';

/** @brief 判断一个 token 是否是 ssh 可执行程序。 */
bool isSshExecutable(const QString &token)
{
    return QFileInfo(token).fileName() == QStringLiteral("ssh");
}

} // namespace

QByteArray ShellShim::injectionCommand()
{
    // 设计约束逐条说明：
    //   * 整行以空格开头——配合 bash 的 HISTCONTROL=ignorespace 与 zsh 的 hist_ignore_space，
    //     注入命令不会污染用户的历史记录。
    //   * 外层用 [ -n "$BASH_VERSION$ZSH_VERSION" ] 限定，其他 Shell 下什么都不定义；
    //     case "$-" in *i*) 再限定只在交互式 Shell 生效，脚本里的 ssh 不受影响。
    //   * argv 用 0x1E 连接后再 base64——**不能直接把 0x1E 放进 OSC**：它是 C0 控制字符，
    //     libvterm 会在此处中止字符串解析，导致只收到第一个参数。base64 的字符集
    //     [A-Za-z0-9+/=] 在 OSC 中完全安全。
    //   * base64 不可用时直接执行原生 ssh，绝不发出无人应答的 OSC——那会让 shim 永远
    //     阻塞在 read 上，把用户的终端挂死。
    //   * read 从 /dev/tty 读取回执，因此即使 stdout 被重定向也能收到 Shellium 的答复。
    //     回执文本 take/pass 会被 PTY 回显到屏幕上，这是已知的外观瑕疵。两种消除办法都
    //     被否决了：`read -s` 并非 POSIX，不支持时返回非零，再用 `||` 回退就会吃掉用户
    //     的下一行输入；`stty -echo` 多数实现走 TCSAFLUSH，会丢弃已在缓冲区里的输入，
    //     回执若先于 stty 抵达就被吞掉，shim 将永久阻塞。宁可留下一行噪音，也不能冒
    //     丢输入或死锁的风险。
    return QByteArrayLiteral(
        " if [ -n \"$BASH_VERSION$ZSH_VERSION\" ]; then"
        " export HISTCONTROL=ignorespace;"
        " ssh() {"
        " case \"$-\" in *i*) ;; *) command ssh \"$@\"; return $?;; esac;"
        " _shellium_a='';"
        " for _shellium_x in \"$@\"; do"
        " _shellium_a=\"$_shellium_a$_shellium_x$(printf '\\036')\";"
        " done;"
        " _shellium_b=$(printf '%s' \"$_shellium_a\" | base64 2>/dev/null | tr -d '\\n');"
        " unset _shellium_a _shellium_x;"
        " if [ -z \"$_shellium_b\" ]; then unset _shellium_b; command ssh \"$@\"; return $?; fi;"
        " printf '\\033]") + kOscNumber + QByteArrayLiteral(
        ";connect;%s\\007' \"$_shellium_b\" > /dev/tty;"
        " unset _shellium_b;"
        " IFS= read -r _shellium_r < /dev/tty;"
        " if [ \"$_shellium_r\" = \"take\" ]; then unset _shellium_r; return 0; fi;"
        " unset _shellium_r; command ssh \"$@\";"
        " };"
        " fi");
}

QByteArray ShellShim::acceptResponse()
{
    return QByteArrayLiteral("take\n");
}

QByteArray ShellShim::declineResponse()
{
    return QByteArrayLiteral("pass\n");
}

ShellShim::Event ShellShim::parseEvent(const QByteArray &payload)
{
    Event event;
    const qsizetype separator = payload.indexOf(';');
    if (separator < 0) {
        return event;
    }
    if (payload.left(separator) != QByteArrayLiteral("connect")) {
        return event;
    }

    // 载荷是 base64 编码的 0x1E 分隔 argv：0x1E 是 C0 控制字符，直接放进 OSC 会被
    // libvterm 当作字符串终止符，只能收到第一个参数。
    const QByteArray body = QByteArray::fromBase64(
        payload.mid(separator + 1), QByteArray::AbortOnBase64DecodingErrors);
    for (const QByteArray &argument : body.split(kArgumentSeparator)) {
        if (!argument.isEmpty()) {
            event.arguments.append(QString::fromUtf8(argument));
        }
    }
    if (event.arguments.isEmpty()) {
        return event;
    }
    event.kind = Event::Kind::Connect;
    return event;
}

ConnectionTarget ShellShim::resolveTarget(const QStringList &arguments,
                                          const ConnectionTarget &currentTarget,
                                          QString *declineReason)
{
    const auto decline = [declineReason](const QString &reason) {
        if (declineReason != nullptr) {
            *declineReason = reason;
        }
        return ConnectionTarget{};
    };

    // 这些开关意味着 ssh 不会进入交互式 Shell，接管它没有意义。
    static const QSet<QString> nonInteractiveFlags = {
        QStringLiteral("-N"), QStringLiteral("-f"), QStringLiteral("-T"),
        QStringLiteral("-W"), QStringLiteral("-O"), QStringLiteral("-Q"),
        QStringLiteral("-M"), QStringLiteral("-s")
    };

    // 这些选项会消费下一个 token，但不改变接管所需的目标信息。
    static const QSet<QString> optionsWithValue = {
        QStringLiteral("-B"), QStringLiteral("-b"), QStringLiteral("-c"),
        QStringLiteral("-D"), QStringLiteral("-E"), QStringLiteral("-e"),
        QStringLiteral("-I"), QStringLiteral("-L"), QStringLiteral("-m"),
        QStringLiteral("-R"), QStringLiteral("-S"), QStringLiteral("-w")
    };

    QString username;
    QString identityFile;
    QString host;
    quint16 port = 22;
    qsizetype index = 0;
    qsizetype hostIndex = -1;

    if (index < arguments.size() && isSshExecutable(arguments.at(index))) {
        // shim 只上报 ssh 之后的参数，但用户可能写了 `command ssh ...`，容错处理。
        ++index;
    }

    while (index < arguments.size()) {
        const QString token = arguments.at(index++);
        if (token == QStringLiteral("--")) {
            if (index < arguments.size()) {
                host = arguments.at(index);
                hostIndex = index;
            }
            break;
        }
        if (nonInteractiveFlags.contains(token)) {
            return decline(tr("该 ssh 调用不会进入交互式 Shell"));
        }
        if (token == QStringLiteral("-F") || token == QStringLiteral("-J")) {
            // 自定义配置文件与显式跳板链无法用一条 direct-tcpip 忠实复现，
            // 强行接管会连到与用户预期不同的路径上。
            return decline(tr("该 ssh 调用使用了 Shellium 无法忠实复现的连接路径"));
        }
        if (token == QStringLiteral("-p") && index < arguments.size()) {
            bool ok = false;
            const uint parsed = arguments.at(index++).toUInt(&ok);
            if (ok && parsed > 0 && parsed <= 65535) {
                port = static_cast<quint16>(parsed);
            }
            continue;
        }
        if (token == QStringLiteral("-l") && index < arguments.size()) {
            username = arguments.at(index++);
            continue;
        }
        if (token == QStringLiteral("-i") && index < arguments.size()) {
            identityFile = arguments.at(index++);
            continue;
        }
        if (token == QStringLiteral("-o") && index < arguments.size()) {
            const QString option = arguments.at(index++);
            const QString key = option.section(QLatin1Char('='), 0, 0).trimmed().toLower();
            const QString value = option.section(QLatin1Char('='), 1).trimmed();
            if (key == QStringLiteral("user")) {
                username = value;
            } else if (key == QStringLiteral("port")) {
                bool ok = false;
                const uint parsed = value.toUInt(&ok);
                if (ok && parsed > 0 && parsed <= 65535) {
                    port = static_cast<quint16>(parsed);
                }
            } else if (key == QStringLiteral("identityfile")) {
                identityFile = value;
            } else if (key == QStringLiteral("proxyjump")
                       || key == QStringLiteral("proxycommand")) {
                return decline(tr("该 ssh 调用使用了 Shellium 无法忠实复现的连接路径"));
            }
            continue;
        }
        if (optionsWithValue.contains(token) && index < arguments.size()) {
            ++index;
            continue;
        }
        if (token.startsWith(QStringLiteral("-p")) && token.size() > 2) {
            bool ok = false;
            const uint parsed = token.mid(2).toUInt(&ok);
            if (ok && parsed > 0 && parsed <= 65535) {
                port = static_cast<quint16>(parsed);
            }
            continue;
        }
        if (token.startsWith(QLatin1Char('-'))) {
            // 未知短选项通常是不带参数的开关；保守跳过，避免把它当作目标主机。
            continue;
        }

        host = token;
        hostIndex = index - 1;
        break;
    }

    if (host.isEmpty()) {
        return decline(tr("未能从命令中解析出目标主机"));
    }
    if (hostIndex >= 0 && hostIndex + 1 < arguments.size()) {
        // `ssh web01 uptime` 执行完就返回外层 Shell，用户从未进入那台机器。
        return decline(tr("该 ssh 调用只在远端执行一次性命令"));
    }

    const qsizetype at = host.lastIndexOf(QLatin1Char('@'));
    if (at > 0) {
        if (username.isEmpty()) {
            username = host.left(at);
        }
        host = host.mid(at + 1);
    }

    ConnectionTarget target;
    target.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    target.host = host;
    target.username = username.isEmpty() ? currentTarget.username : username;
    target.port = port;
    target.identityFile = identityFile;
    target.displayName = target.connectionSpecifier();
    // 由 Shellium 亲自建立，其存在与身份都是确定事实。
    target.confidence = ConnectionTarget::Confidence::Confirmed;
    // 兼容标志沿用当前链：终端与 SFTP 必须对同一条链使用一致的协商策略。
    target.allowLegacyHmacSha1 = currentTarget.allowLegacyHmacSha1;
    return target;
}
