#pragma once

#include <QAbstractButton>
#include <QIcon>
#include <QSize>
#include <QString>
#include <QStyle>
#include <QWidget>

/**
 * @brief 集中提供界面图标解析与按钮装饰，保证各功能区采用一致的现代化视觉语言。
 *
 * Linux 桌面通常通过图标主题提供语义明确的图标，macOS 或精简运行环境则可能缺少同名
 * 资源。这里始终准备一个 Qt 标准图标作为回退，避免某个平台出现“有按钮但没有图标”的
 * 空白控件。该工具只处理展示，不保存状态，也不改变按钮原有的文字和点击行为。
 */
namespace UiIcons {

/**
 * @brief 优先从系统主题读取图标，失败时返回 Qt 自带的跨平台图标。
 * @param context 提供当前应用样式的控件；不得为 nullptr。
 * @param themeName freedesktop 语义图标名，例如 document-save、view-refresh。
 * @param fallback 系统主题缺少目标图标时使用的 Qt 标准图标。
 * @return 可直接设置给 QAction、QPushButton 或 QToolButton 的有效图标。
 *
 * 之所以不直接依赖主题图标，是因为 macOS 的 Fusion 样式通常没有完整的 freedesktop
 * 图标主题；显式回退可让开发、测试和发布环境保持相同的可操作性。
 */
[[nodiscard]] inline QIcon themed(const QWidget *context,
                                  const QString &themeName,
                                  const QStyle::StandardPixmap fallback)
{
    Q_ASSERT(context != nullptr);
    const QIcon themeIcon = QIcon::fromTheme(themeName);
    return themeIcon.isNull() ? context->style()->standardIcon(fallback) : themeIcon;
}

/**
 * @brief 为文字按钮补充语义图标和辅助说明。
 * @param button 待装饰按钮；不得为 nullptr。
 * @param icon 已通过 themed() 解析的图标。
 * @param toolTip 鼠标悬停和无障碍场景使用的完整动作说明；空值表示保留原说明。
 *
 * 统一使用 16 像素图标可避免不同主题的原始尺寸撑高紧凑工具区，同时保留按钮文字，
 * 让不熟悉图标含义的用户无需猜测。
 */
inline void decorateButton(QAbstractButton *button,
                           const QIcon &icon,
                           const QString &toolTip = {})
{
    Q_ASSERT(button != nullptr);
    button->setIcon(icon);
    button->setIconSize(QSize(16, 16));
    if (!toolTip.isEmpty()) {
        button->setToolTip(toolTip);
    }
}

} // namespace UiIcons
