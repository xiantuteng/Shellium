#pragma once

#include "core/ConnectionTarget.h"
#include "sftp/SftpResumeLedger.h"

#include <QDateTime>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>

/** @brief 远端 OpenSSH SFTP 中继的纯命令构造逻辑，独立暴露以便安全回归测试。 */
namespace SftpRemoteRelay {

/**
 * @brief 构造在外层服务器上启动内层 SFTP 子系统的安全命令行。
 * @param target 内层服务器，使用其中的用户名、地址与端口。
 * @return 参数均经过 POSIX Shell 单 token 转义的 `command ssh ... -s ... sftp` 命令。
 */
[[nodiscard]] QByteArray buildCommand(const ConnectionTarget &target);

} // namespace SftpRemoteRelay

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

    /**
     * @brief 权限的 `ls -l` 符号形式，例如 `drwxr-xr-x`。
     *
     * 八进制看起来紧凑，但要判断"同组的人能不能写"得先在脑子里做一次进制换算；符号形式是
     * 所有 Unix 工具的通用写法，一眼就能读。首字符取自条目类型，与远端 ls 完全一致。
     *
     * setuid/setgid/sticky 位按惯例覆盖在对应的 x 位上：有执行权限时写 s/s/t，没有时写
     * 大写的 S/S/T，因此不会丢失信息。
     */
    [[nodiscard]] QString permissionText() const
    {
        QString text;
        text.reserve(10);
        // 类型位由服务器的 SSH_FILEXFER_TYPE_* 提供，比从 mode 里猜 S_IFMT 可靠：部分
        // 服务器在 permissions 中根本不带类型位。
        text.append(isSymbolicLink ? QLatin1Char('l')
                                   : (isDirectory ? QLatin1Char('d') : QLatin1Char('-')));

        struct Triplet {
            quint32 readBit;
            quint32 writeBit;
            quint32 executeBit;
            quint32 specialBit;
            char specialChar;
        };
        static constexpr Triplet triplets[] = {
            {0400U, 0200U, 0100U, 04000U, 's'},
            {0040U, 0020U, 0010U, 02000U, 's'},
            {0004U, 0002U, 0001U, 01000U, 't'},
        };
        for (const Triplet &triplet : triplets) {
            text.append((permissions & triplet.readBit) != 0U ? QLatin1Char('r')
                                                              : QLatin1Char('-'));
            text.append((permissions & triplet.writeBit) != 0U ? QLatin1Char('w')
                                                               : QLatin1Char('-'));
            const bool executable = (permissions & triplet.executeBit) != 0U;
            if ((permissions & triplet.specialBit) != 0U) {
                const QChar special = QLatin1Char(triplet.specialChar);
                text.append(executable ? special : special.toUpper());
            } else {
                text.append(executable ? QLatin1Char('x') : QLatin1Char('-'));
            }
        }
        return text;
    }
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

    /**
     * @brief 已证明两端内容一致而未重传的文件数。
     *
     * 必须与"已传输"分开计数：把跳过的文件算进传输数，用户看到的就是一次干净的成功，
     * 从而误以为远端已经更新。这两个数字合起来才等于处理过的文件总数。
     */
    quint64 filesSkippedIdentical = 0;

    /** @brief 因选择"替换"而被删除的目标端多余条目数。 */
    quint64 removedForReplace = 0;
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
            QString headline = QObject::tr("已传输 %1：%2/%3 个文件，共 %4 字节")
                                   .arg(rootDescription)
                                   .arg(filesTransferred)
                                   .arg(fileTotal)
                                   .arg(bytesTransferred);
            if (filesSkippedIdentical > 0) {
                // 必须并进首行：界面只把第一行放进状态栏，"0/5 个文件"单独出现会让用户以为
                // 传输失败了，而真实情况是那 5 个文件本来就一模一样。
                headline += QObject::tr("；另有 %1 个文件内容一致，未重传")
                                .arg(filesSkippedIdentical);
            }
            lines.append(headline);
        }
        if (removedForReplace > 0) {
            lines.append(QObject::tr("替换模式下已删除目标端 %1 个多余条目")
                             .arg(removedForReplace));
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

/**
 * @brief 判断一次终端 cwd 上报是否应当应用到文件管理器的当前路径框。
 * @param followEnabled 用户是否已开启"跟随终端目录"。
 * @param eventStableKey 上报该 cwd 的服务器稳定键。
 * @param selectedStableKey 文件管理器当前选中服务器的稳定键。
 * @param workingDirectory 上报的绝对路径。
 *
 * 关闭跟随、路径为空、或上报来自当前未选中的服务器（比如用户正浏览外层，cwd 事件却来自
 * 内层）时都不应用，否则会在用户没有察觉的情况下把路径框跳到另一台服务器。
 */
[[nodiscard]] inline bool shouldFollowTerminalWorkingDirectory(const bool followEnabled,
                                                                const QString &eventStableKey,
                                                                const QString &selectedStableKey,
                                                                const QString &workingDirectory)
{
    return followEnabled && !workingDirectory.isEmpty() && !selectedStableKey.isEmpty()
        && eventStableKey == selectedStableKey;
}

/**
 * @brief 远程文件列表的排序状态。
 *
 * column 为 -1 表示"还原默认"，即保持服务器返回的原始顺序。必须显式区分它与"按第 0 列
 * 升序"，否则三态循环走到第三步时无法表达。
 */
struct SortState {
    int column = -1;
    Qt::SortOrder order = Qt::AscendingOrder;

    [[nodiscard]] bool operator==(const SortState &other) const
    {
        // column 为 -1 时 order 无意义，比较它会让两个等价的"默认状态"被判为不等。
        return column == other.column && (column < 0 || order == other.order);
    }
};

/**
 * @brief 计算点击某个列头之后的新排序状态。
 * @param current 当前排序状态。
 * @param clickedColumn 被点击的列。
 * @return 同一列上按 升序 → 降序 → 默认 循环；点击其他列则从该列升序重新开始。
 */
[[nodiscard]] inline SortState nextSortState(const SortState &current, const int clickedColumn)
{
    if (current.column != clickedColumn) {
        return {clickedColumn, Qt::AscendingOrder};
    }
    if (current.order == Qt::AscendingOrder) {
        return {clickedColumn, Qt::DescendingOrder};
    }
    return {};
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

/** @brief 目录传输遇到同名远端目录时的处理方式，由用户在传输前明确选择。 */
enum class DirectoryConflictPolicy {
    /** @brief 合并更新：保留目标端已有条目，逐个文件按内容判定是否需要重传。 */
    Merge,
    /** @brief 替换：先递归删除目标端同名目录，再完整写入，结果与源端严格一致。 */
    Replace
};

/** @brief 对单个文件最终采取的动作。 */
enum class TransferAction {
    /** @brief 从头传输并截断目标，任何存疑情况都落到这里。 */
    FullTransfer,
    /** @brief 从已验证的偏移继续写入残片。 */
    Resume,
    /** @brief 已证明两端内容一致，不产生任何字节传输。 */
    SkipIdentical
};

/** @brief 源文件的身份快照；contentHash 为空表示这一侧算不出完整哈希。 */
struct SourceSnapshot {
    quint64 size = 0;

    /** @brief 秒级修改时间，可与目标端（SFTP 只有秒精度）直接比较。 */
    qint64 mtime = -1;

    /**
     * @brief 毫秒精度的修改时间，只与本地账本里的记录比较。
     *
     * 同一秒内把文件改成另一份等长内容时，秒数和大小都不变；只有毫秒精度能分辨出这是两次
     * 不同的写入。远端源拿不到毫秒精度时填 mtime * 1000。
     */
    qint64 mtimeMs = -1;

    QByteArray contentHash;
};

/** @brief 目标端**正式文件**的身份快照，用于判断是否可以整体跳过。 */
struct DestinationSnapshot {
    bool exists = false;
    quint64 size = 0;
    qint64 mtime = -1;
    QByteArray contentHash;
};

/** @brief 目标端残片（.part）的现状；续传只可能发生在残片上。 */
struct PartialSnapshot {
    bool exists = false;
    quint64 size = 0;
};

/** @brief 判定结果，reason 会如实呈现给用户，便于分辨"跳过"到底基于什么证据。 */
struct TransferDecision {
    TransferAction action = TransferAction::FullTransfer;
    quint64 resumeOffset = 0;
    QString reason;
};

/**
 * @brief 决定对一个文件采取全量传输、续传还是跳过。
 * @param source 源文件身份快照。
 * @param destination 目标端正式文件快照；不存在时 exists 为 false。
 * @param partial 目标端残片快照；不存在时 exists 为 false。
 * @param ledger 本地账本中与该目的路径对应的记录：中断残片凭据，或上次同步的完成存档。
 * @param policy 用户选择的续传或覆盖策略。
 * @param mtimeTrusted 目标端是否已被实测证明能可靠保存修改时间。
 *
 * 三条不可让步的规则：
 * 1. **绝不在正式文件上续写**。续传只发生在 .part 残片上，因此"目标比源小就接着写"这种
 *    会把两个不同文件拼成一个的路径根本不存在。
 * 2. **大小相同不等于内容相同**。跳过必须拿到证据，且证据分强弱：两端完整哈希是强证据，
 *    拿得到就以它为准；拿不到才退到账本存档（源的毫秒级时间戳 + 目标端时间戳仍等于回读值
 *    + 该目标端确实能存住时间戳）。存档判据挡不住"别人用等长内容覆盖且时间戳恰好同秒"，
 *    因此绝不能用它去否决哈希给出的结论。
 * 3. **凭据必须证明残片属于当前这个源**。账本里记的源大小、哈希（或退化到修改时间）与
 *    现在的源不符，残片一律作废——源在中断期间被改过，续传只会产出"旧头 + 新尾"。
 *
 * 函数不访问文件系统与网络，全部边界都可以在没有 SFTP 服务器的情况下被单元测试覆盖。
 */
[[nodiscard]] inline TransferDecision decideTransfer(
    const SourceSnapshot &source,
    const DestinationSnapshot &destination,
    const PartialSnapshot &partial,
    const std::optional<SftpResume::LedgerRecord> &ledger,
    const ExistingFilePolicy policy,
    const bool mtimeTrusted)
{
    if (policy == ExistingFilePolicy::Overwrite) {
        return {TransferAction::FullTransfer, 0, QObject::tr("已选择覆盖重传")};
    }

    // ---- 跳过判定：只认证据，不认巧合 ----
    if (destination.exists && destination.size == source.size) {
        // 两端内容哈希都拿得到时，它是唯一权威：哈希说不一样就必须重传，绝不能再回头去看
        // 时间戳类的弱证据——那等于用一个可能被撞上的巧合去否决一个确凿的事实。
        if (!source.contentHash.isEmpty() && !destination.contentHash.isEmpty()) {
            if (source.contentHash == destination.contentHash) {
                return {TransferAction::SkipIdentical, 0, QObject::tr("内容哈希一致")};
            }
        } else {
            // 算不出哈希时（服务器没有 sha256sum/shasum）才退到存档判据：源的毫秒级时间戳与
            // 存档一致，且目标端时间戳仍等于我们当时回读到的值。
            //
            // 必须清楚这条判据的边界：它防不住"别人用一份等长内容覆盖了远端、且其修改时间
            // 恰好与我们记录的秒数相同"——两个文件在同一秒内被写出就会撞上。所以它只是没有
            // 哈希可用时的退路，不是首选。
            const bool archiveProvesIdentical = ledger.has_value()
                && ledger->kind == SftpResume::RecordKind::Complete
                && mtimeTrusted
                && ledger->sourceSize == source.size
                && source.mtimeMs >= 0 && ledger->sourceMtimeMs == source.mtimeMs
                && ledger->destinationMtime >= 0
                && ledger->destinationMtime == destination.mtime;
            if (archiveProvesIdentical) {
                return {TransferAction::SkipIdentical, 0, QObject::tr("与上次同步记录一致")};
            }
        }
    }

    // ---- 续传判定：残片 + 凭据 + 源未变，三者缺一不可 ----
    if (partial.exists && ledger.has_value()
        && ledger->kind == SftpResume::RecordKind::Partial) {
        const SftpResume::LedgerRecord &record = *ledger;
        const bool hashProvesSource = !record.sourceHash.isEmpty()
            && !source.contentHash.isEmpty()
            && record.sourceHash == source.contentHash;
        // 没有哈希可比时退到"大小 + 修改时间"，这正是下载方向遇到没有 sha256 命令的服务器
        // 时的情形；两项都必须已知且相等，未知（-1）绝不当作匹配。
        const bool quickCheckProvesSource = record.sourceHash.isEmpty()
            && record.sourceMtime >= 0 && source.mtime >= 0
            && record.sourceMtime == source.mtime;
        const bool sourceUnchanged = record.sourceSize == source.size
            && (hashProvesSource || quickCheckProvesSource);

        // 残片可以比检查点更长（最后一批写没来得及记账），但绝不能更短：更短说明有写入
        // 没落盘或残片被外部改过。续传一律从检查点开始，检查点之后的字节会被重新写一遍。
        const bool partialConsistent = record.bytesWritten > 0
            && record.bytesWritten < source.size
            && partial.size >= record.bytesWritten
            && partial.size <= source.size;

        if (sourceUnchanged && partialConsistent) {
            return {TransferAction::Resume,
                    record.bytesWritten,
                    hashProvesSource ? QObject::tr("残片已按内容哈希验证")
                                     : QObject::tr("残片已按大小与修改时间验证")};
        }
        if (!sourceUnchanged) {
            return {TransferAction::FullTransfer, 0, QObject::tr("源文件已变化，残片作废")};
        }
        return {TransferAction::FullTransfer, 0, QObject::tr("残片与续传凭据不一致，重新传输")};
    }

    return {TransferAction::FullTransfer, 0, QObject::tr("无可用续传凭据")};
}

/** @brief 残片文件名后缀；与正式文件同目录，传完后原子改名覆盖正式文件。 */
inline constexpr QLatin1String kPartialSuffix{".shellium-part"};

/**
 * @brief 由目的路径推导出同目录下的隐藏残片路径。
 * @param destinationPath 正式目标文件的完整路径（远端用 /，本地用当前平台分隔符）。
 * @param separator 路径分隔符。
 *
 * 残片必须与正式文件同目录：跨目录改名可能跨文件系统，从而失去原子性，正式文件也就可能
 * 被观察到半写状态。
 */
[[nodiscard]] inline QString partialPathFor(const QString &destinationPath,
                                            const QChar separator = QLatin1Char('/'))
{
    const qsizetype index = destinationPath.lastIndexOf(separator);
    const QString directory = index < 0 ? QString{} : destinationPath.left(index + 1);
    const QString name = index < 0 ? destinationPath : destinationPath.mid(index + 1);
    return directory + QLatin1Char('.') + name + kPartialSuffix;
}

/** @brief 校验残片尾部时读取的窗口大小；半写损伤只可能发生在尾部边界。 */
inline constexpr quint64 kTailVerificationBytes = 1ULL * 1024ULL * 1024ULL;

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
     * @param conflictPolicy 远端已存在同名目录时的处理方式，必须由用户显式选择。
     *
     * 遍历过程不跟随符号链接，单个文件失败不会中断整棵目录树，结果统一由
     * recursiveTransferFinished 汇总返回。
     *
     * Replace 会在写入前递归删除远端同名目录，因此源端已删除的文件不会残留；删除与写入
     * 在同一个请求、同一条连接上完成，中间不存在"目录已删但上传没开始"的窗口。
     */
    quint64 uploadDirectory(const ConnectionTarget &target,
                            const QString &localDirectory,
                            const QString &remoteParentDirectory,
                            SftpTransfer::ExistingFilePolicy policy =
                                SftpTransfer::ExistingFilePolicy::ResumeWhenPossible,
                            SftpTransfer::DirectoryConflictPolicy conflictPolicy =
                                SftpTransfer::DirectoryConflictPolicy::Merge);

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

    /**
     * @brief 异步探测一个远端路径是否存在及其类型。
     *
     * 目录传输在入队**之前**就必须知道远端有没有同名目录，否则只能事后才发现冲突，而那时
     * 用户已经无从选择"合并还是替换"。结果由 pathStatReady 返回。
     */
    quint64 statPath(const ConnectionTarget &target, const QString &remotePath);

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

    /**
     * @brief 改用独立路径存放续传账本。
     *
     * 供测试隔离使用：账本记录着"哪些残片可以续传"，用真实用户账本跑测试既会污染用户数据，
     * 也会让用例结果依赖上一次运行留下的状态。必须在发起任何传输之前调用。
     */
    void setResumeLedgerPath(const QString &path);

signals:
    /** @brief 目录读取成功，requestId 用于丢弃过期响应。 */
    void directoryListed(quint64 requestId,
                         const ConnectionTarget &target,
                         const QString &path,
                         const QList<RemoteFileInfo> &entries);

    /** @brief 上传、下载、重命名或删除成功。 */
    void operationSucceeded(quint64 requestId, const QString &description);

    /**
     * @brief 远端路径探测结果。
     * @param exists 路径是否存在；不存在不是错误，因此不走 operationFailed。
     * @param isDirectory 存在时是否为目录。
     */
    void pathStatReady(quint64 requestId,
                       const ConnectionTarget &target,
                       const QString &remotePath,
                       bool exists,
                       bool isDirectory);

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

    /**
     * @brief 续传凭据账本。
     *
     * 与连接池同样用 shared_ptr：后台任务在服务销毁后仍需写入最终的已传字节数，否则一次
     * 关标签页就会让整段残片失去凭据，下次只能从头再传。
     */
    std::shared_ptr<SftpResume::Ledger> m_ledger;

    mutable QMutex m_trustMutex;
    QSet<QString> m_temporarilyTrustedHosts;
    std::atomic<quint64> m_nextRequestId{1};
};

Q_DECLARE_METATYPE(SftpService::ErrorKind)
