#include "i18n/LocalizationService.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

namespace {

/** @brief 应用翻译资源在 Qt 资源系统中的固定前缀，与 CMake 的 RESOURCE_PREFIX 保持一致。 */
const QString kResourcePrefix = QStringLiteral(":/i18n/");

/** @brief 应用 .qm 文件名前缀，对应 qt_add_translations 的 TS 文件基名。 */
const QString kAppCatalog = QStringLiteral("shellium_");

/**
 * @brief Qt 标准按钮（QMessageBox / QDialogButtonBox）的源文案，原样登记给 lupdate。
 *
 * 这些文案本应由 qtbase_*.qm 翻译，但 Homebrew 的 qtbase 不带 translations 目录、打包也
 * 未必携带，于是不管选什么语言 QMessageBox 都是 Yes/No。把它们以 Qt 自身的上下文名收进
 * 本程序的 .ts，按钮就能跟随应用语言；上下文与原文必须和 QPlatformTheme::
 * defaultStandardButtonText()、QCocoaTheme::standardButtonText() 逐字一致，否则查不到。
 * 没有这份登记，lupdate 的 -no-obsolete 会把 .ts 里的这些条目当作废弃删掉。
 */
[[maybe_unused]] const char *const kQtStandardButtonTexts[] = {
    QT_TRANSLATE_NOOP("QPlatformTheme", "OK"),
    QT_TRANSLATE_NOOP("QPlatformTheme", "Save"),
    QT_TRANSLATE_NOOP("QPlatformTheme", "Save All"),
    QT_TRANSLATE_NOOP("QPlatformTheme", "Open"),
    QT_TRANSLATE_NOOP("QPlatformTheme", "&Yes"),
    QT_TRANSLATE_NOOP("QPlatformTheme", "Yes to &All"),
    QT_TRANSLATE_NOOP("QPlatformTheme", "&No"),
    QT_TRANSLATE_NOOP("QPlatformTheme", "N&o to All"),
    QT_TRANSLATE_NOOP("QPlatformTheme", "Abort"),
    QT_TRANSLATE_NOOP("QPlatformTheme", "Retry"),
    QT_TRANSLATE_NOOP("QPlatformTheme", "Ignore"),
    QT_TRANSLATE_NOOP("QPlatformTheme", "Close"),
    QT_TRANSLATE_NOOP("QPlatformTheme", "Cancel"),
    QT_TRANSLATE_NOOP("QPlatformTheme", "Discard"),
    QT_TRANSLATE_NOOP("QPlatformTheme", "Help"),
    QT_TRANSLATE_NOOP("QPlatformTheme", "Apply"),
    QT_TRANSLATE_NOOP("QPlatformTheme", "Reset"),
    QT_TRANSLATE_NOOP("QPlatformTheme", "Restore Defaults"),
    // macOS 的 Cocoa 主题把 Discard 换成了 “Don't Save”。
    QT_TRANSLATE_NOOP("QCocoaTheme", "Don't Save"),
};

} // namespace

LocalizationService::LocalizationService(QObject *parent)
    : QObject(parent)
    , m_currentLanguage(sourceLanguage())
{
}

LocalizationService::~LocalizationService()
{
    // 必须显式卸载：QTranslator 析构不会把自己从 QCoreApplication 的翻译器列表里摘掉，
    // 若只靠父子关系删除对象，应用退出时会解引用已释放的翻译器而崩溃。
    uninstallTranslators();
}

QString LocalizationService::sourceLanguage()
{
    return QStringLiteral("en");
}

QStringList LocalizationService::supportedLanguages()
{
    // 新增语言时，这里、availableLanguages() 与 CMake 的 TS_FILES 三处必须同步；只加 .ts
    // 不加这里，resolve() 就永远不会把用户请求匹配到新语言。源语言 en 无 .qm。
    return {
        QStringLiteral("en"),    QStringLiteral("fr"),    QStringLiteral("de"),
        QStringLiteral("it"),    QStringLiteral("ja"),    QStringLiteral("ko"),
        QStringLiteral("pt_BR"), QStringLiteral("ro"),    QStringLiteral("ru"),
        QStringLiteral("zh_CN"), QStringLiteral("es"),    QStringLiteral("zh_TW"),
        QStringLiteral("uk"),    QStringLiteral("ar"),
    };
}

QVector<LanguageOption> LocalizationService::availableLanguages()
{
    // 顺序即设置下拉的展示顺序；nativeName 用该语言自身写法，"system" 例外跟随当前界面语言。
    return {
        {QStringLiteral("system"), tr("Follow System")},
        {QStringLiteral("zh_CN"), QStringLiteral("简体中文")},
        {QStringLiteral("zh_TW"), QStringLiteral("繁體中文")},
        {QStringLiteral("en"), QStringLiteral("English")},
        {QStringLiteral("fr"), QStringLiteral("Français")},
        {QStringLiteral("de"), QStringLiteral("Deutsch")},
        {QStringLiteral("it"), QStringLiteral("Italiano")},
        {QStringLiteral("ko"), QStringLiteral("한국어")},
        {QStringLiteral("pt_BR"), QStringLiteral("Português (BR)")},
        {QStringLiteral("ro"), QStringLiteral("Română")},
        {QStringLiteral("ru"), QStringLiteral("Русский")},
        {QStringLiteral("es"), QStringLiteral("Español")},
        {QStringLiteral("uk"), QStringLiteral("Українська")},
        {QStringLiteral("ar"), QStringLiteral("العربية")},
        {QStringLiteral("ja"), QStringLiteral("日本語")},
    };
}

