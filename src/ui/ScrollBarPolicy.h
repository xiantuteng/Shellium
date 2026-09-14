#pragma once

#include <QAbstractScrollArea>
#include <QApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QStyleOptionSlider>
#include <QTimer>
#include <QWheelEvent>

/**
 * @brief 全应用滚动条绘制策略：仅在手动滚动期间绘制圆角滑块。
 *
 * 只接管绘制而不切换 ScrollBarPolicy 或控件尺寸，避免终端列数、表格宽度随显隐变化。
 * 所有操作均在 GUI 线程执行；每个滑块的计时器由滑块持有，销毁页面时自动取消回调。
 */
class ScrollBarPolicy final : public QObject {
public:
    /** @brief 安装全局过滤器，覆盖现有及后续创建的滚动条。
     * @param application GUI 应用，负责本策略的生命周期；不执行 IO，也不抛出业务异常。
     */
    explicit ScrollBarPolicy(QApplication &application) : QObject(&application)
    {
        application.installEventFilter(this);
    }

protected:
    /** @brief 识别手动输入并绘制滑块，自动追加内容不会触发显示。
     * @param watched 当前事件接收对象，非 QWidget 对象直接放行。
     * @param event Qt 事件；输入事件始终继续分发，保留原生滚动和拖动行为。
     * @return 仅接管滚动条绘制时返回 true，其余返回 false。
     */
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!widget) return false;
        auto *bar = qobject_cast<QScrollBar *>(widget);
        if (bar && event->type() == QEvent::Paint) {
            // 不调用原生绘制，避免系统主题再次画出方角轨道、箭头或常驻滑块。
            QPainter painter(bar);
            if (bar->property("shelliumScrollActive").toBool()
                && bar->maximum() > bar->minimum()) {
                QStyleOptionSlider option;
                option.initFrom(bar);
                option.orientation = bar->orientation();
                option.minimum = bar->minimum();
                option.maximum = bar->maximum();
                option.sliderPosition = bar->sliderPosition();
                option.sliderValue = bar->value();
                option.singleStep = bar->singleStep();
                option.pageStep = bar->pageStep();
                option.upsideDown = bar->invertedAppearance();
                if (bar->orientation() == Qt::Horizontal && bar->isRightToLeft())
                    option.upsideDown = !option.upsideDown;
                QRectF rect = bar->style()->subControlRect(
                    QStyle::CC_ScrollBar, &option, QStyle::SC_ScrollBarSlider, bar);
                // 沿短轴居中收窄至 6px，圆角半径取短边一半，在各种平台上保持胶囊外形。
                if (bar->orientation() == Qt::Vertical) {
                    rect.setLeft(rect.center().x() - 3);
                    rect.setWidth(6);
                } else {
                    rect.setTop(rect.center().y() - 3);
                    rect.setHeight(6);
                }
                QColor color = bar->palette().color(QPalette::Text);
                color.setAlpha(bar->isSliderDown() ? 180 : 110);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(Qt::NoPen);
                painter.setBrush(color);
                painter.drawRoundedRect(rect, 3, 3);
            }
            return true;
        }

        bool manualInput = event->type() == QEvent::Wheel;
        if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            // 终端普通输入和文本编辑不会亮起滚动条，仅识别具有滚动语义的导航键。
            manualInput = key == Qt::Key_PageUp || key == Qt::Key_PageDown
                || key == Qt::Key_Up || key == Qt::Key_Down
                || key == Qt::Key_Left || key == Qt::Key_Right
                || key == Qt::Key_Home || key == Qt::Key_End;
        }
        if (bar && (event->type() == QEvent::MouseButtonPress
                    || event->type() == QEvent::MouseButtonRelease
                    || (event->type() == QEvent::MouseMove && bar->isSliderDown()))) {
            reveal(bar);
        } else if (manualInput) {
            // 从 viewport 或其子控件向上查找最近滚动区，避免让其他页面的滑块一起显示。
            if (bar) revealAfterInput(bar, event->type());
            else for (QWidget *parent = widget; parent; parent = parent->parentWidget()) {
                if (auto *area = qobject_cast<QAbstractScrollArea *>(parent)) {
                    revealAfterInput(area->verticalScrollBar(), event->type());
                    revealAfterInput(area->horizontalScrollBar(), event->type());
                    break;
                }
            }
        }
        return false;
    }

private:
    /** @brief 导航键必须实际移动滚动位置才显示，避免终端方向键发送远程命令时误触发。
     * @param bar 待检查的滚动条，通过弱指针检测销毁，页面关闭后不再访问。
     * @param type 原始输入类型；滚轮立即反馈，键盘等待 Qt 完成默认事件处理再比较位置。
     */
    void revealAfterInput(QScrollBar *bar, QEvent::Type type)
    {
        if (type != QEvent::KeyPress) {
            reveal(bar);
            return;
        }
        const int previousValue = bar->value();
        // 使用策略自身作为连接上下文，避免策略提前销毁后访问失效的 this。
        const QPointer<QScrollBar> guardedBar(bar);
        QTimer::singleShot(0, this, [this, guardedBar, previousValue] {
            if (guardedBar && guardedBar->value() != previousValue) reveal(guardedBar);
        });
    }

    /** @brief 显示单个有可滚动范围的滑块，并重置空闲计时。
     * @param bar 目标滚动条；无滚动范围时直接返回，不创建计时器。
     * 计时器作为滚动条子对象复用，避免高频触控板事件累积延迟任务；拖动未释放时继续保留。
     */
    void reveal(QScrollBar *bar)
    {
        if (bar->maximum() <= bar->minimum()) return;
        auto *timer = bar->findChild<QTimer *>(QStringLiteral("shelliumScrollIdle"),
                                             Qt::FindDirectChildrenOnly);
        if (!timer) {
            timer = new QTimer(bar);
            timer->setObjectName(QStringLiteral("shelliumScrollIdle"));
            timer->setInterval(1000); // 一秒无操作后隐藏，兼顾短暂停顿与持续滚动。
            connect(timer, &QTimer::timeout, bar, [bar, timer] {
                if (bar->isSliderDown()) return;
                timer->stop();
                bar->setProperty("shelliumScrollActive", false);
                bar->update();
            });
        }
        bar->setProperty("shelliumScrollActive", true);
        bar->update();
        timer->start();
    }
};
