#include "sftp/SftpService.h"
#include "ssh/KnownHostsManager.h"
#include "platform/NativeSocket.h"

#include "ssh/SshTunnelBroker.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QMimeDatabase>
#include <QPointer>
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QWaitCondition>
#include <QtConcurrentRun>

#include <algorithm>
#include <deque>
#include <functional>
#include <memory>

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#if !defined(Q_OS_WIN)
#include <sys/time.h>
#endif

namespace {

/**
 * @brief 把一个参数编码成可安全交给远端 POSIX Shell 的单个 token。
 * @param value 未经转义的参数值。
 * @return 使用单引号包裹、并正确拆分内部单引号后的 UTF-8 字节串。
 *
 * 远端命令里出现的路径与主机名都来自用户数据，绝不能直接拼进 exec 字符串；否则空格、
 * 分号或命令替换语法可能改变要执行的命令。单引号编码只表达一个 argv，不做任何展开。
 */
QByteArray quotePosixShellArgument(QString value)
{
    value.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
    return "'" + value.toUtf8() + "'";
}

/** @brief 后台操作连接阶段的结构化结果。 */
struct ConnectResult {
    bool success = false;
    SftpService::ErrorKind kind = SftpService::ErrorKind::Connection;
    QString message;
    QString fingerprint;
};

} // namespace

namespace SftpRemoteRelay {

namespace {

/**
 * @brief 把一个参数编码成可安全交给远端 POSIX Shell 的单个 token。
 * @param value 未经转义的参数值。
 * @return 使用单引号包裹、并正确拆分内部单引号后的 UTF-8 字节串。
 *
 * 内层主机名与用户名来自终端命令，绝不能直接拼进外层服务器的 exec 字符串；否则空格、
 * 分号或命令替换语法可能改变要执行的命令。单引号编码只表达一个 argv，不做任何展开。
 */
QByteArray quoteRemoteShellArgument(const QString &value)
{
    return quotePosixShellArgument(value);
}

} // namespace

/**
 * @brief 构造在外层服务器上启动内层 SFTP 子系统的 OpenSSH 命令。
 * @param target 内层服务器；用户名、地址和端口均已由 SSH 命令解析器规范化。
 * @return 可交给 ssh_channel_request_exec 的安全命令行。
 *
 * `-s ... sftp` 要求 OpenSSH 把最后一个参数当作子系统名，而不是执行普通 Shell 命令；
 * `BatchMode=yes` 禁止密码提示混入二进制 SFTP 数据流，`ConnectTimeout` 防止错误地址让后台
 * 任务永久阻塞。认证由外层服务器自己的默认密钥、ssh-agent 与 ~/.ssh/config 完成，这正是
 * 本机无法读取远端私钥时仍能工作的关键。
 */
QByteArray buildCommand(const ConnectionTarget &target)
{
    QByteArray command = "command ssh -q -o BatchMode=yes -o ConnectTimeout=12 -p "
        + QByteArray::number(target.port);
    if (!target.username.trimmed().isEmpty()) {
        command += " -l " + quoteRemoteShellArgument(target.username.trimmed());
    }
    command += " -s -- " + quoteRemoteShellArgument(target.host.trimmed()) + " sftp";
    return command;
}

} // namespace SftpRemoteRelay

namespace {

/**
 * @brief 对 libssh 句柄进行 RAII 管理，确保所有失败分支都释放网络与 SFTP 资源。
 *
 * 类只在单个后台任务线程中使用，不在线程间共享 libssh 会话。
 */
class SftpConnection final {
public:
    SftpConnection() = default;
    SftpConnection(const SftpConnection &) = delete;
    SftpConnection &operator=(const SftpConnection &) = delete;

    ~SftpConnection()
    {
        close();
    }

    /**
     * @brief 建立 SSH、校验主机密钥、完成认证并初始化 SFTP 子系统。
     * @param target 目标服务器及完整跳板链。
     * @param allowVerifiedHostKey 用户是否已确认本次遇到的未知或已变更指纹。
     * @return 结构化连接结果；需要人工核验时会附带 SHA-256 指纹。
     */
    ConnectResult open(const ConnectionTarget &target, const bool allowVerifiedHostKey)
    {
        ConnectResult directResult = openDirect(target, allowVerifiedHostKey);
        if (directResult.success) {
            return directResult;
        }

        // 主机密钥异常必须由本机的显式确认流程处理，不能改走远端 OpenSSH 后绕过界面警告。
        // 只有传输失败或本机缺少内层认证材料时才尝试远端密钥兜底。
        if (target.tunnelRouteId.isEmpty()
            || (directResult.kind != SftpService::ErrorKind::Connection
                && directResult.kind != SftpService::ErrorKind::Authentication)) {
            return directResult;
        }

        ConnectionTarget relayOrigin;
        ConnectionTarget relayTarget;
        if (!SshTunnelBroker::instance().remoteSftpRelay(
                target.tunnelRouteId, &relayOrigin, &relayTarget)) {
            return directResult;
        }

        // 常规路径可能已经创建了半成品 libssh 会话或隧道 fd；必须完整释放后再把 m_session
        // 用作外层连接，否则旧句柄会泄漏，lastError 也可能错误引用第一次失败。
        close();
        const ConnectResult relayResult = openViaRemoteOpenSsh(relayOrigin, relayTarget);
        if (relayResult.success) {
            return relayResult;
        }

        ConnectResult combined = relayResult;
        combined.message = QObject::tr("常规 SFTP 路径失败：%1\n远端密钥 SFTP 兜底失败：%2")
                               .arg(directResult.message, relayResult.message);
        return combined;
    }

    [[nodiscard]] sftp_session handle() const { return m_sftp; }

    /**
     * @brief 计算远端文件的 SHA-256，用于在不搬运字节的前提下判断两端内容是否一致。
     * @return 小写十六进制摘要；无法计算时返回空（调用方必须据此降级，不能当作"不一致"）。
     *
     * 让服务器自己读一遍盘只花它的本地 I/O，几乎不占带宽；把整个远端文件拉回来算哈希则和
     * 重传一次一样贵，那样校验就失去了意义。因此拿不到远端哈希时正确的做法是直接重传。
     *
     * 经外层 OpenSSH 中继（openViaRemoteOpenSsh）时，m_session 连的是**外层**服务器，在那里
     * 执行命令会算到一个完全不相干的文件上，因此这条路径必须直接拒绝而不是凑合执行。
     */
    [[nodiscard]] QByteArray remoteSha256(const QString &path)
    {
        if (m_session == nullptr || m_remoteSftpChannel != nullptr || m_hashTool == HashTool::None) {
            return {};
        }
        const QByteArray quoted = quotePosixShellArgument(path);
        if (m_hashTool == HashTool::Unknown) {
            QByteArray digest = parseHexDigest(
                runRemoteCommand("sha256sum -- " + quoted));
            if (!digest.isEmpty()) {
                m_hashTool = HashTool::Sha256Sum;
                return digest;
            }
            digest = parseHexDigest(runRemoteCommand("shasum -a 256 -- " + quoted));
            if (!digest.isEmpty()) {
                m_hashTool = HashTool::Shasum;
                return digest;
            }
            // 两条命令都拿不到摘要：可能服务器没装这些工具，也可能只是这个文件读不了。
            // 单次失败不永久禁用（否则一个读不了的文件会让整棵树失去校验能力），但连续
            // 失败几次基本可以断定是服务器没有该命令，此后不再为每个文件白跑两次 exec。
            if (++m_hashProbeFailures >= kHashProbeFailureLimit) {
                m_hashTool = HashTool::None;
            }
            return {};
        }
        const QByteArray command = m_hashTool == HashTool::Sha256Sum
            ? "sha256sum -- " + quoted
            : "shasum -a 256 -- " + quoted;
        return parseHexDigest(runRemoteCommand(command));
    }

    /**
     * @brief 连接是否仍可用于下一次操作。
     *
     * 池化连接可能因服务器空闲超时、网络中断或承载它的隧道随标签页关闭而失效。复用前必须
     * 核对，否则用户会看到一次莫名其妙的失败，而不是自动重连。
     */
    [[nodiscard]] bool isAlive() const
    {
        const bool relayAlive = m_remoteSftpChannel == nullptr
            || (ssh_channel_is_open(m_remoteSftpChannel) != 0
                && ssh_channel_is_eof(m_remoteSftpChannel) == 0);
        return m_session != nullptr && m_sftp != nullptr && relayAlive
            && ssh_is_connected(m_session) != 0;
    }
    [[nodiscard]] QString lastError() const
    {
        return m_session == nullptr ? QObject::tr("未知 libssh 错误")
                                    : QString::fromUtf8(ssh_get_error(m_session));
    }

private:
    /**
     * @brief 使用本机 libssh 直连或经已注册隧道连接内层服务器。
     * @param target 内层目标。
     * @param allowVerifiedHostKey 是否已由用户确认内层服务器本次返回的指纹。
     * @return 完整连接与 SFTP 初始化结果。
     */
    ConnectResult openDirect(const ConnectionTarget &target, const bool allowVerifiedHostKey)
    {
        NativeSocketHandle tunnelFd = InvalidNativeSocket;
        if (!target.tunnelRouteId.isEmpty()) {
            QString tunnelError;
            tunnelFd = SshTunnelBroker::instance().openSocket(target.tunnelRouteId, &tunnelError);
            if (!isNativeSocketValid(tunnelFd)) {
                return failure(SftpService::ErrorKind::Connection,
                               QObject::tr("无法复用外层 SSH 会话：%1").arg(tunnelError));
            }
        }

        const ConnectResult connected = connectAndAuthenticate(
            target, allowVerifiedHostKey, tunnelFd, QObject::tr("SFTP"));
        if (!connected.success) {
            return connected;
        }

        m_sftp = sftp_new(m_session);
        if (m_sftp == nullptr || sftp_init(m_sftp) != SSH_OK) {
            return failure(SftpService::ErrorKind::Connection,
                           QObject::tr("服务器未能初始化 SFTP 子系统：%1")
                               .arg(QString::fromUtf8(ssh_get_error(m_session))));
        }
        return {.success = true};
    }

    /**
     * @brief 重连外层服务器，并让外层原生 OpenSSH 使用远端私钥启动内层 SFTP。
     * @param relayOrigin 外层服务器配置，包含本次会话可用的认证信息。
     * @param target 内层服务器配置。
     * @return 外层认证、exec 命令与 SFTP 协议初始化的综合结果。
     */
    ConnectResult openViaRemoteOpenSsh(const ConnectionTarget &relayOrigin,
                                       const ConnectionTarget &target)
    {
        // 外层已经由当前终端完成过主机密钥确认，known_hosts 应有记录；这里仍强制校验且不
        // 自动接受未知指纹，防止兜底路径成为绕过主机身份校验的后门。
        const ConnectResult connected = connectAndAuthenticate(
            relayOrigin, false, -1, QObject::tr("外层 SSH"));
        if (!connected.success) {
            return connected;
        }

        ssh_channel relayChannel = ssh_channel_new(m_session);
        if (relayChannel == nullptr) {
            return failure(SftpService::ErrorKind::Connection,
                           QObject::tr("无法创建远端 SFTP 中继通道：%1")
                               .arg(QString::fromUtf8(ssh_get_error(m_session))));
        }

        if (ssh_channel_open_session(relayChannel) != SSH_OK) {
            const QString error = QString::fromUtf8(ssh_get_error(m_session));
            ssh_channel_free(relayChannel);
            return failure(SftpService::ErrorKind::Connection,
                           QObject::tr("外层服务器拒绝启动 SFTP 中继会话：%1").arg(error));
        }

        const QByteArray command = SftpRemoteRelay::buildCommand(target);
        if (ssh_channel_request_exec(relayChannel, command.constData()) != SSH_OK) {
            const QString error = QString::fromUtf8(ssh_get_error(m_session));
            ssh_channel_free(relayChannel);
            return failure(SftpService::ErrorKind::Connection,
                           QObject::tr("外层服务器无法执行内层 OpenSSH：%1").arg(error));
        }

        // sftp_new_channel 虽然通常接收 subsystem request 创建的通道，但 SFTP 协议只依赖
        // 通道标准输入输出。这里 exec 的远端 OpenSSH 已把自己的 stdio 接到内层 sftp 子系统，
        // 因而可以直接由 libssh 的 SFTP 客户端发送 SSH_FXP_INIT 等协议包。
        m_sftp = sftp_new_channel(m_session, relayChannel);
        if (m_sftp == nullptr) {
            ssh_channel_free(relayChannel);
            return failure(SftpService::ErrorKind::Connection,
                           QObject::tr("无法绑定远端 OpenSSH 的 SFTP 数据通道：%1")
                               .arg(QString::fromUtf8(ssh_get_error(m_session))));
        }
        m_remoteSftpChannel = relayChannel;

        if (sftp_init(m_sftp) != SSH_OK) {
            const QString diagnostic = readRelayDiagnostic(relayChannel);
            const QString detail = diagnostic.isEmpty()
                ? QString::fromUtf8(ssh_get_error(m_session))
                : diagnostic;
            // 远端 OpenSSH 使用的是外层服务器上的密钥/Agent，本机密码输入框无法修复这条
            // 路径；按连接错误上报并展示真实 stderr，避免诱导用户输入一份不会被使用的密码。
            return failure(SftpService::ErrorKind::Connection,
                           QObject::tr("外层 OpenSSH 未能建立内层 SFTP：%1").arg(detail));
        }
        return {.success = true};
    }

