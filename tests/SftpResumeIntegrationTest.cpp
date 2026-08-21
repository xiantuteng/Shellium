#include "core/ConnectionTarget.h"
#include "sftp/SftpService.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <optional>

/**
 * @brief 针对真实 SFTP 服务器的续传与"内容是否需要更新"回归。
 *
 * 这些行为无法用桩替身验证：真正要确认的是**服务器上那个文件最终变成了什么样**——它由
 * stat 返回的大小、服务端是否保存了修改时间、rename 能否覆盖已存在的文件、以及残片在
 * 中断后到底留下了多少字节共同决定，任何一个环节用假对象模拟都会把被测逻辑一起模拟掉。
 *
 * 缺少 SHELLIUM_IT_* 环境变量时整组用例自行跳过。
 */
class SftpResumeIntegrationTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    /** @brief 大小相同但内容不同的文件必须真的被更新，而不是被判成"已完成"。 */
    void updatesRemoteFileWhenSizeMatchesButContentDiffers();

    /** @brief 内容确实没变时不重传，但必须如实报告为"跳过"而不是"已上传"。 */
    void skipsSecondUploadOfUnchangedFileAndSaysSo();

    /** @brief 中断后续传出来的文件必须与源逐字节相同。 */
    void resumesInterruptedUploadIntoIdenticalBytes();

    /** @brief 中断后只改动源文件尾部（大小不变）时，残片必须作废并全量重传。 */
    void discardsPartialWhenOnlySourceTailChanged();

    /** @brief 合并更新：远端多出来的文件保留，内容变了的文件必须被更新。 */
    void mergeKeepsExtraRemoteFilesAndUpdatesChangedOnes();

    /** @brief 替换：远端多出来的文件必须被删除，结果与本地严格一致。 */
    void replaceRemovesExtraRemoteFiles();

private:
    [[nodiscard]] static ConnectionTarget testTarget();

    /** @brief 生成可验证的伪随机内容；全零内容会让"写错位置"这类错误互相抵消。 */
    [[nodiscard]] static QByteArray makePayload(qint64 size, quint32 seed);

    /** @brief 把内容写到本地文件，失败即断言。 */
    static void writeLocalFile(const QString &path, const QByteArray &content);

    /** @brief 远端测试目录，随进程号隔离，避免并发运行互相踩踏。 */
    [[nodiscard]] QString remoteRoot() const;

    /**
     * @brief 在远端建出本组用例共用的测试目录。
     *
     * 服务没有单独的建目录接口，这里用"上传一个同名空目录"来创建，顺带也覆盖了递归上传
     * 建根目录这条路径。
     */
    void ensureRemoteRoot();

    bool m_configured = false;
    QTemporaryDir m_ledgerDirectory;
};

