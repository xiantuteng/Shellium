#include "productivity/ProductivityModel.h"

#include "storage/ApplicationDatabase.h"

#include <QSet>

ProductivityModel::ProductivityModel(ApplicationDatabase *database, QObject *parent)
    : QObject(parent)
    , m_database(database)
{
}

void ProductivityModel::setDatabase(ApplicationDatabase *database)
{
    if (m_database == database) {
        return;
    }
    m_database = database;
    // 数据源切换后原界面的树和补全缓存都已失效；Snippets 是当前唯一无需主机上下文即可
    // 完整刷新的一类数据，因此立即广播。命令历史由终端在切换主机时按 hostId 主动读取。
    emit snippetsChanged();
}

bool ProductivityModel::ensureDatabase() const
{
    if (m_database != nullptr) {
        return true;
    }
    emit errorOccurred(tr("Productivity is not connected to the app database yet"));
    return false;
}

bool ProductivityModel::saveSnippet(const SnippetRecord &record)
{
    if (!ensureDatabase()) {
        return false;
    }
    QString error;
    if (!m_database->saveSnippet(record, &error)) {
        emit errorOccurred(tr("Failed to save snippet: %1").arg(error));
        return false;
    }
    emit snippetsChanged();
    return true;
}

bool ProductivityModel::removeSnippet(const QString &id, int *removedCount)
{
    if (!ensureDatabase()) {
        return false;
    }
    QString error;
    if (!m_database->removeSnippet(id, removedCount, &error)) {
        emit errorOccurred(tr("Failed to delete snippet: %1").arg(error));
        return false;
    }
    emit snippetsChanged();
    return true;
}

QList<SnippetRecord> ProductivityModel::snippetsInGroup(const QString &parentId, bool *ok) const
{
    if (ok != nullptr) {
        *ok = false;
    }
    if (!ensureDatabase()) {
        return {};
    }
    QString error;
    const QList<SnippetRecord> records = m_database->listSnippetsInGroup(parentId, ok, &error);
    if (!error.isEmpty()) {
        emit errorOccurred(tr("Failed to read snippet groups: %1").arg(error));
    }
    return records;
}

std::optional<SnippetRecord> ProductivityModel::snippet(const QString &id) const
{
    if (!ensureDatabase()) {
        return std::nullopt;
    }
    QString error;
    const std::optional<SnippetRecord> record = m_database->findSnippet(id, &error);
    if (!error.isEmpty()) {
        emit errorOccurred(tr("Failed to read snippets: %1").arg(error));
    }
    return record;
}

bool ProductivityModel::recordCommand(const QString &hostId, const QString &command)
{
    if (!ensureDatabase()) {
        return false;
    }
    if (command.trimmed().isEmpty() || command.front().isSpace()) {
        return true;
    }

    CommandHistoryEntry entry;
    entry.hostId = hostId;
    entry.command = command.trimmed();
    QString error;
    if (!m_database->appendCommandHistory(entry, &error).has_value()) {
        emit errorOccurred(tr("Failed to write command history: %1").arg(error));
        return false;
    }
    emit commandHistoryChanged(hostId);
    return true;
}

QList<CommandHistoryEntry> ProductivityModel::recentHistory(const QString &hostId,
                                                            const int limit,
                                                            bool *ok) const
{
    if (ok != nullptr) {
        *ok = false;
    }
    if (!ensureDatabase()) {
        return {};
    }
    QString error;
    const QList<CommandHistoryEntry> history = m_database->recentCommands(hostId, limit, ok, &error);
    if (!error.isEmpty()) {
        emit errorOccurred(tr("Failed to read command history: %1").arg(error));
    }
    return history;
}

QList<CommandHistoryEntry> ProductivityModel::recentHistoryAcrossHosts(const int limit,
                                                                       bool *ok) const
{
    if (ok != nullptr) {
        *ok = false;
    }
    if (!ensureDatabase()) {
        return {};
    }
    QString error;
    const QList<CommandHistoryEntry> history =
        m_database->recentCommandsAcrossHosts(limit, ok, &error);
    if (!error.isEmpty()) {
        emit errorOccurred(tr("Failed to read command history: %1").arg(error));
    }
    return history;
}

QStringList ProductivityModel::completionCandidates(const QString &hostId,
                                                     const QString &prefix,
                                                     const int limit) const
{
    if (limit <= 0 || !ensureDatabase()) {
        return {};
    }

    // 为去重和前缀过滤多读取一批记录；设置有限上限，避免多年历史一次性进入 GUI 线程。
    bool ok = false;
    const int historyLimit = qMax(limit * 10, 500);
    const QList<CommandHistoryEntry> history = recentHistory(hostId, historyLimit, &ok);
    if (!ok) {
        return {};
    }

    QStringList candidates;
    QSet<QString> seen;
    for (const CommandHistoryEntry &entry : history) {
        if (!entry.command.startsWith(prefix, Qt::CaseInsensitive) || seen.contains(entry.command)) {
            continue;
        }
        seen.insert(entry.command);
        candidates.append(entry.command);
        if (candidates.size() >= limit) {
            break;
        }
    }
    return candidates;
}
