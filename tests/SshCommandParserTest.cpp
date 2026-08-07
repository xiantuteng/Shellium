#include "core/ConnectionTarget.h"
#include "ssh/ShellShim.h"
#include "ssh/SshCommandParser.h"
#include "terminal/TerminalView.h"
#include "terminal/LocalShellWorker.h"
#include "security/Vault.h"
#include "storage/ApplicationDatabase.h"

#include <QSignalSpy>
#include <QInputMethodEvent>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

#include <libssh/libssh.h>

/**
 * @brief 验证嵌套 SSH 命令解析和连接链安全边界。
 *
 * 这些测试不访问网络，只验证用户输入如何转换成文件管理器后续使用的独立连接目标。
 */
class SshCommandParserTest final : public QObject {
    Q_OBJECT

private slots:
    /** @brief 普通 Shell 命令不应改变当前服务器链。 */
    void ignoresOrdinaryCommands();

    /** @brief 最常见的 user@host 与端口选项应生成经过当前服务器的嵌套目标。 */
    void parsesNestedTargetAndInheritsOuterRoute();

    /** @brief 嵌套命令显式 -J 时，额外跳板必须追加而不能丢失外层服务器。 */
    void appendsExplicitJumpAfterCurrentServer();

    /** @brief exit、logout 和 Ctrl-D 都应离开一层嵌套服务器。 */
    void recognizesLeaveCommands_data();
    void recognizesLeaveCommands();

    /** @brief 不同跳板环境中的同名主机必须得到不同信任键。 */
    void stableKeyIncludesJumpRoute();

    /** @brief 当前 libssh 构建必须实际包含并接受 hmac-sha1 兼容配置。 */
    void libsshSupportsOptInLegacyHmac();

    /** @brief 普通字符与回车必须从终端视图完整发送，并产生可解析的整行命令。 */
    void terminalViewForwardsKeyboardInput();

    /** @brief Tab 必须发送给 Shell 做补全，不能把焦点切换到其他控件。 */
    void terminalViewForwardsCompletionKeys();

    /** @brief 物理 Ctrl+C 必须编码为 ETX，而不是被 Qt 当作复制快捷键吞掉。 */
    void terminalViewForwardsInterruptKey();

    /** @brief macOS 输入法提交的字符必须发送到远端而不能被只读文本控件吞掉。 */
    void terminalViewForwardsInputMethodText();

    /** @brief 分块到达的标准 SSH password 提示必须被识别且不泄露任何输入。 */
    void terminalViewDetectsSplitAuthenticationPrompt();

    /** @brief 最终 Permission denied 必须与仍可重试的密码错误提示区分。 */
    /** @brief 不进入交互式 Shell 的 ssh 用法不得压栈。 */
    void ignoresNonInteractiveSshInvocations();

    /** @brief 带退出码的 exit 同样是退出。 */
    void treatsExitWithStatusAsLeave();

    /** @brief VtermBridge 必须把私有 OSC 交出来，而不是渲染成屏幕乱码。 */
    void terminalViewDeliversShellIntegrationEvent();

    /** @brief shim 上报的 argv 要能还原成可直接建立的目标。 */
    void shellShimResolvesInteractiveTarget();

    /** @brief 无法忠实复现的 ssh 用法必须拒绝接管，交还给原生 ssh。 */
    void shellShimDeclinesUnsupportedInvocations();

    void terminalViewDetectsFinalAuthenticationFailure();

    /** @brief 解析失败、连接被拒、超时等非认证失败同样必须触发回滚。 */
    void terminalViewDetectsNonAuthenticationSshFailures();

    /** @brief Ctrl+C 需要如实上报，供 TerminalPane 撤销未确认的嵌套目标。 */
    void terminalViewReportsCtrlCAsNestedSshAbort();

    /** @brief 行中的 Ctrl-D 不是 EOF，不得被当作退出服务器。 */
    void terminalViewIgnoresCtrlDInsideCommandLine();

    /** @brief 本地键盘缓冲缺失时必须能从 PTY 当前光标行恢复嵌套 ssh 命令。 */
    void terminalViewRecoversSshCommandFromCursorLine();

