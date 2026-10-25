#include "core/HostRecord.h"
#include "ssh/SecureCrtLaunch.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QLocalServer>
#include <QTemporaryDir>
#include <QTest>

/**
 * @brief 验证绿盟助手拉起 SecureCRT 的命令行能变成一次不落盘的连接。
 *
 * 票据和占位密码必须留在进程内字段里。普通启动参数不能被误认成一次连接。
 */
class SecureCrtLaunchTest final : public QObject {
    Q_OBJECT

private slots:
    void parsesNsfocusSecureCrtCommand();
    void explicitPortWinsOverHostSuffix();
    void parsesBracketedIpv6();
    void parsesSshUrl();
    void ignoresOrdinaryStartupArguments();
    void rejectsNonSshProtocol();
    void reportsMissingDestination();
    void shimForwardsWithoutReplacingRealClient();
    void runningInstanceReceivesTheTicketOnce();

private:
    [[nodiscard]] static QString writeExecutableCopy(const QString &directory, const QString &name);
};

QString SecureCrtLaunchTest::writeExecutableCopy(const QString &directory, const QString &name)
{
    const QString source = QCoreApplication::applicationFilePath();
    const QString destination = directory + QLatin1Char('/') + name;
    QFile::remove(destination);
    if (!QFile::copy(source, destination)) {
        return {};
    }
    QFile copied(destination);
    copied.setPermissions(QFile::permissions(source) | QFileDevice::ExeOwner);
    return destination;
}

void SecureCrtLaunchTest::parsesNsfocusSecureCrtCommand()
{
    const std::optional<SecureCrtLaunchRequest> request = parseSecureCrtLaunch({
        QStringLiteral("SecureCRT"),
        QStringLiteral("/N"),
        QStringLiteral("10.104.18.8"),
        QStringLiteral("/SSH2"),
        QStringLiteral("/P"),
        QStringLiteral("60022"),
        QStringLiteral("/AUTH"),
        QStringLiteral("keyboard-interactive"),
        QStringLiteral("/PASSWORD"),
        QStringLiteral("place18holder"),
        QStringLiteral("root#OSMS-V5.6R10F01-SIGN-ticket@10.1.2.3"),
    });

    QVERIFY(request.has_value());
    QCOMPARE(request->error, SecureCrtLaunchRequest::Error::None);
    QCOMPARE(request->target.displayName, QStringLiteral("10.104.18.8"));
    QCOMPARE(request->target.host, QStringLiteral("10.1.2.3"));
    QCOMPARE(request->target.port, quint16{60022});
    QCOMPARE(request->target.username, QStringLiteral("root"));
    QCOMPARE(request->target.loginUsername, QStringLiteral("root#OSMS-V5.6R10F01-SIGN-ticket"));
    QCOMPARE(request->target.password, QStringLiteral("place18holder"));
    QCOMPARE(request->target.authPreference, ConnectionTarget::AuthPreference::KeyboardInteractive);
    QVERIFY(!request->target.connectionSpecifier().contains(QStringLiteral("ticket")));
    QCOMPARE(HostRecord::fromConnectionTarget(request->target).username, QStringLiteral("root"));
}

void SecureCrtLaunchTest::explicitPortWinsOverHostSuffix()
{
    const std::optional<SecureCrtLaunchRequest> request = parseSecureCrtLaunch({
        QStringLiteral("SecureCRT"),
        QStringLiteral("/SSH2"),
        QStringLiteral("/P"),
        QStringLiteral("22"),
        QStringLiteral("ops#ticket@127.0.0.1:2222"),
    });
    QVERIFY(request.has_value());
    QCOMPARE(request->target.host, QStringLiteral("127.0.0.1"));
    QCOMPARE(request->target.port, quint16{22});
    QCOMPARE(request->target.loginUsername, QStringLiteral("ops#ticket"));
}

void SecureCrtLaunchTest::parsesBracketedIpv6()
{
    const std::optional<SecureCrtLaunchRequest> request = parseSecureCrtLaunch({
        QStringLiteral("SecureCRT"),
        QStringLiteral("/SSH2"),
        QStringLiteral("ops#ticket@[2001:db8::8]:2222"),
    });
    QVERIFY(request.has_value());
    QCOMPARE(request->error, SecureCrtLaunchRequest::Error::None);
    QCOMPARE(request->target.host, QStringLiteral("2001:db8::8"));
    QCOMPARE(request->target.port, quint16{2222});
    QCOMPARE(request->target.username, QStringLiteral("ops"));
}

void SecureCrtLaunchTest::parsesSshUrl()
{
    const std::optional<SecureCrtLaunchRequest> request = parseSecureCrtLaunch({
        QStringLiteral("SecureCRT"),
        QStringLiteral("ssh://ops%23ticket:place18holder@10.0.0.8:60022"),
    });
    QVERIFY(request.has_value());
    QCOMPARE(request->target.host, QStringLiteral("10.0.0.8"));
    QCOMPARE(request->target.port, quint16{60022});
    QCOMPARE(request->target.username, QStringLiteral("ops"));
    QCOMPARE(request->target.loginUsername, QStringLiteral("ops#ticket"));
    QCOMPARE(request->target.password, QStringLiteral("place18holder"));
}

void SecureCrtLaunchTest::ignoresOrdinaryStartupArguments()
{
    QVERIFY(!parseSecureCrtLaunch({
        QStringLiteral("Shellium"),
        QStringLiteral("--lang"),
        QStringLiteral("zh_CN"),
        QStringLiteral("-psn_0_12345"),
    }).has_value());
}

