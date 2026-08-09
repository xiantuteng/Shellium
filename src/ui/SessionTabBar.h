#pragma once

#include <QColor>
#include <QTabBar>

class QAbstractButton;
class QMetaMethod;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QWheelEvent;
class QWidget;

/**
 * @brief 用于窗口标题区的胶囊式会话标签栏。
 *
 * Qt 原生 QTabBar 的外观更接近文档页签，并且关闭按钮会永久占用每个标签的空间。
 * 本控件保留 QTabBar 已验证的索引、拖动和滚动行为，只接管尺寸与绘制：当前标签使用
 * 高对比圆角底色，关闭按钮仅在当前或悬停标签中出现，从而得到接近现代终端客户端的
 * 紧凑标题栏体验。控件不持有任何终端对象，标签生命周期仍由 MainWindow 统一管理。
 */
class SessionTabBar final : public QTabBar {
    Q_OBJECT

public:
    /**
     * @brief 创建标题栏会话标签控件。
     * @param parent Qt 父控件，通常是窗口顶部的自定义标题区。
     */
    explicit SessionTabBar(QWidget *parent = nullptr);

    /**
     * @brief 更新标签栏颜色并立即重绘。
     * @param textColor 未选中标签的文字颜色。
     * @param selectedTextColor 当前标签的文字与图标颜色。
     * @param hoverColor 未选中标签悬停时的背景颜色。
     * @param selectedColor 当前标签的胶囊背景颜色。
     * @param accentColor 当前会话的连接感知强调色。
     *
     * 颜色由 MainWindow 根据深浅主题集中下发，避免该控件私自读取全局 Palette 后与始终
     * 保持深色的原生标题区产生反差。该方法只改变展示，不改变当前索引或标签顺序。
     */
    void setVisualColors(const QColor &textColor,
                         const QColor &selectedTextColor,
                         const QColor &hoverColor,
                         const QColor &selectedColor,
                         const QColor &accentColor);

    /**
     * @brief 向左滚动一格标签。
     *
     * QTabBar 的滚动偏移量是私有实现细节，无法直接读写。本方法转而驱动 QTabBar 自带的
     * 内置滚动按钮，从而完整复用 Qt 已验证的滚动、边界和动画行为；内置按钮本身被移出
     * 可视区域，由标题栏中左右分置的外部按钮代为呈现。
     */
    void scrollTabsLeft();

    /** @brief 向右滚动一格标签；实现方式与 scrollTabsLeft 相同。 */
    void scrollTabsRight();

signals:
    /**
     * @brief 标签是否溢出、以及两端是否还能继续滚动发生变化。
     * @param scrollable 标签总宽度超过可视宽度，需要显示外部滚动按钮。
     * @param canScrollLeft 左侧仍有被遮挡的标签。
     * @param canScrollRight 右侧仍有被遮挡的标签。
     *
     * 只在状态真正变化时发出。外部按钮必须使用 retainSizeWhenHidden，否则显隐会改变
     * 标签栏可用宽度，反过来又改变溢出判定，形成与终端滚动条同类的抖动死循环。
     */
    void scrollStateChanged(bool scrollable, bool canScrollLeft, bool canScrollRight);

protected:
    /** @brief 尺寸变化后重新同步滚动状态。 */
    void resizeEvent(QResizeEvent *event) override;

    /** @brief 标签增删后重新同步滚动状态。 */
    void tabLayoutChange() override;

    /**
     * @brief 监视内置滚动按钮的可用性与显隐变化。
     * @param watched 事件来源，只处理两个内置滚动按钮。
     * @param event 当前事件。
     * @return 始终交还给基类继续处理，本过滤器只做观察。
     *
     * QTabBar 完成一次滚动后只更新内置按钮的 enabled 状态并重绘，并不会调用
     * tabLayoutChange()。仅依赖布局回调会让外部箭头停留在滚动前的可用状态——向右滚动
     * 之后左箭头依旧是灰的，用户再也回不去。
     */
    bool eventFilter(QObject *watched, QEvent *event) override;

    /**
     * @brief 新监听者连接 scrollStateChanged 时补发一次当前状态。
     * @param signal 本次被连接（或断开）的信号。
     *
     * syncScrollButtons() 只在状态真正变化时才发出信号：如果监听者是在状态已经稳定之后
     * 才 connect() 上来的（例如外部按钮的连接语句写在初始 addTab 循环之后，而此时标签栏
     * 尚未参与真实布局、宽度还是构造期的临时值), 且后续真实布局算出的溢出结论恰好与临时
     * 值算出的结论相同，就再也不会有变化触发新的信号——新监听者会永远拿不到当前真实状态，
     * 外部箭头因而卡在错误的初始显隐上。这里在连接建立后补发一次缓存值弥补这个空窗，对
     * 已经连接的旧监听者只是重复一次相同数值，不会产生副作用。
     */
    void connectNotify(const QMetaMethod &signal) override;