    /** @brief 内层 Shell 提示符必须在渲染完成后被识别，用于延迟开放第 2 层文件管理。 */
    void terminalViewDetectsRenderedShellPrompt();

    /** @brief 光标下方残留的旧提示符不能把正在等待密码的目标误判为认证成功。 */
    void terminalViewIgnoresStalePromptBelowCursor();

    /** @brief PTY 的 CRLF 必须形成新行，不能让 Shell 提示符横向堆叠。 */
    void terminalViewRendersCarriageReturnAndLineFeed();

    /** @brief 本地 zsh 必须在真实 PTY 中接收命令、返回输出并正常退出。 */
    void localZshExecutesInteractiveCommand();

    /** @brief ETX 必须通过 PTY 的 VINTR 终止本地 zsh 前台命令。 */
    void localZshInterruptsForegroundCommand();

    /** @brief SQLite 初始化必须创建全部核心功能表并开启外键约束。 */
    void databaseCreatesCoreSchema();

    /** @brief 保险库应正确往返加密，并拒绝被篡改的密文。 */
    void vaultEncryptsAndAuthenticatesCredentials();
};

namespace {

/** @brief 创建测试使用的外层服务器，集中维护默认字段。 */
ConnectionTarget outerTarget()
{
    ConnectionTarget target;
    target.id = QStringLiteral("outer-id");
    target.displayName = QStringLiteral("生产跳板机");
    target.host = QStringLiteral("gateway.example.com");
    target.username = QStringLiteral("operator");
    target.port = 2222;
    target.proxyJumps = {QStringLiteral("edge@example.com:22")};
    target.allowLegacyHmacSha1 = true;
    return target;
}

} // namespace

void SshCommandParserTest::ignoresOrdinaryCommands()
{
    const SshCommandParser::Result result =
        SshCommandParser::parse(QStringLiteral("printf 'ssh fake@example.com'"), outerTarget());
    QCOMPARE(result.action, SshCommandParser::Action::None);
    QVERIFY(!result.target.isValid());
}

void SshCommandParserTest::parsesNestedTargetAndInheritsOuterRoute()
{
    const SshCommandParser::Result result = SshCommandParser::parse(
        QStringLiteral("ssh -p 2200 deploy@10.20.0.15"), outerTarget());

    QCOMPARE(result.action, SshCommandParser::Action::EnterServer);
    QCOMPARE(result.target.host, QStringLiteral("10.20.0.15"));
    QCOMPARE(result.target.username, QStringLiteral("deploy"));
    QCOMPARE(result.target.port, quint16(2200));
    QCOMPARE(result.target.parentId, QStringLiteral("outer-id"));
    QCOMPARE(result.target.allowLegacyHmacSha1, outerTarget().allowLegacyHmacSha1);
    QCOMPARE(result.target.proxyJumps,
             QStringList({QStringLiteral("edge@example.com:22"),
                          QStringLiteral("operator@gateway.example.com:2222")}));
}

void SshCommandParserTest::appendsExplicitJumpAfterCurrentServer()
{
    const SshCommandParser::Result result = SshCommandParser::parse(
        QStringLiteral("command ssh -J relay@inside-gateway root@database.internal"),
        outerTarget());

    QCOMPARE(result.action, SshCommandParser::Action::EnterServer);
    QCOMPARE(result.target.proxyJumps,
             QStringList({QStringLiteral("edge@example.com:22"),
                          QStringLiteral("operator@gateway.example.com:2222"),
                          QStringLiteral("relay@inside-gateway")}));
}

void SshCommandParserTest::recognizesLeaveCommands_data()
{
    QTest::addColumn<QString>("command");
    QTest::newRow("exit") << QStringLiteral("exit");
    QTest::newRow("logout") << QStringLiteral("logout");
    QTest::newRow("ctrl-d") << QStringLiteral("Ctrl-D");
}

void SshCommandParserTest::recognizesLeaveCommands()
{
    QFETCH(QString, command);
    const SshCommandParser::Result result = SshCommandParser::parse(command, outerTarget());
    QCOMPARE(result.action, SshCommandParser::Action::LeaveServer);
}

