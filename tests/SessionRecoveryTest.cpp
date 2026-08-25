#include "core/ConnectionTarget.h"
#include "terminal/TerminalPane.h"
#include "terminal/TerminalView.h"

#include <QSignalSpy>
#include <QTest>

/**
 * @brief 覆盖 SSH 异常断线后的"手动恢复提示"这一终端本地状态。
 *
 * 用例只驱动 TerminalPane 的本地分支，不发起任何网络连接：断线恢复的关键行为——不再擅自
 * 启动本地 zsh、终端只接受 r 与 x——完全发生在没有任何 PTY 存活的时刻，正是最容易在重构
 * 中被误接回 openLocalShell() 的一段。
 */
class SessionRecoveryTest final : public QObject {
    Q_OBJECT

private slots:
    void promptOffersManualChoiceInsteadOfLocalShell();
    void closeKeyRequestsTabClose();
    void otherKeysKeepThePrompt();
    void reconnectKeyWithoutTargetKeepsThePrompt();

private:
    /** @brief 构造一个不会被真正连接的示例目标，仅用于提示文案和状态位。 */
    [[nodiscard]] static ConnectionTarget sampleTarget();
};

ConnectionTarget SessionRecoveryTest::sampleTarget()
{
    ConnectionTarget target;
    target.id = QStringLiteral("recovery-test-host");
    target.host = QStringLiteral("198.51.100.7");
    target.port = 22;
    target.username = QStringLiteral("root");
    return target;
}

void SessionRecoveryTest::promptOffersManualChoiceInsteadOfLocalShell()
{
    TerminalPane pane;
    pane.beginManualRecovery(sampleTarget(), QStringLiteral("自动重连已达到 3 次上限"));

    QVERIFY2(pane.isAwaitingManualRecovery(),
             "重连耗尽后必须停在等待用户决定的状态");
    const QString screen = pane.terminalPlainText();
    QVERIFY2(screen.contains(QStringLiteral("自动重连已达到 3 次上限")),
             "提示必须复述断线原因，用户才知道自己在为什么做选择");
    QVERIFY2(screen.contains(QStringLiteral("按 r 重新连接")),
             "提示必须明确给出手动重连按键");
    QVERIFY2(screen.contains(QStringLiteral("按 x 关闭")),
             "提示必须明确给出关闭标签页按键");
    // 这是本用例真正防守的回归：断线后擅自启动的本地 zsh 与刚断开的服务器毫无关系，
    // 却会顶掉现场，用户下一条命令很可能打在本机上。
    QVERIFY2(!screen.contains(QStringLiteral("正在启动本地 zsh")),
             "断线恢复提示期间不得启动本地终端");
    QVERIFY2(!pane.isConnected(), "提示状态下没有任何可交互的会话");
}

void SessionRecoveryTest::closeKeyRequestsTabClose()
{
    TerminalPane pane;
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));
    pane.beginManualRecovery(sampleTarget(), QStringLiteral("SSH 连接因网络异常中断"));

    auto *view = pane.findChild<TerminalView *>();
    QVERIFY2(view != nullptr, "终端面板必须持有可接收按键的视图");
    QSignalSpy closeSpy(&pane, &TerminalPane::closeRequested);
    QSignalSpy broadcastSpy(&pane, &TerminalPane::broadcastSourceInput);

    QTest::keyClick(view, Qt::Key_X);

    QCOMPARE(closeSpy.count(), 1);
    QVERIFY2(!pane.isAwaitingManualRecovery(), "做出选择后必须离开提示状态");
    // 提示期间的按键是本地抉择，不是会话输入；广播出去会让一次"重连还是关闭"变成对其他
    // 服务器的误操作。
    QCOMPARE(broadcastSpy.count(), 0);
}

void SessionRecoveryTest::otherKeysKeepThePrompt()
{
    TerminalPane pane;
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));
    pane.beginManualRecovery(sampleTarget(), QStringLiteral("SSH 连接因网络异常中断"));

    auto *view = pane.findChild<TerminalView *>();
    QVERIFY(view != nullptr);
    QSignalSpy closeSpy(&pane, &TerminalPane::closeRequested);

    QTest::keyClick(view, Qt::Key_A);
    QTest::keyClick(view, Qt::Key_Return);

    QCOMPARE(closeSpy.count(), 0);
    QVERIFY2(pane.isAwaitingManualRecovery(),
             "无关按键既不能关闭标签页，也不能把用户悄悄带出提示状态");
}

void SessionRecoveryTest::reconnectKeyWithoutTargetKeepsThePrompt()
{
    TerminalPane pane;
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));
    // 目标无效意味着没有任何可复用的连接计划：这是"面板刚建好就被要求恢复"的退化情形，
    // 用例只验证按 r 不会静默失灵，因此全程不会发起任何网络连接。
    pane.beginManualRecovery(ConnectionTarget{}, QStringLiteral("SSH 连接因网络异常中断"));

    auto *view = pane.findChild<TerminalView *>();
    QVERIFY(view != nullptr);
    QTest::keyClick(view, Qt::Key_R);

    QVERIFY2(pane.isAwaitingManualRecovery(), "无法重连时必须继续停在提示上");
    QVERIFY2(pane.terminalPlainText().contains(QStringLiteral("没有可重连的服务器")),
             "按键必须有可见反馈，否则用户会以为终端已经卡死");
}

QTEST_MAIN(SessionRecoveryTest)

#include "SessionRecoveryTest.moc"