    /**
     * @brief 标签溢出时，让横向/纵向滚轮驱动标签左右滚动。
     * @param event 当前滚轮事件。
     *
     * QTabBar 默认不处理滚轮；只有 m_scrollable 为 true（确实有内容被遮挡）时才消费事件，
     * 否则交还基类，避免在标签没有溢出时吞掉本该继续向上传递的滚轮事件。
     */
    void wheelEvent(QWheelEvent *event) override;

    /** @brief 返回包含胶囊留白和按需关闭按钮空间的标签建议尺寸。 */
    QSize tabSizeHint(int index) const override;

    /** @brief 绘制所有可见标签、终端图标、标题和悬停关闭按钮。 */
    void paintEvent(QPaintEvent *event) override;

    /**
     * @brief 在标签栏空白区按下左键时启动原生窗口拖动。
     * @param event 当前鼠标按下事件。
     *
     * 只接管 tabAt() 返回 -1 的空白区域；标签胶囊内部仍交给 QTabBar，确保选择、拖动排序
     * 与自绘关闭按钮保持原有行为。
     */
    void mousePressEvent(QMouseEvent *event) override;

    /**
     * @brief 双击标签栏空白区时切换窗口的标准放大/恢复状态。
     * @param event 当前鼠标双击事件。
     *
     * 双击已有标签仍由 QTabBar 处理，避免标题栏窗口手势吞掉标签自身的扩展交互。
     */
    void mouseDoubleClickEvent(QMouseEvent *event) override;

    /** @brief 跟踪悬停标签，使关闭按钮和背景无需点击即可反馈。 */
    void mouseMoveEvent(QMouseEvent *event) override;

    /** @brief 鼠标离开后清除悬停态，避免关闭按钮残留。 */
    void leaveEvent(QEvent *event) override;

    /** @brief 命中自绘关闭按钮时发出 QTabBar::tabCloseRequested。 */
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    /**
     * @brief 计算指定标签内关闭按钮的点击区域。
     * @param index 标签索引；无效索引返回空矩形。
     * @return 相对于标签栏的关闭按钮矩形。
     */
    [[nodiscard]] QRect closeButtonRect(int index) const;

    /** @brief 把内置滚动按钮移出可视区域，并在状态变化时发出 scrollStateChanged。 */
    void syncScrollButtons();

    /**
     * @brief QTabBar 自带的左右滚动按钮，由 Qt 在构造时创建并全权管理生命周期。
     *
     * 通过 Qt 固定的对象名查找；查不到时两者保持为空，外部滚动按钮相应保持隐藏，
     * 标签栏退化为 Qt 默认行为而不会崩溃。
     */
    QAbstractButton *m_internalScrollLeft = nullptr;
    QAbstractButton *m_internalScrollRight = nullptr;

    /** @brief 最近一次发出的滚动状态，用于抑制重复信号并避免布局递归。 */
    bool m_scrollable = false;
    bool m_canScrollLeft = false;
    bool m_canScrollRight = false;

    /** @brief 当前鼠标悬停标签；-1 表示鼠标不位于任何标签。 */
    int m_hoveredIndex = -1;
    /** @brief 未选中标签文字色。 */
    QColor m_textColor = QColor(QStringLiteral("#aeb6cc"));
    /** @brief 当前标签文字色。 */
    QColor m_selectedTextColor = QColor(QStringLiteral("#f8f9fd"));
    /** @brief 普通标签悬停背景色。 */
    QColor m_hoverColor = QColor(QStringLiteral("#353c56"));
    /** @brief 当前标签胶囊背景色。 */
    QColor m_selectedColor = QColor(QStringLiteral("#424b68"));
    /** @brief 当前标签终端图标与微型状态点的强调色。 */
    QColor m_accentColor = QColor(QStringLiteral("#2bd981"));
    /**
     * @brief 鼠标精确悬停在关闭按钮上时叉号线条的颜色。
     *
     * 悬停整个标签只让叉号"出现"（仍用默认前景色）；只有鼠标坐标真正落在关闭按钮命中
     * 区内才需要额外的强反馈，否则用户区分不出"标签悬停"和"即将点击关闭"这两种状态。
     */
    QColor m_closeHoverColor = QColor(QStringLiteral("#ff6b6b"));
    /** @brief 滚轮横向/纵向 delta 的累积值，凑够一格（120）才触发一次滚动，避免跳跃。 */
    int m_wheelDeltaAccumulator = 0;
};
