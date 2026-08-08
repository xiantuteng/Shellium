#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QStyle>
#include <QWidget>
#include <QtMath>

#include <cmath>

/**
 * @brief 集中提供界面图标解析与按钮装饰，保证各功能区采用一致的现代化视觉语言。
 *
 * Linux 桌面通常通过图标主题提供语义明确的图标，macOS 或精简运行环境则可能缺少同名
 * 资源。这里始终准备一个 Qt 标准图标作为回退，避免某个平台出现“有按钮但没有图标”的
 * 空白控件。该工具只处理展示，不保存状态，也不改变按钮原有的文字和点击行为。
 */
namespace UiIcons {

/**
 * @brief Shellium 自绘线性图标的语义类型。
 *
 * 标题栏和导航轨属于品牌视觉，不应依赖操作系统可能缺失、风格也不统一的旧式标准图标。
 * 这些类型只描述动作含义，实际颜色和像素尺寸由调用方按当前界面层级传入。
 */
enum class Symbol {
    Sidebar,
    Hosts,
    Snippets,
    Forwarding,
    Terminal,
    Folder,
    Plus,
    Search,
    Settings,
    Command,
    Lock,
    Bell
};

/**
 * @brief 绘制适合标题栏和导航轨的高分屏线性图标。
 * @param context 用于取得屏幕设备像素比的控件；不得为 nullptr。
 * @param symbol 待绘制的语义图标。
 * @param color 线条颜色，通常使用导航区的主文字色或弱化文字色。
 * @param logicalSize Qt 逻辑像素尺寸；函数内部按设备像素比创建清晰位图。
 * @return 带正确 devicePixelRatio 的 QIcon。
 *
 * 这里采用 QPainter 而不是字体字符：字体回退会让同一个“文件夹”在不同系统上出现完全
 * 不同的笔画粗细，自绘路径则能稳定保持 1.7px 圆角线条，与 Termius 类界面一致。
 */
[[nodiscard]] inline QIcon symbol(const QWidget *context,
                                  const Symbol symbol,
                                  const QColor &color,
                                  const QSize logicalSize = QSize(20, 20))
{
    Q_ASSERT(context != nullptr);
    const qreal pixelRatio = context->devicePixelRatioF();
    QPixmap pixmap((QSizeF(logicalSize) * pixelRatio).toSize());
    pixmap.setDevicePixelRatio(pixelRatio);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(logicalSize.width() / 24.0, logicalSize.height() / 24.0);
    QPen pen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (symbol) {
    case Symbol::Sidebar:
        painter.drawRoundedRect(QRectF(3.5, 4.0, 17.0, 16.0), 2.5, 2.5);
        painter.drawLine(QPointF(9.0, 4.5), QPointF(9.0, 19.5));
        painter.drawLine(QPointF(5.8, 8.0), QPointF(7.0, 8.0));
        painter.drawLine(QPointF(5.8, 11.0), QPointF(7.0, 11.0));
        break;
    case Symbol::Hosts:
        for (int row = 0; row < 3; ++row) {
            const qreal top = 4.0 + row * 5.6;
            painter.drawRoundedRect(QRectF(3.5, top, 17.0, 4.2), 1.3, 1.3);
            painter.drawEllipse(QPointF(6.3, top + 2.1), 0.65, 0.65);
            painter.drawLine(QPointF(15.0, top + 2.1), QPointF(18.0, top + 2.1));
        }
        break;
    case Symbol::Snippets: {
        QPainterPath leftBrace;
        leftBrace.moveTo(9.0, 3.5);
        leftBrace.cubicTo(6.2, 3.5, 7.4, 9.5, 4.8, 10.7);
        leftBrace.cubicTo(7.4, 12.0, 6.2, 20.5, 9.0, 20.5);
        painter.drawPath(leftBrace);
        QPainterPath rightBrace;
        rightBrace.moveTo(15.0, 3.5);
        rightBrace.cubicTo(17.8, 3.5, 16.6, 9.5, 19.2, 10.7);
        rightBrace.cubicTo(16.6, 12.0, 17.8, 20.5, 15.0, 20.5);
        painter.drawPath(rightBrace);
        break;
    }
    case Symbol::Forwarding:
        painter.drawLine(QPointF(4.0, 8.0), QPointF(18.0, 8.0));
        painter.drawLine(QPointF(15.0, 5.0), QPointF(18.0, 8.0));
        painter.drawLine(QPointF(15.0, 11.0), QPointF(18.0, 8.0));
        painter.drawLine(QPointF(20.0, 16.0), QPointF(6.0, 16.0));
        painter.drawLine(QPointF(9.0, 13.0), QPointF(6.0, 16.0));
        painter.drawLine(QPointF(9.0, 19.0), QPointF(6.0, 16.0));
        break;
    case Symbol::Terminal:
        painter.drawRoundedRect(QRectF(2.8, 4.0, 18.4, 16.0), 3.0, 3.0);
        painter.drawLine(QPointF(6.5, 9.0), QPointF(9.5, 12.0));
        painter.drawLine(QPointF(9.5, 12.0), QPointF(6.5, 15.0));
        painter.drawLine(QPointF(12.5, 15.0), QPointF(17.0, 15.0));
        break;
    case Symbol::Folder: {
        QPainterPath folder;
        folder.moveTo(3.0, 7.0);
        folder.lineTo(3.0, 18.5);
        folder.quadTo(3.0, 20.5, 5.0, 20.5);
        folder.lineTo(19.0, 20.5);
        folder.quadTo(21.0, 20.5, 21.0, 18.5);
        folder.lineTo(21.0, 8.5);
        folder.quadTo(21.0, 6.5, 19.0, 6.5);
        folder.lineTo(12.0, 6.5);
        folder.lineTo(9.5, 3.8);
        folder.lineTo(5.0, 3.8);
        folder.quadTo(3.0, 3.8, 3.0, 5.8);
        folder.closeSubpath();
        painter.drawPath(folder);
        break;
    }
    case Symbol::Plus:
        painter.drawLine(QPointF(12.0, 4.5), QPointF(12.0, 19.5));
        painter.drawLine(QPointF(4.5, 12.0), QPointF(19.5, 12.0));
        break;
    case Symbol::Search:
        painter.drawEllipse(QRectF(4.0, 4.0, 11.5, 11.5));
        painter.drawLine(QPointF(14.0, 14.0), QPointF(20.0, 20.0));
        break;
    case Symbol::Settings:
        painter.drawEllipse(QRectF(8.5, 8.5, 7.0, 7.0));
        painter.drawEllipse(QRectF(4.0, 4.0, 16.0, 16.0));
        for (int index = 0; index < 8; ++index) {
            const qreal angle = qDegreesToRadians(index * 45.0);
            painter.drawLine(QPointF(12.0 + std::cos(angle) * 8.0,
                                     12.0 + std::sin(angle) * 8.0),
                             QPointF(12.0 + std::cos(angle) * 10.0,
                                     12.0 + std::sin(angle) * 10.0));
        }
        break;
    case Symbol::Command:
        painter.drawRoundedRect(QRectF(3.0, 4.0, 18.0, 16.0), 3.0, 3.0);
        painter.drawLine(QPointF(6.5, 9.0), QPointF(9.5, 12.0));
        painter.drawLine(QPointF(9.5, 12.0), QPointF(6.5, 15.0));
        painter.drawLine(QPointF(12.5, 15.0), QPointF(17.0, 15.0));
        break;
    case Symbol::Lock:
        painter.drawRoundedRect(QRectF(5.0, 10.0, 14.0, 10.5), 2.4, 2.4);
        painter.drawArc(QRectF(7.5, 3.0, 9.0, 12.0), 0, 180 * 16);
        painter.drawEllipse(QPointF(12.0, 15.0), 0.8, 0.8);
        break;
    case Symbol::Bell:
        painter.drawArc(QRectF(6.0, 4.0, 12.0, 14.0), 0, 180 * 16);
        painter.drawLine(QPointF(6.0, 11.0), QPointF(4.5, 17.0));
        painter.drawLine(QPointF(4.5, 17.0), QPointF(19.5, 17.0));
        painter.drawLine(QPointF(19.5, 17.0), QPointF(18.0, 11.0));
        painter.drawArc(QRectF(9.0, 15.0, 6.0, 6.0), 180 * 16, 180 * 16);
        break;
    }
    painter.end();
    return QIcon(pixmap);
}

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