void SecureCrtLaunchTest::rejectsNonSshProtocol()
{
    const std::optional<SecureCrtLaunchRequest> request = parseSecureCrtLaunch({
        QStringLiteral("SecureCRT"),
        QStringLiteral("/TELNET"),
        QStringLiteral("10.0.0.8"),
    });
    QVERIFY(request.has_value());
    QCOMPARE(request->error, SecureCrtLaunchRequest::Error::UnsupportedProtocol);
    QVERIFY(!secureCrtLaunchErrorText(*request).isEmpty());
}

void SecureCrtLaunchTest::reportsMissingDestination()
{
    const std::optional<SecureCrtLaunchRequest> request = parseSecureCrtLaunch({
        QStringLiteral("SecureCRT"),
        QStringLiteral("/SSH2"),
        QStringLiteral("/P"),
        QStringLiteral("22"),
    });
    QVERIFY(request.has_value());
    QCOMPARE(request->error, SecureCrtLaunchRequest::Error::MissingDestination);
}

void SecureCrtLaunchTest::shimForwardsWithoutReplacingRealClient()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString shellium = writeExecutableCopy(directory.path(), QStringLiteral("shellium-copy"));
    QVERIFY(!shellium.isEmpty());

    const QString bundle = directory.filePath(QStringLiteral("SecureCRT.app"));
    const SecureCrtShimResult installed = ensureSecureCrtShim(shellium, bundle);
    QCOMPARE(installed.status, SecureCrtShimResult::Status::Installed);

    const QString executable = bundle + QStringLiteral("/Contents/MacOS/SecureCRT");
    QFile script(executable);
    QVERIFY(script.open(QIODevice::ReadOnly));
    const QString scriptText = QString::fromUtf8(script.readAll());
    script.close();
    QVERIFY(scriptText.contains(shellium));
    QVERIFY(QFileInfo(executable).isExecutable());
    QCOMPARE(ensureSecureCrtShim(shellium, bundle).status, SecureCrtShimResult::Status::Unchanged);

    const QString updatedExecutable = writeExecutableCopy(directory.path(), QStringLiteral("shellium-updated"));
    QVERIFY(!updatedExecutable.isEmpty());
    QCOMPARE(ensureSecureCrtShim(updatedExecutable, bundle).status, SecureCrtShimResult::Status::Updated);
    QFile updatedScript(executable);
    QVERIFY(updatedScript.open(QIODevice::ReadOnly));
    QVERIFY(QString::fromUtf8(updatedScript.readAll()).contains(updatedExecutable));
    updatedScript.close();

    QTemporaryDir realClientDir;
    QVERIFY(realClientDir.isValid());
    const QString realBundle = realClientDir.filePath(QStringLiteral("SecureCRT.app"));
    const QString realExecutable = realBundle + QStringLiteral("/Contents/MacOS/SecureCRT");
    QVERIFY(QDir().mkpath(realBundle + QStringLiteral("/Contents/MacOS")));
    QVERIFY(QDir().mkpath(realBundle + QStringLiteral("/Contents")));
    QFile plist(realBundle + QStringLiteral("/Contents/Info.plist"));
    QVERIFY(plist.open(QIODevice::WriteOnly));
    plist.write("<plist><dict><key>CFBundleIdentifier</key><string>com.vandyke.SecureCRT</string></dict></plist>");
    plist.close();
    QFile realBinary(realExecutable);
    QVERIFY(realBinary.open(QIODevice::WriteOnly));
    realBinary.write("REAL");
    realBinary.close();

    const SecureCrtShimResult left = ensureSecureCrtShim(shellium, realBundle);
    QCOMPARE(left.status, SecureCrtShimResult::Status::LeftExistingClient);
    QVERIFY(realBinary.open(QIODevice::ReadOnly));
    QCOMPARE(realBinary.readAll(), QByteArray("REAL"));
}

void SecureCrtLaunchTest::runningInstanceReceivesTheTicketOnce()
{
    const QString serverName = QStringLiteral("shellium-securecrt-test-%1")
                                   .arg(QCoreApplication::applicationPid());
    QLocalServer::removeServer(serverName);

    SecureCrtLaunchRequest received;
    bool gotRequest = false;
    QString listenError;
    QObject owner;
    SecureCrtLaunchServer *server = startSecureCrtLaunchServer(
        &owner,
        serverName,
        [&received, &gotRequest](const SecureCrtLaunchRequest &request) {
            received = request;
            gotRequest = true;
        },
        &listenError);
    QVERIFY2(server != nullptr, qPrintable(listenError));

    SecureCrtLaunchRequest outgoing;
    outgoing.target.host = QStringLiteral("10.1.2.3");
    outgoing.target.port = 60022;
    outgoing.target.username = QStringLiteral("root");
    outgoing.target.loginUsername = QStringLiteral("root#ticket");
    outgoing.target.password = QStringLiteral("place18holder");
    outgoing.target.displayName = QStringLiteral("10.104.18.8");
    outgoing.target.authPreference = ConnectionTarget::AuthPreference::KeyboardInteractive;

    QString deliverError;
    QVERIFY2(deliverSecureCrtLaunch(serverName, outgoing, 2000, &deliverError), qPrintable(deliverError));
    QVERIFY(gotRequest);
    QCOMPARE(received.target.host, outgoing.target.host);
    QCOMPARE(received.target.port, outgoing.target.port);
    QCOMPARE(received.target.username, QStringLiteral("root"));
    QCOMPARE(received.target.loginUsername, QStringLiteral("root#ticket"));
    QCOMPARE(received.target.password, QStringLiteral("place18holder"));
    QCOMPARE(received.target.authPreference, ConnectionTarget::AuthPreference::KeyboardInteractive);
    QLocalServer::removeServer(serverName);
}

QTEST_MAIN(SecureCrtLaunchTest)
#include "SecureCrtLaunchTest.moc"
