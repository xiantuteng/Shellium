#pragma once

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QFrame>
#include <QIcon>
#include <QIconEngine>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QStyle>
#include <QWidget>
#include <QtMath>

#include <algorithm>
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
    /** @brief 主机树节点专用的轻量服务器图标，轮廓比导航栏 Hosts 更细。 */
    HostItem,
    Snippets,
    Forwarding,
    Terminal,
    /** @brief 四宫格工作空间，用于区分多终端容器与单终端标签。 */
    Workspace,
    Folder,
    /** @brief 带斜杠的文件夹，表示远程文件管理当前处于隐藏状态。 */
    FolderHidden,
    Plus,
    Search,
    Settings,
    Command,
    Lock,
    Bell,
    ChevronLeft,
    ChevronRight,
    Edit,
    Trash,
    Refresh,
    FolderAdd,
    Connect,
    Close,
    Key,
    Check,
    Play,
    Stop,
    Pause,
    Cancel,
    ArrowUp,
    Send,
    Upload,
    Download,
    Eye,
    /** @brief 带斜线的眼睛，表示当前密码可见，点击后将恢复隐藏。 */
    EyeHidden,
    Info,
    /** @brief 三角形警示图标，用于不可逆操作说明等需要用户注意但尚未出错的状态。 */
    Warning,
    FullscreenEnter,
    FullscreenExit,
    Copy,
    File,
    Clock,
    /** @brief AI 助手侧栏入口与消息发送方标记，四角星形状。 */
    Sparkle,
    /** @brief “最小上下文”一类安全/权限提示徽标。 */
    Shield,
    /** @brief 附加终端输出等回形针式“附件”动作。 */
    Paperclip
};

/**
 * @brief 把一个语义图标绘制为指定分辨率的高分屏透明位图。
 * @param symbol 待绘制的语义图标。
 * @param color 线条颜色，通常使用导航区的主文字色或弱化文字色。
 * @param logicalSize Qt 逻辑像素尺寸；函数内部按设备像素比创建清晰位图。
 * @param pixelRatio 目标绘制设备的像素比，必须大于 0。
 * @return 带正确 devicePixelRatio 的透明位图。
 *
 * 这里采用 QPainter 而不是字体字符：字体回退会让同一个“文件夹”在不同系统上出现完全
 * 不同的笔画粗细，自绘路径则能稳定保持 1.7px 圆角线条，与 Termius 类界面一致。
 */
