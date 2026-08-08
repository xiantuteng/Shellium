#include "core/ConnectionTarget.h"
#include "ssh/ShellShim.h"
#include "terminal/TerminalEchoFilter.h"
#include "ssh/SshCommandParser.h"
#include "terminal/TerminalView.h"
#include "terminal/LocalShellWorker.h"
#include "security/Vault.h"
#include "settings/TerminalSettings.h"
#include "storage/ApplicationDatabase.h"

#include <QClipboard>
#include <QApplication>
#include <QFile>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QInputMethodEvent>
#include <QMenu>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSqlError>
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

    /** @brief 自动重连策略必须限制危险数值并完整持久化。 */
    void terminalSettingsPersistReconnectPolicy();

    /** @brief 普通字符与回车必须从终端视图完整发送，并产生可解析的整行命令。 */
    void terminalViewForwardsKeyboardInput();

    /** @brief Tab 必须发送给 Shell 做补全，不能把焦点切换到其他控件。 */
    void terminalViewForwardsCompletionKeys();

    /** @brief 物理 Ctrl+C 必须编码为 ETX，而不是被 Qt 当作复制快捷键吞掉。 */
    void terminalViewForwardsInterruptKey();

    /** @brief macOS 输入法提交的字符必须发送到远端而不能被只读文本控件吞掉。 */
    void terminalViewForwardsInputMethodText();

    /** @brief 终端右键菜单必须发出当前目录上传请求。 */
    void terminalViewContextMenuRequestsCurrentDirectoryUpload();

    /** @brief 分块到达的标准 SSH password 提示必须被识别且不泄露任何输入。 */
    void terminalViewDetectsSplitAuthenticationPrompt();

    /** @brief 最终 Permission denied 必须与仍可重试的密码错误提示区分。 */
    /** @brief 不进入交互式 Shell 的 ssh 用法不得压栈。 */
    void ignoresNonInteractiveSshInvocations();

    /** @brief 带退出码的 exit 同样是退出。 */
    void treatsExitWithStatusAsLeave();

    /** @brief 本地行捕获必须收齐字符、不发往远端、且只提交一次。 */
    void terminalViewCapturesLocalLine();

    /** @brief 自动重复的回车不得提交本地行，否则会在提示刚出现时提交空密码。 */
    void terminalViewIgnoresAutoRepeatDuringCapture();

    /** @brief 空行提交与 Ctrl+C 放弃必须能被区分。 */
    void terminalViewDistinguishesEmptyLineFromCancel();

    /** @brief 密码常从密码管理器粘贴，捕获期间必须接受粘贴而不是把按键吞掉。 */
    void terminalViewAcceptsPasteDuringCapture();

    /** @brief 普通粘贴必须保留多行能力，同时移除可注入终端状态的危险控制字符。 */
    void terminalViewPreparesClipboardTextSafely();

    /** @brief 回显过滤器要能扣掉整段、跨块的期望串，并在匹配不上时如数放行。 */
    void echoFilterRemovesExpectedEcho();

    /** @brief 双宽字符（中日韩）不得在每个字后面多渲染一个占位字符。 */
    void terminalViewRendersWideCharacters();

    /** @brief VtermBridge 必须把私有 OSC 交出来，而不是渲染成屏幕乱码。 */
    void terminalViewDeliversShellIntegrationEvent();

    /** @brief shim 上报的 argv 要能还原成可直接建立的目标。 */
    void shellShimResolvesInteractiveTarget();

    /** @brief 无法忠实复现的 ssh 用法必须拒绝接管，交还给原生 ssh。 */
    void shellShimDeclinesUnsupportedInvocations();

    /** @brief shim 追加历史策略时必须保留用户已有的 HISTCONTROL 选项。 */
    void shellShimPreservesExistingHistoryPolicy();

    /** @brief 安装命令必须拒绝不兼容 Shell，且只在成功时落下权限受限的脚本。 */
    void shellShimInstallGuardsUnsupportedShells();

    /** @brief -i 指向远端私钥，绝不能被本机 libssh 当作本地路径使用。 */
    void neverUsesRemoteIdentityFileLocally();

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

    /** @brief 首次创建后应只持久化公开配置和密文，并能在新服务实例中重新解锁。 */
    void credentialVaultPersistsEncryptedSecretsAcrossUnlocks();

    /** @brief 错误主密码、空校验块与被篡改的凭据都必须安全失败且保持锁定。 */
    void credentialVaultRejectsWrongPasswordAndCorruptData();

    /** @brief 闲置计时达到阈值才应清除密钥，禁用超时后不得自动锁定。 */
    void credentialVaultLocksAfterIdleTimeout();
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

