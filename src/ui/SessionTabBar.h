#pragma once

#include <QColor>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QTabBar>

class QAbstractButton;
class QMetaMethod;
class QMouseEvent;
class QPainter;
class QPaintEvent;
class QResizeEvent;
class QVariantAnimation;
class QWheelEvent;
class QWidget;

/**
 * @brief 用于窗口标题区的胶囊式会话标签栏。
 *
 * Qt 原生 QTabBar 的外观更接近文档页签，并且关闭按钮会永久占用每个标签的空间。
 * 本控件保留 QTabBar 已验证的索引、拖动和滚动行为，只接管尺寸与绘制：当前标签使用
 * 高对比圆角底色，关闭按钮仅在悬停标签中出现，从而得到接近现代终端客户端的
 * 紧凑标题栏体验。控件不持有任何终端对象，标签生命周期仍由 MainWindow 统一管理。
 */
class SessionTabBar final : public QTabBar {
    Q_OBJECT

public:
    /**
     * @brief 顶层标签的业务类型，用于在标题栏中区分单终端与多终端工作空间。
     *
     * 类型只影响图标和拖入校验，不改变 QTabBar 的索引语义；真实页面仍由 MainWindow
     * 持有。这样标题栏不会反向依赖终端控件，也便于自动化用例单独验证拖放状态机。
     */
    enum class TabKind {
        Terminal,
        Workspace
    };

    /**
     * @brief 标签对应会话的连接状态，驱动终端图标右下角的微型状态点。
     *
     * Unknown 表示"还没有连接结果可展示"（刚打开的本地终端、正在首次建立连接前的
     * 一瞬间），此时不画点——每个标签常驻一个默认灰点只会制造噪音，不传递任何信息。
     */
    enum class ConnectionState {
        Unknown,
        Connecting,
        Connected,
        Failed
    };

    /**
     * @brief 创建标题栏会话标签控件。
     * @param parent Qt 父控件，通常是窗口顶部的自定义标题区。
     */
    explicit SessionTabBar(QWidget *parent = nullptr);

    /**
     * @brief 更新指定标签的连接状态点，立即重绘。
     * @param index 目标标签的当前索引；越界时安全忽略。
     * @param state 新的连接状态。
     *
     * 状态点画在每一个标签上（不只是当前选中的那个），因为它回答的是"哪些后台标签
     * 掉线了"，而选中态本身已经通过胶囊底色单独表达，两者不能合并成一个视觉元素。
     */
    void setTabConnectionState(int index, ConnectionState state);

    /**
     * @brief 设置指定标签的业务类型并立即刷新图标。
     * @param index 目标标签当前索引；越界时安全忽略。
     * @param kind 单终端或工作空间。
     */
    void setTabKind(int index, TabKind kind);

    /** @brief 返回指定标签的业务类型；无效索引按单终端处理以保持向后兼容。 */
    [[nodiscard]] TabKind tabKind(int index) const;

    /**
     * @brief 更新标签栏颜色并立即重绘。
     * @param textColor 未选中标签的文字颜色。
     * @param selectedTextColor 当前标签的文字与图标颜色。
     * @param hoverColor 未选中标签悬停时的背景颜色。
     * @param selectedColor 当前标签的胶囊背景颜色。
     *
     * 颜色由 MainWindow 根据深浅主题集中下发，避免该控件私自读取全局 Palette 后与始终
     * 保持深色的原生标题区产生反差。该方法只改变展示，不改变当前索引或标签顺序。
     */
    void setVisualColors(const QColor &textColor,
                         const QColor &selectedTextColor,
                         const QColor &hoverColor,
                         const QColor &selectedColor);

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
     * @brief 用户把一个终端标签拖放到此前选中的标签或页面/侧栏接收区。
     * @param sourceIndex 被拖入的单终端标签在释放时的索引。
     * @param targetIndex 接收拖入的终端或工作空间标签在释放时的索引。
     * @param globalDropPosition 鼠标释放时的全局坐标，供 MainWindow 识别终端页或工作空间侧栏。
     * @param droppedOnTargetTab true 表示直接落在目标胶囊；false 表示需要进一步命中接收控件。
     */
    void workspaceMergeRequested(int sourceIndex,
                                 int targetIndex,
                                 const QPoint &globalDropPosition,
                                 bool droppedOnTargetTab);