namespace {

/** @brief 一次单文件请求的结局。 */
struct RequestOutcome {
    bool finished = false;
    bool succeeded = false;
    QString message;
};

/** @brief 等待单文件请求结束；超时按未完成返回，由调用方断言。 */
RequestOutcome awaitRequest(SftpService &service, const quint64 requestId, const int timeoutMs = 120000)
{
    RequestOutcome outcome;
    QEventLoop loop;
    const QMetaObject::Connection okConnection = QObject::connect(
        &service, &SftpService::operationSucceeded, &loop,
        [&](const quint64 id, const QString &description) {
            if (id != requestId) {
                return;
            }
            outcome = {true, true, description};
            loop.quit();
        });
    const QMetaObject::Connection failConnection = QObject::connect(
        &service, &SftpService::operationFailed, &loop,
        [&](const quint64 id, const ConnectionTarget &, SftpService::ErrorKind,
            const QString &message) {
            if (id != requestId) {
                return;
            }
            outcome = {true, false, message};
            loop.quit();
        });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(okConnection);
    QObject::disconnect(failConnection);
    return outcome;
}

/** @brief 等待递归传输结束并取回汇总；连接阶段失败时返回空值。 */
std::optional<SftpRecursive::TransferSummary> awaitRecursive(SftpService &service,
                                                             const quint64 requestId,
                                                             const int timeoutMs = 180000)
{
    std::optional<SftpRecursive::TransferSummary> summary;
    QEventLoop loop;
    const QMetaObject::Connection finishedConnection = QObject::connect(
        &service, &SftpService::recursiveTransferFinished, &loop,
        [&](const quint64 id, const SftpRecursive::TransferSummary &value) {
            if (id != requestId) {
                return;
            }
            summary = value;
            loop.quit();
        });
    const QMetaObject::Connection failConnection = QObject::connect(
        &service, &SftpService::operationFailed, &loop,
        [&](const quint64 id, const ConnectionTarget &, SftpService::ErrorKind, const QString &) {
            if (id == requestId) {
                loop.quit();
            }
        });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(finishedConnection);
    QObject::disconnect(failConnection);
    return summary;
}

/** @brief 下载远端文件并返回其全部字节；失败时返回空值。 */
std::optional<QByteArray> fetchRemoteBytes(SftpService &service,
                                           const ConnectionTarget &target,
                                           const QString &remotePath,
                                           const QString &localScratchPath)
{
    QFile::remove(localScratchPath);
    const quint64 requestId = service.downloadFile(target, remotePath, localScratchPath,
                                                   SftpTransfer::ExistingFilePolicy::Overwrite);
    const RequestOutcome outcome = awaitRequest(service, requestId);
    if (!outcome.finished || !outcome.succeeded) {
        return std::nullopt;
    }
    QFile file(localScratchPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    return file.readAll();
}

} // namespace

void SftpResumeIntegrationTest::initTestCase()
{
    const QByteArray host = qgetenv("SHELLIUM_IT_HOST");
    const QByteArray user = qgetenv("SHELLIUM_IT_USER");
    const QByteArray password = qgetenv("SHELLIUM_IT_PASSWORD");
    m_configured = !host.isEmpty() && !user.isEmpty() && !password.isEmpty();
    if (!m_configured) {
        QSKIP("未设置 SHELLIUM_IT_HOST / SHELLIUM_IT_USER / SHELLIUM_IT_PASSWORD，跳过集成测试");
    }
    QVERIFY(m_ledgerDirectory.isValid());
    ensureRemoteRoot();
}

void SftpResumeIntegrationTest::ensureRemoteRoot()
{
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    const QString localRoot = QDir(staging.path()).filePath(QFileInfo(remoteRoot()).fileName());
    QVERIFY(QDir().mkpath(localRoot));

    SftpService service;
    service.setResumeLedgerPath(m_ledgerDirectory.filePath(QStringLiteral("bootstrap.json")));
    const std::optional<SftpRecursive::TransferSummary> summary = awaitRecursive(
        service, service.uploadDirectory(testTarget(), localRoot, QStringLiteral("/tmp"),
                                         SftpTransfer::ExistingFilePolicy::Overwrite,
                                         SftpTransfer::DirectoryConflictPolicy::Merge));
    QVERIFY2(summary.has_value(), "无法在远端创建测试目录");
}

void SftpResumeIntegrationTest::cleanupTestCase()
{
    if (!m_configured) {
        return;
    }
    // 用替换模式上传一个空目录，等于把远端测试目录清空；随后删掉空目录本身。这样清理逻辑
    // 复用被测代码路径，不需要再写一套递归删除。
    QTemporaryDir emptyDirectory;
    if (!emptyDirectory.isValid()) {
        return;
    }
    const QString scratchName = QFileInfo(remoteRoot()).fileName();
    const QString localEmpty = QDir(emptyDirectory.path()).filePath(scratchName);
    QDir().mkpath(localEmpty);

    SftpService service;
    service.setResumeLedgerPath(m_ledgerDirectory.filePath(QStringLiteral("cleanup.json")));
    const quint64 requestId = service.uploadDirectory(
        testTarget(), localEmpty, QStringLiteral("/tmp"),
        SftpTransfer::ExistingFilePolicy::Overwrite,
        SftpTransfer::DirectoryConflictPolicy::Replace);
    awaitRecursive(service, requestId);
    awaitRequest(service, service.removePath(testTarget(), remoteRoot(), true), 30000);
}

ConnectionTarget SftpResumeIntegrationTest::testTarget()
{
    ConnectionTarget target;
    target.id = QStringLiteral("sftp-resume-it");
    target.host = QString::fromUtf8(qgetenv("SHELLIUM_IT_HOST"));
    target.username = QString::fromUtf8(qgetenv("SHELLIUM_IT_USER"));
    target.password = QString::fromUtf8(qgetenv("SHELLIUM_IT_PASSWORD"));
    const QByteArray portEnv = qgetenv("SHELLIUM_IT_PORT");
    target.port = portEnv.isEmpty() ? quint16(22) : static_cast<quint16>(portEnv.toUShort());
    target.confidence = ConnectionTarget::Confidence::Confirmed;
    return target;
}

QByteArray SftpResumeIntegrationTest::makePayload(const qint64 size, const quint32 seed)
{
    QByteArray payload;
    payload.resize(size);
    QRandomGenerator generator(seed);
    for (qint64 index = 0; index < size; index += 4) {
        const quint32 word = generator.generate();
        std::memcpy(payload.data() + index, &word, std::min<qint64>(4, size - index));
    }
    return payload;
}

void SftpResumeIntegrationTest::writeLocalFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(content), static_cast<qint64>(content.size()));
    file.close();
}