    /**
     * @brief 配置、连接、校验并认证一个 SSH 会话。
     * @param target 实际要由本机 libssh 认证的服务器。
     * @param allowVerifiedHostKey 是否允许写入或替换此前已由界面确认的主机指纹。
     * @param transportFd 已连通的隧道 fd；负数表示按 host/proxyJumps 自行连接。
     * @param authenticationLabel 认证失败消息中的连接角色名称。
     * @return 成功时 m_session 已认证；失败时返回可分类诊断。
     */
    ConnectResult connectAndAuthenticate(const ConnectionTarget &target,
                                         const bool allowVerifiedHostKey,
                                         const NativeSocketHandle transportFd,
                                         const QString &authenticationLabel)
    {
        m_session = ssh_new();
        if (m_session == nullptr) {
            if (isNativeSocketValid(transportFd)) {
                // ssh_new 失败时 fd 尚未交给 libssh，仍由调用方负责关闭，避免隧道端点泄漏。
                closeNativeSocket(transportFd);
            }
            return failure(SftpService::ErrorKind::Connection,
                           QObject::tr("无法创建 libssh 会话。"));
        }

        const QByteArray host = target.host.toUtf8();
        const QByteArray user = target.username.toUtf8();
        const unsigned int port = target.port;
        ssh_options_set(m_session, SSH_OPTIONS_HOST, host.constData());
        if (isNativeSocketValid(transportFd)) {
            // SSH_OPTIONS_FD 接管 fd 的生命周期；HOST 仍保留真实地址，使 known_hosts 校验
            // 绑定内层主机而不是 socketpair 的本地端。
            const socket_t socketHandle = static_cast<socket_t>(transportFd);
            if (ssh_options_set(m_session, SSH_OPTIONS_FD, &socketHandle) != SSH_OK) {
                // 选项设置失败代表 libssh 没有接管 fd；此分支必须显式关闭调用方端点。
                closeNativeSocket(transportFd);
                return failure(SftpService::ErrorKind::Connection,
                               QObject::tr("无法绑定内层 SSH 隧道：%1")
                                   .arg(QString::fromUtf8(ssh_get_error(m_session))));
            }
        }
        if (!user.isEmpty()) {
            ssh_options_set(m_session, SSH_OPTIONS_USER, user.constData());
        }
        ssh_options_set(m_session, SSH_OPTIONS_PORT, &port);

        if (!target.identityFile.trimmed().isEmpty()) {
            const QByteArray identity = QFile::encodeName(target.identityFile);
            ssh_options_set(m_session, SSH_OPTIONS_IDENTITY, identity.constData());
        }
        if (!isNativeSocketValid(transportFd) && !target.proxyJumps.isEmpty()) {
            const QByteArray jumps = target.proxyJumps.join(QLatin1Char(',')).toUtf8();
            if (ssh_options_set(m_session, SSH_OPTIONS_PROXYJUMP, jumps.constData()) != SSH_OK) {
                return failure(SftpService::ErrorKind::Connection,
                               QObject::tr("无法配置跳板链：%1")
                                   .arg(QString::fromUtf8(ssh_get_error(m_session))));
            }
        }

        if (target.allowLegacyHmacSha1) {
            // 文件管理必须与终端使用完全相同的双向 MAC 配置，否则会出现“终端能连、SFTP 失败”。
            constexpr char legacyHmac[] = "hmac-sha1";
            ssh_options_set(m_session, SSH_OPTIONS_HMAC_C_S, legacyHmac);
            ssh_options_set(m_session, SSH_OPTIONS_HMAC_S_C, legacyHmac);
        }

        if (ssh_connect(m_session) != SSH_OK) {
            const QString error = QString::fromUtf8(ssh_get_error(m_session));
            if (!target.allowLegacyHmacSha1
                && error.contains(QStringLiteral("no match for method mac algo"), Qt::CaseInsensitive)
                && error.contains(QStringLiteral("hmac-sha1"), Qt::CaseInsensitive)) {
                return failure(SftpService::ErrorKind::LegacyAlgorithmRequired,
                               QObject::tr("服务器仅支持旧版 hmac-sha1：%1").arg(error));
            }
            return failure(SftpService::ErrorKind::Connection,
                           QObject::tr("SSH 连接失败：%1").arg(error));
        }

        const ConnectResult verification = verifyServerKey(allowVerifiedHostKey);
        if (!verification.success) {
            return verification;
        }

        const QByteArray secret = target.password.toUtf8();
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
            return failure(SftpService::ErrorKind::Authentication,
                           QObject::tr("%1 认证失败：%2。请检查用户名、密码、私钥或 SSH Agent。")
                               .arg(authenticationLabel,
                                    QString::fromUtf8(ssh_get_error(m_session))));
        }
        return {.success = true};
    }

    /**
     * @brief 读取远端 OpenSSH 写入扩展数据流的短诊断。
     * @param channel 已执行 `ssh -s ... sftp` 的外层 session channel。
     * @return 去除首尾空白且限制为 4096 字节的错误文本。
     */
    static QString readRelayDiagnostic(ssh_channel channel)
    {
        QByteArray diagnostic;
        char buffer[1024];
        while (diagnostic.size() < 4096) {
            const int count = ssh_channel_read_timeout(
                channel, buffer, sizeof(buffer), 1, 100);
            if (count <= 0) {
                break;
            }
            const qsizetype remainingCapacity = 4096 - diagnostic.size();
            diagnostic.append(buffer,
                              std::min<qsizetype>(count, remainingCapacity));
        }
        return QString::fromUtf8(diagnostic).trimmed();
    }

    /** @brief 释放当前 SFTP、通道与 SSH 会话，使同一对象可以安全尝试另一条连接路径。 */
    void close()
    {
        if (m_sftp != nullptr) {
            // sftp_free 同时释放 sftp_new_channel 接管的 relay channel，不能再次 free。
            sftp_free(m_sftp);
            m_sftp = nullptr;
            m_remoteSftpChannel = nullptr;
        }
        if (m_session != nullptr) {
            ssh_disconnect(m_session);
            ssh_free(m_session);
            m_session = nullptr;
        }
    }

    ConnectResult verifyServerKey(const bool allowVerifiedHostKey)
    {
        ssh_key serverKey = nullptr;
        unsigned char *hash = nullptr;
        size_t hashLength = 0;
        QString fingerprint = QObject::tr("无法读取指纹");

        if (ssh_get_server_publickey(m_session, &serverKey) == SSH_OK
            && ssh_get_publickey_hash(serverKey,
                                      SSH_PUBLICKEY_HASH_SHA256,
                                      &hash,
                                      &hashLength) == SSH_OK) {
            char *rawFingerprint = ssh_get_fingerprint_hash(
                SSH_PUBLICKEY_HASH_SHA256, hash, hashLength);
            if (rawFingerprint != nullptr) {
                fingerprint = QString::fromUtf8(rawFingerprint);
                ssh_string_free_char(rawFingerprint);
            }
        }
        if (hash != nullptr) {
            ssh_clean_pubkey_hash(&hash);
        }
        if (serverKey != nullptr) {
            ssh_key_free(serverKey);
        }

        const ssh_known_hosts_e state = ssh_session_is_known_server(m_session);
        if (state == SSH_KNOWN_HOSTS_OK) {
            return {.success = true};
        }
        if (state == SSH_KNOWN_HOSTS_CHANGED || state == SSH_KNOWN_HOSTS_OTHER) {
            if (!allowVerifiedHostKey) {
                ConnectResult result = failure(
                    SftpService::ErrorKind::HostKeyChanged,
                    QObject::tr("服务器主机密钥与 known_hosts 记录不一致，必须先核验新指纹。"));
                result.fingerprint = fingerprint;
                return result;
            }

            // 只有界面展示新指纹且用户明确确认后才会进入此分支。与首次信任不同，冲突旧行
            // 必须先被精确替换；简单追加会留下旧行，下一次连接仍会继续报 CHANGED。
            QString replacementError;
            if (!KnownHostsManager::replaceChangedHostKey(m_session, &replacementError)) {
                return failure(SftpService::ErrorKind::Connection,
                               QObject::tr("替换 known_hosts 中的旧主机密钥失败：%1")
                                   .arg(replacementError));
            }
            return {.success = true};
        }
        if ((state == SSH_KNOWN_HOSTS_UNKNOWN || state == SSH_KNOWN_HOSTS_NOT_FOUND)
            && !allowVerifiedHostKey) {
            ConnectResult result = failure(SftpService::ErrorKind::HostKeyUnknown,
                                           QObject::tr("该服务器尚未被信任。"));
            result.fingerprint = fingerprint;
            return result;
        }
        if (state == SSH_KNOWN_HOSTS_ERROR) {
            return failure(SftpService::ErrorKind::Connection,
                           QObject::tr("读取 known_hosts 失败：%1")
                               .arg(QString::fromUtf8(ssh_get_error(m_session))));
        }

        // 只有界面已展示指纹且用户明确确认时 allowVerifiedHostKey 才会为 true。
        if (ssh_session_update_known_hosts(m_session) != SSH_OK) {
            return failure(SftpService::ErrorKind::Connection,
                           QObject::tr("写入 known_hosts 失败：%1")
                               .arg(QString::fromUtf8(ssh_get_error(m_session))));
        }
        return {.success = true};
    }

    /**
     * @brief 在同一条 SSH 会话上开一条 exec 通道执行命令，并收集其标准输出。
     * @return 标准输出原文；执行失败、超时或无输出时返回空。
     *
     * 调用方必须已持有该连接的 mutex：libssh 会话不是线程安全的。命令固定为只读的摘要
     * 计算，不接受来自界面的任意命令，参数一律经过单引号编码。
     */
    [[nodiscard]] QByteArray runRemoteCommand(const QByteArray &command)
    {
        ssh_channel channel = ssh_channel_new(m_session);
        if (channel == nullptr) {
            return {};
        }
        if (ssh_channel_open_session(channel) != SSH_OK) {
            ssh_channel_free(channel);
            return {};
        }
        if (ssh_channel_request_exec(channel, command.constData()) != SSH_OK) {
            ssh_channel_close(channel);
            ssh_channel_free(channel);
            return {};
        }

        QByteArray output;
        QElapsedTimer timer;
        timer.start();
        char buffer[512];
        while (output.size() < 4096 && timer.elapsed() < kRemoteCommandTimeoutMs) {
            const int count = ssh_channel_read_timeout(channel, buffer, sizeof(buffer), 0, 500);
            if (count < 0) {
                break;
            }
            if (count == 0) {
                if (ssh_channel_is_eof(channel) != 0) {
                    break;
                }
                continue;
            }
            output.append(buffer, count);
        }
        ssh_channel_send_eof(channel);
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        return output;
    }

    /** @brief 从摘要工具输出里取出 64 位十六进制摘要；格式不符一律返回空。 */
    [[nodiscard]] static QByteArray parseHexDigest(const QByteArray &output)
    {
        const QByteArray trimmed = output.trimmed();
        if (trimmed.size() < 64) {
            return {};
        }
        const QByteArray candidate = trimmed.left(64).toLower();
        for (const char character : candidate) {
            const bool hexadecimal = (character >= '0' && character <= '9')
                || (character >= 'a' && character <= 'f');
            if (!hexadecimal) {
                return {};
            }
        }
        return candidate;
    }

    static ConnectResult failure(const SftpService::ErrorKind kind, const QString &message)
    {
        return {.success = false, .kind = kind, .message = message};
    }

    /** @brief 远端摘要命令探测结果，按连接缓存，避免每个文件都试一遍两条命令。 */
    enum class HashTool { Unknown, Sha256Sum, Shasum, None };

    /** @brief 远端摘要命令的等待上限；超时只会退化为重传，不影响正确性。 */
    static constexpr qint64 kRemoteCommandTimeoutMs = 60000;

    ssh_session m_session = nullptr;
    sftp_session m_sftp = nullptr;
    /** @brief 远端 OpenSSH 兜底使用的 exec channel；所有权随 m_sftp 一起释放。 */
    ssh_channel m_remoteSftpChannel = nullptr;
    /** @brief 连续探测失败的次数，达到上限后判定服务器没有摘要命令。 */
    static constexpr int kHashProbeFailureLimit = 3;

    HashTool m_hashTool = HashTool::Unknown;
    int m_hashProbeFailures = 0;
};

using SftpRecursive::joinRemotePath;

/** @brief 把连接阶段错误异步投递回 SftpService 所在的 GUI 线程。 */
void deliverConnectionFailure(const QPointer<SftpService> &service,
                              const quint64 requestId,
                              const ConnectionTarget &target,
                              const ConnectResult &result)
{
    if (service == nullptr) {
        return;
    }
    QMetaObject::invokeMethod(service, [service, requestId, target, result]() {
        if (service == nullptr) {
            return;
        }
        if (result.kind == SftpService::ErrorKind::HostKeyUnknown) {
            emit service->hostKeyVerificationRequired(target, result.fingerprint);
        } else if (result.kind == SftpService::ErrorKind::HostKeyChanged) {
            emit service->hostKeyChangeVerificationRequired(target, result.fingerprint);
        }
        emit service->operationFailed(requestId, target, result.kind, result.message);
    }, Qt::QueuedConnection);
}

/** @brief 把字节级进度投递回 GUI 线程；服务已销毁时静默丢弃。 */
void postTransferProgress(const QPointer<SftpService> &service,
                          const quint64 requestId,
                          const quint64 completed,
                          const quint64 total)
{
    if (service == nullptr) {
        return;
    }
    QMetaObject::invokeMethod(service, [service, requestId, completed, total]() {
        if (service != nullptr) {
            emit service->transferProgress(requestId, completed, total);
        }
    }, Qt::QueuedConnection);
}

/** @brief 把递归传输的条目级进度投递回 GUI 线程。 */
void postRecursiveProgress(const QPointer<SftpService> &service,
                           const quint64 requestId,
                           const quint64 filesCompleted,
                           const quint64 filesTotal,
                           const QString &currentItem)
{
    if (service == nullptr) {
        return;
    }
    QMetaObject::invokeMethod(service, [service, requestId, filesCompleted, filesTotal, currentItem]() {
        if (service != nullptr) {
            emit service->recursiveTransferProgress(requestId, filesCompleted, filesTotal, currentItem);
        }
    }, Qt::QueuedConnection);
}