[[nodiscard]] inline QPixmap renderSymbolPixmap(const Symbol symbol,
                                                const QColor &color,
                                                const QSize logicalSize,
                                                const qreal pixelRatio)
{
    Q_ASSERT(pixelRatio > 0.0);
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
    case Symbol::HostItem: {
        if (symbol == Symbol::HostItem) {
            // 树中每台主机都会重复出现该图标，沿用导航栏的 1.8px 轮廓会形成连续的深色块。
            // 1.4px 在 16～24px 常用尺寸下仍保持清晰，同时更接近系统文件夹图标的视觉重量。
            QPen hostItemPen = pen;
            hostItemPen.setWidthF(1.4);
            painter.setPen(hostItemPen);
        }
        // 侧栏节点使用两层服务器轮廓，放进 14px 的圆角图标容器后仍能辨认；导航入口尺寸
        // 更大，继续保留三层机架以增强识别度。两种图标共享语义但适配各自的信息密度。
        const int rowCount = symbol == Symbol::HostItem ? 2 : 3;
        const qreal firstTop = symbol == Symbol::HostItem ? 6.2 : 4.0;
        const qreal rowGap = symbol == Symbol::HostItem ? 7.0 : 5.6;
        for (int row = 0; row < rowCount; ++row) {
            const qreal top = firstTop + row * rowGap;
            painter.drawRoundedRect(QRectF(3.5, top, 17.0, 4.2), 1.3, 1.3);
            painter.drawEllipse(QPointF(6.3, top + 2.1), 0.65, 0.65);
            painter.drawLine(QPointF(15.0, top + 2.1), QPointF(18.0, top + 2.1));
        }
        break;
    }
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
    case Symbol::Workspace:
        // 四个等尺寸圆角窗格直接表达“同一界面中的多个终端”，并与单终端的命令提示符
        // 轮廓形成稳定区分；间距保留到 2px，缩放到导航轨 21px 时仍不会粘连。
        for (int row = 0; row < 2; ++row) {
            for (int column = 0; column < 2; ++column) {
                painter.drawRoundedRect(
                    QRectF(3.5 + column * 9.5, 3.5 + row * 9.5, 7.5, 7.5),
                    1.8,
                    1.8);
            }
        }
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
    case Symbol::FolderHidden: {
        // 与 Folder 共用同一条轮廓，额外叠加一道贯穿斜杠。同形加否定符是文件、通知等
        // 开关类图标的通用表达，用户无需记忆两个互不相关的图形。
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
        painter.drawLine(QPointF(4.5, 20.5), QPointF(19.5, 3.5));
        break;
    }
    case Symbol::Plus:
        painter.drawLine(QPointF(12.0, 4.5), QPointF(12.0, 19.5));
        painter.drawLine(QPointF(4.5, 12.0), QPointF(19.5, 12.0));
        break;
    case Symbol::ChevronLeft:
        painter.drawLine(QPointF(14.5, 5.0), QPointF(8.5, 12.0));
        painter.drawLine(QPointF(8.5, 12.0), QPointF(14.5, 19.0));
        break;
    case Symbol::ChevronRight:
        painter.drawLine(QPointF(9.5, 5.0), QPointF(15.5, 12.0));
        painter.drawLine(QPointF(15.5, 12.0), QPointF(9.5, 19.0));
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
    case Symbol::Edit: {
        QPainterPath pencil;
        pencil.moveTo(15.0, 4.5);
        pencil.lineTo(19.5, 9.0);
        pencil.lineTo(9.0, 19.5);
        pencil.lineTo(4.5, 20.5);
        pencil.lineTo(5.5, 16.0);
        pencil.closeSubpath();
        painter.drawPath(pencil);
        painter.drawLine(QPointF(13.0, 6.5), QPointF(17.5, 11.0));
        break;
    }
    case Symbol::Trash: {
        painter.drawLine(QPointF(4.0, 7.0), QPointF(20.0, 7.0));
        painter.drawRoundedRect(QRectF(9.0, 3.0, 6.0, 3.2), 1.0, 1.0);
        QPainterPath body;
        body.moveTo(6.0, 7.0);
        body.lineTo(7.0, 20.5);
        body.quadTo(7.0, 21.5, 8.0, 21.5);
        body.lineTo(16.0, 21.5);
        body.quadTo(17.0, 21.5, 17.0, 20.5);
        body.lineTo(18.0, 7.0);
        painter.drawPath(body);
        painter.drawLine(QPointF(10.0, 10.0), QPointF(10.0, 18.0));
        painter.drawLine(QPointF(14.0, 10.0), QPointF(14.0, 18.0));
        break;
    }
    case Symbol::Refresh: {
        painter.drawArc(QRectF(4.5, 4.5, 15.0, 15.0), 40 * 16, 300 * 16);
        painter.drawLine(QPointF(17.7, 7.2), QPointF(19.7, 5.3));
        painter.drawLine(QPointF(17.7, 7.2), QPointF(15.5, 6.3));
        break;
    }
    case Symbol::FolderAdd: {
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
        painter.drawLine(QPointF(12.0, 11.5), QPointF(12.0, 17.0));
        painter.drawLine(QPointF(9.2, 14.3), QPointF(14.8, 14.3));
        break;
    }
    case Symbol::Connect:
        painter.drawLine(QPointF(4.0, 12.0), QPointF(15.0, 12.0));
        painter.drawLine(QPointF(10.5, 6.0), QPointF(16.5, 12.0));
        painter.drawLine(QPointF(10.5, 18.0), QPointF(16.5, 12.0));
        painter.drawLine(QPointF(19.5, 5.0), QPointF(19.5, 19.0));
        break;
    case Symbol::Close:
        painter.drawLine(QPointF(6.0, 6.0), QPointF(18.0, 18.0));
        painter.drawLine(QPointF(18.0, 6.0), QPointF(6.0, 18.0));
        break;
    case Symbol::Key:
        painter.drawEllipse(QRectF(3.5, 9.5, 7.0, 7.0));
        painter.drawLine(QPointF(10.3, 13.0), QPointF(20.0, 13.0));
        painter.drawLine(QPointF(16.0, 13.0), QPointF(16.0, 16.0));
        painter.drawLine(QPointF(19.0, 13.0), QPointF(19.0, 15.5));
        break;
    case Symbol::Check:
        painter.drawLine(QPointF(5.0, 12.5), QPointF(10.0, 17.5));
        painter.drawLine(QPointF(10.0, 17.5), QPointF(19.0, 6.5));
        break;
    case Symbol::Play: {
        QPainterPath triangle;
        triangle.moveTo(8.0, 5.5);
        triangle.lineTo(19.0, 12.0);
        triangle.lineTo(8.0, 18.5);
        triangle.closeSubpath();
        painter.drawPath(triangle);
        break;
    }
    case Symbol::Stop:
        painter.drawRoundedRect(QRectF(6.5, 6.5, 11.0, 11.0), 1.5, 1.5);
        break;
    case Symbol::Pause:
        painter.drawLine(QPointF(9.0, 5.5), QPointF(9.0, 18.5));
        painter.drawLine(QPointF(15.0, 5.5), QPointF(15.0, 18.5));
        break;
    case Symbol::Cancel:
        painter.drawEllipse(QRectF(4.0, 4.0, 16.0, 16.0));
        painter.drawLine(QPointF(7.5, 7.5), QPointF(16.5, 16.5));
        break;
    case Symbol::ArrowUp:
        painter.drawLine(QPointF(12.0, 19.0), QPointF(12.0, 6.0));
        painter.drawLine(QPointF(6.5, 11.5), QPointF(12.0, 6.0));
        painter.drawLine(QPointF(17.5, 11.5), QPointF(12.0, 6.0));
        break;
    case Symbol::Send: {
        QPainterPath plane;
        plane.moveTo(4.0, 12.5);
        plane.lineTo(20.0, 4.5);
        plane.lineTo(13.5, 20.0);
        plane.lineTo(11.0, 13.5);
        plane.closeSubpath();
        painter.drawPath(plane);
        painter.drawLine(QPointF(11.0, 13.5), QPointF(20.0, 4.5));
        break;
    }
    case Symbol::Upload:
        painter.drawLine(QPointF(12.0, 17.0), QPointF(12.0, 5.0));
        painter.drawLine(QPointF(7.0, 10.0), QPointF(12.0, 5.0));
        painter.drawLine(QPointF(17.0, 10.0), QPointF(12.0, 5.0));
        painter.drawLine(QPointF(5.0, 19.0), QPointF(19.0, 19.0));
        break;
    case Symbol::Download:
        painter.drawLine(QPointF(12.0, 5.0), QPointF(12.0, 17.0));
        painter.drawLine(QPointF(7.0, 12.0), QPointF(12.0, 17.0));
        painter.drawLine(QPointF(17.0, 12.0), QPointF(12.0, 17.0));
        painter.drawLine(QPointF(5.0, 19.0), QPointF(19.0, 19.0));
        break;
    case Symbol::Eye:
    case Symbol::EyeHidden: {
        QPainterPath eye;
        eye.moveTo(3.5, 12.0);
        eye.quadTo(12.0, 4.5, 20.5, 12.0);
        eye.quadTo(12.0, 19.5, 3.5, 12.0);
        eye.closeSubpath();
        painter.drawPath(eye);
        painter.drawEllipse(QPointF(12.0, 12.0), 2.2, 2.2);
        if (symbol == Symbol::EyeHidden) {
            // 隐藏动作沿用同一眼睛轮廓并叠加斜线，用户无需读取文字即可判断下一次点击会
            // 恢复掩码；斜线略伸出轮廓，缩放到 16px 时仍保持足够辨识度。
            painter.drawLine(QPointF(4.5, 4.5), QPointF(19.5, 19.5));
        }
        break;
    }
    case Symbol::Info:
        painter.drawEllipse(QRectF(4.0, 4.0, 16.0, 16.0));
        painter.drawLine(QPointF(12.0, 10.5), QPointF(12.0, 17.0));
        painter.drawEllipse(QPointF(12.0, 7.3), 0.9, 0.9);
        break;
    case Symbol::Warning: {
        // 使用圆角折线而不是平台字体里的“⚠”字符，可确保 macOS、Linux 与高 DPI 屏幕上
        // 都保持相同的笔画和基线，并避免系统把文本符号替换成彩色 Emoji。
        QPainterPath triangle;
        triangle.moveTo(11.0, 3.8);
        triangle.quadTo(12.0, 2.2, 13.0, 3.8);
        triangle.lineTo(21.0, 18.0);
        triangle.quadTo(22.0, 20.0, 19.7, 20.0);
        triangle.lineTo(4.3, 20.0);
        triangle.quadTo(2.0, 20.0, 3.0, 18.0);
        triangle.closeSubpath();
        painter.drawPath(triangle);
        painter.drawLine(QPointF(12.0, 8.0), QPointF(12.0, 13.5));
        painter.drawEllipse(QPointF(12.0, 17.0), 0.8, 0.8);
        break;
    }
    case Symbol::FullscreenEnter:
        painter.drawLine(QPointF(5.0, 9.0), QPointF(5.0, 5.0));
        painter.drawLine(QPointF(5.0, 5.0), QPointF(9.0, 5.0));
        painter.drawLine(QPointF(15.0, 5.0), QPointF(19.0, 5.0));
        painter.drawLine(QPointF(19.0, 5.0), QPointF(19.0, 9.0));
        painter.drawLine(QPointF(5.0, 15.0), QPointF(5.0, 19.0));
        painter.drawLine(QPointF(5.0, 19.0), QPointF(9.0, 19.0));
        painter.drawLine(QPointF(19.0, 15.0), QPointF(19.0, 19.0));
        painter.drawLine(QPointF(19.0, 19.0), QPointF(15.0, 19.0));
        break;
    case Symbol::FullscreenExit:
        painter.drawLine(QPointF(9.0, 5.0), QPointF(9.0, 9.0));
        painter.drawLine(QPointF(9.0, 9.0), QPointF(5.0, 9.0));
        painter.drawLine(QPointF(15.0, 5.0), QPointF(15.0, 9.0));
        painter.drawLine(QPointF(15.0, 9.0), QPointF(19.0, 9.0));
        painter.drawLine(QPointF(9.0, 19.0), QPointF(9.0, 15.0));
        painter.drawLine(QPointF(9.0, 15.0), QPointF(5.0, 15.0));
        painter.drawLine(QPointF(15.0, 19.0), QPointF(15.0, 15.0));
        painter.drawLine(QPointF(15.0, 15.0), QPointF(19.0, 15.0));
        break;
    case Symbol::Copy:
        painter.drawRoundedRect(QRectF(8.0, 3.5, 12.0, 12.0), 2.0, 2.0);
        painter.drawRoundedRect(QRectF(3.5, 8.5, 12.0, 12.0), 2.0, 2.0);
        break;
    case Symbol::File: {
        QPainterPath file;
        file.moveTo(6.0, 3.5);
        file.lineTo(14.5, 3.5);
        file.lineTo(19.0, 8.0);
        file.lineTo(19.0, 20.5);
        file.lineTo(6.0, 20.5);
        file.closeSubpath();
        painter.drawPath(file);
        painter.drawLine(QPointF(14.5, 3.5), QPointF(14.5, 8.0));
        painter.drawLine(QPointF(14.5, 8.0), QPointF(19.0, 8.0));
        painter.drawLine(QPointF(9.0, 12.0), QPointF(16.0, 12.0));
        painter.drawLine(QPointF(9.0, 16.0), QPointF(14.0, 16.0));
        break;
    }
    case Symbol::Clock:
        painter.drawEllipse(QRectF(4.0, 4.0, 16.0, 16.0));
        painter.drawLine(QPointF(12.0, 12.0), QPointF(12.0, 7.5));
        painter.drawLine(QPointF(12.0, 12.0), QPointF(16.0, 14.0));
        break;
    case Symbol::Sparkle: {
        // 大星主锋芒 + 两颗小星，呼应参考设计里的 AI 头像符号；四条曲线锋芒比直线菱形
        // 更接近常见的“闪光”语义，在 16~24px 下也不会糊成一个十字。
        QPainterPath big;
        big.moveTo(12.5, 3.0);
        big.cubicTo(12.9, 8.0, 13.8, 10.4, 19.5, 11.5);
        big.cubicTo(13.8, 12.6, 12.9, 15.0, 12.5, 20.0);
        big.cubicTo(12.1, 15.0, 11.2, 12.6, 5.5, 11.5);
        big.cubicTo(11.2, 10.4, 12.1, 8.0, 12.5, 3.0);
        big.closeSubpath();
        painter.setBrush(painter.pen().color());
        painter.drawPath(big);
        painter.setBrush(Qt::NoBrush);
        QPainterPath small;
        small.moveTo(5.2, 15.0);
        small.cubicTo(5.4, 16.8, 5.8, 17.6, 8.0, 18.0);
        small.cubicTo(5.8, 18.4, 5.4, 19.2, 5.2, 21.0);
        small.cubicTo(5.0, 19.2, 4.6, 18.4, 2.4, 18.0);
        small.cubicTo(4.6, 17.6, 5.0, 16.8, 5.2, 15.0);
        small.closeSubpath();
        painter.drawPath(small);
        break;
    }
    case Symbol::Shield:
        painter.drawPath([]() {
            QPainterPath path;
            path.moveTo(12.0, 2.8);
            path.lineTo(19.5, 5.5);
            path.lineTo(19.5, 11.5);
            path.cubicTo(19.5, 16.5, 16.3, 19.8, 12.0, 21.2);
            path.cubicTo(7.7, 19.8, 4.5, 16.5, 4.5, 11.5);
            path.lineTo(4.5, 5.5);
            path.closeSubpath();
            return path;
        }());
        painter.drawPolyline(QVector<QPointF>{
            QPointF(8.7, 12.0), QPointF(10.8, 14.2), QPointF(15.3, 9.2)}.data(), 3);
        break;
    case Symbol::Paperclip:
        painter.drawPath([]() {
            QPainterPath path;
            path.moveTo(16.5, 8.0);
            path.lineTo(9.5, 15.0);
            path.cubicTo(7.9, 16.6, 7.9, 19.0, 9.5, 20.5);
            path.cubicTo(11.1, 22.0, 13.5, 22.0, 15.0, 20.5);
            path.lineTo(20.5, 15.0);
            path.cubicTo(23.0, 12.5, 23.0, 8.5, 20.5, 6.0);
            path.cubicTo(18.0, 3.5, 14.0, 3.5, 11.5, 6.0);
            path.lineTo(6.5, 11.0);
            return path;
        }());
        break;
    }
    painter.end();
    return pixmap;
}

