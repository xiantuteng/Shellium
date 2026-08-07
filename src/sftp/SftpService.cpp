#include "sftp/SftpService.h"

#include "ssh/SshTunnelBroker.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSaveFile>
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QtConcurrentRun>

#include <functional>
#include <memory>

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <fcntl.h>

namespace {

/** @brief 后台操作连接阶段的结构化结果。 */
struct ConnectResult {
    bool success = false;
    SftpService::ErrorKind kind = SftpService::ErrorKind::Connection;
    QString message;
    QString fingerprint;
};

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
        if (m_sftp != nullptr) {
            sftp_free(m_sftp);
        }
        if (m_session != nullptr) {
            ssh_disconnect(m_session);
            ssh_free(m_session);
        }
    }

    /**
     * @brief 建立 SSH、校验主机密钥、完成认证并初始化 SFTP 子系统。
     * @param target 目标服务器及完整跳板链。
     * @param allowUnknownHost 用户是否已确认本次遇到的未知指纹。
     * @return 结构化连接结果；未知主机会附带 SHA-256 指纹。
     */
    ConnectResult open(const ConnectionTarget &target, const bool allowUnknownHost)
    {
        m_session = ssh_new();
        if (m_session == nullptr) {
            return failure(SftpService::ErrorKind::Connection,
                           QObject::tr("无法创建 libssh 会话。"));
        }

        const QByteArray host = target.host.toUtf8();
        const QByteArray user = target.username.toUtf8();
        const unsigned int port = target.port;
        ssh_options_set(m_session, SSH_OPTIONS_HOST, host.constData());
        int tunnelFd = -1;
        if (!target.tunnelRouteId.isEmpty()) {
            QString tunnelError;
            tunnelFd = SshTunnelBroker::instance().openSocket(target.tunnelRouteId, &tunnelError);
            if (tunnelFd < 0) {
                return failure(SftpService::ErrorKind::Connection,
                               QObject::tr("无法复用外层 SSH 会话：%1").arg(tunnelError));
            }
            // SSH_OPTIONS_FD 让 libssh 在已连接 socket 上执行内层握手，同时 HOST 仍保持真实
            // 内层地址，known_hosts 校验不会错误绑定到 127.0.0.1。
            ssh_options_set(m_session, SSH_OPTIONS_FD, &tunnelFd);
        }
        if (!user.isEmpty()) {
            ssh_options_set(m_session, SSH_OPTIONS_USER, user.constData());
        }
        ssh_options_set(m_session, SSH_OPTIONS_PORT, &port);

        if (!target.identityFile.trimmed().isEmpty()) {
            const QByteArray identity = QFile::encodeName(target.identityFile);
            ssh_options_set(m_session, SSH_OPTIONS_IDENTITY, identity.constData());
        }
        if (target.tunnelRouteId.isEmpty() && !target.proxyJumps.isEmpty()) {
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
                           QObject::tr("SFTP 认证失败：%1。请检查用户名、密码、私钥或 SSH Agent。")
                               .arg(QString::fromUtf8(ssh_get_error(m_session))));
        }

        m_sftp = sftp_new(m_session);
        if (m_sftp == nullptr || sftp_init(m_sftp) != SSH_OK) {
            return failure(SftpService::ErrorKind::Connection,
                           QObject::tr("服务器未能初始化 SFTP 子系统：%1")
                               .arg(QString::fromUtf8(ssh_get_error(m_session))));
        }
        return {.success = true};
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
        return m_session != nullptr && m_sftp != nullptr
            && ssh_is_connected(m_session) != 0;
    }
    [[nodiscard]] QString lastError() const
    {
        return m_session == nullptr ? QObject::tr("未知 libssh 错误")
                                    : QString::fromUtf8(ssh_get_error(m_session));
    }

private:
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
 * libssh 会话不是线程安全的，因此同一条连接上的操作由 mutex 串行化；不同目标的请求仍然
 * 并行。串行化是可接受的代价：用户在一台服务器上的文件操作本来就是顺序发生的。
 */
struct PooledConnection {
    QMutex mutex;
    std::unique_ptr<SftpConnection> connection;
    QElapsedTimer idleTimer;
};

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
        QMutexLocker locker(&mutex);
        entries.clear();
    }

    /**
     * @brief 标记某个请求为已取消。
     *
     * 取消标志放在池里而不是 SftpService 里：后台任务只持有池的 shared_ptr，服务本身可能在
     * 传输结束前就被销毁（用户关掉了标签页），那正是最需要让循环停下来的时刻。
     */
    void requestCancel(const quint64 requestId)
    {
        QMutexLocker locker(&cancelMutex);
        cancelledRequests.insert(requestId);
    }

    /** @brief 查询请求是否已被取消，供传输循环在分块边界轮询。 */
    [[nodiscard]] bool isCancelled(const quint64 requestId) const
    {
        QMutexLocker locker(&cancelMutex);
        return cancelledRequests.contains(requestId);
    }

    /** @brief 请求结束后清除标志，避免取消集合随会话时长无限增长。 */
    void clearCancel(const quint64 requestId)
    {
        QMutexLocker locker(&cancelMutex);
        cancelledRequests.remove(requestId);
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

    mutable QMutex cancelMutex;
    QSet<quint64> cancelledRequests;
};

