#include "ui/ButtonCursorPolicy.h"

#include <QDialogButtonBox>
#include <QEnterEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>
#include <QtTest>

/** @brief 验证按钮策略覆盖已有、动态及内嵌按钮，同时保留非按钮交互与点击事件。 */
class ButtonCursorTest final : public QObject {
    Q_OBJECT
private slots:
    /** @brief 无参数、无返回值；通过 QtTest 断言报告光标或事件分发异常。 */
    void appliesToAllButtons()
    {
        // 先创建普通按钮，验证安装时也会补齐已有控件，而非仅处理后续显示事件。
        QPushButton existingButton;
        ButtonCursorPolicy policy(*qApp);
        QCOMPARE(existingButton.cursor().shape(), Qt::PointingHandCursor);

        // 图标按钮属于另一种具体类型，动态构造和样式初始化后应获得同一策略。
        QToolButton iconButton;
        iconButton.setIcon(iconButton.style()->standardIcon(QStyle::SP_DialogCloseButton));
        iconButton.ensurePolished();
        QCOMPARE(iconButton.cursor().shape(), Qt::PointingHandCursor);

        QDialogButtonBox dialogButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        for (QAbstractButton *button : dialogButtons.buttons()) {
            button->ensurePolished();
            QCOMPARE(button->cursor().shape(), Qt::PointingHandCursor);
        }

        // 模拟业务代码覆盖光标后再次悬停，验证 Enter 分支能恢复统一反馈。
        iconButton.setCursor(Qt::ArrowCursor);
        QEnterEvent enterEvent(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
        QApplication::sendEvent(&iconButton, &enterEvent);
        QCOMPARE(iconButton.cursor().shape(), Qt::PointingHandCursor);

        // 非按钮输入区必须继续显示文本光标；策略不能扩散到所有 QWidget。
        QLineEdit editor;
        editor.setCursor(Qt::IBeamCursor);
        editor.ensurePolished();
        QApplication::sendEvent(&editor, &enterEvent);
        QCOMPARE(editor.cursor().shape(), Qt::IBeamCursor);

        // 点击信号验证过滤器没有吞掉事件，禁用后的点击仍按 Qt 原有规则被忽略。
        QSignalSpy clicks(&existingButton, &QPushButton::clicked);
        QTest::mouseClick(&existingButton, Qt::LeftButton);
        QCOMPARE(clicks.count(), 1);
        existingButton.setEnabled(false);
        QTest::mouseClick(&existingButton, Qt::LeftButton);
        QCOMPARE(clicks.count(), 1);
    }
};

// QtTest 创建 QApplication，确保测试走真实 QWidget 事件处理路径。
QTEST_MAIN(ButtonCursorTest)
#include "ButtonCursorTest.moc"
