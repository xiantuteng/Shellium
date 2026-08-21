#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>

#include <optional>

/**
 * @brief 断点续传的本地凭据账本，以及"目标端能否保存修改时间"的实测结论。
 *
 * 续传唯一能安全成立的前提是：**能证明目标端那段残片正是当前这个源文件的前缀**。仅凭
 * "目标比源小" 去续写，在源已被替换成另一个文件时会产出"旧头 + 新尾"的损坏文件，且既不
 * 报错也无从察觉。因此每次中断都要留下一份身份快照（源大小、修改时间、内容哈希、已确认
 * 写入的字节数），下次续传前逐项复核，任一不符就丢弃残片重传。
 *
 * 凭据刻意存在**本地**而不是写到远端目录：
 * - 远端隐藏文件会污染部署目录、Web 根目录和 git 工作区，还会被递归下载当成普通文件抓回；
 * - 每个文件多两次 SFTP 往返，几千个小文件的目录树会被往返时延拖垮；
 * - 下载方向的"源"在远端，本来就没法在远端便宜地算出源哈希。
 * 代价是换一台机器无法接着传——而那恰恰是最不该信任残片的场景，重传才是正确答案。
 *
 * 所有方法都可以从后台传输线程调用；内部自带互斥锁，落盘使用 QSaveFile 原子替换，因此
 * 进程被杀不会留下半个 JSON。
 */
namespace SftpResume {

/** @brief 超过该时长未更新的记录会在下次加载时清理，避免账本无限增长。 */
inline constexpr qint64 kRecordLifetimeSeconds = 14LL * 24LL * 60LL * 60LL;

/** @brief 账本记录的两种用途：中断残片的续传凭据，或一次已完成同步的存档。 */
enum class RecordKind {
    /** @brief 传输被中断，目标端留下了一段残片。 */
    Partial,
    /** @brief 传输已完成，记录我们究竟把哪一版内容写到了哪里。 */
    Complete
};

/**
 * @brief 一条同步凭据：要么证明残片属于哪个源，要么证明目标端现在是哪一版内容。
 *
 * sourceHash 为空表示这一侧算不出源的完整哈希（下载方向且远端没有 sha256 命令），此时
 * 只能退到大小 + 修改时间判据；判定函数会据此自动降级，而不是把空哈希当成"匹配"。
 */
struct LedgerRecord {
    /** @brief 记录用途。 */
    RecordKind kind = RecordKind::Partial;

    /** @brief 源路径，仅用于诊断展示，不参与身份判定。 */
    QString sourcePath;

    /** @brief 源文件的完整大小。 */
    quint64 sourceSize = 0;

    /** @brief 源文件的修改时间（Unix 秒）；-1 表示未知。 */
    qint64 sourceMtime = -1;

    /**
     * @brief 源文件修改时间的毫秒精度值；-1 表示未知。
     *
     * 秒级时间戳有一个致命盲区：同一秒内把文件改成**另一份等长内容**，秒数和大小都不变，
     * 于是"内容一致"的判定成立，而目标端根本没被更新。这在保存后立刻同步、脚本连续写文件
     * 时非常容易发生。本地源能拿到毫秒精度，用它比对就能把这个窗口关掉。
     */
    qint64 sourceMtimeMs = -1;

    /** @brief 源文件的完整内容哈希（十六进制）；为空表示无法计算。 */
    QByteArray sourceHash;

    /** @brief Partial：已确认落到 .part 上的字节数；只在检查点推进，宁可偏小不可偏大。 */
    quint64 bytesWritten = 0;

    /** @brief Complete：写完时目标文件的大小，用于判断目标端是否仍是我们写下的那一份。 */
    quint64 destinationSize = 0;

    /**
     * @brief Complete：写完后目标端实际保存下来的修改时间（Unix 秒）；-1 表示未知。
     *
     * 记的是**回读值**而非我们请求的值，因此目标文件系统的取整会被如实反映。下次比对时它
     * 还能顺带发现"目标文件被别人动过"——那时目标端的时间戳不再等于这个值。
     */
    qint64 destinationMtime = -1;

    /** @brief 记录更新时间（Unix 秒），供过期清理使用。 */
    qint64 updatedAt = 0;

    /** @brief 记录是否具备参与判定的最低信息量。 */
    [[nodiscard]] bool isUsable() const
    {
        if (kind == RecordKind::Complete) {
            return true;
        }
        return bytesWritten > 0 && sourceSize > 0 && bytesWritten <= sourceSize;
    }
};

/** @brief 某个目标端"写回修改时间是否真的生效"的实测结论。 */
struct MtimeTrust {
    bool trusted = false;
    qint64 updatedAt = 0;
};

/**
 * @brief 线程安全的续传账本，按 (目标, 目的路径) 保存凭据并记录 mtime 可信度。
 *
 * 默认落在应用数据目录；测试可传入独立路径，互不干扰。
 */
class Ledger final {
public:
    /**
     * @brief 构造账本。
     * @param filePath 存储路径；留空时使用应用数据目录下的默认文件。
     */
    explicit Ledger(QString filePath = {})
        : m_filePath(filePath.isEmpty() ? defaultFilePath() : std::move(filePath))
    {
    }

