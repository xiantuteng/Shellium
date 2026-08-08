#pragma once

#include "core/ConnectionTarget.h"

#include <QDateTime>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <atomic>
#include <memory>

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
 * @brief 递归传输的纯逻辑：路径规范化、越界防护、传输计划与结果汇总。
 *
 * 这些函数刻意不接触 libssh、文件系统和 Qt 界面，只做数据到数据的变换。递归传输里最容易
 * 出错、后果也最严重的部分（远端文件名把写入位置带到目标目录之外、符号链接导致无限递归、
 * 目录顺序错误导致 mkdir 失败）因此可以在没有真实 SFTP 服务器的情况下被单元测试覆盖。
 */
namespace SftpRecursive {

/**
 * @brief 遍历源端（本地或远端）得到的一个条目，路径相对于传输根目录。
 *
 * 统一用 POSIX 风格的相对路径描述两个方向，使上传与下载共用同一套计划构建与安全校验。
 */
struct SourceEntry {
    QString relativePath;
    quint64 size = 0;
    bool isDirectory = false;
    bool isSymbolicLink = false;
};

/** @brief 传输计划中的一项：要么建目录，要么传一个文件。 */
struct TransferItem {
    QString sourcePath;
    QString destinationPath;
    quint64 size = 0;
    bool isDirectory = false;
};

/** @brief 被主动跳过的条目及原因，最终需要如实告知用户。 */
struct SkippedEntry {
    QString path;
    QString reason;
};

/** @brief 传输失败的单个条目；单点失败不会中断整个目录传输。 */
struct FailedEntry {
    QString path;
    QString message;
};

/**
 * @brief 一次递归传输的完整计划。
 *
 * items 已排好序：任一目录一定排在它的内容之前，这样上传时 mkdir、下载时 mkpath 都能保证
 * 父目录已经存在，不需要在传输循环里反复回溯建目录。
 */
struct TransferPlan {
    QList<TransferItem> items;
    QList<SkippedEntry> skipped;
    quint64 totalBytes = 0;
    quint64 fileCount = 0;
    quint64 directoryCount = 0;
};

/** @brief 递归传输结束后的汇总，用于一次性向用户报告成功、跳过与失败。 */
struct TransferSummary {
    QString rootDescription;
    quint64 filesTransferred = 0;
    quint64 bytesTransferred = 0;
    quint64 fileTotal = 0;
    QList<SkippedEntry> skipped;
    QList<FailedEntry> failed;
    bool cancelled = false;

    /**
     * @brief 汇总成一条可直接展示的中文消息，包含跳过与失败清单。
     *
     * 跳过和失败最多各列出 kMaxListedEntries 条：递归传输失败往往是成片发生的（整棵子树
     * 没有权限），把上千行原样倒给用户等于什么也没说，反而会淹没真正需要看的第一条原因。
     */
    [[nodiscard]] QString toMessage() const
    {
        constexpr qsizetype kMaxListedEntries = 10;
        QStringList lines;
        if (cancelled) {
            lines.append(QObject::tr("传输已取消：%1").arg(rootDescription));
        } else {
            lines.append(QObject::tr("已传输 %1：%2/%3 个文件，共 %4 字节")
                             .arg(rootDescription)
                             .arg(filesTransferred)
                             .arg(fileTotal)
                             .arg(bytesTransferred));
        }
        const auto appendList = [&lines](const QString &title, const QStringList &details) {
            if (details.isEmpty()) {
                return;
            }
            lines.append(title.arg(details.size()));
            for (qsizetype index = 0; index < details.size() && index < kMaxListedEntries; ++index) {
                lines.append(QStringLiteral("  · ") + details.at(index));
            }
            if (details.size() > kMaxListedEntries) {
                lines.append(QObject::tr("  · 其余 %1 项省略")
                                 .arg(details.size() - kMaxListedEntries));
            }
        };

        QStringList skippedDetails;
        skippedDetails.reserve(skipped.size());
        for (const SkippedEntry &entry : skipped) {
            skippedDetails.append(QStringLiteral("%1（%2）").arg(entry.path, entry.reason));
        }
        QStringList failedDetails;
        failedDetails.reserve(failed.size());
        for (const FailedEntry &entry : failed) {
            failedDetails.append(QStringLiteral("%1：%2").arg(entry.path, entry.message));
        }
        appendList(QObject::tr("已跳过 %1 项："), skippedDetails);
        appendList(QObject::tr("失败 %1 项："), failedDetails);
        return lines.join(QLatin1Char('\n'));
    }

