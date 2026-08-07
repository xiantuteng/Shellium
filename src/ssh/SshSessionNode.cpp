#include "ssh/SshSessionNode.h"

#include <QFile>
#include <QtEndian>

#include <algorithm>
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

SshSessionNode::SshSessionNode(ConnectionTarget target, QByteArray shellIntegrationScript)
    : m_target(std::move(target))
    , m_shellIntegrationScript(std::move(shellIntegrationScript))
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
        ::close(pending.workerFd);
        ::close(pending.callerFd);
    }
    m_pendingForwards.clear();

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
    m_secret.fill('\0');
}

SshSessionNode::StepResult SshSessionNode::beginEstablish(const int transportFd,
                                                          const int columns,
                                                          const int rows)
{
    m_columns = std::max(columns, 1);
    m_rows = std::max(rows, 1);
    m_terminalModes = interactiveTerminalModes();
    m_secret = m_target.password.toUtf8();

    m_session = ssh_new();
    if (m_session == nullptr) {
        m_stage = Stage::Failed;
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
        // 该超时只保护 libssh 内部仍然同步执行的部分（例如 DNS 解析）。握手本身在非阻塞
        // 模式下由 advanceEstablish 的整体计时器限时。
        const long timeoutSeconds = 15;
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
            m_stage = Stage::Failed;
            return failure(tr("无法配置 SSH 跳板链：%1").arg(lastError()));
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
        return failure(tr("该层尚未开始建立或已经失败。"));
    }
    if (m_establishTimer.isValid() && m_establishTimer.elapsed() > kEstablishTimeoutMs) {
        m_stage = Stage::Failed;
        return failure(tr("连接 %1 超时。").arg(m_target.connectionSpecifier(true)));
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
            return failure(tr("用户拒绝了未知服务器主机密钥。"));
        }
        if (ssh_session_update_known_hosts(m_session) != SSH_OK) {
            m_stage = Stage::Failed;
            return failure(tr("写入 known_hosts 失败：%1").arg(lastError()));
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
            return failure(tr("用户取消了 %1 的认证。")
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
                return failure(tr("无法创建 SSH Shell 通道：%1").arg(lastError()));
            }
        }
        const int result = ssh_channel_open_session(m_shell);
        if (result == SSH_AGAIN) {
            return pending();
        }
        if (result != SSH_OK) {
            m_stage = Stage::Failed;
            return failure(tr("无法创建 SSH Shell 通道：%1").arg(lastError()));
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
            return failure(tr("服务器拒绝分配交互式 PTY：%1").arg(lastError()));
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
            return failure(tr("服务器拒绝启动交互式 Shell：%1").arg(lastError()));
        }
        m_stage = Stage::Ready;
        m_establishTimer.invalidate();
        return success();
    }

    case Stage::Idle:
    case Stage::Ready:
    case Stage::Failed:
        break;
    }
    m_stage = Stage::Failed;
    return failure(tr("连接状态机进入了未定义状态。"));
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
        return failure(tr("SSH 连接失败：%1").arg(error));
    }
    return evaluateHostKey();
}

SshSessionNode::StepResult SshSessionNode::evaluateHostKey()
{
    const ssh_known_hosts_e knownState = ssh_session_is_known_server(m_session);
    if (knownState == SSH_KNOWN_HOSTS_CHANGED || knownState == SSH_KNOWN_HOSTS_OTHER) {
        m_stage = Stage::Failed;
        return failure(tr("服务器主机密钥与 known_hosts 不一致，连接已终止。"));
    }
    if (knownState == SSH_KNOWN_HOSTS_ERROR) {
        m_stage = Stage::Failed;
        return failure(tr("读取 known_hosts 失败：%1").arg(lastError()));
    }
    if (knownState == SSH_KNOWN_HOSTS_UNKNOWN || knownState == SSH_KNOWN_HOSTS_NOT_FOUND) {
        m_stage = Stage::AwaitingHostKeyDecision;
        m_hostKeyDecided = false;
        m_hostKeyTrusted = false;
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
            m_stage = Stage::OpeningChannel;
            return pending();
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
            m_stage = Stage::OpeningChannel;
            return pending();
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
            m_stage = Stage::OpeningChannel;
            return pending();
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
        return failure(tr("SSH 认证失败：%1。请检查密码、私钥或 SSH Agent。").arg(lastError()));
    }

    m_stage = Stage::Failed;
    return failure(tr("认证状态机进入了未定义状态。"));
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
    m_execStage = ExecStage::Opening;
    m_execTimer.start();
    return true;
}

