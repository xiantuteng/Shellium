#include "ssh/SshSessionNode.h"

#include <QFile>
#include <QThread>
#include <QtEndian>

#include <utility>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

/**
 * @brief 向 RFC 4254 终端模式字节串追加一个 opcode + uint32 值。
 * @param modes 待写入的模式缓冲区。
 * @param opcode RFC 4254 第 8 节定义的单字节终端模式编号。
 * @param value 模式值；布尔模式使用 0/1，控制字符使用 ASCII 值。
 */
void appendTerminalMode(QByteArray &modes, const quint8 opcode, const quint32 value)
{
    modes.append(static_cast<char>(opcode));
    const quint32 networkValue = qToBigEndian(value);
    modes.append(reinterpret_cast<const char *>(&networkValue), sizeof(networkValue));
}

/**
 * @brief 构造适合交互式 Shell 的 RFC 4254 PTY 模式。
 * @return 以 TTY_OP_END 结尾、可直接传给 libssh 的模式字节串。
 *
 * 部分旧 SSH 服务端不会为没有携带 modes 的 PTY 请求应用系统默认 termios，导致 ECHO、
 * ICANON 或 ONLCR 关闭。显式发送这些标准模式后，字符由远端行规程回显，回车提交命令，
 * 输出换行也会转换为 CRLF；密码程序仍可在运行期间自行关闭 ECHO。
 */
QByteArray interactiveTerminalModes()
{
    QByteArray modes;
    modes.reserve(96);

    // 常用控制字符：Ctrl+C、退格、Ctrl+D、Ctrl+Z 与软件流控。
    appendTerminalMode(modes, 1, 3);    // VINTR = Ctrl+C
    appendTerminalMode(modes, 3, 127);  // VERASE = DEL
    appendTerminalMode(modes, 5, 4);    // VEOF = Ctrl+D
    appendTerminalMode(modes, 8, 17);   // VSTART = Ctrl+Q
    appendTerminalMode(modes, 9, 19);   // VSTOP = Ctrl+S
    appendTerminalMode(modes, 10, 26);  // VSUSP = Ctrl+Z

    appendTerminalMode(modes, 36, 1);   // ICRNL：输入 CR 转为 NL，回车才能提交规范模式命令。
    appendTerminalMode(modes, 38, 1);   // IXON：支持终端软件流控。
    appendTerminalMode(modes, 42, 1);   // IUTF8：按 UTF-8 处理擦除边界。
    appendTerminalMode(modes, 50, 1);   // ISIG：启用 Ctrl+C/Ctrl+Z 信号字符。
    appendTerminalMode(modes, 51, 1);   // ICANON：由远端行规程按行编辑并提交输入。
    appendTerminalMode(modes, 53, 1);   // ECHO：显示用户输入的普通字符。
    appendTerminalMode(modes, 54, 1);   // ECHOE：退格时擦除已回显字符。
    appendTerminalMode(modes, 55, 1);   // ECHOK：kill-line 后正确清理显示。
    appendTerminalMode(modes, 59, 1);   // IEXTEN：启用实现定义的扩展输入处理。
    appendTerminalMode(modes, 70, 1);   // OPOST：启用输出后处理。
    appendTerminalMode(modes, 72, 1);   // ONLCR：输出 NL 转为 CRLF，提示符自动进入下一行。
    appendTerminalMode(modes, 91, 1);   // CS8：使用 8 位字符宽度，避免破坏 UTF-8 字节。
    appendTerminalMode(modes, 128, 38400); // TTY_OP_ISPEED：常见输入波特率占位值。
    appendTerminalMode(modes, 129, 38400); // TTY_OP_OSPEED：常见输出波特率占位值。

    modes.append('\0'); // TTY_OP_END：RFC 要求以单字节 0 终止，不再跟随 uint32 值。
    return modes;
}

/**
 * @brief 从已连接会话提取 SHA-256 主机公钥指纹。
 * @param session 已完成握手但尚未认证的会话。
 * @return 适合直接展示给用户核验的指纹，读取失败时返回明确占位文本。
 */
QString serverFingerprint(ssh_session session)
{
    ssh_key serverKey = nullptr;
    unsigned char *hash = nullptr;
    size_t hashLength = 0;
    QString result = QObject::tr("无法读取指纹");

    if (ssh_get_server_publickey(session, &serverKey) == SSH_OK
        && ssh_get_publickey_hash(serverKey,
                                  SSH_PUBLICKEY_HASH_SHA256,
                                  &hash,
                                  &hashLength) == SSH_OK) {
        char *rawFingerprint = ssh_get_fingerprint_hash(
            SSH_PUBLICKEY_HASH_SHA256, hash, hashLength);
        if (rawFingerprint != nullptr) {
            result = QString::fromUtf8(rawFingerprint);
            ssh_string_free_char(rawFingerprint);
        }
    }
    if (hash != nullptr) {
        ssh_clean_pubkey_hash(&hash);
    }
    if (serverKey != nullptr) {
        ssh_key_free(serverKey);
    }
    return result;
}

