#pragma once

#include <QFile>
#include <QFileDevice>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QSaveFile>
#include <QString>

#include <libssh/libssh.h>

/**
 * @brief 提供 `known_hosts` 中已变更主机密钥的安全替换能力。
 *
 * libssh 的 `ssh_session_update_known_hosts()` 只会追加当前服务器密钥，不会删除冲突的旧
 * 条目。因此服务器重装或主动轮换密钥后，单纯再次写入仍会在下一次连接时得到
 * `SSH_KNOWN_HOSTS_CHANGED`。本工具先让调用方完成新指纹的人工核验，再以原子文件替换方式
 * 删除 libssh 解析后确认属于该目标的旧条目并写入当前会话导出的新条目。
 *
 * 这里刻意不提供“忽略校验”接口：调用方只有在界面明确展示新指纹、用户确认服务器确实
 * 更换过密钥后才能调用替换方法，从而保留对中间人攻击的防护边界。
 */
namespace KnownHostsManager {

namespace Detail {

/**
 * @brief 去除 known_hosts 单条记录末尾的换行符，便于比较 libssh 返回值与磁盘内容。
 * @param line 可能带有 LF 或 CRLF 结尾的一条原始记录。
 * @return 仅移除行结束符后的记录；记录内部空白保持不变。
 */
inline QByteArray withoutLineEnding(QByteArray line)
{
    while (line.endsWith('\n') || line.endsWith('\r')) {
        line.chop(1);
    }
    return line;
}

/**
 * @brief 返回 known_hosts 记录中的主机字段结束位置。
 * @param line 已去除换行符的记录。
 * @return 首段空白的下标；格式无效时返回 -1。
 *
 * 正常记录形如 `host key-type base64`。这里只定位首字段而不按任意空白整体 split，是为了
 * 后续能够原样保留 libssh 导出的密钥字段，避免无意义地改写文件格式。
 */
inline qsizetype hostFieldEnd(const QByteArray &line)
{
    for (qsizetype index = 0; index < line.size(); ++index) {
        if (line.at(index) == ' ' || line.at(index) == '\t') {
            return index;
        }
    }
    return -1;
}

/**
 * @brief 在旧记录使用 OpenSSH 哈希主机名时，把同一哈希字段复用到新记录。
 * @param oldLine libssh 命中的旧记录。
 * @param exportedLine 当前会话导出的新记录。
 * @return 可写回文件的新记录；输入格式异常时退回 libssh 导出的原始记录。
 *
 * `ssh_session_export_known_hosts_entry()` 通常导出明文主机名。如果旧文件启用了
 * `HashKnownHosts`，直接写入会泄露原本隐藏的地址。复用旧记录的 `|1|salt|hash` 首字段既
 * 保持隐私属性，又不会改变当前会话所代表的主机。普通或逗号分隔的主机字段不复用，因为
 * 把同一把新密钥绑定给旧行中的其他别名会扩大用户本次确认的授权范围。
 */
inline QByteArray preserveHashedHostField(const QByteArray &oldLine,
                                          const QByteArray &exportedLine)
{
    const qsizetype newHostEnd = hostFieldEnd(exportedLine);
    if (newHostEnd <= 0) {
        return exportedLine;
    }

    // 调用方既可能传完整旧行，也可能已在扫描阶段提取出单独的主机字段；两种输入都接受，
    // 避免“没有空白”被误判为无效记录而把哈希地址降级为明文。
    const qsizetype oldHostEnd = hostFieldEnd(oldLine);
    const QByteArray oldHostField = oldHostEnd > 0 ? oldLine.left(oldHostEnd) : oldLine;
    if (!oldHostField.startsWith("|1|")) {
        return exportedLine;
    }
    return oldHostField + exportedLine.mid(newHostEnd);
}

/**
 * @brief 在完整文件中替换属于指定主机的全部 known_hosts 记录。
 * @param original 文件原始字节，可能包含注释、空行以及 CRLF。
 * @param lookupHost libssh 使用的规范主机标识；标准端口为 host，非标准端口为 [host]:port。
 * @param newLine 当前服务器的新记录。
 * @param replaced 是否至少命中并替换了一条目标记录。
 * @return 替换后的完整文件内容；未命中时原样返回。
 *
 * `ssh_session_get_known_hosts_entry()` 在 CHANGED/OTHER 状态下不保证返回冲突条目，而且
 * `ssh_knownhosts_entry::unparsed` 实际只保存主机字段，并非完整原始行。因此这里不能依赖
 * “旧行字符串”。逐行交给 `ssh_known_hosts_parse_line()` 匹配，可正确识别 OpenSSH 哈希主机
 * 名、非标准端口和逗号分隔别名，同时不会误删注释中恰好出现的地址。目标的不同密钥算法
 * 记录会一起移除，避免残留旧算法导致后续协商再次进入 OTHER 状态。
 */
inline QByteArray replaceHostEntries(const QByteArray &original,
                                     const QByteArray &lookupHost,
                                     const QByteArray &newLine,
                                     bool *replaced)
{
    Q_ASSERT(replaced != nullptr);
    *replaced = false;

    QByteArray rewritten;
    rewritten.reserve(original.size() + newLine.size() + 1);
    QByteArray matchedHostField;

    qsizetype lineStart = 0;
    while (lineStart < original.size()) {
        const qsizetype newline = original.indexOf('\n', lineStart);
        const qsizetype lineEnd = newline < 0 ? original.size() : newline;
        const qsizetype segmentEnd = newline < 0 ? original.size() : newline + 1;
        const QByteArray segment = original.mid(lineStart, segmentEnd - lineStart);
        const QByteArray comparable = withoutLineEnding(
            original.mid(lineStart, lineEnd - lineStart));
        // 部分 libssh 版本在解析过程中会复用输入缓冲。先复制磁盘中的原始主机字段，避免
        // 哈希字段在匹配成功后已被规范化为明文，进而失去原本的地址隐藏属性。
        const qsizetype rawHostEnd = hostFieldEnd(comparable);
        const QByteArray rawHostField = rawHostEnd > 0
            ? comparable.left(rawHostEnd)
            : QByteArray{};

        struct ssh_knownhosts_entry *parsedEntry = nullptr;
        const int parseResult = ssh_known_hosts_parse_line(
            lookupHost.constData(), comparable.constData(), &parsedEntry);
        if (parseResult == SSH_OK && parsedEntry != nullptr) {
            *replaced = true;
            // libssh 为完成哈希匹配会把 entry->unparsed 规范化为明文主机，不能从结构体
            // 回取旧字段；必须直接保留磁盘原行的首字段，才能维持 |1|salt|HMAC 形式。
            if (matchedHostField.isEmpty() && !rawHostField.isEmpty()) {
                matchedHostField = rawHostField;
            }
        } else {
            // SSH_AGAIN 表示该行格式有效但不属于目标；SSH_ERROR 表示注释、空行、标记行或
            // 损坏记录。两者都必须原样保留，替换操作不能顺便“清理”用户的其他内容。
            rewritten.append(segment);
        }
        SSH_KNOWNHOSTS_ENTRY_FREE(parsedEntry);
        lineStart = segmentEnd;
    }

    if (!*replaced) {
        return original;
    }
    const QByteArray normalizedNew = preserveHashedHostField(
        matchedHostField, withoutLineEnding(newLine));
    // 新记录统一从完整的新行开始。若前一条记录没有换行，先补 LF，防止两条粘连失效。
    if (!rewritten.isEmpty() && !rewritten.endsWith('\n')) {
        rewritten.append('\n');
    }
    rewritten.append(normalizedNew);
    rewritten.append('\n');
    return rewritten;
}

} // namespace Detail

/**
 * @brief 用当前已连接 SSH 会话的公钥替换 `known_hosts` 中冲突的旧记录。
 * @param session 已完成 SSH 握手、且校验结果为 CHANGED 或 OTHER 的 libssh 会话。
 * @param errorMessage 失败时接收可直接展示的中文诊断；可为空。
 * @return 原子替换完成且 libssh 重新校验为 OK 时返回 true。
 *
 * 用户级 known_hosts 文件可写，系统级全局文件是只读信任源。如果冲突只存在于全局文件，
 * 本方法不会尝试提权或绕过它，而是返回明确错误，让管理员修正系统配置。写入后还会立即用
 * 当前会话重新校验；若校验未通过，则尽力原子恢复原文件，避免留下半完成的信任状态。
 */
inline bool replaceChangedHostKey(ssh_session session, QString *errorMessage = nullptr)
{
    if (session == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QObject::tr("The SSH session is invalid; cannot update known_hosts.");
        }
        return false;
    }

