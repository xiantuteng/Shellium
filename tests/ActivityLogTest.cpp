#include "core/ActivityLog.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

/**
 * @brief 运行日志的落盘行为回归。
 *
 * 标题栏的瞬时提示条已被移除，原本发往它的每一条消息现在只剩日志这一条去路：如果写入
 * 悄悄失败或被换行撑散，用户将既看不到提示也查不到记录，故障会彻底静默。这些用例覆盖
 * 目录自动创建、追加而非覆盖、单行结构与体积轮转。
 */
class ActivityLogTest final : public QObject {
    Q_OBJECT

private slots:
    /** @brief 日志目录不存在时必须自动创建，而不是丢弃消息。 */
    void createsMissingDirectoryAndWritesMessage();

    /** @brief 连续消息必须逐行追加，不能互相覆盖。 */
    void appendsMessagesInOrder();

    /** @brief 含换行的消息必须折成一行，保持按行检索可用。 */
    void flattensEmbeddedNewlinesIntoSingleLine();

    /** @brief 空消息是旧提示条"清除"的遗留语义，不应产生日志行。 */
    void ignoresEmptyMessage();

    /** @brief 超过上限后轮转为 .1，新日志从空文件重新开始。 */
    void rotatesOversizedFile();
};

namespace {

/** @brief 读取日志文件的全部非空行。 */
QStringList readLines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll())
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

} // namespace

void ActivityLogTest::createsMissingDirectoryAndWritesMessage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("nested/logs/shellium.log"));
    ActivityLog::instance().setFilePath(path);

    QVERIFY(ActivityLog::instance().append(QStringLiteral("等待内层 SSH 密码")));

    QVERIFY(QFile::exists(path));
    const QStringList lines = readLines(path);
    QCOMPARE(lines.size(), 1);
    QVERIFY(lines.first().endsWith(QStringLiteral("等待内层 SSH 密码")));
    // 时间戳必须真的写在前面，否则日志无法与服务端记录按时间对齐。
    QVERIFY(lines.first().startsWith(QStringLiteral("20")));
}

void ActivityLogTest::appendsMessagesInOrder()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("shellium.log"));
    ActivityLog::instance().setFilePath(path);

    QVERIFY(ActivityLog::instance().append(QStringLiteral("第一条")));
    QVERIFY(ActivityLog::instance().append(QStringLiteral("第二条")));

    const QStringList lines = readLines(path);
    QCOMPARE(lines.size(), 2);
    QVERIFY(lines.at(0).endsWith(QStringLiteral("第一条")));
    QVERIFY(lines.at(1).endsWith(QStringLiteral("第二条")));
}

void ActivityLogTest::flattensEmbeddedNewlinesIntoSingleLine()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("shellium.log"));
    ActivityLog::instance().setFilePath(path);

    QVERIFY(ActivityLog::instance().append(QStringLiteral("传输失败：\r\n磁盘已满")));

    const QStringList lines = readLines(path);
    QCOMPARE(lines.size(), 1);
    QVERIFY(lines.first().endsWith(QStringLiteral("传输失败：  磁盘已满")));
}

void ActivityLogTest::ignoresEmptyMessage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("shellium.log"));
    ActivityLog::instance().setFilePath(path);

    QVERIFY(ActivityLog::instance().append(QString()));

    QVERIFY(readLines(path).isEmpty());
}

void ActivityLogTest::rotatesOversizedFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("shellium.log"));
    {
        QFile seed(path);
        QVERIFY(seed.open(QIODevice::WriteOnly));
        // 略微超过 1 MiB 上限，模拟长期运行后的日志。
        QVERIFY(seed.write(QByteArray(1024 * 1024 + 16, 'x')) > 0);
    }
    ActivityLog::instance().setFilePath(path);

    QVERIFY(ActivityLog::instance().append(QStringLiteral("轮转后的第一条")));

    QVERIFY(QFile::exists(path + QStringLiteral(".1")));
    const QStringList lines = readLines(path);
    QCOMPARE(lines.size(), 1);
    QVERIFY(lines.first().endsWith(QStringLiteral("轮转后的第一条")));
}

QTEST_MAIN(ActivityLogTest)
#include "ActivityLogTest.moc"