QString SftpResumeIntegrationTest::remoteRoot() const
{
    return QStringLiteral("/tmp/shellium-resume-it-%1").arg(QCoreApplication::applicationPid());
}

void SftpResumeIntegrationTest::updatesRemoteFileWhenSizeMatchesButContentDiffers()
{
    QTemporaryDir localDirectory;
    QVERIFY(localDirectory.isValid());
    SftpService service;
    service.setResumeLedgerPath(m_ledgerDirectory.filePath(QStringLiteral("same-size.json")));
    const ConnectionTarget target = testTarget();

    // 两份内容完全不同、长度却分毫不差——这正是"改了几个字符"最常见的形态。
    const qint64 size = 64 * 1024 + 13;
    const QByteArray first = makePayload(size, 1);
    const QByteArray second = makePayload(size, 2);
    QCOMPARE(first.size(), second.size());
    QVERIFY(first != second);

    const QString localPath = QDir(localDirectory.path()).filePath(QStringLiteral("same-size.bin"));
    const QString remotePath = remoteRoot() + QStringLiteral("/same-size.bin");

    writeLocalFile(localPath, first);
    RequestOutcome outcome = awaitRequest(
        service, service.uploadFile(target, localPath, remotePath,
                                    SftpTransfer::ExistingFilePolicy::ResumeWhenPossible));
    QVERIFY2(outcome.finished && outcome.succeeded, qPrintable(outcome.message));

    // 覆盖成同样长度的另一份内容，再用**默认的断点续传策略**上传。
    writeLocalFile(localPath, second);
    outcome = awaitRequest(
        service, service.uploadFile(target, localPath, remotePath,
                                    SftpTransfer::ExistingFilePolicy::ResumeWhenPossible));
    QVERIFY2(outcome.finished && outcome.succeeded, qPrintable(outcome.message));

    const std::optional<QByteArray> remoteBytes = fetchRemoteBytes(
        service, target, remotePath,
        QDir(localDirectory.path()).filePath(QStringLiteral("verify.bin")));
    QVERIFY(remoteBytes.has_value());
    QCOMPARE(remoteBytes->size(), second.size());
    // 旧实现在这里会拿到 first：大小相同就被判成"已完成"，远端一个字节都没变。
    QVERIFY2(*remoteBytes == second, "远端文件未被更新，仍是第一次上传的内容");
}

void SftpResumeIntegrationTest::skipsSecondUploadOfUnchangedFileAndSaysSo()
{
    QTemporaryDir localDirectory;
    QVERIFY(localDirectory.isValid());
    SftpService service;
    service.setResumeLedgerPath(m_ledgerDirectory.filePath(QStringLiteral("unchanged.json")));
    const ConnectionTarget target = testTarget();

    const QByteArray payload = makePayload(32 * 1024 + 7, 3);
    const QString localPath = QDir(localDirectory.path()).filePath(QStringLiteral("stable.bin"));
    const QString remotePath = remoteRoot() + QStringLiteral("/stable.bin");
    writeLocalFile(localPath, payload);

    RequestOutcome first = awaitRequest(
        service, service.uploadFile(target, localPath, remotePath,
                                    SftpTransfer::ExistingFilePolicy::ResumeWhenPossible));
    QVERIFY2(first.finished && first.succeeded, qPrintable(first.message));
    QVERIFY(first.message.contains(QStringLiteral("已上传")));

    const RequestOutcome second = awaitRequest(
        service, service.uploadFile(target, localPath, remotePath,
                                    SftpTransfer::ExistingFilePolicy::ResumeWhenPossible));
    QVERIFY2(second.finished && second.succeeded, qPrintable(second.message));
    // 跳过必须说成跳过：把"内容一致、未重传"讲成"已上传"正是用户误以为文件更新了的根源。
    QVERIFY2(second.message.contains(QStringLiteral("内容一致")),
             qPrintable(QStringLiteral("第二次上传的消息未如实说明跳过：") + second.message));

    // 内容仍必须与本地一致，跳过不能建立在错误前提上。
    const std::optional<QByteArray> remoteBytes = fetchRemoteBytes(
        service, target, remotePath,
        QDir(localDirectory.path()).filePath(QStringLiteral("verify.bin")));
    QVERIFY(remoteBytes.has_value());
    QVERIFY(*remoteBytes == payload);
}

