#pragma once

#include <QRect>
#include <QWidget>

class QPaintEvent;

/**
 * @brief 拖动会话标签离开标题栏时，覆盖在中央工作区之上的落点提示层。
 *
 * SessionTabBar 在指针离开标签栏下沿时会亮出一个跟随光标的浮动标签快照，并发出
 * externalDragStarted；MainWindow 据此显示并置顶本层。当目标页已是多终端工作空间时，
 * 由 TerminalWorkspaceGroup 自行显示与内部拖动一致的半区高亮；当目标页只是单个终端时，
 * 由本层按光标所在象限画出「左 / 右 / 上 / 下半区」高亮，预告松手后新会话会并到哪一侧，
 * 从而区分水平还是垂直排列。命中方向由 MainWindow 在合并时通过 consumeEdge() 读走。
 *
 * 本层始终对鼠标透明，不拦截任何交互——真正的合并仍由 SessionTabBar::mouseReleaseEvent
 * 按释放时的全局坐标交给 MainWindow::mergeTerminalTabs。
 */
class SessionTabDropOverlay final : public QWidget {
    Q_OBJECT

public:
    /** @brief 松手后新会话相对目标终端出现的方向；None 表示未命中具体半区（整块提示）。 */
    enum class DropEdge {
        None,
        Left,
        Right,
        Top,
        Bottom
    };

    explicit SessionTabDropOverlay(QWidget *parent = nullptr);

    /** @brief 复位方向命中，回到整块虚线提示；每次开始新的拖入前调用。 */
    void resetEdge();

    /**
     * @brief 按本层局部坐标下的光标位置刷新命中的半区。
     * @param localPosition 光标在本控件坐标系中的位置。
     *
     * 落在控件外时回退为整块提示。象限判定与分屏内部拖动一致：先比较归一化的横纵偏移
     * 绝对值决定走水平还是垂直，再按符号决定具体边。
     */
    void updateForLocalPos(const QPoint &localPosition);

    /** @brief 取出并复位最近一次方向命中。 */
    DropEdge consumeEdge();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    /** @brief 返回当前 m_edge 对应的高亮矩形（本控件局部坐标）；None 时返回空矩形。 */
    [[nodiscard]] QRect zoneForEdge() const;

    DropEdge m_edge = DropEdge::None;
};
