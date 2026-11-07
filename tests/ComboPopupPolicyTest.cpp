#include "ui/ComboPopupPolicy.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QFrame>
#include <QtTest>

/** @brief 验证应用级策略让所有下拉框的弹出层只保留圆角列表，不再有直角外框。 */
class ComboPopupPolicyTest final : public QObject {
    Q_OBJECT
private slots:
    void removesSquareFrameFromEveryComboPopup()
    {
        // 安装前已存在的下拉框也要补齐处理。
        QComboBox existing;
        existing.addItems({QStringLiteral("UTF-8"), QStringLiteral("GBK")});
        ComboPopupPolicy policy(*qApp);
        verifyRoundedPopup(&existing);

        // 安装后动态创建、且没有手动调用 modernizePopup() 的下拉框（包括可编辑的）。
        QComboBox editable;
        editable.setEditable(true);
        editable.addItems({QStringLiteral("UTF-8"), QStringLiteral("Big5"),
                           QStringLiteral("CP437")});
        editable.show();
        QVERIFY(QTest::qWaitForWindowExposed(&editable));
        verifyRoundedPopup(&editable);

        // 真正弹出后仍保持无边框和透明背景。
        editable.showPopup();
        QApplication::processEvents();
        QWidget *container = editable.view()->window();
        QVERIFY(container->isVisible());
        QCOMPARE(container->windowType(), Qt::Popup);
        verifyRoundedPopup(&editable);
        editable.hidePopup();
    }

private:
    static void verifyRoundedPopup(QComboBox *combo)
    {
        QWidget *container = combo->view()->window();
        QVERIFY(container != nullptr && container != combo->view());
        QVERIFY2(container->testAttribute(Qt::WA_TranslucentBackground),
                 "下拉容器背景必须透明，圆角之外不能露出方形底色");
        QVERIFY2(container->windowFlags().testFlag(Qt::FramelessWindowHint),
                 "下拉容器必须禁用系统窗口装饰");
        QVERIFY(container->windowFlags().testFlag(Qt::NoDropShadowWindowHint));
        auto *frame = qobject_cast<QFrame *>(container);
        QVERIFY(frame != nullptr);
        QCOMPARE(frame->frameShape(), QFrame::NoFrame);
    }
};

QTEST_MAIN(ComboPopupPolicyTest)
#include "ComboPopupPolicyTest.moc"