    /** @brief 是否存在需要用户注意的内容（被取消、有跳过或有失败）。 */
    [[nodiscard]] bool hasWarnings() const
    {
        return cancelled || !skipped.isEmpty() || !failed.isEmpty();
    }
};

/** @brief 按 POSIX 规则拼接远端路径，不使用本机平台相关的分隔符。 */
[[nodiscard]] inline QString joinRemotePath(const QString &directory, const QString &name)
{
    if (name.isEmpty()) {
        return directory;
    }
    if (directory.isEmpty()) {
        return name;
    }
    return directory.endsWith(QLatin1Char('/'))
        ? directory + name
        : directory + QLatin1Char('/') + name;
}

/**
 * @brief 纯词法规范化 POSIX 路径：折叠重复分隔符，消解 "." 与 ".."。
 *
 * 刻意不查询文件系统，也不解析符号链接：递归传输要防的是"远端返回的名字把写入位置带到
 * 目标目录之外"，这属于路径拼接层面的问题，必须在发起任何 I/O **之前**就判定。词法消解
 * 之所以够用，是因为传输过程本身从不跟随符号链接，路径中不会出现"经由链接改变含义"的段。
 */
[[nodiscard]] inline QString normalizePosixPath(const QString &path)
{
    const bool absolute = path.startsWith(QLatin1Char('/'));
    QStringList resolved;
    const QStringList segments = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &segment : segments) {
        if (segment == QStringLiteral(".")) {
            continue;
        }
        if (segment == QStringLiteral("..")) {
            if (!resolved.isEmpty() && resolved.constLast() != QStringLiteral("..")) {
                resolved.removeLast();
            } else if (!absolute) {
                // 相对路径的前导 ".." 必须保留，否则 "../x" 会被悄悄变成 "x" 而通过越界检查。
                resolved.append(segment);
            }
            // 绝对路径在根目录上的 ".." 按 POSIX 语义等于根目录本身，直接丢弃。
            continue;
        }
        resolved.append(segment);
    }
    const QString joined = resolved.join(QLatin1Char('/'));
    if (absolute) {
        return QLatin1Char('/') + joined;
    }
    return joined.isEmpty() ? QStringLiteral(".") : joined;
}

/** @brief 判断规范化后的 candidate 是否仍位于 root 之内（root 自身视为在内）。 */
[[nodiscard]] inline bool isWithinRoot(const QString &root, const QString &candidate)
{
    const QString normalizedRoot = normalizePosixPath(root);
    const QString normalizedCandidate = normalizePosixPath(candidate);
    if (normalizedCandidate == normalizedRoot) {
        return true;
    }
    const QString prefix = normalizedRoot.endsWith(QLatin1Char('/'))
        ? normalizedRoot
        : normalizedRoot + QLatin1Char('/');
    // 必须比到分隔符为止：否则 /home/user 会把 /home/user-evil 也判成"在内"。
    return normalizedCandidate.startsWith(prefix);
}

/**
 * @brief 把相对路径安全地接到根目录上。
 * @return 拼接后的规范化绝对路径；相对路径会逃逸出根目录时返回空字符串。
 */
[[nodiscard]] inline QString resolveWithinRoot(const QString &root, const QString &relativePath)
{
    if (relativePath.startsWith(QLatin1Char('/'))) {
        // 遍历结果本应是相对路径。出现绝对路径说明来源不可信，直接拒绝而不是试图修正。
        return {};
    }
    const QString candidate = normalizePosixPath(joinRemotePath(root, relativePath));
    return isWithinRoot(root, candidate) ? candidate : QString{};
}

/**
 * @brief 由遍历结果构建有序传输计划，并挑出必须跳过的条目。
 * @param sourceRoot 源端根目录（上传时为本地目录，下载时为远端目录）。
 * @param destinationRoot 目标端根目录。
 *
 * 符号链接一律跳过而不跟随：跟随会在环形链接上无限递归，更严重的是远端一条指向
 * /etc 或 ../../ 的链接会让下载把文件写到用户指定目录之外。跳过的条目会进入 skipped，
 * 由界面如实告知用户，而不是静默丢弃。
 *
 * 结果按"目录深度优先、同层目录先于文件"排序，保证任一目录都排在它的内容之前：传输循环
 * 因此可以顺序执行，不必在写每个文件前回溯创建父目录。
 */