/** @brief 构造一个失败结果。 */
SshSessionNode::StepResult failure(QString message)
{
    SshSessionNode::StepResult result;
    result.message = std::move(message);
    return result;
}

/** @brief 构造一个成功结果。 */
SshSessionNode::StepResult success()
{
    SshSessionNode::StepResult result;
    result.ok = true;
    return result;
}

} // namespace

SshSessionNode::SshSessionNode(ConnectionTarget target)
    : m_target(std::move(target))
{
}

SshSessionNode::~SshSessionNode()
{
    for (ActiveForward &forward : m_forwards) {
        releaseForward(forward);
    }
    m_forwards.clear();

    if (m_shell != nullptr) {
        ssh_channel_send_eof(m_shell);
        ssh_channel_close(m_shell);
        ssh_channel_free(m_shell);
        m_shell = nullptr;
    }
    if (m_session != nullptr) {
        ssh_disconnect(m_session);
        ssh_free(m_session);
        m_session = nullptr;
    }
    // 该层持有的密码副本随对象一同消失前先覆盖，缩短明文在进程内的驻留时间。
    m_target.password.fill(QChar::Null);
}

SshSessionNode::StepResult SshSessionNode::connectTransport(const int transportFd,
                                                            const long timeoutSeconds)
{
    m_session = ssh_new();
    if (m_session == nullptr) {
        return failure(tr("无法创建 libssh 会话。"));
    }

    const QByteArray host = m_target.host.toUtf8();
    const QByteArray user = m_target.username.toUtf8();
    const unsigned int port = m_target.port;
    ssh_options_set(m_session, SSH_OPTIONS_HOST, host.constData());
    if (!user.isEmpty()) {
        ssh_options_set(m_session, SSH_OPTIONS_USER, user.constData());
    }
    ssh_options_set(m_session, SSH_OPTIONS_PORT, &port);

    if (transportFd >= 0) {
        // SSH_OPTIONS_FD 让 libssh 在已连接 socket 上直接握手，同时 HOST 仍保持真实内层
        // 地址，known_hosts 校验不会错误绑定到隧道本地端。
        int fd = transportFd;
        ssh_options_set(m_session, SSH_OPTIONS_FD, &fd);
    } else {
        ssh_options_set(m_session, SSH_OPTIONS_TIMEOUT, &timeoutSeconds);
    }

    if (!m_target.identityFile.trimmed().isEmpty()) {
        const QByteArray identity = QFile::encodeName(m_target.identityFile);
        ssh_options_set(m_session, SSH_OPTIONS_IDENTITY, identity.constData());
    }
    if (transportFd < 0 && !m_target.proxyJumps.isEmpty()) {
        // 复用隧道时外层路径已由隧道本身表达；此处再配置 ProxyJump 会让 libssh 试图从
        // 内层服务器重新跳一次，因此只在直连模式下应用跳板链。
        const QByteArray jumps = m_target.proxyJumps.join(QLatin1Char(',')).toUtf8();
        if (ssh_options_set(m_session, SSH_OPTIONS_PROXYJUMP, jumps.constData()) != SSH_OK) {
            return failure(tr("无法配置 SSH 跳板链：%1")
                               .arg(QString::fromUtf8(ssh_get_error(m_session))));
        }
    }

    if (m_target.allowLegacyHmacSha1) {
        // hmac-sha1 只对用户明确确认的单个连接启用，并同时配置两个传输方向；如果只配置
        // client->server，部分旧服务端会在反向协商时再次失败。
        constexpr char legacyHmac[] = "hmac-sha1";
        ssh_options_set(m_session, SSH_OPTIONS_HMAC_C_S, legacyHmac);
        ssh_options_set(m_session, SSH_OPTIONS_HMAC_S_C, legacyHmac);
    }

    if (ssh_connect(m_session) != SSH_OK) {
        const QString error = QString::fromUtf8(ssh_get_error(m_session));
        // libssh 没有为“算法无交集”提供独立错误码，只能对其稳定错误文本做精确分类。
        // 同时要求出现 MAC 协商语义和 hmac-sha1，避免对普通 kex/网络错误错误降级。
        if (!m_target.allowLegacyHmacSha1
            && error.contains(QStringLiteral("no match for method mac algo"), Qt::CaseInsensitive)
            && error.contains(QStringLiteral("hmac-sha1"), Qt::CaseInsensitive)) {
            StepResult result = failure(error);
            result.legacyHmacRequired = true;
            return result;
        }
        return failure(tr("SSH 连接失败：%1").arg(error));
    }
    return success();
}

