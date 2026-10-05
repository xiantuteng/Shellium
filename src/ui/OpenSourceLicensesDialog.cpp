#include "ui/OpenSourceLicensesDialog.h"

#include "i18n/Retranslate.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QSplitter>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <utility>

namespace {

/** @brief LGPL 源代码索取入口；与关于页的官网链接保持同一地址。 */
constexpr auto kSourceRequestUrl = "https://shellium.net";

/** @brief 多个许可证文件之间的分隔线，纯文本里也一眼能看出边界。 */
constexpr auto kLicenseSeparator = "\n\n----------------------------------------"
                                   "----------------------------------------\n\n";

OpenSourceComponent component(const char *name,
                              const char *license,
                              const char *homepage,
                              QStringList licenseFiles,
                              const char *attribution = "",
                              bool lgpl = false)
{
    return OpenSourceComponent{QString::fromUtf8(name),
                               QString::fromUtf8(license),
                               QString::fromUtf8(homepage),
                               QString::fromUtf8(attribution),
                               std::move(licenseFiles),
                               lgpl};
}

} // namespace

namespace OpenSourceNotices {

QList<OpenSourceComponent> components()
{
    // LGPL 组件放最前：声明里的“可替换、可索取源代码”说的就是它们，用户最先看到。
    return {
        component("Qt", "LGPL-3.0-only", "https://www.qt.io/",
                  {QStringLiteral("LGPL-3.0.txt"), QStringLiteral("GPL-3.0.txt")},
                  "Copyright (C) The Qt Company Ltd. and other contributors.\n"
                  "Qt itself contains third-party components; see "
                  "https://doc.qt.io/qt-6/licenses-used-in-qt.html",
                  true),
        component("libssh", "LGPL-2.1-or-later", "https://www.libssh.org/",
                  {QStringLiteral("libssh-COPYING.txt")},
                  "Copyright (C) The libssh team.", true),
        component("GLib", "LGPL-2.1-or-later", "https://gitlab.gnome.org/GNOME/glib",
                  {QStringLiteral("LGPL-2.1.txt")},
                  "Copyright (C) The GLib authors.", true),
        component("GNU gettext runtime (libintl)", "LGPL-2.1-or-later",
                  "https://www.gnu.org/software/gettext/",
                  {QStringLiteral("LGPL-2.1.txt")},
                  "Copyright (C) Free Software Foundation, Inc.", true),

        component("libvterm", "MIT", "https://www.leonerd.org.uk/code/libvterm/",
                  {QStringLiteral("libvterm-LICENSE.txt")}),
        component("libsodium", "ISC", "https://github.com/jedisct1/libsodium",
                  {QStringLiteral("libsodium-LICENSE.txt")}),
        component("spdlog", "MIT", "https://github.com/gabime/spdlog",
                  {QStringLiteral("spdlog-LICENSE.txt")}),
        component("{fmt}", "MIT", "https://github.com/fmtlib/fmt",
                  {QStringLiteral("fmt-LICENSE.txt")}),
        component("minizip", "Zlib", "https://github.com/madler/zlib/tree/develop/contrib/minizip",
                  {QStringLiteral("minizip-LICENSE.txt")}),
        component("zlib", "Zlib", "https://zlib.net/", {QStringLiteral("zlib-LICENSE.txt")}),
        component("OpenSSL", "Apache-2.0", "https://openssl-library.org/",
                  {QStringLiteral("openssl-LICENSE.txt")}),

        // 以下是 Qt 运行时间接带入安装包的动态库，同样要随包附带许可证。
        component("ICU", "Unicode-3.0", "https://icu.unicode.org/",
                  {QStringLiteral("icu-LICENSE.txt")}),
        component("D-Bus", "AFL-2.1 OR GPL-2.0-or-later",
                  "https://www.freedesktop.org/wiki/Software/dbus/",
                  {QStringLiteral("dbus-COPYING.txt"), QStringLiteral("AFL-2.1.txt")},
                  "Shellium uses D-Bus under the terms of the Academic Free License 2.1."),
        component("HarfBuzz", "MIT-Modern-Variant", "https://harfbuzz.github.io/",
                  {QStringLiteral("harfbuzz-COPYING.txt")}),
        // FTL 第 3 节要求在产品文档中原样出现这句署名。
        component("FreeType", "FTL", "https://freetype.org/",
                  {QStringLiteral("freetype-LICENSE.txt"), QStringLiteral("FTL.txt")},
                  "Portions of this software are copyright © 2026 The FreeType Project "
                  "(https://freetype.org). All rights reserved."),
        component("Graphite", "MIT OR LGPL-2.1-or-later OR MPL-2.0 OR GPL-2.0-or-later",
                  "https://github.com/silnrsi/graphite",
                  {QStringLiteral("graphite2-LICENSE.txt")},
                  "Shellium uses Graphite under the terms of the MIT License."),
        component("libpng", "libpng-2.0", "http://www.libpng.org/pub/png/libpng.html",
                  {QStringLiteral("libpng-LICENSE.txt")}),
        // IJG 许可证要求在文档中注明基于 IJG 的工作。
        component("libjpeg-turbo", "IJG AND BSD-3-Clause AND Zlib", "https://libjpeg-turbo.org/",
                  {QStringLiteral("libjpeg-turbo-LICENSE.md.txt"),
                   QStringLiteral("libjpeg-turbo-README.ijg.txt")},
                  "This software is based in part on the work of the Independent JPEG Group."),
        component("PCRE2", "BSD-3-Clause WITH PCRE2-exception",
                  "https://github.com/PCRE2Project/pcre2", {QStringLiteral("pcre2-LICENCE.txt")}),
        component("Zstandard", "BSD-3-Clause", "https://github.com/facebook/zstd",
                  {QStringLiteral("zstd-LICENSE.txt")}),
        component("Brotli", "MIT", "https://github.com/google/brotli",
                  {QStringLiteral("brotli-LICENSE.txt")}),
        component("double-conversion", "BSD-3-Clause", "https://github.com/google/double-conversion",
                  {QStringLiteral("double-conversion-LICENSE.txt")}),
        component("libb2 (BLAKE2)", "CC0-1.0", "https://github.com/BLAKE2/libb2",
                  {QStringLiteral("CC0-1.0.txt")}),
        component("MD4C", "MIT", "https://github.com/mity/md4c",
                  {QStringLiteral("md4c-LICENSE.txt")}),

        // Windows 安装包随附的 MinGW 运行时。
        component("MinGW-w64 winpthreads", "MIT AND BSD-3-Clause", "https://www.mingw-w64.org/",
                  {QStringLiteral("winpthreads-COPYING.txt")}),
        component("GCC runtime libraries (libgcc, libstdc++)",
                  "GPL-3.0-or-later WITH GCC-exception-3.1", "https://gcc.gnu.org/",
                  {QStringLiteral("GCC-RUNTIME-EXCEPTION.txt"), QStringLiteral("GPL-3.0.txt")}),

        component("Simple Icons", "CC0-1.0", "https://simpleicons.org/",
                  {QStringLiteral("CC0-1.0.txt")},
                  "Linux distribution names and logos are trademarks of their respective owners "
                  "and are used only to identify the remote system type; this does not imply "
                  "endorsement."),

        // 内置终端字体：名称必须与 BundledFonts::familyNames() 一致，测试据此核对没有漏项。
        component("Source Code Pro", "OFL-1.1", "https://github.com/adobe-fonts/source-code-pro",
                  {QStringLiteral("fonts/SourceCodePro-OFL.txt")}),
        component("Source Code Pro Medium", "OFL-1.1",
                  "https://github.com/adobe-fonts/source-code-pro",
                  {QStringLiteral("fonts/SourceCodePro-OFL.txt")},
                  "Modified from Source Code Pro Medium: font family name changed."),
        component("Fira Mono", "OFL-1.1", "https://github.com/mozilla/Fira",
                  {QStringLiteral("fonts/FiraMono-OFL.txt")}),
        component("Fira Mono Medium", "OFL-1.1", "https://github.com/mozilla/Fira",
                  {QStringLiteral("fonts/FiraMono-OFL.txt")},
                  "Modified from Fira Mono Medium: font family name changed."),
        component("Inconsolata-g", "OFL-1.1", "https://github.com/chrissimpkins/codeface",
                  {QStringLiteral("fonts/Inconsolata-g-LICENSE.txt"),
                   QStringLiteral("OFL-1.1.txt")}),
        component("Anonymous Pro", "OFL-1.1",
                  "https://www.marksimonson.com/fonts/view/anonymous-pro",
                  {QStringLiteral("fonts/AnonymousPro-OFL.txt")}),
        component("Ubuntu Mono", "Ubuntu-font-1.0", "https://design.ubuntu.com/font",
                  {QStringLiteral("fonts/UbuntuMono-UFL.txt")}),
        component("Droid Sans Mono", "Apache-2.0", "https://github.com/chrissimpkins/codeface",
                  {QStringLiteral("fonts/DroidSansMono-LICENSE.txt"),
                   QStringLiteral("Apache-2.0.txt")}),
        component("DejaVu Sans Mono", "Bitstream-Vera", "https://dejavu-fonts.github.io/",
                  {QStringLiteral("fonts/DejaVuSansMono-LICENSE.txt")}),
        component("PT Mono", "OFL-1.1", "https://www.paratype.com/fonts/pt/pt-mono",
                  {QStringLiteral("fonts/PTMono-OFL.txt")}),
        component("Cascadia Code", "OFL-1.1", "https://github.com/microsoft/cascadia-code",
                  {QStringLiteral("fonts/CascadiaCode-OFL.txt")}),
        component("Fira Code", "OFL-1.1", "https://github.com/tonsky/FiraCode",
                  {QStringLiteral("fonts/FiraCode-OFL.txt")}),
        component("JetBrains Mono", "OFL-1.1", "https://github.com/JetBrains/JetBrainsMono",
                  {QStringLiteral("fonts/JetBrainsMono-OFL.txt")}),
        component("Meslo LG S", "Apache-2.0", "https://github.com/andreberg/Meslo-Font",
                  {QStringLiteral("fonts/MesloLGS-LICENSE.txt"),
                   QStringLiteral("Apache-2.0.txt")}),
    };
}

QString resourcePath(const QString &licenseFile)
{
    return QStringLiteral(":/licenses/") + licenseFile;
}

QString licenseText(const OpenSourceComponent &component)
{
    QStringList parts;
    for (const QString &file : component.licenseFiles) {
        QFile source(resourcePath(file));
        if (!source.open(QIODevice::ReadOnly)) {
            return {};
        }
        QString text = QString::fromUtf8(source.readAll());
        // 部分上游文件（如 PT Mono 的 OFL）带 UTF-8 BOM，直接显示会多出一个不可见字符。
        if (text.startsWith(QChar(0xFEFF))) {
            text.remove(0, 1);
        }
        parts.append(text.trimmed());
    }
    return parts.join(QString::fromLatin1(kLicenseSeparator));
}

} // namespace OpenSourceNotices

