#include "i18n/LocalizationService.h"
#include "settings/TerminalSettings.h"

#include <QApplication>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
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
    /** @brief 验证所有语言的终端提示保留回车换行，防止下一行继承上一行的光标列。 */
    void terminalMessagesPreserveCarriageReturns();
    void resolvesExplicitSupportedLanguage();
    void resolvesUnknownToSourceOrSystem();
    void applyLoadsCatalogAndEmitsSignal();
    void applySourceLanguageUninstallsCatalog();
    void untranslatedLanguageStillAppliesAndFallsBackToEnglish();
    void arabicSetsRightToLeftLayout();
    void standardButtonsFollowAppLanguage();
    void applyRejectsUnsupportedLanguage();
    void settingsNormalizesLanguageTag();
};

/**
 * @brief 检查实际编译的翻译资源是否保留终端协议需要的 CRLF。
 *
 * 此测试无参数、无返回值；资源加载失败或控制字符缺失时由 Qt Test 报告失败。
 * LF 只下移光标，CR 才归零列位置，因此不能用普通界面文本的换行规则处理终端提示。
 * 遍历全部语言与全部含 CRLF 的终端提示，同时检查顺序和数量，覆盖启动、认证和重连。
 */
void LocalizationTest::terminalMessagesPreserveCarriageReturns()
{
    // 使用与 TerminalPane 相同的上下文和源字符串，确保验证的是运行时真正读到的译文。
    const QList<const char *> sources = {
        "Starting local terminal…\r\n",
        "Permission denied, please try again.\r\n%1's password: ",
        "\r\nCould not establish the nested connection to %1: %2\r\n",
        "\r\nNo server to reconnect to; press x to close this tab\r\n",
        "\r\nReconnecting manually…\r\n",
        "Shellium could not take over this connection (%1); handing it to the remote ssh to run directly.\r\n"
    };
    LocalizationService service;
    for (const QString &language : LocalizationService::supportedLanguages()) {
        QVERIFY(service.apply(language));
        for (const char *source : sources) {
            const QString translated = QCoreApplication::translate("TerminalPane", source);
            // 仅提取控制字符，允许各语言自由调整文字，同时禁止遗漏 CR 或意外增加 LF。
            QString controls;
            for (const QChar character : translated) {
                if (character == QLatin1Char('\r') || character == QLatin1Char('\n')) {
                    controls += character;
                }
            }
            const QString expected = QStringLiteral("\r\n").repeated(
                QString::fromUtf8(source).count(QLatin1Char('\n')));
            QVERIFY2(controls == expected, qPrintable(language + QStringLiteral(": ")
                                                       + QString::fromUtf8(source)));
        }
    }
}