void SshCommandParserTest::terminalViewContextMenuRequestsCurrentDirectoryUpload()
{
    TerminalView view;
    QSignalSpy uploadSpy(&view, &TerminalView::uploadToCurrentDirectoryRequested);
    bool foundUploadAction = false;
    QMenu *menu = view.createTerminalContextMenu();
    QVERIFY(menu != nullptr);
    for (QAction *action : menu->actions()) {
        if (action->text().contains(QStringLiteral("上传到当前远程目录"))) {
            foundUploadAction = true;
            action->trigger();
            break;
        }
    }
    // createTerminalContextMenu 明确把所有权交给调用方，测试也必须覆盖这条释放契约。
    delete menu;

    QVERIFY2(foundUploadAction, "终端右键菜单没有提供当前远程目录上传动作");
    QCOMPARE(uploadSpy.count(), 1);
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

void SshCommandParserTest::neverUsesRemoteIdentityFileLocally()
{
    ConnectionTarget current;
    current.id = QStringLiteral("layer-1");
    current.host = QStringLiteral("10.0.0.1");
    current.username = QStringLiteral("root");

    // -i 的路径属于远端文件系统，而接管后的认证由本机 libssh 完成。照搬这个路径要么
    // 找不到文件，要么更糟——撞上本机一个同名的无关私钥，用错误的身份去认证。
    for (const QStringList &arguments :
         {QStringList{QStringLiteral("-i"), QStringLiteral("/root/.ssh/inner_key"),
                      QStringLiteral("10.0.0.2")},
          QStringList{QStringLiteral("-o"), QStringLiteral("IdentityFile=/root/.ssh/k"),
                      QStringLiteral("10.0.0.2")}}) {
        QString reason;
        const ConnectionTarget target =
            ShellShim::resolveTarget(arguments, current, &reason);
        QVERIFY2(!target.isValid(), qPrintable(arguments.join(QLatin1Char(' '))));
        QVERIFY2(!reason.isEmpty(), "拒绝接管必须给出原因");
    }

    // 降级路径无法拒绝（用户已经连过去了），但同样不得把远端路径交给本机；仍要正确消费
    // 该参数，否则它的取值会被误当成目标主机名。
    const SshCommandParser::Result parsed = SshCommandParser::parse(
        QStringLiteral("ssh -i /root/.ssh/inner_key root@10.0.0.2"), current);
    QCOMPARE(parsed.action, SshCommandParser::Action::EnterServer);
    QCOMPARE(parsed.target.host, QStringLiteral("10.0.0.2"));
    QVERIFY2(parsed.target.identityFile.isEmpty(),
             qPrintable(QStringLiteral("远端私钥路径被传给了本机：%1")
                            .arg(parsed.target.identityFile)));
}

void SshCommandParserTest::terminalViewCapturesLocalLine()
{
    TerminalView view;
    QSignalSpy lineSpy(&view, &TerminalView::localLineSubmitted);
    QSignalSpy inputSpy(&view, &TerminalView::inputBytes);
    QSignalSpy commandSpy(&view, &TerminalView::commandSubmitted);

    view.beginLocalLineCapture();
    QVERIFY(view.isCapturingLocalLine());

    QTest::keyClicks(&view, QStringLiteral("hunter2"));
    // 密码绝不能发往远端：这一层的 SSH 会话尚未认证完成，远端还不存在。
    QCOMPARE(inputSpy.size(), 0);

    QTest::keyClick(&view, Qt::Key_Return);
    QCOMPARE(lineSpy.size(), 1);
    QCOMPARE(lineSpy.at(0).at(0).toString(), QStringLiteral("hunter2"));
    QCOMPARE(lineSpy.at(0).at(1).toBool(), false);
    QVERIFY(!view.isCapturingLocalLine());
    // 回车不得同时走普通路径，否则密码会以命令的形式再发一遍。
    QCOMPARE(inputSpy.size(), 0);
    QCOMPARE(commandSpy.size(), 0);
}

void SshCommandParserTest::terminalViewIgnoresAutoRepeatDuringCapture()
{
    TerminalView view;
    QSignalSpy lineSpy(&view, &TerminalView::localLineSubmitted);
    view.beginLocalLineCapture();

    // 用户敲完 ssh 命令后按住回车稍久，重复事件会恰好落在密码捕获刚开启的瞬间。若不忽略
    // 就会提交一个空行，现象是"密码提示一出现就立刻失败"。
    QKeyEvent repeated(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier,
                       QStringLiteral("\r"), true);
    QCoreApplication::sendEvent(&view, &repeated);
    QCOMPARE(lineSpy.size(), 0);
    QVERIFY(view.isCapturingLocalLine());

    // 真实按下的回车仍必须提交。
    QTest::keyClicks(&view, QStringLiteral("pw"));
    QTest::keyClick(&view, Qt::Key_Return);
    QCOMPARE(lineSpy.size(), 1);
    QCOMPARE(lineSpy.at(0).at(0).toString(), QStringLiteral("pw"));
}

void SshCommandParserTest::terminalViewDistinguishesEmptyLineFromCancel()
{
    TerminalView view;
    QSignalSpy lineSpy(&view, &TerminalView::localLineSubmitted);

    // 直接回车：空行但不是放弃，上层应当重新提示（与 OpenSSH 一致）。
    view.beginLocalLineCapture();
    QTest::keyClick(&view, Qt::Key_Return);
    QCOMPARE(lineSpy.size(), 1);
    QVERIFY(lineSpy.at(0).at(0).toString().isEmpty());
    QCOMPARE(lineSpy.at(0).at(1).toBool(), false);

    // Ctrl+C：明确放弃。
    view.beginLocalLineCapture();
#if defined(Q_OS_MACOS)
    QTest::keyClick(&view, Qt::Key_C, Qt::MetaModifier);
#else
    QTest::keyClick(&view, Qt::Key_C, Qt::ControlModifier);
#endif
    QCOMPARE(lineSpy.size(), 2);
    QCOMPARE(lineSpy.at(1).at(1).toBool(), true);
}

void SshCommandParserTest::terminalViewAcceptsPasteDuringCapture()
{
    // 尾随换行是密码管理器的常态；控制字符对密码没有意义，必须剔除后再进入缓冲。
    QGuiApplication::clipboard()->setText(QStringLiteral("8QyKZBrJ\n"));

    TerminalView view;
    QSignalSpy lineSpy(&view, &TerminalView::localLineSubmitted);
    QSignalSpy inputSpy(&view, &TerminalView::inputBytes);
    view.beginLocalLineCapture();

    // macOS 上 Cmd+V 携带 Qt::ControlModifier。若不显式处理粘贴，它会被"追加普通字符"
    // 分支排除并整个吞掉，用户随后按回车就提交了空密码——手打正常、粘贴永远连不上。
    QTest::keySequence(&view, QKeySequence(QKeySequence::Paste));
    QCOMPARE(inputSpy.size(), 0);
    QVERIFY(view.isCapturingLocalLine());

    QTest::keyClick(&view, Qt::Key_Return);
    QCOMPARE(lineSpy.size(), 1);
    QCOMPARE(lineSpy.at(0).at(0).toString(), QStringLiteral("8QyKZBrJ"));
    QCOMPARE(lineSpy.at(0).at(1).toBool(), false);

    // 粘贴之后继续手打也必须能接上。
    QGuiApplication::clipboard()->setText(QStringLiteral("abc"));
    view.beginLocalLineCapture();
    QTest::keySequence(&view, QKeySequence(QKeySequence::Paste));
    QTest::keyClicks(&view, QStringLiteral("123"));
    QTest::keyClick(&view, Qt::Key_Return);
    QCOMPARE(lineSpy.size(), 2);
    QCOMPARE(lineSpy.at(1).at(0).toString(), QStringLiteral("abc123"));
}

void SshCommandParserTest::terminalViewPreparesClipboardTextSafely()
{
    const QString raw = QStringLiteral("echo first\r\necho second\targ")
        + QChar(0x001b)
        + QChar::Null
        + QChar(0x009b);
    const TerminalView::PastePreparation prepared = TerminalView::prepareClipboardPaste(raw);

    // CRLF 规范化后，多行脚本仍保持两行；Tab 是有意义的终端输入，也必须保留。
    QCOMPARE(prepared.text, QStringLiteral("echo first\necho second\targ"));
    QVERIFY(prepared.containsLineBreak);
    QCOMPARE(prepared.removedControlCharacters, qsizetype(3));
    QVERIFY(prepared.requiresConfirmation());

    // 普通单行文本无需打断用户；该快速路径也是最常见的复制命令使用方式。
    const TerminalView::PastePreparation ordinary =
        TerminalView::prepareClipboardPaste(QStringLiteral("ls -la /srv/app"));
    QCOMPARE(ordinary.text, QStringLiteral("ls -la /srv/app"));
    QVERIFY(!ordinary.containsLineBreak);
    QCOMPARE(ordinary.removedControlCharacters, qsizetype(0));
    QVERIFY(!ordinary.requiresConfirmation());

    // 裸 CR 同样代表换行，不能因来源平台不同而悄悄变成普通字符或重复两次换行。
    const TerminalView::PastePreparation legacyMac =
        TerminalView::prepareClipboardPaste(QStringLiteral("one\rtwo"));
    QCOMPARE(legacyMac.text, QStringLiteral("one\ntwo"));
    QVERIFY(legacyMac.requiresConfirmation());
}

void SshCommandParserTest::echoFilterRemovesExpectedEcho()
{
    // 无登记时原样放行，且不应产生额外拷贝路径上的差异。
    {
        TerminalEchoFilter filter;
        QCOMPARE(filter.filter(QByteArrayLiteral("hello")), QByteArrayLiteral("hello"));
    }

    // 整段一次到达：期望串被扣掉，前后内容保留。
    {
        TerminalEchoFilter filter;
        filter.expect(QByteArrayLiteral("take"));
        QCOMPARE(filter.filter(QByteArrayLiteral("abc take def")),
                 QByteArrayLiteral("abc  def"));
    }

    // 跨数据块：期望串被网络拆成两半，尾部必须扣留到下一块到达后再判定。
    {
        TerminalEchoFilter filter;
        filter.expect(QByteArrayLiteral("take"));
        QCOMPARE(filter.filter(QByteArrayLiteral("xx ta")), QByteArrayLiteral("xx "));
        QCOMPARE(filter.filter(QByteArrayLiteral("ke yy")), QByteArrayLiteral(" yy"));
    }

    // 部分匹配最终没有兑现：扣留的字节必须原样吐出来，不能被吞掉。
    {
        TerminalEchoFilter filter;
        filter.expect(QByteArrayLiteral("take"));
        QCOMPARE(filter.filter(QByteArrayLiteral("ta")), QByteArray());
        QCOMPARE(filter.filter(QByteArrayLiteral("lk")), QByteArrayLiteral("talk"));
    }

    // 多段按登记顺序依次匹配。
    {
        TerminalEchoFilter filter;
        filter.expect(QByteArrayLiteral("one"));
        filter.expect(QByteArrayLiteral("two"));
        QCOMPARE(filter.filter(QByteArrayLiteral("[one][two][one]")),
                 QByteArrayLiteral("[][][one]"));
    }

    // 预算耗尽：远端把回显改写过时必须放弃匹配，绝不能无限期扣住用户的输出。
    {
        TerminalEchoFilter filter;
        filter.expect(QByteArrayLiteral("never-appears"));
        QByteArray produced;
        for (int round = 0; round < 12; ++round) {
            produced += filter.filter(QByteArray(1024, 'x'));
        }
        QCOMPARE(produced.size(), 12 * 1024);
    }

    // reset 必须交出扣留的字节：会话切换后旧期望串永远不会再出现。
    {
        TerminalEchoFilter filter;
        filter.expect(QByteArrayLiteral("take"));
        QCOMPARE(filter.filter(QByteArrayLiteral("ta")), QByteArray());
        QCOMPARE(filter.reset(), QByteArrayLiteral("ta"));
        QCOMPARE(filter.filter(QByteArrayLiteral("take")), QByteArrayLiteral("take"));
    }
}

void SshCommandParserTest::terminalViewRendersWideCharacters()
{
    TerminalView view;
    view.appendTerminalOutput(QStringLiteral("正在连接 ok").toUtf8());
    const QString rendered = view.toPlainText().trimmed();
    QVERIFY2(rendered == QStringLiteral("正在连接 ok"),
             qPrintable(QStringLiteral("实际渲染为：[%1]").arg(rendered)));
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

void SshCommandParserTest::shellShimPreservesExistingHistoryPolicy()
{
    const QByteArray script = ShellShim::scriptBody();

    // 先做结构断言，直接锁住曾经覆盖用户设置的危险写法。仅验证最终输出还不够：未来若
    // 恰好由 Bash 自身合并变量，测试可能误绿，而源码仍在无条件赋值。
    QVERIFY(!script.contains("export HISTCONTROL=ignorespace"));
    QVERIFY(script.contains("${HISTCONTROL:+$HISTCONTROL:}ignorespace"));

    const auto evaluateHistoryControl = [&script](const QByteArray &initialValue) {
        QProcess bash;
        const QByteArray command = "HISTCONTROL=" + initialValue
            + "; export HISTCONTROL; " + script
            + "; printf '%s' \"$HISTCONTROL\"";
        bash.start(QStringLiteral("/bin/bash"),
                   {QStringLiteral("--noprofile"), QStringLiteral("--norc"),
                    QStringLiteral("-c"), QString::fromUtf8(command)});
        if (!bash.waitForStarted(5000) || !bash.waitForFinished(5000)
            || bash.exitStatus() != QProcess::NormalExit || bash.exitCode() != 0) {
            return QByteArrayLiteral("<process-failed>") + bash.readAllStandardError();
        }
        return bash.readAllStandardOutput();
    };

    // 原有去重策略必须保留，并在其后追加 ignorespace；已经包含 ignoreboth 时不重复追加。
    QCOMPARE(evaluateHistoryControl(QByteArrayLiteral("erasedups")),
             QByteArrayLiteral("erasedups:ignorespace"));
    QCOMPARE(evaluateHistoryControl(QByteArrayLiteral("ignoreboth")),
             QByteArrayLiteral("ignoreboth"));
}

void SshCommandParserTest::shellShimInstallGuardsUnsupportedShells()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString scriptPath = directory.filePath(QStringLiteral(".shellium-shim"));
    const QByteArray install = ShellShim::installCommand(scriptPath.toUtf8());

    const auto runInstall = [&install](const QString &loginShell) {
        QProcess process;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("SHELL"), loginShell);
        process.setProcessEnvironment(environment);
        process.start(QStringLiteral("/bin/sh"),
                      {QStringLiteral("-c"), QString::fromUtf8(install)});
        if (!process.waitForStarted(5000) || !process.waitForFinished(5000)
            || process.exitStatus() != QProcess::NormalExit) {
            return -1;
        }
        return process.exitCode();
    };

    // fish/csh 不能解析 `. path` 与 POSIX 函数体；安装阶段必须明确失败且绝不能留下文件，
    // 这样 SshSessionStack 才不会继续向用户的交互终端发送不兼容语法。
    QCOMPARE(runInstall(QStringLiteral("/usr/bin/fish")), 64);
    QVERIFY(!QFile::exists(scriptPath));

    // bash 环境正常落盘；umask 077 是安全边界，临时脚本不得被同机其他用户读取或修改。
    QCOMPARE(runInstall(QStringLiteral("/bin/bash")), 0);
    QFile scriptFile(scriptPath);
    QVERIFY(scriptFile.open(QIODevice::ReadOnly));
    QCOMPARE(scriptFile.readAll().trimmed(),
             (ShellShim::scriptBody() + "\nrm -f " + scriptPath.toUtf8()).trimmed());
    const QFile::Permissions permissions = scriptFile.permissions();
    QVERIFY(!(permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                             | QFileDevice::ReadOther | QFileDevice::WriteOther)));
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
        QStringLiteral("plugin_state"), QStringLiteral("vault_metadata")
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