void SshSessionNode::pumpExec()
{
    if (m_execChannel == nullptr) {
        return;
    }

    const auto finish = [this](const bool succeeded) {
        // 只 free 不 close：非阻塞会话上 ssh_channel_close 会返回 SSH_AGAIN 并留下半写包，
        // 之后整条 SSH 流失步，连交互式 Shell 也会一起废掉。
        ssh_channel_free(m_execChannel);
        m_execChannel = nullptr;
        m_execStage = ExecStage::Finished;
        m_execHasResult = true;
        m_execSucceeded = succeeded;
    };

    // 安装脚本只是写个小文件；超时说明远端异常，此时放弃即可，交互式会话不受影响。
    constexpr qint64 kExecTimeoutMs = 15000;
    if (m_execTimer.isValid() && m_execTimer.elapsed() > kExecTimeoutMs) {
        finish(false);
        return;
    }

    switch (m_execStage) {
    case ExecStage::Opening: {
        const int result = ssh_channel_open_session(m_execChannel);
        if (result == SSH_AGAIN) {
            return;
        }
        if (result != SSH_OK) {
            finish(false);
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
            finish(false);
            return;
        }
        m_execStage = ExecStage::Draining;
        return;
    }
    case ExecStage::Draining: {
        char buffer[1024];
        for (const int stderrStream : {0, 1}) {
            while (ssh_channel_read_nonblocking(
                       m_execChannel, buffer, sizeof(buffer), stderrStream) > 0) {
            }
        }
        if (ssh_channel_is_eof(m_execChannel) != 0) {
            finish(true);
        }
        return;
    }
    case ExecStage::Idle:
    case ExecStage::Finished:
        return;
    }
}

bool SshSessionNode::takeExecResult(bool *succeeded)
{
    if (!m_execHasResult) {
        return false;
    }
    m_execHasResult = false;
    if (succeeded != nullptr) {
        *succeeded = m_execSucceeded;
    }
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
        return reportError(tr("该层 SSH 会话尚未建立"));
    }

    int socketPair[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, socketPair) != 0) {
        return reportError(tr("创建 SFTP 隧道 socket 失败：%1")
                               .arg(QString::fromLocal8Bit(std::strerror(errno))));
    }

    ssh_channel channel = ssh_channel_new(m_session);
    if (channel == nullptr) {
        ::close(socketPair[0]);
        ::close(socketPair[1]);
        return reportError(tr("无法在 %1 上分配转发通道：%2")
                               .arg(m_target.connectionSpecifier(true), lastError()));
    }

    // 工作线程一侧必须非阻塞，否则轮询循环会被没有数据的隧道读操作卡住。
    fcntl(socketPair[0], F_SETFL, fcntl(socketPair[0], F_GETFL, 0) | O_NONBLOCK);
    if (nonBlockingCaller) {
        // 嵌套层的 libssh 会话工作在非阻塞模式，其传输 fd 必须同样非阻塞，否则握手读取会
        // 阻塞工作线程并再次饿死承载它的外层通道。SFTP 的同步会话则需要保留阻塞语义。
        fcntl(socketPair[1], F_SETFL, fcntl(socketPair[1], F_GETFL, 0) | O_NONBLOCK);
    }

    PendingForward pending;
    pending.id = m_nextForwardId++;
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

QList<SshSessionNode::ForwardOutcome> SshSessionNode::advancePendingForwards()
{
    QList<ForwardOutcome> outcomes;
    for (qsizetype index = m_pendingForwards.size() - 1; index >= 0; --index) {
        PendingForward &pending = m_pendingForwards[index];

        const int result = ssh_channel_open_forward(
            pending.channel, pending.host.constData(), pending.port, "127.0.0.1", 0);

        if (result == SSH_AGAIN) {
            if (pending.timer.elapsed() <= pending.timeoutMs) {
                continue;
            }
            // 远端向不可路由地址转发时会一直没有响应。没有时限，这条请求就会永远悬挂，
            // 发起方（建层或 SFTP）也就永远等不到结论。
            ForwardOutcome outcome;
            outcome.id = pending.id;
            outcome.error = tr("服务器 %1 转发到 %2:%3 超时（%4 秒）")
                                .arg(m_target.connectionSpecifier(true),
                                     QString::fromUtf8(pending.host))
                                .arg(pending.port)
                                .arg(pending.timeoutMs / 1000);
            outcomes.append(outcome);
            // 只 free 不 close：通道从未真正打开，远端没有对应的 channel id；在非阻塞会话上
            // ssh_channel_close 会返回 SSH_AGAIN 并留下半写的 CHANNEL_CLOSE 包，之后整条
            // SSH 流失步，连带把本层的交互式 Shell 一起废掉。
            ssh_channel_free(pending.channel);
            ::close(pending.workerFd);
            ::close(pending.callerFd);
            m_pendingForwards.removeAt(index);
            continue;
        }

        ForwardOutcome outcome;
        outcome.id = pending.id;
        if (result == SSH_OK) {
            outcome.ok = true;
            outcome.callerFd = pending.callerFd;
            m_forwards.append({pending.workerFd, pending.channel, {}, {}});
        } else {
            outcome.error = tr("服务器 %1 无法转发到 %2:%3：%4")
                                .arg(m_target.connectionSpecifier(true),
                                     QString::fromUtf8(pending.host))
                                .arg(pending.port)
                                .arg(lastError());
            ssh_channel_free(pending.channel);
            ::close(pending.workerFd);
            ::close(pending.callerFd);
        }
        outcomes.append(outcome);
        m_pendingForwards.removeAt(index);
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
    return outcomes;
}

void SshSessionNode::releaseForward(ActiveForward &forward)
{
    if (forward.socketFd >= 0) {
        ::close(forward.socketFd);
        forward.socketFd = -1;
    }
    if (forward.channel != nullptr) {
        // 同上：ssh_channel_free 会在需要时自行关闭，并且正确处理非阻塞会话上尚未完成的
        // 关闭握手；显式先 close 反而可能留下半写包并破坏整条 SSH 流。
        ssh_channel_free(forward.channel);
        forward.channel = nullptr;
    }
}