OpenSourceLicensesDialog::OpenSourceLicensesDialog(QWidget *parent)
    : QDialog(parent)
    , m_components(OpenSourceNotices::components())
    , m_noticeLabel(new QLabel(this))
    , m_componentList(new QListWidget(this))
    , m_licenseView(new QTextBrowser(this))
{
    setObjectName(QStringLiteral("openSourceLicensesDialog"));
    resize(860, 620);

    m_noticeLabel->setObjectName(QStringLiteral("openSourceNoticeLabel"));
    m_noticeLabel->setWordWrap(true);
    m_noticeLabel->setTextFormat(Qt::RichText);
    m_noticeLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_noticeLabel->setOpenExternalLinks(true);

    m_componentList->setObjectName(QStringLiteral("openSourceComponentList"));
    // 横纵滚动条同时存在时，两者交汇处会留下一个空白角块；全局滚动条策略空闲时隐藏滚动条，
    // 角块却仍在，看起来像列表右下角多了个小方框。只保留纵向滚动，过长的组件名省略显示。
    m_componentList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_componentList->setTextElideMode(Qt::ElideRight);
    for (const OpenSourceComponent &entry : std::as_const(m_components)) {
        auto *item = new QListWidgetItem(entry.name, m_componentList);
        item->setToolTip(entry.name);
    }

    m_licenseView->setObjectName(QStringLiteral("openSourceLicenseView"));
    m_licenseView->setOpenExternalLinks(true);
    m_licenseView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_componentList);
    splitter->addWidget(m_licenseView);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({220, 640});

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_noticeLabel);
    layout->addWidget(splitter, 1);
    layout->addWidget(buttons);

    connect(m_componentList, &QListWidget::currentRowChanged, this,
            &OpenSourceLicensesDialog::showComponent);

    i18n::onRetranslate(this, [this] {
        setWindowTitle(tr("Open Source Licenses"));
        QStringList lgplNames;
        for (const OpenSourceComponent &entry : std::as_const(m_components)) {
            if (entry.lgpl) {
                lgplNames.append(entry.name);
            }
        }
        // 顶部只留 LGPL 必需的一句声明；FreeType、IJG 等署名语句随各自组件的详情页展示。
        const QString website = QString::fromLatin1(kSourceRequestUrl);
        const QString link = QStringLiteral("<a href=\"%1\">%1</a>").arg(website);
        m_noticeLabel->setText(
            tr("%1 are licensed under the LGPL and dynamically linked, so you can replace them "
               "with modified versions. Source code is available from each project's website, "
               "or on request via %2 for three years.")
                .arg(lgplNames.join(QStringLiteral(", ")).toHtmlEscaped(), link));
        showComponent(m_componentList->currentRow());
    });

    m_componentList->setCurrentRow(0);
}

