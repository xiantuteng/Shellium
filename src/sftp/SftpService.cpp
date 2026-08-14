#include "sftp/SftpService.h"

#include "ssh/SshTunnelBroker.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
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

#include <fcntl.h>
#include <unistd.h>

namespace {

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
QByteArray quoteRemoteShellArgument(QString value)
{
    value.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
    return "'" + value.toUtf8() + "'";
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
     * @param allowUnknownHost 用户是否已确认本次遇到的未知指纹。
     * @return 结构化连接结果；未知主机会附带 SHA-256 指纹。
     */
    ConnectResult open(const ConnectionTarget &target, const bool allowUnknownHost)
    {
        ConnectResult directResult = openDirect(target, allowUnknownHost);
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
     * @param allowUnknownHost 是否已由用户确认内层服务器指纹。
     * @return 完整连接与 SFTP 初始化结果。
     */
    ConnectResult openDirect(const ConnectionTarget &target, const bool allowUnknownHost)
    {
        int tunnelFd = -1;
        if (!target.tunnelRouteId.isEmpty()) {
            QString tunnelError;
            tunnelFd = SshTunnelBroker::instance().openSocket(target.tunnelRouteId, &tunnelError);
            if (tunnelFd < 0) {
                return failure(SftpService::ErrorKind::Connection,
                               QObject::tr("无法复用外层 SSH 会话：%1").arg(tunnelError));
            }
        }

        const ConnectResult connected = connectAndAuthenticate(
            target, allowUnknownHost, tunnelFd, QObject::tr("SFTP"));
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
     * @param allowUnknownHost 是否允许写入此前已由界面确认的未知主机指纹。
     * @param transportFd 已连通的隧道 fd；负数表示按 host/proxyJumps 自行连接。
     * @param authenticationLabel 认证失败消息中的连接角色名称。
     * @return 成功时 m_session 已认证；失败时返回可分类诊断。
     */
    ConnectResult connectAndAuthenticate(const ConnectionTarget &target,
                                         const bool allowUnknownHost,
                                         const int transportFd,
                                         const QString &authenticationLabel)
    {
        m_session = ssh_new();
        if (m_session == nullptr) {
            if (transportFd >= 0) {
                // ssh_new 失败时 fd 尚未交给 libssh，仍由调用方负责关闭，避免隧道端点泄漏。
                ::close(transportFd);
            }
            return failure(SftpService::ErrorKind::Connection,
                           QObject::tr("无法创建 libssh 会话。"));
        }

        const QByteArray host = target.host.toUtf8();
        const QByteArray user = target.username.toUtf8();
        const unsigned int port = target.port;
        ssh_options_set(m_session, SSH_OPTIONS_HOST, host.constData());
        if (transportFd >= 0) {
            // SSH_OPTIONS_FD 接管 fd 的生命周期；HOST 仍保留真实地址，使 known_hosts 校验
            // 绑定内层主机而不是 socketpair 的本地端。
            int fd = transportFd;
            if (ssh_options_set(m_session, SSH_OPTIONS_FD, &fd) != SSH_OK) {
                // 选项设置失败代表 libssh 没有接管 fd；此分支必须显式关闭调用方端点。
                ::close(transportFd);
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
        if (transportFd < 0 && !target.proxyJumps.isEmpty()) {
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

        const ConnectResult verification = verifyServerKey(allowUnknownHost);
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

    ConnectResult verifyServerKey(const bool allowUnknownHost)
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
            return failure(SftpService::ErrorKind::HostKeyChanged,
                           QObject::tr("服务器主机密钥与 known_hosts 记录不一致。为防止中间人攻击，连接已终止。"));
        }
        if ((state == SSH_KNOWN_HOSTS_UNKNOWN || state == SSH_KNOWN_HOSTS_NOT_FOUND)
            && !allowUnknownHost) {
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

        // 只有界面已展示指纹且用户明确确认时 allowUnknownHost 才会为 true。
        if (ssh_session_update_known_hosts(m_session) != SSH_OK) {
            return failure(SftpService::ErrorKind::Connection,
                           QObject::tr("写入 known_hosts 失败：%1")
                               .arg(QString::fromUtf8(ssh_get_error(m_session))));
        }
        return {.success = true};
    }

    static ConnectResult failure(const SftpService::ErrorKind kind, const QString &message)
    {
        return {.success = false, .kind = kind, .message = message};
    }

    ssh_session m_session = nullptr;
    sftp_session m_sftp = nullptr;
    /** @brief 远端 OpenSSH 兜底使用的 exec channel；所有权随 m_sftp 一起释放。 */
    ssh_channel m_remoteSftpChannel = nullptr;
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

/**
 * @brief 把一个本地文件写到远端路径。
 *
 * 递归上传与单文件上传共用同一实现：两条路径若各写一份，超时、部分写和取消这些边界条件
 * 迟早会只在其中一条上被修好。
 */
TransferOutcome writeRemoteFile(SftpConnection *connection,
                                const QString &localPath,
                                const QString &remotePath,
                                const SftpTransfer::ExistingFilePolicy policy,
                                const ProgressReporter &onProgress,
                                const CancelProbe &isCancelled,
                                QString *errorMessage)
{
    QFile localFile(localPath);
    if (!localFile.open(QIODevice::ReadOnly)) {
        *errorMessage = QObject::tr("无法读取本地文件：%1").arg(localFile.errorString());
        return TransferOutcome::Failed;
    }

    const quint64 localSize = static_cast<quint64>(std::max<qint64>(localFile.size(), 0));
    const QByteArray encodedRemotePath = remotePath.toUtf8();
    quint64 remoteSize = 0;
    if (policy == SftpTransfer::ExistingFilePolicy::ResumeWhenPossible) {
        if (sftp_attributes attributes = sftp_stat(connection->handle(),
                                                   encodedRemotePath.constData())) {
            remoteSize = attributes->size;
            sftp_attributes_free(attributes);
        }
    }
    const SftpTransfer::ResumePlan resumePlan =
        SftpTransfer::makeResumePlan(localSize, remoteSize, policy);
    if (onProgress && resumePlan.offset > 0) {
        // 已存在且确认可续传的字节计入完成进度，否则恢复任务会从 0% 突然跳到后半程。
        onProgress(resumePlan.offset);
    }
    if (resumePlan.alreadyComplete) {
        return TransferOutcome::Ok;
    }

    const int openFlags = O_WRONLY | O_CREAT
        | (resumePlan.truncateDestination ? O_TRUNC : 0);
    sftp_file remoteFile = sftp_open(connection->handle(),
                                     encodedRemotePath.constData(),
                                     openFlags,
                                     0644);
    if (remoteFile == nullptr) {
        *errorMessage = QObject::tr("无法创建远程文件 %1：%2")
                            .arg(remotePath, connection->lastError());
        return TransferOutcome::Failed;
    }
    if (resumePlan.offset > 0) {
        if (!localFile.seek(static_cast<qint64>(resumePlan.offset))) {
            sftp_close(remoteFile);
            *errorMessage = QObject::tr("无法定位本地续传偏移 %1：%2")
                                .arg(resumePlan.offset)
                                .arg(localFile.errorString());
            return TransferOutcome::Failed;
        }
        // sftp_seek64 没有返回值；偏移已经由同一 SFTP 会话上的 stat 验证，随后首个 write
        // 若失败会进入统一错误路径，不能通过额外写入探测而破坏远端内容。
        sftp_seek64(remoteFile, resumePlan.offset);
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

    // 把最旧的未确认写请求等到完成；出错时仍会消耗掉 aio 句柄，避免内存泄漏和会话错乱。
    const auto drainOne = [&]() -> bool {
        InFlightWrite front = inFlight.front();
        inFlight.pop_front();
        const ssize_t written = sftp_aio_wait_write(&front.aio);
        if (written < 0 || static_cast<size_t>(written) != front.requestedLen) {
            *errorMessage = QObject::tr("上传 %1 失败：%2")
                                .arg(remotePath, connection->lastError());
            return false;
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
    return outcome;
}

/**
 * @brief 把一个远端文件读到本地路径。
 *
 * 取消或网络失败时保留已写入部分，下一次选择续传策略即可从现有大小继续。覆盖策略会先
 * 截断目标；续传策略在本地文件异常大于远端源时也会自动截断，避免保留旧尾部。
 */
TransferOutcome readRemoteFile(SftpConnection *connection,
                               const QString &remotePath,
                               const QString &localPath,
                               const SftpTransfer::ExistingFilePolicy policy,
                               const ProgressReporter &onProgress,
                               const CancelProbe &isCancelled,
                               QString *errorMessage,
                               quint64 *remoteSize = nullptr)
{
    const QByteArray encodedRemotePath = remotePath.toUtf8();
    sftp_file remoteFile = sftp_open(connection->handle(),
                                     encodedRemotePath.constData(),
                                     O_RDONLY,
                                     0);
    if (remoteFile == nullptr) {
        *errorMessage = QObject::tr("无法读取远程文件 %1：%2")
                            .arg(remotePath, connection->lastError());
        return TransferOutcome::Failed;
    }
    quint64 resolvedRemoteSize = 0;
    if (sftp_attributes attributes = sftp_fstat(remoteFile)) {
        resolvedRemoteSize = attributes->size;
        sftp_attributes_free(attributes);
    }
    if (remoteSize != nullptr) {
        *remoteSize = resolvedRemoteSize;
    }

    quint64 localSize = 0;
    if (policy == SftpTransfer::ExistingFilePolicy::ResumeWhenPossible) {
        const QFileInfo localInfo(localPath);
        if (localInfo.exists() && localInfo.isFile()) {
            localSize = static_cast<quint64>(std::max<qint64>(localInfo.size(), 0));
        }
    }
    const SftpTransfer::ResumePlan resumePlan =
        SftpTransfer::makeResumePlan(resolvedRemoteSize, localSize, policy);
    if (onProgress && resumePlan.offset > 0) {
        onProgress(resumePlan.offset);
    }
    if (resumePlan.alreadyComplete) {
        sftp_close(remoteFile);
        return TransferOutcome::Ok;
    }

    QFile localFile(localPath);
    const QIODevice::OpenMode localMode = resumePlan.truncateDestination
        ? QIODevice::WriteOnly | QIODevice::Truncate
        : QIODevice::ReadWrite;
    if (!localFile.open(localMode)) {
        sftp_close(remoteFile);
        *errorMessage = QObject::tr("无法写入本地文件 %1：%2")
                            .arg(localPath, localFile.errorString());
        return TransferOutcome::Failed;
    }
    if (resumePlan.offset > 0) {
        if (!localFile.seek(static_cast<qint64>(resumePlan.offset))) {
            sftp_close(remoteFile);
            *errorMessage = QObject::tr("无法定位本地续传偏移 %1：%2")
                                .arg(resumePlan.offset)
                                .arg(localFile.errorString());
            return TransferOutcome::Failed;
        }
        sftp_seek64(remoteFile, resumePlan.offset);
    }

    QByteArray buffer(kTransferChunkBytes, Qt::Uninitialized);
    TransferOutcome outcome = TransferOutcome::Ok;
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
        if (localFile.write(buffer.constData(), bytesRead) != bytesRead) {
            *errorMessage = QObject::tr("保存 %1 失败：%2")
                                .arg(localPath, localFile.errorString());
            outcome = TransferOutcome::Failed;
            break;
        }
        if (onProgress) {
            onProgress(static_cast<quint64>(bytesRead));
        }
    }
    sftp_close(remoteFile);
    if (!localFile.flush() && outcome == TransferOutcome::Ok) {
        *errorMessage = QObject::tr("刷新本地文件 %1 失败：%2")
                            .arg(localPath, localFile.errorString());
        outcome = TransferOutcome::Failed;
    }
    localFile.close();
    return outcome;
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
    const bool allowUnknown = isTemporarilyTrusted(target);
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

quint64 SftpService::uploadFile(const ConnectionTarget &target,
                                const QString &localPath,
                                const QString &remotePath,
                                const SftpTransfer::ExistingFilePolicy policy)
{
    const quint64 requestId = nextRequestId();
    const bool allowUnknown = isTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);

    (void)QtConcurrent::run([service, pool = m_pool, requestId, target, localPath,
                             remotePath, allowUnknown, policy]() {
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
        const TransferOutcome outcome = writeRemoteFile(
            connection,
            localPath,
            remotePath,
            policy,
            [&completed, service, requestId, total](const quint64 delta) {
                completed += delta;
                postTransferProgress(service, requestId, completed, total);
            },
            [pool, requestId]() { return pool->waitUntilRunnable(requestId); },
            &errorMessage);
        pool->clearCancel(requestId);

        if (outcome != TransferOutcome::Ok) {
            const ConnectResult error{false,
                                      outcome == TransferOutcome::Cancelled
                                          ? ErrorKind::LocalIo
                                          : ErrorKind::RemoteOperation,
                                      outcome == TransferOutcome::Cancelled
                                          ? QObject::tr("上传 %1 已取消").arg(remotePath)
                                          : errorMessage};
            deliverConnectionFailure(service, requestId, target, error);
            return;
        }
        if (service != nullptr) {
            QMetaObject::invokeMethod(service, [service, requestId, remotePath]() {
                if (service != nullptr) {
                    emit service->operationSucceeded(
                        requestId, QObject::tr("已上传到 %1").arg(remotePath));
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
    const bool allowUnknown = isTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);

    (void)QtConcurrent::run([service, pool = m_pool, requestId, target, remotePath,
                             localPath, allowUnknown, policy]() {
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
        const TransferOutcome outcome = readRemoteFile(
            connection,
            remotePath,
            localPath,
            policy,
            [&completed, &total, service, requestId](const quint64 delta) {
                completed += delta;
                postTransferProgress(service, requestId, completed, total);
            },
            [pool, requestId]() { return pool->waitUntilRunnable(requestId); },
            &errorMessage,
            &total);
        pool->clearCancel(requestId);

        if (outcome != TransferOutcome::Ok) {
            const ConnectResult error{false, ErrorKind::LocalIo,
                                      outcome == TransferOutcome::Cancelled
                                          ? QObject::tr("下载 %1 已取消").arg(remotePath)
                                          : errorMessage};
            deliverConnectionFailure(service, requestId, target, error);
            return;
        }
        if (service != nullptr) {
            QMetaObject::invokeMethod(service, [service, requestId, localPath]() {
                if (service != nullptr) {
                    emit service->operationSucceeded(
                        requestId, QObject::tr("已下载到 %1").arg(localPath));
                }
            }, Qt::QueuedConnection);
        }
    });
    return requestId;
}

quint64 SftpService::uploadDirectory(const ConnectionTarget &target,
                                     const QString &localDirectory,
                                     const QString &remoteParentDirectory,
                                     const SftpTransfer::ExistingFilePolicy policy)
{
    const quint64 requestId = nextRequestId();
    const bool allowUnknown = isTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);

    (void)QtConcurrent::run([service, pool = m_pool, requestId, target, localDirectory,
                             remoteParentDirectory, allowUnknown, policy]() {
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
            return;
        }

        const auto isCancelled = [pool, requestId]() {
            return pool->waitUntilRunnable(requestId);
        };
        quint64 completedBytes = 0;
        quint64 completedFiles = 0;

        QString rootError;
        if (!ensureRemoteDirectory(connection, remoteRoot, &rootError)) {
            // 连根目录都建不出来时没有任何可传内容，按整体失败处理而不是逐项失败刷屏。
            pool->clearCancel(requestId);
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
            const TransferOutcome outcome = writeRemoteFile(
                connection,
                item.sourcePath,
                item.destinationPath,
                policy,
                [&completedBytes, service, requestId, total = plan.totalBytes](const quint64 delta) {
                    completedBytes += delta;
                    postTransferProgress(service, requestId, completedBytes, total);
                },
                isCancelled,
                &itemError);
            if (outcome == TransferOutcome::Cancelled) {
                summary.cancelled = true;
                break;
            }
            if (outcome == TransferOutcome::Failed) {
                // 单个文件失败不中断整棵树：用户宁愿拿到 99% 的目录加一份失败清单，也不愿
                // 因为一个没有读权限的文件而整个传输白跑。
                summary.failed.append({item.destinationPath, itemError});
                continue;
            }
            ++completedFiles;
        }

        summary.filesTransferred = completedFiles;
        summary.bytesTransferred = completedBytes;
        pool->clearCancel(requestId);
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
    const bool allowUnknown = isTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);

    (void)QtConcurrent::run([service, pool = m_pool, requestId, target, remoteDirectory,
                             localParentDirectory, allowUnknown, policy]() {
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
            postRecursiveFinished(service, requestId, summary);
            return;
        }

        const SftpRecursive::TransferPlan plan = SftpRecursive::buildTransferPlan(
            normalizedRemote, localRoot, remoteEntries);
        summary.skipped += plan.skipped;
        summary.fileTotal = plan.fileCount;

        if (!QDir().mkpath(localRoot)) {
            pool->clearCancel(requestId);
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
            const TransferOutcome outcome = readRemoteFile(
                connection,
                item.sourcePath,
                item.destinationPath,
                policy,
                [&completedBytes, service, requestId, total = plan.totalBytes](const quint64 delta) {
                    completedBytes += delta;
                    postTransferProgress(service, requestId, completedBytes, total);
                },
                isCancelled,
                &itemError);
            if (outcome == TransferOutcome::Cancelled) {
                summary.cancelled = true;
                break;
            }
            if (outcome == TransferOutcome::Failed) {
                summary.failed.append({item.sourcePath, itemError});
                continue;
            }
            ++completedFiles;
        }

        summary.filesTransferred = completedFiles;
        summary.bytesTransferred = completedBytes;
        pool->clearCancel(requestId);
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
    const bool allowUnknown = isTemporarilyTrusted(target);
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
    const bool allowUnknown = isTemporarilyTrusted(target);
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
    const bool allowUnknown = isTemporarilyTrusted(target);
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
    const bool allowUnknown = isTemporarilyTrusted(target);
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

void SftpService::trustUnknownHost(const ConnectionTarget &target)
{
    QMutexLocker locker(&m_trustMutex);
    m_temporarilyTrustedHosts.insert(target.stableKey());
}

quint64 SftpService::nextRequestId()
{
    return m_nextRequestId.fetch_add(1, std::memory_order_relaxed);
}

bool SftpService::isTemporarilyTrusted(const ConnectionTarget &target) const
{
    QMutexLocker locker(&m_trustMutex);
    return m_temporarilyTrustedHosts.contains(target.stableKey());
}
