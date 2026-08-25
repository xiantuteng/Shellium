#include "settings/BundledFonts.h"

#include <QFontDatabase>
#include <QtGlobal>

#include <array>

namespace {

/** @brief 一款内置字体的资源路径与它注册后应当出现的族名。 */
struct BundledFontEntry {
    const char *resourcePath;
    const char *familyName;
};

/**
 * @brief 内置字体清单，顺序即设置界面的展示顺序。
 *
 * 两个 Medium 文件的族名已经被 scripts/rename_font_family.py 改成独立族名；上游原件声明的
 * 族名仍是基础族，Qt 会把它们并进 "Source Code Pro" / "Fira Mono"，界面上就选不到中等字重。
 * 详见 resources/fonts/NOTICE.md。
 */
constexpr std::array<BundledFontEntry, 14> kBundledFonts{{
    {":/resources/fonts/SourceCodePro-Regular.ttf", "Source Code Pro"},
    {":/resources/fonts/SourceCodeProMedium-Regular.ttf", "Source Code Pro Medium"},
    {":/resources/fonts/FiraMono-Regular.ttf", "Fira Mono"},
    {":/resources/fonts/FiraMonoMedium-Regular.ttf", "Fira Mono Medium"},
    {":/resources/fonts/Inconsolata-g.ttf", "Inconsolata-g"},
    {":/resources/fonts/AnonymousPro-Regular.ttf", "Anonymous Pro"},
    {":/resources/fonts/UbuntuMono-Regular.ttf", "Ubuntu Mono"},
    {":/resources/fonts/DroidSansMono.ttf", "Droid Sans Mono"},
    {":/resources/fonts/DejaVuSansMono.ttf", "DejaVu Sans Mono"},
    {":/resources/fonts/PTMono-Regular.ttf", "PT Mono"},
    {":/resources/fonts/CascadiaCode-Regular.ttf", "Cascadia Code"},
    {":/resources/fonts/FiraCode-Regular.ttf", "Fira Code"},
    {":/resources/fonts/JetBrainsMono-Regular.ttf", "JetBrains Mono"},
    // 上游文件名是 MesloLGS，name 表里的族名是 "Meslo LG S"（S 表示行距最紧的一档）。
    // 这里必须写族名而不是文件名，否则注册后校验会判定族名不符并把这一项丢掉。
    {":/resources/fonts/MesloLGS-Regular.ttf", "Meslo LG S"},
}};

} // namespace

namespace BundledFonts {

QStringList familyNames()
{
    QStringList families;
    families.reserve(static_cast<qsizetype>(kBundledFonts.size()));
    for (const BundledFontEntry &entry : kBundledFonts) {
        families.append(QString::fromLatin1(entry.familyName));
    }
    return families;
}

QStringList registerAll()
{
    // 记录已注册的族名而不是一个布尔量：字体资源注册失败时下次调用仍会重试，而成功的部分
    // 不会被重复 addApplicationFont（重复注册同一份数据会让 QFontDatabase 多出一份副本）。
    static QStringList registeredFamilies;

    for (const BundledFontEntry &entry : kBundledFonts) {
        const QString expectedFamily = QString::fromLatin1(entry.familyName);
        if (registeredFamilies.contains(expectedFamily)) {
            continue;
        }

        const QString resourcePath = QString::fromLatin1(entry.resourcePath);
        const int fontId = QFontDatabase::addApplicationFont(resourcePath);
        if (fontId < 0) {
            qWarning("内置字体注册失败：%s", qUtf8Printable(resourcePath));
            continue;
        }

        // 族名对不上说明字体文件被换过或 name 表被改动过：此时设置界面里那一项会指向一个
        // 并不存在的族，选中后被静默替换成别的字体。宁可警告并跳过，也不要留下假条目。
        const QStringList actualFamilies = QFontDatabase::applicationFontFamilies(fontId);
        if (!actualFamilies.contains(expectedFamily)) {
            qWarning("内置字体族名不符：%s 期望 \"%s\"，实际 \"%s\"",
                     qUtf8Printable(resourcePath),
                     qUtf8Printable(expectedFamily),
                     qUtf8Printable(actualFamilies.join(QLatin1String(", "))));
            QFontDatabase::removeApplicationFont(fontId);
            continue;
        }

        registeredFamilies.append(expectedFamily);
    }

    // 按清单顺序返回，调用方（设置界面）不必再排一次序。
    QStringList ordered;
    ordered.reserve(registeredFamilies.size());
    for (const BundledFontEntry &entry : kBundledFonts) {
        const QString family = QString::fromLatin1(entry.familyName);
        if (registeredFamilies.contains(family)) {
            ordered.append(family);
        }
    }
    return ordered;
}

} // namespace BundledFonts