    /**
     * @brief 指针拖着一个终端标签离开了标签栏下沿，进入"拖进终端区/侧栏"手势。
     *
     * MainWindow 收到后显示并置顶中央落点提示层。此后跟随光标的是本控件创建的浮动标签
     * 快照（一个无边框顶层控件），释放仍由 mouseReleaseEvent 按全局坐标决定是否合并。
     */
    void externalDragStarted();

    /** @brief 拖出手势进行中，浮动快照移到了新的全局坐标。用于更新落点提示。 */
    void externalDragMoved(const QPoint &globalPosition);

    /** @brief 上述手势结束（释放或取消），落点提示层应隐藏。 */
    void externalDragFinished();

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

    /**
     * @brief 绘制所有可见标签、拖拽预览、终端图标、标题和悬停关闭按钮。
     * @param event 当前重绘事件；事件区域用于裁剪无关绘制。
     *
     * QTabBar 的原生拖拽偏移属于私有状态，完全自绘后不能再由基类 paintEvent 帮忙应用。
     * 本实现因此使用稳定标签标识和公开的 tabRect() 自行合成拖拽及换位动画，避免原位置
     * 标签与 Qt 原生拖拽快照同时出现而形成文字重影。
     */
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

    /**
     * @brief 标签插入后为其分配进程内唯一标识。
     * @param index 新标签插入后的索引。
     *
     * 动画不能用索引标识标签，因为拖动换位本身会改变索引；单独维护稳定标识才能在
     * tabMoved 前后正确匹配同一个可视标签，并且不会占用业务层可能使用的 tabData。
     */
    void tabInserted(int index) override;

    /**
     * @brief 标签删除后同步移除稳定标识并终止对应动画。
     * @param index 被删除标签原先所在的索引。
     */
    void tabRemoved(int index) override;

private:
    /**
     * @brief 计算指定标签内关闭按钮的点击区域。
     * @param index 标签索引；无效索引返回空矩形。
     * @return 相对于标签栏的关闭按钮矩形。
     */
    [[nodiscard]] QRect closeButtonRect(int index) const;

    /**
     * @brief 在给定矩形中绘制一个完整的会话标签。
     * @param painter 已启用抗锯齿的目标画笔。
     * @param index 标签当前逻辑索引，用于读取标题和选中状态。
     * @param rawRect 已叠加动画或拖拽偏移的可视矩形。
     * @param hovered 是否绘制悬停背景与关闭按钮。
     *
     * 静态标签、换位动画标签和鼠标下的拖拽标签必须共用这一条绘制路径，否则拖动时
     * Qt 的原生快照与自绘外观不一致，会再次出现两套文字轮廓叠加的问题。
     */
    void drawSessionTab(QPainter &painter,
                        int index,
                        const QRect &rawRect,
                        bool hovered) const;

    /**
     * @brief 处理 QTabBar 已完成的索引移动，同步稳定标签标识的顺序。
     * @param from 移动前索引。
     * @param to 移动后索引。
     */
    void handleTabMoved(int from, int to);

    /**
     * @brief 返回指定索引对应的稳定标签标识。
     * @param index 当前标签索引。
     * @return 有效标识；索引无效或标识尚未同步时返回 0。
     */
    [[nodiscard]] quint64 tabIdAt(int index) const;

    /**
     * @brief 查找稳定标签标识当前所在的逻辑索引。
     * @param tabId 标签稳定标识。
     * @return 当前索引；标签已删除时返回 -1。
     */
    [[nodiscard]] int indexForTabId(quint64 tabId) const;

