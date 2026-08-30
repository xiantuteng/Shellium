#pragma once

#include <QWidget>

class QPaintEvent;

/**
 * @brief 拖动会话标签离开标题栏时，覆盖在中央工作区之上的落点提示层。
 *
 * SessionTabBar 在指针离开标签栏下沿时会亮出一个跟随光标的浮动标签快照，并发出
 * externalDragStarted；MainWindow 据此显示并置顶本层，在整个终端区周围画一圈虚线高亮，
 * 明确告诉用户"松手即把该会话并入当前页的工作空间"。本层不接管命中判定——真正的合并
 * 仍由 SessionTabBar::mouseReleaseEvent 按释放时的全局坐标交给 MainWindow::mergeTerminalTabs。
 * 因此它是纯装饰控件：平时隐藏，显示期间对鼠标透明，不拦截任何交互。
 */
class SessionTabDropOverlay final : public QWidget {
    Q_OBJECT

public:
    explicit SessionTabDropOverlay(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
};