void SshCommandParserTest::credentialVaultPersistsEncryptedSecretsAcrossUnlocks()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    ApplicationDatabase database(
        temporaryDirectory.filePath(QStringLiteral("credential-vault.db")));
    QString error;
    QVERIFY2(database.initialize(&error), qPrintable(error));

    CredentialVault firstSession(database);
    QCOMPARE(firstSession.state(&error), CredentialVault::State::Uninitialized);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(firstSession.initialize(QStringLiteral("一条足够长的主密码-2026"), &error),
             qPrintable(error));
    QCOMPARE(firstSession.state(&error), CredentialVault::State::Unlocked);

    QByteArray secret = QByteArrayLiteral("database-root-password");
    HostRecord host;
    host.id = QStringLiteral("host-db");
    host.name = QStringLiteral("生产数据库");
    host.host = QStringLiteral("db.internal");
    ConnectionTarget configuredTarget = host.toConnectionTarget();
    configuredTarget.password = QString::fromUtf8(secret);
    QVERIFY2(firstSession.saveConnectionPassword(host,
                                                 configuredTarget,
                                                 QStringLiteral("数据库 root"),
                                                 &error),
             qPrintable(error));
    QVERIFY2(!host.credentialId.isEmpty(), "新凭据没有回写到主机 credentialId");
    QCOMPARE(database.findHost(host.id, &error)->credentialId, host.credentialId);

    // 直接检查数据库，确保磁盘列中不存在明文；nonce 与 payload 分开存储便于明确验证格式。
    const std::optional<CredentialRecord> stored =
        database.findCredential(host.credentialId, &error);
    QVERIFY2(stored.has_value(), qPrintable(error));
    QVERIFY(!stored->encryptedPayload.contains(secret));
    QCOMPARE(stored->cipher, Vault::cipherAlgorithm());
    QCOMPARE(stored->nonce.size(), Vault::nonceSize());
    QCOMPARE(stored->kdfSalt.size(), qsizetype(16));

    firstSession.lock();
    QVERIFY(!firstSession.loadSecret(host.credentialId, &error).has_value());
    QVERIFY(!error.isEmpty());

    // 新服务对象模拟应用重启：它只能从公开 salt/verifier 恢复状态，绝不会继承上一对象的密钥。
    CredentialVault secondSession(database);
    QCOMPARE(secondSession.state(&error), CredentialVault::State::Locked);
    error.clear();
    QVERIFY2(secondSession.unlock(QStringLiteral("一条足够长的主密码-2026"), &error),
             qPrintable(error));
    std::optional<QByteArray> loaded = secondSession.loadSecret(host.credentialId, &error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(*loaded, secret);

    ConnectionTarget restoredTarget = host.toConnectionTarget();
    QVERIFY2(secondSession.applyHostCredential(host, restoredTarget, &error), qPrintable(error));
    QCOMPARE(restoredTarget.password, QString::fromUtf8(secret));

    // 主机引用写入失败时，前一步插入的凭据必须随外层事务回滚，不能留下无人引用的密文。
    HostRecord invalidHost;
    invalidHost.id = QStringLiteral("host-with-missing-parent");
    invalidHost.parentId = QStringLiteral("missing-parent");
    invalidHost.name = QStringLiteral("无法保存的主机");
    invalidHost.host = QStringLiteral("invalid.internal");
    ConnectionTarget invalidTarget = invalidHost.toConnectionTarget();
    invalidTarget.password = QStringLiteral("must-be-rolled-back");
    error.clear();
    QVERIFY(!secondSession.saveConnectionPassword(invalidHost, invalidTarget, {}, &error));
    QVERIFY2(!error.isEmpty(), "主机外键失败没有向 UI 返回原因");
    QVERIFY(invalidHost.credentialId.isEmpty());
    bool credentialsReadable = false;
    QCOMPARE(database.listCredentials(&credentialsReadable, &error).size(), 1);
    QVERIFY2(credentialsReadable, qPrintable(error));

    // 测试也遵守生产约束：使用完的明文副本都主动清零，而不是等待分配器未来覆盖内存。
    Vault::secureErase(*loaded);
    QVERIFY(loaded->isEmpty());
    Vault::secureErase(secret);
}