    /**
     * @brief 返回叠加当前动画横向偏移后的标签矩形。
     * @param index 当前标签索引。
     * @return 用于本帧绘制的可视矩形；无效索引返回空矩形。
     */
    [[nodiscard]] QRect visualTabRect(int index) const;

    /**
     * @brief 从指定横向偏移平滑回到标签的最终布局位置。
     * @param tabId 需要移动的稳定标签标识。
     * @param startOffset 动画首帧相对 tabRect() 的横向像素偏移。
     * @param duration 动画时长（毫秒）；0 表示尊重系统关闭动画的设置并立即落位。
     *
     * 若同一标签尚在上一段动画中，新动画会从它当前的屏幕位置续接，避免用户快速跨越
     * 多个标签时出现回跳。动画对象由本控件持有并在结束后自动清理。
     */
    void startTabMoveAnimation(quint64 tabId, int startOffset, int duration);

    /**
     * @brief 停止并清理指定标签尚未完成的位移动画。
     * @param tabId 标签稳定标识；不存在对应动画时安全无操作。
     */
    void stopTabMoveAnimation(quint64 tabId);

    /**
     * @brief 隐藏 Qt 为原生 QTabBar 创建的拖拽快照子控件。
     * @param childrenBeforeMove 调用基类 mouseMoveEvent 前存在的直属子控件集合。
     *
     * 原生快照使用系统 QTabBar 样式，无法复用本类的胶囊绘制，而且本类不调用基类
     * paintEvent 后它也不会跟随鼠标更新位置。首次拖动时通过“本次新增直属子控件”定位它，
     * 后续保存弱引用；只隐藏该新增控件，不依赖 Qt 私有类名或对象名。
     */
    void hideNativeDragPreview(const QList<QWidget *> &childrenBeforeMove);

    /**
     * @brief 清空一次鼠标拖拽手势的临时状态，但保留仍在运行的落位动画。
     *
     * 该方法用于正常释放、关闭标签以及基类因鼠标按键丢失而取消拖拽的路径，确保下一次
     * 按下不会沿用旧标签标识或旧坐标。
     */
    void resetPointerDragState();

    /** @brief 把内置滚动按钮移出可视区域，并在状态变化时发出 scrollStateChanged。 */
    void syncScrollButtons();

    /** @brief 创建跟随光标的浮动标签快照（无边框顶层控件），并发 externalDragStarted。 */
    void startExternalDragGhost();

    /** @brief 把浮动标签快照移到给定全局坐标下方偏移处。 */
    void moveExternalDragGhost(const QPoint &globalPosition);

    /** @brief 销毁浮动标签快照并发 externalDragFinished；无快照时安全无操作。 */
    void endExternalDragGhost();

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
    /**
     * @brief 鼠标精确落在关闭按钮命中区内的标签；-1 表示没有。
     *
     * 必须由鼠标事件维护，不能在 paintEvent 里现查 QCursor::pos()：绘制随时可能被别的
     * 原因触发（窗口激活、grab()、其他控件重叠），那一刻的全局光标位置与控件真正收到的
     * 事件并不一定一致，反馈就会时有时无。
     */
    int m_closeHoveredIndex = -1;
    /** @brief 未选中标签文字色。 */
    QColor m_textColor = QColor(QStringLiteral("#aeb6cc"));
    /** @brief 当前标签文字色。 */
    QColor m_selectedTextColor = QColor(QStringLiteral("#f8f9fd"));
    /** @brief 普通标签悬停背景色。 */
    QColor m_hoverColor = QColor(QStringLiteral("#353c56"));
    /** @brief 当前标签胶囊背景色。 */
    QColor m_selectedColor = QColor(QStringLiteral("#424b68"));
    /**
     * @brief 鼠标精确悬停在关闭按钮上时叉号线条的颜色。
     *
     * 悬停整个标签只让叉号"出现"（仍用默认前景色）；只有鼠标坐标真正落在关闭按钮命中
     * 区内才需要额外的强反馈，否则用户区分不出"标签悬停"和"即将点击关闭"这两种状态。
     */
    QColor m_closeHoverColor = QColor(QStringLiteral("#ff6b6b"));
    /** @brief 滚轮横向/纵向 delta 的累积值，凑够一格（120）才触发一次滚动，避免跳跃。 */
    int m_wheelDeltaAccumulator = 0;

