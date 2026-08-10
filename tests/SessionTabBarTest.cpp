#include "ui/SessionTabBar.h"

#include <QAbstractButton>
#include <QImage>
#include <QMouseEvent>
#include <QProxyStyle>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>
#include <QWidget>

namespace {

/**
 * @brief 向控件同步投递一条携带指定按键状态的鼠标移动事件。
 * @param widget 事件接收控件。
 * @param position 相对于控件的逻辑像素坐标。
 * @param buttons 移动发生时仍按住的鼠标键集合。
 *
 * QTest::mouseMove() 固定生成“不带按键”的移动事件，无法进入 QTabBar 的拖拽重排状态；
 * 直接构造事件既能精确控制按键，也避免 macOS 测试进程缺少辅助功能权限时全局光标移动
 * 被系统静默拒绝。
 */
void sendMouseMove(QWidget &widget, const QPoint &position, const Qt::MouseButtons buttons)
{
    QMouseEvent event(QEvent::MouseMove,
                      QPointF(position),
                      QPointF(widget.mapToGlobal(position)),
                      Qt::NoButton,
                      buttons,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &event);
}

/**
 * @brief 统计逻辑矩形内与目标颜色接近的设备像素数量。
 * @param frame QWidget::grab() 得到的高 DPI 位图。
 * @param logicalRect 相对于控件的逻辑像素采样范围。
 * @param target 目标颜色。
 * @param tolerance 每个 RGB 通道允许的抗锯齿误差。
 * @return 命中颜色的设备像素数量；采样范围无效时返回 0。
 *
 * Retina 屏幕上 QPixmap 坐标使用设备像素，测试几何使用逻辑像素。统一乘以 DPR 后再采样，
 * 才不会把 2x 图像的左上四分之一误当成完整标签；忽略 alpha 是因为 grab() 可能已把透明
 * 标签栏与父背景完成合成，但胶囊内部的 RGB 仍保持目标颜色。
 */
[[nodiscard]] int countPixelsNearColor(const QPixmap &frame,
                                       const QRect &logicalRect,
                                       const QColor &target,
                                       const int tolerance = 3)
{
    const QImage image = frame.toImage();
    const qreal ratio = frame.devicePixelRatio();
    const QRect deviceBounds = image.rect();
    const QRect deviceRect(qFloor(logicalRect.left() * ratio),
                           qFloor(logicalRect.top() * ratio),
                           qCeil(logicalRect.width() * ratio),
                           qCeil(logicalRect.height() * ratio));
    const QRect sampleRect = deviceRect.intersected(deviceBounds);
    int matchingPixels = 0;
    for (int y = sampleRect.top(); y <= sampleRect.bottom(); ++y) {
        for (int x = sampleRect.left(); x <= sampleRect.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (qAbs(pixel.red() - target.red()) <= tolerance
                && qAbs(pixel.green() - target.green()) <= tolerance
                && qAbs(pixel.blue() - target.blue()) <= tolerance) {
                ++matchingPixels;
            }
        }
    }
    return matchingPixels;
}

/**
 * @brief 为视觉动画用例提供确定的非零系统动画时长。
 *
 * QTabBar 生产实现会尊重 SH_Widget_Animation_Duration=0 的“减少动态效果”设置。自动化测试
 * 不能依赖执行机器的个人辅助功能配置，因此只在该用例的局部代理样式中固定为 180ms；
 * 其他样式提示全部委托给平台原样式，标签尺寸和字体渲染仍与真实环境一致。
 */
class DeterministicAnimationStyle final : public QProxyStyle {
public:
    /** @brief 创建委托给应用当前样式的局部代理。 */
    DeterministicAnimationStyle() = default;