[[nodiscard]] inline TransferPlan buildTransferPlan(const QString &sourceRoot,
                                                    const QString &destinationRoot,
                                                    const QList<SourceEntry> &entries)
{
    TransferPlan plan;
    QList<SourceEntry> accepted;
    accepted.reserve(entries.size());

    for (const SourceEntry &entry : entries) {
        if (entry.isSymbolicLink) {
            plan.skipped.append({entry.relativePath, QObject::tr("符号链接（不跟随）")});
            continue;
        }
        if (entry.relativePath.isEmpty() || entry.relativePath == QStringLiteral(".")) {
            continue;
        }
        if (resolveWithinRoot(destinationRoot, entry.relativePath).isEmpty()) {
            plan.skipped.append({entry.relativePath, QObject::tr("路径逃逸出目标目录")});
            continue;
        }
        accepted.append(entry);
    }

    std::stable_sort(accepted.begin(), accepted.end(),
                     [](const SourceEntry &left, const SourceEntry &right) {
                         const QString leftParent = left.relativePath.section(QLatin1Char('/'), 0, -2);
                         const QString rightParent = right.relativePath.section(QLatin1Char('/'), 0, -2);
                         if (leftParent != rightParent) {
                             // 父目录路径的字典序天然让 "a" 排在 "a/b" 之前，从而保证先建目录。
                             return leftParent < rightParent;
                         }
                         if (left.isDirectory != right.isDirectory) {
                             return left.isDirectory;
                         }
                         return left.relativePath < right.relativePath;
                     });

    for (const SourceEntry &entry : accepted) {
        TransferItem item;
        item.sourcePath = joinRemotePath(sourceRoot, entry.relativePath);
        item.destinationPath = resolveWithinRoot(destinationRoot, entry.relativePath);
        item.isDirectory = entry.isDirectory;
        item.size = entry.isDirectory ? 0 : entry.size;
        plan.items.append(item);
        if (entry.isDirectory) {
            ++plan.directoryCount;
        } else {
            ++plan.fileCount;
            plan.totalBytes += entry.size;
        }
    }
    return plan;
}

/**
 * @brief 计算整体进度百分比。
 * @return 0 到 100 的百分比；total 为 0（大小未知或空目录）时返回 -1 表示不确定。
 */
[[nodiscard]] inline int progressPercent(const quint64 completed, const quint64 total)
{
    if (total == 0) {
        return -1;
    }
    const quint64 bounded = completed > total ? total : completed;
    return static_cast<int>((bounded * 100) / total);
}

} // namespace SftpRecursive

Q_DECLARE_METATYPE(SftpRecursive::TransferSummary)

/**
 * @brief 远程文件管理器的纯界面状态选择逻辑。
 *
 * 把“连接链刷新后应选哪一层”从 QWidget 中抽离，避免用户手动选择外层后被无关刷新抢回
 * 最内层。该逻辑不依赖网络和控件，可由单元测试覆盖每标签状态隔离所需的边界条件。
 */
namespace SftpUiState {

/**
 * @brief 计算服务器链刷新后的选中下标。
 * @param stableKeys 新连接链中从外到内排列的稳定键。
 * @param previousKey 刷新前用户选中服务器的稳定键。
 * @param previousIndex 刷新前下标，仅在原目标已消失时用于选择最接近的剩余层。
 * @param followInnermost true 时始终选择最新最内层；false 时优先保留手工选择。
 * @return 新链中的下标；列表为空时返回 -1。
 */
[[nodiscard]] inline int selectTargetIndex(const QStringList &stableKeys,
                                           const QString &previousKey,
                                           const int previousIndex,
                                           const bool followInnermost)
{
    if (stableKeys.isEmpty()) {
        return -1;
    }
    if (followInnermost) {
        return static_cast<int>(stableKeys.size() - 1);
    }

    const qsizetype preservedIndex = stableKeys.indexOf(previousKey);
    if (preservedIndex >= 0) {
        return static_cast<int>(preservedIndex);
    }

    // 手选层已经退出时选择与原位置最接近的剩余层，而不是返回 -1 留下一个无法操作的
    // 文件管理器；向内层收缩时通常会自然落回新的最内层。
    return std::clamp(previousIndex, 0, static_cast<int>(stableKeys.size() - 1));
}

} // namespace SftpUiState

