#include "ssh/SshSessionNode.h"
#include "ssh/KnownHostsManager.h"
#include "platform/NativeSocket.h"

#include <QFile>
#include <QLoggingCategory>
#include <QtEndian>

#include <algorithm>
#include <utility>

#include <cstring>

namespace {

/** @brief SSH 传输层诊断分类；中继已交付后的异步失败无法再走返回值，只能记录到日志。 */
Q_LOGGING_CATEGORY(sshLog, "shellium.ssh", QtWarningMsg)

/** @brief 后台 exec 与中继 stderr 最多保留的字节数，避免异常远端持续输出耗尽内存。 */
constexpr qsizetype kMaximumDiagnosticBytes = 4096;

/** @brief exec 中继启动后的短暂观察窗口；用于捕获“命令不存在”等立即失败。 */
constexpr qint64 kRelayConfirmationMs = 250;

/**
 * @brief 把远端诊断追加到受限缓冲。
 * @param destination 目标诊断缓冲。
 * @param data 本轮读到的数据。
 *
 * 保留最前面的诊断是有意选择：Shell 通常把“command not found”等根因写在第一行，后续
 * 重复输出价值较低。达到上限后直接丢弃尾部，确保远端无法用 stderr 制造无界内存增长。
 */
void appendBoundedDiagnostic(QByteArray &destination, const QByteArray &data)
{
    const qsizetype remaining = kMaximumDiagnosticBytes - destination.size();
    if (remaining > 0) {
        destination.append(data.left(remaining));
    }
}

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
    QString result = QObject::tr("Could not read the fingerprint");

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
 * @brief 生成在远端执行的中继命令，用它的标准输入输出充当到内网目标的字节管道。
 *
 * 服务器设了 AllowTcpForwarding no 时 direct-tcpip 会被按策略拒绝，但 session 通道不受
 * 该策略管辖。这正是 OpenSSH 早年文档里 `ProxyCommand "ssh gateway nc %h %p"` 的做法。
 *
 * 三种实现依次尝试，覆盖绝大多数 Linux 环境：
 *   1. bash 的 /dev/tcp——无需任何外部程序，最可预期；
 *   2. nc；
 *   3. socat。
 *
 * 用 `{ ...; } 3<>...` 复合重定向而不是 `exec 3<>...`：后者在非交互 Shell 中重定向失败会
 * 直接终止整个 Shell，后面的回退链就没机会执行了。trap 保证通道关闭时后台的 cat 一并退出，
 * 不在服务器上留下游荡进程。
 */
QByteArray buildRelayCommand(const QByteArray &host, const quint16 port)
{
    const QByteArray target = host + " " + QByteArray::number(port);
    const QByteArray devTcp = "/dev/tcp/" + host + "/" + QByteArray::number(port);
    return "{ cat <&3 & p=$!; trap \"kill $p 2>/dev/null\" EXIT; cat >&3; } 3<>" + devTcp
        + " 2>/dev/null && exit 0; "
          "command -v nc >/dev/null 2>&1 && exec nc " + target + "; "
          "command -v socat >/dev/null 2>&1 && exec socat - TCP:" + host + ":"
        + QByteArray::number(port) + "; "
          "echo shellium: no relay tool available >&2; exit 1";
}

/** @brief 构造一个失败结果。 */
SshSessionNode::StepResult failure(QString message)
{
    SshSessionNode::StepResult result;
    result.message = std::move(message);
    return result;
}

/** @brief 构造一个成功且已完成的结果。 */
SshSessionNode::StepResult success()
{
    SshSessionNode::StepResult result;
    result.ok = true;
    return result;
}

/** @brief 构造一个“尚未得出结论，请再次推进”的结果。 */
SshSessionNode::StepResult pending()
{
    SshSessionNode::StepResult result;
    result.ok = true;
    result.pending = true;
    return result;
}

/**
 * @brief 构造一个“需要用户提供密码”的结果。
 * @param retry 上一次密码是否已被服务器拒绝。
 */
SshSessionNode::StepResult credentialRequired(const bool retry)
{
    SshSessionNode::StepResult result;
    result.ok = true;
    result.pending = true;
    result.credentialRequired = true;
    result.credentialRetry = retry;
    return result;
}

/** @brief 允许的密码尝试次数，与 OpenSSH 默认一致。 */
constexpr int kMaxPasswordAttempts = 3;

/**
 * @brief 建立过程的整体超时。
 *
 * 非阻塞模式下 libssh 不再依据 SSH_OPTIONS_TIMEOUT 中断握手，必须由调用方自行限时，
 * 否则无响应的服务器会让该层永远停留在推进循环中。
 */
constexpr qint64 kEstablishTimeoutMs = 30000;

} // namespace

SshSessionNode::SshSessionNode(ConnectionTarget target, const Role role)
    : m_target(std::move(target))
    , m_role(role)
{
}