    /**
     * @brief 返回样式提示；仅覆盖通用控件动画时长。
     * @param hint 待查询的样式提示枚举。
     * @param option 可选的绘制状态，原样转交基类。
     * @param widget 发起查询的控件，原样转交基类。
     * @param returnData 可选扩展返回结构，原样转交基类。
     * @return 动画时长查询固定返回 180，其他查询返回基类结果。
     */
    int styleHint(StyleHint hint,
                  const QStyleOption *option = nullptr,
                  const QWidget *widget = nullptr,
                  QStyleHintReturn *returnData = nullptr) const override
    {
        if (hint == QStyle::SH_Widget_Animation_Duration) {
            return 180;
        }
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
};

} // namespace

/**
 * @brief 覆盖标题栏会话标签控件的滚动接线、关闭按钮命中判定与悬停反馈。
 *
 * 这些行为都依赖 QTabBar 的内部实现细节，或者是"信号发出时机早于监听者存在"这类容易被
 * 人工测试忽略的时序问题。纯人工点击很难发现这种"看起来正常"的退化，因此必须由自动化
 * 用例守住。
 */
class SessionTabBarTest final : public QObject {
    Q_OBJECT

private slots:
    /**
     * @brief 标签溢出时应通过 scrollStateChanged 通知外部按钮，并真正滚动标签。
     *
     * 用例同时断言"状态被上报"和"滚动确实发生"：只断言信号会让一个空实现的 scrollTabsRight
     * 也能通过，而滚动位移才是用户真正感知到的结果。
     */
    void reportsScrollStateAndScrollsTabs();

    /** @brief 未溢出时不得报告可滚动，否则标题栏会常驻两个无效箭头。 */
    void doesNotReportScrollableWhenTabsFit();

    /**
     * @brief 晚于状态稳定才连接 scrollStateChanged 的监听者也必须学到当前真实状态。
     *
     * 复现 MainWindow 的真实顺序：先在标签栏尚未参与真实布局（宽度还是构造期临时值）时
     * 连续 addTab，再连接信号，最后单跳 resize 到最终宽度。如果最终宽度算出的溢出结论
     * 与临时宽度算出的结论恰好相同，旧实现里 syncScrollButtons() 因"值未变化"不会重新
     * 发出信号，新监听者就永远学不到当前真实状态——外部箭头会卡在错误的初始显隐上。
     */
    void lateListenerLearnsCurrentScrollStateOnConnect();

    /** @brief 溢出时滚轮应驱动标签左右滚动，且每格滚轮只滚动一步。 */
    void wheelScrollsTabsWhenOverflowing();

    /** @brief 未溢出时滚轮不应被标签栏吞掉。 */
    void wheelDoesNothingWhenTabsFit();

    /** @brief 精确悬停在关闭按钮上时，叉号线条颜色必须与仅悬停标签时不同。 */
    void closeButtonStrokeChangesColorOnPreciseHover();

    /** @brief 拖拽开始后原逻辑位置不得继续绘制第二份选中标签。 */
    void draggingPaintsOnlyOneTabCopy();