/**
 * @brief 根据控件当前调色板实时绘制自定义图标的 QIconEngine。
 *
 * 普通 QIcon 会把创建时的颜色永久烘焙进位图；应用从浅色切到深色后，那张位图仍然是旧
 * 颜色，正是工具栏和树节点图标在另一种主题下消失的根因。本引擎仅在调用方原本使用控件
 * WindowText 颜色时跟随调色板，每次绘制都重新读取当前颜色；显式品牌色、终端配色和主按钮
 * 深色图标仍保持调用方指定值，不会被全局主题擅自覆盖。
 */
class PaletteAwareSymbolIconEngine final : public QIconEngine {
public:
    /**
     * @brief 创建可随控件调色板变化的图标引擎。
     * @param context 图标所属控件；销毁后安全退回 initialColor。
     * @param symbol 图标语义。
     * @param initialColor 创建时指定的颜色。
     * @param followWindowText true 表示绘制时读取控件当前 WindowText；false 表示始终使用
     *        initialColor。
     * @param logicalSize 图标设计尺寸。
     */
    PaletteAwareSymbolIconEngine(const QWidget *context,
                                 const Symbol symbol,
                                 const QColor &initialColor,
                                 const bool followWindowText,
                                 const QSize logicalSize)
        : m_context(const_cast<QWidget *>(context))
        , m_symbol(symbol)
        , m_initialColor(initialColor)
        , m_followWindowText(followWindowText)
        , m_logicalSize(logicalSize)
    {
    }

