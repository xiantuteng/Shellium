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
    // 不加这里，resolve() 就永远不会把用户请求匹配到新语言。
    return {QStringLiteral("en"), QStringLiteral("zh_CN")};
}

QVector<LanguageOption> LocalizationService::availableLanguages()
{
    return {
        {QStringLiteral("system"), tr("Follow System")},
        {QStringLiteral("en"), QStringLiteral("English")},
        {QStringLiteral("zh_CN"), QStringLiteral("简体中文")},
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
        // 再按语言前缀匹配：zh_HK -> zh_CN、en_GB -> en。
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