void SshCommandParserTest::stableKeyIncludesJumpRoute()
{
    ConnectionTarget first;
    first.host = QStringLiteral("db.internal");
    first.username = QStringLiteral("root");
    first.proxyJumps = {QStringLiteral("jump-a.example.com")};

    ConnectionTarget second = first;
    second.proxyJumps = {QStringLiteral("jump-b.example.com")};

    QVERIFY(first.stableKey() != second.stableKey());
}

void SshCommandParserTest::libsshSupportsOptInLegacyHmac()
{
    const QString supported = QString::fromUtf8(ssh_get_supported_methods(SSH_MAC_C_S));
    QVERIFY2(supported.split(QLatin1Char(',')).contains(QStringLiteral("hmac-sha1")),
             qPrintable(QStringLiteral("当前 libssh 不支持 hmac-sha1，实际算法列表：%1")
                            .arg(supported)));

    ssh_session session = ssh_new();
    QVERIFY(session != nullptr);
    constexpr char legacyHmac[] = "hmac-sha1";
    QCOMPARE(ssh_options_set(session, SSH_OPTIONS_HMAC_C_S, legacyHmac), SSH_OK);
    QCOMPARE(ssh_options_set(session, SSH_OPTIONS_HMAC_S_C, legacyHmac), SSH_OK);
    ssh_free(session);
}

void SshCommandParserTest::terminalViewForwardsKeyboardInput()
{
    TerminalView view;
    view.show();
    view.setFocus();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    QSignalSpy byteSpy(&view, &TerminalView::inputBytes);
    QSignalSpy commandSpy(&view, &TerminalView::commandSubmitted);
    QStringList returnSignalOrder;
    connect(&view, &TerminalView::inputBytes, &view,
            [&returnSignalOrder](const QByteArray &bytes) {
                if (bytes == QByteArrayLiteral("\r")) {
                    returnSignalOrder.append(QStringLiteral("input"));
                }
            });
    connect(&view, &TerminalView::commandSubmitted, &view,
            [&returnSignalOrder](const QString &) {
                returnSignalOrder.append(QStringLiteral("command"));
            });
    QTest::keyClicks(&view, QStringLiteral("pwd"));
    QTest::keyClick(&view, Qt::Key_Return);

    QByteArray transmitted;
    for (const QList<QVariant> &arguments : byteSpy) {
        transmitted.append(arguments.at(0).toByteArray());
    }
    QCOMPARE(transmitted, QByteArrayLiteral("pwd\r"));
    QCOMPARE(commandSpy.size(), 1);
    QCOMPARE(commandSpy.at(0).at(0).toString(), QStringLiteral("pwd"));
    // PTY 必须先取得回车，随后上层才能根据完整输入启动嵌套 SFTP 等派生动作。
    QCOMPARE(returnSignalOrder,
             QStringList({QStringLiteral("input"), QStringLiteral("command")}));
}

void SshCommandParserTest::terminalViewForwardsCompletionKeys()
{
    TerminalView view;
    view.show();
    view.setFocus();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QSignalSpy byteSpy(&view, &TerminalView::inputBytes);

    QTest::keyClick(&view, Qt::Key_Tab);
    QTest::keyClick(&view, Qt::Key_Backtab, Qt::ShiftModifier);

    QByteArray transmitted;
    for (const QList<QVariant> &arguments : byteSpy) {
        transmitted.append(arguments.at(0).toByteArray());
    }
    QCOMPARE(transmitted, QByteArrayLiteral("\t\x1b[Z"));
    QVERIFY(view.hasFocus());
}

void SshCommandParserTest::terminalViewForwardsInterruptKey()
{
    TerminalView view;
    view.show();
    view.setFocus();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QSignalSpy byteSpy(&view, &TerminalView::inputBytes);

#if defined(Q_OS_MACOS)
    // Qt/macOS 将物理 Control 表示为 MetaModifier，Command 才是 ControlModifier。
    QTest::keyClick(&view, Qt::Key_C, Qt::MetaModifier);
#else
    QTest::keyClick(&view, Qt::Key_C, Qt::ControlModifier);
#endif

    QCOMPARE(byteSpy.size(), 1);
    QCOMPARE(byteSpy.at(0).at(0).toByteArray(), QByteArray(1, '\x03'));
}