/** @brief 把递归传输汇总投递回 GUI 线程。 */
void postRecursiveFinished(const QPointer<SftpService> &service,
                           const quint64 requestId,
                           const SftpRecursive::TransferSummary &summary)
{
    if (service == nullptr) {
        return;
    }
    QMetaObject::invokeMethod(service, [service, requestId, summary]() {
        if (service != nullptr) {
            emit service->recursiveTransferFinished(requestId, summary);
        }
    }, Qt::QueuedConnection);
}

/**
 * @brief 一条按目标缓存、可跨请求复用的 SFTP 连接。
 *
 * libssh 会话不是线程安全的，因此同一条连接上的操作由 mutex 串行化；不同目标、以及同一
 * 目标的不同通道（传输 / 目录浏览）仍然并行。
 */
struct PooledConnection {
    QMutex mutex;
    std::unique_ptr<SftpConnection> connection;
    QElapsedTimer idleTimer;
};

/**
 * @brief 传输与远程改动共用的连接池键。
 *
 * 同一台服务器上的写操作串行执行是合理的：用户的复制、改名、删除本来就有先后语义。
 */
QString transferPoolKey(const ConnectionTarget &target)
{
    return target.stableKey();
}

/**
 * @brief 目录浏览专用的连接池键。
 *
 * 浏览若与传输共用一条连接，就要排在传输的 mutex 后面，于是"上传一个大文件期间无法切换
 * 目录"。浏览是只读且短暂的，单独占一条连接不会与传输产生任何一致性问题，代价只是每台
 * 服务器多一次登录，而它同样会被连接池缓存复用。
 */
QString browsePoolKey(const ConnectionTarget &target)
{
    return target.stableKey() + QStringLiteral("|lane=browse");
}

} // namespace

/**
 * @brief 按目标缓存 SFTP 连接，避免每次文件操作都重做一次完整 SSH 握手。
 *
 * 原实现每个请求新建一条连接：握手、指纹校验、认证、sftp_init 全套重来。对嵌套层还要
 * 额外开一条 direct-tcpip 隧道，于是"点开一个目录"要花掉数次完整登录的时间。
 */
struct SftpConnectionPool {
    /** @brief 空闲超过该时长的连接会被丢弃，避免长期占用 fd 和服务器会话槽。 */
    static constexpr qint64 kIdleTimeoutMs = 300000;

    /** @brief 取出（必要时创建）目标对应的池条目。 */
    QSharedPointer<PooledConnection> acquire(const QString &key)
    {
        QMutexLocker locker(&mutex);
        evictIdleLocked();
        auto iterator = entries.find(key);
        if (iterator == entries.end()) {
            iterator = entries.insert(key, QSharedPointer<PooledConnection>::create());
        }
        return iterator.value();
    }

    /** @brief 丢弃某个目标的缓存连接，用于连接级错误后强制重连。 */
    void invalidate(const QString &key)
    {
        QMutexLocker locker(&mutex);
        entries.remove(key);
    }

    /** @brief 清空全部缓存，供服务析构时释放所有网络资源。 */
    void clear()
    {
        {
            QMutexLocker locker(&mutex);
            entries.clear();
        }
        {
            QMutexLocker controlLocker(&controlMutex);
            // 服务销毁时暂停任务若不被唤醒，会永远持有连接池 shared_ptr 并泄漏线程。
            // 将所有暂停请求转成取消后统一唤醒，让后台循环在下一个边界安全退出。
            cancelledRequests.unite(pausedRequests);
            pausedRequests.clear();
            controlChanged.wakeAll();
        }
    }

    /**
     * @brief 标记某个请求为已取消。
     *
     * 取消标志放在池里而不是 SftpService 里：后台任务只持有池的 shared_ptr，服务本身可能在
     * 传输结束前就被销毁（用户关掉了标签页），那正是最需要让循环停下来的时刻。
     */
    void requestCancel(const quint64 requestId)
    {
        QMutexLocker locker(&controlMutex);
        cancelledRequests.insert(requestId);
        pausedRequests.remove(requestId);
        controlChanged.wakeAll();
    }

    /** @brief 标记请求暂停；后台循环会在下一个 64 KiB 边界进入条件等待。 */
    void requestPause(const quint64 requestId)
    {
        QMutexLocker locker(&controlMutex);
        if (!cancelledRequests.contains(requestId)) {
            pausedRequests.insert(requestId);
        }
    }

    /** @brief 清除暂停标志并唤醒对应后台循环。 */
    void requestResume(const quint64 requestId)
    {
        QMutexLocker locker(&controlMutex);
        pausedRequests.remove(requestId);
        controlChanged.wakeAll();
    }

    /**
     * @brief 在分块边界等待请求恢复，或报告请求已经取消。
     * @return true 表示调用方应停止传输，false 表示可以继续下一块。
     *
     * 使用条件变量而不是轮询休眠，暂停期间不会消耗 CPU；取消会先移除暂停标志再唤醒，
     * 因此暂停中的任务也能立即进入安全退出路径。
     */
    [[nodiscard]] bool waitUntilRunnable(const quint64 requestId)
    {
        QMutexLocker locker(&controlMutex);
        while (pausedRequests.contains(requestId)
               && !cancelledRequests.contains(requestId)) {
            controlChanged.wait(&controlMutex);
        }
        return cancelledRequests.contains(requestId);
    }

    /** @brief 请求结束后清除标志，避免取消集合随会话时长无限增长。 */
    void clearCancel(const quint64 requestId)
    {
        QMutexLocker locker(&controlMutex);
        cancelledRequests.remove(requestId);
        pausedRequests.remove(requestId);
    }

private:
    /** @brief 移除空闲过久的条目；调用方必须已持有 mutex。 */
    void evictIdleLocked()
    {
        for (auto iterator = entries.begin(); iterator != entries.end();) {
            const QSharedPointer<PooledConnection> &entry = iterator.value();
            // 正在被某个请求使用的条目一定拿不到锁，此时跳过即可，下一轮再评估。
            if (entry->mutex.tryLock()) {
                const bool expired = entry->connection != nullptr
                    && entry->idleTimer.isValid()
                    && entry->idleTimer.elapsed() > kIdleTimeoutMs;
                entry->mutex.unlock();
                if (expired) {
                    iterator = entries.erase(iterator);
                    continue;
                }
            }
            ++iterator;
        }
    }

    QMutex mutex;
    QHash<QString, QSharedPointer<PooledConnection>> entries;

    /** @brief 保护取消、暂停集合并供条件变量原子释放/重获的控制锁。 */
    QMutex controlMutex;

    /** @brief 暂停或取消状态变化时唤醒等待中的传输线程。 */
    QWaitCondition controlChanged;

    /** @brief 已请求取消、等待后台循环退出的请求编号。 */
    QSet<quint64> cancelledRequests;

    /** @brief 当前停在分块边界或将在下一边界暂停的请求编号。 */
    QSet<quint64> pausedRequests;
};

namespace {

/**
 * @brief 借出目标对应的池化连接，必要时重新建立。
 * @return 可用连接；失败时返回 nullptr 并已向界面投递结构化错误。
 *
 * 调用方必须已持有该条目的 mutex：libssh 会话不是线程安全的。
 */
SftpConnection *ensureConnected(PooledConnection *entry,
                                const QString &poolKey,
                                const ConnectionTarget &target,
                                const bool allowUnknownHost,
                                const QPointer<SftpService> &service,
                                const quint64 requestId,
                                SftpConnectionPool *pool)
{
    if (entry->connection != nullptr && entry->connection->isAlive()) {
        entry->idleTimer.restart();
        return entry->connection.get();
    }

    // 连接可能因服务器空闲超时、网络中断或承载它的隧道随标签页关闭而失效；此处透明重连，
    // 用户不该为了一次后台超时而看到失败。
    entry->connection = std::make_unique<SftpConnection>();
    const ConnectResult opened = entry->connection->open(target, allowUnknownHost);
    if (!opened.success) {
        entry->connection.reset();
        pool->invalidate(poolKey);
        deliverConnectionFailure(service, requestId, target, opened);
        return nullptr;
    }
    entry->idleTimer.start();
    return entry->connection.get();
}

/** @brief 单个文件传输的结局；取消必须与失败区分，否则会被当成错误弹窗。 */
enum class TransferOutcome { Ok, Failed, Cancelled };

/** @brief 每传输一段就上报的进度回调，参数是本次新增的字节数。 */
using ProgressReporter = std::function<void(quint64 deltaBytes)>;

/** @brief 分块边界上的取消轮询；返回 true 表示应尽快停止。 */
using CancelProbe = std::function<bool()>;

constexpr qint64 kTransferChunkBytes = 64 * 1024;

/**
 * @brief 上传流水线深度：允许同时有多少个 SSH_FXP_WRITE 请求处于未确认状态。
 *
 * sftp_write() 是同步的，每写一个约 32KB 的协议包就要等服务端确认，吞吐量因此被单次
 * RTT 卡死而不是被带宽卡死。用 sftp_aio_begin_write/sftp_aio_wait_write 维持多个
 * in-flight 请求，才能把吞吐量提到带宽上限。
 *
 * 经跳板链或经远端 OpenSSH 中继（openViaRemoteOpenSsh）的连接，有效往返时延比直连明显
 * 更高：带宽时延积（带宽 × 往返时延）可能超出较浅的流水线所能覆盖的在途字节数，届时
 * 吞吐量又会退化成被往返时延卡死。64 与 OpenSSH 自带 sftp 客户端的默认并发请求数
 * （-R 64）一致，是经过大规模验证的取值，服务端不会因此感到意外。
 */
constexpr int kUploadPipelineDepth = 64;

/** @brief 账本检查点间隔：每确认写入这么多字节就更新一次已传字节数。 */
constexpr quint64 kLedgerCheckpointBytes = 8ULL * 1024ULL * 1024ULL;

/** @brief 一次文件传输所需的续传上下文，上传与下载共用。 */
struct TransferContext {
    /** @brief 续传凭据账本；后台任务全程持有，服务销毁后仍可写入。 */
    SftpResume::Ledger *ledger = nullptr;

    /** @brief 目标服务器的稳定键，用于隔离不同服务器上的同名路径。 */
    QString targetKey;

    /** @brief 用户选择的续传或覆盖策略。 */
    SftpTransfer::ExistingFilePolicy policy =
        SftpTransfer::ExistingFilePolicy::ResumeWhenPossible;
};

/** @brief 单个文件的传输结果，动作必须回传给调用方以便如实统计"传输"与"跳过"。 */
struct FileTransferResult {
    TransferOutcome outcome = TransferOutcome::Ok;
    SftpTransfer::TransferAction action = SftpTransfer::TransferAction::FullTransfer;
    QString reason;
};

/** @brief 远端条目的结构化 stat 结果；不存在不是错误。 */
struct RemoteStat {
    bool exists = false;
    quint64 size = 0;
    qint64 mtime = -1;
    bool isDirectory = false;
};

/** @brief 读取远端条目属性；路径不存在时返回 exists == false。 */
RemoteStat statRemote(SftpConnection *connection, const QString &path)
{
    RemoteStat result;
    const QByteArray encoded = path.toUtf8();
    if (sftp_attributes attributes = sftp_stat(connection->handle(), encoded.constData())) {
        result.exists = true;
        result.size = attributes->size;
        const quint64 modified = attributes->mtime64 != 0 ? attributes->mtime64 : attributes->mtime;
        // 0 与"未提供"无法区分，一律当作未知；未知的时间戳绝不能参与"内容一致"的判定。
        result.mtime = modified == 0 ? -1 : static_cast<qint64>(modified);
        result.isDirectory = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
        sftp_attributes_free(attributes);
    }
    return result;
}

/** @brief 本地文件的修改时间（Unix 秒）；无效时返回 -1。 */
qint64 localMtimeSeconds(const QFileInfo &info)
{
    const QDateTime modified = info.lastModified();
    return modified.isValid() ? modified.toSecsSinceEpoch() : -1;
}

/** @brief 本地残片路径：与正式文件同目录的隐藏文件，保证改名不跨文件系统。 */
QString localPartialPath(const QString &localPath)
{
    const QFileInfo info(localPath);
    return info.absolutePath() + QStringLiteral("/.") + info.fileName()
        + QString(SftpTransfer::kPartialSuffix);
}

/** @brief 计算本地文件的完整 SHA-256（十六进制）；读失败返回空。 */
QByteArray hashLocalFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return hash.result().toHex();
}

/**
 * @brief 把本地文件的一段内容喂进已有的哈希器。
 * @return 区间是否被完整读出；false 时哈希结果不可用。
 *
 * 续传时用来补齐"已经传过、这一轮不会再读"的前缀，使流式哈希仍能得到整份文件的摘要。
 */
bool appendFileRangeToHash(QCryptographicHash &hash,
                           const QString &path,
                           const quint64 offset,
                           const quint64 length)
{
    if (length == 0) {
        return true;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(static_cast<qint64>(offset))) {
        return false;
    }
    quint64 remaining = length;
    while (remaining > 0) {
        const QByteArray chunk = file.read(
            std::min<qint64>(kTransferChunkBytes, static_cast<qint64>(remaining)));
        if (chunk.isEmpty()) {
            return false;
        }
        hash.addData(chunk);
        remaining -= static_cast<quint64>(chunk.size());
    }
    return true;
}

/** @brief 计算本地文件某个区间的 SHA-256；区间不完整时返回空而不是部分结果。 */
QByteArray hashLocalRange(const QString &path, const quint64 offset, const quint64 length)
{
    if (length == 0) {
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(static_cast<qint64>(offset))) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    quint64 remaining = length;
    while (remaining > 0) {
        const QByteArray chunk = file.read(
            std::min<qint64>(kTransferChunkBytes, static_cast<qint64>(remaining)));
        if (chunk.isEmpty()) {
            return {};
        }
        hash.addData(chunk);
        remaining -= static_cast<quint64>(chunk.size());
    }
    return hash.result().toHex();
}