/**
 * @brief 单文件和递归传输共用的续传决策与安全上限。
 *
 * 该命名空间只处理大小与策略，不访问文件系统或网络，便于在没有真实 SFTP 服务器时验证
 * “续传、已完成、目标异常偏大、强制覆盖”等关键边界。
 */
namespace SftpTransfer {

/** @brief 目标已存在时的处理策略。 */
enum class ExistingFilePolicy {
    ResumeWhenPossible,
    Overwrite
};

/** @brief 根据源和目标大小计算出的续传动作。 */
struct ResumePlan {
    /** @brief 源端和目标端都应跳转到的字节偏移。 */
    quint64 offset = 0;

    /** @brief 打开目标时是否必须截断，防止异常偏大的旧文件残留尾部。 */
    bool truncateDestination = true;

    /** @brief 两端大小已完全一致，可直接视为完成而无需发起读写。 */
    bool alreadyComplete = false;
};

/**
 * @brief 依据源文件大小、目标已有大小和用户策略制定续传动作。
 * @param sourceSize 完整源文件大小。
 * @param destinationSize 目标文件当前大小；不存在时传 0。
 * @param policy 用户选择的续传或覆盖策略。
 * @return 安全偏移、是否截断以及是否已完成。
 */
[[nodiscard]] inline ResumePlan makeResumePlan(
    const quint64 sourceSize,
    const quint64 destinationSize,
    const ExistingFilePolicy policy)
{
    if (policy == ExistingFilePolicy::Overwrite || destinationSize > sourceSize) {
        // 目标比源更大时绝不能从源末尾继续写，否则旧尾部会保留并生成损坏文件。
        return {.offset = 0, .truncateDestination = true, .alreadyComplete = false};
    }
    if (destinationSize == sourceSize) {
        return {.offset = sourceSize,
                .truncateDestination = false,
                .alreadyComplete = true};
    }
    return {.offset = destinationSize,
            .truncateDestination = false,
            .alreadyComplete = false};
}

/** @brief 安全预览允许读取的硬上限，调用方传入更大值也会被压到该范围。 */
inline constexpr quint64 MaximumPreviewBytes = 2ULL * 1024ULL * 1024ULL;

/** @brief 把预览请求限制在 1 字节到 2 MiB，避免零值或超大值绕过内存保护。 */
[[nodiscard]] inline quint64 boundedPreviewLimit(const quint64 requestedBytes)
{
    return std::clamp<quint64>(requestedBytes,
                               static_cast<quint64>(1),
                               MaximumPreviewBytes);
}

} // namespace SftpTransfer

/**
 * @brief 在后台线程中执行 libssh SFTP 操作，并把结果安全送回主线程。
 *
 * 连接按目标池化：同一服务器的操作复用一条已认证连接，不同目标仍并行，
 * 且某个服务器断线不会污染其他请求。服务不会持久化密码，日志和错误消息中也不会包含密码。
 */
