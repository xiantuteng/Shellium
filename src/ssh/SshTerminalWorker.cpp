#include "ssh/SshTerminalWorker.h"

#include <QFile>
#include <QMutexLocker>
#include <QtEndian>

#include <libssh/libssh.h>

#include <algorithm>

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
 * @param session 已完成 TCP/SSH 握手但尚未认证的会话。
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

/**
 * @brief 释放 libssh 通道和会话的局部 RAII 守卫。
 *
 * run() 有多个错误返回点，统一守卫可以保证认证失败、用户拒绝指纹等路径也不会泄漏句柄。
 */
class SessionGuard final {
public:
    ~SessionGuard()
    {
        if (channel != nullptr) {
            ssh_channel_send_eof(channel);
            ssh_channel_close(channel);
            ssh_channel_free(channel);
        }
        if (session != nullptr) {
            ssh_disconnect(session);
            ssh_free(session);
        }
    }

    ssh_session session = nullptr;
    ssh_channel channel = nullptr;
};

} // namespace

SshTerminalWorker::SshTerminalWorker(const ConnectionTarget &target, QObject *parent)
    : QThread(parent)
    , m_target(target)
{
}

SshTerminalWorker::~SshTerminalWorker()
{
    requestStop();
    // 析构时必须等待工作线程离开 libssh 调用，避免 QObject 销毁后线程继续发信号。
    wait(3000);
}

void SshTerminalWorker::enqueueInput(const QByteArray &bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    QMutexLocker locker(&m_mutex);
    m_pendingInput.append(bytes);
}

void SshTerminalWorker::updateTerminalSize(const int columns, const int rows)
{
    QMutexLocker locker(&m_mutex);
    const int normalizedColumns = std::max(columns, 1);
    const int normalizedRows = std::max(rows, 1);
    if (m_columns != normalizedColumns || m_rows != normalizedRows) {
        m_columns = normalizedColumns;
        m_rows = normalizedRows;
        m_sizeDirty = true;
    }
}

void SshTerminalWorker::requestStop()
{
    QMutexLocker locker(&m_mutex);
    m_stopRequested = true;
    // 如果线程正等待用户确认指纹，必须同时唤醒，否则关闭窗口会一直等待对话框结果。
    m_hostKeyCondition.wakeAll();
}

void SshTerminalWorker::confirmUnknownHost(const bool trusted)
{
    QMutexLocker locker(&m_mutex);
    if (!m_waitingForHostKey) {
        return;
    }
    m_hostKeyTrusted = trusted;
    m_waitingForHostKey = false;
    m_hostKeyCondition.wakeAll();
}

int SshTerminalWorker::openForwardSocket(const QString &host,
                                         const quint16 port,
                                         QString *errorMessage)
{
    if (!isRunning() || shouldStop()) {
        if (errorMessage != nullptr) {
            *errorMessage = tr("外层 SSH 会话已经结束");
        }
        return -1;
    }

    auto request = QSharedPointer<ForwardRequest>::create();
    request->host = host;
    request->port = port;
    {
        QMutexLocker locker(&m_mutex);
        m_forwardRequests.append(request);
    }

    QMutexLocker requestLocker(&request->mutex);
    if (!request->condition.wait(&request->mutex, 15000) || !request->completed) {
        request->cancelled = true;
        if (errorMessage != nullptr) {
            *errorMessage = tr("创建内层 SSH 转发通道超时");
        }
        return -1;
    }
    if (errorMessage != nullptr) {
        *errorMessage = request->error;
    }
    return request->clientFd;
}

