#include "ui/ScrollBarPolicy.h"
#include <QScrollArea>
#include <QtTest>

/** @brief 验证全局滚动条的输入触发、空闲隐藏和布局稳定性。 */
class ScrollBarPolicyTest final : public QObject {
    Q_OBJECT
private slots:
    /** @brief 自动定位保持隐藏；手动滚动显示并在空闲后隐藏，期间视口尺寸不变。 */
    void manualScrollLifecycle()
    {
        ScrollBarPolicy policy(*qApp);
        QScrollArea area;
        auto *content = new QWidget;
        content->resize(1200, 1600);
        area.setWidget(content);
        area.resize(300, 300);
        area.show();
        QTest::qWait(30);
        auto *bar = area.verticalScrollBar();
        const QSize viewportSize = area.viewport()->size(); // 显隐不应导致终端重排或表格抖动。
        bar->setValue(200);
        QVERIFY(!bar->property("shelliumScrollActive").toBool());
        QWheelEvent wheel(QPointF(20, 20), QPointF(20, 20), QPoint(), QPoint(0, -120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(area.viewport(), &wheel);
        QVERIFY(bar->property("shelliumScrollActive").toBool());
        QVERIFY(bar->value() > 200);
        QCOMPARE(area.viewport()->size(), viewportSize);
        QTRY_VERIFY_WITH_TIMEOUT(!bar->property("shelliumScrollActive").toBool(), 1600);
        QCOMPARE(area.viewport()->size(), viewportSize);
        // 水平滚动条同样被统一策略覆盖，拖动期间不能因计时到期而消失。
        auto *horizontal = area.horizontalScrollBar();
        horizontal->setSliderDown(true);
        QTest::mousePress(horizontal, Qt::LeftButton);
        QVERIFY(horizontal->property("shelliumScrollActive").toBool());
        QTest::qWait(1100);
        QVERIFY(horizontal->property("shelliumScrollActive").toBool());
        horizontal->setSliderDown(false);
        QTest::mouseRelease(horizontal, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!horizontal->property("shelliumScrollActive").toBool(), 1600);
    }
};

// Qt 测试入口建立 QApplication，确保绘制与输入事件在 GUI 线程执行。
QTEST_MAIN(ScrollBarPolicyTest)
#include "ScrollBarPolicyTest.moc"