SshSessionNode::~SshSessionNode()
{
    for (ActiveForward &forward : m_forwards) {
        releaseForward(forward);
    }
    m_forwards.clear();

    if (m_execChannel != nullptr) {
        ssh_channel_free(m_execChannel);
        m_execChannel = nullptr;
    }

    for (PendingForward &pending : m_pendingForwards) {
        // 同样只 free 不 close：这些通道从未打开成功。
        ssh_channel_free(pending.channel);
        closeNativeSocket(pending.workerFd);
        closeNativeSocket(pending.callerFd);
    }
    m_pendingForwards.clear();

    if (m_shell != nullptr) {
        // 会话已经处于非阻塞模式，显式 close/send_eof 可能返回 SSH_AGAIN 并把半个关闭包
        // 留在整条 SSH 流里，进而破坏同一会话上的其他通道。libssh 的 free 会完成必要清理。
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
    m_secret.fill('\0');
}

SshSessionNode::StepResult SshSessionNode::beginEstablish(const NativeSocketHandle transportFd,
                                                          const int columns,
                                                          const int rows)
{
    // 调用即转移隧道句柄所有权。先放入 RAII 守卫，保证 ssh_new/ssh_options_set 的任何早退
    // 都会正确使用 closesocket/close；只有 libssh 明确接管成功后才 release。
    UniqueNativeSocket transportOwner(transportFd);
    m_columns = std::max(columns, 1);
    m_rows = std::max(rows, 1);
    m_terminalModes = interactiveTerminalModes();
    m_secret = m_target.password.toUtf8();

    m_session = ssh_new();
    if (m_session == nullptr) {
        m_stage = Stage::Failed;
        return failure(tr("Could not create a libssh session."));
    }

    const QByteArray host = m_target.host.toUtf8();
    const QByteArray user = m_target.username.toUtf8();
    const unsigned int port = m_target.port;
    ssh_options_set(m_session, SSH_OPTIONS_HOST, host.constData());
    if (!user.isEmpty()) {
        ssh_options_set(m_session, SSH_OPTIONS_USER, user.constData());
    }
    ssh_options_set(m_session, SSH_OPTIONS_PORT, &port);

    // 交互式终端的每次击键都是一个极小的包，Nagle 算法会把它压到上一个 ACK 回来之后再
    // 发；在高延迟链路上这表现为回显一顿一顿的。禁用后按键立刻上路。
    const int noDelay = 1;
    ssh_options_set(m_session, SSH_OPTIONS_NODELAY, &noDelay);

    if (isNativeSocketValid(transportFd)) {
        // SSH_OPTIONS_FD 让 libssh 在已连接 socket 上直接握手，同时 HOST 仍保持真实内层
        // 地址，known_hosts 校验不会错误绑定到隧道本地端。
        const socket_t socketHandle = static_cast<socket_t>(transportFd);
        if (ssh_options_set(m_session, SSH_OPTIONS_FD, &socketHandle) != SSH_OK) {
            m_stage = Stage::Failed;
            return failure(tr("Could not bind the SSH tunnel socket: %1").arg(lastError()));
        }
        static_cast<void>(transportOwner.release());
    } else {
        // 该超时只保护 libssh 内部仍然同步执行的部分（例如 DNS 解析）。握手本身在非阻塞
        // 模式下由 advanceEstablish 的整体计时器限时。
        const long timeoutSeconds = 15;
        ssh_options_set(m_session, SSH_OPTIONS_TIMEOUT, &timeoutSeconds);
    }

    if (!m_target.identityFile.trimmed().isEmpty()) {
        const QByteArray identity = QFile::encodeName(m_target.identityFile);
        ssh_options_set(m_session, SSH_OPTIONS_IDENTITY, identity.constData());
    }
    if (!isNativeSocketValid(transportFd) && !m_target.proxyJumps.isEmpty()) {
        // 复用隧道时外层路径已由隧道本身表达；此处再配置 ProxyJump 会让 libssh 试图从
        // 内层服务器重新跳一次，因此只在直连模式下应用跳板链。
        const QByteArray jumps = m_target.proxyJumps.join(QLatin1Char(',')).toUtf8();
        if (ssh_options_set(m_session, SSH_OPTIONS_PROXYJUMP, jumps.constData()) != SSH_OK) {
            m_stage = Stage::Failed;
            return failure(tr("Could not configure the SSH jump chain: %1").arg(lastError()));
        }
    }

    if (m_target.allowLegacyHmacSha1) {
        // hmac-sha1 只对用户明确确认的单个连接启用，并同时配置两个传输方向；如果只配置
        // client->server，部分旧服务端会在反向协商时再次失败。
        constexpr char legacyHmac[] = "hmac-sha1";
        ssh_options_set(m_session, SSH_OPTIONS_HMAC_C_S, legacyHmac);
        ssh_options_set(m_session, SSH_OPTIONS_HMAC_S_C, legacyHmac);
    }

    // 必须在 ssh_connect 之前切换：非阻塞模式是嵌套层不死锁的前提，握手期间工作线程要能
    // 回去泵送承载本层传输的外层转发通道。
    ssh_set_blocking(m_session, 0);

    m_stage = Stage::Connecting;
    m_establishTimer.start();
    return pending();
}

SshSessionNode::StepResult SshSessionNode::advanceEstablish()
{
    if (m_stage == Stage::Ready) {
        return success();
    }
    if (m_stage == Stage::Failed || m_stage == Stage::Idle) {
        return failure(tr("This layer has not started connecting yet or has already failed."));
    }
    if (m_establishTimer.isValid() && m_establishTimer.elapsed() > kEstablishTimeoutMs) {
        m_stage = Stage::Failed;
        return failure(tr("Connection to %1 timed out.").arg(m_target.connectionSpecifier(true)));
    }

    switch (m_stage) {
    case Stage::Connecting:
        return advanceConnecting();

    case Stage::AwaitingHostKeyDecision: {
        if (!m_hostKeyDecided) {
            return pending();
        }
        if (!m_hostKeyTrusted) {
            m_stage = Stage::Failed;
            return failure(m_hostKeyReplacementRequired
                               ? tr("The user did not confirm the server host key change; the connection was terminated.")
                               : tr("The user rejected the unknown server host key."));
        }
        if (m_hostKeyReplacementRequired) {
            QString replacementError;
            if (!KnownHostsManager::replaceChangedHostKey(m_session, &replacementError)) {
                m_stage = Stage::Failed;
                return failure(tr("Failed to replace the old host key in known_hosts: %1")
                                   .arg(replacementError));
            }
        } else if (ssh_session_update_known_hosts(m_session) != SSH_OK) {
            m_stage = Stage::Failed;
            return failure(tr("Failed to write known_hosts: %1").arg(lastError()));
        }
        m_stage = Stage::Authenticating;
        m_authPhase = AuthPhase::None;
        return pending();
    }

    case Stage::Authenticating:
        return advanceAuthenticating();

    case Stage::AwaitingCredential: {
        if (!m_credentialProvided) {
            return pending();
        }
        m_credentialProvided = false;
        if (m_secret.isEmpty()) {
            m_stage = Stage::Failed;
            return failure(tr("The user cancelled authentication for %1.")
                               .arg(m_target.connectionSpecifier(true)));
        }
        m_stage = Stage::Authenticating;
        m_authPhase = AuthPhase::Password;
        // 用户思考和输入密码的时间不该计入建立超时。
        m_establishTimer.start();
        return pending();
    }

    case Stage::OpeningChannel: {
        if (m_shell == nullptr) {
            m_shell = ssh_channel_new(m_session);
            if (m_shell == nullptr) {
                m_stage = Stage::Failed;
                return failure(tr("Could not create the SSH shell channel: %1").arg(lastError()));
            }
        }
        const int result = ssh_channel_open_session(m_shell);
        if (result == SSH_AGAIN) {
            return pending();
        }
        if (result != SSH_OK) {
            m_stage = Stage::Failed;
            return failure(tr("Could not create the SSH shell channel: %1").arg(lastError()));
        }
        m_stage = Stage::RequestingPty;
        return pending();
    }

    case Stage::RequestingPty: {
        const int result = ssh_channel_request_pty_size_modes(
            m_shell,
            "xterm-256color",
            m_columns,
            m_rows,
            reinterpret_cast<const unsigned char *>(m_terminalModes.constData()),
            static_cast<size_t>(m_terminalModes.size()));
        if (result == SSH_AGAIN) {
            return pending();
        }
        if (result != SSH_OK) {
            m_stage = Stage::Failed;
            return failure(tr("The server refused to allocate an interactive PTY: %1").arg(lastError()));
        }
        m_ptyApplied = true;
        m_stage = Stage::RequestingShell;
        return pending();
    }

    case Stage::RequestingShell: {
        const int result = ssh_channel_request_shell(m_shell);
        if (result == SSH_AGAIN) {
            return pending();
        }
        if (result != SSH_OK) {
            m_stage = Stage::Failed;
            return failure(tr("The server refused to start an interactive shell: %1").arg(lastError()));
        }
        m_stage = Stage::Ready;
        m_establishTimer.invalidate();
        // 握手刚刚结束，这一刻就是本层最近一次数据往来；从此开始计空闲。
        noteActivity();
        return success();
    }

    case Stage::Idle:
    case Stage::Ready:
    case Stage::Failed:
        break;
    }
    m_stage = Stage::Failed;
    return failure(tr("The connection state machine entered an undefined state."));
}

SshSessionNode::StepResult SshSessionNode::advanceConnecting()
{
    const int result = ssh_connect(m_session);
    if (result == SSH_AGAIN) {
        return pending();
    }
    if (result != SSH_OK) {
        const QString error = lastError();
        m_stage = Stage::Failed;
        // libssh 没有为“算法无交集”提供独立错误码，只能对其稳定错误文本做精确分类。
        // 同时要求出现 MAC 协商语义和 hmac-sha1，避免对普通 kex/网络错误错误降级。
        if (!m_target.allowLegacyHmacSha1
            && error.contains(QStringLiteral("no match for method mac algo"), Qt::CaseInsensitive)
            && error.contains(QStringLiteral("hmac-sha1"), Qt::CaseInsensitive)) {
            StepResult legacy = failure(error);
            legacy.legacyHmacRequired = true;
            return legacy;
        }
        return failure(tr("SSH connection failed: %1").arg(error));
    }
    return evaluateHostKey();
}

SshSessionNode::StepResult SshSessionNode::evaluateHostKey()
{
    const ssh_known_hosts_e knownState = ssh_session_is_known_server(m_session);
    if (knownState == SSH_KNOWN_HOSTS_CHANGED || knownState == SSH_KNOWN_HOSTS_OTHER) {
        // 密钥冲突不能自动信任，但也不应把正常的服务器重装变成只能手改文件的死路。暂停
        // 状态机并展示新指纹；只有用户通过可信渠道核验后才允许精确替换旧记录。
        m_stage = Stage::AwaitingHostKeyDecision;
        m_hostKeyDecided = false;
        m_hostKeyTrusted = false;
        m_hostKeyReplacementRequired = true;
        m_establishTimer.invalidate();

        StepResult result;
        result.ok = true;
        result.pending = true;
        result.hostKeyChanged = true;
        result.fingerprint = serverFingerprint(m_session);
        return result;
    }
    if (knownState == SSH_KNOWN_HOSTS_ERROR) {
        m_stage = Stage::Failed;
        return failure(tr("Failed to read known_hosts: %1").arg(lastError()));
    }
    if (knownState == SSH_KNOWN_HOSTS_UNKNOWN || knownState == SSH_KNOWN_HOSTS_NOT_FOUND) {
        m_stage = Stage::AwaitingHostKeyDecision;
        m_hostKeyDecided = false;
        m_hostKeyTrusted = false;
        m_hostKeyReplacementRequired = false;
        // 等待用户核验期间不能让整体超时把连接掐断，人核对指纹远比握手慢。
        m_establishTimer.invalidate();

        StepResult result;
        result.ok = true;
        result.pending = true;
        result.hostKeyUnknown = true;
        result.fingerprint = serverFingerprint(m_session);
        return result;
    }
    m_stage = Stage::Authenticating;
    m_authPhase = AuthPhase::None;
    return pending();
}

SshSessionNode::StepResult SshSessionNode::advanceAuthenticating()
{
    // libssh 在非阻塞模式下要求收到 SSH_AUTH_AGAIN 后用完全相同的参数重试，因此这里始终
    // 使用缓存的 m_secret，而不是每轮从 QString 重新生成临时缓冲。
    const char *secret = m_secret.isEmpty() ? nullptr : m_secret.constData();

    int result = SSH_AUTH_DENIED;
    switch (m_authPhase) {
    case AuthPhase::None:
        result = ssh_userauth_none(m_session, nullptr);
        if (result == SSH_AUTH_AGAIN) {
            return pending();
        }
        if (result == SSH_AUTH_SUCCESS) {
            return authenticationSucceeded();
        }
        // publickey_auto 会尝试 SSH Agent、默认密钥以及通过 SSH_OPTIONS_IDENTITY 指定的
        // 密钥。当用户输入的密码实际是私钥口令时，也允许 libssh 用它解密私钥。
        m_authPhase = AuthPhase::PublicKey;
        return pending();

    case AuthPhase::PublicKey:
        result = ssh_userauth_publickey_auto(m_session, nullptr, secret);
        if (result == SSH_AUTH_AGAIN) {
            return pending();
        }
        if (result == SSH_AUTH_SUCCESS) {
            return authenticationSucceeded();
        }
        if (m_secret.isEmpty()) {
            // 公钥与 Agent 都不被接受，且命令行没有携带密码——接管用户手打的 ssh 时这是
            // 常态。向上层索要密码，而不是直接判定失败。
            m_stage = Stage::AwaitingCredential;
            m_establishTimer.invalidate();
            return credentialRequired(m_passwordRejected);
        }
        m_authPhase = AuthPhase::Password;
        return pending();

    case AuthPhase::Password:
        result = ssh_userauth_password(m_session, nullptr, m_secret.constData());
        if (result == SSH_AUTH_AGAIN) {
            return pending();
        }
        if (result == SSH_AUTH_SUCCESS) {
            return authenticationSucceeded();
        }
        // 与 OpenSSH 一致地允许重试：输错一次不该让整层连接失败，否则用户必须从头再敲
        // 一遍 ssh 命令。
        m_secret.fill('\0');
        m_secret.clear();
        m_target.password.fill(QChar::Null);
        m_target.password.clear();
        m_passwordRejected = true;
        if (m_passwordAttempts < kMaxPasswordAttempts) {
            m_stage = Stage::AwaitingCredential;
            m_establishTimer.invalidate();
            return credentialRequired(true);
        }
        m_stage = Stage::Failed;
        return failure(tr("SSH authentication failed: %1. Check the password, private key, or SSH agent.").arg(lastError()));
    }

    m_stage = Stage::Failed;
    return failure(tr("The authentication state machine entered an undefined state."));
}

SshSessionNode::StepResult SshSessionNode::authenticationSucceeded()
{
    if (m_role == Role::Transit) {
        // 跳板层不启动远端 Shell。很多堡垒机账号刻意配置了 ForceCommand 或禁用 PTY，若仍
        // 沿用交互层流程会在认证成功后错误失败；转发通道只依赖已认证 session，直接 Ready
        // 即可。认证报文刚完成也属于链路活动，从此刻开始分层保活计时。
        m_stage = Stage::Ready;
        m_establishTimer.invalidate();
        noteActivity();
        return success();
    }
    m_stage = Stage::OpeningChannel;
    return pending();
}

void SshSessionNode::provideCredential(const QString &secret)
{
    if (m_stage != Stage::AwaitingCredential) {
        return;
    }
    m_secret = secret.toUtf8();
    // 同一份凭据写回目标，使随后的 SFTP 能直接复用而不必再次询问用户——这正是原先弹出
    // 第二个密码框的根源。
    m_target.password = secret;
    m_credentialProvided = true;
    if (!secret.isEmpty()) {
        ++m_passwordAttempts;
    }
}

void SshSessionNode::provideHostKeyDecision(const bool trusted)
{
    if (m_stage != Stage::AwaitingHostKeyDecision) {
        return;
    }
    m_hostKeyDecided = true;
    m_hostKeyTrusted = trusted;
    // 用户已作答，重新开始计时以便后续认证阶段仍受整体超时保护。
    m_establishTimer.start();
}

bool SshSessionNode::isReady() const
{
    return m_stage == Stage::Ready;
}

bool SshSessionNode::isAwaitingCredential() const
{
    return m_stage == Stage::AwaitingCredential && !m_credentialProvided;
}

void SshSessionNode::resizePty(const int columns, const int rows)
{
    m_columns = std::max(columns, 1);
    m_rows = std::max(rows, 1);
    if (m_shell == nullptr || !m_ptyApplied) {
        // 建立过程尚未走到 PTY 请求时只记录尺寸；RequestingPty 阶段会用最新值发起请求，
        // 因此窗口在连接期间被拖动也不会让远端拿到过时的行列数。
        return;
    }
    ssh_channel_change_pty_size(m_shell, m_columns, m_rows);
}

bool SshSessionNode::beginDetachedExec(const QByteArray &command)
{
    if (m_session == nullptr || m_execChannel != nullptr) {
        return false;
    }
    m_execChannel = ssh_channel_new(m_session);
    if (m_execChannel == nullptr) {
        return false;
    }
    m_execCommand = command;
    m_execDiagnostic.clear();
    m_execOutput.clear();
    m_execError.clear();
    m_execHasResult = false;
    m_execSucceeded = false;
    m_execStage = ExecStage::Opening;
    m_execTimer.start();
    return true;
}

void SshSessionNode::pumpExec()
{
    if (m_execChannel == nullptr) {
        return;
    }

    const auto finish = [this](const bool succeeded, const QString &reason = QString()) {
        // 只 free 不 close：非阻塞会话上 ssh_channel_close 会返回 SSH_AGAIN 并留下半写包，
        // 之后整条 SSH 流失步，连交互式 Shell 也会一起废掉。
        ssh_channel_free(m_execChannel);
        m_execChannel = nullptr;
        m_execStage = ExecStage::Finished;
        m_execHasResult = true;
        m_execSucceeded = succeeded;
        m_execError = succeeded ? QString() : reason;
    };

    // 安装脚本只是写个小文件；超时说明远端异常，此时放弃即可，交互式会话不受影响。
    constexpr qint64 kExecTimeoutMs = 15000;
    if (m_execTimer.isValid() && m_execTimer.elapsed() > kExecTimeoutMs) {
        finish(false, tr("The background command timed out"));
        return;
    }

    switch (m_execStage) {
    case ExecStage::Opening: {
        const int result = ssh_channel_open_session(m_execChannel);
        if (result == SSH_AGAIN) {
            return;
        }
        if (result != SSH_OK) {
            finish(false, tr("Could not open the background command channel: %1").arg(lastError()));
            return;
        }
        m_execStage = ExecStage::Requesting;
        return;
    }
    case ExecStage::Requesting: {
        const int result = ssh_channel_request_exec(m_execChannel, m_execCommand.constData());
        if (result == SSH_AGAIN) {
            return;
        }
        if (result != SSH_OK) {
            finish(false, tr("The remote refused to run the background command: %1").arg(lastError()));
            return;
        }
        m_execStage = ExecStage::Draining;
        return;
    }
    case ExecStage::Draining: {
        char buffer[1024];
        while (true) {
            const int count = ssh_channel_read_nonblocking(
                m_execChannel, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                break;
            }
            // 远端命令由服务器端 Shell 解释，异常登录脚本可能输出远超预期的数据；识别逻辑
            // 虽只需两行 os-release 字段，仍要限制 stdout，防止不可信服务端耗尽客户端内存。
            appendBoundedDiagnostic(m_execOutput, QByteArray(buffer, count));
        }
        while (true) {
            const int count = ssh_channel_read_nonblocking(
                m_execChannel, buffer, sizeof(buffer), 1);
            if (count <= 0) {
                break;
            }
            appendBoundedDiagnostic(m_execDiagnostic, QByteArray(buffer, count));
        }
        if (ssh_channel_is_eof(m_execChannel) != 0) {
            uint32_t exitCode = static_cast<uint32_t>(-1);
            ssh_channel_get_exit_state(m_execChannel, &exitCode, nullptr, nullptr);
            if (exitCode == 0) {
                finish(true);
            } else {
                const QString diagnostic = QString::fromUtf8(m_execDiagnostic).trimmed();
                const QString exitDescription = exitCode == static_cast<uint32_t>(-1)
                    ? tr("The remote returned no exit code")
                    : tr("Exit code %1").arg(exitCode);
                finish(false,
                       diagnostic.isEmpty()
                           ? exitDescription
                           : tr("%1: %2").arg(exitDescription, diagnostic));
            }
        }
        return;
    }
    case ExecStage::Idle:
    case ExecStage::Finished:
        return;
    }
}

bool SshSessionNode::takeExecResult(bool *succeeded,
                                    QByteArray *standardOutput,
                                    QString *errorMessage)
{
    if (!m_execHasResult) {
        return false;
    }
    m_execHasResult = false;
    if (succeeded != nullptr) {
        *succeeded = m_execSucceeded;
    }
    if (standardOutput != nullptr) {
        *standardOutput = m_execOutput;
    }
    if (errorMessage != nullptr) {
        *errorMessage = m_execError;
    }
    m_execError.clear();
    m_execOutput.clear();
    return true;
}

void SshSessionNode::queueOutgoing(const QByteArray &bytes)
{
    m_outgoing += bytes;
}

bool SshSessionNode::flushOutgoing()
{
    while (!m_outgoing.isEmpty()) {
        const int written = writeToShell(m_outgoing.constData(),
                                         static_cast<int>(m_outgoing.size()));
        if (written == SSH_AGAIN || written == 0) {
            return false;
        }
        if (written < 0) {
            // 写失败交由后续的交互式写入路径统一报错；这里只放弃注入，不影响会话可用性。
            m_outgoing.clear();
            return true;
        }
        m_outgoing.remove(0, written);
    }
    return true;
}

int SshSessionNode::writeToShell(const char *data, const int size)
{
    if (m_shell == nullptr) {
        return SSH_ERROR;
    }
    const int written = ssh_channel_write(m_shell, data, static_cast<uint32_t>(size));
    if (written > 0) {
        noteActivity();
    }
    return written;
}

int SshSessionNode::readFromShell(char *buffer, const int size, const bool stderrStream)
{
    if (m_shell == nullptr) {
        return 0;
    }
    const int count = ssh_channel_read_nonblocking(
        m_shell, buffer, static_cast<uint32_t>(size), stderrStream ? 1 : 0);
    if (count > 0) {
        noteActivity();
    }
    return count > 0 ? count : 0;
}

void SshSessionNode::noteActivity()
{
    // QElapsedTimer::start() 对已在计时的实例等同于重启，无需先判 isValid。
    m_idleTimer.start();
}

qint64 SshSessionNode::idleMs() const
{
    return m_idleTimer.isValid() ? m_idleTimer.elapsed() : -1;
}

SshKeepAlive::LayerIdleState SshSessionNode::keepAliveState() const
{
    return {isReady(),
            idleMs(),
            m_keepAliveAttemptTimer.isValid() ? m_keepAliveAttemptTimer.elapsed() : -1};
}

bool SshSessionNode::sendKeepAlive()
{
    if (m_session == nullptr) {
        return false;
    }
    // 在真正进入 libssh 前记录尝试时刻。无论成功还是缓冲暂满，下一轮都应经过最小退避；
    // 否则失败分支会按约 10ms 的 SSH 泵送周期忙重试。
    m_keepAliveAttemptTimer.start();
    // 空载荷足矣：RFC 4253 规定 SSH_MSG_IGNORE 的接收方解析后直接丢弃，而链路上的 NAT 与
    // 状态防火墙只看得到一个真实的加密报文，空闲表项就此刷新。
    if (ssh_send_ignore(m_session, "") != SSH_OK) {
        // 非阻塞会话上发送缓冲可能暂时写不下。这不是会话故障，绝不能据此断开，也绝不能
        // 在这里等待——工作线程一旦被钉住，整栈的数据搬运都会停摆。不重置计时，让下一轮
        // 在 keepAliveState 的最小重试窗口结束后再重试。
        return false;
    }
    noteActivity();
    return true;
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
    // requestPopLayer 发生时外层会话仍要继续使用；只 free 能避免非阻塞 close 留下半写包
    // 让整条复用连接失步。置空后 isShellAlive() 会立即反映该层已经关闭。
    ssh_channel_free(m_shell);
    m_shell = nullptr;
}

quint64 SshSessionNode::beginForward(const QString &host,
                                     const quint16 port,
                                     const bool nonBlockingCaller,
                                     const int timeoutMs,
                                     QString *error)
{
    const auto reportError = [error](const QString &message) -> quint64 {
        if (error != nullptr) {
            *error = message;
        }
        return 0;
    };

    if (m_session == nullptr) {
        return reportError(tr("The SSH session for this layer is not established yet"));
    }

    NativeSocketHandle socketPair[2] = {InvalidNativeSocket, InvalidNativeSocket};
    QString socketError;
    if (!createConnectedSocketPair(&socketPair[0], &socketPair[1], &socketError)) {
        return reportError(tr("Failed to create the SFTP tunnel socket: %1").arg(socketError));
    }

    ssh_channel channel = ssh_channel_new(m_session);
    if (channel == nullptr) {
        closeNativeSocket(socketPair[0]);
        closeNativeSocket(socketPair[1]);
        return reportError(tr("Could not allocate a forwarding channel on %1: %2")
                               .arg(m_target.connectionSpecifier(true), lastError()));
    }

    // 工作线程的轮询循环每约 10ms 才会调用一次 pumpForwards() 搬运这对 socket 里的数据
    // （交互终端空闲时尤其如此，参见 SshSessionStack::pumpLoop 的轮询节流）。内核默认的
    // socketpair 缓冲区往往只有几十 KB，SFTP 一侧的阻塞写会在缓冲区写满后卡住，直到下一次
    // 轮询才被排空——相当于把吞吐量摊薄到"缓冲区大小 / 10ms"，SFTP 流水线再深也无济于事。
    // 显式调大到 4MiB 让缓冲区能吸收多个轮询周期的数据量，轮询间隔就不再是瓶颈；
    // setsockopt 失败时保持系统默认值，不影响功能正确性。
    constexpr int kForwardSocketBufferBytes = 4 * 1024 * 1024;
    for (const NativeSocketHandle socketHandle : socketPair) {
        // 缓冲调优失败不影响正确性，保留系统默认值即可，因此这里有意忽略诊断。
        setNativeSocketBufferSize(socketHandle, kForwardSocketBufferBytes);
    }

    // 工作线程一侧必须非阻塞，否则轮询循环会被没有数据的隧道读操作卡住。
    setNativeSocketNonBlocking(socketPair[0], true);
    if (nonBlockingCaller) {
        // 嵌套层的 libssh 会话工作在非阻塞模式，其传输 fd 必须同样非阻塞，否则握手读取会
        // 阻塞工作线程并再次饿死承载它的外层通道。SFTP 的同步会话则需要保留阻塞语义。
        setNativeSocketNonBlocking(socketPair[1], true);
    }

    PendingForward pending;
    pending.id = m_nextForwardId++;
    // 已知该服务器禁用转发时直接走中继，不必每次先撞一次墙。
    pending.transport = m_forwardingProhibited ? ForwardTransport::ExecRelay
                                               : ForwardTransport::DirectTcpIp;
    pending.channel = channel;
    pending.workerFd = socketPair[0];
    pending.callerFd = socketPair[1];
    pending.host = host.toUtf8();
    pending.port = port;
    pending.timeoutMs = timeoutMs;
    pending.timer.start();
    m_pendingForwards.append(std::move(pending));
    return m_pendingForwards.constLast().id;
}

void SshSessionNode::discardPending(PendingForward &pending)
{
    if (pending.channel != nullptr) {
        // 只 free 不 close：通道可能从未真正打开，远端没有对应的 channel id；在非阻塞会话上
        // ssh_channel_close 会返回 SSH_AGAIN 并留下半写的 CHANNEL_CLOSE 包，之后整条 SSH 流
        // 失步，连带把本层的交互式 Shell 一起废掉。
        ssh_channel_free(pending.channel);
        pending.channel = nullptr;
    }
    if (isNativeSocketValid(pending.workerFd)) {
        closeNativeSocket(pending.workerFd);
        pending.workerFd = InvalidNativeSocket;
    }
    if (isNativeSocketValid(pending.callerFd)) {
        closeNativeSocket(pending.callerFd);
        pending.callerFd = InvalidNativeSocket;
    }
}

bool SshSessionNode::convertToRelay(PendingForward &pending)
{
    // 被拒的那条通道已经不可用，必须换一条新的；socketpair 两端仍然有效，继续沿用。
    if (pending.channel != nullptr) {
        ssh_channel_free(pending.channel);
    }
    pending.channel = ssh_channel_new(m_session);
    if (pending.channel == nullptr) {
        return false;
    }
    pending.transport = ForwardTransport::ExecRelay;
    pending.relayStage = RelayStage::OpeningSession;
    pending.relayExecStartedAtMs = -1;
    pending.relayDiagnostic.clear();
    pending.timer.start();
    return true;
}

bool SshSessionNode::advanceDirectForward(PendingForward &pending,
                                          QList<ForwardOutcome> &outcomes)
{
    const int result = ssh_channel_open_forward(
        pending.channel, pending.host.constData(), pending.port, "127.0.0.1", 0);

    if (result == SSH_AGAIN) {
        if (pending.timer.elapsed() <= pending.timeoutMs) {
            return false;
        }
        ForwardOutcome outcome;
        outcome.id = pending.id;
        // 远端向不可路由地址转发时会一直没有响应。没有时限，这条请求就会永远悬挂，
        // 发起方（建层或 SFTP）也就永远等不到结论。
        outcome.error = tr("Server %1 timed out forwarding to %2:%3 (%4 s)")
                            .arg(m_target.connectionSpecifier(true),
                                 QString::fromUtf8(pending.host))
                            .arg(pending.port)
                            .arg(pending.timeoutMs / 1000);
        outcomes.append(outcome);
        discardPending(pending);
        return true;
    }

    if (result == SSH_OK) {
        ForwardOutcome outcome;
        outcome.id = pending.id;
        outcome.ok = true;
        outcome.callerFd = pending.callerFd;
        m_forwards.append({pending.workerFd, pending.channel, {}, {}});
        pending.channel = nullptr;
        pending.workerFd = -1;
        pending.callerFd = InvalidNativeSocket;
        outcomes.append(outcome);
        return true;
    }

    // RFC 4254 的 SSH_OPEN_ADMINISTRATIVELY_PROHIBITED：服务端按策略拒绝转发
    // （AllowTcpForwarding no），而不是目标不可达。改用只需 session 通道的中继，
    // 该策略管不到它。
    const QString error = lastError();
    if (error.contains(QStringLiteral("error (1)"))) {
        m_forwardingProhibited = true;
        if (convertToRelay(pending)) {
            return false;
        }
    }

    ForwardOutcome outcome;
    outcome.id = pending.id;
    outcome.error = tr("Server %1 could not forward to %2:%3: %4")
                        .arg(m_target.connectionSpecifier(true),
                             QString::fromUtf8(pending.host))
                        .arg(pending.port)
                        .arg(error);
    outcomes.append(outcome);
    discardPending(pending);
    return true;
}

bool SshSessionNode::advanceRelayForward(PendingForward &pending,
                                         QList<ForwardOutcome> &outcomes)
{
    const auto fail = [&](const QString &reason) {
        ForwardOutcome outcome;
        outcome.id = pending.id;
        outcome.error = tr("Server %1 could not relay to %2:%3: %4")
                            .arg(m_target.connectionSpecifier(true),
                                 QString::fromUtf8(pending.host))
                            .arg(pending.port)
                            .arg(reason);
        outcomes.append(outcome);
        discardPending(pending);
        return true;
    };

    if (pending.timer.elapsed() > pending.timeoutMs) {
        return fail(tr("Still not established after waiting %1 s").arg(pending.timeoutMs / 1000));
    }

    switch (pending.relayStage) {
    case RelayStage::OpeningSession: {
        const int result = ssh_channel_open_session(pending.channel);
        if (result == SSH_AGAIN) {
            return false;
        }
        if (result != SSH_OK) {
            return fail(lastError());
        }
        pending.relayStage = RelayStage::RequestingExec;
        return false;
    }
    case RelayStage::RequestingExec: {
        const QByteArray command = buildRelayCommand(pending.host, pending.port);
        const int result = ssh_channel_request_exec(pending.channel, command.constData());
        if (result == SSH_AGAIN) {
            return false;
        }
        if (result != SSH_OK) {
            return fail(lastError());
        }
        // exec 请求被接受只表示服务端愿意启动命令，不表示远端确实有可用的中继工具。
        // 进入短暂确认阶段，先捕获“no relay tool available”等立即失败，再向调用方交付 fd。
        pending.relayStage = RelayStage::ConfirmingExec;
        pending.relayExecStartedAtMs = pending.timer.elapsed();
        return false;
    }
    case RelayStage::ConfirmingExec: {
        char diagnosticBuffer[1024];
        while (true) {
            const int count = ssh_channel_read_nonblocking(
                pending.channel, diagnosticBuffer, sizeof(diagnosticBuffer), 1);
            if (count <= 0) {
                break;
            }
            appendBoundedDiagnostic(pending.relayDiagnostic,
                                    QByteArray(diagnosticBuffer, count));
        }

        if (ssh_channel_is_eof(pending.channel) != 0) {
            const QString diagnostic = QString::fromUtf8(pending.relayDiagnostic).trimmed();
            return fail(diagnostic.isEmpty()
                            ? tr("The remote relay command exited immediately after starting")
                            : diagnostic);
        }
        if (pending.timer.elapsed() - pending.relayExecStartedAtMs < kRelayConfirmationMs) {
            return false;
        }

        // 中继在观察窗内持续运行：它的标准输入输出就是到目标的字节管道，后续搬运与
        // direct-tcpip 通道完全一致。此处才把 fd 交给调用方，避免伪成功。
        ForwardOutcome outcome;
        outcome.id = pending.id;
        outcome.ok = true;
        outcome.callerFd = pending.callerFd;
        m_forwards.append({pending.workerFd, pending.channel, {}, {}});
        pending.channel = nullptr;
        pending.workerFd = -1;
        pending.callerFd = InvalidNativeSocket;
        outcomes.append(outcome);
        return true;
    }
    }
    return fail(tr("The relay state machine entered an undefined state"));
}

QList<SshSessionNode::ForwardOutcome> SshSessionNode::advancePendingForwards()
{
    QList<ForwardOutcome> outcomes;
    for (qsizetype index = m_pendingForwards.size() - 1; index >= 0; --index) {
        PendingForward &pending = m_pendingForwards[index];
        const bool settled = pending.transport == ForwardTransport::DirectTcpIp
            ? advanceDirectForward(pending, outcomes)
            : advanceRelayForward(pending, outcomes);
        if (settled) {
            m_pendingForwards.removeAt(index);
        }
    }
    return outcomes;
}

QList<SshSessionNode::ForwardOutcome> SshSessionNode::pumpForwards()
{
    // 先推进待建立请求：新建成的通道本轮就能开始搬运数据，少一轮往返延迟。
    QList<ForwardOutcome> outcomes = advancePendingForwards();

    char tunnelBuffer[64 * 1024];
    for (qsizetype index = m_forwards.size() - 1; index >= 0; --index) {
        ActiveForward &forward = m_forwards[index];
        bool forwardRemoved = false;

        while (true) {
            const qint64 count = readNativeSocket(forward.socketFd,
                                                  tunnelBuffer,
                                                  sizeof(tunnelBuffer));
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
                qCWarning(sshLog) << "SSH 转发通道写入失败：" << lastError();
                // 这里同样不能显式 close。立即 free 并移除本条桥，避免下一轮继续访问坏通道。
                releaseForward(forward);
                m_forwards.removeAt(index);
                forwardRemoved = true;
                break;
            }
            // 转发通道上的密文同样承载在本层这条 TCP 上，因此也算本层活跃：内层会话正在
            // 忙碌时不必再为外层补发保活。
            noteActivity();
            forward.socketToChannel.remove(0, written);
        }
        if (forwardRemoved) {
            // 上面的写入失败分支已经移除了当前元素，不能继续访问失效的引用。
            continue;
        }

        // 中继命令的诊断信息走 stderr，不能混进数据流；只搬运标准输出。中继已交付后若
        // 异步失败，返回值通道已经不存在，因此把诊断写入专用日志供排障。
        QByteArray runtimeDiagnostic;
        while (true) {
            const int count = ssh_channel_read_nonblocking(
                forward.channel, tunnelBuffer, sizeof(tunnelBuffer), 1);
            if (count <= 0) {
                break;
            }
            appendBoundedDiagnostic(runtimeDiagnostic, QByteArray(tunnelBuffer, count));
        }
        if (!runtimeDiagnostic.isEmpty()) {
            qCWarning(sshLog).noquote()
                << tr("exec relay diagnostics for server %1: %2")
                       .arg(m_target.connectionSpecifier(true),
                            QString::fromUtf8(runtimeDiagnostic).trimmed());
        }
        while (true) {
            const int count = ssh_channel_read_nonblocking(
                forward.channel, tunnelBuffer, sizeof(tunnelBuffer), 0);
            if (count > 0) {
                noteActivity();
                forward.channelToSocket.append(tunnelBuffer, count);
            } else {
                break;
            }
        }
        while (!forward.channelToSocket.isEmpty()) {
            const qint64 written = writeNativeSocket(forward.socketFd,
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
    return outcomes;
}

quint64 SshSessionNode::beginRemoteListen(const QString &ruleId,
                                          const QString &bindHost,
                                          const quint16 bindPort,
                                          QString *error)
{
    const auto fail = [error](const QString &message) -> quint64 {
        if (error != nullptr) {
            *error = message;
        }
        return 0;
    };
    if (!isReady() || m_session == nullptr) {
        return fail(tr("The SSH session has not finished authenticating"));
    }
    if (ruleId.trimmed().isEmpty() || bindHost.trimmed().isEmpty() || bindPort == 0) {
        return fail(tr("The remote listening rule, address, and port must be valid"));
    }
    const auto sameRule = [&ruleId](const auto &listener) {
        return listener.ruleId == ruleId;
    };
    if (std::any_of(m_pendingRemoteListeners.cbegin(),
                    m_pendingRemoteListeners.cend(), sameRule)
        || std::any_of(m_remoteListeners.cbegin(), m_remoteListeners.cend(), sameRule)) {
        return fail(tr("This remote forwarding rule is already started or starting"));
    }

    PendingRemoteListen pendingListener;
    pendingListener.requestId = m_nextRemoteListenId++;
    pendingListener.ruleId = ruleId;
    pendingListener.bindHost = bindHost.trimmed().toUtf8();
    pendingListener.bindPort = bindPort;
    pendingListener.timer.start();
    m_pendingRemoteListeners.append(pendingListener);
    return pendingListener.requestId;
}

void SshSessionNode::cancelRemoteListen(const QString &ruleId)
{
    // 尚未得到服务端确认的请求本地直接撤销；上层同步请求会由 SshSessionStack 主动唤醒，
    // 因此这里不再制造一个“启动失败”结果与停止操作竞态。
    for (qsizetype index = m_pendingRemoteListeners.size() - 1; index >= 0; --index) {
        if (m_pendingRemoteListeners.at(index).ruleId == ruleId) {
            m_pendingRemoteListeners.removeAt(index);
        }
    }
    for (RemoteListener &listener : m_remoteListeners) {
        if (listener.ruleId == ruleId) {
            listener.cancelRequested = true;
        }
    }
}

void SshSessionNode::pumpRemoteForwarding(QList<RemoteListenOutcome> &outcomes,
                                          QList<RemoteAcceptedConnection> &accepted)
{
    constexpr qint64 kRemoteListenTimeoutMs = 15000;

    // tcpip-forward 是一次 SSH global request，在非阻塞会话上同样可能返回 SSH_AGAIN；每轮
    // 最多推进一次，绝不能在此自旋，否则交互 Shell 和既有转发都会饥饿。
    for (qsizetype index = m_pendingRemoteListeners.size() - 1; index >= 0; --index) {
        PendingRemoteListen &pendingListener = m_pendingRemoteListeners[index];
        int boundPort = pendingListener.bindPort;
        const int result = ssh_channel_listen_forward(m_session,
                                                      pendingListener.bindHost.constData(),
                                                      pendingListener.bindPort,
                                                      &boundPort);
        if (result == SSH_AGAIN && pendingListener.timer.elapsed() <= kRemoteListenTimeoutMs) {
            continue;
        }

        RemoteListenOutcome outcome;
        outcome.requestId = pendingListener.requestId;
        outcome.ruleId = pendingListener.ruleId;
        if (result == SSH_OK && boundPort > 0 && boundPort <= 65535) {
            outcome.ok = true;
            outcome.boundPort = static_cast<quint16>(boundPort);
            m_remoteListeners.append({pendingListener.ruleId,
                                      pendingListener.bindHost,
                                      outcome.boundPort,
                                      false});
        } else if (result == SSH_AGAIN) {
            outcome.error = tr("Timed out requesting remote listening on %1:%2")
                                .arg(QString::fromUtf8(pendingListener.bindHost))
                                .arg(pendingListener.bindPort);
        } else {
            outcome.error = tr("The server refused remote listening on %1:%2: %3")
                                .arg(QString::fromUtf8(pendingListener.bindHost))
                                .arg(pendingListener.bindPort)
                                .arg(lastError());
        }
        outcomes.append(outcome);
        m_pendingRemoteListeners.removeAt(index);
    }

    // 取消请求也可能返回 SSH_AGAIN。标志一直保留到服务端确认或明确失败，但从标记时起就不
    // 再把新 channel 交给运行时，确保用户按下“停止”后不会看到连接数继续增长。
    for (qsizetype index = m_remoteListeners.size() - 1; index >= 0; --index) {
        RemoteListener &listener = m_remoteListeners[index];
        if (!listener.cancelRequested) {
            continue;
        }
        const int result = ssh_channel_cancel_forward(m_session,
                                                      listener.bindHost.constData(),
                                                      listener.boundPort);
        if (result == SSH_AGAIN) {
            continue;
        }
        if (result != SSH_OK) {
            qCWarning(sshLog).noquote()
                << tr("Failed to cancel remote listener %1:%2: %3")
                       .arg(QString::fromUtf8(listener.bindHost))
                       .arg(listener.boundPort)
                       .arg(lastError());
        }
        m_remoteListeners.removeAt(index);
    }

    // 单轮设置上限，避免突发大量远程连接把终端 IO 永久饿死；剩余 channel 下轮继续接收。
    constexpr int kMaximumAcceptsPerPump = 32;
    for (int attempt = 0; attempt < kMaximumAcceptsPerPump; ++attempt) {
        int destinationPort = 0;
        char *originator = nullptr;
        int originatorPort = 0;
        ssh_channel channel = ssh_channel_open_forward_port(
            m_session, 0, &destinationPort, &originator, &originatorPort);
        if (originator != nullptr) {
            ssh_string_free_char(originator);
        }
        if (channel == nullptr) {
            break;
        }

        const auto listenerIt = std::find_if(
            m_remoteListeners.cbegin(), m_remoteListeners.cend(),
            [destinationPort](const RemoteListener &listener) {
                return !listener.cancelRequested && listener.boundPort == destinationPort;
            });
        if (listenerIt == m_remoteListeners.cend()) {
            // 监听已经停止或端口无法匹配；free-only 规则同样适用于服务端主动打开的 channel。
            ssh_channel_free(channel);
            continue;
        }

        NativeSocketHandle socketPair[2] = {InvalidNativeSocket, InvalidNativeSocket};
        QString socketError;
        if (!createConnectedSocketPair(&socketPair[0], &socketPair[1], &socketError)) {
            qCWarning(sshLog) << "为远程转发创建 socketpair 失败：" << socketError;
            ssh_channel_free(channel);
            continue;
        }
        // SshSessionStack 每轮泵送所有通道，工作端必须非阻塞；调用端交给独立转发运行时，
        // 后者会按自己的 poll 循环设置模式并负责最终关闭。
        setNativeSocketNonBlocking(socketPair[0], true);
        m_forwards.append({socketPair[0], channel, {}, {}});
        accepted.append({listenerIt->ruleId, socketPair[1]});
    }
}

void SshSessionNode::releaseForward(ActiveForward &forward)
{
    if (isNativeSocketValid(forward.socketFd)) {
        closeNativeSocket(forward.socketFd);
        forward.socketFd = InvalidNativeSocket;
    }
    if (forward.channel != nullptr) {
        // 同上：ssh_channel_free 会在需要时自行关闭，并且正确处理非阻塞会话上尚未完成的
        // 关闭握手；显式先 close 反而可能留下半写包并破坏整条 SSH 流。
        ssh_channel_free(forward.channel);
        forward.channel = nullptr;
    }
}