    /** @brief 标签换位时，被让开的相邻标签应从旧位置平滑移动到新位置。 */
    void reorderedNeighborAnimatesFromPreviousPosition();
};

void SessionTabBarTest::reportsScrollStateAndScrollsTabs()
{
    SessionTabBar tabBar;
    // 单个标签最小宽度为 132，420 像素最多容纳 3 个；加入 12 个必然溢出。
    tabBar.resize(420, 42);
    tabBar.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tabBar));

    QSignalSpy scrollSpy(&tabBar, &SessionTabBar::scrollStateChanged);
    for (int index = 0; index < 12; ++index) {
        tabBar.addTab(QStringLiteral("终端 %1").arg(index + 1));
    }
    tabBar.setCurrentIndex(0);
    QCoreApplication::processEvents();

    QVERIFY2(!scrollSpy.isEmpty(),
             "标签溢出后必须上报可滚动状态，否则标题栏不会显示左右滚动按钮");
    const QList<QVariant> lastState = scrollSpy.last();
    QVERIFY2(lastState.at(0).toBool(), "溢出时 scrollable 必须为 true");
    QVERIFY2(lastState.at(2).toBool(), "停在最左端时右侧必须仍可滚动");

    // 位于最左端时不应报告可向左滚动，否则外部按钮会给出可点击却无效果的假反馈。
    QVERIFY2(!lastState.at(1).toBool(), "停在最左端时左侧不应可滚动");

    const int firstTabLeftBefore = tabBar.tabRect(0).left();
    tabBar.scrollTabsRight();
    QCoreApplication::processEvents();
    const int firstTabLeftAfter = tabBar.tabRect(0).left();
    QVERIFY2(firstTabLeftAfter < firstTabLeftBefore,
             "scrollTabsRight 必须真正改变标签滚动偏移；内置滚动按钮对象名变更会导致此处失败");

    // 向右滚动之后必须重新上报"左侧可滚动"，外部左箭头才会由禁用变为可用。
    QCoreApplication::processEvents();
    QVERIFY2(scrollSpy.last().at(1).toBool(), "滚动后左侧应变为可滚动");

    tabBar.scrollTabsLeft();
    QCoreApplication::processEvents();
    QCOMPARE(tabBar.tabRect(0).left(), firstTabLeftBefore);
}

void SessionTabBarTest::doesNotReportScrollableWhenTabsFit()
{
    SessionTabBar tabBar;
    tabBar.resize(900, 42);
    tabBar.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tabBar));

    QSignalSpy scrollSpy(&tabBar, &SessionTabBar::scrollStateChanged);
    tabBar.addTab(QStringLiteral("终端 1"));
    tabBar.addTab(QStringLiteral("终端 2"));
    QCoreApplication::processEvents();

    for (const QList<QVariant> &state : std::as_const(scrollSpy)) {
        QVERIFY2(!state.at(0).toBool(), "标签未溢出时不得报告可滚动");
    }
}

void SessionTabBarTest::lateListenerLearnsCurrentScrollStateOnConnect()
{
    auto *parent = new QWidget();
    auto *tabBar = new SessionTabBar(parent);
    // 40 个标签在构造期的临时宽度（未参与任何布局，通常是 QWidget 的默认宽度）和后面
    // 单跳 resize 到的 1200 像素下都必然溢出，因此两次判定结论都是 true，不会自然发生
    // "值变化"来触发信号——这正是复现该缺陷所需要的条件。
    for (int index = 0; index < 40; ++index) {
        tabBar->addTab(QStringLiteral("终端 %1").arg(index + 1));
    }

    QSignalSpy lateSpy(tabBar, &SessionTabBar::scrollStateChanged);
    QVERIFY2(!lateSpy.isEmpty(),
             "connect() 建立的同一时刻就应补发一次当前状态，哪怕状态早已稳定");
    QVERIFY2(lateSpy.last().at(0).toBool(),
             "补发的状态必须反映真实的当前溢出结论（此时已有 40 个标签，理应可滚动）");

    // 单跳 resize（不 show()，避免窗口系统插入多次中间尺寸的 resizeEvent，令状态在中途
    // 先翻转再翻回来，从而掩盖"从未变化"这种最危险的情形）。
    tabBar->resize(1200, 42);
    QCoreApplication::processEvents();

    QVERIFY2(!lateSpy.isEmpty(),
             "晚连接的监听者必须能学到当前真实的可滚动状态");
    QVERIFY2(lateSpy.last().at(0).toBool(),
             "40 个标签在 1200 像素宽度下依然溢出，晚连接的监听者不能收到 false");

    delete parent;
}

