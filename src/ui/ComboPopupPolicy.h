#pragma once

#include "ui/UiIcons.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QEvent>

/**
 * @brief 应用级下拉框弹出层策略：所有 QComboBox 的下拉列表只保留圆角，不再出现直角外框。
 *
 * 下拉列表装在一个独立的顶层容器里，容器自带 StyledPanel 框架、系统窗口装饰和矩形阴影，
 * 样式表的 border-radius 只作用于内部列表，于是圆角列表外面会再包一圈直角边框。
 * UiIcons::modernizePopup(QComboBox*) 能去掉这层外框，但以前要在每个下拉框构造后手动
 * 调用，新加的下拉框一旦遗漏就会出现直角边框。这里在应用层统一接管，动态创建的下拉框
 * 同样生效。
 *
 * 容器的无边框、透明背景等属性必须在其原生窗口创建（第一次弹出）之前设置。下拉框自身
 * 在第一次显示前一定会收到 Polish 事件，而那时下拉列表尚未弹出过，正好是设置的时机。
 */
class ComboPopupPolicy final : public QObject {
public:
    /**
     * @brief 安装策略，并为安装前已经存在、尚未弹出过的下拉框补齐处理。
     * @param application 所属 GUI 应用；调用必须发生在 GUI 线程。策略由应用持有。
     */
    explicit ComboPopupPolicy(QApplication &application)
        : QObject(&application)
    {
        application.installEventFilter(this);
        for (QWidget *widget : QApplication::allWidgets()) {
            if (auto *combo = qobject_cast<QComboBox *>(widget)) {
                apply(combo);
            }
        }
    }

    /**
     * @brief 去掉下拉框弹出容器的直角外框。
     * @param combo 目标下拉框，不得为空。
     *
     * 容器已经创建过原生窗口时跳过：此时再改窗口标志会重建原生窗口，透明背景也不再生效，
     * 反而可能让正在使用的下拉列表闪烁。重复调用是安全的。
     */
    static void apply(QComboBox *combo)
    {
        QAbstractItemView *view = combo->view();
        QWidget *container = view != nullptr ? view->window() : nullptr;
        if (container == nullptr || container == view
            || container->testAttribute(Qt::WA_WState_Created)
            || container->testAttribute(Qt::WA_TranslucentBackground)) {
            return;
        }
        UiIcons::modernizePopup(combo);
    }

protected:
    /**
     * @brief 下拉框完成样式初始化时处理其弹出容器。
     * @return 始终返回 false，不拦截任何事件。
     *
     * 不在 ChildAdded 时处理：那时派生类可能尚未构造完成，qobject_cast 也未必可靠。
     */
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Polish) {
            if (auto *combo = qobject_cast<QComboBox *>(watched)) {
                apply(combo);
            }
        }
        return false;
    }
};