void SshCommandParserTest::terminalViewForwardsInputMethodText()
{
    TerminalView view;
    QSignalSpy byteSpy(&view, &TerminalView::inputBytes);

    QInputMethodEvent inputEvent;
    inputEvent.setCommitString(QStringLiteral("ls -la"));
    QApplication::sendEvent(&view, &inputEvent);

    QCOMPARE(byteSpy.size(), 1);
    QCOMPARE(byteSpy.at(0).at(0).toByteArray(), QByteArrayLiteral("ls -la"));
}

void SshCommandParserTest::terminalViewDetectsSplitAuthenticationPrompt()
{
    TerminalView view;
    QSignalSpy promptSpy(&view, &TerminalView::authenticationPromptDetected);
    view.appendTerminalOutput(QByteArrayLiteral("root@10.0.0.2's pass"));
    QCOMPARE(promptSpy.size(), 0);
    view.appendTerminalOutput(QByteArrayLiteral("word:"));
    QCOMPARE(promptSpy.size(), 1);

    // 同一提示的后续绘制更新不得重复触发，避免把下一条普通命令误当成第二份密码。
    view.appendTerminalOutput(QByteArrayLiteral(" "));
    QCOMPARE(promptSpy.size(), 1);

    // 第一次密码提交后，OpenSSH 可能因密码错误重新显示认证提示。新提示必须再次触发，
    // TerminalPane 才能用第二次输入覆盖错误缓存，而不是永远保留第一次的错误密码。
    QTest::keyClick(&view, Qt::Key_Return);
    view.appendTerminalOutput(QByteArrayLiteral("Permission denied, please try again.\r\n"));
    view.appendTerminalOutput(QByteArrayLiteral("root@10.0.0.2's password:"));
    QCOMPARE(promptSpy.size(), 2);
}

void SshCommandParserTest::ignoresNonInteractiveSshInvocations()
{
    ConnectionTarget current;
    current.id = QStringLiteral("layer-1");
    current.host = QStringLiteral("10.0.0.1");
    current.username = QStringLiteral("root");

    // 这些调用执行完就返回外层 Shell，用户从未"身处"目标机器，也永远不会为它敲 exit。
    // 压栈会制造不可撤销的幽灵层，之后文件管理器就会指向一台错误的服务器。
    const QStringList nonInteractive = {
        QStringLiteral("ssh web01 uptime"),
        QStringLiteral("ssh root@web01 'systemctl restart nginx'"),
        QStringLiteral("ssh -N -L 8080:localhost:80 jump01"),
        QStringLiteral("ssh -f -N jump01"),
        QStringLiteral("ssh -T git@github.com"),
        QStringLiteral("ssh -W inner:22 jump01"),
    };
    for (const QString &command : nonInteractive) {
        const SshCommandParser::Result parsed = SshCommandParser::parse(command, current);
        QVERIFY2(parsed.action == SshCommandParser::Action::None, qPrintable(command));
    }

    // 真正的交互式登录仍必须被识别，否则降级路径就完全失效了。
    const SshCommandParser::Result interactive =
        SshCommandParser::parse(QStringLiteral("ssh root@10.0.0.2"), current);
    QCOMPARE(interactive.action, SshCommandParser::Action::EnterServer);
    QCOMPARE(interactive.target.host, QStringLiteral("10.0.0.2"));
}

void SshCommandParserTest::treatsExitWithStatusAsLeave()
{
    ConnectionTarget current;
    current.id = QStringLiteral("layer-2");
    current.host = QStringLiteral("10.0.0.2");

    // 只认裸 exit 会让链只增不减，长会话必然偏离真实状态。
    for (const QString &command : {QStringLiteral("exit"),
                                   QStringLiteral("exit 0"),
                                   QStringLiteral("exit 130"),
                                   QStringLiteral("logout")}) {
        const SshCommandParser::Result parsed = SshCommandParser::parse(command, current);
        QVERIFY2(parsed.action == SshCommandParser::Action::LeaveServer, qPrintable(command));
    }

    // 形似但并非退出的命令不得触发弹栈。
    for (const QString &command : {QStringLiteral("exit_handler"),
                                   QStringLiteral("echo exit")}) {
        const SshCommandParser::Result parsed = SshCommandParser::parse(command, current);
        QVERIFY2(parsed.action == SshCommandParser::Action::None, qPrintable(command));
    }
}