void SshCommandParserTest::credentialVaultRejectsWrongPasswordAndCorruptData()
{
    ApplicationDatabase database(ApplicationDatabase::inMemoryPath());
    QString error;
    QVERIFY2(database.initialize(&error), qPrintable(error));
    CredentialVault vault(database);
    QVERIFY2(vault.initialize(QStringLiteral("correct-master-password"), &error), qPrintable(error));
    QVERIFY2(vault.saveSecret(QStringLiteral("cred-1"),
                              QStringLiteral("生产密码"),
                              CredentialType::Password,
                              QByteArrayLiteral("top-secret"),
                              &error),
             qPrintable(error));
    vault.lock();

    QVERIFY(!vault.unlock(QStringLiteral("wrong-master-password"), &error));
    QVERIFY(!vault.isUnlocked());
    QVERIFY2(!error.isEmpty(), "错误主密码没有返回可诊断信息");

    error.clear();
    QVERIFY2(vault.unlock(QStringLiteral("correct-master-password"), &error), qPrintable(error));
    QSqlQuery corruptCredential(database.connection());
    QVERIFY2(corruptCredential.exec(QStringLiteral(
                 "UPDATE credentials SET encrypted_payload = zeroblob(length(encrypted_payload)) "
                 "WHERE id = 'cred-1'")),
             qPrintable(corruptCredential.lastError().text()));
    QVERIFY(!vault.loadSecret(QStringLiteral("cred-1"), &error).has_value());
    QVERIFY2(!error.isEmpty(), "被篡改的认证密文被当成了正常凭据");

    vault.lock();
    QSqlQuery corruptVerifier(database.connection());
    QVERIFY2(corruptVerifier.exec(QStringLiteral("UPDATE vault_metadata SET verifier = X'' WHERE id = 1")),
             qPrintable(corruptVerifier.lastError().text()));
    error.clear();
    QCOMPARE(vault.state(&error), CredentialVault::State::Corrupt);
    QVERIFY2(!error.isEmpty(), "空校验块没有被识别为损坏状态");
    QVERIFY(!vault.unlock(QStringLiteral("correct-master-password"), &error));
    QVERIFY(!vault.isUnlocked());

    // 损坏配置绝不能被 initialize 当成首次启动覆盖，否则原有凭据会永久失去解密密钥。
    QVERIFY(!vault.initialize(QStringLiteral("replacement-password"), &error));

    // “没有 vault_metadata 行”只有在凭据表也为空时才是真正首次启动。旧库若已有密文却
    // 丢失全局配置，必须停在损坏状态，否则新建主密码会让旧密文永久不可恢复。
    ApplicationDatabase orphanedDatabase(ApplicationDatabase::inMemoryPath());
    QVERIFY2(orphanedDatabase.initialize(&error), qPrintable(error));
    CredentialRecord orphaned;
    orphaned.id = QStringLiteral("orphaned-credential");
    orphaned.name = QStringLiteral("缺少保险库配置的旧凭据");
    orphaned.encryptedPayload = QByteArray(16, '\x51');
    orphaned.cipher = Vault::cipherAlgorithm();
    orphaned.nonce = QByteArray(Vault::nonceSize(), '\x52');
    orphaned.kdfAlgorithm = Vault::kdfAlgorithm();
    orphaned.kdfSalt = QByteArray(16, '\x53');
    orphaned.kdfIterations = Vault::kdfOperationsLimit();
    orphaned.kdfMemoryKib = Vault::kdfMemoryKib();
    orphaned.kdfParallelism = 1;
    QVERIFY2(orphanedDatabase.saveCredential(orphaned, &error), qPrintable(error));
    CredentialVault orphanedVault(orphanedDatabase);
    error.clear();
    QCOMPARE(orphanedVault.state(&error), CredentialVault::State::Corrupt);
    QVERIFY2(!error.isEmpty(), "已有凭据但缺少校验块的数据库被误判为首次启动");
    QVERIFY(!orphanedVault.initialize(QStringLiteral("must-not-overwrite"), &error));
}