void SftpResumeIntegrationTest::resumesInterruptedUploadIntoIdenticalBytes()
{
    QTemporaryDir localDirectory;
    QVERIFY(localDirectory.isValid());
    SftpService service;
    service.setResumeLedgerPath(m_ledgerDirectory.filePath(QStringLiteral("resume.json")));
    const ConnectionTarget target = testTarget();

    const QByteArray payload = makePayload(6 * 1024 * 1024 + 101, 4);
    const QString localPath = QDir(localDirectory.path()).filePath(QStringLiteral("large.bin"));
    const QString remotePath = remoteRoot() + QStringLiteral("/large.bin");
    writeLocalFile(localPath, payload);

    quint64 activeRequest = 0;
    bool cancelIssued = false;
    const QMetaObject::Connection progressConnection = connect(
        &service, &SftpService::transferProgress, &service,
        [&](const quint64 id, const quint64 completed, const quint64 total) {
            if (id != activeRequest || cancelIssued || total == 0) {
                return;
            }
            if (completed * 4 >= total) {
                cancelIssued = true;
                service.cancelRequest(id);
            }
        });

    activeRequest = service.uploadFile(target, localPath, remotePath,
                                       SftpTransfer::ExistingFilePolicy::ResumeWhenPossible);
    const RequestOutcome interrupted = awaitRequest(service, activeRequest);
    disconnect(progressConnection);
    QVERIFY(interrupted.finished);
    QVERIFY2(!interrupted.succeeded, "传输本应被取消，却报告成功");

    // 中断之后正式文件不该存在——字节全部落在残片上，正式文件绝不会停在"传了一半"的状态。
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    QVERIFY2(!fetchRemoteBytes(service, target, remotePath,
                               QDir(scratch.path()).filePath(QStringLiteral("half.bin")))
                  .has_value(),
             "中断后远端出现了半写的正式文件");

    activeRequest = service.uploadFile(target, localPath, remotePath,
                                       SftpTransfer::ExistingFilePolicy::ResumeWhenPossible);
    const RequestOutcome resumed = awaitRequest(service, activeRequest);
    QVERIFY2(resumed.finished && resumed.succeeded, qPrintable(resumed.message));

    const std::optional<QByteArray> remoteBytes = fetchRemoteBytes(
        service, target, remotePath,
        QDir(localDirectory.path()).filePath(QStringLiteral("verify.bin")));
    QVERIFY(remoteBytes.has_value());
    QCOMPARE(remoteBytes->size(), payload.size());
    QVERIFY2(*remoteBytes == payload, "续传后的文件与源不一致");
}