void SshCommandParserTest::terminalViewDeliversShellIntegrationEvent()
{
    TerminalView view;
    QSignalSpy eventSpy(&view, &TerminalView::shellIntegrationEvent);

    // 私有 OSC 编号 libvterm 自己不认识，必须靠未识别序列兜底回调交出来；漏接的话整条
    // 序列会被当作普通文本打印到用户屏幕上。
    // argv 以 base64 传输：0x1E 是 C0 控制字符，直接放进 OSC 会被 libvterm 当作字符串
    // 终止符，只能收到第一个参数——这个坑正是本用例存在的理由。
    view.appendTerminalOutput(QByteArrayLiteral(
        "\033]6773;connect;cm9vdEAxMC4wLjAuMh4=\007"));
    QCOMPARE(eventSpy.size(), 1);

    const ShellShim::Event event =
        ShellShim::parseEvent(eventSpy.at(0).at(0).toByteArray());
    QCOMPARE(event.kind, ShellShim::Event::Kind::Connect);
    QCOMPARE(event.arguments, QStringList{QStringLiteral("root@10.0.0.2")});

    // 被网络拆成多个分片的载荷必须累积后再解析，否则会得到半截主机名。
    QSignalSpy splitSpy(&view, &TerminalView::shellIntegrationEvent);
    view.appendTerminalOutput(QByteArrayLiteral("\033]6773;conn"));
    view.appendTerminalOutput(QByteArrayLiteral("ect;LXAeMjIyMh53ZWIwMR4=\007"));
    QCOMPARE(splitSpy.size(), 1);
    const ShellShim::Event split =
        ShellShim::parseEvent(splitSpy.at(0).at(0).toByteArray());
    QCOMPARE(split.arguments,
             (QStringList{QStringLiteral("-p"), QStringLiteral("2222"),
                          QStringLiteral("web01")}));
}

void SshCommandParserTest::shellShimResolvesInteractiveTarget()
{
    ConnectionTarget current;
    current.id = QStringLiteral("layer-1");
    current.host = QStringLiteral("10.0.0.1");
    current.username = QStringLiteral("root");

    QString reason;
    const ConnectionTarget simple = ShellShim::resolveTarget(
        {QStringLiteral("deploy@10.0.0.2")}, current, &reason);
    QVERIFY(simple.isValid());
    QCOMPARE(simple.host, QStringLiteral("10.0.0.2"));
    QCOMPARE(simple.username, QStringLiteral("deploy"));
    QCOMPARE(simple.port, quint16(22));
    // 由 Shellium 亲自建立，身份是确定事实。
    QCOMPARE(simple.confidence, ConnectionTarget::Confidence::Confirmed);

    // 未写用户名时继承当前层，这与 OpenSSH 的默认行为一致。
    const ConnectionTarget inherited = ShellShim::resolveTarget(
        {QStringLiteral("-p"), QStringLiteral("2222"), QStringLiteral("10.0.0.3")},
        current, &reason);
    QVERIFY(inherited.isValid());
    QCOMPARE(inherited.username, QStringLiteral("root"));
    QCOMPARE(inherited.port, quint16(2222));
}