void SshTerminalWorker::run()
{
    SessionGuard guard;
    guard.session = ssh_new();
    if (guard.session == nullptr) {
        emit connectionError(tr("无法创建 libssh 会话。"));
        return;
    }

    const QByteArray host = m_target.host.toUtf8();
    const QByteArray user = m_target.username.toUtf8();
    const unsigned int port = m_target.port;
    ssh_options_set(guard.session, SSH_OPTIONS_HOST, host.constData());
    if (!user.isEmpty()) {
        ssh_options_set(guard.session, SSH_OPTIONS_USER, user.constData());
    }
    ssh_options_set(guard.session, SSH_OPTIONS_PORT, &port);
    const long timeoutSeconds = 15;
    ssh_options_set(guard.session, SSH_OPTIONS_TIMEOUT, &timeoutSeconds);

    if (!m_target.identityFile.trimmed().isEmpty()) {
        const QByteArray identity = QFile::encodeName(m_target.identityFile);
        ssh_options_set(guard.session, SSH_OPTIONS_IDENTITY, identity.constData());
    }
    if (!m_target.proxyJumps.isEmpty()) {
        const QByteArray jumps = m_target.proxyJumps.join(QLatin1Char(',')).toUtf8();
        if (ssh_options_set(guard.session, SSH_OPTIONS_PROXYJUMP, jumps.constData()) != SSH_OK) {
            emit connectionError(tr("无法配置 SSH 跳板链：%1")
                                     .arg(QString::fromUtf8(ssh_get_error(guard.session))));
            return;
        }
    }

    if (m_target.allowLegacyHmacSha1) {
        // hmac-sha1 只对用户明确确认的单个连接启用，并同时配置两个传输方向；如果只配置
        // client->server，部分旧服务端会在反向协商时再次失败。
        constexpr char legacyHmac[] = "hmac-sha1";
        ssh_options_set(guard.session, SSH_OPTIONS_HMAC_C_S, legacyHmac);
        ssh_options_set(guard.session, SSH_OPTIONS_HMAC_S_C, legacyHmac);
    }

    if (ssh_connect(guard.session) != SSH_OK) {
        const QString error = QString::fromUtf8(ssh_get_error(guard.session));
        // libssh 没有为“算法无交集”提供独立错误码，只能对其稳定错误文本做精确分类。
        // 同时要求出现 MAC 协商语义和 hmac-sha1，避免对普通 kex/网络错误错误降级。
        if (!m_target.allowLegacyHmacSha1
            && error.contains(QStringLiteral("no match for method mac algo"), Qt::CaseInsensitive)
            && error.contains(QStringLiteral("hmac-sha1"), Qt::CaseInsensitive)) {
            emit legacyHmacRequired(error);
        } else {
            emit connectionError(tr("SSH 连接失败：%1").arg(error));
        }
        return;
    }

    const ssh_known_hosts_e knownState = ssh_session_is_known_server(guard.session);
    if (knownState == SSH_KNOWN_HOSTS_CHANGED || knownState == SSH_KNOWN_HOSTS_OTHER) {
        emit connectionError(tr("服务器主机密钥与 known_hosts 不一致，连接已终止。"));
        return;
    }
    if (knownState == SSH_KNOWN_HOSTS_ERROR) {
        emit connectionError(tr("读取 known_hosts 失败：%1")
                                 .arg(QString::fromUtf8(ssh_get_error(guard.session))));
        return;
    }
    if (knownState == SSH_KNOWN_HOSTS_UNKNOWN || knownState == SSH_KNOWN_HOSTS_NOT_FOUND) {
        {
            QMutexLocker locker(&m_mutex);
            m_waitingForHostKey = true;
            m_hostKeyTrusted = false;
        }
        emit hostKeyVerificationRequired(m_target, serverFingerprint(guard.session));

        QMutexLocker locker(&m_mutex);
        while (m_waitingForHostKey && !m_stopRequested) {
            m_hostKeyCondition.wait(&m_mutex);
        }
        if (m_stopRequested || !m_hostKeyTrusted) {
            emit connectionError(tr("用户拒绝了未知服务器主机密钥。"));
            return;
        }
        locker.unlock();

        if (ssh_session_update_known_hosts(guard.session) != SSH_OK) {
            emit connectionError(tr("写入 known_hosts 失败：%1")
                                     .arg(QString::fromUtf8(ssh_get_error(guard.session))));
            return;
        }
    }

    const QByteArray secret = m_target.password.toUtf8();
    int authentication = ssh_userauth_none(guard.session, nullptr);
    if (authentication != SSH_AUTH_SUCCESS) {
        authentication = ssh_userauth_publickey_auto(
            guard.session, nullptr, secret.isEmpty() ? nullptr : secret.constData());
    }
    if (authentication != SSH_AUTH_SUCCESS && !secret.isEmpty()) {
        authentication = ssh_userauth_password(guard.session, nullptr, secret.constData());
    }
    if (authentication != SSH_AUTH_SUCCESS) {
        emit connectionError(tr("SSH 认证失败：%1。请检查密码、私钥或 SSH Agent。")
                                 .arg(QString::fromUtf8(ssh_get_error(guard.session))));
        return;
    }

    guard.channel = ssh_channel_new(guard.session);
    if (guard.channel == nullptr || ssh_channel_open_session(guard.channel) != SSH_OK) {
        emit connectionError(tr("无法创建 SSH Shell 通道：%1")
                                 .arg(QString::fromUtf8(ssh_get_error(guard.session))));
        return;
    }

    int initialColumns = 80;
    int initialRows = 24;
    {
        QMutexLocker locker(&m_mutex);
        initialColumns = m_columns;
        initialRows = m_rows;
        m_sizeDirty = false;
    }
    const QByteArray terminalModes = interactiveTerminalModes();
    if (ssh_channel_request_pty_size_modes(
            guard.channel,
            "xterm-256color",
            initialColumns,
            initialRows,
            reinterpret_cast<const unsigned char *>(terminalModes.constData()),
            static_cast<size_t>(terminalModes.size())) != SSH_OK
        || ssh_channel_request_shell(guard.channel) != SSH_OK) {
        emit connectionError(tr("服务器拒绝分配交互式 PTY：%1")
                                 .arg(QString::fromUtf8(ssh_get_error(guard.session))));
        return;
    }

    ssh_set_blocking(guard.session, 0);
    emit shellConnected();

    /** @brief 当前外层会话承载的一个本地 socket ↔ SSH direct-tcpip 双向桥。 */
    struct ActiveForward {
        int socketFd = -1;
        ssh_channel channel = nullptr;
        QByteArray socketToChannel;
        QByteArray channelToSocket;
    };
    QList<ActiveForward> activeForwards;

    QByteArray readBuffer(64 * 1024, Qt::Uninitialized);
    while (!shouldStop() && ssh_channel_is_open(guard.channel)
           && !ssh_channel_is_eof(guard.channel)) {
        QByteArray pendingInput;
        int columns = 0;
        int rows = 0;
        bool resizeRequired = false;
        {
            QMutexLocker locker(&m_mutex);
            pendingInput.swap(m_pendingInput);
            resizeRequired = m_sizeDirty;
            columns = m_columns;
            rows = m_rows;
            m_sizeDirty = false;
        }

        QList<QSharedPointer<ForwardRequest>> forwardRequests;
        {
            QMutexLocker locker(&m_mutex);
            forwardRequests.swap(m_forwardRequests);
        }
        for (const QSharedPointer<ForwardRequest> &request : forwardRequests) {
            int socketPair[2] = {-1, -1};
            QString forwardError;
            ssh_channel forwardChannel = nullptr;
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, socketPair) != 0) {
                forwardError = tr("创建 SFTP 隧道 socket 失败：%1")
                                   .arg(QString::fromLocal8Bit(std::strerror(errno)));
            } else {
                forwardChannel = ssh_channel_new(guard.session);
                const QByteArray targetHost = request->host.toUtf8();
                int openResult = forwardChannel == nullptr ? SSH_ERROR : SSH_AGAIN;
                while (openResult == SSH_AGAIN && !shouldStop()) {
                    openResult = ssh_channel_open_forward(forwardChannel,
                                                          targetHost.constData(),
                                                          request->port,
                                                          "127.0.0.1",
                                                          0);
                    if (openResult == SSH_AGAIN) {
                        QThread::msleep(5);
                    }
                }
                if (openResult != SSH_OK) {
                    forwardError = tr("外层服务器无法转发到 %1:%2：%3")
                                       .arg(request->host)
                                       .arg(request->port)
                                       .arg(QString::fromUtf8(ssh_get_error(guard.session)));
                }
            }

            QMutexLocker requestLocker(&request->mutex);
            if (!forwardError.isEmpty() || request->cancelled) {
                if (forwardChannel != nullptr) {
                    ssh_channel_close(forwardChannel);
                    ssh_channel_free(forwardChannel);
                }
                if (socketPair[0] >= 0) close(socketPair[0]);
                if (socketPair[1] >= 0) close(socketPair[1]);
                request->error = request->cancelled ? tr("SFTP 隧道请求已取消") : forwardError;
                request->clientFd = -1;
            } else {
                fcntl(socketPair[0], F_SETFL, fcntl(socketPair[0], F_GETFL, 0) | O_NONBLOCK);
                activeForwards.append({socketPair[0], forwardChannel, {}, {}});
                request->clientFd = socketPair[1];
            }
            request->completed = true;
            request->condition.wakeAll();
        }

        if (resizeRequired) {
            ssh_channel_change_pty_size(guard.channel, columns, rows);
        }

        qsizetype writeOffset = 0;
        while (writeOffset < pendingInput.size() && !shouldStop()) {
            const int written = ssh_channel_write(
                guard.channel,
                pendingInput.constData() + writeOffset,
                static_cast<uint32_t>(pendingInput.size() - writeOffset));
            if (written == SSH_AGAIN || written == 0) {
                // 非阻塞通道暂时不可写不是失败。旧实现把 SSH_AGAIN(-2) 误加到偏移量，
                // 会让下一次写入从缓冲区之前读取，表现为回车偶尔有效而普通命令丢失。
                QThread::msleep(5);
                continue;
            }
            if (written < 0) {
                emit connectionError(tr("向远端终端写入数据失败：%1")
                                         .arg(QString::fromUtf8(ssh_get_error(guard.session))));
                return;
            }
            writeOffset += written;
        }

        bool receivedData = false;
        for (const int stderrChannel : {0, 1}) {
            while (true) {
                const int bytesRead = ssh_channel_read_nonblocking(
                    guard.channel,
                    readBuffer.data(),
                    static_cast<uint32_t>(readBuffer.size()),
                    stderrChannel);
                if (bytesRead > 0) {
                    receivedData = true;
                    emit outputReady(QByteArray(readBuffer.constData(), bytesRead));
                } else {
                    break;
                }
            }
        }
        if (!receivedData) {
            // 10ms 轮询在交互延迟和空闲 CPU 占用之间取得平衡，且停止请求能很快生效。
            QThread::msleep(10);
        }

        for (qsizetype index = activeForwards.size() - 1; index >= 0; --index) {
            ActiveForward &forward = activeForwards[index];
            char tunnelBuffer[64 * 1024];
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
                if (written == SSH_AGAIN || written == 0) break;
                if (written < 0) {
                    ssh_channel_close(forward.channel);
                    break;
                }
                forward.socketToChannel.remove(0, written);
            }
            while (true) {
                const int count = ssh_channel_read_nonblocking(forward.channel,
                                                               tunnelBuffer,
                                                               sizeof(tunnelBuffer),
                                                               0);
                if (count > 0) forward.channelToSocket.append(tunnelBuffer, count);
                else break;
            }
            while (!forward.channelToSocket.isEmpty()) {
                const ssize_t written = ::write(forward.socketFd,
                                                forward.channelToSocket.constData(),
                                                forward.channelToSocket.size());
                if (written > 0) forward.channelToSocket.remove(0, written);
                else break;
            }
            if (ssh_channel_is_eof(forward.channel)
                && forward.channelToSocket.isEmpty()) {
                close(forward.socketFd);
                ssh_channel_close(forward.channel);
                ssh_channel_free(forward.channel);
                activeForwards.removeAt(index);
            }
        }
    }

    for (ActiveForward &forward : activeForwards) {
        close(forward.socketFd);
        ssh_channel_close(forward.channel);
        ssh_channel_free(forward.channel);
    }

    uint32_t exitCode = static_cast<uint32_t>(-1);
    // libssh 0.12 使用结构化 exit state 取代已废弃的 get_exit_status；服务器未发送退出码时
    // 保留 -1 语义，界面据此区分正常 Shell 退出和网络中断。
    ssh_channel_get_exit_state(guard.channel, &exitCode, nullptr, nullptr);
    emit shellDisconnected(exitCode == static_cast<uint32_t>(-1)
                               ? -1
                               : static_cast<int>(exitCode));
}

bool SshTerminalWorker::shouldStop() const
{
    QMutexLocker locker(&m_mutex);
    return m_stopRequested;
}