void SftpResumeIntegrationTest::discardsPartialWhenOnlySourceTailChanged()
{
    QTemporaryDir localDirectory;
    QVERIFY(localDirectory.isValid());
    SftpService service;
    service.setResumeLedgerPath(m_ledgerDirectory.filePath(QStringLiteral("tail.json")));
    const ConnectionTarget target = testTarget();

    const qint64 size = 6 * 1024 * 1024 + 55;
    QByteArray original = makePayload(size, 5);
    const QString localPath = QDir(localDirectory.path()).filePath(QStringLiteral("tail.bin"));
    const QString remotePath = remoteRoot() + QStringLiteral("/tail.bin");
    writeLocalFile(localPath, original);

    quint64 activeRequest = 0;
    bool cancelIssued = false;
    const QMetaObject::Connection progressConnection = connect(
        &service, &SftpService::transferProgress, &service,
        [&](const quint64 id, const quint64 completed, const quint64 total) {
            if (id != activeRequest || cancelIssued || total == 0) {
                return;
            }
            if (completed * 4 >= total) {
                cancelIssued = true;
                service.cancelRequest(id);
            }
        });
    activeRequest = service.uploadFile(target, localPath, remotePath,
                                       SftpTransfer::ExistingFilePolicy::ResumeWhenPossible);
    const RequestOutcome interrupted = awaitRequest(service, activeRequest);
    disconnect(progressConnection);
    QVERIFY(interrupted.finished);
    QVERIFY(!interrupted.succeeded);

    // 只改最后一段、长度分毫不变：残片覆盖的那段前缀依旧完全一样，只有整文件哈希能发现
    // 差异。若判据是"前缀一致就接着写"，这里就会拼出"旧头 + 新尾"的坏文件。
    QByteArray modified = original;
    const QByteArray newTail = makePayload(64 * 1024, 6);
    modified.replace(size - newTail.size(), newTail.size(), newTail);
    QCOMPARE(modified.size(), original.size());
    QVERIFY(modified.left(size - newTail.size()) == original.left(size - newTail.size()));
    writeLocalFile(localPath, modified);

    const RequestOutcome retried = awaitRequest(
        service, service.uploadFile(target, localPath, remotePath,
                                    SftpTransfer::ExistingFilePolicy::ResumeWhenPossible));
    QVERIFY2(retried.finished && retried.succeeded, qPrintable(retried.message));

    const std::optional<QByteArray> remoteBytes = fetchRemoteBytes(
        service, target, remotePath,
        QDir(localDirectory.path()).filePath(QStringLiteral("verify.bin")));
    QVERIFY(remoteBytes.has_value());
    QCOMPARE(remoteBytes->size(), modified.size());
    QVERIFY2(*remoteBytes == modified, "残片未作废，远端文件是新旧两版拼接出来的");
}

void SftpResumeIntegrationTest::mergeKeepsExtraRemoteFilesAndUpdatesChangedOnes()
{
    QTemporaryDir localDirectory;
    QVERIFY(localDirectory.isValid());
    SftpService service;
    service.setResumeLedgerPath(m_ledgerDirectory.filePath(QStringLiteral("merge.json")));
    const ConnectionTarget target = testTarget();

    const QString treeName = QStringLiteral("merge-tree");
    const QString localTree = QDir(localDirectory.path()).filePath(treeName);
    const QString remoteTree = remoteRoot() + QLatin1Char('/') + treeName;

    const QByteArray stable = makePayload(8192, 7);
    const QByteArray before = makePayload(4096, 8);
    const QByteArray after = makePayload(4096, 9);
    QCOMPARE(before.size(), after.size());

    writeLocalFile(QDir(localTree).filePath(QStringLiteral("stable.bin")), stable);
    writeLocalFile(QDir(localTree).filePath(QStringLiteral("nested/changing.bin")), before);

    std::optional<SftpRecursive::TransferSummary> summary = awaitRecursive(
        service, service.uploadDirectory(target, localTree, remoteRoot(),
                                         SftpTransfer::ExistingFilePolicy::ResumeWhenPossible,
                                         SftpTransfer::DirectoryConflictPolicy::Merge));
    QVERIFY(summary.has_value());
    QVERIFY(summary->failed.isEmpty());
    QCOMPARE(summary->filesTransferred, 2ULL);

    // 造一个"远端有、本地没有"的文件：合并模式必须保留它。
    const QString extraLocal = QDir(localDirectory.path()).filePath(QStringLiteral("extra.bin"));
    const QByteArray extra = makePayload(1024, 10);
    writeLocalFile(extraLocal, extra);
    const QString extraRemote = remoteTree + QStringLiteral("/extra.bin");
    RequestOutcome extraUpload = awaitRequest(
        service, service.uploadFile(target, extraLocal, extraRemote,
                                    SftpTransfer::ExistingFilePolicy::Overwrite));
    QVERIFY2(extraUpload.finished && extraUpload.succeeded, qPrintable(extraUpload.message));

    // 同大小改内容，再次合并上传。
    writeLocalFile(QDir(localTree).filePath(QStringLiteral("nested/changing.bin")), after);
    summary = awaitRecursive(
        service, service.uploadDirectory(target, localTree, remoteRoot(),
                                         SftpTransfer::ExistingFilePolicy::ResumeWhenPossible,
                                         SftpTransfer::DirectoryConflictPolicy::Merge));
    QVERIFY(summary.has_value());
    QVERIFY(summary->failed.isEmpty());
    QCOMPARE(summary->removedForReplace, 0ULL);

    const std::optional<QByteArray> changedBytes = fetchRemoteBytes(
        service, target, remoteTree + QStringLiteral("/nested/changing.bin"),
        QDir(localDirectory.path()).filePath(QStringLiteral("verify-changed.bin")));
    QVERIFY(changedBytes.has_value());
    // 目录里的文件同样必须被更新——用户报告的第二个症状正是"文件夹替换后里面还是旧文件"。
    QVERIFY2(*changedBytes == after, "目录内同大小的文件没有被更新");

    const std::optional<QByteArray> extraBytes = fetchRemoteBytes(
        service, target, extraRemote,
        QDir(localDirectory.path()).filePath(QStringLiteral("verify-extra.bin")));
    QVERIFY2(extraBytes.has_value(), "合并模式不应删除远端多出来的文件");
    QVERIFY(*extraBytes == extra);
}