/** @brief 计算远端文件某个区间的 SHA-256；只用于校验残片尾部这类很小的窗口。 */
QByteArray hashRemoteRange(SftpConnection *connection,
                           const QString &path,
                           const quint64 offset,
                           const quint64 length)
{
    if (length == 0) {
        return {};
    }
    const QByteArray encoded = path.toUtf8();
    sftp_file file = sftp_open(connection->handle(), encoded.constData(), O_RDONLY, 0);
    if (file == nullptr) {
        return {};
    }
    sftp_seek64(file, offset);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(kTransferChunkBytes, Qt::Uninitialized);
    quint64 remaining = length;
    while (remaining > 0) {
        const ssize_t bytesRead = sftp_read(
            file, buffer.data(),
            std::min<quint64>(static_cast<quint64>(buffer.size()), remaining));
        if (bytesRead <= 0) {
            sftp_close(file);
            return {};
        }
        hash.addData(QByteArrayView(buffer.constData(), bytesRead));
        remaining -= static_cast<quint64>(bytesRead);
    }
    sftp_close(file);
    return hash.result().toHex();
}

/**
 * @brief 把源文件的修改时间写回远端目标，并回读确认它真的被保存了。
 * @return 服务端实际保存下来的时间戳；未能保存时返回 -1。
 *
 * 不能假设写入即生效：修改时间需要文件属主权限，而对象存储网关一类的 SFTP 实现会返回
 * 成功却不保存。只有回读一致才算数，否则下次"大小与修改时间一致"的判定就建立在假前提上。
 */
qint64 applyRemoteMtime(SftpConnection *connection, const QString &path, const qint64 mtimeSeconds)
{
    if (mtimeSeconds < 0) {
        return -1;
    }
    struct timeval times[2];
    times[0].tv_sec = static_cast<time_t>(mtimeSeconds);
    times[0].tv_usec = 0;
    times[1].tv_sec = static_cast<time_t>(mtimeSeconds);
    times[1].tv_usec = 0;
    const QByteArray encoded = path.toUtf8();
    if (sftp_utimes(connection->handle(), encoded.constData(), times) != SSH_OK) {
        return -1;
    }
    const RemoteStat verified = statRemote(connection, path);
    // SFTP v3 的时间戳只有秒精度，服务端文件系统还可能再取整（FAT 为 2 秒）。这里比较的是
    // 回读值本身，因此取整会被如实反映为"没存住"，而不是留下一个偶尔判错的模糊容差。
    if (!verified.exists || verified.mtime != mtimeSeconds) {
        return -1;
    }
    return verified.mtime;
}

/**
 * @brief 把残片原子改名成正式文件。
 *
 * SFTP v3 的 SSH_FXP_RENAME 在目标已存在时可能失败（OpenSSH 即如此），此时只能先删除再
 * 改名。这段窗口里正式文件短暂不存在，但相比让整次传输失败、或直接在正式文件上就地改写，
 * 它仍是最小的代价：就地改写会让读到一半的进程看到"半旧半新"的内容。
 */
bool renameRemoteOverwrite(SftpConnection *connection,
                           const QString &fromPath,
                           const QString &toPath,
                           QString *errorMessage)
{
    const QByteArray from = fromPath.toUtf8();
    const QByteArray to = toPath.toUtf8();
    if (sftp_rename(connection->handle(), from.constData(), to.constData()) == SSH_OK) {
        return true;
    }
    if (sftp_unlink(connection->handle(), to.constData()) == SSH_OK
        && sftp_rename(connection->handle(), from.constData(), to.constData()) == SSH_OK) {
        return true;
    }
    *errorMessage = QObject::tr("无法把临时文件改名为 %1：%2")
                        .arg(toPath, connection->lastError());
    return false;
}

/**
 * @brief 把一个本地文件写到远端路径。
 *
 * 递归上传与单文件上传共用同一实现：两条路径若各写一份，超时、部分写和取消这些边界条件
 * 迟早会只在其中一条上被修好。
 *
 * 字节一律先写到同目录的隐藏残片 `.名字.shellium-part`，全部写完并核对长度后才原子改名
 * 覆盖正式文件。因此正式文件在任何时刻要么是旧版要么是新版，绝不会停在"传了一半"的状态，
 * 也不会被下一次续传误当成可以接着写的残片。
 */
FileTransferResult writeRemoteFile(SftpConnection *connection,
                                   const QString &localPath,
                                   const QString &remotePath,
                                   const TransferContext &context,
                                   const ProgressReporter &onProgress,
                                   const CancelProbe &isCancelled,
                                   QString *errorMessage)
{
    QFile localFile(localPath);
    if (!localFile.open(QIODevice::ReadOnly)) {
        *errorMessage = QObject::tr("无法读取本地文件：%1").arg(localFile.errorString());
        return {TransferOutcome::Failed};
    }

    const QFileInfo localInfo(localPath);
    const quint64 localSize = static_cast<quint64>(std::max<qint64>(localFile.size(), 0));
    const qint64 localMtime = localMtimeSeconds(localInfo);
    const qint64 localMtimeMs = localInfo.lastModified().isValid()
        ? localInfo.lastModified().toMSecsSinceEpoch()
        : -1;
    const QString partPath = SftpTransfer::partialPathFor(remotePath);
    const QString ledgerKey = SftpResume::Ledger::makeKey(
        context.targetKey + QStringLiteral("|up"), remotePath);
    const QString trustKey = QStringLiteral("up|") + context.targetKey;
    const bool mtimeTrusted = context.ledger->mtimeTrusted(trustKey);

    SftpTransfer::SourceSnapshot source{localSize, localMtime, localMtimeMs, {}};
    SftpTransfer::DestinationSnapshot destination;
    SftpTransfer::PartialSnapshot partial;
    std::optional<SftpResume::LedgerRecord> record;

    if (context.policy == SftpTransfer::ExistingFilePolicy::ResumeWhenPossible) {
        const RemoteStat finalStat = statRemote(connection, remotePath);
        destination.exists = finalStat.exists && !finalStat.isDirectory;
        destination.size = finalStat.size;
        destination.mtime = finalStat.mtime;
        const RemoteStat partStat = statRemote(connection, partPath);
        partial.exists = partStat.exists && !partStat.isDirectory;
        partial.size = partStat.size;
        record = context.ledger->lookup(ledgerKey);

        // 存档里已经记着"这份内容的哈希"，本地文件的大小与毫秒级时间戳都没变时可以直接复用，
        // 省掉一次整文件读盘。
        const bool archiveMatchesSource = record.has_value()
            && record->kind == SftpResume::RecordKind::Complete
            && record->sourceSize == localSize
            && localMtimeMs >= 0 && record->sourceMtimeMs == localMtimeMs
            && !record->sourceHash.isEmpty();

        // 大小相同才有"可能不必重传"，这时优先向服务器要摘要：它只让服务端读一遍自己的盘，
        // 不占带宽，却能给出确凿结论。时间戳类证据挡不住"别人用等长内容覆盖、且修改时间恰好
        // 落在同一秒"，因此只要哈希拿得到就以哈希为准。
        if (destination.exists && destination.size == localSize
            && !(isCancelled && isCancelled())) {
            destination.contentHash = connection->remoteSha256(remotePath);
            if (!destination.contentHash.isEmpty()) {
                source.contentHash = archiveMatchesSource ? record->sourceHash
                                                          : hashLocalFile(localPath);
            }
        }

        // 残片凭据带哈希时，同样必须算出源哈希才能判断残片还算不算数。
        const bool needHashForResume = partial.exists && record.has_value()
            && record->kind == SftpResume::RecordKind::Partial
            && !record->sourceHash.isEmpty();
        if (source.contentHash.isEmpty() && needHashForResume
            && !(isCancelled && isCancelled())) {
            source.contentHash = hashLocalFile(localPath);
        }
    }

    const SftpTransfer::TransferDecision decision = SftpTransfer::decideTransfer(
        source, destination, partial, record, context.policy, mtimeTrusted);
    if (decision.action == SftpTransfer::TransferAction::SkipIdentical) {
        if (onProgress) {
            // 跳过的文件也要计入进度，否则整体百分比会永远停在不到 100% 的位置。
            onProgress(localSize);
        }
        return {TransferOutcome::Ok, decision.action, decision.reason};
    }

    quint64 offset = 0;
    QString effectiveReason = decision.reason;
    if (decision.action == SftpTransfer::TransferAction::Resume) {
        // 尾部窗口回读：源已被证明未变，剩下要防的是残片本身被写坏。半写与短写的损伤永远
        // 发生在尾部边界，校验这一小段就能挡掉绝大多数情况，代价只有很小的下行流量。
        const quint64 window = std::min<quint64>(SftpTransfer::kTailVerificationBytes,
                                                 decision.resumeOffset);
        const QByteArray localTail = hashLocalRange(localPath, decision.resumeOffset - window, window);
        const QByteArray remoteTail = hashRemoteRange(connection, partPath,
                                                      decision.resumeOffset - window, window);
        if (!localTail.isEmpty() && localTail == remoteTail) {
            offset = decision.resumeOffset;
        } else {
            effectiveReason = QObject::tr("残片尾部校验未通过，重新传输");
        }
    }

    if (offset == 0) {
        // 任何存疑情况都从零开始：残片和凭据一并清掉，绝不留下会被下次误信的半成品。
        const QByteArray encodedPart = partPath.toUtf8();
        sftp_unlink(connection->handle(), encodedPart.constData());
        context.ledger->discard(ledgerKey);
    } else if (onProgress) {
        onProgress(offset);
    }

    const QByteArray encodedPartPath = partPath.toUtf8();
    const int openFlags = O_WRONLY | O_CREAT | (offset > 0 ? 0 : O_TRUNC);
    sftp_file remoteFile = sftp_open(connection->handle(),
                                     encodedPartPath.constData(),
                                     openFlags,
                                     0644);
    if (remoteFile == nullptr) {
        *errorMessage = QObject::tr("无法创建远程临时文件 %1：%2")
                            .arg(partPath, connection->lastError());
        return {TransferOutcome::Failed};
    }
    if (offset > 0) {
        if (!localFile.seek(static_cast<qint64>(offset))) {
            sftp_close(remoteFile);
            *errorMessage = QObject::tr("无法定位本地续传偏移 %1：%2")
                                .arg(offset)
                                .arg(localFile.errorString());
            return {TransferOutcome::Failed};
        }
        // sftp_seek64 没有返回值；偏移已经由同一 SFTP 会话上的 stat 与尾部哈希双重验证，
        // 随后首个 write 若失败会进入统一错误路径，不能通过额外写入探测而破坏远端内容。
        sftp_seek64(remoteFile, offset);
    }

    struct InFlightWrite {
        sftp_aio aio = nullptr;
        size_t requestedLen = 0;
    };

    std::deque<InFlightWrite> inFlight;
    QByteArray carry;
    qsizetype carryOffset = 0;
    bool localEof = false;
    TransferOutcome outcome = TransferOutcome::Ok;
    quint64 confirmedBytes = offset;
    quint64 lastCheckpoint = offset;

    // 边传边算整份文件的摘要：这一轮无论如何都要把这些字节读一遍，顺手哈希不增加任何 I/O。
    // 传完就能把摘要写进存档，于是下一次同步只需向服务器要一次摘要即可判定，连本地都不用再读。
    QCryptographicHash streamHash(QCryptographicHash::Sha256);
    bool streamHashValid = offset == 0
        || appendFileRangeToHash(streamHash, localPath, 0, offset);

    // 只把"服务端已确认"的字节记入检查点：在途请求随时可能失败，把它们算进去会让下次从一个
    // 并不存在的位置续写。宁可记少，也绝不记多。
    const auto checkpoint = [&]() {
        SftpResume::LedgerRecord pending;
        pending.kind = SftpResume::RecordKind::Partial;
        pending.sourcePath = localPath;
        pending.sourceSize = localSize;
        pending.sourceMtime = localMtime;
        pending.sourceMtimeMs = localMtimeMs;
        pending.bytesWritten = confirmedBytes;
        context.ledger->store(ledgerKey, pending);
        lastCheckpoint = confirmedBytes;
    };

    const auto drainOne = [&]() -> bool {
        InFlightWrite front = inFlight.front();
        inFlight.pop_front();
        const ssize_t written = sftp_aio_wait_write(&front.aio);
        if (written < 0 || static_cast<size_t>(written) != front.requestedLen) {
            *errorMessage = QObject::tr("上传 %1 失败：%2")
                                .arg(remotePath, connection->lastError());
            return false;
        }
        confirmedBytes += static_cast<quint64>(written);
        if (confirmedBytes - lastCheckpoint >= kLedgerCheckpointBytes) {
            checkpoint();
        }
        if (onProgress) {
            onProgress(static_cast<quint64>(written));
        }
        return true;
    };

    while (outcome == TransferOutcome::Ok) {
        if (isCancelled && isCancelled()) {
            outcome = TransferOutcome::Cancelled;
            break;
        }

        if (carryOffset >= carry.size() && !localEof) {
            carry = localFile.read(kTransferChunkBytes);
            carryOffset = 0;
            if (carry.isEmpty()) {
                localEof = true;
            } else if (streamHashValid) {
                streamHash.addData(carry);
            }
        }

        const bool haveData = carryOffset < carry.size();
        if (haveData && static_cast<int>(inFlight.size()) < kUploadPipelineDepth) {
            sftp_aio aio = nullptr;
            const ssize_t requested = sftp_aio_begin_write(remoteFile,
                                                            carry.constData() + carryOffset,
                                                            static_cast<size_t>(carry.size() - carryOffset),
                                                            &aio);
            if (requested < 0) {
                *errorMessage = QObject::tr("上传 %1 失败：%2")
                                    .arg(remotePath, connection->lastError());
                outcome = TransferOutcome::Failed;
                break;
            }
            carryOffset += requested;
            inFlight.push_back({aio, static_cast<size_t>(requested)});
            continue;
        }

        if (!inFlight.empty()) {
            if (!drainOne()) {
                outcome = TransferOutcome::Failed;
                break;
            }
            continue;
        }

        if (!haveData) {
            break;
        }
    }

    // 未确认的请求必须逐一 wait 掉（即便传输已失败/取消），否则 aio 句柄内存泄漏。
    while (!inFlight.empty()) {
        InFlightWrite front = inFlight.front();
        inFlight.pop_front();
        sftp_aio_wait_write(&front.aio);
    }
    sftp_close(remoteFile);
    localFile.close();

    if (outcome != TransferOutcome::Ok) {
        // 中断时才计算源哈希：正常传输不必为此多读一遍本地文件，而一旦要留下残片，就必须
        // 留下能证明"这段残片属于哪个源"的最强凭据。
        SftpResume::LedgerRecord pending;
        pending.kind = SftpResume::RecordKind::Partial;
        pending.sourcePath = localPath;
        pending.sourceSize = localSize;
        pending.sourceMtime = localMtime;
        pending.sourceMtimeMs = localMtimeMs;
        pending.sourceHash = hashLocalFile(localPath);
        pending.bytesWritten = confirmedBytes;
        context.ledger->store(ledgerKey, pending);
        return {outcome, decision.action, effectiveReason};
    }

    // 传输期间源被改过，残片就成了两个版本的混合体，只能整体作废。
    const QFileInfo afterInfo(localPath);
    if (static_cast<quint64>(std::max<qint64>(afterInfo.size(), 0)) != localSize
        || localMtimeSeconds(afterInfo) != localMtime) {
        const QByteArray encoded = partPath.toUtf8();
        sftp_unlink(connection->handle(), encoded.constData());
        context.ledger->discard(ledgerKey);
        *errorMessage = QObject::tr("本地文件 %1 在传输过程中被修改，已放弃本次结果")
                            .arg(localPath);
        return {TransferOutcome::Failed};
    }

    const RemoteStat writtenStat = statRemote(connection, partPath);
    if (!writtenStat.exists || writtenStat.size != localSize) {
        *errorMessage = QObject::tr("远端临时文件长度与本地不一致（%1 / %2 字节），已放弃改名")
                            .arg(writtenStat.size)
                            .arg(localSize);
        return {TransferOutcome::Failed};
    }
    if (!renameRemoteOverwrite(connection, partPath, remotePath, errorMessage)) {
        return {TransferOutcome::Failed};
    }

    // 写回并回读确认修改时间。存住了就留下一条"我们把这一版写到了这里"的存档，下次同步据此
    // 跳过未改动的文件；存不住就如实记为不可信，此后一律全量重传，而不是拿假前提判"一致"。
    const qint64 storedMtime = applyRemoteMtime(connection, remotePath, localMtime);
    context.ledger->setMtimeTrusted(trustKey, storedMtime >= 0);
    if (storedMtime >= 0 && localMtimeMs >= 0) {
        SftpResume::LedgerRecord archive;
        archive.kind = SftpResume::RecordKind::Complete;
        archive.sourcePath = localPath;
        archive.sourceSize = localSize;
        archive.sourceMtime = localMtime;
        archive.sourceMtimeMs = localMtimeMs;
        archive.sourceHash = streamHashValid ? streamHash.result().toHex() : source.contentHash;
        archive.destinationSize = localSize;
        archive.destinationMtime = storedMtime;
        context.ledger->store(ledgerKey, archive);
    } else {
        context.ledger->discard(ledgerKey);
    }
    return {TransferOutcome::Ok, decision.action, effectiveReason};
}

