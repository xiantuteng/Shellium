#include "ui/SessionTabBar.h"
#include "ui/SessionTabStrip.h"

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
     * @brief 标签溢出时应上报可滚动状态，箭头按固定像素滚动而不是整格跳。
     *
     * 同时断言"状态被上报"和"位移确实发生且不是标签宽度的整数倍"：只断言信号会让一个
     * 空实现也通过，只断言位移又区分不出像素滚动与旧的整格滚动。
     */
    void arrowsScrollByPixelsAndReportState();

    /** @brief 未溢出时不得报告可滚动，否则标题栏会常驻两个无效箭头。 */
    void doesNotReportScrollableWhenTabsFit();

    /**
     * @brief 晚于状态稳定才连接 scrollStateChanged 的监听者也必须学到当前真实状态。
     *
     * 状态只在变化时发出；若监听者在状态稳定之后才连接，且之后再无变化，它就永远学不到
     * 真实状态，外部箭头会卡在错误的初始显隐上。
     */
    void lateListenerLearnsCurrentScrollStateOnConnect();

    /** @brief 触控板逐像素跟手，鼠标滚轮每格折算固定像素，且都收敛在合法范围内。 */
    void wheelScrollsByPixelsWhenOverflowing();

    /** @brief 未溢出时滚轮不应被视口吞掉。 */
    void wheelDoesNothingWhenTabsFit();

    /** @brief 选中被卷出视口的标签时，必须把它完整滚进视野。 */
    void selectingHiddenTabScrollsItIntoView();

    /**
     * @brief 监听者在处理状态信号时引发的嵌套重算不得嵌套发出信号。
     *
     * MainWindow 在信号里显示外部箭头，而显示子控件会同步激活父布局、改变视口宽度并
     * 再次计算状态。嵌套发出会让箭头在 setVisible(true) 返回前就被 setVisible(false)，
     * 留下"已隐藏却仍标记为可见"的矛盾状态。
     */
    void nestedStateChangeIsDeliveredAfterOuterSignal();

    /** @brief 精确悬停在关闭按钮上时，叉号线条颜色必须与仅悬停标签时不同。 */
    void closeButtonStrokeChangesColorOnPreciseHover();

    /** @brief 拖拽开始后原逻辑位置不得继续绘制第二份选中标签。 */
    void draggingPaintsOnlyOneTabCopy();

    /** @brief 标签换位时，被让开的相邻标签应从旧位置平滑移动到新位置。 */
    void reorderedNeighborAnimatesFromPreviousPosition();

    /**
     * @brief 连接状态点必须按每个标签独立显示，且关闭的标签不能把状态漏给复用的新标签。
     *
     * 状态点不受"是否为当前选中标签"影响——它回答的是"这个标签的会话是否掉线"，选中态
     * 已经由胶囊底色单独表达。标签关闭后，稳定标识对应的状态必须一并清理，否则新开的
     * 标签一插入同一个索引就会平白顶着一个不属于自己的红点或绿点。
     */
    void connectionStateDotReflectsPerTabStateAndClearsOnRemoval();
};

namespace {

/** @brief 向标签栏投递一条滚轮事件；视口通过事件过滤器接收标签栏上的滚轮。 */
bool sendWheel(SessionTabBar *tabBar, const QPoint &pixelDelta, const QPoint &angleDelta)
{
    const QPointF local(tabBar->width() / 2.0, tabBar->height() / 2.0);
    QWheelEvent event(local,
                      tabBar->mapToGlobal(local),
                      pixelDelta,
                      angleDelta,
                      Qt::NoButton,
                      Qt::NoModifier,
                      Qt::NoScrollPhase,
                      false);
    event.setAccepted(false);
    QCoreApplication::sendEvent(tabBar, &event);
    return event.isAccepted();
}

/** @brief 创建一个装满标签、必然溢出的视口；单个标签至少 132 像素宽。 */
void fillOverflowingStrip(SessionTabStrip &strip, const int tabCount = 12)
{
    strip.resize(420, 42);
    for (int index = 0; index < tabCount; ++index) {
        strip.tabBar()->addTab(QStringLiteral("终端 %1").arg(index + 1));
    }
    strip.tabBar()->setCurrentIndex(0);
}

} // namespace

