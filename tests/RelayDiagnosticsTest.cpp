#include "ssh/SshSessionNode.h"

#include <QTest>

/**
 * @brief 验证 exec 中继打开 session 通道失败时的诊断文案。
 *
 * OpenSSH 在 MaxSessions 用尽时只回 "error (2) open failed"，用户看到后会去排查网络和目标
 * 服务；判定是纯字符串逻辑，不需要真实服务器即可钉死。
 */
class RelayDiagnosticsTest final : public QObject {
    Q_OBJECT

private slots:
    /** @brief error (2) 必须保留原文并追加 MaxSessions 提示。 */
    void connectFailedSessionOpenMentionsMaxSessions();

    /** @brief 其他错误原样返回，不能误导到会话数上限。 */
    void otherErrorsAreReturnedUnchanged();
};

void RelayDiagnosticsTest::connectFailedSessionOpenMentionsMaxSessions()
{
    const QString raw = QStringLiteral("Channel opening failure: channel 81 error (2) open failed");
    const QString described = SshRelayDiagnostics::describeSessionOpenFailure(raw);
    QVERIFY2(described.startsWith(raw), qPrintable(described));
    QVERIFY2(described.contains(QStringLiteral("MaxSessions")), qPrintable(described));
    QVERIFY2(described.contains(QStringLiteral("AllowTcpForwarding")), qPrintable(described));
}

void RelayDiagnosticsTest::otherErrorsAreReturnedUnchanged()
{
    for (const QString &raw : {QStringLiteral("Channel opening failure: channel 3 error (1) administratively prohibited"),
                               QStringLiteral("Socket error: disconnected"),
                               QString()}) {
        QCOMPARE(SshRelayDiagnostics::describeSessionOpenFailure(raw), raw);
    }
}

QTEST_MAIN(RelayDiagnosticsTest)

#include "RelayDiagnosticsTest.moc"