/**
 * @brief 把一个远端文件读到本地路径。
 *
 * 与上传方向完全对称：字节先落到本地隐藏残片，核对长度后再原子改名覆盖正式文件；中断时
 * 保留残片与凭据，下次续传前逐项复核源是否变化、残片是否仍是那段前缀。
 *
 * 下载方向唯一的能力差别是源在远端：只有当服务器提供 sha256sum/shasum 时才能拿到源的完整
 * 哈希；拿不到时退到"大小 + 修改时间"判据，而不是把整份文件拉回来算哈希——那和重传一次
 * 一样贵，校验也就失去了意义。
 */
FileTransferResult readRemoteFile(SftpConnection *connection,
                                  const QString &remotePath,
                                  const QString &localPath,
                                  const TransferContext &context,
                                  const ProgressReporter &onProgress,
                                  const CancelProbe &isCancelled,
                                  QString *errorMessage,
                                  quint64 *remoteSize = nullptr)
{
    const RemoteStat sourceStat = statRemote(connection, remotePath);
    if (!sourceStat.exists) {
        *errorMessage = QObject::tr("无法读取远程文件 %1：%2")
                            .arg(remotePath, connection->lastError());
        return {TransferOutcome::Failed};
    }
    if (remoteSize != nullptr) {
        *remoteSize = sourceStat.size;
    }

    const QString partPath = localPartialPath(localPath);
    const QString ledgerKey = SftpResume::Ledger::makeKey(
        context.targetKey + QStringLiteral("|down"), localPath);
    // 修改时间能否保存取决于**本地**落盘位置的文件系统（下到 FAT/exFAT 上会被取整），
    // 因此可信度按目标目录记录，而不是按远程服务器记录。
    const QString trustKey = QStringLiteral("down|") + QFileInfo(localPath).absolutePath();
    const bool mtimeTrusted = context.ledger->mtimeTrusted(trustKey);

    // 远端源只有秒级时间戳，毫秒位无从得知；填成整秒即可，它只与账本里同样来自远端的记录
    // 比较，不会与本地毫秒值混用。
    SftpTransfer::SourceSnapshot source{sourceStat.size, sourceStat.mtime,
                                        sourceStat.mtime < 0 ? -1 : sourceStat.mtime * 1000,
                                        {}};
    SftpTransfer::DestinationSnapshot destination;
    SftpTransfer::PartialSnapshot partial;
    std::optional<SftpResume::LedgerRecord> record;

    if (context.policy == SftpTransfer::ExistingFilePolicy::ResumeWhenPossible) {
        const QFileInfo destinationInfo(localPath);
        destination.exists = destinationInfo.exists() && destinationInfo.isFile();
        destination.size = destination.exists
            ? static_cast<quint64>(std::max<qint64>(destinationInfo.size(), 0))
            : 0;
        destination.mtime = destination.exists ? localMtimeSeconds(destinationInfo) : -1;
        const QFileInfo partialInfo(partPath);
        partial.exists = partialInfo.exists() && partialInfo.isFile();
        partial.size = partial.exists
            ? static_cast<quint64>(std::max<qint64>(partialInfo.size(), 0))
            : 0;
        record = context.ledger->lookup(ledgerKey);

        // 本地目标文件自上次同步以来没被动过时，它的内容哈希就是存档里那一条，无需再读一遍盘。
        const bool archiveMatchesDestination = record.has_value()
            && record->kind == SftpResume::RecordKind::Complete
            && !record->sourceHash.isEmpty()
            && record->destinationSize == destination.size
            && record->destinationMtime >= 0
            && record->destinationMtime == destination.mtime;

        // 与上传方向同理：大小相同才有跳过的余地，此时优先取远端摘要作为裁决依据。
        if (destination.exists && destination.size == source.size
            && !(isCancelled && isCancelled())) {
            source.contentHash = connection->remoteSha256(remotePath);
            if (!source.contentHash.isEmpty()) {
                destination.contentHash = archiveMatchesDestination ? record->sourceHash
                                                                    : hashLocalFile(localPath);
            }
        }

        const bool needHashForResume = partial.exists && record.has_value()
            && record->kind == SftpResume::RecordKind::Partial
            && !record->sourceHash.isEmpty();
        if (source.contentHash.isEmpty() && needHashForResume
            && !(isCancelled && isCancelled())) {
            source.contentHash = connection->remoteSha256(remotePath);
        }
    }

    const SftpTransfer::TransferDecision decision = SftpTransfer::decideTransfer(
        source, destination, partial, record, context.policy, mtimeTrusted);
    if (decision.action == SftpTransfer::TransferAction::SkipIdentical) {
        if (onProgress) {
            onProgress(source.size);
        }
        return {TransferOutcome::Ok, decision.action, decision.reason};
    }

    quint64 offset = 0;
    QString effectiveReason = decision.reason;
    if (decision.action == SftpTransfer::TransferAction::Resume) {
        const quint64 window = std::min<quint64>(SftpTransfer::kTailVerificationBytes,
                                                 decision.resumeOffset);
        const QByteArray localTail = hashLocalRange(partPath, decision.resumeOffset - window, window);
        const QByteArray remoteTail = hashRemoteRange(connection, remotePath,
                                                      decision.resumeOffset - window, window);
        if (!localTail.isEmpty() && localTail == remoteTail) {
            offset = decision.resumeOffset;
        } else {
            effectiveReason = QObject::tr("残片尾部校验未通过，重新传输");
        }
    }

    if (offset == 0) {
        QFile::remove(partPath);
        context.ledger->discard(ledgerKey);
    } else if (onProgress) {
        onProgress(offset);
    }

    const QByteArray encodedRemotePath = remotePath.toUtf8();
    sftp_file remoteFile = sftp_open(connection->handle(),
                                     encodedRemotePath.constData(),
                                     O_RDONLY,
                                     0);
    if (remoteFile == nullptr) {
        *errorMessage = QObject::tr("无法读取远程文件 %1：%2")
                            .arg(remotePath, connection->lastError());
        return {TransferOutcome::Failed};
    }

    QFile partialFile(partPath);
    const QIODevice::OpenMode localMode = offset > 0
        ? QIODevice::ReadWrite
        : QIODevice::WriteOnly | QIODevice::Truncate;
    if (!partialFile.open(localMode)) {
        sftp_close(remoteFile);
        *errorMessage = QObject::tr("无法写入本地临时文件 %1：%2")
                            .arg(partPath, partialFile.errorString());
        return {TransferOutcome::Failed};
    }
    if (offset > 0) {
        if (!partialFile.seek(static_cast<qint64>(offset))) {
            sftp_close(remoteFile);
            *errorMessage = QObject::tr("无法定位本地续传偏移 %1：%2")
                                .arg(offset)
                                .arg(partialFile.errorString());
            return {TransferOutcome::Failed};
        }
        sftp_seek64(remoteFile, offset);
    }

    QByteArray buffer(kTransferChunkBytes, Qt::Uninitialized);
    TransferOutcome outcome = TransferOutcome::Ok;
    quint64 confirmedBytes = offset;
    quint64 lastCheckpoint = offset;

    // 与上传方向同理：这些字节反正要过一遍内存，顺手算出整份内容的摘要供下次同步判定。
    QCryptographicHash streamHash(QCryptographicHash::Sha256);
    bool streamHashValid = offset == 0
        || appendFileRangeToHash(streamHash, partPath, 0, offset);
    while (true) {
        if (isCancelled && isCancelled()) {
            outcome = TransferOutcome::Cancelled;
            break;
        }
        const ssize_t bytesRead = sftp_read(remoteFile, buffer.data(), buffer.size());
        if (bytesRead == 0) {
            break;
        }
        if (bytesRead < 0) {
            *errorMessage = QObject::tr("下载 %1 失败：%2")
                                .arg(remotePath, connection->lastError());
            outcome = TransferOutcome::Failed;
            break;
        }
        if (partialFile.write(buffer.constData(), bytesRead) != bytesRead) {
            *errorMessage = QObject::tr("保存 %1 失败：%2")
                                .arg(partPath, partialFile.errorString());
            outcome = TransferOutcome::Failed;
            break;
        }
        if (streamHashValid) {
            streamHash.addData(QByteArrayView(buffer.constData(), bytesRead));
        }
        confirmedBytes += static_cast<quint64>(bytesRead);
        if (confirmedBytes - lastCheckpoint >= kLedgerCheckpointBytes) {
            // 检查点前必须先落盘：账本一旦记下这个字节数，续传就会信任残片里的对应长度。
            partialFile.flush();
            SftpResume::LedgerRecord pending;
            pending.kind = SftpResume::RecordKind::Partial;
            pending.sourcePath = remotePath;
            pending.sourceSize = source.size;
            pending.sourceMtime = source.mtime;
            pending.sourceMtimeMs = source.mtimeMs;
            pending.sourceHash = source.contentHash;
            pending.bytesWritten = confirmedBytes;
            context.ledger->store(ledgerKey, pending);
            lastCheckpoint = confirmedBytes;
        }
        if (onProgress) {
            onProgress(static_cast<quint64>(bytesRead));
        }
    }
    sftp_close(remoteFile);
    if (!partialFile.flush() && outcome == TransferOutcome::Ok) {
        *errorMessage = QObject::tr("刷新本地文件 %1 失败：%2")
                            .arg(partPath, partialFile.errorString());
        outcome = TransferOutcome::Failed;
    }
    partialFile.close();

    if (outcome != TransferOutcome::Ok) {
        SftpResume::LedgerRecord pending;
        pending.kind = SftpResume::RecordKind::Partial;
        pending.sourcePath = remotePath;
        pending.sourceSize = source.size;
        pending.sourceMtime = source.mtime;
        pending.sourceMtimeMs = source.mtimeMs;
        // 远端有摘要命令时补齐哈希凭据；没有就留空，续传判定会自动退到大小 + 修改时间。
        pending.sourceHash = source.contentHash.isEmpty()
            ? connection->remoteSha256(remotePath)
            : source.contentHash;
        pending.bytesWritten = confirmedBytes;
        context.ledger->store(ledgerKey, pending);
        return {outcome, decision.action, effectiveReason};
    }

    const RemoteStat afterStat = statRemote(connection, remotePath);
    if (!afterStat.exists || afterStat.size != source.size || afterStat.mtime != source.mtime) {
        QFile::remove(partPath);
        context.ledger->discard(ledgerKey);
        *errorMessage = QObject::tr("远程文件 %1 在传输过程中被修改，已放弃本次结果")
                            .arg(remotePath);
        return {TransferOutcome::Failed};
    }

    const QFileInfo writtenInfo(partPath);
    if (static_cast<quint64>(std::max<qint64>(writtenInfo.size(), 0)) != source.size) {
        *errorMessage = QObject::tr("本地临时文件长度与远端不一致（%1 / %2 字节），已放弃改名")
                            .arg(writtenInfo.size())
                            .arg(source.size);
        return {TransferOutcome::Failed};
    }

    // POSIX rename 在目标已存在时原子替换，因此正式文件不会出现"先删后建"的空窗；
    // QFile::rename 在目标存在时直接失败，无法提供这一语义。
    if (::rename(QFile::encodeName(partPath).constData(),
                 QFile::encodeName(localPath).constData()) != 0) {
        *errorMessage = QObject::tr("无法把临时文件改名为 %1：%2")
                            .arg(localPath, QString::fromLocal8Bit(strerror(errno)));
        return {TransferOutcome::Failed};
    }
    qint64 storedMtime = -1;
    if (source.mtime >= 0) {
        QFile destinationFile(localPath);
        if (destinationFile.open(QIODevice::ReadWrite)) {
            destinationFile.setFileTime(QDateTime::fromSecsSinceEpoch(source.mtime),
                                        QFileDevice::FileModificationTime);
            destinationFile.close();
        }
        // 同样必须回读确认：目标文件系统（如 FAT）可能把秒数取整，取整后的时间戳再拿去比
        // "是否一致"就会永远不等，那还不如老实记为不可信。
        if (localMtimeSeconds(QFileInfo(localPath)) == source.mtime) {
            storedMtime = source.mtime;
        }
    }
    context.ledger->setMtimeTrusted(trustKey, storedMtime >= 0);
    if (storedMtime >= 0) {
        SftpResume::LedgerRecord archive;
        archive.kind = SftpResume::RecordKind::Complete;
        archive.sourcePath = remotePath;
        archive.sourceSize = source.size;
        archive.sourceMtime = source.mtime;
        archive.sourceMtimeMs = source.mtimeMs;
        archive.sourceHash = streamHashValid ? streamHash.result().toHex() : source.contentHash;
        archive.destinationSize = source.size;
        archive.destinationMtime = storedMtime;
        context.ledger->store(ledgerKey, archive);
    } else {
        context.ledger->discard(ledgerKey);
    }
    return {TransferOutcome::Ok, decision.action, effectiveReason};
}

