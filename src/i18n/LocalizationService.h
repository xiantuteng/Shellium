#pragma once

#include <QObject>
#include <QString>
#include <QVector>

class QTranslator;

/**
 * @brief 一个可在界面里展示的候选语言。
 *
 * code 是持久化与运行时都使用的稳定标识：字面量 "system" 或 QLocale 名称（如 "zh_CN"、
 * "en"）。nativeName 用该语言自身的写法给出，便于用户在不认识当前界面语言时也能找到自己
 * 的语言；"system" 项例外，它的显示名跟随当前界面语言翻译。
 */
struct LanguageOption {
    QString code;
    QString nativeName;
};

/**
 * @brief 加载并切换应用界面语言的应用级服务。
 *
 * 单一职责：把一个语言码解析成本程序真正带有翻译的语言，装载对应的 Qt 资源 .qm 以及 Qt
 * 自带控件文案的 qtbase_*.qm，并同步 QLocale 默认值与布局方向。安装翻译器后 Qt 会自动向
 * 所有顶层控件投递 QEvent::LanguageChange，各窗口据此重建可见文案，因此本服务不持有任何
 * QWidget，也不直接调用界面代码。
 *
 * 必须在 GUI 主线程创建，且 apply() 要在任何窗口或对话框构造之前首次调用：设置对话框和
 * 终端视图在构造时就会读取已翻译文案，晚一步切换会让首帧停留在旧语言直到控件重建。
 */
class LocalizationService final : public QObject {
    Q_OBJECT

public:
    explicit LocalizationService(QObject *parent = nullptr);
    ~LocalizationService() override;

    /**
     * @brief 供设置界面展示的候选语言，顺序即下拉框顺序。
     *
     * 首项固定为 "system"（跟随操作系统）。其余为本程序已内置翻译的语言，包含源语言
     * zh_CN（它不需要 .qm，直接使用源码里的中文字面量）。
     */
    [[nodiscard]] static QVector<LanguageOption> availableLanguages();

    /**
     * @brief 本程序带有可用翻译的具体语言码集合，不含 "system"。
     *
     * 源语言 zh_CN 也在其中：它没有 .qm，apply() 对它只重置 QLocale 而不装载翻译器。
     */
    [[nodiscard]] static QStringList supportedLanguages();

    /** @brief 源语言（同时也是没有 .qm 的那一门语言）。 */
    [[nodiscard]] static QString sourceLanguage();

    /**
     * @brief 把存储值解析成一个一定受支持的具体语言码。
     * @param stored 设置里的原始值：可能是 "system"、受支持码、仅语言码或完全非法的输入。
     * @return supportedLanguages() 中的某一项；无法匹配时回退源语言，绝不返回 "system"。
     *
     * "system" 依次尝试 QLocale::system().uiLanguages()：先精确匹配 `ll_CC`，再按语言前缀
     * 匹配（例如系统为 zh_HK 时落到 zh_CN），都失败才用源语言。
     */
    [[nodiscard]] QString resolve(const QString &stored) const;

    /**
     * @brief 卸载旧翻译器并装载 language 对应的翻译。
     * @param language 具体语言码，通常来自 resolve()；传入 "system" 或未知值会被视为源语言。
     * @return 该语言需要 .qm 且成功装载，或该语言是源语言无需 .qm，返回 true；.qm 缺失
     *         返回 false 且不改变当前语言。
     *
     * 成功后：安装应用与 qtbase 翻译器、QLocale::setDefault(QLocale(language))、按该区域
     * 设置 qApp 布局方向，最后发出 languageChanged。qtbase_*.qm 缺失不视为失败，只是 Qt
     * 自带控件文案保持英文。
     */
    bool apply(const QString &language);

    /** @brief 最近一次 apply() 成功装载的语言码；初始为源语言。 */
    [[nodiscard]] QString currentLanguage() const { return m_currentLanguage; }

signals:
    void languageChanged(const QString &language);

private:
    void uninstallTranslators();

    QTranslator *m_appTranslator = nullptr;
    QTranslator *m_qtTranslator = nullptr;
    QString m_currentLanguage;
};
