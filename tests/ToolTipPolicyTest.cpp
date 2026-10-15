#include "ui/ToolTipPolicy.h"

#include <QtTest>

namespace {

/** @brief 返回 Qt 内部当前的提示标签；不存在时返回空指针。 */
QWidget *toolTipLabel()
{
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QLatin1String("qtooltip_label")) {
            return widget;
        }
    }
    return nullptr;
}

/** @brief 按策略对当前光标与提示尺寸计算期望位置。 */
QPoint expectedPlacement(const QWidget *label)
{
    const QPoint cursor = QCursor::pos();
    const QScreen *screen = QGuiApplication::screenAt(cursor);
    if (screen == nullptr) {
        screen = label->screen();
    }
    return ToolTipPolicy::placement(
        cursor, label->size(), screen == nullptr ? QRect() : screen->availableGeometry());
}

} // namespace

/** @brief 验证悬停提示紧贴光标、靠屏幕边缘时收回，并使用界面正文字号。 */
class ToolTipPolicyTest final : public QObject {
    Q_OBJECT
private slots:
    /** @brief 纯几何：默认在光标右下方，底部或右侧放不下时翻转/收回，不越过屏幕。 */
    void placementStaysNextToCursor()
    {
        const QRect screen(0, 0, 1000, 800);
        const QSize tip(200, 40);

        QCOMPARE(ToolTipPolicy::placement(QPoint(300, 300), tip, screen),
                 QPoint(300, 300) + ToolTipPolicy::CursorOffset);

        // 底部放不下：整块提示翻到光标上方，且不压住热点。
        const QPoint nearBottom = ToolTipPolicy::placement(QPoint(300, 790), tip, screen);
        QCOMPARE(nearBottom.y(), 790 - ToolTipPolicy::AboveCursorGap - tip.height());
        QVERIFY(nearBottom.y() + tip.height() <= 790);

        // 右侧放不下：水平收进屏幕，右边缘贴齐可用区域。
        const QPoint nearRight = ToolTipPolicy::placement(QPoint(950, 300), tip, screen);
        QCOMPARE(nearRight.x() + tip.width() - 1, screen.right());

        // 非零原点的副屏同样以其可用区域为界。
        const QRect secondary(-1200, 100, 1200, 700);
        const QPoint onSecondary = ToolTipPolicy::placement(QPoint(-1199, 150), tip, secondary);
        QVERIFY(secondary.contains(QRect(onSecondary, tip)));
    }

    /**
     * @brief 真实 QToolTip：新建与复用两条路径都落在策略位置，字号与正文一致。
     *
     * Qt 默认按平台光标高度偏移（macOS 上远大于 20px），且使用小号提示字体；
     * 未安装策略时两项断言都会失败。
     */
    void realToolTipFollowsPolicy()
    {
        // 与应用主题一样存在 QToolTip 样式规则，确认样式表不会把字号重置回平台默认。
        qApp->setStyleSheet(QStringLiteral(
            "QToolTip { color: #222; background: #fff; border: 1px solid #ccc; padding: 5px; }"));
        ToolTipPolicy policy(*qApp);

        QWidget host;
        host.resize(300, 200);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));

        QToolTip::showText(QCursor::pos(), QStringLiteral("grokbot@8.148.83.73:50022"), &host);
        QTRY_VERIFY(toolTipLabel() != nullptr && toolTipLabel()->isVisible());
        QWidget *label = toolTipLabel();
        QCOMPARE(label->pos(), expectedPlacement(label));
        QCOMPARE(label->font().pointSizeF(), QApplication::font().pointSizeF());

        // 已显示的提示换文字时 Qt 只 move() 复用的标签，不会再次 Show。
        QToolTip::showText(QCursor::pos(),
                           QStringLiteral("grokbot@8.148.83.73:50022\nSystem: Debian 13"),
                           &host);
        QCOMPARE(toolTipLabel(), label);
        QCOMPARE(label->pos(), expectedPlacement(label));

        QToolTip::hideText();
        qApp->setStyleSheet(QString());
    }
};

QTEST_MAIN(ToolTipPolicyTest)
#include "ToolTipPolicyTest.moc"