    /**
     * @brief 与当前标签顺序一一对应的稳定标识列表。
     *
     * 标识只服务于绘制动画，不参与持久化，也不会写入 tabData，避免与 MainWindow 后续
     * 可能保存的会话业务数据发生字段冲突。0 被保留为“无效标识”。
     */
    QList<quint64> m_tabIds;
    /** @brief 下一个可分配的稳定标签标识；会话进程内单调递增即可保证唯一。 */
    quint64 m_nextTabId = 1;
    /** @brief 各标签当前相对 tabRect() 的横向动画偏移，单位为逻辑像素。 */
    QHash<quint64, int> m_tabAnimationOffsets;
    /** @brief 各标签正在运行的动画对象，由本控件负责停止与延迟销毁。 */
    QHash<quint64, QVariantAnimation *> m_tabMoveAnimations;
    /** @brief 各标签当前的连接状态，按稳定标识索引，随标签关闭一并清理。 */
    QHash<quint64, ConnectionState> m_tabConnectionStates;
    /** @brief 各标签的业务类型，按稳定标识保存，拖动换位后无需按索引搬运。 */
    QHash<quint64, TabKind> m_tabKinds;

    /** @brief 本次左键按下的标签稳定标识；0 表示没有可拖动标签。 */
    quint64 m_pressedTabId = 0;
    /** @brief 本次拖动允许接收合并的、按下前已选中标签标识；0 表示只做普通排序。 */
    quint64 m_workspaceDropTargetTabId = 0;
    /** @brief 目标标签在按下源标签前的屏幕矩形，用于避免基类重排后命中区随目标移走。 */
    QRect m_workspaceDropTargetRect;
    /**
     * @brief 当前指针是否位于可触发工作空间合并的区域。
     *
     * 该状态只驱动目标标签的虚线高亮；最终释放仍会由 MainWindow 使用全局坐标再次校验，
     * 防止鼠标在窗口外释放时误合并。
     */
    bool m_workspaceDropArmed = false;
    /** @brief 左键按下时相对于标签栏的坐标，用于复用 Qt 的拖拽启动距离语义。 */
    QPoint m_dragStartPosition;
    /** @brief 鼠标按下点到标签左边缘的距离，使拖拽标签始终保持原抓取位置。 */
    int m_pressedOffsetWithinTab = 0;
    /** @brief 最近一次带左键的移动坐标，用于绘制随鼠标移动的唯一拖拽标签。 */
    QPoint m_lastDragPosition;
    /** @brief 是否已越过系统拖拽阈值并进入标签重排状态。 */
    bool m_dragActive = false;
    /** @brief 本次手势是否已进入"拖出标签栏"状态（浮动快照可见、落点层已请求显示）。 */
    bool m_externalDragActive = false;
    /** @brief 跟随光标的浮动标签快照；无边框顶层控件，仅在拖出标签栏期间存在。 */
    QPointer<QWidget> m_dragGhost;

    /**
     * @brief Qt 内部原生拖拽快照的弱引用。
     *
     * 该子控件仍由 QTabBar 管理生命周期；这里只在拖动期间将其隐藏，防止系统样式文字与
     * 本类自绘文字叠加。QPointer 可在 Qt 内部提前销毁它时自动置空，避免悬空访问。
     */
    QPointer<QWidget> m_nativeDragPreview;
};