    /** @brief 组合出账本键；同一目的路径在不同服务器上互不影响。 */
    [[nodiscard]] static QString makeKey(const QString &targetKey, const QString &destinationPath)
    {
        return targetKey + QLatin1Char('\n') + destinationPath;
    }

    /** @brief 查询凭据；不存在或已损坏时返回空值。 */
    [[nodiscard]] std::optional<LedgerRecord> lookup(const QString &key) const
    {
        QMutexLocker locker(&m_mutex);
        ensureLoadedLocked();
        const auto iterator = m_records.constFind(key);
        if (iterator == m_records.constEnd() || !iterator.value().isUsable()) {
            return std::nullopt;
        }
        return iterator.value();
    }

    /** @brief 写入或更新凭据；落盘按节流进行，请求收尾时由 flush() 兜底。 */
    void store(const QString &key, LedgerRecord record)
    {
        record.updatedAt = nowSeconds();
        QMutexLocker locker(&m_mutex);
        ensureLoadedLocked();
        m_records.insert(key, std::move(record));
        markDirtyLocked();
    }

    /** @brief 删除凭据；传输成功或残片作废时必须调用，避免下次拿旧凭据去判定。 */
    void discard(const QString &key)
    {
        QMutexLocker locker(&m_mutex);
        ensureLoadedLocked();
        if (m_records.remove(key) > 0) {
            markDirtyLocked();
        }
    }

    /**
     * @brief 把挂起的改动立即写盘。
     *
     * 每传完一个文件就整份 JSON 重写一次，在几千个文件的目录树上会退化成 O(n²) 次写盘，
     * 因此平时按时间节流；一次请求结束时必须调用本方法，否则最后几条记录会随进程退出丢失，
     * 下次同步又要白传一遍。
     */
    void flush()
    {
        QMutexLocker locker(&m_mutex);
        if (m_dirty) {
            saveLocked();
            m_dirty = false;
            m_sinceLastSave.restart();
        }
    }

    ~Ledger() { flush(); }

    /**
     * @brief 目标端能否可靠保存修改时间。
     *
     * 默认 false：从未实测过就意味着目标文件的 mtime 不是我们写上去的，此时"大小相同且
     * mtime 相同"不足以证明内容一致，必须全量重传。保守方向永远是多传一次。
     */
    [[nodiscard]] bool mtimeTrusted(const QString &trustKey) const
    {
        QMutexLocker locker(&m_mutex);
        ensureLoadedLocked();
        const auto iterator = m_trust.constFind(trustKey);
        return iterator != m_trust.constEnd() && iterator.value().trusted;
    }

    /** @brief 记录一次修改时间写回后的回读结论。 */
    void setMtimeTrusted(const QString &trustKey, const bool trusted)
    {
        QMutexLocker locker(&m_mutex);
        ensureLoadedLocked();
        const auto iterator = m_trust.constFind(trustKey);
        if (iterator != m_trust.constEnd() && iterator.value().trusted == trusted) {
            // 结论没变就不必反复写盘：递归传输里每个文件都会调用一次。
            return;
        }
        m_trust.insert(trustKey, MtimeTrust{trusted, nowSeconds()});
        markDirtyLocked();
    }

    /** @brief 账本文件路径，供诊断与测试断言使用。 */
    [[nodiscard]] QString filePath() const { return m_filePath; }

private:
    /** @brief 默认账本位置；应用数据目录不可用时退回临时目录，绝不写入用户项目目录。 */
    [[nodiscard]] static QString defaultFilePath()
    {
        QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (directory.isEmpty()) {
            directory = QDir::tempPath();
        }
        QDir().mkpath(directory);
        return QDir(directory).filePath(QStringLiteral("sftp-resume-ledger.json"));
    }

    [[nodiscard]] static qint64 nowSeconds() { return QDateTime::currentSecsSinceEpoch(); }