SshSessionNode::StepResult SshSessionNode::verifyHostKey()
{
    const ssh_known_hosts_e knownState = ssh_session_is_known_server(m_session);
    if (knownState == SSH_KNOWN_HOSTS_CHANGED || knownState == SSH_KNOWN_HOSTS_OTHER) {
        return failure(tr("服务器主机密钥与 known_hosts 不一致，连接已终止。"));
    }
    if (knownState == SSH_KNOWN_HOSTS_ERROR) {
        return failure(tr("读取 known_hosts 失败：%1")
                           .arg(QString::fromUtf8(ssh_get_error(m_session))));
    }
    if (knownState == SSH_KNOWN_HOSTS_UNKNOWN || knownState == SSH_KNOWN_HOSTS_NOT_FOUND) {
        StepResult result;
        result.hostKeyUnknown = true;
        result.fingerprint = serverFingerprint(m_session);
        return result;
    }
    return success();
}

SshSessionNode::StepResult SshSessionNode::acceptHostKey()
{
    if (ssh_session_update_known_hosts(m_session) != SSH_OK) {
        return failure(tr("写入 known_hosts 失败：%1")
                           .arg(QString::fromUtf8(ssh_get_error(m_session))));
    }
    return success();
}

SshSessionNode::StepResult SshSessionNode::authenticate()
{
    const QByteArray secret = m_target.password.toUtf8();
    int authentication = ssh_userauth_none(m_session, nullptr);
    if (authentication != SSH_AUTH_SUCCESS) {
        // publickey_auto 会尝试 SSH Agent、默认密钥以及通过 SSH_OPTIONS_IDENTITY 指定的密钥。
        // 当用户输入的密码实际是私钥口令时，也允许 libssh 用它解密私钥。
        authentication = ssh_userauth_publickey_auto(
            m_session, nullptr, secret.isEmpty() ? nullptr : secret.constData());
    }
    if (authentication != SSH_AUTH_SUCCESS && !secret.isEmpty()) {
        authentication = ssh_userauth_password(m_session, nullptr, secret.constData());
    }
    if (authentication != SSH_AUTH_SUCCESS) {
        return failure(tr("SSH 认证失败：%1。请检查密码、私钥或 SSH Agent。")
                           .arg(QString::fromUtf8(ssh_get_error(m_session))));
    }
    return success();
}

SshSessionNode::StepResult SshSessionNode::openShell(const int columns, const int rows)
{
    m_shell = ssh_channel_new(m_session);
    if (m_shell == nullptr || ssh_channel_open_session(m_shell) != SSH_OK) {
        return failure(tr("无法创建 SSH Shell 通道：%1")
                           .arg(QString::fromUtf8(ssh_get_error(m_session))));
    }

    const QByteArray terminalModes = interactiveTerminalModes();
    if (ssh_channel_request_pty_size_modes(
            m_shell,
            "xterm-256color",
            columns,
            rows,
            reinterpret_cast<const unsigned char *>(terminalModes.constData()),
            static_cast<size_t>(terminalModes.size())) != SSH_OK
        || ssh_channel_request_shell(m_shell) != SSH_OK) {
        return failure(tr("服务器拒绝分配交互式 PTY：%1")
                           .arg(QString::fromUtf8(ssh_get_error(m_session))));
    }

    // Shell 建立后整个会话切换为非阻塞，交互读写和转发泵送才能在同一轮询循环中共存。
    ssh_set_blocking(m_session, 0);
    return success();
}

void SshSessionNode::resizePty(const int columns, const int rows)
{
    if (m_shell == nullptr) {
        return;
    }
    ssh_channel_change_pty_size(m_shell, columns, rows);
}

int SshSessionNode::writeToShell(const char *data, const int size)
{
    if (m_shell == nullptr) {
        return SSH_ERROR;
    }
    return ssh_channel_write(m_shell, data, static_cast<uint32_t>(size));
}

int SshSessionNode::readFromShell(char *buffer, const int size, const bool stderrStream)
{
    if (m_shell == nullptr) {
        return 0;
    }
    const int count = ssh_channel_read_nonblocking(
        m_shell, buffer, static_cast<uint32_t>(size), stderrStream ? 1 : 0);
    return count > 0 ? count : 0;
}

bool SshSessionNode::isShellAlive() const
{
    return m_shell != nullptr && ssh_channel_is_open(m_shell) != 0
        && ssh_channel_is_eof(m_shell) == 0;
}

int SshSessionNode::shellExitStatus() const
{
    if (m_shell == nullptr) {
        return -1;
    }
    uint32_t exitCode = static_cast<uint32_t>(-1);
    // libssh 0.12 使用结构化 exit state 取代已废弃的 get_exit_status；服务器未发送退出码时
    // 保留 -1 语义，界面据此区分正常 Shell 退出和网络中断。
    ssh_channel_get_exit_state(m_shell, &exitCode, nullptr, nullptr);
    return exitCode == static_cast<uint32_t>(-1) ? -1 : static_cast<int>(exitCode);
}