/**
 * @brief 在远端创建目录；目录已存在不视为错误。
 *
 * 递归上传必然会碰到部分目录已存在（续传、目标端已有同名结构）。把 FILE_ALREADY_EXISTS
 * 当失败会让整棵树都传不动，而这恰恰是最常见的正常情况。
 */
bool ensureRemoteDirectory(SftpConnection *connection,
                           const QString &remotePath,
                           QString *errorMessage)
{
    const QByteArray encoded = remotePath.toUtf8();
    if (sftp_mkdir(connection->handle(), encoded.constData(), 0755) == SSH_OK) {
        return true;
    }
    if (sftp_get_error(connection->handle()) == SSH_FX_FILE_ALREADY_EXISTS) {
        return true;
    }
    // 有些服务器对已存在目录返回 PERMISSION_DENIED 或 FAILURE，需要再 stat 一次才能确定。
    if (sftp_attributes attributes = sftp_stat(connection->handle(), encoded.constData())) {
        const bool isDirectory = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
        sftp_attributes_free(attributes);
        if (isDirectory) {
            return true;
        }
    }
    *errorMessage = QObject::tr("无法创建远程目录 %1：%2")
                        .arg(remotePath, connection->lastError());
    return false;
}

/**
 * @brief 广度优先遍历远端目录树，收集相对根目录的条目。
 *
 * 刻意用显式队列而不是函数递归：远端目录深度由服务器决定，深层结构不应该把客户端的调用栈
 * 打爆。符号链接只记录不展开，因此环形链接不会导致无限遍历。
 */
QList<SftpRecursive::SourceEntry> collectRemoteTree(SftpConnection *connection,
                                                    const QString &remoteRoot,
                                                    const CancelProbe &isCancelled,
                                                    QList<SftpRecursive::FailedEntry> *failures,
                                                    bool *cancelled)
{
    QList<SftpRecursive::SourceEntry> collected;
    QStringList pending{QString{}};

    while (!pending.isEmpty()) {
        if (isCancelled && isCancelled()) {
            *cancelled = true;
            break;
        }
        const QString relativeDirectory = pending.takeFirst();
        const QString absoluteDirectory = joinRemotePath(remoteRoot, relativeDirectory);
        const QByteArray encoded = absoluteDirectory.toUtf8();
        sftp_dir directory = sftp_opendir(connection->handle(), encoded.constData());
        if (directory == nullptr) {
            // 单个子目录不可读（常见于权限不足）不应中断整棵树的传输，记录后继续。
            failures->append({absoluteDirectory,
                              QObject::tr("无法打开远程目录：%1").arg(connection->lastError())});
            continue;
        }

        while (sftp_attributes attributes = sftp_readdir(connection->handle(), directory)) {
            const QString name = QString::fromUtf8(
                attributes->name == nullptr ? "" : attributes->name);
            const bool isDirectory = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
            const bool isSymbolicLink = attributes->type == SSH_FILEXFER_TYPE_SYMLINK;
            const quint64 size = attributes->size;
            sftp_attributes_free(attributes);

            if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")) {
                continue;
            }
            if (name.contains(QLatin1Char('/'))) {
                // 合法 SFTP 服务器不会在单个条目名里返回分隔符；出现即视为恶意，直接丢弃。
                failures->append({joinRemotePath(absoluteDirectory, name),
                                  QObject::tr("远端返回了包含路径分隔符的条目名，已忽略")});
                continue;
            }

            SftpRecursive::SourceEntry entry;
            entry.relativePath = joinRemotePath(relativeDirectory, name);
            entry.size = size;
            entry.isDirectory = isDirectory;
            entry.isSymbolicLink = isSymbolicLink;
            collected.append(entry);

            // 符号链接即使指向目录也不展开：跟随会在环形链接上无限递归，也可能把下载引到
            // 用户选定目录之外的位置。
            if (isDirectory && !isSymbolicLink) {
                pending.append(entry.relativePath);
            }
        }
        sftp_closedir(directory);
    }
    return collected;
}

/**
 * @brief 递归删除一棵远端目录树。
 * @param removedCount 累计删除的条目数，用于如实汇报"替换"到底动了多少东西。
 * @return 是否完整删除；任一条目删不掉都返回 false，并把原因写入 errorMessage。
 *
 * 只在用户显式选择"替换"时调用。删除顺序必须自底向上：SFTP 的 rmdir 不会递归，父目录在
 * 内容清空前删不掉。符号链接按条目本身删除，绝不跟随——跟随会把链接指向的目录一起删掉，
 * 那是灾难性的越权删除。
 */
bool removeRemoteTree(SftpConnection *connection,
                      const QString &remoteRoot,
                      const CancelProbe &isCancelled,
                      quint64 *removedCount,
                      QString *errorMessage)
{
    QList<SftpRecursive::FailedEntry> scanFailures;
    bool cancelled = false;
    const QList<SftpRecursive::SourceEntry> entries = collectRemoteTree(
        connection, remoteRoot, isCancelled, &scanFailures, &cancelled);
    if (cancelled) {
        *errorMessage = QObject::tr("替换前的扫描被取消");
        return false;
    }
    if (!scanFailures.isEmpty()) {
        // 有读不到的子目录就意味着删不干净，此时继续删会得到一个"半替换"的目录树，比直接
        // 拒绝更糟：用户会以为目标端已经和源端一致。
        *errorMessage = QObject::tr("无法完整扫描待替换目录：%1")
                            .arg(scanFailures.constFirst().message);
        return false;
    }

    // 深的路径排在前面，保证目录在其内容之后才被删除。
    QList<SftpRecursive::SourceEntry> ordered = entries;
    std::sort(ordered.begin(), ordered.end(),
              [](const SftpRecursive::SourceEntry &left,
                 const SftpRecursive::SourceEntry &right) {
                  const int leftDepth = left.relativePath.count(QLatin1Char('/'));
                  const int rightDepth = right.relativePath.count(QLatin1Char('/'));
                  if (leftDepth != rightDepth) {
                      return leftDepth > rightDepth;
                  }
                  return left.relativePath > right.relativePath;
              });

    for (const SftpRecursive::SourceEntry &entry : std::as_const(ordered)) {
        if (isCancelled && isCancelled()) {
            *errorMessage = QObject::tr("替换过程被取消");
            return false;
        }
        const QString absolute = joinRemotePath(remoteRoot, entry.relativePath);
        const QByteArray encoded = absolute.toUtf8();
        const bool isRealDirectory = entry.isDirectory && !entry.isSymbolicLink;
        const int removed = isRealDirectory
            ? sftp_rmdir(connection->handle(), encoded.constData())
            : sftp_unlink(connection->handle(), encoded.constData());
        if (removed != SSH_OK) {
            *errorMessage = QObject::tr("无法删除 %1：%2")
                                .arg(absolute, connection->lastError());
            return false;
        }
        ++(*removedCount);
    }
    return true;
}

/** @brief 遍历本地目录树，收集相对根目录的条目；符号链接只记录不展开。 */
QList<SftpRecursive::SourceEntry> collectLocalTree(const QString &localRoot)
{
    QList<SftpRecursive::SourceEntry> collected;
    const QDir rootDirectory(localRoot);
    // NoIteratorFlags 表示不跟随符号链接进入子树；隐藏文件必须包含，否则 .git、.env 之类
    // 会被静默漏传，而用户以为整个目录都上传了。
    QDirIterator iterator(localRoot,
                          QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        SftpRecursive::SourceEntry entry;
        entry.relativePath = rootDirectory.relativeFilePath(info.absoluteFilePath());
        entry.isSymbolicLink = info.isSymLink();
        entry.isDirectory = !entry.isSymbolicLink && info.isDir();
        entry.size = entry.isDirectory ? 0 : static_cast<quint64>(info.size());
        collected.append(entry);
    }
    return collected;
}

} // namespace

SftpService::SftpService(QObject *parent)
    : QObject(parent)
    , m_pool(std::make_shared<SftpConnectionPool>())
    , m_ledger(std::make_shared<SftpResume::Ledger>())
{
    qRegisterMetaType<ConnectionTarget>();
    qRegisterMetaType<QList<RemoteFileInfo>>();
    qRegisterMetaType<SftpService::ErrorKind>();
    qRegisterMetaType<SftpRecursive::TransferSummary>();
}

SftpService::~SftpService()
{
    // 仍在执行的后台任务各自持有 shared_ptr，池会在最后一个任务结束后才真正释放。
    m_pool->clear();
}

quint64 SftpService::listDirectory(const ConnectionTarget &target, const QString &path)
{
    const quint64 requestId = nextRequestId();
    const bool allowUnknown = takeTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);

    (void)QtConcurrent::run([service, pool = m_pool, requestId, target, path, allowUnknown]() {
        const QString poolKey = browsePoolKey(target);
        const QSharedPointer<PooledConnection> entry = pool->acquire(poolKey);
        QMutexLocker locker(&entry->mutex);
        SftpConnection *connection = ensureConnected(
            entry.data(), poolKey, target, allowUnknown, service, requestId, pool.get());
        if (connection == nullptr) {
            return;
        }

        // 空路径表示"该账号的默认主目录"：让服务端把 "." 规范化成绝对路径。首次进入一台
        // 服务器时无从得知它的主目录，写死 / 既不符合直觉，也常常没有列目录权限。
        QString resolvedPath = path;
        if (resolvedPath.isEmpty()) {
            char *home = sftp_canonicalize_path(connection->handle(), ".");
            if (home != nullptr) {
                resolvedPath = QString::fromUtf8(home);
                ssh_string_free_char(home);
            } else {
                resolvedPath = QStringLiteral("/");
            }
        }

        const QByteArray encodedPath = resolvedPath.toUtf8();
        sftp_dir directory = sftp_opendir(connection->handle(), encodedPath.constData());
        if (directory == nullptr) {
            const ConnectResult error{false,
                                      ErrorKind::RemoteOperation,
                                      QObject::tr("无法打开远程目录 %1：%2")
                                          .arg(resolvedPath, connection->lastError())};
            deliverConnectionFailure(service, requestId, target, error);
            return;
        }

        QList<RemoteFileInfo> entries;
        while (sftp_attributes attributes = sftp_readdir(connection->handle(), directory)) {
            const QString name = QString::fromUtf8(attributes->name == nullptr
                                                       ? ""
                                                       : attributes->name);
            if (name != QStringLiteral(".") && name != QStringLiteral("..")) {
                RemoteFileInfo entry;
                entry.name = name;
                entry.path = joinRemotePath(resolvedPath, name);
                entry.size = attributes->size;
                entry.permissions = attributes->permissions;
                entry.owner = attributes->owner == nullptr
                    ? QString::number(attributes->uid)
                    : QString::fromUtf8(attributes->owner);
                entry.group = attributes->group == nullptr
                    ? QString::number(attributes->gid)
                    : QString::fromUtf8(attributes->group);
                const quint64 modified = attributes->mtime64 != 0
                    ? attributes->mtime64
                    : attributes->mtime;
                entry.modifiedAt = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(modified));
                entry.isDirectory = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
                entry.isSymbolicLink = attributes->type == SSH_FILEXFER_TYPE_SYMLINK;
                entries.append(entry);
            }
            sftp_attributes_free(attributes);
        }
        const bool directoryReadFailed = sftp_dir_eof(directory) == 0;
        sftp_closedir(directory);

        if (directoryReadFailed) {
            const ConnectResult error{false, ErrorKind::RemoteOperation,
                                      QObject::tr("读取远程目录 %1 时连接中断：%2")
                                          .arg(resolvedPath, connection->lastError())};
            deliverConnectionFailure(service, requestId, target, error);
            return;
        }

        std::sort(entries.begin(), entries.end(), [](const RemoteFileInfo &left,
                                                     const RemoteFileInfo &right) {
            if (left.isDirectory != right.isDirectory) {
                return left.isDirectory;
            }
            return QString::localeAwareCompare(left.name, right.name) < 0;
        });

        if (service != nullptr) {
            QMetaObject::invokeMethod(service, [service, requestId, target, resolvedPath, entries]() {
                if (service != nullptr) {
                    emit service->directoryListed(requestId, target, resolvedPath, entries);
                }
            }, Qt::QueuedConnection);
        }
    });
    return requestId;
}

