#pragma once

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QFont>
#include <QRect>
#include <QScreen>
#include <QToolTip>
#include <QWidget>

/**
 * @brief 应用级悬停提示策略：让 QToolTip 紧贴鼠标，并使用与主界面一致的字号。
 *
 * Qt 按平台光标尺寸计算提示偏移；macOS 报告的光标尺寸远大于可见箭头，提示会落在鼠标
 * 下方很远处。平台默认的提示字体也比界面正文小一号。这里不替换 QToolTip（保留其延时、
 * 富文本与离开区域自动隐藏等行为），只在提示标签显示或复用移动时把它重新放到光标旁边。
 */
class ToolTipPolicy final : public QObject {
public:
    /** @brief 提示左上角相对光标热点的偏移，刚好避开标准箭头光标的可见部分。 */
    static constexpr QPoint CursorOffset{2, 20};
    /** @brief 下方放不下而翻到光标上方时，提示底边与热点之间保留的间距。 */
    static constexpr int AboveCursorGap = 6;

    /**
     * @brief 安装提示策略。
     * @param application 所属 GUI 应用；调用必须发生在 GUI 线程，且应在应用字体确定之后。
     * 策略由应用持有，随应用销毁。
     */
    explicit ToolTipPolicy(QApplication &application)
        : QObject(&application)
    {
        // 平台主题给提示的是小号系统字体；改用界面正文字号，避免提示比它所解释的文字还小。
        QFont font = QToolTip::font();
        font.setPointSizeF(QApplication::font().pointSizeF());
        QToolTip::setFont(font);
        application.installEventFilter(this);
    }

    /**
     * @brief 计算提示应处的全局位置。
     * @param cursor 光标热点的全局坐标。
     * @param tipSize 提示标签尺寸。
     * @param available 光标所在屏幕的可用区域；为空时不做边界约束。
     * @return 提示左上角的全局坐标：默认在光标右下方，底部放不下时翻到光标上方，
     *         并水平收进屏幕，不会遮住光标热点。
     */
    static QPoint placement(const QPoint &cursor, const QSize &tipSize, const QRect &available)
    {
        QPoint position = cursor + CursorOffset;
        if (available.isEmpty()) {
            return position;
        }
        if (position.y() + tipSize.height() - 1 > available.bottom()) {
            position.setY(cursor.y() - AboveCursorGap - tipSize.height());
        }
        position.setX(qMin(position.x(), available.right() - tipSize.width() + 1));
        position.setX(qMax(position.x(), available.left()));
        position.setY(qMax(position.y(), available.top()));
        return position;
    }

protected:
    /**
     * @brief 在 Qt 提示标签显示或被复用移动后，把它放回光标旁。
     * @param watched 接收事件的对象；只处理 Qt 内部提示标签。
     * @param event Qt 分发的事件，不得为空。
     * @return 始终返回 false，不拦截任何事件。
     *
     * 新提示的移动在显示前挂起、与 Show 一起投递；已显示的提示换文字时 Qt 只会 move()，
     * 因此两种事件都要处理。目标位置只取决于光标与尺寸，重复移动是幂等的，不会递归。
     */
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::Show && event->type() != QEvent::Move) {
            return false;
        }
        auto *label = qobject_cast<QWidget *>(watched);
        if (label == nullptr || !label->isWindow()
            || label->objectName() != QLatin1String("qtooltip_label")) {
            return false;
        }
        const QPoint cursor = QCursor::pos();
        const QScreen *screen = QGuiApplication::screenAt(cursor);
        if (screen == nullptr) {
            screen = label->screen();
        }
        const QPoint target = placement(
            cursor, label->size(), screen == nullptr ? QRect() : screen->availableGeometry());
        if (label->pos() != target) {
            label->move(target);
        }
        return false;
    }
};