void SshCommandParserTest::credentialVaultLocksAfterIdleTimeout()
{
    ApplicationDatabase database(ApplicationDatabase::inMemoryPath());
    QString error;
    QVERIFY2(database.initialize(&error), qPrintable(error));
    CredentialVault vault(database);
    QVERIFY2(vault.initialize(QStringLiteral("idle-timeout-master-password"), &error),
             qPrintable(error));

    vault.setIdleTimeoutMs(500);
    vault.recordActivity(1000);
    QVERIFY(!vault.hasIdleTimedOut(1499));
    QVERIFY(!vault.lockIfIdle(1499));
    QVERIFY(vault.isUnlocked());
    QVERIFY(vault.hasIdleTimedOut(1500));
    QVERIFY(vault.lockIfIdle(1500));
    QVERIFY(!vault.isUnlocked());

    QVERIFY2(vault.unlock(QStringLiteral("idle-timeout-master-password"), &error),
             qPrintable(error));
    vault.setIdleTimeoutMs(0); // 用户明确禁用自动锁定时，再大的时间差也不能清除密钥。
    vault.recordActivity(2000);
    QVERIFY(!vault.lockIfIdle(2000000));
    QVERIFY(vault.isUnlocked());
}

void SshCommandParserTest::terminalSettingsPersistReconnectPolicy()
{
    ApplicationDatabase database(ApplicationDatabase::inMemoryPath());
    QString error;
    QVERIFY2(database.initialize(&error), qPrintable(error));
    TerminalSettingsService service(&database);
    QVERIFY2(service.load(&error), qPrintable(error));

    TerminalSettings settings = service.currentSettings();
    settings.automaticReconnect = true;
    settings.reconnectMaxAttempts = 99;
    settings.reconnectBaseDelaySeconds = 0;
    QVERIFY2(service.save(settings, &error), qPrintable(error));
    QCOMPARE(service.currentSettings().reconnectMaxAttempts, 10);
    QCOMPARE(service.currentSettings().reconnectBaseDelaySeconds, 1);

    TerminalSettingsService reloaded(&database);
    QVERIFY2(reloaded.load(&error), qPrintable(error));
    QVERIFY(reloaded.currentSettings().automaticReconnect);
    QCOMPARE(reloaded.currentSettings().reconnectMaxAttempts, 10);
    QCOMPARE(reloaded.currentSettings().reconnectBaseDelaySeconds, 1);
    QCOMPARE(reloaded.shortcut(QStringLiteral("command.palette")),
             QKeySequence(QStringLiteral("Ctrl+Shift+P")));
}

QTEST_MAIN(SshCommandParserTest)

#include "SshCommandParserTest.moc"