void SftpService::setResumeLedgerPath(const QString &path)
{
    // 直接换掉整个账本对象：已经在跑的后台任务各自持有旧账本的 shared_ptr，会把结果写完
    // 再释放，因此切换不会让进行中的传输失去凭据。
    m_ledger = std::make_shared<SftpResume::Ledger>(path);
}

quint64 SftpService::uploadFile(const ConnectionTarget &target,
                                const QString &localPath,
                                const QString &remotePath,
                                const SftpTransfer::ExistingFilePolicy policy)
{
    const quint64 requestId = nextRequestId();
    const bool allowUnknown = takeTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);

    (void)QtConcurrent::run([service, pool = m_pool, ledger = m_ledger, requestId, target,
                             localPath, remotePath, allowUnknown, policy]() {
        const quint64 total = static_cast<quint64>(QFileInfo(localPath).size());
        const QString poolKey = transferPoolKey(target);
        const QSharedPointer<PooledConnection> entry = pool->acquire(poolKey);
        QMutexLocker locker(&entry->mutex);
        SftpConnection *connection = ensureConnected(
            entry.data(), poolKey, target, allowUnknown, service, requestId, pool.get());
        if (connection == nullptr) {
            return;
        }

        quint64 completed = 0;
        QString errorMessage;
        const FileTransferResult result = writeRemoteFile(
            connection,
            localPath,
            remotePath,
            TransferContext{ledger.get(), target.stableKey(), policy},
            [&completed, service, requestId, total](const quint64 delta) {
                completed += delta;
                postTransferProgress(service, requestId, completed, total);
            },
            [pool, requestId]() { return pool->waitUntilRunnable(requestId); },
            &errorMessage);
        pool->clearCancel(requestId);
        // 账本按时间节流写盘，请求收尾必须兜底 flush：否则最后几条记录会随进程退出丢失，
        // 下次同步又要把这些文件白传一遍。
        ledger->flush();

        if (result.outcome != TransferOutcome::Ok) {
            const ConnectResult error{false,
                                      result.outcome == TransferOutcome::Cancelled
                                          ? ErrorKind::LocalIo
                                          : ErrorKind::RemoteOperation,
                                      result.outcome == TransferOutcome::Cancelled
                                          ? QObject::tr("上传 %1 已取消").arg(remotePath)
                                          : errorMessage};
            deliverConnectionFailure(service, requestId, target, error);
            return;
        }
        if (service != nullptr) {
            // 跳过必须说成跳过。把"内容一致、未重传"讲成"已上传"，正是用户以为文件更新了、
            // 实际上远端一个字节都没变的根源。
            const QString message = result.action == SftpTransfer::TransferAction::SkipIdentical
                ? QObject::tr("%1 内容一致（%2），未重传").arg(remotePath, result.reason)
                : QObject::tr("已上传到 %1").arg(remotePath);
            QMetaObject::invokeMethod(service, [service, requestId, message]() {
                if (service != nullptr) {
                    emit service->operationSucceeded(requestId, message);
                }
            }, Qt::QueuedConnection);
        }
    });
    return requestId;
}

quint64 SftpService::downloadFile(const ConnectionTarget &target,
                                  const QString &remotePath,
                                  const QString &localPath,
                                  const SftpTransfer::ExistingFilePolicy policy)
{
    const quint64 requestId = nextRequestId();
    const bool allowUnknown = takeTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);

    (void)QtConcurrent::run([service, pool = m_pool, ledger = m_ledger, requestId, target,
                             remotePath, localPath, allowUnknown, policy]() {
        const QString poolKey = transferPoolKey(target);
        const QSharedPointer<PooledConnection> entry = pool->acquire(poolKey);
        QMutexLocker locker(&entry->mutex);
        SftpConnection *connection = ensureConnected(
            entry.data(), poolKey, target, allowUnknown, service, requestId, pool.get());
        if (connection == nullptr) {
            return;
        }

        quint64 total = 0;
        quint64 completed = 0;
        QString errorMessage;
        const FileTransferResult result = readRemoteFile(
            connection,
            remotePath,
            localPath,
            TransferContext{ledger.get(), target.stableKey(), policy},
            [&completed, &total, service, requestId](const quint64 delta) {
                completed += delta;
                postTransferProgress(service, requestId, completed, total);
            },
            [pool, requestId]() { return pool->waitUntilRunnable(requestId); },
            &errorMessage,
            &total);
        pool->clearCancel(requestId);
        // 账本按时间节流写盘，请求收尾必须兜底 flush：否则最后几条记录会随进程退出丢失，
        // 下次同步又要把这些文件白传一遍。
        ledger->flush();

        if (result.outcome != TransferOutcome::Ok) {
            const ConnectResult error{false, ErrorKind::LocalIo,
                                      result.outcome == TransferOutcome::Cancelled
                                          ? QObject::tr("下载 %1 已取消").arg(remotePath)
                                          : errorMessage};
            deliverConnectionFailure(service, requestId, target, error);
            return;
        }
        if (service != nullptr) {
            const QString message = result.action == SftpTransfer::TransferAction::SkipIdentical
                ? QObject::tr("%1 内容一致（%2），未重新下载").arg(localPath, result.reason)
                : QObject::tr("已下载到 %1").arg(localPath);
            QMetaObject::invokeMethod(service, [service, requestId, message]() {
                if (service != nullptr) {
                    emit service->operationSucceeded(requestId, message);
                }
            }, Qt::QueuedConnection);
        }
    });
    return requestId;
}

quint64 SftpService::uploadDirectory(const ConnectionTarget &target,
                                     const QString &localDirectory,
                                     const QString &remoteParentDirectory,
                                     const SftpTransfer::ExistingFilePolicy policy,
                                     const SftpTransfer::DirectoryConflictPolicy conflictPolicy)
{
    const quint64 requestId = nextRequestId();
    const bool allowUnknown = takeTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);

    (void)QtConcurrent::run([service, pool = m_pool, ledger = m_ledger, requestId, target,
                             localDirectory, remoteParentDirectory, allowUnknown, policy,
                             conflictPolicy]() {
        const QString rootName = QDir(localDirectory).dirName();
        const QString remoteRoot = joinRemotePath(remoteParentDirectory, rootName);
        SftpRecursive::TransferSummary summary;
        summary.rootDescription = remoteRoot;

        // 先完整遍历本地目录再开始传输：这样能在第一个字节发出前就知道总量，进度条从一开始
        // 就有意义。代价是超大目录树的遍历延迟，但本地遍历远快于网络传输，这笔账划算。
        const SftpRecursive::TransferPlan plan = SftpRecursive::buildTransferPlan(
            localDirectory, remoteRoot, collectLocalTree(localDirectory));
        summary.skipped = plan.skipped;
        summary.fileTotal = plan.fileCount;

        // 整棵树的传输全程持有传输通道那条池化连接的 mutex：libssh 会话不是线程安全的，
        // 而在一条会话上交错多个传输只会让所有操作都变慢。目录浏览走独立通道，不受此锁
        // 阻塞；取消标志同样刻意不走这把锁，随时都能生效。
        const QString poolKey = transferPoolKey(target);
        const QSharedPointer<PooledConnection> entry = pool->acquire(poolKey);
        QMutexLocker locker(&entry->mutex);
        SftpConnection *connection = ensureConnected(
            entry.data(), poolKey, target, allowUnknown, service, requestId, pool.get());
        if (connection == nullptr) {
            pool->clearCancel(requestId);
        // 账本按时间节流写盘，请求收尾必须兜底 flush：否则最后几条记录会随进程退出丢失，
        // 下次同步又要把这些文件白传一遍。
        ledger->flush();
            return;
        }

        const auto isCancelled = [pool, requestId]() {
            return pool->waitUntilRunnable(requestId);
        };
        quint64 completedBytes = 0;
        quint64 completedFiles = 0;

        // 用户选择"替换"就意味着结果必须与源端严格一致：先递归删掉远端同名目录，源端已经
        // 删除的文件才不会残留。删除与写入在同一个请求、同一条连接上完成，中间不存在
        // "目录已删除但上传还没开始"的窗口。
        if (conflictPolicy == SftpTransfer::DirectoryConflictPolicy::Replace) {
            const RemoteStat existing = statRemote(connection, remoteRoot);
            if (existing.exists && existing.isDirectory) {
                QString removeError;
                if (!removeRemoteTree(connection, remoteRoot, isCancelled,
                                      &summary.removedForReplace, &removeError)) {
                    pool->clearCancel(requestId);
        // 账本按时间节流写盘，请求收尾必须兜底 flush：否则最后几条记录会随进程退出丢失，
        // 下次同步又要把这些文件白传一遍。
        ledger->flush();
                    deliverConnectionFailure(service, requestId, target,
                                             {false, ErrorKind::RemoteOperation, removeError});
                    return;
                }
                const QByteArray encodedRoot = remoteRoot.toUtf8();
                sftp_rmdir(connection->handle(), encodedRoot.constData());
            } else if (existing.exists) {
                // 同名的普通文件挡住了目录位置；删掉它才能建目录，这同样属于用户选择的替换。
                const QByteArray encodedRoot = remoteRoot.toUtf8();
                sftp_unlink(connection->handle(), encodedRoot.constData());
                ++summary.removedForReplace;
            }
        }

        QString rootError;
        if (!ensureRemoteDirectory(connection, remoteRoot, &rootError)) {
            // 连根目录都建不出来时没有任何可传内容，按整体失败处理而不是逐项失败刷屏。
            pool->clearCancel(requestId);
        // 账本按时间节流写盘，请求收尾必须兜底 flush：否则最后几条记录会随进程退出丢失，
        // 下次同步又要把这些文件白传一遍。
        ledger->flush();
            deliverConnectionFailure(service, requestId, target,
                                     {false, ErrorKind::RemoteOperation, rootError});
            return;
        }

        for (const SftpRecursive::TransferItem &item : plan.items) {
            if (isCancelled()) {
                summary.cancelled = true;
                break;
            }
            postRecursiveProgress(service, requestId, completedFiles, plan.fileCount,
                                  item.destinationPath);
            QString itemError;
            if (item.isDirectory) {
                if (!ensureRemoteDirectory(connection, item.destinationPath, &itemError)) {
                    summary.failed.append({item.destinationPath, itemError});
                }
                continue;
            }
            const FileTransferResult result = writeRemoteFile(
                connection,
                item.sourcePath,
                item.destinationPath,
                TransferContext{ledger.get(), target.stableKey(), policy},
                [&completedBytes, service, requestId, total = plan.totalBytes](const quint64 delta) {
                    completedBytes += delta;
                    postTransferProgress(service, requestId, completedBytes, total);
                },
                isCancelled,
                &itemError);
            if (result.outcome == TransferOutcome::Cancelled) {
                summary.cancelled = true;
                break;
            }
            if (result.outcome == TransferOutcome::Failed) {
                // 单个文件失败不中断整棵树：用户宁愿拿到 99% 的目录加一份失败清单，也不愿
                // 因为一个没有读权限的文件而整个传输白跑。
                summary.failed.append({item.destinationPath, itemError});
                continue;
            }
            // 跳过与传输必须分开计数：把跳过算成已传输，汇总就会宣称"N 个文件已更新"，
            // 而远端其实一个字节都没变。
            if (result.action == SftpTransfer::TransferAction::SkipIdentical) {
                ++summary.filesSkippedIdentical;
                continue;
            }
            ++completedFiles;
        }

        summary.filesTransferred = completedFiles;
        summary.bytesTransferred = completedBytes;
        pool->clearCancel(requestId);
        // 账本按时间节流写盘，请求收尾必须兜底 flush：否则最后几条记录会随进程退出丢失，
        // 下次同步又要把这些文件白传一遍。
        ledger->flush();
        postRecursiveFinished(service, requestId, summary);
    });
    return requestId;
}

