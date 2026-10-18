#include "update/UpdateChecker.h"

#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

/**
 * @brief 检查更新的版本比较、跳过规则、清单解析与端到端请求。
 *
 * 网络路径用 file:// 清单驱动真实的 QNetworkAccessManager：既走到了 finished 回调里的
 * 解析与判定，又不依赖外网或本机端口。
 */
class UpdateCheckerTest final : public QObject {
    Q_OBJECT

private slots:
    void versionComparisonIsNumericNotLexical();
    /** @brief 跳过只压制同一版本（及更低版本）的自动提示，更高版本照常提醒，手动检查不受影响。 */
    void skippedVersionSilencesOnlyAutomaticChecksForThatVersion();
    void manifestParsingMatchesTheWebsiteApi();
    void checkReportsAvailableUpdateFromManifest();
    void automaticCheckHonoursSkippedVersionButManualCheckDoesNot();
    /** @brief 自动检查尚未返回时再点手动检查，结果必须按手动检查回报，不能被静默丢掉。 */
    void manualRequestDuringAutomaticCheckIsNotSwallowed();
    void checkReportsUpToDateWhenManifestIsOlder();
    void unknownCurrentVersionFailsWithoutNetworkRequest();
    void missingManifestReportsFailure();

private:
    /** @brief 在临时目录写一份清单，返回 file:// 地址。 */
    QUrl writeManifest(const QByteArray &payload);

    QTemporaryDir m_directory;
    int m_manifestCounter = 0;
};

QUrl UpdateCheckerTest::writeManifest(const QByteArray &payload)
{
    const QString path =
        m_directory.filePath(QStringLiteral("latest-%1.json").arg(++m_manifestCounter));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size()) {
        qFatal("无法写入测试清单 %s", qPrintable(path));
    }
    return QUrl::fromLocalFile(path);
}

void UpdateCheckerTest::versionComparisonIsNumericNotLexical()
{
    // 按字符串比较 "0.10.0" < "0.9.0"，升级到两位数小版本那天提示就会失灵。
    QVERIFY(UpdateChecker::isNewer(QStringLiteral("0.10.0"), QStringLiteral("0.9.0")));
    QVERIFY(UpdateChecker::isNewer(QStringLiteral("0.3.6"), QStringLiteral("0.3.5")));
    QVERIFY(UpdateChecker::isNewer(QStringLiteral("v1.0.0"), QStringLiteral("0.3.5")));
    QVERIFY(!UpdateChecker::isNewer(QStringLiteral("0.3.5"), QStringLiteral("0.3.5")));
    QVERIFY(!UpdateChecker::isNewer(QStringLiteral("0.3.5"), QStringLiteral("0.3.6")));
    // 0.4 与 0.4.0 是同一个版本，不能因为段数不同被当成新版本。
    QVERIFY(!UpdateChecker::isNewer(QStringLiteral("0.4.0"), QStringLiteral("0.4")));
    QVERIFY(!UpdateChecker::isNewer(QStringLiteral("garbage"), QStringLiteral("0.3.5")));
    QVERIFY(!UpdateChecker::isNewer(QStringLiteral("0.4.0"), QString()));
}

void UpdateCheckerTest::skippedVersionSilencesOnlyAutomaticChecksForThatVersion()
{
    const QString current = QStringLiteral("0.3.5");
    QVERIFY(UpdateChecker::shouldNotify(QStringLiteral("0.4.0"), current, QString(), false));
    QVERIFY(!UpdateChecker::shouldNotify(QStringLiteral("0.4.0"), current,
                                         QStringLiteral("0.4.0"), false));
    // 跳过记录写的是 "v0.4.0" 或 "0.4" 也要认出是同一个版本。
    QVERIFY(!UpdateChecker::shouldNotify(QStringLiteral("0.4.0"), current,
                                         QStringLiteral("v0.4"), false));
    QVERIFY(UpdateChecker::shouldNotify(QStringLiteral("0.4.1"), current,
                                        QStringLiteral("0.4.0"), false));
    QVERIFY(UpdateChecker::shouldNotify(QStringLiteral("0.4.0"), current,
                                        QStringLiteral("0.4.0"), true));
    // 不比当前版本新的永远不提示，跳过与否都一样。
    QVERIFY(!UpdateChecker::shouldNotify(current, current, QString(), true));
}

void UpdateCheckerTest::manifestParsingMatchesTheWebsiteApi()
{
    // 与 shellium-web/src/worker/index.ts 的 /api/releases 返回格式一致（files 内容省略）。
    QString error;
    const std::optional<UpdateRelease> release = UpdateChecker::parseManifest(
        R"({"version":"0.3.5","publishedAt":"2026-10-16T02:25:41Z","files":{"macos":[]}})", &error);
    QVERIFY2(release.has_value(), qPrintable(error));
    QCOMPARE(release->version, QStringLiteral("0.3.5"));
    QCOMPARE(release->publishedAt, QDateTime(QDate(2026, 10, 16), QTime(2, 25, 41), QTimeZone::UTC));

    const std::optional<UpdateRelease> tagged =
        UpdateChecker::parseManifest(R"({"version":"v0.4.0"})", &error);
    QVERIFY(tagged.has_value());
    QCOMPARE(tagged->version, QStringLiteral("0.4.0"));
    QVERIFY(!tagged->publishedAt.isValid());

    // R2 里还没有 latest.json 时 Worker 返回 version: null。
    error.clear();
    QVERIFY(!UpdateChecker::parseManifest(R"({"version":null,"publishedAt":null,"files":{}})",
                                          &error).has_value());
    QVERIFY(!error.isEmpty());
    QVERIFY(!UpdateChecker::parseManifest("<html>502</html>", &error).has_value());
    QVERIFY(!UpdateChecker::parseManifest(R"({"version":"latest"})", &error).has_value());
}

