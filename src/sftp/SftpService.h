#pragma once

#include "core/ConnectionTarget.h"

#include <QDateTime>
#include <QMutex>
#include <QObject>
#include <QSet>

#include <atomic>

/**
 * @brief 远程目录中的一个文件或文件夹条目。
 *
 * path 始终保存完整远端路径，避免界面切换目录后对旧选择执行到错误位置。
 */
struct RemoteFileInfo {
    QString name;
    QString path;
    quint64 size = 0;
    quint32 permissions = 0;
    QString owner;
    QString group;
    QDateTime modifiedAt;
    bool isDirectory = false;
    bool isSymbolicLink = false;
};

Q_DECLARE_METATYPE(RemoteFileInfo)
Q_DECLARE_METATYPE(QList<RemoteFileInfo>)

/**
 * @brief 在后台线程中执行 libssh SFTP 操作，并把结果安全送回主线程。
 *
 * 每个请求建立独立 SSH/SFTP 会话，因此不同终端分屏、不同嵌套服务器可以并行传输，
 * 且某个服务器断线不会污染其他请求。服务不会持久化密码，日志和错误消息中也不会包含密码。
 */
class SftpService final : public QObject {
    Q_OBJECT

public:
    /** @brief 供界面区分并采取不同恢复动作的错误类别。 */
    enum class ErrorKind {
        Connection,
        Authentication,
        HostKeyUnknown,
        HostKeyChanged,
        LegacyAlgorithmRequired,
        RemoteOperation,
        LocalIo
    };
    Q_ENUM(ErrorKind)

    explicit SftpService(QObject *parent = nullptr);

    /** @brief 异步列出远程目录，不阻塞 Qt 界面线程。 */
    quint64 listDirectory(const ConnectionTarget &target, const QString &path);

    /** @brief 异步上传单个本地文件到指定远端完整路径。 */
    quint64 uploadFile(const ConnectionTarget &target,
                       const QString &localPath,
                       const QString &remotePath);

    /** @brief 异步下载单个远端文件到指定本地完整路径。 */
    quint64 downloadFile(const ConnectionTarget &target,
                         const QString &remotePath,
                         const QString &localPath);

    /** @brief 异步重命名或移动一个远端条目。 */
    quint64 renamePath(const ConnectionTarget &target,
                       const QString &oldPath,
                       const QString &newPath);

    /** @brief 异步删除文件或空目录；目录类型由界面明确传入。 */
    quint64 removePath(const ConnectionTarget &target,
                       const QString &path,
                       bool directory);

    /**
     * @brief 允许目标在本次应用运行期间写入 known_hosts。
     * @param target 用户已确认指纹的目标。
     *
     * 此方法必须由明确的用户确认触发；不会提供静默全局信任开关。
     */
    void trustUnknownHost(const ConnectionTarget &target);

signals:
    /** @brief 目录读取成功，requestId 用于丢弃过期响应。 */
    void directoryListed(quint64 requestId,
                         const ConnectionTarget &target,
                         const QString &path,
                         const QList<RemoteFileInfo> &entries);

    /** @brief 上传、下载、重命名或删除成功。 */
    void operationSucceeded(quint64 requestId, const QString &description);

    /** @brief 请求失败，并携带可用于恢复判断的结构化错误类别。 */
    void operationFailed(quint64 requestId,
                         const ConnectionTarget &target,
                         SftpService::ErrorKind kind,
                         const QString &message);

    /** @brief 首次连接主机时，要求界面展示 SHA-256 指纹并由用户决定是否信任。 */
    void hostKeyVerificationRequired(const ConnectionTarget &target,
                                     const QString &fingerprint);

    /** @brief 文件传输进度；total 为 0 表示服务器未提供大小。 */
    void transferProgress(quint64 requestId, quint64 completed, quint64 total);

private:
    /** @brief 生成线程安全且进程内单调递增的请求编号。 */
    quint64 nextRequestId();

    /** @brief 判断用户是否已在本次运行中确认过未知主机。 */
    bool isTemporarilyTrusted(const ConnectionTarget &target) const;

    mutable QMutex m_trustMutex;
    QSet<QString> m_temporarilyTrustedHosts;
    std::atomic<quint64> m_nextRequestId{1};
};

Q_DECLARE_METATYPE(SftpService::ErrorKind)