void SessionTabBarTest::wheelScrollsTabsWhenOverflowing()
{
    SessionTabBar tabBar;
    tabBar.resize(420, 42);
    tabBar.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tabBar));

    for (int index = 0; index < 12; ++index) {
        tabBar.addTab(QStringLiteral("终端 %1").arg(index + 1));
    }
    tabBar.setCurrentIndex(0);
    QCoreApplication::processEvents();

    const int firstTabLeftBefore = tabBar.tabRect(0).left();

    // 向下/向右滚滚轮（angleDelta 为负）应该让标签向右滚动。
    QWheelEvent rightEvent(QPointF(tabBar.width() / 2.0, tabBar.height() / 2.0),
                           tabBar.mapToGlobal(QPoint(tabBar.width() / 2, tabBar.height() / 2)),
                           QPoint(0, 0),
                           QPoint(0, -120),
                           Qt::NoButton,
                           Qt::NoModifier,
                           Qt::NoScrollPhase,
                           false);
    QCoreApplication::sendEvent(&tabBar, &rightEvent);
    QCoreApplication::processEvents();

    const int firstTabLeftAfterScrollRight = tabBar.tabRect(0).left();
    QVERIFY2(firstTabLeftAfterScrollRight < firstTabLeftBefore,
             "向下/向右滚动滚轮时标签必须向右滚动（更早的标签被卷出可视区）");

    // 向上/向左滚滚轮（angleDelta 为正）应该让标签向左滚回去。
    QWheelEvent leftEvent(QPointF(tabBar.width() / 2.0, tabBar.height() / 2.0),
                          tabBar.mapToGlobal(QPoint(tabBar.width() / 2, tabBar.height() / 2)),
                          QPoint(0, 0),
                          QPoint(0, 120),
                          Qt::NoButton,
                          Qt::NoModifier,
                          Qt::NoScrollPhase,
                          false);
    QCoreApplication::sendEvent(&tabBar, &leftEvent);
    QCoreApplication::processEvents();

    QCOMPARE(tabBar.tabRect(0).left(), firstTabLeftBefore);
}

void SessionTabBarTest::wheelDoesNothingWhenTabsFit()
{
    SessionTabBar tabBar;
    tabBar.resize(900, 42);
    tabBar.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tabBar));

    tabBar.addTab(QStringLiteral("终端 1"));
    tabBar.addTab(QStringLiteral("终端 2"));
    QCoreApplication::processEvents();

    const int firstTabLeftBefore = tabBar.tabRect(0).left();
    QWheelEvent event(QPointF(tabBar.width() / 2.0, tabBar.height() / 2.0),
                      tabBar.mapToGlobal(QPoint(tabBar.width() / 2, tabBar.height() / 2)),
                      QPoint(0, 0),
                      QPoint(0, -120),
                      Qt::NoButton,
                      Qt::NoModifier,
                      Qt::NoScrollPhase,
                      false);
    QCoreApplication::sendEvent(&tabBar, &event);
    QCoreApplication::processEvents();

    QCOMPARE(tabBar.tabRect(0).left(), firstTabLeftBefore);
}