void UpdateCheckerTest::checkReportsAvailableUpdateFromManifest()
{
    UpdateChecker checker;
    checker.setCurrentVersion(QStringLiteral("0.3.5"));
    checker.setEndpoint(writeManifest(R"({"version":"0.4.0","publishedAt":"2026-11-01T00:00:00Z"})"));
    QSignalSpy available(&checker, &UpdateChecker::updateAvailable);
    QSignalSpy failed(&checker, &UpdateChecker::checkFailed);

    checker.check(QString(), false);
    QVERIFY(checker.isChecking());
    QVERIFY2(available.wait(5000), failed.isEmpty() ? "没有结果" : qPrintable(failed.first().first().toString()));
    const auto release = available.first().at(0).value<UpdateRelease>();
    QCOMPARE(release.version, QStringLiteral("0.4.0"));
    QCOMPARE(available.first().at(1).toBool(), false);
    QVERIFY(!checker.isChecking());
}

void UpdateCheckerTest::automaticCheckHonoursSkippedVersionButManualCheckDoesNot()
{
    UpdateChecker checker;
    checker.setCurrentVersion(QStringLiteral("0.3.5"));
    checker.setEndpoint(writeManifest(R"({"version":"0.4.0"})"));
    QSignalSpy available(&checker, &UpdateChecker::updateAvailable);
    QSignalSpy upToDate(&checker, &UpdateChecker::noUpdateAvailable);

    checker.check(QStringLiteral("0.4.0"), false);
    QVERIFY(upToDate.wait(5000));
    QCOMPARE(available.size(), 0);
    QCOMPARE(upToDate.first().at(0).toString(), QStringLiteral("0.4.0"));

    checker.check(QStringLiteral("0.4.0"), true);
    QVERIFY(available.wait(5000));
    QCOMPARE(available.first().at(1).toBool(), true);
}

void UpdateCheckerTest::manualRequestDuringAutomaticCheckIsNotSwallowed()
{
    UpdateChecker checker;
    checker.setCurrentVersion(QStringLiteral("0.3.5"));
    checker.setEndpoint(writeManifest(R"({"version":"0.4.0"})"));
    QSignalSpy available(&checker, &UpdateChecker::updateAvailable);
    QSignalSpy upToDate(&checker, &UpdateChecker::noUpdateAvailable);

    checker.check(QStringLiteral("0.4.0"), false);
    QVERIFY(checker.isChecking());
    // 若这次手动检查被丢掉，返回的会是“已跳过、不必提示”，用户点了菜单却什么都看不到。
    checker.check(QStringLiteral("0.4.0"), true);
    QVERIFY2(available.wait(5000), upToDate.isEmpty() ? "没有结果" : "被当成了自动检查");
    QCOMPARE(upToDate.size(), 0);
    QCOMPARE(available.first().at(1).toBool(), true);
    QVERIFY(!checker.isChecking());
}

void UpdateCheckerTest::checkReportsUpToDateWhenManifestIsOlder()
{
    // 仓库里的版本号先于官网发布递增（例如本地 0.3.6、官网仍是 0.3.5），不能提示“降级”。
    UpdateChecker checker;
    checker.setCurrentVersion(QStringLiteral("0.3.6"));
    checker.setEndpoint(writeManifest(R"({"version":"0.3.5"})"));
    QSignalSpy available(&checker, &UpdateChecker::updateAvailable);
    QSignalSpy upToDate(&checker, &UpdateChecker::noUpdateAvailable);

    checker.check(QString(), true);
    QVERIFY(upToDate.wait(5000));
    QCOMPARE(available.size(), 0);
}

void UpdateCheckerTest::unknownCurrentVersionFailsWithoutNetworkRequest()
{
    // 测试与裸 QApplication 下没有版本号；必须同步失败、不发请求，主窗口测试才不会访问外网。
    QCoreApplication::setApplicationVersion(QString());
    UpdateChecker checker;
    checker.setEndpoint(writeManifest(R"({"version":"0.4.0"})"));
    QSignalSpy failed(&checker, &UpdateChecker::checkFailed);

    checker.check(QString(), false);
    QCOMPARE(failed.size(), 1);
    QVERIFY(!checker.isChecking());
}

void UpdateCheckerTest::missingManifestReportsFailure()
{
    UpdateChecker checker;
    checker.setCurrentVersion(QStringLiteral("0.3.5"));
    checker.setEndpoint(QUrl::fromLocalFile(m_directory.filePath(QStringLiteral("missing.json"))));
    QSignalSpy failed(&checker, &UpdateChecker::checkFailed);

    checker.check(QString(), true);
    QVERIFY(failed.wait(5000));
    QVERIFY(!failed.first().at(0).toString().isEmpty());
    QCOMPARE(failed.first().at(1).toBool(), true);
}

QTEST_MAIN(UpdateCheckerTest)
#include "UpdateCheckerTest.moc"
