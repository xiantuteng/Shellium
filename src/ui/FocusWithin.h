#pragma once

#include <QApplication>
#include <QStyle>
#include <QVariant>
#include <QWidget>

namespace FocusWithin {

/** @brief 样式表据以高亮外框的动态属性名，用法：`QFrame#X[focusWithin="true"]`。 */
inline constexpr char kProperty[] = "focusWithin";

/**
 * @brief 让组合输入框的外框在内部任一控件获得焦点时可被样式表高亮。
 * @param container 承担描边的外框（例如包住无边框 QLineEdit 的 QFrame）。
 *
 * 搜索栏这类组合控件把描边画在外框上，内部输入框本身无边框；Qt 样式表没有
 * `:focus-within`，外框的 `:focus` 只在外框自己持有焦点时成立，因此普通输入框靠全局
 * `QLineEdit:focus` 获得的强调色描边在这里永远不会出现。这里改为监听应用级焦点变化，
 * 把"焦点是否落在外框子树内"写成动态属性并重新 polish，使样式表能按属性选择器换色。
 * 连接以 container 为接收者，外框销毁时自动断开。
 */
inline void track(QWidget *container)
{
    container->setProperty(kProperty, false);
    QObject::connect(qApp, &QApplication::focusChanged, container,
                     [container](QWidget *, QWidget *now) {
                         const bool within = now != nullptr
                             && (now == container || container->isAncestorOf(now));
                         if (container->property(kProperty).toBool() == within) {
                             return;
                         }
                         container->setProperty(kProperty, within);
                         // 动态属性变化不会让样式表自动重算，必须显式 unpolish/polish。
                         container->style()->unpolish(container);
                         container->style()->polish(container);
                         container->update();
                     });
}

} // namespace FocusWithin