    const ssh_known_hosts_e state = ssh_session_is_known_server(session);
    if (state != SSH_KNOWN_HOSTS_CHANGED && state != SSH_KNOWN_HOSTS_OTHER) {
        if (errorMessage != nullptr) {
            *errorMessage = QObject::tr("The current SSH session is no longer in a host key conflict state.");
        }
        return false;
    }

    char *knownHostsPathRaw = nullptr;
    if (ssh_options_get(session, SSH_OPTIONS_KNOWNHOSTS, &knownHostsPathRaw) != SSH_OK
        || knownHostsPathRaw == nullptr) {
        if (knownHostsPathRaw != nullptr) {
            ssh_string_free_char(knownHostsPathRaw);
        }
        if (errorMessage != nullptr) {
            *errorMessage = QObject::tr("Could not determine the location of your known_hosts file.");
        }
        return false;
    }
    const QString knownHostsPath = QFile::decodeName(knownHostsPathRaw);
    ssh_string_free_char(knownHostsPathRaw);

    char *exportedEntryRaw = nullptr;
    if (ssh_session_export_known_hosts_entry(session, &exportedEntryRaw) != SSH_OK
        || exportedEntryRaw == nullptr) {
        if (exportedEntryRaw != nullptr) {
            ssh_string_free_char(exportedEntryRaw);
        }
        if (errorMessage != nullptr) {
            *errorMessage = QObject::tr("Could not export the server's new host key: %1")
                                .arg(QString::fromUtf8(ssh_get_error(session)));
        }
        return false;
    }
    const QByteArray newLine(exportedEntryRaw);
    ssh_string_free_char(exportedEntryRaw);