    /** @brief 复制引擎；QIcon 隐式共享分离时由 Qt 调用。 */
    [[nodiscard]] QIconEngine *clone() const override
    {
        return new PaletteAwareSymbolIconEngine(*this);
    }

    /**
     * @brief 把图标绘制到目标矩形。
     * @param painter Qt 提供的目标画笔，不可为空。
     * @param rect 目标逻辑像素矩形。
     * @param mode 正常、激活、选中或禁用模式；禁用模式读取 Disabled 调色板组。
     * @param state 开关状态；当前线性符号不因 On/Off 改形，状态由调用方选择不同 Symbol 表达。
     */
    void paint(QPainter *painter,
               const QRect &rect,
               const QIcon::Mode mode,
               const QIcon::State state) override
    {
        Q_UNUSED(state)
        if (painter == nullptr || rect.isEmpty()) {
            return;
        }
        const qreal pixelRatio = painter->device() == nullptr
            ? 1.0
            : painter->device()->devicePixelRatioF();
        const QPixmap pixmap = renderSymbolPixmap(
            m_symbol, colorForMode(mode), rect.size(), std::max(pixelRatio, 1.0));
        painter->drawPixmap(rect, pixmap);
    }

    /**
     * @brief 返回指定像素尺寸的位图，供菜单、按钮和测试直接请求。
     * @return DPR=1 的位图；高分屏路径由 scaledPixmap() 保留真实像素比。
     */
    [[nodiscard]] QPixmap pixmap(const QSize &size,
                                 const QIcon::Mode mode,
                                 const QIcon::State state) override
    {
        Q_UNUSED(state)
        return renderSymbolPixmap(m_symbol, colorForMode(mode), size, 1.0);
    }

