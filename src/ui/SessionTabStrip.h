#pragma once

#include <QWidget>

class QEvent;
class QMetaMethod;
class QResizeEvent;
class QVariantAnimation;
class QWheelEvent;
class SessionTabBar;

/**
 * @brief 标题栏会话标签的裁剪视口，按像素水平滚动其中的 SessionTabBar。
 *
 * QTabBar 自带的滚动只能整格跳：偏移量是私有状态，唯一入口是内置按钮，每按一次就把下
 * 一个标签整体卷进来。本控件改为让标签栏始终保持"装下全部标签"的完整宽度，自身只
 * 露出其中一段，通过移动标签栏的横坐标实现任意像素的滚动。标签栏因此永远不会进入
 * 溢出态，命中、拖动排序与自绘全部沿用它自己的坐标，不需要逐处换算滚动偏移。
 */
class SessionTabStrip final : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief 创建视口及其内部的会话标签栏。
     * @param parent Qt 父控件，通常是窗口顶部的自定义标题区。
     */
    explicit SessionTabStrip(QWidget *parent = nullptr);

    /** @brief 返回视口内的会话标签栏；生命周期归本控件所有。 */
    [[nodiscard]] SessionTabBar *tabBar() const;

    /** @brief 当前向右卷入的像素数；0 表示第一个标签贴住视口左缘。 */
    [[nodiscard]] int scrollOffset() const;

    /** @brief 允许的最大滚动像素数；标签未溢出时为 0。 */
    [[nodiscard]] int maximumScrollOffset() const;

    /**
     * @brief 按给定像素数滚动标签。
     * @param pixels 正值向右卷入更靠后的标签，负值向左回卷；结果收敛在合法范围内。
     * @param animated true 时平滑过渡，连续调用会在上一段的终点上继续累加。
     */
    void scrollByPixels(int pixels, bool animated);

    /** @brief 外部左箭头的一次点击：向左平滑滚动固定像素。 */
    void scrollTabsLeft();

    /** @brief 外部右箭头的一次点击：向右平滑滚动固定像素。 */
    void scrollTabsRight();

    /** @brief 建议尺寸等于全部标签的总宽度，"+"因此紧贴最后一个标签。 */
    [[nodiscard]] QSize sizeHint() const override;

    /** @brief 最小宽度为 0，标签溢出时由布局把视口压缩到可用宽度。 */
    [[nodiscard]] QSize minimumSizeHint() const override;

signals:
    /**
     * @brief 标签是否溢出、以及两端是否还能继续滚动发生变化。
     * @param scrollable 标签总宽度超过视口宽度，需要显示外部滚动按钮。
     * @param canScrollLeft 左侧仍有被遮挡的内容。
     * @param canScrollRight 右侧仍有被遮挡的内容。
     *
     * 只在状态真正变化时发出。外部按钮必须使用 retainSizeWhenHidden，否则显隐会改变
     * 视口可用宽度，反过来又改变溢出判定，形成抖动死循环。
     */
    void scrollStateChanged(bool scrollable, bool canScrollLeft, bool canScrollRight);

protected:
    /** @brief 视口尺寸变化后重新摆放标签栏并收敛滚动偏移。 */
    void resizeEvent(QResizeEvent *event) override;

    /**
     * @brief 标签栏建议尺寸变化（LayoutRequest）时同步几何与滚动状态。
     *
     * 只靠 tabLayoutChanged 信号不够：主窗口删除、插入镜像标签时会用 QSignalBlocker 屏蔽
     * 标签栏的 currentChanged，连带屏蔽了布局通知，视口便停在旧宽度，滚动箭头也不消失。
     * 标签栏的 updateGeometry() 会向没有布局的父控件投递 LayoutRequest，它不受信号屏蔽影响。
     */
    bool event(QEvent *event) override;

    /**
     * @brief 拦截标签栏上的滚轮事件，在溢出时转为像素滚动。
     * @param watched 事件来源，只处理内部标签栏。
     * @param event 当前事件。
     * @return 已作为滚动消费时返回 true；未溢出时交还标签栏的默认行为。
     */
    bool eventFilter(QObject *watched, QEvent *event) override;

    /**
     * @brief 新监听者连接 scrollStateChanged 时补发一次当前状态。
     * @param signal 本次被连接的信号。
     *
     * 状态只在变化时发出；监听者若在状态稳定后才连接，就永远学不到真实状态，外部
     * 箭头会卡在错误的初始显隐上。
     */
    void connectNotify(const QMetaMethod &signal) override;

private:
    /**
     * @brief 处理一次滚轮输入。
     * @param event 滚轮事件。
     * @return 是否已消费。
     *
     * 触控板提供 pixelDelta 时逐像素跟手；普通鼠标滚轮只有 angleDelta，按每格固定像素
     * 折算，同样落在像素级偏移上，而不是跳过一整个标签。
     */
    bool handleWheel(QWheelEvent *event);

    /** @brief 设置滚动偏移并立即移动标签栏；会收敛到合法范围。 */
    void setScrollOffset(int offset);

    /** @brief 按当前标签总宽度和视口宽度摆放标签栏，必要时收敛偏移。 */
    void syncTabBarGeometry();

    /** @brief 标签布局变化后同步视口建议尺寸，并在事件循环中补一次几何同步。 */
    void handleTabLayoutChanged();

    /** @brief 让指定标签完整落入视口；已经可见时不滚动。 */
    void ensureTabVisible(int index);

    /** @brief 停止进行中的平滑滚动，偏移停留在当前位置。 */
    void stopScrollAnimation();

    /** @brief 重新计算滚动状态，变化时发出 scrollStateChanged。 */
    void syncScrollState();

    /** @brief 标签全部内容的宽度，即标签栏的建议宽度。 */
    [[nodiscard]] int contentWidth() const;

    SessionTabBar *m_tabBar = nullptr;
    /** @brief 当前滚动偏移（像素），标签栏位于 x = -m_scrollOffset。 */
    int m_scrollOffset = 0;
    /** @brief 平滑滚动的终点；连续点击箭头时在它的基础上累加，而不是从半途起算。 */
    int m_scrollTarget = 0;
    /** @brief 平滑滚动动画；由本控件持有。 */
    QVariantAnimation *m_scrollAnimation = nullptr;
    /** @brief 已投递但尚未执行的几何同步，避免一次批量增删标签排出多次同步。 */
    bool m_geometrySyncPending = false;
    /** @brief 鼠标滚轮 angleDelta 的未满一像素余量，保证高精度滚轮的小增量不被丢弃。 */
    int m_wheelAngleRemainder = 0;

    /** @brief 最近一次发出的滚动状态，用于抑制重复信号。 */
    bool m_scrollable = false;
    bool m_canScrollLeft = false;
    bool m_canScrollRight = false;
    /** @brief 正在发出 scrollStateChanged；期间的重算请求推迟到发射结束后执行。 */
    bool m_emittingScrollState = false;
    /** @brief 发射期间有新的重算请求，外层发射结束后需要再算一次。 */
    bool m_scrollStateDirty = false;
};