struct SftpConnectionPool;

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

    /** @brief 关闭全部池化连接；必须在 .cpp 中定义，连接池是不完整类型。 */
    ~SftpService() override;

    /**
     * @brief 异步列出远程目录，不阻塞 Qt 界面线程。
     * @param path 远端绝对路径；**传空字符串表示该账号的默认主目录**，由服务端解析。
     *
     * 首次进入一台服务器时无从得知它的主目录，写死 `/` 既不符合直觉，也常常没有权限。
     * 解析结果通过 directoryListed 的 path 参数返回，调用方据此更新路径框。
     */
    quint64 listDirectory(const ConnectionTarget &target, const QString &path);

    /** @brief 异步上传单个本地文件到指定远端完整路径。 */
    quint64 uploadFile(const ConnectionTarget &target,
                       const QString &localPath,
                       const QString &remotePath,
                       SftpTransfer::ExistingFilePolicy policy =
                           SftpTransfer::ExistingFilePolicy::ResumeWhenPossible);

    /** @brief 异步下载单个远端文件到指定本地完整路径。 */
    quint64 downloadFile(const ConnectionTarget &target,
                         const QString &remotePath,
                         const QString &localPath,
                         SftpTransfer::ExistingFilePolicy policy =
                             SftpTransfer::ExistingFilePolicy::ResumeWhenPossible);

    /**
     * @brief 异步递归上传整个本地目录。
     * @param localDirectory 要上传的本地目录。
     * @param remoteParentDirectory 远端父目录；目录本身会以同名在其下创建。
     *
     * 遍历过程不跟随符号链接，单个文件失败不会中断整棵目录树，结果统一由
     * recursiveTransferFinished 汇总返回。
     */
    quint64 uploadDirectory(const ConnectionTarget &target,
                            const QString &localDirectory,
                            const QString &remoteParentDirectory,
                            SftpTransfer::ExistingFilePolicy policy =
                                SftpTransfer::ExistingFilePolicy::ResumeWhenPossible);

    /**
     * @brief 异步递归下载整个远端目录。
     * @param remoteDirectory 要下载的远端目录。
     * @param localParentDirectory 本地父目录；目录本身会以同名在其下创建。
     *
     * 远端返回的名字一律经过越界校验后才落盘，指向目标目录之外的条目会被跳过并上报。
     */
    quint64 downloadDirectory(const ConnectionTarget &target,
                              const QString &remoteDirectory,
                              const QString &localParentDirectory,
                              SftpTransfer::ExistingFilePolicy policy =
                                  SftpTransfer::ExistingFilePolicy::ResumeWhenPossible);

    /**
     * @brief 请求取消一个仍在进行的递归传输。
     *
     * 只置标志，由后台任务在下一个分块边界自行停止：强杀线程会留下半写的远端文件和未释放
     * 的 libssh 句柄。已经传完的文件保留在目标端，汇总里会说明传输被中断。
     */
    void cancelRequest(quint64 requestId);

    /**
     * @brief 暂停传输请求。
     *
     * 暂停在下一次 64 KiB 分块边界生效；已经进入 libssh 的单次阻塞读写无法被安全打断。
     */
    void pauseRequest(quint64 requestId);

    /** @brief 恢复被暂停的传输请求，并唤醒等待中的后台任务。 */
    void resumeRequest(quint64 requestId);

    /**
     * @brief 异步读取远程文件的安全预览片段。
     * @param maximumBytes 请求读取上限，服务端还会强制限制在 2 MiB 内。
     *
     * 只返回原始字节，不写入磁盘、不启动外部程序、不解释脚本；界面只能以只读文本或图片
     * 解码方式展示。
     */
    quint64 previewFile(const ConnectionTarget &target,
                        const QString &remotePath,
                        quint64 maximumBytes = SftpTransfer::MaximumPreviewBytes);

    /** @brief 异步修改远程文件或目录的 POSIX 权限位。 */
    quint64 chmodPath(const ConnectionTarget &target,
                      const QString &remotePath,
                      quint32 permissions);

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

    /**
     * @brief 递归传输的条目级进度。
     *
     * transferProgress 只按字节走，一个 4 GiB 大文件和四千个小文件在进度条上无法区分，用户
     * 也看不出卡在哪一项。这里额外给出"第几个 / 共几个"和当前条目名，两个信号并行使用。
     */
    void recursiveTransferProgress(quint64 requestId,
                                   quint64 filesCompleted,
                                   quint64 filesTotal,
                                   const QString &currentItem);

    /** @brief 递归传输结束（含被取消与部分失败），携带完整汇总。 */
    void recursiveTransferFinished(quint64 requestId,
                                   const SftpRecursive::TransferSummary &summary);

    /**
     * @brief 远程文件预览读取成功。
     * @param data 不超过安全上限的原始内容。
     * @param mimeType 根据文件名和内容推断的 MIME 类型，仅用于选择只读展示控件。
     * @param truncated 文件是否大于读取上限；true 时界面必须明确提示内容不完整。
     */
    void filePreviewReady(quint64 requestId,
                          const ConnectionTarget &target,
                          const QString &remotePath,
                          const QByteArray &data,
                          const QString &mimeType,
                          bool truncated);

private:
    /** @brief 生成线程安全且进程内单调递增的请求编号。 */
    quint64 nextRequestId();

    /** @brief 判断用户是否已在本次运行中确认过未知主机。 */
    bool isTemporarilyTrusted(const ConnectionTarget &target) const;

    /**
     * @brief 按目标缓存的 SFTP 连接池。
     *
     * 用 shared_ptr 是为了让仍在执行的后台任务安全地持有它：请求提交后服务本身可能先被
     * 销毁，而任务还需要用连接完成收尾。
     */
    std::shared_ptr<SftpConnectionPool> m_pool;

    mutable QMutex m_trustMutex;
    QSet<QString> m_temporarilyTrustedHosts;
    std::atomic<quint64> m_nextRequestId{1};
};

Q_DECLARE_METATYPE(SftpService::ErrorKind)