QString SshSessionNode::lastError() const
{
    if (m_session == nullptr) {
        return {};
    }
    return QString::fromUtf8(ssh_get_error(m_session));
}

void SshSessionNode::closeShell()
{
    if (m_shell == nullptr) {
        return;
    }
    ssh_channel_send_eof(m_shell);
    ssh_channel_close(m_shell);
}

bool SshSessionNode::openForward(const QString &host,
                                 const quint16 port,
                                 int *callerFd,
                                 const std::function<bool()> &shouldAbort,
                                 QString *error)
{
    const auto reportError = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    if (m_session == nullptr) {
        return reportError(tr("该层 SSH 会话尚未建立"));
    }

    int socketPair[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, socketPair) != 0) {
        return reportError(tr("创建 SFTP 隧道 socket 失败：%1")
                               .arg(QString::fromLocal8Bit(std::strerror(errno))));
    }

    ssh_channel forwardChannel = ssh_channel_new(m_session);
    const QByteArray targetHost = host.toUtf8();
    int openResult = forwardChannel == nullptr ? SSH_ERROR : SSH_AGAIN;
    while (openResult == SSH_AGAIN && !shouldAbort()) {
        openResult = ssh_channel_open_forward(
            forwardChannel, targetHost.constData(), port, "127.0.0.1", 0);
        if (openResult == SSH_AGAIN) {
            QThread::msleep(5);
        }
    }

    if (openResult != SSH_OK) {
        const QString message = tr("服务器 %1 无法转发到 %2:%3：%4")
                                    .arg(m_target.connectionSpecifier(true), host)
                                    .arg(port)
                                    .arg(QString::fromUtf8(ssh_get_error(m_session)));
        if (forwardChannel != nullptr) {
            ssh_channel_close(forwardChannel);
            ssh_channel_free(forwardChannel);
        }
        ::close(socketPair[0]);
        ::close(socketPair[1]);
        return reportError(message);
    }

    // 工作线程一侧必须非阻塞，否则轮询循环会被没有数据的隧道读操作卡住；调用方一端保持
    // 默认阻塞语义，可直接交给 libssh 的 SSH_OPTIONS_FD 等同步消费者。
    fcntl(socketPair[0], F_SETFL, fcntl(socketPair[0], F_GETFL, 0) | O_NONBLOCK);
    m_forwards.append({socketPair[0], forwardChannel, {}, {}});
    if (callerFd != nullptr) {
        *callerFd = socketPair[1];
    }
    return true;
}

void SshSessionNode::pumpForwards()
{
    char tunnelBuffer[64 * 1024];
    for (qsizetype index = m_forwards.size() - 1; index >= 0; --index) {
        ActiveForward &forward = m_forwards[index];

        while (true) {
            const ssize_t count = ::read(forward.socketFd, tunnelBuffer, sizeof(tunnelBuffer));
            if (count > 0) {
                forward.socketToChannel.append(tunnelBuffer, count);
            } else {
                if (count == 0) {
                    ssh_channel_send_eof(forward.channel);
                }
                break;
            }
        }
        while (!forward.socketToChannel.isEmpty()) {
            const int written = ssh_channel_write(forward.channel,
                                                  forward.socketToChannel.constData(),
                                                  forward.socketToChannel.size());
            if (written == SSH_AGAIN || written == 0) {
                break;
            }
            if (written < 0) {
                ssh_channel_close(forward.channel);
                break;
            }
            forward.socketToChannel.remove(0, written);
        }
        while (true) {
            const int count = ssh_channel_read_nonblocking(
                forward.channel, tunnelBuffer, sizeof(tunnelBuffer), 0);
            if (count > 0) {
                forward.channelToSocket.append(tunnelBuffer, count);
            } else {
                break;
            }
        }
        while (!forward.channelToSocket.isEmpty()) {
            const ssize_t written = ::write(forward.socketFd,
                                            forward.channelToSocket.constData(),
                                            forward.channelToSocket.size());
            if (written > 0) {
                forward.channelToSocket.remove(0, written);
            } else {
                break;
            }
        }
        if (ssh_channel_is_eof(forward.channel) && forward.channelToSocket.isEmpty()) {
            releaseForward(forward);
            m_forwards.removeAt(index);
        }
    }
}

void SshSessionNode::releaseForward(ActiveForward &forward)
{
    if (forward.socketFd >= 0) {
        ::close(forward.socketFd);
        forward.socketFd = -1;
    }
    if (forward.channel != nullptr) {
        ssh_channel_close(forward.channel);
        ssh_channel_free(forward.channel);
        forward.channel = nullptr;
    }
}