    /**
     * @brief 返回适配高分屏比例的位图。
     * @param size Qt 请求的逻辑尺寸。
     * @param mode 图标模式。
     * @param state 图标开关状态。
     * @param scale 设备像素比。
     * @return 设置了对应 DPR 的清晰位图。
     */
    [[nodiscard]] QPixmap scaledPixmap(const QSize &size,
                                       const QIcon::Mode mode,
                                       const QIcon::State state,
                                       const qreal scale) override
    {
        Q_UNUSED(state)
        return renderSymbolPixmap(m_symbol,
                                  colorForMode(mode),
                                  size,
                                  std::max(scale, 1.0));
    }

    /** @brief 返回稳定引擎名称，便于 Qt 与诊断工具识别自定义实现。 */
    [[nodiscard]] QString key() const override
    {
        return QStringLiteral("ShelliumPaletteAwareSymbol");
    }

    /**
     * @brief 返回图标的设计尺寸，供 Qt 在菜单和工具栏没有显式尺寸时选择合理大小。
     * @param mode 图标模式；所有模式共用同一设计尺寸。
     * @return 仅包含构造时逻辑尺寸的列表。
     */
    [[nodiscard]] QList<QSize> availableSizes(
        const QIcon::Mode mode = QIcon::Normal,
        const QIcon::State state = QIcon::Off) override
    {
        Q_UNUSED(mode)
        Q_UNUSED(state)
        return {m_logicalSize};
    }

private:
    /**
     * @brief 解析当前绘制模式应使用的颜色。
     * @param mode QIcon 当前模式。
     * @return 跟随主题时取控件实时调色板；控件已销毁或显式色图标返回初始颜色。
     */
    [[nodiscard]] QColor colorForMode(const QIcon::Mode mode) const
    {
        if (!m_followWindowText || m_context.isNull()) {
            return m_initialColor;
        }
        const QPalette::ColorGroup group = mode == QIcon::Disabled
            ? QPalette::Disabled
            : QPalette::Active;
        return m_context->palette().color(group, QPalette::WindowText);
    }

