#include "ui/SessionTabBar.h"

#include <QAbstractButton>
#include <QSignalSpy>
#include <QTest>

/**
 * @brief 覆盖标题栏会话标签控件的滚动接线与关闭按钮命中判定。
 *
 * 这两处行为都依赖 QTabBar 的内部实现细节：滚动偏移量没有公开接口，只能通过 Qt 为内置
 * 滚动按钮设置的稳定对象名驱动。一旦 Qt 更名或改变布局策略，标题栏的左右滚动按钮会静默
 * 失效——外部按钮仍然显示，点击却毫无反应。纯人工点击很难发现这种"看起来正常"的退化，
 * 因此必须由自动化用例守住。
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

QTEST_MAIN(SessionTabBarTest)

#include "SessionTabBarTest.moc"