void SshCommandParserTest::shellShimDeclinesUnsupportedInvocations()
{
    ConnectionTarget current;
    current.id = QStringLiteral("layer-1");
    current.host = QStringLiteral("10.0.0.1");
    current.username = QStringLiteral("root");

    // 宁可退回原生 ssh，也不能用一个错误的目标去建立会话。
    const QList<QStringList> unsupported = {
        {QStringLiteral("web01"), QStringLiteral("uptime")},
        {QStringLiteral("-N"), QStringLiteral("web01")},
        {QStringLiteral("-J"), QStringLiteral("jump01"), QStringLiteral("web01")},
        {QStringLiteral("-o"), QStringLiteral("ProxyCommand=nc %h %p"),
         QStringLiteral("web01")},
        {QStringLiteral("-F"), QStringLiteral("/tmp/cfg"), QStringLiteral("web01")},
        {QStringLiteral("-T"), QStringLiteral("git@github.com")},
    };
    for (const QStringList &arguments : unsupported) {
        QString reason;
        const ConnectionTarget target =
            ShellShim::resolveTarget(arguments, current, &reason);
        QVERIFY2(!target.isValid(), qPrintable(arguments.join(QLatin1Char(' '))));
        // 必须给出原因：远端 shim 正阻塞等待回执，界面也要能解释为什么没接管。
        QVERIFY2(!reason.isEmpty(), qPrintable(arguments.join(QLatin1Char(' '))));
    }
}

void SshCommandParserTest::terminalViewDetectsFinalAuthenticationFailure()
{
    TerminalView view;
    QSignalSpy failureSpy(&view, &TerminalView::nestedSshFailed);

    // 含 "please try again" 的输出表示 OpenSSH 仍会继续询问密码，不能撤销候选层级。
    view.appendTerminalOutput(QByteArrayLiteral("Permission denied, please try again.\r\n"));
    QCOMPARE(failureSpy.size(), 0);
    view.appendTerminalOutput(QByteArrayLiteral(
        "root@10.0.0.2: Permission denied (publickey,password).\r\n"));
    QCOMPARE(failureSpy.size(), 1);
}

void SshCommandParserTest::terminalViewDetectsNonAuthenticationSshFailures()
{
    // 嵌套 ssh 的失败远不止认证被拒。这些输出如果不被识别，失败的尝试就会在连接链上
    // 留下一个用户从未进入过的幽灵层，而它常常和真实层同名，界面上根本分辨不出来。
    const QList<QByteArray> failures = {
        QByteArrayLiteral("ssh: Could not resolve hostname 10194.135.15: "
                          "Name or service not known\r\n"),
        QByteArrayLiteral("ssh: connect to host 10.0.0.9 port 22: Connection refused\r\n"),
        QByteArrayLiteral("ssh: connect to host 10.0.0.9 port 22: No route to host\r\n"),
        QByteArrayLiteral("ssh: connect to host 10.0.0.9 port 22: Connection timed out\r\n"),
        QByteArrayLiteral("Host key verification failed.\r\n"),
        QByteArrayLiteral("Connection closed by 10.0.0.9 port 22\r\n"),
    };

    for (const QByteArray &output : failures) {
        TerminalView view;
        QSignalSpy failureSpy(&view, &TerminalView::nestedSshFailed);
        view.appendTerminalOutput(output);
        QVERIFY2(failureSpy.size() == 1, output.constData());
    }
}

void SshCommandParserTest::terminalViewReportsCtrlCAsNestedSshAbort()
{
    TerminalView view;
    QSignalSpy abortSpy(&view, &TerminalView::nestedSshAborted);

    // Ctrl+C 会让正在连接的 ssh 直接退出。视图只如实报告按键，是否回滚由 TerminalPane
    // 依据"当前是否有未确认的嵌套目标"决定。
#if defined(Q_OS_MACOS)
    QTest::keyClick(&view, Qt::Key_C, Qt::MetaModifier);
#else
    QTest::keyClick(&view, Qt::Key_C, Qt::ControlModifier);
#endif
    QCOMPARE(abortSpy.size(), 1);
}

void SshCommandParserTest::terminalViewIgnoresCtrlDInsideCommandLine()
{
    TerminalView view;
    QSignalSpy commandSpy(&view, &TerminalView::commandSubmitted);

    // 行中的 Ctrl-D 是 delete-char-forward，不是 EOF。把它当作退出会弹掉一层真实存在的
    // 服务器，让文件管理器少一层。
    QTest::keyClicks(&view, QStringLiteral("cat"));
#if defined(Q_OS_MACOS)
    QTest::keyClick(&view, Qt::Key_D, Qt::MetaModifier);
#else
    QTest::keyClick(&view, Qt::Key_D, Qt::ControlModifier);
#endif
    QCOMPARE(commandSpy.size(), 0);

    // 空行上的 Ctrl-D 才是真正的 EOF。
#if defined(Q_OS_MACOS)
    QTest::keyClick(&view, Qt::Key_D, Qt::MetaModifier);
#else
    QTest::keyClick(&view, Qt::Key_D, Qt::ControlModifier);
#endif
    QCOMPARE(commandSpy.size(), 1);
    QCOMPARE(commandSpy.at(0).at(0).toString(), QStringLiteral("Ctrl-D"));
}