    /** @brief 首次访问时读盘；文件缺失或损坏一律按空账本处理（只会导致多传，不会传错）。 */
    void ensureLoadedLocked() const
    {
        if (m_loaded) {
            return;
        }
        m_loaded = true;

        QFile file(m_filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        file.close();
        if (!document.isObject()) {
            return;
        }

        const qint64 now = nowSeconds();
        const QJsonObject root = document.object();
        const QJsonObject records = root.value(QStringLiteral("records")).toObject();
        for (auto iterator = records.constBegin(); iterator != records.constEnd(); ++iterator) {
            const QJsonObject value = iterator.value().toObject();
            LedgerRecord record;
            record.kind = value.value(QStringLiteral("complete")).toBool()
                ? RecordKind::Complete
                : RecordKind::Partial;
            record.sourcePath = value.value(QStringLiteral("sourcePath")).toString();
            record.sourceSize = static_cast<quint64>(
                value.value(QStringLiteral("sourceSize")).toString().toULongLong());
            record.sourceMtime = static_cast<qint64>(
                value.value(QStringLiteral("sourceMtime")).toString().toLongLong());
            record.sourceMtimeMs = static_cast<qint64>(
                value.value(QStringLiteral("sourceMtimeMs")).toString().toLongLong());
            record.sourceHash = value.value(QStringLiteral("sourceHash")).toString().toLatin1();
            record.destinationSize = static_cast<quint64>(
                value.value(QStringLiteral("destinationSize")).toString().toULongLong());
            record.destinationMtime = static_cast<qint64>(
                value.value(QStringLiteral("destinationMtime")).toString().toLongLong());
            record.bytesWritten = static_cast<quint64>(
                value.value(QStringLiteral("bytesWritten")).toString().toULongLong());
            record.updatedAt = static_cast<qint64>(
                value.value(QStringLiteral("updatedAt")).toString().toLongLong());
            if (!record.isUsable() || now - record.updatedAt > kRecordLifetimeSeconds) {
                continue;
            }
            m_records.insert(iterator.key(), record);
        }

        const QJsonObject trust = root.value(QStringLiteral("mtimeTrust")).toObject();
        for (auto iterator = trust.constBegin(); iterator != trust.constEnd(); ++iterator) {
            const QJsonObject value = iterator.value().toObject();
            MtimeTrust entry;
            entry.trusted = value.value(QStringLiteral("trusted")).toBool();
            entry.updatedAt = static_cast<qint64>(
                value.value(QStringLiteral("updatedAt")).toString().toLongLong());
            if (now - entry.updatedAt > kRecordLifetimeSeconds) {
                continue;
            }
            m_trust.insert(iterator.key(), entry);
        }
    }

    /** @brief 标记有未落盘改动，并在超过节流间隔时立即写盘。 */
    void markDirtyLocked()
    {
        m_dirty = true;
        if (!m_sinceLastSave.isValid() || m_sinceLastSave.elapsed() >= kSaveThrottleMs) {
            saveLocked();
            m_dirty = false;
            m_sinceLastSave.restart();
        }
    }

    /**
     * @brief 原子写盘。
     *
     * 大小与时间戳一律以字符串保存：JSON 数值是 double，超过 2^53 的文件大小会被静默截断，
     * 而续传判定要求"字节数精确相等"，一个被舍入的大小会让判定在超大文件上永远失败。
     */
    void saveLocked() const
    {
        QJsonObject records;
        for (auto iterator = m_records.constBegin(); iterator != m_records.constEnd(); ++iterator) {
            const LedgerRecord &record = iterator.value();
            QJsonObject value;
            value.insert(QStringLiteral("complete"), record.kind == RecordKind::Complete);
            value.insert(QStringLiteral("sourcePath"), record.sourcePath);
            value.insert(QStringLiteral("sourceSize"), QString::number(record.sourceSize));
            value.insert(QStringLiteral("sourceMtime"), QString::number(record.sourceMtime));
            value.insert(QStringLiteral("sourceMtimeMs"), QString::number(record.sourceMtimeMs));
            value.insert(QStringLiteral("sourceHash"), QString::fromLatin1(record.sourceHash));
            value.insert(QStringLiteral("destinationSize"),
                         QString::number(record.destinationSize));
            value.insert(QStringLiteral("destinationMtime"),
                         QString::number(record.destinationMtime));
            value.insert(QStringLiteral("bytesWritten"), QString::number(record.bytesWritten));
            value.insert(QStringLiteral("updatedAt"), QString::number(record.updatedAt));
            records.insert(iterator.key(), value);
        }
        QJsonObject trust;
        for (auto iterator = m_trust.constBegin(); iterator != m_trust.constEnd(); ++iterator) {
            QJsonObject value;
            value.insert(QStringLiteral("trusted"), iterator.value().trusted);
            value.insert(QStringLiteral("updatedAt"), QString::number(iterator.value().updatedAt));
            trust.insert(iterator.key(), value);
        }

        QJsonObject root;
        root.insert(QStringLiteral("records"), records);
        root.insert(QStringLiteral("mtimeTrust"), trust);

        QDir().mkpath(QFileInfo(m_filePath).absolutePath());
        QSaveFile file(m_filePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return;
        }
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        // 写失败只会丢掉续传凭据（下次全量重传），不影响正确性，因此不向上抛错。
        file.commit();
    }

    /** @brief 两次写盘之间的最小间隔；崩溃最多丢掉这段时间内的记录，代价只是多传一次。 */
    static constexpr qint64 kSaveThrottleMs = 2000;

    mutable QMutex m_mutex;
    QString m_filePath;
    bool m_dirty = false;
    QElapsedTimer m_sinceLastSave;
    mutable bool m_loaded = false;
    mutable QHash<QString, LedgerRecord> m_records;
    mutable QHash<QString, MtimeTrust> m_trust;
};

} // namespace SftpResume