void SftpResumeIntegrationTest::replaceRemovesExtraRemoteFiles()
{
    QTemporaryDir localDirectory;
    QVERIFY(localDirectory.isValid());
    SftpService service;
    service.setResumeLedgerPath(m_ledgerDirectory.filePath(QStringLiteral("replace.json")));
    const ConnectionTarget target = testTarget();

    const QString treeName = QStringLiteral("replace-tree");
    const QString localTree = QDir(localDirectory.path()).filePath(treeName);
    const QString remoteTree = remoteRoot() + QLatin1Char('/') + treeName;

    writeLocalFile(QDir(localTree).filePath(QStringLiteral("keep.bin")), makePayload(2048, 11));
    writeLocalFile(QDir(localTree).filePath(QStringLiteral("nested/deep.bin")), makePayload(3072, 12));

    std::optional<SftpRecursive::TransferSummary> summary = awaitRecursive(
        service, service.uploadDirectory(target, localTree, remoteRoot(),
                                         SftpTransfer::ExistingFilePolicy::ResumeWhenPossible,
                                         SftpTransfer::DirectoryConflictPolicy::Merge));
    QVERIFY(summary.has_value());
    QVERIFY(summary->failed.isEmpty());

    const QString staleLocal = QDir(localDirectory.path()).filePath(QStringLiteral("stale.bin"));
    writeLocalFile(staleLocal, makePayload(512, 13));
    const QString staleRemote = remoteTree + QStringLiteral("/nested/stale.bin");
    const RequestOutcome staleUpload = awaitRequest(
        service, service.uploadFile(target, staleLocal, staleRemote,
                                    SftpTransfer::ExistingFilePolicy::Overwrite));
    QVERIFY2(staleUpload.finished && staleUpload.succeeded, qPrintable(staleUpload.message));

    summary = awaitRecursive(
        service, service.uploadDirectory(target, localTree, remoteRoot(),
                                         SftpTransfer::ExistingFilePolicy::ResumeWhenPossible,
                                         SftpTransfer::DirectoryConflictPolicy::Replace));
    QVERIFY(summary.has_value());
    QVERIFY(summary->failed.isEmpty());
    QVERIFY2(summary->removedForReplace > 0, "替换模式没有删除任何远端条目");
    // 替换后必须重新写入全部文件，不存在"内容一致所以跳过"的情况：目录已经被整个删掉了。
    QCOMPARE(summary->filesTransferred, 2ULL);
    QCOMPARE(summary->filesSkippedIdentical, 0ULL);

    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    QVERIFY2(!fetchRemoteBytes(service, target, staleRemote,
                               QDir(scratch.path()).filePath(QStringLiteral("stale.bin")))
                  .has_value(),
             "替换模式后本地已不存在的文件仍留在远端");

    const std::optional<QByteArray> keptBytes = fetchRemoteBytes(
        service, target, remoteTree + QStringLiteral("/nested/deep.bin"),
        QDir(scratch.path()).filePath(QStringLiteral("deep.bin")));
    QVERIFY(keptBytes.has_value());
    QCOMPARE(keptBytes->size(), 3072);
}

QTEST_MAIN(SftpResumeIntegrationTest)
#include "SftpResumeIntegrationTest.moc"