void SshCommandParserTest::terminalViewRecoversSshCommandFromCursorLine()
{
    TerminalView view;
    QSignalSpy commandSpy(&view, &TerminalView::commandSubmitted);

    // 模拟屏幕下方残留旧内容、光标回到上一行输入 ssh 的情况。QTextDocument 最后一段是
    // "stale lower row"，只有 libvterm 的真实光标行包含用户实际提交的命令。
    view.appendTerminalOutput(QByteArrayLiteral(
        "old command\r\nstale lower row\x1b[1A\r\x1b[2K"
        "[root@outer ~]# ssh root@10.0.0.2"));
    QTest::keyClick(&view, Qt::Key_Return);

    QCOMPARE(commandSpy.size(), 1);
    QCOMPARE(commandSpy.at(0).at(0).toString(),
             QStringLiteral("ssh root@10.0.0.2"));
}

void SshCommandParserTest::terminalViewDetectsRenderedShellPrompt()
{
    TerminalView view;
    QSignalSpy shellPromptSpy(&view, &TerminalView::shellPromptDetected);

    // 登录横幅不能被当作认证完成；只有最后一行出现真正的交互式提示符才触发信号。
    view.appendTerminalOutput(QByteArrayLiteral("Welcome to Ubuntu\r\nLast login: today\r\n"));
    QCOMPARE(shellPromptSpy.size(), 0);
    view.appendTerminalOutput(QByteArrayLiteral("root@ubuntu:~# "));
    QCOMPARE(shellPromptSpy.size(), 1);
}

void SshCommandParserTest::terminalViewIgnoresStalePromptBelowCursor()
{
    TerminalView view;
    QSignalSpy shellPromptSpy(&view, &TerminalView::shellPromptDetected);

    // 第二行保留一个以 # 结尾的旧提示符，然后把光标移回第一行的 password 提示。
    // 文件管理认证状态必须以光标所在行判断，不能读取 QTextDocument 的最后一段。
    view.appendTerminalOutput(QByteArrayLiteral(
        "root@10.0.0.3's password:\r\n[root@old ~]# \x1b[1A\r"));
    QCOMPARE(shellPromptSpy.size(), 0);
}

void SshCommandParserTest::terminalViewRendersCarriageReturnAndLineFeed()
{
    TerminalView view;
    view.appendTerminalOutput(QByteArrayLiteral("[root@server ~]# "));
    view.appendTerminalOutput(QByteArrayLiteral("\r\n[root@server ~]# "));

    const QStringList renderedLines = view.toPlainText().split(QLatin1Char('\n'));
    QCOMPARE(renderedLines.size(), 2);
    QCOMPARE(renderedLines.at(0).trimmed(), QStringLiteral("[root@server ~]#"));
    QCOMPARE(renderedLines.at(1).trimmed(), QStringLiteral("[root@server ~]#"));
}

void SshCommandParserTest::localZshExecutesInteractiveCommand()
{
    LocalShellWorker worker;
    QSignalSpy startedSpy(&worker, &LocalShellWorker::shellStarted);
    QSignalSpy outputSpy(&worker, &LocalShellWorker::outputReady);
    QSignalSpy finishedSpy(&worker, &LocalShellWorker::shellFinished);
    QSignalSpy errorSpy(&worker, &LocalShellWorker::shellError);

    worker.start();
    QVERIFY2(startedSpy.wait(3000), "本地 zsh 未能在 3 秒内启动");
    worker.enqueueInput(QByteArrayLiteral(
        "printf '__SHELLIUM_ZSH_OK__\\n'\r"
        "print -r -- /bin/zs\t\r"
        "exit\r"));
    QVERIFY2(finishedSpy.wait(5000), "本地 zsh 未能执行命令并正常退出");

    QByteArray completeOutput;
    for (const QList<QVariant> &arguments : outputSpy) {
        completeOutput.append(arguments.at(0).toByteArray());
    }
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty() ? "" : qPrintable(errorSpy.at(0).at(0).toString()));
    QVERIFY2(completeOutput.contains(QByteArrayLiteral("__SHELLIUM_ZSH_OK__")),
             completeOutput.constData());
    QVERIFY2(completeOutput.contains(QByteArrayLiteral("/bin/zsh")),
             "本地 zsh 未对 /bin/zs<Tab> 执行路径补全");
    QCOMPARE(finishedSpy.at(0).at(0).toInt(), 0);
}