void SessionTabBarTest::closeButtonStrokeChangesColorOnPreciseHover()
{
    SessionTabBar tabBar;
    tabBar.resize(400, 42);
    tabBar.addTab(QStringLiteral("终端 1"));
    tabBar.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tabBar));

    const QRect tabRect = tabBar.tabRect(0);
    // 关闭按钮固定在标签右侧 29~11 像素处（见 SessionTabBar::closeButtonRect），叉号左上到
    // 右下的对角线端点落在该矩形内、且不与背景圆的边缘重叠，是采样描边颜色的稳定位置。
    const QPoint closeButtonCenter(tabRect.right() - 20, tabRect.top() + tabRect.height() / 2);
    const QPoint strokeSamplePoint = closeButtonCenter + QPoint(-3, -3);

    // grab() 返回的是设备像素位图：Retina 上一个逻辑像素对应 2x2 个设备像素，直接拿逻辑
    // 坐标去 pixelColor() 会采到左上角四分之一处——那里只有胶囊底色，两次采样自然永远
    // 相等，用例于是恒假地"通过不了"。
    const auto sampleStroke = [strokeSamplePoint](const QPixmap &frame) {
        const QImage image = frame.toImage();
        const qreal ratio = frame.devicePixelRatio();
        return image.pixelColor(QPoint(qRound(strokeSamplePoint.x() * ratio),
                                       qRound(strokeSamplePoint.y() * ratio)));
    };

    // 悬停状态必须完全由控件收到的鼠标事件驱动，这里也就直接投递 QMouseEvent：
    // QCursor::setPos() 在 macOS 上没有辅助功能授权时是静默失效的，若绘制时现查全局光标，
    // 两帧量到的只会是同一个画面；QTest::mouseMove() 则要经过窗口系统队列，同一个可执行
    // 文件里跑过别的用例之后本窗口未必还是活动窗口，第二次移动会被整条丢掉。
    const auto hoverAt = [&tabBar](const QPoint &point) {
        QMouseEvent move(QEvent::MouseMove,
                         QPointF(point),
                         QPointF(tabBar.mapToGlobal(point)),
                         Qt::NoButton,
                         Qt::NoButton,
                         Qt::NoModifier);
        QCoreApplication::sendEvent(&tabBar, &move);
        QCoreApplication::processEvents();
    };

    // 先只悬停在标签上（远离关闭按钮的位置），触发默认前景色叉号。
    const QPoint plainHoverPoint(tabRect.left() + 20, tabRect.top() + tabRect.height() / 2);
    hoverAt(plainHoverPoint);
    const QColor plainHoverStrokeColor = sampleStroke(tabBar.grab());

    // 精确悬停到关闭按钮命中区内，叉号必须换成醒目的悬停色。
    hoverAt(closeButtonCenter);
    const QColor preciseHoverStrokeColor = sampleStroke(tabBar.grab());

    QVERIFY2(plainHoverStrokeColor != preciseHoverStrokeColor,
             "精确悬停在关闭按钮上时叉号线条颜色必须与仅悬停标签时不同，否则用户看不出"
             "鼠标真的移到了关闭按钮上");
}

void SessionTabBarTest::draggingPaintsOnlyOneTabCopy()
{
    SessionTabBar tabBar;
    tabBar.resize(520, 42);
    // 使用高饱和纯色可把胶囊主体与字体抗锯齿像素可靠区分；该颜色只作用于测试控件，
    // 不依赖操作系统当前深浅主题，也不会因父窗口背景变化产生误判。
    const QColor selectedColor(QStringLiteral("#ff0044"));
    tabBar.setVisualColors(QColor(QStringLiteral("#00ff66")),
                           QColor(QStringLiteral("#ffffff")),
                           QColor(QStringLiteral("#202020")),
                           selectedColor,
                           QColor(QStringLiteral("#00ffff")));
    tabBar.addTab(QStringLiteral("唯一拖拽标签"));
    tabBar.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tabBar));

    const QRect originalTabRect = tabBar.tabRect(0);
    const QPoint pressPosition = originalTabRect.center();
    // 目标位置与原标签间隔超过一个完整标签宽度，同时仍位于控件内部；这样两份图像若同时
    // 存在会落在完全分离的采样区，避免重叠部分掩盖“原位置残留”缺陷。
    const QPoint dragPosition(pressPosition.x() + originalTabRect.width() + 80,
                              pressPosition.y());
    QTest::mousePress(&tabBar, Qt::LeftButton, Qt::NoModifier, pressPosition);
    sendMouseMove(tabBar, dragPosition, Qt::LeftButton);

    const QPixmap draggingFrame = tabBar.grab();
    const QRect originalPillInterior = originalTabRect.adjusted(8, 8, -8, -8);
    QRect draggedPillInterior = originalPillInterior;
    draggedPillInterior.translate(dragPosition.x() - pressPosition.x(), 0);

    const int originalSelectedPixels = countPixelsNearColor(
        draggingFrame, originalPillInterior, selectedColor);
    const int draggedSelectedPixels = countPixelsNearColor(
        draggingFrame, draggedPillInterior, selectedColor);
    QVERIFY2(originalSelectedPixels < 12,
             "拖拽开始后原逻辑位置仍存在选中胶囊，说明自绘标签与 Qt 原生快照发生了重叠");
    QVERIFY2(draggedSelectedPixels > 300,
             "消除原位置重影时不能让拖拽标签消失；鼠标位置必须保留清晰的胶囊预览");

    QTest::mouseRelease(&tabBar, Qt::LeftButton, Qt::NoModifier, dragPosition);
}

