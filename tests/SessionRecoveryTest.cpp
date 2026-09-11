#include "core/ConnectionTarget.h"
#include "terminal/TerminalPane.h"
#include "terminal/TerminalView.h"

#include <QSignalSpy>
#include <QTest>
#include <QTextCursor>

/**
 * @brief 覆盖 SSH 异常断线后的"手动恢复提示"这一终端本地状态。
 *
 * 用例只驱动 TerminalPane 的本地分支，不发起任何网络连接：断线恢复的关键行为——不再擅自
 * 启动本地 Shell、终端只接受 r 与 x——完全发生在没有任何 PTY/ConPTY 存活的时刻，正是最容易在重构
 * 中被误接回 openLocalShell() 的一段。
 */
class SessionRecoveryTest final : public QObject {
    Q_OBJECT

private slots:
    /** @brief 二进制留下黑色隐藏画笔和未结束 OSC 后，断线提示仍必须可见。 */
    void recoveryClearsPoisonedTerminalState();
    /** @brief 分别覆盖纯颜色污染以及会吞掉文本的未结束控制字符串。 */
    void recoveryClearsPoisonedTerminalState_data();
    /** @brief `tail` 一类命令退出、Shell 即将重画提示符时，污染必须自动清除。 */
    void promptAfterCorruptingCommandAutoResetsTerminal();
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

void SessionRecoveryTest::recoveryClearsPoisonedTerminalState_data()
{
    QTest::addColumn<QByteArray>("suffix");
    QTest::newRow("black-concealed") << QByteArray();
    QTest::newRow("unfinished-osc") << QByteArrayLiteral("\x1b]0;unfinished");
    QTest::newRow("unfinished-dcs") << QByteArrayLiteral("\x1bPunfinished");
    QTest::newRow("unfinished-csi") << QByteArrayLiteral("\x1b[38;2;");
}

void SessionRecoveryTest::recoveryClearsPoisonedTerminalState()
{
    QFETCH(QByteArray, suffix);
    TerminalPane pane;
    auto *view = pane.findChild<TerminalView *>();
    QVERIFY(view != nullptr);
    // 用确定性的控制序列模拟二进制污染，避免依赖私有 JAR 或随机字节碰巧触发属性。
    view->appendTerminalOutput(QByteArrayLiteral("\x1b[30;40;8mhidden") + suffix);
    pane.beginManualRecovery(sampleTarget(), QStringLiteral("recovery-marker"));
    const QString text = pane.terminalPlainText();
    QVERIFY2(text.contains(QStringLiteral("recovery-marker")), "未结束 OSC 不得吞掉恢复提示");
    // 仅断言文字存在不够：隐藏属性会让文档有文字、屏幕却是黑色。
    const auto cursor = view->document()->find(QStringLiteral("recovery-marker"));
    QVERIFY(!cursor.isNull());
    QVERIFY(cursor.charFormat().foreground().color() != cursor.charFormat().background().color());
}

void SessionRecoveryTest::promptAfterCorruptingCommandAutoResetsTerminal()
{
    TerminalPane pane;
    auto *view = pane.findChild<TerminalView *>();
    QVERIFY(view != nullptr);

    // 模拟 `tail hotline-operation-server.jar0`：残留一块隐藏黑底文字，随后是 ZIP 中央
    // 目录里凑出的残缺 OSC，紧跟一个换行——这正是让 VtermBridge 识别为二进制噪音并强制
    // 中止的路径，同时也是用户报告里"选中才看得见文字"的黑屏现场。
    view->appendTerminalOutput(
        QByteArrayLiteral("\x1b[30;40;8mhidden\x1b]org/bouncycastle/Utils.class\r\n"));
    QVERIFY2(view->hasPendingCorruption(),
             "未收尾字符串被强制中止后必须留下待处理标记");

    // Shell 集成的 cwd 上报钩子挂在 PROMPT_COMMAND/precmd 上：`tail` 一退出、即将重画
    // 提示符前就会发出这一条 OSC 6773，不需要用户知道有 Ctrl/Cmd+Shift+R 这回事。
    view->appendTerminalOutput(QByteArrayLiteral("\x1b]6773;cwd;/root\x07"));

    QVERIFY2(!view->hasPendingCorruption(), "命令退出后必须自动清除污染标记");
    view->appendTerminalOutput(QByteArrayLiteral("[root@vm ~]# "));
    QTest::qWait(50);
    const auto cursor = view->document()->find(QStringLiteral("[root@vm ~]# "));
    QVERIFY(!cursor.isNull());
    QVERIFY2(cursor.charFormat().foreground().color() != cursor.charFormat().background().color(),
             "自动复位必须真正清掉隐藏画笔，新提示符不能继承黑底黑字");

    // 正常命令绝不能触发这条自动复位：没发生过污染时收到 cwd 上报只应更新工作目录。
    TerminalPane cleanPane;
    auto *cleanView = cleanPane.findChild<TerminalView *>();
    QSignalSpy cwdSpy(&cleanPane, &TerminalPane::remoteWorkingDirectoryChanged);
    cleanView->appendTerminalOutput(QByteArrayLiteral("ls\r\n"));
    cleanView->appendTerminalOutput(QByteArrayLiteral("\x1b]6773;cwd;/root\x07"));
    QCOMPARE(cwdSpy.count(), 1);
    QVERIFY(!cleanView->hasPendingCorruption());
}

void SessionRecoveryTest::promptOffersManualChoiceInsteadOfLocalShell()
{
    TerminalPane pane;
    pane.beginManualRecovery(sampleTarget(), QStringLiteral("Automatic reconnect reached its limit of 3 attempts"));

    QVERIFY2(pane.isAwaitingManualRecovery(),
             "重连耗尽后必须停在等待用户决定的状态");
    const QString screen = pane.terminalPlainText();
    QVERIFY2(screen.contains(QStringLiteral("Automatic reconnect reached its limit of 3 attempts")),
             "提示必须复述断线原因，用户才知道自己在为什么做选择");
    QVERIFY2(screen.contains(QStringLiteral("Press r to reconnect")),
             "提示必须明确给出手动重连按键");
    QVERIFY2(screen.contains(QStringLiteral("press x to close this tab")),
             "提示必须明确给出关闭标签页按键");
    // 这是本用例真正防守的回归：断线后擅自启动的本地 Shell 与刚断开的服务器毫无关系，
    // 却会顶掉现场，用户下一条命令很可能打在本机上。
    QVERIFY2(!screen.contains(QStringLiteral("Starting local terminal")),
             "断线恢复提示期间不得启动本地终端");
    QVERIFY2(!pane.isConnected(), "提示状态下没有任何可交互的会话");
}

void SessionRecoveryTest::closeKeyRequestsTabClose()
{
    TerminalPane pane;
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));
    pane.beginManualRecovery(sampleTarget(), QStringLiteral("The SSH connection was interrupted by a network error"));

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
    pane.beginManualRecovery(sampleTarget(), QStringLiteral("The SSH connection was interrupted by a network error"));

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
    pane.beginManualRecovery(ConnectionTarget{}, QStringLiteral("The SSH connection was interrupted by a network error"));

    auto *view = pane.findChild<TerminalView *>();
    QVERIFY(view != nullptr);
    QTest::keyClick(view, Qt::Key_R);

    QVERIFY2(pane.isAwaitingManualRecovery(), "无法重连时必须继续停在提示上");
    QVERIFY2(pane.terminalPlainText().contains(QStringLiteral("No server to reconnect to")),
             "按键必须有可见反馈，否则用户会以为终端已经卡死");
}

QTEST_MAIN(SessionRecoveryTest)

#include "SessionRecoveryTest.moc"
