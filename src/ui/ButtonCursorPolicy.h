#pragma once

#include <QAbstractButton>
#include <QApplication>
#include <QEvent>

/**
 * @brief 应用级按钮光标策略，统一普通按钮、图标按钮及 Qt 内部按钮的悬停反馈。
 *
 * 使用 QAbstractButton 识别按钮，避免逐个页面设置而遗漏动态创建的控件。
 * 不修改应用全局覆盖光标，因此输入框、终端和窗口边缘仍保留自己的光标行为。
 */
class ButtonCursorPolicy final : public QObject {
public:
    /**
     * @brief 安装按钮策略，并为安装前已经存在的按钮补齐光标。
     * @param application 所属 GUI 应用；调用必须发生在 GUI 线程。
     * 策略由应用持有，随应用销毁；重复设置同一光标不会改变按钮的点击或禁用行为。
     */
    explicit ButtonCursorPolicy(QApplication &application)
        : QObject(&application)
    {
        application.installEventFilter(this);
        for (QWidget *widget : QApplication::allWidgets()) {
            if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
                button->setCursor(Qt::PointingHandCursor);
            }
        }
    }

protected:
    /**
     * @brief 在按钮完成初始化、显示或鼠标进入时应用手型光标。
     * @param watched 接收事件的对象；非按钮对象保持原状。
     * @param event Qt 分发的事件，不得为空。
     * @return 始终返回 false，继续原有事件分发，不拦截点击、焦点或状态变化。
     *
     * 不在构造阶段的 ChildAdded 事件中识别按钮，因为此时派生类型可能尚未构造完成。
     * Enter 额外覆盖控件初始化后自行设置箭头光标的情况，保证实际悬停反馈一致。
     */
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Polish || event->type() == QEvent::Show
            || event->type() == QEvent::Enter) {
            if (auto *button = qobject_cast<QAbstractButton *>(watched)) {
                button->setCursor(Qt::PointingHandCursor);
            }
        }
        return false;
    }
};