void LocalizationTest::resolvesExplicitSupportedLanguage()
{
    LocalizationService service;
    QCOMPARE(service.resolve(QStringLiteral("en")), QStringLiteral("en"));
    QCOMPARE(service.resolve(QStringLiteral("zh_CN")), QStringLiteral("zh_CN"));
    QCOMPARE(service.resolve(QStringLiteral("ZH-cn")), QStringLiteral("zh_CN"));
    QCOMPARE(service.resolve(QStringLiteral("zh-Hans-SG")), QStringLiteral("zh_CN"));
    QCOMPARE(service.resolve(QStringLiteral("zh-Hant-HK")), QStringLiteral("zh_TW"));
    QCOMPARE(service.resolve(QStringLiteral("zh_TW")), QStringLiteral("zh_TW"));
    QCOMPARE(service.resolve(QStringLiteral("en_GB")), QStringLiteral("en"));
    QCOMPARE(service.resolve(QStringLiteral("fr-CA")), QStringLiteral("fr"));
    QCOMPARE(service.resolve(QStringLiteral("pt")), QStringLiteral("pt_BR"));
    QCOMPARE(service.resolve(QStringLiteral("ar_EG")), QStringLiteral("ar"));
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

    // 无 Q_OBJECT 的嵌套对话框类（用 Q_DECLARE_TR_FUNCTIONS）其 tr() 上下文必须与 lupdate
    // 提取时一致，否则按需弹出的“新建主机”等对话框会退回英文源文案。
    QCOMPARE(QCoreApplication::translate("HostRecordDialog", "Host configuration"),
             QString::fromUtf8("主机配置"));
    QCOMPARE(QCoreApplication::translate("PortForwardRuleDialog", "New port forwarding"),
             QString::fromUtf8("新建端口转发"));
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

void LocalizationTest::untranslatedLanguageStillAppliesAndFallsBackToEnglish()
{
    LocalizationService service;
    // 每种受支持语言的 .qm 都打进了资源；apply() 必须成功，QLocale 随之切换。未翻译的
    // 条目由 Qt 自动回落到英文源，因此对任意受支持语言 apply() 都不会失败。
    QVERIFY(service.apply(QStringLiteral("fr")));
    QCOMPARE(service.currentLanguage(), QStringLiteral("fr"));
    QCOMPARE(QLocale().language(), QLocale::French);
    // fr 已有机翻初稿，常见词条应已本地化。
    QCOMPARE(QCoreApplication::translate("HostManagerWidget", "New host"),
             QString::fromUtf8("Nouvel hôte"));

    // 繁体中文由 OpenCC 从 zh_CN 转换而来，应当已有译文。
    QVERIFY(service.apply(QStringLiteral("zh_TW")));
    QCOMPARE(QCoreApplication::translate("SettingsDialog", "Settings"),
             QString::fromUtf8("設定"));
}

void LocalizationTest::arabicSetsRightToLeftLayout()
{
    if (qApp == nullptr) {
        QSKIP("needs a QGuiApplication");
    }
    LocalizationService service;
    QVERIFY(service.apply(QStringLiteral("ar")));
    QCOMPARE(qApp->layoutDirection(), Qt::RightToLeft);
    QVERIFY(service.apply(QStringLiteral("en")));
    QCOMPARE(qApp->layoutDirection(), Qt::LeftToRight);
}

void LocalizationTest::standardButtonsFollowAppLanguage()
{
    // QMessageBox 的标准按钮文案来自 Qt 的 QPlatformTheme 上下文。Homebrew/打包后的 Qt 往往
    // 没有 qtbase_*.qm，所以本程序自己的 .qm 必须收录这些条目，否则不管选什么语言都是 Yes/No。
    LocalizationService service;
    QVERIFY(service.apply(QStringLiteral("zh_CN")));
    {
        QMessageBox box(QMessageBox::Question, QStringLiteral("t"), QStringLiteral("m"),
                        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        QCOMPARE(box.button(QMessageBox::Yes)->text(), QString::fromUtf8("是(&Y)"));
        QCOMPARE(box.button(QMessageBox::No)->text(), QString::fromUtf8("否(&N)"));
        QCOMPARE(box.button(QMessageBox::Cancel)->text(), QString::fromUtf8("取消"));
    }

    // 每门受支持语言都要有译文；这三条在所有语言里都与英文原文不同，可以直接判断是否命中。
    for (const QString &language : LocalizationService::supportedLanguages()) {
        if (language == LocalizationService::sourceLanguage()) {
            continue;
        }
        QVERIFY(service.apply(language));
        for (const char *source : {"&Yes", "Cancel", "Restore Defaults"}) {
            QVERIFY2(QCoreApplication::translate("QPlatformTheme", source)
                         != QLatin1String(source),
                     qPrintable(language + QStringLiteral(": ") + QLatin1String(source)));
        }
        QVERIFY2(QCoreApplication::translate("QCocoaTheme", "Don't Save")
                     != QLatin1String("Don't Save"),
                 qPrintable(language));
    }

    QVERIFY(service.apply(QStringLiteral("en")));
    QMessageBox box(QMessageBox::Question, QStringLiteral("t"), QStringLiteral("m"),
                    QMessageBox::Yes);
    QCOMPARE(box.button(QMessageBox::Yes)->text(), QStringLiteral("&Yes"));
}

void LocalizationTest::applyRejectsUnsupportedLanguage()
{
    LocalizationService service;
    QVERIFY(service.apply(QStringLiteral("zh_CN")));
    // "th" 不在受支持列表里：apply() 返回 false 且不改变当前语言。
    QVERIFY(!LocalizationService::supportedLanguages().contains(QStringLiteral("th")));
    QVERIFY(!service.apply(QStringLiteral("th")));
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