    QPointer<QWidget> m_context;
    Symbol m_symbol;
    QColor m_initialColor;
    bool m_followWindowText = false;
    QSize m_logicalSize;
};

/**
 * @brief 创建同时兼容浅色和深色主题的自绘语义图标。
 * @param context 图标所属控件；用于读取设备比例和实时调色板，不得为空。
 * @param symbol 待绘制的语义图标。
 * @param color 初始线条色。若它等于 context 当前 WindowText，图标会在后续主题切换时自动
 *        跟随新的 WindowText；显式传入的品牌色保持固定。
 * @param logicalSize 图标设计尺寸。
 * @return 使用动态绘制引擎的 QIcon。
 *
 * “是否跟随主题”在创建时按颜色来源判定，而不是简单让全部图标追随应用色。终端会话标题栏
 * 使用终端调色板前景色，主操作按钮使用固定深色；这两类若被强制替换成 WindowText，反而会
 * 在终端自定义配色或绿色按钮上失去对比度。
 */
[[nodiscard]] inline QIcon symbol(const QWidget *context,
                                  const Symbol symbol,
                                  const QColor &color,
                                  const QSize logicalSize = QSize(20, 20))
{
    Q_ASSERT(context != nullptr);
    const bool followWindowText = color == context->palette().color(QPalette::WindowText);
    return QIcon(new PaletteAwareSymbolIconEngine(
        context, symbol, color, followWindowText, logicalSize));
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
/**
 * @brief 让右键菜单/下拉弹出菜单真正呈现圆角，而不是圆角内容叠在方形窗口背景上。
 * @param menu 尚未 exec()/popup() 过的菜单；不得为 nullptr。
 *
 * QMenu 的圆角完全由样式表的 border-radius 画在内容区域内，但顶层弹出窗口默认背景
 * 不透明，圆角以外的四个角仍会露出方形背景，看起来像多了一层直角外框。必须在原生
 * 窗口创建之前（即 exec/popup/show 之前）设置该属性才会生效，因此调用方须在构造
 * 菜单后、真正弹出前调用本函数。
 */
inline void modernizePopup(QMenu *menu)
{
    Q_ASSERT(menu != nullptr);
    menu->setAttribute(Qt::WA_TranslucentBackground);
}

/**
 * @brief 让 QComboBox 弹出列表真正呈现圆角，而不是圆角内容叠在方形窗口背景上。
 * @param combo 已构造、尚未弹出过下拉列表的下拉框；不得为 nullptr。
 *
 * 原理与 modernizePopup(QMenu*) 相同：QComboBox 的下拉列表容器
 * （QComboBoxPrivateContainer）是一个独立的 QFrame 顶层窗口。样式表里的 border-radius
 * 只画在内部列表上，而容器自己的 StyledPanel 框架仍会在外面绘制一圈直角边框；仅设置
 * 透明背景只能消除四角底色，不能消除这道框线。因此这里同时关闭容器框架并启用透明背景，
 * 最终只保留内部 QAbstractItemView 的圆角矩形。相关属性必须在该顶层窗口创建原生句柄之前
 * 设置；QComboBox::view() 会按需创建容器但不会显示它，所以在构造下拉框后立即调用即可。
 */
inline void modernizePopup(QComboBox *combo)
{
    Q_ASSERT(combo != nullptr);
    if (QAbstractItemView *view = combo->view()) {
        if (QWidget *container = view->window()) {
            container->setAttribute(Qt::WA_TranslucentBackground);
            if (auto *containerFrame = qobject_cast<QFrame *>(container)) {
                // 下拉容器默认的 StyledPanel 正是截图中包住圆角列表的直角矩形。关闭框架只会
                // 移除容器装饰，不会影响内部列表由全局 QSS 绘制的圆角、边框和选中状态。
                containerFrame->setFrameShape(QFrame::NoFrame);
                containerFrame->setLineWidth(0);
                containerFrame->setMidLineWidth(0);
            }
        }
    }
}

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