void SessionTabBarTest::arrowsScrollByPixelsAndReportState()
{
    SessionTabStrip strip;
    QSignalSpy scrollSpy(&strip, &SessionTabStrip::scrollStateChanged);
    fillOverflowingStrip(strip);
    strip.show();
    QVERIFY(QTest::qWaitForWindowExposed(&strip));
    QCoreApplication::processEvents();

    QVERIFY2(!scrollSpy.isEmpty(), "标签溢出后必须上报可滚动状态");
    QVERIFY2(scrollSpy.last().at(0).toBool(), "溢出时 scrollable 必须为 true");
    QVERIFY2(!scrollSpy.last().at(1).toBool(), "停在最左端时左侧不应可滚动");
    QVERIFY2(scrollSpy.last().at(2).toBool(), "停在最左端时右侧必须仍可滚动");
    QCOMPARE(strip.scrollOffset(), 0);

    const int tabWidth = strip.tabBar()->tabRect(0).width();
    strip.scrollTabsRight();
    QTRY_VERIFY_WITH_TIMEOUT(strip.scrollOffset() > 0, 1000);
    // 等平滑滚动走完，读到的才是一次点击的完整步长。
    QTest::qWait(300);
    const int step = strip.scrollOffset();
    // 像素滚动的一步既不是 0 也不是一个标签的宽度：旧实现每次正好卷入一整个标签。
    QVERIFY2(step > 0 && step % tabWidth != 0,
             qPrintable(QStringLiteral("箭头一次滚动 %1 像素，仍是整格滚动（标签宽 %2）")
                            .arg(QString::number(step), QString::number(tabWidth))));
    // 视口移动的是整条标签栏，标签栏内部坐标保持不变。
    QCOMPARE(strip.tabBar()->x(), -step);
    QCOMPARE(strip.tabBar()->tabRect(0).left(), 0);
    QVERIFY2(scrollSpy.last().at(1).toBool(), "滚动后左侧应变为可滚动");

    // 连续点击在上一段的终点上累加，而不是从动画半途起算。
    strip.scrollTabsRight();
    strip.scrollTabsRight();
    QTRY_COMPARE_WITH_TIMEOUT(strip.scrollOffset(), step * 3, 1000);

    for (int click = 0; click < 3; ++click) {
        strip.scrollTabsLeft();
    }
    QTRY_COMPARE_WITH_TIMEOUT(strip.scrollOffset(), 0, 1000);
    QVERIFY2(!scrollSpy.last().at(1).toBool(), "回到最左端后左侧不应可滚动");

    // 一直向右点会停在最右端，最后一个标签右缘恰好贴住视口右缘。
    for (int click = 0; click < 40; ++click) {
        strip.scrollTabsRight();
    }
    QTRY_COMPARE_WITH_TIMEOUT(strip.scrollOffset(), strip.maximumScrollOffset(), 2000);
    const QRect lastTab = strip.tabBar()->tabRect(strip.tabBar()->count() - 1);
    QCOMPARE(strip.tabBar()->mapToParent(lastTab.topRight()).x() + 1, strip.width());
    QVERIFY2(!scrollSpy.last().at(2).toBool(), "到达最右端后右侧不应可滚动");
}

void SessionTabBarTest::doesNotReportScrollableWhenTabsFit()
{
    SessionTabStrip strip;
    strip.resize(900, 42);
    strip.show();
    QVERIFY(QTest::qWaitForWindowExposed(&strip));

    QSignalSpy scrollSpy(&strip, &SessionTabStrip::scrollStateChanged);
    strip.tabBar()->addTab(QStringLiteral("终端 1"));
    strip.tabBar()->addTab(QStringLiteral("终端 2"));
    QCoreApplication::processEvents();

    for (const QList<QVariant> &state : std::as_const(scrollSpy)) {
        QVERIFY2(!state.at(0).toBool(), "标签未溢出时不得报告可滚动");
    }
    QCOMPARE(strip.maximumScrollOffset(), 0);
}

void SessionTabBarTest::lateListenerLearnsCurrentScrollStateOnConnect()
{
    SessionTabStrip strip;
    fillOverflowingStrip(strip, 40);
    QCoreApplication::processEvents();

    QSignalSpy lateSpy(&strip, &SessionTabStrip::scrollStateChanged);
    QVERIFY2(!lateSpy.isEmpty(),
             "connect() 建立的同一时刻就应补发一次当前状态，哪怕状态早已稳定");
    QVERIFY2(lateSpy.last().at(0).toBool(),
             "补发的状态必须反映真实的当前溢出结论（40 个标签理应可滚动）");

    strip.resize(1200, 42);
    QCoreApplication::processEvents();
    QVERIFY2(lateSpy.last().at(0).toBool(),
             "40 个标签在 1200 像素宽度下依然溢出，晚连接的监听者不能收到 false");
}

