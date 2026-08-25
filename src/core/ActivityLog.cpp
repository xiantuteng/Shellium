#include "core/ActivityLog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QStandardPaths>

ActivityLog &ActivityLog::instance()
{
    // 函数内静态量：C++11 起由编译器保证线程安全初始化，且不依赖跨翻译单元的构造顺序，
    // 因此 main() 之前就被调用也不会拿到未构造的对象。
    static ActivityLog log;
    return log;
}

QString ActivityLog::defaultFilePath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) {
        // 极少数受限环境拿不到数据目录；退回临时目录仍然可写，日志不至于整体失效。
        base = QDir::tempPath() + QStringLiteral("/Shellium");
    }
    return base + QStringLiteral("/logs/shellium.log");
}

ActivityLog::ActivityLog()
    : m_filePath(defaultFilePath())
{
}

void ActivityLog::setFilePath(const QString &path)
{
    const QMutexLocker locker(&m_mutex);
    m_filePath = path;
}

QString ActivityLog::filePath() const
{
    const QMutexLocker locker(&m_mutex);
    return m_filePath;
}

bool ActivityLog::append(const QString &message)
{
    if (message.isEmpty()) {
        // 旧提示条用空串表示"清除"，这个语义在日志里没有对应物，直接丢弃。
        return true;
    }

    const QMutexLocker locker(&m_mutex);
    if (m_filePath.isEmpty()) {
        return false;
    }

    const QFileInfo info(m_filePath);
    if (!QDir().mkpath(info.absolutePath())) {
        return false;
    }
    rotateIfNeededLocked();

    QFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return false;
    }
    // 每条一行：时间戳用 ISO-8601 带毫秒，便于与服务端日志按时间对齐；消息里的换行统一
    // 折成空格，避免一条状态把行结构撑散，破坏按行检索。
    QString flattened = message;
    flattened.replace(QLatin1Char('\r'), QLatin1Char(' '));
    flattened.replace(QLatin1Char('\n'), QLatin1Char(' '));
    const QString line =
        QDateTime::currentDateTime().toString(Qt::ISODateWithMs) + QStringLiteral(" ")
        + flattened + QStringLiteral("\n");
    return file.write(line.toUtf8()) >= 0;
}

void ActivityLog::rotateIfNeededLocked()
{
    const QFileInfo info(m_filePath);
    if (!info.exists() || info.size() < kMaximumFileBytes) {
        return;
    }
    // 只保留一代历史：日志是排查辅助，不值得为它无限占用用户磁盘。旧的 .1 先删除，
    // 否则 rename 在多数平台上会失败，文件将无休止增长。
    const QString rotated = m_filePath + QStringLiteral(".1");
    QFile::remove(rotated);
    QFile::rename(m_filePath, rotated);
}
