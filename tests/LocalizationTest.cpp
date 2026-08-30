#include "i18n/LocalizationService.h"
#include "settings/TerminalSettings.h"

#include <QApplication>
#include <QLocale>
#include <QSignalSpy>
#include <QTest>

/**
 * @brief LocalizationService 与 TerminalSettings 语言字段的行为测试。
 *
 * 源语言是英文（源码里的 tr() 字面量即英文），zh_CN 通过 :/i18n/shellium_zh_CN.qm 提供。
 * 覆盖：语言码解析永远落在受支持集合内、apply() 真的装载 .qm 并发信号、回到源语言会卸载
 * 翻译器、非法/缺失语言被明确拒绝，以及设置结构对语言标签的规范化往返。
 */
class LocalizationTest final : public QObject {
    Q_OBJECT

private slots:
    void resolvesExplicitSupportedLanguage();
    void resolvesUnknownToSourceOrSystem();
    void applyLoadsCatalogAndEmitsSignal();
    void applySourceLanguageUninstallsCatalog();
    void applyRejectsUnsupportedLanguage();
    void settingsNormalizesLanguageTag();
};

void LocalizationTest::resolvesExplicitSupportedLanguage()
{
    LocalizationService service;
    QCOMPARE(service.resolve(QStringLiteral("en")), QStringLiteral("en"));
    QCOMPARE(service.resolve(QStringLiteral("zh_CN")), QStringLiteral("zh_CN"));
    QCOMPARE(service.resolve(QStringLiteral("ZH-cn")), QStringLiteral("zh_CN"));
    QCOMPARE(service.resolve(QStringLiteral("zh-Hant-HK")), QStringLiteral("zh_CN"));
    QCOMPARE(service.resolve(QStringLiteral("en_GB")), QStringLiteral("en"));
}

void LocalizationTest::resolvesUnknownToSourceOrSystem()
{
    LocalizationService service;
    const QString resolved = service.resolve(QStringLiteral("xx_YY"));
    QVERIFY(LocalizationService::supportedLanguages().contains(resolved));
    QVERIFY(resolved != QStringLiteral("system"));
    QCOMPARE(service.resolve(QString()), service.resolve(QStringLiteral("system")));
    QCOMPARE(LocalizationService::sourceLanguage(), QStringLiteral("en"));
}

void LocalizationTest::applyLoadsCatalogAndEmitsSignal()
{
    LocalizationService service;
    QSignalSpy spy(&service, &LocalizationService::languageChanged);

    QVERIFY(service.apply(QStringLiteral("zh_CN")));
    QCOMPARE(service.currentLanguage(), QStringLiteral("zh_CN"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("zh_CN"));

    QCOMPARE(QCoreApplication::translate("LocalizationService", "Follow System"),
             QString::fromUtf8("跟随系统"));
    QCOMPARE(QCoreApplication::translate("QObject", "Database initialization failed"),
             QString::fromUtf8("数据库初始化失败"));
    QCOMPARE(QLocale().language(), QLocale::Chinese);
}

void LocalizationTest::applySourceLanguageUninstallsCatalog()
{
    LocalizationService service;
    QVERIFY(service.apply(QStringLiteral("zh_CN")));
    QVERIFY(service.apply(QStringLiteral("en")));
    QCOMPARE(service.currentLanguage(), QStringLiteral("en"));
    QCOMPARE(QCoreApplication::translate("LocalizationService", "Follow System"),
             QStringLiteral("Follow System"));
}

void LocalizationTest::applyRejectsUnsupportedLanguage()
{
    LocalizationService service;
    QVERIFY(service.apply(QStringLiteral("zh_CN")));
    QVERIFY(!service.apply(QStringLiteral("ja")));
    QCOMPARE(service.currentLanguage(), QStringLiteral("zh_CN"));
}

void LocalizationTest::settingsNormalizesLanguageTag()
{
    TerminalSettings settings;
    QCOMPARE(settings.language, QStringLiteral("system"));

    settings.language = QStringLiteral("  EN  ");
    QCOMPARE(settings.normalized().language, QStringLiteral("en"));

    settings.language = QStringLiteral("zh-cn");
    QCOMPARE(settings.normalized().language, QStringLiteral("zh_CN"));

    settings.language = QStringLiteral("not a locale!!");
    QCOMPARE(settings.normalized().language, QStringLiteral("system"));

    settings.language = QString();
    QCOMPARE(settings.normalized().language, QStringLiteral("system"));

    TerminalSettings other = settings.normalized();
    other.language = QStringLiteral("en");
    QVERIFY(!(settings.normalized() == other));
}

namespace {

int runLocalizationTest(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication application(argc, argv);
    LocalizationTest testObject;
    return QTest::qExec(&testObject, argc, argv);
}

} // namespace

int main(int argc, char **argv)
{
    return runLocalizationTest(argc, argv);
}

#include "LocalizationTest.moc"