void SessionTabBarTest::wheelScrollsByPixelsWhenOverflowing()
{
    SessionTabStrip strip;
    fillOverflowingStrip(strip);
    strip.show();
    QVERIFY(QTest::qWaitForWindowExposed(&strip));
    QCoreApplication::processEvents();

    // 触控板：向左轻扫 7 像素（pixelDelta 为负）就只滚 7 像素。
    QVERIFY(sendWheel(strip.tabBar(), QPoint(-7, 0), QPoint(-14, 0)));
    QCOMPARE(strip.scrollOffset(), 7);
    QCOMPARE(strip.tabBar()->x(), -7);

    // 普通鼠标滚轮只有 angleDelta：向下一格折算为固定像素，同样不是整格标签。
    QVERIFY(sendWheel(strip.tabBar(), QPoint(), QPoint(0, -120)));
    const int afterNotch = strip.scrollOffset();
    QVERIFY(afterNotch > 7);
    QVERIFY(afterNotch - 7 != strip.tabBar()->tabRect(0).width());

    // 高精度滚轮的小增量不能被整除截断丢掉：四次 30 等于一整格。
    for (int tick = 0; tick < 4; ++tick) {
        QVERIFY(sendWheel(strip.tabBar(), QPoint(), QPoint(0, -30)));
    }
    QCOMPARE(strip.scrollOffset(), afterNotch + (afterNotch - 7));

    // 向上 / 向左滚回去，并在左端收敛为 0。
    QVERIFY(sendWheel(strip.tabBar(), QPoint(0, 5000), QPoint(0, 10000)));
    QCOMPARE(strip.scrollOffset(), 0);

    // 右端同样收敛。
    QVERIFY(sendWheel(strip.tabBar(), QPoint(-50000, 0), QPoint(-100000, 0)));
    QCOMPARE(strip.scrollOffset(), strip.maximumScrollOffset());
}

void SessionTabBarTest::wheelDoesNothingWhenTabsFit()
{
    SessionTabStrip strip;
    strip.resize(900, 42);
    strip.tabBar()->addTab(QStringLiteral("终端 1"));
    strip.tabBar()->addTab(QStringLiteral("终端 2"));
    strip.show();
    QVERIFY(QTest::qWaitForWindowExposed(&strip));
    QCoreApplication::processEvents();

    QVERIFY(!sendWheel(strip.tabBar(), QPoint(0, -40), QPoint(0, -120))
            || strip.scrollOffset() == 0);
    QCOMPARE(strip.scrollOffset(), 0);
    QCOMPARE(strip.tabBar()->x(), 0);
}

void SessionTabBarTest::selectingHiddenTabScrollsItIntoView()
{
    SessionTabStrip strip;
    fillOverflowingStrip(strip);
    strip.show();
    QVERIFY(QTest::qWaitForWindowExposed(&strip));
    QCoreApplication::processEvents();

    const int last = strip.tabBar()->count() - 1;
    strip.tabBar()->setCurrentIndex(last);
    const QRect lastInStrip(strip.tabBar()->mapToParent(strip.tabBar()->tabRect(last).topLeft()),
                            strip.tabBar()->tabRect(last).size());
    QVERIFY2(strip.rect().contains(lastInStrip),
             "选中最后一个标签后它必须完整位于视口内");

    strip.tabBar()->setCurrentIndex(0);
    QCOMPARE(strip.scrollOffset(), 0);
}

void SessionTabBarTest::nestedStateChangeIsDeliveredAfterOuterSignal()
{
    SessionTabStrip strip;
    strip.resize(900, 42);
    strip.show();
    QVERIFY(QTest::qWaitForWindowExposed(&strip));

    // 第一次收到"可滚动"时立刻把视口加宽到不再溢出，模拟显示箭头引发的同步布局。
    int depth = 0;
    int maxDepth = 0;
    QList<bool> received;
    connect(&strip,
            &SessionTabStrip::scrollStateChanged,
            &strip,
            [&](const bool scrollable, bool, bool) {
                ++depth;
                maxDepth = std::max(maxDepth, depth);
                received.append(scrollable);
                if (scrollable) {
                    strip.resize(5000, 42);
                }
                --depth;
            });
    received.clear();
    for (int index = 0; index < 12; ++index) {
        strip.tabBar()->addTab(QStringLiteral("终端 %1").arg(index + 1));
    }
    QCoreApplication::processEvents();

    QCOMPARE(maxDepth, 1);
    QVERIFY(!received.isEmpty());
    QCOMPARE(received.last(), false);
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
                           selectedColor);
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
                           QColor(QStringLiteral("#4050ff")));
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