QString LocalizationService::resolve(const QString &stored) const
{
    const QStringList supported = supportedLanguages();
    const QString trimmed = stored.trimmed();

    const auto matchConcrete = [&supported](const QString &candidate) -> QString {
        if (candidate.isEmpty()) {
            return {};
        }
        const QString unified = QString(candidate).replace(QLatin1Char('-'), QLatin1Char('_'));
        // 先精确匹配 ll_CC。
        for (const QString &code : supported) {
            if (code.compare(unified, Qt::CaseInsensitive) == 0) {
                return code;
            }
        }
        // 中文按书写系统区分：Hant/TW/HK/MO -> 繁体，其余中文 -> 简体。放在语言前缀匹配
        // 之前，否则 zh_HK 会先落到列表里更靠前的 zh_CN。
        const QString lower = unified.toLower();
        if (lower.startsWith(QLatin1String("zh"))) {
            const bool traditional = lower.contains(QLatin1String("hant"))
                || lower.contains(QLatin1String("_tw")) || lower.contains(QLatin1String("_hk"))
                || lower.contains(QLatin1String("_mo"));
            const QString target = traditional ? QStringLiteral("zh_TW") : QStringLiteral("zh_CN");
            if (supported.contains(target)) {
                return target;
            }
        }
        // 再按语言前缀匹配：pt_PT -> pt_BR、en_GB -> en。
        const QString languageOnly = unified.section(QLatin1Char('_'), 0, 0).toLower();
        for (const QString &code : supported) {
            if (code.section(QLatin1Char('_'), 0, 0).toLower() == languageOnly) {
                return code;
            }
        }
        return {};
    };

    if (!trimmed.isEmpty() && trimmed.compare(QLatin1String("system"), Qt::CaseInsensitive) != 0) {
        const QString direct = matchConcrete(trimmed);
        if (!direct.isEmpty()) {
            return direct;
        }
        // 存了一个我们不支持的具体语言：按“跟随系统”处理，而不是径直落到源语言，这样系统
        // 语言恰好受支持时用户仍能得到本地化界面。
    }

    for (const QString &uiLanguage : QLocale::system().uiLanguages()) {
        const QString matched = matchConcrete(uiLanguage);
        if (!matched.isEmpty()) {
            return matched;
        }
    }
    return sourceLanguage();
}

void LocalizationService::uninstallTranslators()
{
    for (QTranslator **slot : {&m_appTranslator, &m_qtTranslator}) {
        if (*slot != nullptr) {
            QCoreApplication::removeTranslator(*slot);
            delete *slot;
            *slot = nullptr;
        }
    }
}

bool LocalizationService::apply(const QString &language)
{
    const QStringList supported = supportedLanguages();
    const QString target = language.trimmed();
    const bool wantsSource = target.isEmpty()
        || target.compare(QLatin1String("system"), Qt::CaseInsensitive) == 0
        || target == sourceLanguage();
    if (!wantsSource && !supported.contains(target)) {
        // 明确请求了一门我们没有翻译的语言：拒绝并保持当前状态，不静默回落。调用方若想
        // “跟随系统 / 尽力而为”，应先经过 resolve() 得到一个一定受支持的具体语言码。
        return false;
    }

    const QString effective = wantsSource ? sourceLanguage() : target;
    const bool needsCatalog = effective != sourceLanguage();

    QTranslator *appTranslator = nullptr;
    if (needsCatalog) {
        appTranslator = new QTranslator(this);
        if (!appTranslator->load(kAppCatalog + effective, kResourcePrefix)) {
            // .qm 没打进资源属于构建/打包缺陷，不能静默把界面留在半切换状态。
            delete appTranslator;
            return false;
        }
    }

    QTranslator *qtTranslator = nullptr;
    if (needsCatalog) {
        qtTranslator = new QTranslator(this);
        const QString qtDir = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
        // Qt 自带控件（标准按钮、文件对话框、行编辑右键菜单）的文案。英文是 Qt 的源语言，
        // 通常没有 qtbase_en.qm；加载失败只代表这些文案保持英文，不影响本程序自身翻译。
        if (!qtTranslator->load(QLocale(effective), QStringLiteral("qtbase"),
                                QStringLiteral("_"), qtDir)) {
            delete qtTranslator;
            qtTranslator = nullptr;
        }
    }

    uninstallTranslators();

    m_appTranslator = appTranslator;
    m_qtTranslator = qtTranslator;
    if (m_appTranslator != nullptr) {
        QCoreApplication::installTranslator(m_appTranslator);
    }
    if (m_qtTranslator != nullptr) {
        QCoreApplication::installTranslator(m_qtTranslator);
    }

    const QLocale locale(effective);
    QLocale::setDefault(locale);
    if (QGuiApplication *gui = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        gui->setLayoutDirection(locale.textDirection());
    }

    m_currentLanguage = effective;
    emit languageChanged(effective);
    return true;
}