quint64 SftpService::downloadDirectory(const ConnectionTarget &target,
                                       const QString &remoteDirectory,
                                       const QString &localParentDirectory,
                                       const SftpTransfer::ExistingFilePolicy policy)
{
    const quint64 requestId = nextRequestId();
    const bool allowUnknown = takeTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);

    (void)QtConcurrent::run([service, pool = m_pool, ledger = m_ledger, requestId, target,
                             remoteDirectory, localParentDirectory, allowUnknown, policy]() {
        const QString normalizedRemote = SftpRecursive::normalizePosixPath(remoteDirectory);
        const QString rootName = normalizedRemote.section(QLatin1Char('/'), -1);
        const QString localRoot = QDir::cleanPath(
            localParentDirectory + QLatin1Char('/') + rootName);
        SftpRecursive::TransferSummary summary;
        summary.rootDescription = localRoot;

        const QString poolKey = transferPoolKey(target);
        const QSharedPointer<PooledConnection> entry = pool->acquire(poolKey);
        QMutexLocker locker(&entry->mutex);
        SftpConnection *connection = ensureConnected(
            entry.data(), poolKey, target, allowUnknown, service, requestId, pool.get());
        if (connection == nullptr) {
            pool->clearCancel(requestId);
        // 账本按时间节流写盘，请求收尾必须兜底 flush：否则最后几条记录会随进程退出丢失，
        // 下次同步又要把这些文件白传一遍。
        ledger->flush();
            return;
        }

        const auto isCancelled = [pool, requestId]() {
            return pool->waitUntilRunnable(requestId);
        };

        // 先整棵遍历再传输：readdir 已经带回每个文件的大小，因此这一趟不额外产生 stat 往返，
        // 却能换来一个从头到尾都准确的总字节数。
        bool cancelledWhileScanning = false;
        const QList<SftpRecursive::SourceEntry> remoteEntries = collectRemoteTree(
            connection, normalizedRemote, isCancelled, &summary.failed, &cancelledWhileScanning);
        if (cancelledWhileScanning) {
            summary.cancelled = true;
            pool->clearCancel(requestId);
        // 账本按时间节流写盘，请求收尾必须兜底 flush：否则最后几条记录会随进程退出丢失，
        // 下次同步又要把这些文件白传一遍。
        ledger->flush();
            postRecursiveFinished(service, requestId, summary);
            return;
        }

        const SftpRecursive::TransferPlan plan = SftpRecursive::buildTransferPlan(
            normalizedRemote, localRoot, remoteEntries);
        summary.skipped += plan.skipped;
        summary.fileTotal = plan.fileCount;

        if (!QDir().mkpath(localRoot)) {
            pool->clearCancel(requestId);
        // 账本按时间节流写盘，请求收尾必须兜底 flush：否则最后几条记录会随进程退出丢失，
        // 下次同步又要把这些文件白传一遍。
        ledger->flush();
            deliverConnectionFailure(
                service, requestId, target,
                {false, ErrorKind::LocalIo,
                 QObject::tr("无法创建本地目录 %1").arg(localRoot)});
            return;
        }

        quint64 completedBytes = 0;
        quint64 completedFiles = 0;
        for (const SftpRecursive::TransferItem &item : plan.items) {
            if (isCancelled()) {
                summary.cancelled = true;
                break;
            }
            postRecursiveProgress(service, requestId, completedFiles, plan.fileCount,
                                  item.destinationPath);
            QString itemError;
            if (item.isDirectory) {
                if (!QDir().mkpath(item.destinationPath)) {
                    summary.failed.append({item.destinationPath,
                                           QObject::tr("无法创建本地目录")});
                }
                continue;
            }
            const FileTransferResult result = readRemoteFile(
                connection,
                item.sourcePath,
                item.destinationPath,
                TransferContext{ledger.get(), target.stableKey(), policy},
                [&completedBytes, service, requestId, total = plan.totalBytes](const quint64 delta) {
                    completedBytes += delta;
                    postTransferProgress(service, requestId, completedBytes, total);
                },
                isCancelled,
                &itemError);
            if (result.outcome == TransferOutcome::Cancelled) {
                summary.cancelled = true;
                break;
            }
            if (result.outcome == TransferOutcome::Failed) {
                summary.failed.append({item.sourcePath, itemError});
                continue;
            }
            if (result.action == SftpTransfer::TransferAction::SkipIdentical) {
                ++summary.filesSkippedIdentical;
                continue;
            }
            ++completedFiles;
        }

        summary.filesTransferred = completedFiles;
        summary.bytesTransferred = completedBytes;
        pool->clearCancel(requestId);
        // 账本按时间节流写盘，请求收尾必须兜底 flush：否则最后几条记录会随进程退出丢失，
        // 下次同步又要把这些文件白传一遍。
        ledger->flush();
        postRecursiveFinished(service, requestId, summary);
    });
    return requestId;
}

void SftpService::cancelRequest(const quint64 requestId)
{
    m_pool->requestCancel(requestId);
}

void SftpService::pauseRequest(const quint64 requestId)
{
    m_pool->requestPause(requestId);
}

void SftpService::resumeRequest(const quint64 requestId)
{
    m_pool->requestResume(requestId);
}

quint64 SftpService::previewFile(const ConnectionTarget &target,
                                 const QString &remotePath,
                                 const quint64 maximumBytes)
{
    const quint64 requestId = nextRequestId();
    const quint64 safeLimit = SftpTransfer::boundedPreviewLimit(maximumBytes);
    const bool allowUnknown = takeTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);
    (void)QtConcurrent::run([service, pool = m_pool, requestId, target, remotePath,
                             safeLimit, allowUnknown]() {
        const QString poolKey = transferPoolKey(target);
        const QSharedPointer<PooledConnection> entry = pool->acquire(poolKey);
        QMutexLocker locker(&entry->mutex);
        SftpConnection *connection = ensureConnected(
            entry.data(), poolKey, target, allowUnknown, service, requestId, pool.get());
        if (connection == nullptr) {
            return;
        }

        const QByteArray encodedPath = remotePath.toUtf8();
        sftp_file remoteFile = sftp_open(connection->handle(),
                                         encodedPath.constData(),
                                         O_RDONLY,
                                         0);
        if (remoteFile == nullptr) {
            deliverConnectionFailure(
                service,
                requestId,
                target,
                {false,
                 ErrorKind::RemoteOperation,
                 QObject::tr("无法读取预览文件 %1：%2")
                     .arg(remotePath, connection->lastError())});
            return;
        }

        // 多读一个字节只用于判断是否截断；返回给 GUI 的数据仍严格不超过 2 MiB 硬上限。
        QByteArray data;
        data.reserve(static_cast<qsizetype>(safeLimit));
        QByteArray chunk(kTransferChunkBytes, Qt::Uninitialized);
        bool truncated = false;
        while (static_cast<quint64>(data.size()) <= safeLimit) {
            const quint64 remainingWithSentinel = safeLimit + 1
                - static_cast<quint64>(data.size());
            const qsizetype requested = static_cast<qsizetype>(
                std::min<quint64>(remainingWithSentinel,
                                  static_cast<quint64>(chunk.size())));
            const ssize_t bytesRead = sftp_read(remoteFile,
                                                chunk.data(),
                                                static_cast<size_t>(requested));
            if (bytesRead == 0) {
                break;
            }
            if (bytesRead < 0) {
                const QString message = QObject::tr("读取预览文件 %1 失败：%2")
                                            .arg(remotePath, connection->lastError());
                sftp_close(remoteFile);
                deliverConnectionFailure(service,
                                         requestId,
                                         target,
                                         {false, ErrorKind::RemoteOperation, message});
                return;
            }
            data.append(chunk.constData(), static_cast<qsizetype>(bytesRead));
            if (static_cast<quint64>(data.size()) > safeLimit) {
                data.truncate(static_cast<qsizetype>(safeLimit));
                truncated = true;
                break;
            }
        }
        sftp_close(remoteFile);

        QMimeDatabase mimeDatabase;
        QString mimeType = mimeDatabase.mimeTypeForFile(
            remotePath, QMimeDatabase::MatchExtension).name();
        if (mimeType.isEmpty() || mimeType == QStringLiteral("application/octet-stream")) {
            mimeType = mimeDatabase.mimeTypeForData(data).name();
        }
        if (service != nullptr) {
            QMetaObject::invokeMethod(
                service,
                [service, requestId, target, remotePath, data, mimeType, truncated]() {
                    if (service != nullptr) {
                        emit service->filePreviewReady(requestId,
                                                       target,
                                                       remotePath,
                                                       data,
                                                       mimeType,
                                                       truncated);
                    }
                },
                Qt::QueuedConnection);
        }
    });
    return requestId;
}

quint64 SftpService::chmodPath(const ConnectionTarget &target,
                               const QString &remotePath,
                               const quint32 permissions)
{
    const quint64 requestId = nextRequestId();
    const bool allowUnknown = takeTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);
    (void)QtConcurrent::run([service, pool = m_pool, requestId, target, remotePath,
                             permissions, allowUnknown]() {
        const QString poolKey = transferPoolKey(target);
        const QSharedPointer<PooledConnection> entry = pool->acquire(poolKey);
        QMutexLocker locker(&entry->mutex);
        SftpConnection *connection = ensureConnected(
            entry.data(), poolKey, target, allowUnknown, service, requestId, pool.get());
        if (connection == nullptr) {
            return;
        }

        const QByteArray encodedPath = remotePath.toUtf8();
        const mode_t safePermissions = static_cast<mode_t>(permissions & 07777U);
        if (sftp_chmod(connection->handle(), encodedPath.constData(), safePermissions) != SSH_OK) {
            deliverConnectionFailure(
                service,
                requestId,
                target,
                {false,
                 ErrorKind::RemoteOperation,
                 QObject::tr("修改 %1 权限失败：%2")
                     .arg(remotePath, connection->lastError())});
            return;
        }
        if (service != nullptr) {
            QMetaObject::invokeMethod(service,
                                      [service, requestId, remotePath, safePermissions]() {
                if (service != nullptr) {
                    emit service->operationSucceeded(
                        requestId,
                        QObject::tr("已将 %1 的权限修改为 %2")
                            .arg(remotePath,
                                 QString::number(static_cast<quint32>(safePermissions), 8)
                                     .rightJustified(4, QLatin1Char('0'))));
                }
            }, Qt::QueuedConnection);
        }
    });
    return requestId;
}

quint64 SftpService::renamePath(const ConnectionTarget &target,
                                const QString &oldPath,
                                const QString &newPath)
{
    const quint64 requestId = nextRequestId();
    const bool allowUnknown = takeTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);
    (void)QtConcurrent::run([service, pool = m_pool, requestId, target, oldPath, newPath, allowUnknown]() {
        const QString poolKey = transferPoolKey(target);
        const QSharedPointer<PooledConnection> entry = pool->acquire(poolKey);
        QMutexLocker locker(&entry->mutex);
        SftpConnection *connection = ensureConnected(
            entry.data(), poolKey, target, allowUnknown, service, requestId, pool.get());
        if (connection == nullptr) {
            return;
        }
        const QByteArray oldEncoded = oldPath.toUtf8();
        const QByteArray newEncoded = newPath.toUtf8();
        if (sftp_rename(connection->handle(), oldEncoded.constData(), newEncoded.constData()) != SSH_OK) {
            const ConnectResult error{false, ErrorKind::RemoteOperation,
                                      QObject::tr("重命名失败：%1").arg(connection->lastError())};
            deliverConnectionFailure(service, requestId, target, error);
            return;
        }
        if (service != nullptr) {
            QMetaObject::invokeMethod(service, [service, requestId, newPath]() {
                if (service != nullptr) {
                    emit service->operationSucceeded(
                        requestId, QObject::tr("已重命名为 %1").arg(newPath));
                }
            }, Qt::QueuedConnection);
        }
    });
    return requestId;
}

quint64 SftpService::removePath(const ConnectionTarget &target,
                                const QString &path,
                                const bool directory)
{
    const quint64 requestId = nextRequestId();
    const bool allowUnknown = takeTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);
    (void)QtConcurrent::run([service, pool = m_pool, requestId, target, path, directory, allowUnknown]() {
        const QString poolKey = transferPoolKey(target);
        const QSharedPointer<PooledConnection> entry = pool->acquire(poolKey);
        QMutexLocker locker(&entry->mutex);
        SftpConnection *connection = ensureConnected(
            entry.data(), poolKey, target, allowUnknown, service, requestId, pool.get());
        if (connection == nullptr) {
            return;
        }
        const QByteArray encodedPath = path.toUtf8();
        const int result = directory
            ? sftp_rmdir(connection->handle(), encodedPath.constData())
            : sftp_unlink(connection->handle(), encodedPath.constData());
        if (result != SSH_OK) {
            const ConnectResult error{false, ErrorKind::RemoteOperation,
                                      QObject::tr("删除 %1 失败：%2。目录必须为空。")
                                          .arg(path, connection->lastError())};
            deliverConnectionFailure(service, requestId, target, error);
            return;
        }
        if (service != nullptr) {
            QMetaObject::invokeMethod(service, [service, requestId, path]() {
                if (service != nullptr) {
                    emit service->operationSucceeded(
                        requestId, QObject::tr("已删除 %1").arg(path));
                }
            }, Qt::QueuedConnection);
        }
    });
    return requestId;
}

quint64 SftpService::statPath(const ConnectionTarget &target, const QString &remotePath)
{
    const quint64 requestId = nextRequestId();
    const bool allowUnknown = takeTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);

    (void)QtConcurrent::run([service, pool = m_pool, requestId, target, remotePath,
                             allowUnknown]() {
        // 走浏览通道而不是传输通道：探测是只读且短暂的，排在一个大文件传输后面会让"选好
        // 目录却迟迟弹不出冲突提示"，用户只会以为程序卡住了。
        const QString poolKey = browsePoolKey(target);
        const QSharedPointer<PooledConnection> entry = pool->acquire(poolKey);
        QMutexLocker locker(&entry->mutex);
        SftpConnection *connection = ensureConnected(
            entry.data(), poolKey, target, allowUnknown, service, requestId, pool.get());
        if (connection == nullptr) {
            return;
        }
        const RemoteStat stat = statRemote(connection, remotePath);
        if (service != nullptr) {
            QMetaObject::invokeMethod(service, [service, requestId, target, remotePath, stat]() {
                if (service != nullptr) {
                    emit service->pathStatReady(requestId, target, remotePath,
                                                stat.exists, stat.isDirectory);
                }
            }, Qt::QueuedConnection);
        }
    });
    return requestId;
}

void SftpService::trustHostKey(const ConnectionTarget &target)
{
    QMutexLocker locker(&m_trustMutex);
    m_temporarilyTrustedHosts.insert(target.stableKey());
}

quint64 SftpService::nextRequestId()
{
    return m_nextRequestId.fetch_add(1, std::memory_order_relaxed);
}

bool SftpService::takeTemporarilyTrusted(const ConnectionTarget &target)
{
    QMutexLocker locker(&m_trustMutex);
    // QSet::remove 同时完成“检查”和“消费”，锁内没有另一请求抢走同一授权的窗口。
    // Qt 6.8 直接返回 bool，无需再与 0 比较；直接返回还能避免 MSVC 的 C4804 警告。
    return m_temporarilyTrustedHosts.remove(target.stableKey());
}