void SessionTabBarTest::connectionStateDotReflectsPerTabStateAndClearsOnRemoval()
{
    SessionTabBar tabBar;
    tabBar.resize(420, 42);
    tabBar.setVisualColors(QColor(QStringLiteral("#aeb6cc")),
                           QColor(QStringLiteral("#f8f9fd")),
                           QColor(QStringLiteral("#353c56")),
                           QColor(QStringLiteral("#424b68")));
    tabBar.addTab(QStringLiteral("会话一"));
    tabBar.addTab(QStringLiteral("会话二"));
    tabBar.setCurrentIndex(0);
    tabBar.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tabBar));

    const QColor connectedColor(QStringLiteral("#2bd981"));
    const QColor failedColor(QStringLiteral("#e5484d"));

    // 状态点画在终端图标右下角：图标矩形是 (tabRect.left()+15, 纵向居中-8, 17, 16)，
    // 点心落在图标的 (right-1, bottom-1)。采样一个 10×10 的小方块覆盖点本身即可，
    // 不会碰到右侧仅悬停时才出现的关闭按钮。
    const auto dotSampleRect = [&tabBar](const int index) {
        const QRect tabRect = tabBar.tabRect(index);
        const int iconRight = tabRect.left() + 15 + 17;
        const int iconBottom = tabRect.center().y() - 8 + 16;
        return QRect(iconRight - 5, iconBottom - 5, 10, 10);
    };

    // 当前标签此前会在图标右上角额外绘制同色绿点。该点只重复表达胶囊选中态，没有连接
    // 语义，因此无论会话是否 Connected，该区域都必须保持无绿色像素。
    const auto removedSelectionDotRect = [&tabBar](const int index) {
        const QRect tabRect = tabBar.tabRect(index);
        const int iconRight = tabRect.left() + 15 + 17;
        const int iconTop = tabRect.top() + (tabRect.height() - 16) / 2;
        return QRect(iconRight - 5, iconTop - 3, 10, 10);
    };

    QPixmap frame = tabBar.grab();
    QVERIFY2(countPixelsNearColor(frame, dotSampleRect(0), connectedColor) == 0,
             "没有调用 setTabConnectionState 时不能凭空出现绿色状态点");
    QVERIFY2(countPixelsNearColor(frame, dotSampleRect(1), failedColor) == 0,
             "没有调用 setTabConnectionState 时不能凭空出现红色状态点");

    tabBar.setTabConnectionState(0, SessionTabBar::ConnectionState::Connected);
    tabBar.setTabConnectionState(1, SessionTabBar::ConnectionState::Failed);
    frame = tabBar.grab();
    QVERIFY2(countPixelsNearColor(frame, dotSampleRect(0), connectedColor) > 4,
             "已连接的标签必须显示绿色状态点");
    QVERIFY2(countPixelsNearColor(frame, removedSelectionDotRect(0), connectedColor) == 0,
             "当前标签右上角不能再显示仅用于表达选中状态的绿色装饰点");
    QVERIFY2(countPixelsNearColor(frame, dotSampleRect(1), failedColor) > 4,
             "连接失败的标签必须显示红色状态点，即便它不是当前选中标签");

    // 关闭标签一（Connected）后在同一索引插入一个全新标签；新标签必须是干净的，不能
    // 顶着旧标签留下的状态——两者的稳定标识不同，state 哈希若忘记清理就会张冠李戴。
    tabBar.removeTab(0);
    tabBar.insertTab(0, QStringLiteral("会话三"));
    frame = tabBar.grab();
    QVERIFY2(countPixelsNearColor(frame, dotSampleRect(0), connectedColor) == 0,
             "新插入的标签不能继承已关闭标签残留的连接状态");
}

QTEST_MAIN(SessionTabBarTest)

#include "SessionTabBarTest.moc"
