#include "core/ConnectionTarget.h"
#include "sftp/SftpService.h"

#include <QCoreApplication>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

/**
 * @brief 回归覆盖：上传大文件时改用 sftp_aio 流水线写入之后，多窗口分块与部分接受（服务端
 * 按 max_write_length 截断单次写请求）拼接回原始字节流的正确性。
 *
 * 之前 writeRemoteFile() 用同步的 sftp_write() 逐包等确认，每个约 32KB 的协议包都要等一次
 * 完整往返，同一局域网内实测比使用流水线的客户端慢了一个数量级以上。改成
 * sftp_aio_begin_write/sftp_aio_wait_write 维持多个未确认请求后，真正有风险的是新写的偏移
 * 追踪与"服务端接受字节数小于请求字节数时如何续传剩余数据"这部分逻辑——数据错位或丢字节
 * 不会有编译期或运行期报错，只会体现为下载回来的文件内容不一致，因此用端到端字节比对覆盖。
 */
class SftpUploadPipelineTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    /** @brief 跨越多个流水线窗口的大文件上传后原样下载，字节必须逐一相同。 */
    void uploadThenDownloadRoundTripsExactBytes();

private:
    [[nodiscard]] static ConnectionTarget testTarget();

    bool m_configured = false;
};

void SftpUploadPipelineTest::initTestCase()
{
    const QByteArray host = qgetenv("SHELLIUM_IT_HOST");
    const QByteArray user = qgetenv("SHELLIUM_IT_USER");
    const QByteArray password = qgetenv("SHELLIUM_IT_PASSWORD");
    m_configured = !host.isEmpty() && !user.isEmpty() && !password.isEmpty();
    if (!m_configured) {
        QSKIP("未设置 SHELLIUM_IT_HOST / SHELLIUM_IT_USER / SHELLIUM_IT_PASSWORD，跳过集成测试");
    }
}

ConnectionTarget SftpUploadPipelineTest::testTarget()
{
    ConnectionTarget target;
    target.id = QStringLiteral("sftp-upload-pipeline-it");
    target.host = QString::fromUtf8(qgetenv("SHELLIUM_IT_HOST"));
    target.username = QString::fromUtf8(qgetenv("SHELLIUM_IT_USER"));
    target.password = QString::fromUtf8(qgetenv("SHELLIUM_IT_PASSWORD"));
    const QByteArray portEnv = qgetenv("SHELLIUM_IT_PORT");
    target.port = portEnv.isEmpty() ? quint16(22) : static_cast<quint16>(portEnv.toUShort());
    target.confidence = ConnectionTarget::Confidence::Confirmed;
    return target;
}

void SftpUploadPipelineTest::uploadThenDownloadRoundTripsExactBytes()
{
    QTemporaryDir localDir;
    QVERIFY(localDir.isValid());

    // 3 MiB：默认 64KiB 读块 * 16 深度流水线窗口下会跨越多轮 begin/wait 循环，且大概率会
    // 撞上服务端 max_write_length（常见 32KiB）导致单次 begin_write 只被部分接受，
    // 正是新写的偏移续传逻辑最容易出错的地方。用可验证的伪随机内容而非全零，避免"写错位置
    // 但恰好都是同一个字节"这种巧合掩盖 bug。
    const qint64 payloadSize = 3 * 1024 * 1024 + 777; // 故意不对齐块边界
    QByteArray payload;
    payload.resize(payloadSize);
    QRandomGenerator seeded(QRandomGenerator::securelySeeded());
    for (qint64 index = 0; index < payloadSize; index += 4) {
        const quint32 word = seeded.generate();
        std::memcpy(payload.data() + index, &word,
                    std::min<qint64>(4, payloadSize - index));
    }

    const QString localSourcePath = localDir.filePath(QStringLiteral("upload-source.bin"));
    QFile sourceFile(localSourcePath);
    QVERIFY(sourceFile.open(QIODevice::WriteOnly));
    QCOMPARE(sourceFile.write(payload), payloadSize);
    sourceFile.close();

    const ConnectionTarget target = testTarget();
    SftpService service;
    const QString remotePath =
        QStringLiteral("/tmp/shellium-upload-pipeline-test-%1.bin").arg(QCoreApplication::applicationPid());

    QSignalSpy uploadOk(&service, &SftpService::operationSucceeded);
    QSignalSpy uploadFailed(&service, &SftpService::operationFailed);
    service.uploadFile(target, localSourcePath, remotePath,
                       SftpTransfer::ExistingFilePolicy::Overwrite);
    QVERIFY(uploadOk.wait(30000) || !uploadFailed.isEmpty());
    QVERIFY2(uploadFailed.isEmpty(),
             uploadFailed.isEmpty() ? "" : qPrintable(uploadFailed.constFirst().at(3).toString()));

    const QString localDownloadPath = localDir.filePath(QStringLiteral("download-result.bin"));
    QSignalSpy downloadOk(&service, &SftpService::operationSucceeded);
    QSignalSpy downloadFailed(&service, &SftpService::operationFailed);
    service.downloadFile(target, remotePath, localDownloadPath,
                         SftpTransfer::ExistingFilePolicy::Overwrite);
    QVERIFY(downloadOk.wait(30000) || !downloadFailed.isEmpty());
    QVERIFY2(downloadFailed.isEmpty(),
             downloadFailed.isEmpty() ? "" : qPrintable(downloadFailed.constFirst().at(3).toString()));

    QFile resultFile(localDownloadPath);
    QVERIFY(resultFile.open(QIODevice::ReadOnly));
    const QByteArray roundTripped = resultFile.readAll();
    resultFile.close();

    QCOMPARE(roundTripped.size(), payload.size());
    QVERIFY(roundTripped == payload);

    QSignalSpy removeOk(&service, &SftpService::operationSucceeded);
    service.removePath(target, remotePath, false);
    removeOk.wait(10000);
}

QTEST_MAIN(SftpUploadPipelineTest)
#include "SftpUploadPipelineTest.moc"
