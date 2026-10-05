#pragma once

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

class QLabel;
class QListWidget;
class QTextBrowser;

/**
 * @brief 随 Shellium 分发的一个开源组件及其许可证。
 *
 * 名称、许可证标识与版权文本保持上游原文（英文），不进 tr()：许可证义务针对的是原文，
 * 翻译件没有法律效力，而且逐条翻译几十个组件只会让译文与上游悄悄分叉。
 */
struct OpenSourceComponent {
    /** @brief 对外显示的组件名；内置字体必须与 BundledFonts::familyNames() 的族名一致。 */
    QString name;
    /** @brief SPDX 许可证表达式，例如 "LGPL-2.1-or-later"。 */
    QString license;
    /** @brief 上游主页，同时也是获取对应源代码的入口。 */
    QString homepage;
    /** @brief 许可证要求在文档中出现的署名语句（如 FreeType、IJG），没有则为空。 */
    QString attribution;
    /** @brief 许可证全文所在的资源文件，相对 `:/licenses/`；按显示顺序拼接。 */
    QStringList licenseFiles;
    /** @brief 是否为 LGPL 组件；这类组件必须保持动态链接，并在声明里给出源代码获取方式。 */
    bool lgpl = false;
};

namespace OpenSourceNotices {

/**
 * @brief 返回随安装包分发的全部开源组件，顺序即界面展示顺序。
 *
 * 清单覆盖 macOS/Windows 安装包里实际打进去的每一个第三方动态库（含 Qt 间接带入的 ICU、
 * GLib、HarfBuzz 等）、内置字体与发行版图标。新增或替换依赖时必须同步这里与
 * resources/licenses/，见该目录下的 README.md。
 */
[[nodiscard]] QList<OpenSourceComponent> components();

/**
 * @brief 读取组件的许可证全文；多个文件之间用分隔线隔开。
 * @return 任一文件缺失时返回空字符串，便于测试发现资源清单与组件表不一致。
 */
[[nodiscard]] QString licenseText(const OpenSourceComponent &component);

/** @brief 把资源相对路径转换为完整的 Qt 资源路径（`:/licenses/...`）。 */
[[nodiscard]] QString resourcePath(const QString &licenseFile);

} // namespace OpenSourceNotices

/**
 * @brief “开源软件许可”对话框：顶部一句 LGPL 声明，下方左列组件、右侧显示署名与许可证全文。
 */
class OpenSourceLicensesDialog final : public QDialog {
    Q_OBJECT

public:
    explicit OpenSourceLicensesDialog(QWidget *parent = nullptr);

    /** @brief 选中指定组件；找不到时返回 false。供测试和外部定位使用。 */
    bool selectComponent(const QString &name);

private:
    void showComponent(int row);

    QList<OpenSourceComponent> m_components;
    QLabel *m_noticeLabel = nullptr;
    QListWidget *m_componentList = nullptr;
    QTextBrowser *m_licenseView = nullptr;
};