namespace {

/**
 * @brief 借出目标对应的池化连接，必要时重新建立。
 * @return 可用连接；失败时返回 nullptr 并已向界面投递结构化错误。
 *
 * 调用方必须已持有该条目的 mutex：libssh 会话不是线程安全的。
 */
SftpConnection *ensureConnected(PooledConnection *entry,
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
        pool->invalidate(target.stableKey());
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
 * @brief 把一个本地文件写到远端路径。
 *
 * 递归上传与单文件上传共用同一实现：两条路径若各写一份，超时、部分写和取消这些边界条件
 * 迟早会只在其中一条上被修好。
 */
TransferOutcome writeRemoteFile(SftpConnection *connection,
                                const QString &localPath,
                                const QString &remotePath,
                                const ProgressReporter &onProgress,
                                const CancelProbe &isCancelled,
                                QString *errorMessage)
{
    QFile localFile(localPath);
    if (!localFile.open(QIODevice::ReadOnly)) {
        *errorMessage = QObject::tr("无法读取本地文件：%1").arg(localFile.errorString());
        return TransferOutcome::Failed;
    }

    const QByteArray encodedRemotePath = remotePath.toUtf8();
    sftp_file remoteFile = sftp_open(connection->handle(),
                                     encodedRemotePath.constData(),
                                     O_WRONLY | O_CREAT | O_TRUNC,
                                     0644);
    if (remoteFile == nullptr) {
        *errorMessage = QObject::tr("无法创建远程文件 %1：%2")
                            .arg(remotePath, connection->lastError());
        return TransferOutcome::Failed;
    }

    TransferOutcome outcome = TransferOutcome::Ok;
    while (!localFile.atEnd()) {
        if (isCancelled && isCancelled()) {
            outcome = TransferOutcome::Cancelled;
            break;
        }
        const QByteArray chunk = localFile.read(kTransferChunkBytes);
        qsizetype offset = 0;
        while (offset < chunk.size()) {
            const ssize_t written = sftp_write(remoteFile,
                                               chunk.constData() + offset,
                                               static_cast<size_t>(chunk.size() - offset));
            if (written < 0) {
                *errorMessage = QObject::tr("上传 %1 失败：%2")
                                    .arg(remotePath, connection->lastError());
                outcome = TransferOutcome::Failed;
                break;
            }
            offset += written;
            if (onProgress) {
                onProgress(static_cast<quint64>(written));
            }
        }
        if (outcome != TransferOutcome::Ok) {
            break;
        }
    }
    sftp_close(remoteFile);
    return outcome;
}

/**
 * @brief 把一个远端文件读到本地路径。
 *
 * 使用 QSaveFile：网络中断、磁盘写满或取消时，目标位置不会留下看似完整的半成品。
 */
TransferOutcome readRemoteFile(SftpConnection *connection,
                               const QString &remotePath,
                               const QString &localPath,
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
    if (remoteSize != nullptr) {
        *remoteSize = 0;
        if (sftp_attributes attributes = sftp_fstat(remoteFile)) {
            *remoteSize = attributes->size;
            sftp_attributes_free(attributes);
        }
    }

    QSaveFile localFile(localPath);
    if (!localFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        sftp_close(remoteFile);
        *errorMessage = QObject::tr("无法写入本地文件 %1：%2")
                            .arg(localPath, localFile.errorString());
        return TransferOutcome::Failed;
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
        if (bytesRead < 0 || localFile.write(buffer.constData(), bytesRead) != bytesRead) {
            *errorMessage = QObject::tr("下载或保存 %1 失败：%2")
                                .arg(localPath, connection->lastError());
            outcome = TransferOutcome::Failed;
            break;
        }
        if (onProgress) {
            onProgress(static_cast<quint64>(bytesRead));
        }
    }
    sftp_close(remoteFile);

    if (outcome != TransferOutcome::Ok || !localFile.commit()) {
        localFile.cancelWriting();
        if (outcome == TransferOutcome::Ok) {
            *errorMessage = QObject::tr("保存 %1 失败：%2").arg(localPath, localFile.errorString());
            outcome = TransferOutcome::Failed;
        }
        return outcome;
    }
    return TransferOutcome::Ok;
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
        const QSharedPointer<PooledConnection> entry = pool->acquire(target.stableKey());
        QMutexLocker locker(&entry->mutex);
        SftpConnection *connection = ensureConnected(
            entry.data(), target, allowUnknown, service, requestId, pool.get());
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
                                const QString &remotePath)
{
    const quint64 requestId = nextRequestId();
    const bool allowUnknown = isTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);

    (void)QtConcurrent::run([service, pool = m_pool, requestId, target, localPath, remotePath, allowUnknown]() {
        const quint64 total = static_cast<quint64>(QFileInfo(localPath).size());
        const QSharedPointer<PooledConnection> entry = pool->acquire(target.stableKey());
        QMutexLocker locker(&entry->mutex);
        SftpConnection *connection = ensureConnected(
            entry.data(), target, allowUnknown, service, requestId, pool.get());
        if (connection == nullptr) {
            return;
        }

        quint64 completed = 0;
        QString errorMessage;
        const TransferOutcome outcome = writeRemoteFile(
            connection,
            localPath,
            remotePath,
            [&completed, service, requestId, total](const quint64 delta) {
                completed += delta;
                postTransferProgress(service, requestId, completed, total);
            },
            [pool, requestId]() { return pool->isCancelled(requestId); },
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
                                  const QString &localPath)
{
    const quint64 requestId = nextRequestId();
    const bool allowUnknown = isTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);

    (void)QtConcurrent::run([service, pool = m_pool, requestId, target, remotePath, localPath, allowUnknown]() {
        const QSharedPointer<PooledConnection> entry = pool->acquire(target.stableKey());
        QMutexLocker locker(&entry->mutex);
        SftpConnection *connection = ensureConnected(
            entry.data(), target, allowUnknown, service, requestId, pool.get());
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
            [&completed, &total, service, requestId](const quint64 delta) {
                completed += delta;
                postTransferProgress(service, requestId, completed, total);
            },
            [pool, requestId]() { return pool->isCancelled(requestId); },
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
                                     const QString &remoteParentDirectory)
{
    const quint64 requestId = nextRequestId();
    const bool allowUnknown = isTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);

    (void)QtConcurrent::run([service, pool = m_pool, requestId, target, localDirectory,
                             remoteParentDirectory, allowUnknown]() {
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

        // 整棵树的传输全程持有同一条池化连接的 mutex：libssh 会话不是线程安全的，而在一条
        // 会话上交错多个传输只会让所有操作都变慢。代价是同服务器的目录浏览要等传输结束，
        // 因此取消标志刻意不走这把锁，随时都能生效。
        const QSharedPointer<PooledConnection> entry = pool->acquire(target.stableKey());
        QMutexLocker locker(&entry->mutex);
        SftpConnection *connection = ensureConnected(
            entry.data(), target, allowUnknown, service, requestId, pool.get());
        if (connection == nullptr) {
            pool->clearCancel(requestId);
            return;
        }

        const auto isCancelled = [pool, requestId]() { return pool->isCancelled(requestId); };
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
                                       const QString &localParentDirectory)
{
    const quint64 requestId = nextRequestId();
    const bool allowUnknown = isTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);

    (void)QtConcurrent::run([service, pool = m_pool, requestId, target, remoteDirectory,
                             localParentDirectory, allowUnknown]() {
        const QString normalizedRemote = SftpRecursive::normalizePosixPath(remoteDirectory);
        const QString rootName = normalizedRemote.section(QLatin1Char('/'), -1);
        const QString localRoot = QDir::cleanPath(
            localParentDirectory + QLatin1Char('/') + rootName);
        SftpRecursive::TransferSummary summary;
        summary.rootDescription = localRoot;

        const QSharedPointer<PooledConnection> entry = pool->acquire(target.stableKey());
        QMutexLocker locker(&entry->mutex);
        SftpConnection *connection = ensureConnected(
            entry.data(), target, allowUnknown, service, requestId, pool.get());
        if (connection == nullptr) {
            pool->clearCancel(requestId);
            return;
        }

        const auto isCancelled = [pool, requestId]() { return pool->isCancelled(requestId); };

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

quint64 SftpService::renamePath(const ConnectionTarget &target,
                                const QString &oldPath,
                                const QString &newPath)
{
    const quint64 requestId = nextRequestId();
    const bool allowUnknown = isTemporarilyTrusted(target);
    const QPointer<SftpService> service(this);
    (void)QtConcurrent::run([service, pool = m_pool, requestId, target, oldPath, newPath, allowUnknown]() {
        const QSharedPointer<PooledConnection> entry = pool->acquire(target.stableKey());
        QMutexLocker locker(&entry->mutex);
        SftpConnection *connection = ensureConnected(
            entry.data(), target, allowUnknown, service, requestId, pool.get());
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
        const QSharedPointer<PooledConnection> entry = pool->acquire(target.stableKey());
        QMutexLocker locker(&entry->mutex);
        SftpConnection *connection = ensureConnected(
            entry.data(), target, allowUnknown, service, requestId, pool.get());
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