bool OpenSourceLicensesDialog::selectComponent(const QString &name)
{
    for (int row = 0; row < m_components.size(); ++row) {
        if (m_components.at(row).name == name) {
            m_componentList->setCurrentRow(row);
            return true;
        }
    }
    return false;
}

void OpenSourceLicensesDialog::showComponent(int row)
{
    if (row < 0 || row >= m_components.size()) {
        m_licenseView->clear();
        return;
    }
    const OpenSourceComponent &entry = m_components.at(row);
    QString html = QStringLiteral("<h3>%1</h3>").arg(entry.name.toHtmlEscaped());
    html += QStringLiteral("<p><b>%1:</b> %2<br><b>%3:</b> <a href=\"%4\">%4</a></p>")
                .arg(tr("License").toHtmlEscaped(), entry.license.toHtmlEscaped(),
                     tr("Website").toHtmlEscaped(), entry.homepage.toHtmlEscaped());
    if (!entry.attribution.isEmpty()) {
        html += QStringLiteral("<p>%1</p>")
                    .arg(entry.attribution.toHtmlEscaped().replace(QLatin1Char('\n'),
                                                                   QStringLiteral("<br>")));
    }
    const QString text = OpenSourceNotices::licenseText(entry);
    html += QStringLiteral("<pre style=\"white-space: pre-wrap;\">%1</pre>")
                .arg((text.isEmpty() ? tr("The license text could not be loaded.") : text)
                         .toHtmlEscaped());
    m_licenseView->setHtml(html);
}