void SshCommandParserTest::localZshInterruptsForegroundCommand()
{
    LocalShellWorker worker;
    QSignalSpy startedSpy(&worker, &LocalShellWorker::shellStarted);
    QSignalSpy outputSpy(&worker, &LocalShellWorker::outputReady);
    QSignalSpy finishedSpy(&worker, &LocalShellWorker::shellFinished);

    worker.start();
    QVERIFY2(startedSpy.wait(3000), "本地 zsh 未能启动");
    worker.enqueueInput(QByteArrayLiteral("sleep 10\r"));
    QTest::qWait(150);
    worker.enqueueInput(QByteArray(1, '\x03'));
    worker.enqueueInput(QByteArrayLiteral("printf '__SHELLIUM_INTERRUPT_OK__\\n'\rexit\r"));
    QVERIFY2(finishedSpy.wait(4000), "Ctrl+C 未能及时终止 sleep 10");

    QByteArray completeOutput;
    for (const QList<QVariant> &arguments : outputSpy) {
        completeOutput.append(arguments.at(0).toByteArray());
    }
    QVERIFY2(completeOutput.contains(QByteArrayLiteral("__SHELLIUM_INTERRUPT_OK__")),
             completeOutput.constData());
}

void SshCommandParserTest::databaseCreatesCoreSchema()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    ApplicationDatabase database(
        temporaryDirectory.filePath(QStringLiteral("shellium-test.db")));
    QString error;
    QVERIFY2(database.initialize(&error), qPrintable(error));

    const QStringList requiredTables = {
        QStringLiteral("hosts"), QStringLiteral("credentials"),
        QStringLiteral("snippets"), QStringLiteral("command_history"),
        QStringLiteral("connection_history"), QStringLiteral("port_forwards"),
        QStringLiteral("workspaces"), QStringLiteral("settings"),
        QStringLiteral("plugin_state")
    };
    const QStringList actualTables = database.connection().tables();
    for (const QString &table : requiredTables) {
        QVERIFY2(actualTables.contains(table), qPrintable(QStringLiteral("缺少数据表：%1").arg(table)));
    }

    QSqlQuery foreignKeyQuery(database.connection());
    QVERIFY(foreignKeyQuery.exec(QStringLiteral("PRAGMA foreign_keys")));
    QVERIFY(foreignKeyQuery.next());
    QCOMPARE(foreignKeyQuery.value(0).toInt(), 1);
}

void SshCommandParserTest::vaultEncryptsAndAuthenticatesCredentials()
{
    Vault vault;
    QString error;
    QVERIFY2(vault.unlock(QStringLiteral("correct horse battery staple"), {}, &error),
             qPrintable(error));
    const QByteArray secret = QByteArrayLiteral("root-password-and-private-key");
    const QByteArray encrypted = vault.encrypt(secret, &error);
    QVERIFY2(!encrypted.isEmpty(), qPrintable(error));
    QVERIFY(!encrypted.contains(secret));
    QCOMPARE(vault.decrypt(encrypted, &error), secret);

    QByteArray tampered = encrypted;
    tampered[tampered.size() - 1] = static_cast<char>(tampered.back() ^ 0x01);
    QVERIFY(vault.decrypt(tampered, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    vault.lock();
    QVERIFY(!vault.isUnlocked());
}

QTEST_MAIN(SshCommandParserTest)
#include "SshCommandParserTest.moc"