void SessionTabBarTest::reorderedNeighborAnimatesFromPreviousPosition()
{
    DeterministicAnimationStyle animationStyle;
    SessionTabBar tabBar;
    // QWidget::setStyle 不接管样式对象；局部变量先于 tabBar 构造、后于 tabBar 析构，既能
    // 保证整个用例期间指针有效，也不会改变 qApp 的全局样式而污染其他测试。
    tabBar.setStyle(&animationStyle);
    tabBar.resize(420, 42);
    const QColor neighborColor(QStringLiteral("#00ff55"));
    tabBar.setVisualColors(neighborColor,
                           QColor(QStringLiteral("#ffffff")),
                           QColor(QStringLiteral("#303030")),
                           QColor(QStringLiteral("#4050ff")),
                           QColor(QStringLiteral("#00ffff")));
    tabBar.addTab(QStringLiteral("左侧让位标签"));
    tabBar.addTab(QStringLiteral("右侧拖拽标签"));
    tabBar.setCurrentIndex(1);
    tabBar.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tabBar));

    const QRect oldNeighborRect = tabBar.tabRect(0);
    const QRect draggedRect = tabBar.tabRect(1);
    const QPoint pressPosition(draggedRect.left() + 20, draggedRect.center().y());
    // 向左移动 100 像素已跨过相邻标签中心，足以触发换位；拖拽胶囊的新左边缘为 32，
    // 不会覆盖旧标签左侧 10~31 区域中的绿色终端图标，便于直接观察让位动画首帧。
    const QPoint dragPosition(pressPosition.x() - 100, pressPosition.y());
    QTest::mousePress(&tabBar, Qt::LeftButton, Qt::NoModifier, pressPosition);
    sendMouseMove(tabBar, dragPosition, Qt::LeftButton);

    QCOMPARE(tabBar.tabText(0), QStringLiteral("右侧拖拽标签"));
    const QRect oldNeighborIconArea(oldNeighborRect.left() + 10,
                                    oldNeighborRect.top() + 7,
                                    22,
                                    oldNeighborRect.height() - 14);
    const int greenPixelsAtAnimationStart = countPixelsNearColor(
        tabBar.grab(), oldNeighborIconArea, neighborColor, 8);
    QVERIFY2(greenPixelsAtAnimationStart > 4,
             "逻辑索引换位后，相邻标签应先停留在旧屏幕位置再开始移动，不能瞬间跳到终点");

    // 代理样式固定为 180ms，等待 260ms 可覆盖定时器调度抖动并确保动画 finished 已清理
    // 偏移；鼠标仍保持按下，所以这里只观察相邻标签动画，不混入松手后的拖拽标签落位。
    QTest::qWait(260);
    const int greenPixelsAfterAnimation = countPixelsNearColor(
        tabBar.grab(), oldNeighborIconArea, neighborColor, 8);
    QVERIFY2(greenPixelsAfterAnimation < greenPixelsAtAnimationStart / 3,
             "让位标签动画结束后仍停留在旧位置，说明偏移未收敛到新的 tabRect");

    QTest::mouseRelease(&tabBar, Qt::LeftButton, Qt::NoModifier, dragPosition);
}

QTEST_MAIN(SessionTabBarTest)

#include "SessionTabBarTest.moc"