    const QByteArray normalizedNewLine = Detail::withoutLineEnding(newLine);
    const qsizetype exportedHostEnd = Detail::hostFieldEnd(normalizedNewLine);
    if (exportedHostEnd <= 0) {
        if (errorMessage != nullptr) {
            *errorMessage = QObject::tr("The new host key record exported by libssh has an invalid format.");
        }
        return false;
    }
    // 导出记录的首字段来自 libssh 自身的 ssh_session_get_host_port()，与其读取
    // known_hosts 时使用的标准端口/非标准端口规则完全一致，可直接用于逐行哈希匹配。
    const QByteArray lookupHost = normalizedNewLine.left(exportedHostEnd);

    // 同一进程的终端与 SFTP 可能同时确认同一服务器。串行化读改写，避免两次原子保存互相
    // 覆盖；跨进程竞争仍由“旧记录必须精确存在”的前置条件阻止盲目写入。
    static QMutex replacementMutex;
    QMutexLocker replacementLock(&replacementMutex);

    QFile input(knownHostsPath);
    if (!input.open(QIODevice::ReadOnly)) {
        if (errorMessage != nullptr) {
            *errorMessage = QObject::tr("Could not read %1: %2")
                                .arg(knownHostsPath, input.errorString());
        }
        return false;
    }
    const QFileDevice::Permissions originalPermissions = input.permissions();
    const QByteArray original = input.readAll();
    if (input.error() != QFileDevice::NoError) {
        if (errorMessage != nullptr) {
            *errorMessage = QObject::tr("Failed to read %1: %2")
                                .arg(knownHostsPath, input.errorString());
        }
        return false;
    }
    input.close();

    bool replaced = false;
    const QByteArray rewritten = Detail::replaceHostEntries(
        original, lookupHost, newLine, &replaced);
    if (!replaced) {
        if (errorMessage != nullptr) {
            *errorMessage = QObject::tr(
                "The conflicting record is not in your known_hosts file %1; it may come from a system-level configuration. Contact your administrator to verify and update it.").arg(knownHostsPath);
        }
        return false;
    }

    const auto saveContent = [&](const QByteArray &content, QString *saveError) {
        QSaveFile output(knownHostsPath);
        if (!output.open(QIODevice::WriteOnly)) {
            if (saveError != nullptr) {
                *saveError = output.errorString();
            }
            return false;
        }
        // known_hosts 通常包含敏感的资产地址。继承原权限，避免原子替换后意外扩大可读范围。
        if (!output.setPermissions(originalPermissions)
            || output.write(content) != content.size()
            || !output.commit()) {
            if (saveError != nullptr) {
                *saveError = output.errorString();
            }
            return false;
        }
        return true;
    };

    QString saveError;
    if (!saveContent(rewritten, &saveError)) {
        if (errorMessage != nullptr) {
            *errorMessage = QObject::tr("Failed to update %1: %2").arg(knownHostsPath, saveError);
        }
        return false;
    }

    if (ssh_session_is_known_server(session) == SSH_KNOWN_HOSTS_OK) {
        return true;
    }

    QString rollbackError;
    const bool rolledBack = saveContent(original, &rollbackError);
    if (errorMessage != nullptr) {
        *errorMessage = rolledBack
            ? QObject::tr("Re-check after writing the new host key failed; the original known_hosts file has been restored.")
            : QObject::tr("Re-check after writing the new host key failed, and restoring the original file also failed: %1")
                  .arg(rollbackError);
    }
    return false;
}

} // namespace KnownHostsManager
