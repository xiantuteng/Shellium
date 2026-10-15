#include "core/ConnectionTarget.h"
#include "ssh/ShellShim.h"
#include "ssh/KnownHostsManager.h"
#include "terminal/TerminalEchoFilter.h"
#include "ssh/SshCommandParser.h"
#include "terminal/TerminalView.h"
#include "terminal/LocalShellWorker.h"
#include "security/Vault.h"
#include "ui/VaultDialog.h"
#include "settings/TerminalSettings.h"
#include "storage/ApplicationDatabase.h"

#include <QAbstractButton>
#include <QClipboard>
#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QGuiApplication>
#include <QMimeData>
#include <QSignalSpy>
#include <QInputMethodEvent>
#include <QMenu>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPointer>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QApplication>
#include <QLayout>
#include <QScrollBar>
#include <QSet>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include <libssh/libssh.h>

#include <memory>

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

    /** @brief 替换冲突密钥时只能删除精确旧行，其他主机、注释与空行必须保留。 */
    void knownHostsReplacementPreservesUnrelatedContent();

    /** @brief 哈希过的主机字段必须继续保持哈希，不能因密钥轮换泄露服务器地址。 */
    void knownHostsReplacementPreservesHashedHostname();

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

    /** @brief 拖入本地文件必须过滤出本地路径并发出 filesDropped。 */
    void terminalViewDropEventEmitsLocalFilePaths();

    /** @brief 拖拽内容里没有任何本地文件时不得发出 filesDropped。 */
    void terminalViewDropEventIgnoresNonLocalUrls();

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
    /** @brief 多行粘贴确认默认关闭：多行内容必须直接进入 PTY，不弹任何模态窗口。 */
    void terminalViewMultiLinePasteConfirmationFollowsSetting();

    /** @brief 回显过滤器要能扣掉整段、跨块的期望串，并在匹配不上时如数放行。 */
    void echoFilterRemovesExpectedEcho();

    /** @brief 注入 shim 后屏幕上只能留下一个提示符，不能新旧两个并排。 */
    void shellIntegrationInjectionLeavesSinglePrompt();

    /** @brief readline 接管前抵达时远端会回显两次，屏幕上同样不能留下注入行。 */
    void shellIntegrationInjectionSurvivesDoubleEcho();

    /** @brief 可选期望没有兑现时不得扣留任何字节，也不得改变输出。 */
    void optionalEchoExpectationCostsNothingWhenAbsent();

    /** @brief 双宽字符（中日韩）不得在每个字后面多渲染一个占位字符。 */
    void terminalViewRendersWideCharacters();

    /** @brief VtermBridge 必须把私有 OSC 交出来，而不是渲染成屏幕乱码。 */
    void terminalViewDeliversShellIntegrationEvent();

    /** @brief shim 上报的 argv 要能还原成可直接建立的目标。 */
    void shellShimResolvesInteractiveTarget();

    /** @brief 无法忠实复现的 ssh 用法必须拒绝接管，交还给原生 ssh。 */
    void shellShimDeclinesUnsupportedInvocations();

    /** @brief OpenSSH 详细日志开关必须交还原生 ssh，不能被 libssh 接管后静默吞掉。 */
    void shellShimDeclinesVerboseInvocations();

    /** @brief shim 追加历史策略时必须保留用户已有的 HISTCONTROL 选项。 */
    void shellShimPreservesExistingHistoryPolicy();

    /** @brief 安装命令必须拒绝不兼容 Shell，且只在成功时落下权限受限的脚本。 */
    void shellShimInstallGuardsUnsupportedShells();

    /** @brief cwd 上报载荷必须解析出 Kind::Cwd 与原始路径，空路径视为无效事件。 */
    void shellShimParsesCwdEvent();

    /** @brief 注入脚本必须为 bash 和 zsh 分别挂好去重的 cwd 上报钩子。 */
    void shellShimRegistersCwdHookForBothShells();

    /** @brief 钩子必须随 cwd 一起上报 Shell 进程号，且解析出 Kind::ShellPid。 */
    void shellShimReportsShellPid();

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

    /** @brief Tab 补全、上翻历史后提交的命令必须取远端回显，而不是残缺的本地按键缓冲。 */
    void terminalViewSubmitsShellCompletedCommand();

    /** @brief 折行的长命令要整条拼回；回显不可用时退回本地缓冲。 */
    void terminalViewSubmitsWrappedEchoAndFallsBack();

    /** @brief 上翻历史后行内的 Ctrl-D 不是 EOF。 */
    void terminalViewIgnoresCtrlDOnRecalledLine();

    /** @brief 内层 Shell 提示符必须在渲染完成后被识别，用于延迟开放第 2 层文件管理。 */
    void terminalViewDetectsRenderedShellPrompt();

    /** @brief 光标下方残留的旧提示符不能把正在等待密码的目标误判为认证成功。 */
    void terminalViewIgnoresStalePromptBelowCursor();

    /** @brief PTY 的 CRLF 必须形成新行，不能让 Shell 提示符横向堆叠。 */
    void terminalViewRendersCarriageReturnAndLineFeed();

    /** @brief 连续拖动宽度时只提交最终 PTY 尺寸，并保持 Shell 提示符文本不变。 */
    void terminalResizeDebouncesPtyUpdates();

    /** @brief 平台默认 Shell 必须在真实 PTY/ConPTY 中接收命令、返回输出并正常退出。 */
    /** @brief 验证本地子进程从用户主目录启动，并且不改变应用工作目录。 */
    void localShellStartsInHomeDirectory();
    void localZshExecutesInteractiveCommand();

    /** @brief ETX 必须通过 PTY/ConPTY 终止本地 Shell 前台命令。 */
    void localZshInterruptsForegroundCommand();

    /** @brief SQLite 初始化必须创建全部核心功能表并开启外键约束。 */
    void databaseCreatesCoreSchema();

    /** @brief 保险库应正确往返加密，并拒绝被篡改的密文。 */
    void vaultEncryptsAndAuthenticatesCredentials();

    /** @brief 首次创建后应只持久化公开配置和密文，并能在新服务实例中重新解锁。 */
    void credentialVaultPersistsEncryptedSecretsAcrossUnlocks();

    /** @brief 未启用主密码时也应自动加密保存，并可在重启后的透明模式中自动读取。 */
    void credentialVaultUsesPersistentLocalKeyWithoutMasterPassword();

    /** @brief 主密码保护的启用和关闭必须双向迁移凭据并清除关闭后的保险箱配置。 */
    void credentialVaultMigratesCredentialsAcrossProtectionModes();

    /** @brief 忘记密码重置必须删除凭据但完整保留主机的非敏感配置。 */
    void credentialVaultResetPreservesHostConfiguration();

    /** @brief 编辑仍被多台主机直接引用的历史凭据时必须先为当前主机执行写时分离。 */
    void credentialVaultSeparatesSharedCredentialBeforePasswordUpdate();

    /** @brief 错误主密码、空校验块与被篡改的凭据都必须安全失败且保持锁定。 */
    void credentialVaultRejectsWrongPasswordAndCorruptData();

    /** @brief 闲置计时达到阈值才应清除密钥，禁用超时后不得自动锁定。 */
    void credentialVaultLocksAfterIdleTimeout();

    /** @brief 输出超过一屏后，跟随底部与停留在历史位置都不得被重建文档重置到顶部。 */
    void terminalViewKeepsScrollPositionAcrossRender();

    /** @brief 本地状态提示必须以 CRLF 结束，避免远端输出被推到光标所在列。 */
    void localNoticeEndsWithCarriageReturn();

    /** @brief 应用级样式表不得覆盖终端背景色，空白区与字符区必须同色。 */
    void terminalBackgroundIgnoresApplicationStyleSheet();

    /** @brief 解锁模式下不得有游离在布局之外的可见子控件。 */
    void vaultDialogHasNoStrayWidgets();

    /** @brief 主密码错误要在对话框里内联提示，不能再叠一层需要先关掉的弹窗。 */
    void vaultDialogReportsWrongPasswordInline();

    /** @brief 解锁页必须通过输入确认短语才能执行忘记密码重置。 */
    void vaultDialogResetsForgottenPasswordAfterTypedConfirmation();

    /** @brief 首次创建保险库时必须展示安全徽标和不可恢复警示，且强制流程不渲染取消按钮。 */
    void vaultCreationDialogShowsSecurityHierarchy();

    /** @brief 光标闪烁重建文档时不得丢失滚动位置，跟随底部必须持续有效。 */
    void terminalKeepsBottomAcrossCursorBlink();
};

namespace {

/**
 * @brief 等待终端渲染出帧的宽限时间（毫秒）。
 *
 * TerminalView 把渲染按帧合并：appendTerminalOutput 只把字节喂给 libvterm，同一帧内到达的
 * 多段输出攒在一起画。因此凡是要断言"文档里出现了什么"或"滚动条到了哪里"的用例，都必须先
 * 放行一帧，否则测到的是上一帧的旧状态。
 */
constexpr int kRenderFrameMilliseconds = 60;

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

void SshCommandParserTest::knownHostsReplacementPreservesUnrelatedContent()
{
    // 公钥正文是两个结构合法、内容不同的 Ed25519 公钥 blob。使用真实格式可确保测试经过
    // libssh 逐行解析器，而不是只验证普通字符串替换。
    const QByteArray oldPublicKey = QByteArrayLiteral(
        "AAAAC3NzaC1lZDI1NTE5AAAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    const QByteArray newPublicKey = QByteArrayLiteral(
        "AAAAC3NzaC1lZDI1NTE5AAAAIAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEB");
    const QByteArray lookupHost = QByteArrayLiteral("[8.148.83.73]:2222");
    const QByteArray oldEntry = QByteArrayLiteral(
        "[8.148.83.73]:2222 ssh-ed25519 ") + oldPublicKey;
    const QByteArray newEntry = QByteArrayLiteral(
        "[8.148.83.73]:2222 ssh-ed25519 ") + newPublicKey;
    const QByteArray unrelatedEntry = QByteArrayLiteral(
        "other.example ssh-ed25519 ") + oldPublicKey;
    const QByteArray original = QByteArrayLiteral("# production hosts\r\n")
        + oldEntry + QByteArrayLiteral("\r\n\r\n")
        + unrelatedEntry + QByteArrayLiteral("\n") + oldEntry + QByteArrayLiteral("\n");

    bool replaced = false;
    const QByteArray rewritten = KnownHostsManager::Detail::replaceHostEntries(
        original, lookupHost, newEntry, &replaced);

    QVERIFY(replaced);
    QVERIFY(!rewritten.contains(oldEntry));
    QCOMPARE(rewritten.count(newEntry), 1);
    QVERIFY(rewritten.contains(QByteArrayLiteral("# production hosts\r\n")));
    QVERIFY(rewritten.contains(unrelatedEntry));
}

void SshCommandParserTest::knownHostsReplacementPreservesHashedHostname()
{
    const QByteArray lookupHost = QByteArrayLiteral("8.148.83.73");
    // 盐为 ASCII `salt-for-test`；HMAC-SHA1 按 OpenSSH |1| 格式对 lookupHost 计算。固定
    // 向量让用例无需调用外部 ssh-keygen，也能验证 libssh 确实识别哈希主机字段。
    const QByteArray hashedHost = QByteArrayLiteral(
        "|1|c2FsdC1mb3ItdGVzdA==|tUiOdWbBvCbi3YslYi7Asu2/V6Q=");
    const QByteArray oldEntry = hashedHost
        + QByteArrayLiteral(
            " ssh-ed25519 "
            "AAAAC3NzaC1lZDI1NTE5AAAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    const QByteArray exportedEntry = QByteArrayLiteral(
        "8.148.83.73 ssh-ed25519 "
        "AAAAC3NzaC1lZDI1NTE5AAAAIAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEB");

    bool replaced = false;
    const QByteArray rewritten = KnownHostsManager::Detail::replaceHostEntries(
        oldEntry + QByteArrayLiteral("\n"), lookupHost, exportedEntry, &replaced);

    QVERIFY(replaced);
    QCOMPARE(rewritten,
             hashedHost
                 + QByteArrayLiteral(
                     " ssh-ed25519 "
                     "AAAAC3NzaC1lZDI1NTE5AAAAIAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEB\n"));
    QVERIFY(!rewritten.contains(QByteArrayLiteral("8.148.83.73")));
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
        if (action->text().contains(QStringLiteral("Upload to the current remote directory"))) {
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

void SshCommandParserTest::terminalViewDropEventEmitsLocalFilePaths()
{
    TerminalView view;
    view.resize(400, 300);
    QSignalSpy dropSpy(&view, &TerminalView::filesDropped);

    const QString localPath = QDir::tempPath() + QStringLiteral("/shellium-drop-test.txt");
    QMimeData mimeData;
    mimeData.setUrls({QUrl::fromLocalFile(localPath)});

    QDragEnterEvent dragEnter(QPoint(5, 5), Qt::CopyAction, &mimeData,
                              Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(view.viewport(), &dragEnter);
    QVERIFY2(dragEnter.isAccepted(), "拖入本地文件时终端必须接受拖拽，否则系统不会投递落点事件");

    QDropEvent dropEvent(QPoint(5, 5), Qt::CopyAction, &mimeData,
                         Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(view.viewport(), &dropEvent);

    QCOMPARE(dropSpy.count(), 1);
    QCOMPARE(dropSpy.at(0).at(0).toStringList(), QStringList{localPath});
}

void SshCommandParserTest::terminalViewDropEventIgnoresNonLocalUrls()
{
    TerminalView view;
    view.resize(400, 300);
    QSignalSpy dropSpy(&view, &TerminalView::filesDropped);

    // 远程/网络 URL 没有对应的本地磁盘路径，不能被当作待上传文件处理。
    QMimeData mimeData;
    mimeData.setUrls({QUrl(QStringLiteral("https://example.com/file.txt"))});

    QDropEvent dropEvent(QPoint(5, 5), Qt::CopyAction, &mimeData,
                         Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(view.viewport(), &dropEvent);

    QCOMPARE(dropSpy.count(), 0);
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
    QVERIFY(prepared.requiresConfirmation(true));
    // 控制字符是用户从剪贴板内容表面看不出来的改动，即使关闭了"多行粘贴确认"也必须确认。
    QVERIFY(prepared.requiresConfirmation(false));

    // 普通单行文本无需打断用户；该快速路径也是最常见的复制命令使用方式。
    const TerminalView::PastePreparation ordinary =
        TerminalView::prepareClipboardPaste(QStringLiteral("ls -la /srv/app"));
    QCOMPARE(ordinary.text, QStringLiteral("ls -la /srv/app"));
    QVERIFY(!ordinary.containsLineBreak);
    QCOMPARE(ordinary.removedControlCharacters, qsizetype(0));
    QVERIFY(!ordinary.requiresConfirmation(true));
    QVERIFY(!ordinary.requiresConfirmation(false));

    // 裸 CR 同样代表换行，不能因来源平台不同而悄悄变成普通字符或重复两次换行。
    const TerminalView::PastePreparation legacyMac =
        TerminalView::prepareClipboardPaste(QStringLiteral("one\rtwo"));
    QCOMPARE(legacyMac.text, QStringLiteral("one\ntwo"));
    QVERIFY(legacyMac.requiresConfirmation(true));
    // 纯多行、无控制字符：确认与否完全由设置决定，默认关闭时直接粘贴。
    QVERIFY(!legacyMac.requiresConfirmation(false));
}

void SshCommandParserTest::terminalViewMultiLinePasteConfirmationFollowsSetting()
{
    QGuiApplication::clipboard()->setText(QStringLiteral("echo one\necho two\necho three"));

    TerminalView view;
    TerminalSettings settings;
    // 默认值本身就是"关闭"，显式写出是为了让用例在默认值哪天被改动时立刻失败。
    QVERIFY(!settings.multiLinePasteConfirmation);
    view.applySettings(settings);

    // exec() 会阻塞在事件循环里，因此不能在粘贴之后再检查是否弹窗——那行代码永远等不到。
    // 用定时器在事件循环内部观察：一旦出现模态窗口就记录并关掉它，用例才能给出可读的失败
    // 而不是整体挂死。
    bool modalAppeared = false;
    QTimer modalWatcher;
    modalWatcher.setInterval(20);
    QObject::connect(&modalWatcher, &QTimer::timeout, [&modalAppeared] {
        if (QWidget *modal = QApplication::activeModalWidget(); modal != nullptr) {
            // 记录布尔而不是 QPointer：确认框是 confirmPaste() 的栈对象，exec() 一返回就析构，
            // QPointer 会自动清空，断言反而永远看不到它曾经出现过。
            modalAppeared = true;
            modal->close();
        }
    });
    modalWatcher.start();

    QSignalSpy inputSpy(&view, &TerminalView::inputBytes);
    QTest::keySequence(&view, QKeySequence(QKeySequence::Paste));
    modalWatcher.stop();

    QVERIFY2(!modalAppeared, "多行粘贴确认默认关闭时不得弹出确认框");
    QCOMPARE(inputSpy.size(), 1);
    QCOMPARE(inputSpy.at(0).at(0).toByteArray(),
             QByteArrayLiteral("echo one\necho two\necho three"));
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

    // 命中后要送出替代串，且必须落在命中位置，而不是被挪到本段末尾——替代串是光标控制
    // 序列，它与后面那段远端输出的先后次序就是全部意义所在。
    {
        TerminalEchoFilter filter;
        filter.expect(QByteArrayLiteral("take"), QByteArrayLiteral("<erase>"));
        QCOMPARE(filter.filter(QByteArrayLiteral("abc take def")),
                 QByteArrayLiteral("abc <erase> def"));
    }

    // 替代串默认为空：未登记替代串的老调用点行为不得改变。
    {
        TerminalEchoFilter filter;
        filter.expect(QByteArrayLiteral("take"));
        QCOMPARE(filter.filter(QByteArrayLiteral("[take]")), QByteArrayLiteral("[]"));
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

void SshCommandParserTest::shellIntegrationInjectionLeavesSinglePrompt()
{
    const QByteArray path = QByteArrayLiteral("/tmp/.shellium-shim-42");
    const QByteArray injection = ShellShim::sourceCommand(path);
    const QByteArray prompt = QByteArrayLiteral("[root@tidb01 ~]# ");

    TerminalEchoFilter filter;
    // 与 TerminalPane 收到 shellIntegrationPathReady 时的登记完全一致。
    filter.expect(injection + QByteArrayLiteral("\r\n"),
                  ShellShim::sourceCommandEchoReplacement());

    TerminalView view;
    // 真实远端的字节顺序：登录横幅 → 第一个提示符 → 注入行回显 → 回车 → 脚本静默执行
    // → Shell 因为刚执行完一条命令再画一个提示符。
    view.appendTerminalOutput(
        filter.filter(QByteArrayLiteral("Last login: Mon Aug 10 09:01:14 2026\r\n")));
    view.appendTerminalOutput(filter.filter(prompt));
    view.appendTerminalOutput(filter.filter(injection + QByteArrayLiteral("\r\n")));
    view.appendTerminalOutput(filter.filter(prompt));

    // 连续几段输出会被合帧压到一次重绘里，直接读文档会读到上一帧。等合帧定时器落地。
    QTest::qWait(80);

    const QString rendered = view.terminalPlainText();
    QVERIFY2(!rendered.contains(QStringLiteral("shellium-shim")),
             qPrintable(QStringLiteral("注入行泄漏到屏幕：[%1]").arg(rendered)));
    QVERIFY2(rendered.count(QStringLiteral("[root@tidb01 ~]#")) == 1,
             qPrintable(QStringLiteral("提示符出现了 %1 次：[%2]")
                            .arg(rendered.count(QStringLiteral("[root@tidb01 ~]#")))
                            .arg(rendered)));

    // 擦掉的必须只是提示符那一行：登录横幅是远端的正常输出，一个字也不能被连带清掉。
    QVERIFY2(rendered.contains(QStringLiteral("Last login: Mon Aug 10 09:01:14 2026")),
             qPrintable(QStringLiteral("登录横幅被误擦：[%1]").arg(rendered)));
}

void SshCommandParserTest::shellIntegrationInjectionSurvivesDoubleEcho()
{
    const QByteArray path = QByteArrayLiteral("/tmp/.shm-604e3e15");
    const QByteArray injection = ShellShim::sourceCommand(path);
    const QByteArray prompt = QByteArrayLiteral("[root@tidb01 ~]# ");

    TerminalEchoFilter filter;
    // 与 TerminalPane 收到 shellIntegrationPathReady 时的登记完全一致。
    filter.expect(injection + QByteArrayLiteral("\r\n"),
                  ShellShim::sourceCommandEchoReplacement());
    filter.expectOptional(injection + QByteArrayLiteral("\r\n"),
                          ShellShim::sourceCommandEchoReplacement());

    TerminalView view;
    // 真机抓包（root@10.194.135.11，bash 5）：注入行赶在 readline 接管 PTY 之前抵达时，
    // 行规程先按普通输入回显一次，readline 随后把它当预输入连同提示符再重画一次。
    view.appendTerminalOutput(
        filter.filter(QByteArrayLiteral("Last login: Thu Aug 20 15:59:09 2026\r\r\n")));
    view.appendTerminalOutput(filter.filter(injection + QByteArrayLiteral("\r\n")));
    view.appendTerminalOutput(filter.filter(prompt + injection
                                            + QByteArrayLiteral("\r\n")));
    view.appendTerminalOutput(filter.filter(prompt));

    QTest::qWait(80);

    const QString rendered = view.terminalPlainText();
    QVERIFY2(!rendered.contains(QStringLiteral(".shm-604e3e15")),
             qPrintable(QStringLiteral("注入行泄漏到屏幕：[%1]").arg(rendered)));
    QVERIFY2(rendered.count(QStringLiteral("[root@tidb01 ~]#")) == 1,
             qPrintable(QStringLiteral("提示符出现了 %1 次：[%2]")
                            .arg(rendered.count(QStringLiteral("[root@tidb01 ~]#")))
                            .arg(rendered)));
    QVERIFY2(rendered.contains(QStringLiteral("Last login: Thu Aug 20 15:59:09 2026")),
             qPrintable(QStringLiteral("登录横幅被误擦：[%1]").arg(rendered)));
}

void SshCommandParserTest::optionalEchoExpectationCostsNothingWhenAbsent()
{
    const QByteArray injection =
        ShellShim::sourceCommand(QByteArrayLiteral("/tmp/.shm-604e3e15"))
        + QByteArrayLiteral("\r\n");

    TerminalEchoFilter filter;
    filter.expect(injection);
    filter.expectOptional(injection);

    // 单次回显是常态（会话栈等远端静默后才下发）。第一条兑现之后，只剩那条可选期望，
    // 它绝不能扣留提示符末尾的空格——注入行以空格开头，一扣光标就停在 `#` 上不动了。
    QCOMPARE(filter.filter(injection), QByteArray());
    QCOMPARE(filter.filter(QByteArrayLiteral("[root@tidb01 ~]# ")),
             QByteArrayLiteral("[root@tidb01 ~]# "));

    // 迟迟不兑现的可选期望必须自行过期，不能在很久以后忽然扣掉一段同形的正常输出。
    QByteArray produced;
    for (int round = 0; round < 3; ++round) {
        produced += filter.filter(QByteArray(512, 'x'));
    }
    QCOMPARE(produced.size(), 3 * 512);
    QCOMPARE(filter.filter(injection), injection);
}

void SshCommandParserTest::terminalViewRendersWideCharacters()
{
    TerminalView view;
    view.appendTerminalOutput(QStringLiteral("正在连接 ok").toUtf8());
    const QString rendered = view.terminalPlainText().trimmed();
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

void SshCommandParserTest::shellShimDeclinesVerboseInvocations()
{
    ConnectionTarget current;
    current.id = QStringLiteral("layer-1");
    current.host = QStringLiteral("10.0.0.1");
    current.username = QStringLiteral("root");

    // -v 可以重复至三级。三种写法都必须返回无效接管目标，使远端 shim 回复 pass 后执行
    // `command ssh "$@"`；只有真实 OpenSSH 进程才能把 debug1/debug2/debug3 写入终端。
    const QStringList verboseFlags = {
        QStringLiteral("-v"),
        QStringLiteral("-vv"),
        QStringLiteral("-vvv"),
    };
    for (const QString &verboseFlag : verboseFlags) {
        QString reason;
        const ConnectionTarget target = ShellShim::resolveTarget(
            {verboseFlag, QStringLiteral("root@192.168.0.1")}, current, &reason);
        QVERIFY2(!target.isValid(), qPrintable(verboseFlag));
        QVERIFY2(reason.contains(QStringLiteral("OpenSSH")), qPrintable(reason));
    }

    // shim 回退只决定“谁来执行终端里的 ssh”，不会关闭第 2 层文件管理。命令提交时的
    // 降级解析器仍须识别同一个目标，并记录从当前服务器出发的转发路径；TerminalPane 会
    // 用这个推测目标建立独立 SFTP 会话。这里锁住该协作关系，避免修复日志后意外丢失
    // 嵌套文件管理能力。
    const SshCommandParser::Result inferred = SshCommandParser::parse(
        QStringLiteral("ssh -v root@192.168.0.1"), current);
    QCOMPARE(inferred.action, SshCommandParser::Action::EnterServer);
    QVERIFY(inferred.target.isValid());
    QCOMPARE(inferred.target.host, QStringLiteral("192.168.0.1"));
    QCOMPARE(inferred.target.username, QStringLiteral("root"));
    QCOMPARE(inferred.target.parentId, current.id);
    QCOMPARE(inferred.target.proxyJumps,
             (QStringList{current.connectionSpecifier(true)}));

    // 防止检测条件过宽：普通无参数开关仍可由 Shellium 接管，不能因为参数中存在字母 v
    // 就让所有嵌套连接退化成推测模式。
    QString reason;
    const ConnectionTarget normalTarget = ShellShim::resolveTarget(
        {QStringLiteral("-4"), QStringLiteral("root@192.168.0.1")}, current, &reason);
    QVERIFY(normalTarget.isValid());
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

void SshCommandParserTest::shellShimParsesCwdEvent()
{
    const ShellShim::Event event =
        ShellShim::parseEvent(QByteArrayLiteral("cwd;/srv/app"));
    QCOMPARE(event.kind, ShellShim::Event::Kind::Cwd);
    QCOMPARE(event.workingDirectory, QStringLiteral("/srv/app"));
    QVERIFY(event.arguments.isEmpty());

    // 路径本身允许包含分号，解析时不能在第一个 ';' 之后继续拆分。
    const ShellShim::Event withSemicolon =
        ShellShim::parseEvent(QByteArrayLiteral("cwd;/srv/weird;name"));
    QCOMPARE(withSemicolon.kind, ShellShim::Event::Kind::Cwd);
    QCOMPARE(withSemicolon.workingDirectory, QStringLiteral("/srv/weird;name"));

    // 空路径不是一次有效上报，不能被当作"跳转到根目录"处理。
    const ShellShim::Event empty = ShellShim::parseEvent(QByteArrayLiteral("cwd;"));
    QCOMPARE(empty.kind, ShellShim::Event::Kind::None);
}

void SshCommandParserTest::shellShimReportsShellPid()
{
    const QByteArray script = ShellShim::scriptBody();
    QVERIFY(script.contains(QByteArrayLiteral("]6773;pid;%s\\007")));
    QVERIFY(script.contains(QByteArrayLiteral("\"$$\" \"$PWD\"")));

    const ShellShim::Event event = ShellShim::parseEvent(QByteArrayLiteral("pid;12345"));
    QCOMPARE(event.kind, ShellShim::Event::Kind::ShellPid);
    QCOMPARE(event.processId, qint64(12345));

    QCOMPARE(ShellShim::parseEvent(QByteArrayLiteral("pid;")).kind, ShellShim::Event::Kind::None);
    QCOMPARE(ShellShim::parseEvent(QByteArrayLiteral("pid;0")).kind, ShellShim::Event::Kind::None);
    QCOMPARE(ShellShim::parseEvent(QByteArrayLiteral("pid;12a")).kind, ShellShim::Event::Kind::None);
}


void SshCommandParserTest::shellShimRegistersCwdHookForBothShells()
{
    const QByteArray script = ShellShim::scriptBody();

    // 上报函数必须复用既有的私有 OSC 编号，且携带 cwd 前缀，否则 TerminalPane 无法
    // 与 connect 事件区分开。
    QVERIFY(script.contains(QByteArrayLiteral("]6773;cwd;%s\\007")));
    QVERIFY(script.contains(QByteArrayLiteral("_shellium_report_cwd")));

    // bash 追加到 PROMPT_COMMAND、zsh 追加到 precmd_functions，且都先查重，
    // 避免同一层被多次 source 后钩子重复挂载导致重复上报。
    QVERIFY(script.contains(QByteArrayLiteral("PROMPT_COMMAND=")));
    QVERIFY(script.contains(QByteArrayLiteral("precmd_functions+=(_shellium_report_cwd)")));
    QVERIFY(script.contains(QByteArrayLiteral("*\";_shellium_report_cwd;\"*")));
    QVERIFY(script.contains(QByteArrayLiteral("*\" _shellium_report_cwd \"*")));
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

void SshCommandParserTest::terminalViewSubmitsShellCompletedCommand()
{
    TerminalView view;
    QSignalSpy commandSpy(&view, &TerminalView::commandSubmitted);

    // 测试里没有 PTY，回显由 appendTerminalOutput 手工扮演远端 Shell。
    view.appendTerminalOutput(QByteArrayLiteral("[root@host ~]# "));
    QTest::keyClicks(&view, QStringLiteral("./sta"));
    view.appendTerminalOutput(QByteArrayLiteral("./sta"));
    QTest::keyClick(&view, Qt::Key_Tab);
    view.appendTerminalOutput(QByteArrayLiteral("rt.sh "));
    QTest::keyClick(&view, Qt::Key_Return);
    QCOMPARE(commandSpy.size(), 1);
    QCOMPARE(commandSpy.at(0).at(0).toString(), QStringLiteral("./start.sh"));

    // 路径中段补全后继续手打：缓冲是 "cd /dageo"，远端实际是 "cd /data/geohoop"。
    view.appendTerminalOutput(QByteArrayLiteral("\r\n[root@host ~]# "));
    QTest::keyClicks(&view, QStringLiteral("cd /da"));
    view.appendTerminalOutput(QByteArrayLiteral("cd /da"));
    QTest::keyClick(&view, Qt::Key_Tab);
    view.appendTerminalOutput(QByteArrayLiteral("ta/"));
    QTest::keyClicks(&view, QStringLiteral("geo"));
    view.appendTerminalOutput(QByteArrayLiteral("geo"));
    QTest::keyClick(&view, Qt::Key_Tab);
    // bash 有多个候选时会列出它们，再在新行重画提示符和输入行。
    view.appendTerminalOutput(QByteArrayLiteral(
        "\r\ngeohoop/  geohosma/\r\n[root@host ~]# cd /data/geoho"));
    QTest::keyClicks(&view, QStringLiteral("op"));
    view.appendTerminalOutput(QByteArrayLiteral("op"));
    QTest::keyClick(&view, Qt::Key_Return);
    QCOMPARE(commandSpy.size(), 2);
    QCOMPARE(commandSpy.at(1).at(0).toString(), QStringLiteral("cd /data/geohoop"));

    // 上翻历史：本地缓冲为空，整条命令都来自远端。
    view.appendTerminalOutput(QByteArrayLiteral("\r\n[root@host ~]# "));
    QTest::keyClick(&view, Qt::Key_Up);
    view.appendTerminalOutput(QByteArrayLiteral("vi app"));
    QTest::keyClick(&view, Qt::Key_Return);
    QCOMPARE(commandSpy.size(), 3);
    QCOMPARE(commandSpy.at(2).at(0).toString(), QStringLiteral("vi app"));

    // zsh RPROMPT 右对齐在同一行，与光标隔着大段空白，不属于命令。
    view.appendTerminalOutput(QByteArrayLiteral("\r\n\x1b[70G~/src\r% "));
    QTest::keyClicks(&view, QStringLiteral("ls -"));
    view.appendTerminalOutput(QByteArrayLiteral("ls -"));
    QTest::keyClick(&view, Qt::Key_Tab);
    view.appendTerminalOutput(QByteArrayLiteral("-all"));
    QTest::keyClick(&view, Qt::Key_Return);
    QCOMPARE(commandSpy.size(), 4);
    QCOMPARE(commandSpy.at(3).at(0).toString(), QStringLiteral("ls --all"));

    // 光标左移到行中再回车，光标后的内容仍是命令的一部分。
    view.appendTerminalOutput(QByteArrayLiteral("\r\n$ "));
    QTest::keyClick(&view, Qt::Key_Up);
    view.appendTerminalOutput(QByteArrayLiteral("mv ho ho1006\b\b\b\b"));
    QTest::keyClick(&view, Qt::Key_Return);
    QCOMPARE(commandSpy.size(), 5);
    QCOMPARE(commandSpy.at(4).at(0).toString(), QStringLiteral("mv ho ho1006"));
}

void SshCommandParserTest::terminalViewSubmitsWrappedEchoAndFallsBack()
{
    TerminalView view;
    QSignalSpy commandSpy(&view, &TerminalView::commandSubmitted);

    // 默认 80 列：提示符加 URL 必然折到第二、三行，只读光标行会丢掉开头的 wget。
    const QString url = QStringLiteral("http://10.212.14.25:8888/1006/")
        + QString(120, QLatin1Char('a')) + QStringLiteral(".jar");
    view.appendTerminalOutput(QByteArrayLiteral("[root@host ~]# "));
    QTest::keyClicks(&view, QStringLiteral("wget http"));
    view.appendTerminalOutput(QByteArrayLiteral("wget http"));
    QTest::keyClick(&view, Qt::Key_Tab);
    view.appendTerminalOutput(url.mid(4).toUtf8());
    QTest::keyClick(&view, Qt::Key_Return);
    QCOMPARE(commandSpy.size(), 1);
    QCOMPARE(commandSpy.at(0).at(0).toString(), QStringLiteral("wget ") + url);

    // 提示符被后台输出冲掉、对不上锚点时，宁可用本地缓冲也不能把别的输出当成命令。
    view.appendTerminalOutput(QByteArrayLiteral("\r\n[root@host ~]# "));
    QTest::keyClicks(&view, QStringLiteral("make"));
    QTest::keyClick(&view, Qt::Key_Tab);
    view.appendTerminalOutput(QByteArrayLiteral("\r\nbackground job done"));
    QTest::keyClick(&view, Qt::Key_Return);
    QCOMPARE(commandSpy.size(), 2);
    QCOMPARE(commandSpy.at(1).at(0).toString(), QStringLiteral("make"));

    // 纯手打不依赖回显：远端还没来得及回显就回车，命令也不能丢字。
    view.appendTerminalOutput(QByteArrayLiteral("\r\n[root@host ~]# "));
    QTest::keyClicks(&view, QStringLiteral("ls"));
    QTest::keyClick(&view, Qt::Key_Return);
    QCOMPARE(commandSpy.size(), 3);
    QCOMPARE(commandSpy.at(2).at(0).toString(), QStringLiteral("ls"));
}

void SshCommandParserTest::terminalViewIgnoresCtrlDOnRecalledLine()
{
    TerminalView view;
    QSignalSpy commandSpy(&view, &TerminalView::commandSubmitted);

    view.appendTerminalOutput(QByteArrayLiteral("$ "));
    QTest::keyClick(&view, Qt::Key_Up);
    view.appendTerminalOutput(QByteArrayLiteral("exit\b\b\b\b"));
#if defined(Q_OS_MACOS)
    QTest::keyClick(&view, Qt::Key_D, Qt::MetaModifier);
#else
    QTest::keyClick(&view, Qt::Key_D, Qt::ControlModifier);
#endif
    // 行里还有上翻出来的 exit，这一下只是删掉光标处的字符，不能弹掉一层服务器。
    QCOMPARE(commandSpy.size(), 0);
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
    // 渲染按帧合并：同一帧内到达的多段输出攒在一起画，断言前必须放行一帧。
    QTest::qWait(kRenderFrameMilliseconds);

    const QStringList renderedLines = view.terminalPlainText().split(QLatin1Char('\n'));
    QCOMPARE(renderedLines.size(), 2);
    QCOMPARE(renderedLines.at(0).trimmed(), QStringLiteral("[root@server ~]#"));
    QCOMPARE(renderedLines.at(1).trimmed(), QStringLiteral("[root@server ~]#"));
}

void SshCommandParserTest::terminalResizeDebouncesPtyUpdates()
{
    TerminalView view;
    view.resize(900, 360);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    // 等待首次布局的防抖提交完成，再把它从本用例关注的拖动信号中排除。
    QTest::qWait(120);
    QSignalSpy sizeSpy(&view, &TerminalView::terminalSizeChanged);
    view.appendTerminalOutput(QByteArrayLiteral("[root@tidb01 ~]# "));
    const QString originalText = view.terminalPlainText();

    // 31 次交替 resize 模拟持续拖动分隔条。每次事件间隔显著小于 80ms 防抖窗口，最终
    // 宽度与初始宽度不同，因而稳定后必须恰好产生一次有效字符尺寸提交。
    for (int iteration = 0; iteration < 31; ++iteration) {
        view.resize(iteration % 2 == 0 ? 620 : 840, 360);
        QCoreApplication::processEvents();
        QTest::qWait(2);
    }
    QCOMPARE(sizeSpy.size(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(sizeSpy.size(), 1, 500);

    const QList<QVariant> finalSize = sizeSpy.constFirst();
    QVERIFY(finalSize.at(0).toInt() > 0);
    QVERIFY(finalSize.at(1).toInt() > 0);
    QCOMPARE(view.terminalPlainText(), originalText);
    QCOMPARE(view.terminalPlainText().count(QStringLiteral("[root@tidb01 ~]#")), 1);
}

/**
 * @brief 用独立的测试 Shell 检查启动目录，避免用户配置中的 cd 掩盖真实初始目录。
 * 此用例无参数和返回值；临时文件、进程启动或目录断言失败时由 Qt Test 报错。
 */
void SshCommandParserTest::localShellStartsInHomeDirectory()
{
#if defined(Q_OS_WIN)
    QSKIP("此用例使用 POSIX 脚本检查 forkpty 子进程的初始目录");
#else
    // 不修改全局 cwd，避免影响并行线程；构建测试通常从项目目录运行。
    const QString applicationDirectory = QDir::currentPath();
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    QFile script(temporaryDirectory.filePath(QStringLiteral("check-home.sh")));
    QVERIFY(script.open(QIODevice::WriteOnly));
    // 脚本不加载交互配置，直接比较物理目录；退出码可避免终端回显造成假阳性。
    const QByteArray scriptBytes = "#!/bin/sh\n# 检查初始目录是否为主目录，失败返回非零状态。\n"
        "test \"$(pwd -P)\" = \"$(cd \"$HOME\" && pwd -P)\"\n";
    QCOMPARE(script.write(scriptBytes), qint64(scriptBytes.size()));
    script.close();
    QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    LocalShellWorker worker(script.fileName());
    QSignalSpy finishedSpy(&worker, &LocalShellWorker::shellFinished);
    worker.start();
    QVERIFY2(finishedSpy.wait(5000), "检查启动目录的子进程未及时退出");
    QCOMPARE(finishedSpy.first().first().toInt(), 0);
    QCOMPARE(QDir::currentPath(), applicationDirectory);
#endif
}

namespace {

/** @brief 本地 zsh 用例要启动的程序：Windows 交给平台默认 Shell，POSIX 固定为 zsh。 */
QString localZshPath()
{
#if defined(Q_OS_WIN)
    return {};
#else
    return QStringLiteral("/bin/zsh");
#endif
}

} // namespace

void SshCommandParserTest::localZshExecutesInteractiveCommand()
{
#if !defined(Q_OS_WIN)
    if (!QFileInfo(localZshPath()).isExecutable()) {
        QSKIP("本机未安装 /bin/zsh");
    }
#endif
    // 默认程序跟随用户的登录 Shell；这里用到 zsh 专有语法，必须显式指定 zsh。
    LocalShellWorker worker(localZshPath());
    QSignalSpy startedSpy(&worker, &LocalShellWorker::shellStarted);
    QSignalSpy outputSpy(&worker, &LocalShellWorker::outputReady);
    QSignalSpy finishedSpy(&worker, &LocalShellWorker::shellFinished);
    QSignalSpy errorSpy(&worker, &LocalShellWorker::shellError);

    worker.start();
    QVERIFY2(startedSpy.wait(3000), "本地 Shell 未能在 3 秒内启动");
#if defined(Q_OS_WIN)
    // PowerShell 与 cmd 都支持 echo/exit；Windows 用例关注 ConPTY 交互链路，不把测试绑定
    // 到某一种可选 Shell 的专有补全语法。
    worker.enqueueInput(QByteArrayLiteral("echo __SHELLIUM_ZSH_OK__\rexit\r"));
#else
    worker.enqueueInput(QByteArrayLiteral(
        "printf '__SHELLIUM_ZSH_OK__\\n'\r"
        "print -r -- /bin/zs\t\r"
        "exit\r"));
#endif
    QVERIFY2(finishedSpy.wait(5000), "本地 Shell 未能执行命令并正常退出");

    QByteArray completeOutput;
    for (const QList<QVariant> &arguments : outputSpy) {
        completeOutput.append(arguments.at(0).toByteArray());
    }
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty() ? "" : qPrintable(errorSpy.at(0).at(0).toString()));
    QVERIFY2(completeOutput.contains(QByteArrayLiteral("__SHELLIUM_ZSH_OK__")),
             completeOutput.constData());
#if !defined(Q_OS_WIN)
    QVERIFY2(completeOutput.contains(QByteArrayLiteral("/bin/zsh")),
             "本地 zsh 未对 /bin/zs<Tab> 执行路径补全");
#endif
    QCOMPARE(finishedSpy.at(0).at(0).toInt(), 0);
}

void SshCommandParserTest::localZshInterruptsForegroundCommand()
{
#if !defined(Q_OS_WIN)
    if (!QFileInfo(localZshPath()).isExecutable()) {
        QSKIP("本机未安装 /bin/zsh");
    }
#endif
    // 默认程序跟随用户的登录 Shell；这里用到 zsh 专有语法，必须显式指定 zsh。
    LocalShellWorker worker(localZshPath());
    QSignalSpy startedSpy(&worker, &LocalShellWorker::shellStarted);
    QSignalSpy outputSpy(&worker, &LocalShellWorker::outputReady);
    QSignalSpy finishedSpy(&worker, &LocalShellWorker::shellFinished);

    worker.start();
    QVERIFY2(startedSpy.wait(3000), "本地 Shell 未能启动");
#if defined(Q_OS_WIN)
    // ping 在 PowerShell 与 cmd 均可用，可作为可被 Ctrl+C 中断的前台长任务。
    worker.enqueueInput(QByteArrayLiteral("ping -n 11 127.0.0.1\r"));
#else
    worker.enqueueInput(QByteArrayLiteral("sleep 10\r"));
#endif
    QTest::qWait(150);
    worker.enqueueInput(QByteArray(1, '\x03'));
#if defined(Q_OS_WIN)
    worker.enqueueInput(QByteArrayLiteral("echo __SHELLIUM_INTERRUPT_OK__\rexit\r"));
#else
    worker.enqueueInput(QByteArrayLiteral("printf '__SHELLIUM_INTERRUPT_OK__\\n'\rexit\r"));
#endif
    QVERIFY2(finishedSpy.wait(4000), "Ctrl+C 未能及时终止本地前台长任务");

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

void SshCommandParserTest::credentialVaultUsesPersistentLocalKeyWithoutMasterPassword()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    ApplicationDatabase database(
        temporaryDirectory.filePath(QStringLiteral("local-credential-vault.db")));
    QString error;
    QVERIFY2(database.initialize(&error), qPrintable(error));

    CredentialVault firstSession(database);
    QVERIFY(!firstSession.passwordProtectionEnabled());
    QCOMPARE(firstSession.state(&error), CredentialVault::State::Uninitialized);

    HostRecord host;
    host.id = QStringLiteral("local-key-host");
    host.name = QStringLiteral("透明保存主机");
    host.host = QStringLiteral("192.0.2.10");
    ConnectionTarget target = host.toConnectionTarget();
    target.password = QStringLiteral("自动保存但不可查看的密码");
    QVERIFY2(firstSession.saveConnectionPassword(host, target, {}, &error), qPrintable(error));

    const std::optional<CredentialRecord> stored =
        database.findCredential(host.credentialId, &error);
    QVERIFY2(stored.has_value(), qPrintable(error));
    QCOMPARE(stored->kdfAlgorithm, QStringLiteral("local-key-v1"));
    QVERIFY(!stored->encryptedPayload.contains(target.password.toUtf8()));
    QVERIFY(!firstSession.credentialRequiresUnlock(host.credentialId, &error));

    // 新对象不继承任何内存密钥；仍能从持久化的本机随机密钥恢复，证明关闭主密码后自动
    // 连接不依赖用户每次启动输入口令。
    CredentialVault secondSession(database);
    std::optional<QByteArray> restored = secondSession.loadSecret(host.credentialId, &error);
    QVERIFY2(restored.has_value(), qPrintable(error));
    QCOMPARE(QString::fromUtf8(*restored), target.password);
    Vault::secureErase(*restored);
}

void SshCommandParserTest::credentialVaultMigratesCredentialsAcrossProtectionModes()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    ApplicationDatabase database(
        temporaryDirectory.filePath(QStringLiteral("credential-protection-migration.db")));
    QString error;
    QVERIFY2(database.initialize(&error), qPrintable(error));

    CredentialVault vault(database);
    HostRecord host;
    host.id = QStringLiteral("protection-migration-host");
    host.name = QStringLiteral("保护模式迁移主机");
    host.host = QStringLiteral("192.0.2.80");
    ConnectionTarget target = host.toConnectionTarget();
    target.password = QStringLiteral("迁移前后必须保持一致的密码");
    QVERIFY2(vault.saveConnectionPassword(host, target, {}, &error), qPrintable(error));
    QCOMPARE(database.findCredential(host.credentialId, &error)->kdfAlgorithm,
             QStringLiteral("local-key-v1"));

    const QString masterPassword = QStringLiteral("用于双向迁移测试的主密码-2026");
    QVERIFY2(vault.enablePasswordProtection(masterPassword, &error), qPrintable(error));
    QVERIFY(vault.passwordProtectionEnabled());
    QVERIFY(vault.isUnlocked());
    QCOMPARE(database.setting(QStringLiteral("security/vault_password_enabled"), &error)
                 .value_or(QString()),
             QStringLiteral("1"));
    QCOMPARE(database.findCredential(host.credentialId, &error)->kdfAlgorithm,
             Vault::kdfAlgorithm());

    std::optional<QByteArray> protectedSecret = vault.loadSecret(host.credentialId, &error);
    QVERIFY2(protectedSecret.has_value(), qPrintable(error));
    QCOMPARE(QString::fromUtf8(*protectedSecret), target.password);
    Vault::secureErase(*protectedSecret);

    // 关闭时即使当前会话已经解锁，也必须验证本次提供的密码。错误密码不能修改任何密文、
    // 设置或保险箱公开配置；unlock() 清除了旧会话密钥，因此状态会安全退回 Locked。
    QVERIFY(!vault.disablePasswordProtection(QStringLiteral("错误主密码"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(vault.passwordProtectionEnabled());
    QCOMPARE(vault.state(&error), CredentialVault::State::Locked);
    QVERIFY(database.vaultConfiguration(&error).has_value());
    QCOMPARE(database.findCredential(host.credentialId, &error)->kdfAlgorithm,
             Vault::kdfAlgorithm());

    error.clear();
    QVERIFY2(vault.disablePasswordProtection(masterPassword, &error), qPrintable(error));
    QVERIFY(!vault.passwordProtectionEnabled());
    QCOMPARE(vault.state(&error), CredentialVault::State::Uninitialized);
    QVERIFY(!database.vaultConfiguration(&error).has_value());
    QCOMPARE(database.setting(QStringLiteral("security/vault_password_enabled"), &error)
                 .value_or(QString()),
             QStringLiteral("0"));
    QCOMPARE(database.findCredential(host.credentialId, &error)->kdfAlgorithm,
             QStringLiteral("local-key-v1"));

    // 新服务模拟应用重启：保险箱配置已经彻底清除，但主机密码必须仍可通过本机随机密钥用于
    // 自动连接。这同时证明关闭操作清除的是主密码保护，而不是用户保存的业务凭据。
    CredentialVault restarted(database);
    std::optional<QByteArray> localSecret = restarted.loadSecret(host.credentialId, &error);
    QVERIFY2(localSecret.has_value(), qPrintable(error));
    QCOMPARE(QString::fromUtf8(*localSecret), target.password);
    Vault::secureErase(*localSecret);
}

void SshCommandParserTest::credentialVaultResetPreservesHostConfiguration()
{
    ApplicationDatabase database(ApplicationDatabase::inMemoryPath());
    QString error;
    QVERIFY2(database.initialize(&error), qPrintable(error));

    CredentialVault vault(database);
    HostRecord host;
    host.id = QStringLiteral("forgotten-password-host");
    host.name = QStringLiteral("忘记密码后保留的主机");
    host.host = QStringLiteral("192.0.2.88");
    host.username = QStringLiteral("deploy");
    host.port = 2222;
    host.identityFile = QStringLiteral("/Users/example/.ssh/id_ed25519");
    host.tags = {QStringLiteral("生产"), QStringLiteral("关键")};
    ConnectionTarget target = host.toConnectionTarget();
    target.password = QStringLiteral("重置后必须消失的私钥口令");
    QVERIFY2(vault.saveConnectionPassword(host, target, {}, &error), qPrintable(error));
    QVERIFY(!host.credentialId.isEmpty());
    QVERIFY2(vault.enablePasswordProtection(
                 QStringLiteral("最终确实被忘记的主密码-2026"), &error),
             qPrintable(error));
    vault.lock();
    QCOMPARE(vault.state(&error), CredentialVault::State::Locked);

    QSignalSpy resetSpy(&vault, &CredentialVault::vaultReset);
    QVERIFY2(vault.resetPasswordProtection(&error), qPrintable(error));
    QCOMPARE(resetSpy.count(), 1);
    QVERIFY(!vault.passwordProtectionEnabled());
    QCOMPARE(vault.state(&error), CredentialVault::State::Uninitialized);
    QVERIFY(!database.vaultConfiguration(&error).has_value());
    QCOMPARE(database.setting(QStringLiteral("security/vault_password_enabled"), &error)
                 .value_or(QString()),
             QStringLiteral("0"));
    QVERIFY(!database.setting(QStringLiteral("security/local_credential_key"), &error)
                 .has_value());

    bool credentialsReadable = false;
    QVERIFY(database.listCredentials(&credentialsReadable, &error).isEmpty());
    QVERIFY2(credentialsReadable, qPrintable(error));

    const std::optional<HostRecord> preserved = database.findHost(host.id, &error);
    QVERIFY2(preserved.has_value(), qPrintable(error));
    QVERIFY(preserved->credentialId.isEmpty());
    QCOMPARE(preserved->name, host.name);
    QCOMPARE(preserved->host, host.host);
    QCOMPARE(preserved->username, host.username);
    QCOMPARE(preserved->port, host.port);
    QCOMPARE(preserved->identityFile, host.identityFile);
    QCOMPARE(preserved->tags, host.tags);
}

void SshCommandParserTest::credentialVaultSeparatesSharedCredentialBeforePasswordUpdate()
{
    ApplicationDatabase database(ApplicationDatabase::inMemoryPath());
    QString error;
    QVERIFY2(database.initialize(&error), qPrintable(error));
    CredentialVault vault(database);

    HostRecord first;
    first.id = QStringLiteral("shared-first");
    first.name = QStringLiteral("共享凭据主机一");
    first.host = QStringLiteral("192.0.2.41");
    ConnectionTarget original = first.toConnectionTarget();
    original.password = QStringLiteral("original-shared-secret");
    QVERIFY2(vault.saveConnectionPassword(first, original, {}, &error), qPrintable(error));

    HostRecord second = first;
    second.id = QStringLiteral("shared-second");
    second.name = QStringLiteral("共享凭据主机二");
    QVERIFY2(database.saveHost(second, &error), qPrintable(error));
    QCOMPARE(second.credentialId, first.credentialId);
    bool referencesReadable = false;
    QCOMPARE(database.credentialReferenceCount(
                 first.credentialId, &referencesReadable, &error),
             2);
    QVERIFY2(referencesReadable, qPrintable(error));

    ConnectionTarget updated = second.toConnectionTarget();
    updated.password = QStringLiteral("second-host-new-secret");
    QVERIFY2(vault.saveConnectionPassword(second, updated, {}, &error), qPrintable(error));
    QVERIFY(second.credentialId != first.credentialId);

    std::optional<QByteArray> firstSecret = vault.loadSecret(first.credentialId, &error);
    std::optional<QByteArray> secondSecret = vault.loadSecret(second.credentialId, &error);
    QVERIFY2(firstSecret.has_value() && secondSecret.has_value(), qPrintable(error));
    QCOMPARE(QString::fromUtf8(*firstSecret), QStringLiteral("original-shared-secret"));
    QCOMPARE(QString::fromUtf8(*secondSecret), QStringLiteral("second-host-new-secret"));
    Vault::secureErase(*firstSecret);
    Vault::secureErase(*secondSecret);
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

void SshCommandParserTest::terminalViewKeepsScrollPositionAcrossRender()
{
    TerminalView view;
    // 视图必须真实布局，否则滚动条范围恒为 0，用例会在任何实现下都"通过"。
    view.resize(600, 220);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    for (int line = 0; line < 200; ++line) {
        view.appendTerminalOutput(
            QStringLiteral("history line %1\r\n").arg(line).toUtf8());
    }
    QTest::qWait(kRenderFrameMilliseconds);
    QScrollBar *bar = view.verticalScrollBar();
    QVERIFY2(bar->maximum() > 0, "输出必须超过一屏，否则本用例无法覆盖滚动行为");

    // 场景一：停在底部时新输出到达，应继续跟随底部。VtermBridge::render 会整体重建
    // QTextDocument，滚动条随之归零；若实现依赖重建后尚未刷新的 maximum()，这里会掉到顶部。
    bar->setValue(bar->maximum());
    view.appendTerminalOutput(QByteArrayLiteral("tail follows\r\n"));
    QTest::qWait(kRenderFrameMilliseconds);
    QCOMPARE(bar->value(), bar->maximum());

    // 场景二：用户上翻查看历史时新输出到达，位置必须保持，不得被重建文档弹回顶部。
    const int middle = bar->maximum() / 2;
    QVERIFY2(middle > 0, "滚动范围过小，无法区分'保持原位'与'跳到顶部'");
    bar->setValue(middle);
    view.appendTerminalOutput(QByteArrayLiteral("more output\r\n"));
    QTest::qWait(kRenderFrameMilliseconds);
    QVERIFY2(bar->value() > 0,
             "阅读历史时收到新输出后被重置到了文档顶部");
    QCOMPARE(bar->value(), middle);
}

void SshCommandParserTest::localNoticeEndsWithCarriageReturn()
{
    TerminalView view;
    // 终端里的换行必须是 CRLF：LF 只下移一行而不回到第 0 列，随后的远端输出会从当前
    // 列继续绘制，表现为"Last login 前面有一大段空格"。
    view.appendTerminalOutput(QStringLiteral("正在连接 root@10.0.0.1…\r\n").toUtf8());
    view.appendTerminalOutput(QByteArrayLiteral("Last login: Sat Aug  8"));
    QTest::qWait(kRenderFrameMilliseconds);

    const QStringList renderedLines = view.terminalPlainText().split(QLatin1Char('\n'));
    QCOMPARE(renderedLines.size(), 2);
    // 只断言"不被推到光标列"，不比较整行：光标所在单元格会额外渲染出一个尾随空格，
    // 那是光标绘制的固有产物，与本用例要守住的行首对齐无关。
    QVERIFY2(!renderedLines.at(1).startsWith(QLatin1Char(' ')),
             "CRLF 之后的远端输出必须从第 0 列开始");
    QCOMPARE(renderedLines.at(1).trimmed(), QStringLiteral("Last login: Sat Aug  8"));

    // 仅用 LF 结尾会让下一行从原光标列开始，这正是需要杜绝的效果。
    TerminalView staircase;
    staircase.appendTerminalOutput(QStringLiteral("正在连接 root@10.0.0.1…\n").toUtf8());
    staircase.appendTerminalOutput(QByteArrayLiteral("Last login: Sat Aug  8"));
    const QStringList staircaseLines = staircase.terminalPlainText().split(QLatin1Char('\n'));
    QVERIFY2(staircaseLines.size() == 2 && staircaseLines.at(1).startsWith(QLatin1Char(' ')),
             "该断言固化 LF 的阶梯效应；若 libvterm 行为改变，说明上面的 CRLF 断言需要重新评估");
}

void SshCommandParserTest::terminalBackgroundIgnoresApplicationStyleSheet()
{
    // 主窗口的全局样式表用 palette(base) 描述终端背景，而它取到的是 QApplication 调色板的
    // Base——与终端主题背景差一档。结果是有字符的单元格用终端背景色、空白区用应用色，
    // 同一块终端里出现两种底色。这里用一个明显不同的颜色模拟该冲突。
    const QString previousStyleSheet = qApp->styleSheet();
    qApp->setStyleSheet(QStringLiteral(
        "QPlainTextEdit#TerminalView { background: #ff00ff; }"));

    TerminalSettings settings;
    settings.themeMode = ThemeMode::Dark;
    settings.colorSchemeId = QStringLiteral("default");
    const QColor expectedBackground = settings.normalized().effectivePalette().background;

    TerminalView view;
    view.resize(320, 160);
    view.applySettings(settings);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QCoreApplication::processEvents();

    const QImage rendered = view.grab().toImage();
    // 取右下角一块必定没有字符的区域：终端此时只有一行提示符，右下角只会是控件背景。
    const QColor emptyAreaColor = rendered.pixelColor(rendered.width() - 6,
                                                      rendered.height() - 6);
    qApp->setStyleSheet(previousStyleSheet);

    QCOMPARE(emptyAreaColor.name(), expectedBackground.name());
}

namespace {

/** @brief 递归收集某个布局（含嵌套子布局）直接管理的全部控件。 */
void collectLayoutWidgets(QLayout *layout, QSet<QWidget *> *managed)
{
    if (layout == nullptr) {
        return;
    }
    for (int index = 0; index < layout->count(); ++index) {
        QLayoutItem *item = layout->itemAt(index);
        if (QWidget *widget = item->widget()) {
            managed->insert(widget);
        }
        collectLayoutWidgets(item->layout(), managed);
    }
}

} // namespace

void SshCommandParserTest::vaultDialogHasNoStrayWidgets()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    ApplicationDatabase database(
        temporaryDirectory.filePath(QStringLiteral("vault-dialog.db")));
    QString error;
    QVERIFY2(database.initialize(&error), qPrintable(error));

    CredentialVault vault(database);
    QVERIFY2(vault.initialize(QStringLiteral("一条足够长的主密码-2026"), &error), qPrintable(error));
    vault.lock();
    QCOMPARE(vault.state(&error), CredentialVault::State::Locked);

    // VaultDialog 的构造函数是私有的，只能经 ensureUnlocked 弹出。对话框是模态的，因此
    // 在进入事件循环后取当前模态窗口来检查，再关掉它让 ensureUnlocked 返回。
    QString failure;
    // 新增的自绘徽标首次生成高 DPI pixmap 时可能初始化平台图形资源；offscreen 插件会在
    // 构造阶段短暂处理事件，因此给窗口完成 show/exec 留出一个很小的稳定宽限。
    QTimer::singleShot(50, [&failure]() {
        QWidget *dialog = QApplication::activeModalWidget();
        if (dialog == nullptr) {
            failure = QStringLiteral("解锁对话框未能弹出");
            return;
        }
        QSet<QWidget *> managed;
        collectLayoutWidgets(dialog->layout(), &managed);
        const QList<QWidget *> directChildren =
            dialog->findChildren<QWidget *>(Qt::FindDirectChildrenOnly);
        // QFocusFrame（macOS 焦点环）和 QSizeGrip 由 Qt 自行创建并围绕目标控件浮动定位，
        // 本就不该进入布局；把它们计入会让本用例随焦点时序随机失败。
        static const QSet<QString> qtFloatingHelpers = {
            QStringLiteral("QFocusFrame"), QStringLiteral("QSizeGrip")};
        for (QWidget *child : directChildren) {
            const QString className = QString::fromLatin1(child->metaObject()->className());
            // 显式隐藏的控件不绘制、也不参与 Tab 顺序，是可接受的"备用控件"形态。
            if (child->isHidden() || managed.contains(child)
                || qtFloatingHelpers.contains(className)) {
                continue;
            }
            failure = QStringLiteral("控件 %1 可见但不在任何布局中，会浮在对话框左上角")
                          .arg(QString::fromLatin1(child->metaObject()->className()));
            break;
        }

        auto *cancelButton = dialog->findChild<QAbstractButton *>(
            QStringLiteral("vaultCancelButton"));
        auto *confirmButton = dialog->findChild<QAbstractButton *>(
            QStringLiteral("vaultConfirmButton"));
        if (failure.isEmpty()
            && (cancelButton == nullptr || confirmButton == nullptr
                || cancelButton->geometry().left() >= confirmButton->geometry().left())) {
            failure = QStringLiteral("可取消的保险库对话框未按左侧取消、右侧主操作排列");
        }
        dialog->close();
    });

    // 解锁模式从对象树层面就不创建“确认主密码”输入框；测试仍遍历全部直接子控件，防止
    // 后续重构再次引入未被布局接管、会浮在左上角并意外接收主密码的可见控件。
    VaultDialog::ensureUnlocked(&vault, nullptr, true);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
}

void SshCommandParserTest::vaultDialogReportsWrongPasswordInline()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    ApplicationDatabase database(
        temporaryDirectory.filePath(QStringLiteral("vault-dialog-error.db")));
    QString error;
    QVERIFY2(database.initialize(&error), qPrintable(error));

    CredentialVault vault(database);
    QVERIFY2(vault.initialize(QStringLiteral("一条足够长的主密码-2026"), &error), qPrintable(error));
    vault.lock();

    QString failure;
    QTimer::singleShot(0, [&failure]() {
        QWidget *dialog = QApplication::activeModalWidget();
        if (dialog == nullptr) {
            failure = QStringLiteral("解锁对话框未能弹出");
            return;
        }
        // 使用稳定对象名定位真正参与解锁的密码框，避免测试依赖控件创建顺序；解锁模式不会
        // 创建确认密码框，因此任何后续新增的输入控件也不应改变本用例目标。
        QLineEdit *passwordEdit = dialog->findChild<QLineEdit *>(
            QStringLiteral("vaultPasswordEdit"));
        QAbstractButton *unlockButton = nullptr;
        for (QAbstractButton *button : dialog->findChildren<QAbstractButton *>()) {
            if (button->text() == QStringLiteral("Unlock")) {
                unlockButton = button;
            }
        }
        if (passwordEdit == nullptr || unlockButton == nullptr) {
            failure = QStringLiteral("解锁对话框缺少密码框或解锁按钮");
            dialog->close();
            return;
        }

        passwordEdit->setText(QStringLiteral("错误的主密码"));

        // 用 QMessageBox 报错会在解锁框之上再弹一个模态窗口：它挡住刚输错的密码框，用户
        // 必须先关掉它才能重试。这类弹窗自带事件循环，click() 会一直卡在里面，所以先挂一个
        // 单次定时器：它在弹窗的事件循环里执行，既记录违规也把弹窗关掉让 click() 返回。
        // 定时器在正确实现（没有嵌套弹窗）下会等当前回调返回后才执行，因此捕获栈上 bool
        // 会留下悬空引用，还可能在下一个模态测试开始后误关新窗口。共享状态负责跨事件循环
        // 生命周期，QPointer 则确保原解锁窗口销毁后排队检查直接失效。
        auto blockedByPopup = std::make_shared<bool>(false);
        const QPointer<QWidget> guardedDialog(dialog);
        QTimer::singleShot(0, [guardedDialog, blockedByPopup]() {
            if (guardedDialog == nullptr) {
                return;
            }
            QWidget *popup = QApplication::activeModalWidget();
            if (popup != nullptr && popup != guardedDialog) {
                *blockedByPopup = true;
                popup->close();
            }
        });
        unlockButton->click();

        if (*blockedByPopup) {
            failure = QStringLiteral("解锁失败后弹出了额外的模态窗口");
        } else {
            bool reported = false;
            for (const QLabel *label : dialog->findChildren<QLabel *>()) {
                if (label->isVisible() && label->text().contains(QStringLiteral("Could not unlock the vault"))) {
                    reported = true;
                }
            }
            if (!reported) {
                failure = QStringLiteral("解锁失败后对话框内没有可见的错误说明");
            } else if (!passwordEdit->property("validationError").toBool()) {
                failure = QStringLiteral("解锁失败后主密码字段没有进入红色错误描边状态");
            }
        }
        dialog->close();
    });

    VaultDialog::ensureUnlocked(&vault, nullptr, true);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    // 密码错误绝不能被当成解锁成功。
    QCOMPARE(vault.state(&error), CredentialVault::State::Locked);
}

void SshCommandParserTest::vaultDialogResetsForgottenPasswordAfterTypedConfirmation()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    ApplicationDatabase database(
        temporaryDirectory.filePath(QStringLiteral("vault-forgotten-password-reset.db")));
    QString error;
    QVERIFY2(database.initialize(&error), qPrintable(error));

    CredentialVault vault(database);
    HostRecord host;
    host.id = QStringLiteral("reset-dialog-host");
    host.name = QStringLiteral("重置确认测试主机");
    host.host = QStringLiteral("198.51.100.20");
    ConnectionTarget target = host.toConnectionTarget();
    target.password = QStringLiteral("待删除密码");
    QVERIFY2(vault.saveConnectionPassword(host, target, {}, &error), qPrintable(error));
    QVERIFY2(vault.enablePasswordProtection(
                 QStringLiteral("将被模拟遗忘的主密码-2026"), &error),
             qPrintable(error));
    vault.lock();

    QString failure;
    QTimer::singleShot(50, [&failure]() {
        QWidget *unlockDialog = QApplication::activeModalWidget();
        auto *forgotButton = unlockDialog == nullptr ? nullptr
                                                     : unlockDialog->findChild<QAbstractButton *>(
                                                           QStringLiteral(
                                                               "vaultForgotPasswordButton"));
        if (forgotButton == nullptr) {
            failure = QStringLiteral("解锁窗口缺少忘记密码入口");
            if (unlockDialog != nullptr) {
                unlockDialog->close();
            }
            return;
        }

        // forgotButton->click() 会进入第二层模态循环；预先排队的处理器在确认窗口显示后检查
        // 危险按钮默认禁用，再输入完整短语提交，真实覆盖防误触门槛。
        QTimer::singleShot(0, [&failure]() {
            QWidget *confirmationDialog = QApplication::activeModalWidget();
            auto *confirmationEdit = confirmationDialog == nullptr ? nullptr
                                                                    : confirmationDialog
                                                                          ->findChild<QLineEdit *>(
                                                                              QStringLiteral(
                                                                                  "vaultResetConfirmationEdit"));
            auto *resetButton = confirmationDialog == nullptr ? nullptr
                                                               : confirmationDialog
                                                                     ->findChild<QAbstractButton *>(
                                                                         QStringLiteral(
                                                                             "vaultResetConfirmButton"));
            if (confirmationEdit == nullptr || resetButton == nullptr) {
                failure = QStringLiteral("忘记密码入口没有显示输入确认短语的重置窗口");
                if (confirmationDialog != nullptr) {
                    confirmationDialog->close();
                }
                return;
            }
            if (resetButton->isEnabled()) {
                failure = QStringLiteral("未输入确认短语时永久删除按钮意外可用");
                confirmationDialog->close();
                return;
            }
            confirmationEdit->setText(QStringLiteral("Reset vault"));
            if (!resetButton->isEnabled()) {
                failure = QStringLiteral("输入完整确认短语后仍无法提交重置");
                confirmationDialog->close();
                return;
            }
            resetButton->click();
        });
        forgotButton->click();
    });

    QVERIFY(!VaultDialog::ensureUnlocked(&vault, nullptr, true));
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QCOMPARE(vault.state(&error), CredentialVault::State::Uninitialized);
    QVERIFY(!vault.passwordProtectionEnabled());
    QVERIFY(database.findHost(host.id, &error)->credentialId.isEmpty());
    bool credentialsReadable = false;
    QVERIFY(database.listCredentials(&credentialsReadable, &error).isEmpty());
    QVERIFY2(credentialsReadable, qPrintable(error));
}

void SshCommandParserTest::vaultCreationDialogShowsSecurityHierarchy()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    ApplicationDatabase database(
        temporaryDirectory.filePath(QStringLiteral("vault-dialog-create.db")));
    QString error;
    QVERIFY2(database.initialize(&error), qPrintable(error));

    CredentialVault vault(database);
    QCOMPARE(vault.state(&error), CredentialVault::State::Uninitialized);

    QString failure;
    // 首次生成锁形与警示图标的高 DPI pixmap 可能初始化平台图形资源；offscreen 插件会在
    // 构造阶段短暂处理事件，因此给窗口完成 show/exec 留出一个很小的稳定宽限。
    QTimer::singleShot(50, [&failure]() {
        QWidget *dialog = QApplication::activeModalWidget();
        if (dialog == nullptr) {
            // offscreen 平台在连续执行多个模态测试时可能尚未更新 activeModalWidget；顶层窗口
            // 列表仍已包含并显示目标对话框。按稳定对象名回退查找，避免把平台激活时序误判
            // 成产品窗口没有创建。真实界面断言仍只针对当前可见的 VaultDialog。
            for (QWidget *candidate : QApplication::topLevelWidgets()) {
                if (candidate->isVisible()
                    && candidate->objectName() == QStringLiteral("VaultDialog")) {
                    dialog = candidate;
                    break;
                }
            }
        }
        if (dialog == nullptr) {
            failure = QStringLiteral("保险库创建对话框未能弹出");
            return;
        }

        // 锁形徽标、居中标题和独立警示框共同建立“安全操作”层级。这里只检查稳定的对象语义
        // 与内容，不断言平台相关的字体像素和原生窗口边框，避免测试在 macOS/Linux 间漂移。
        const QLabel *badge = dialog->findChild<QLabel *>(
            QStringLiteral("VaultSecurityBadge"));
        const QLabel *heading = dialog->findChild<QLabel *>(
            QStringLiteral("VaultHeading"));
        const QWidget *warning = dialog->findChild<QWidget *>(
            QStringLiteral("VaultWarningNotice"));
        const QLabel *warningText = dialog->findChild<QLabel *>(
            QStringLiteral("VaultWarningText"));
        const QLineEdit *passwordEdit = dialog->findChild<QLineEdit *>(
            QStringLiteral("vaultPasswordEdit"));
        const QLineEdit *confirmationEdit = dialog->findChild<QLineEdit *>(
            QStringLiteral("vaultConfirmationEdit"));
        const QAbstractButton *cancelButton = dialog->findChild<QAbstractButton *>(
            QStringLiteral("vaultCancelButton"));

        if (badge == nullptr || badge->pixmap().isNull()
            || badge->size() != QSize(44, 44)) {
            failure = QStringLiteral("保险库创建对话框缺少 44px 锁形安全徽标");
        } else if (heading == nullptr
                   || heading->text() != QStringLiteral("Create vault")
                   || !(heading->alignment() & Qt::AlignHCenter)) {
            failure = QStringLiteral("保险库创建对话框缺少居中的操作标题");
        } else if (warning == nullptr || !warning->isVisible()
                   || warningText == nullptr
                   || !warningText->text().contains(QStringLiteral("The master password cannot be recovered"))) {
            failure = QStringLiteral("保险库创建对话框缺少独立的不可恢复警示");
        } else if (passwordEdit == nullptr || confirmationEdit == nullptr
                   || passwordEdit->placeholderText() != QStringLiteral("At least 8 characters")) {
            failure = QStringLiteral("保险库创建对话框没有完整呈现两次密码输入");
        } else if (cancelButton != nullptr) {
            // allowCancel=false 用于首次启动强制创建流程。按钮必须根本不存在，而不是仅禁用，
            // 否则键盘和辅助技术仍会把它当成一个无法执行的操作入口。
            failure = QStringLiteral("强制创建保险库时不应渲染取消按钮");
        }
        dialog->close();
    });

    // 测试通过窗口关闭结束模态循环，不会提交主密码，因此服务仍须保持未初始化。
    QVERIFY(!VaultDialog::ensureUnlocked(&vault, nullptr, false));
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QCOMPARE(vault.state(&error), CredentialVault::State::Uninitialized);
}

void SshCommandParserTest::terminalKeepsBottomAcrossCursorBlink()
{
    TerminalView view;
    view.resize(600, 220);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    for (int line = 0; line < 200; ++line) {
        view.appendTerminalOutput(QStringLiteral("line %1\r\n").arg(line).toUtf8());
    }
    QTest::qWait(kRenderFrameMilliseconds);
    QScrollBar *bar = view.verticalScrollBar();
    QVERIFY2(bar->maximum() > 0, "输出必须超过一屏，否则本用例无法覆盖滚动行为");
    bar->setValue(bar->maximum());
    QCOMPARE(bar->value(), bar->maximum());

    // 光标闪烁定时器每 600ms 触发一次 VtermBridge::render，而 render 会 document->clear()
    // 后整体重建，滚动条随之归零。用户什么都不做，视图也会自己跳回顶部——正是"输入命令
    // 时看不到自己输入内容"的成因。这里等待足够长的时间，让闪烁至少发生一次。
    QTest::qWait(800);
    QCOMPARE(bar->value(), bar->maximum());

    // 停在历史位置时同样不能被闪烁重建弹走。
    const int middle = bar->maximum() / 2;
    QVERIFY(middle > 0);
    bar->setValue(middle);
    QTest::qWait(800);
    QCOMPARE(bar->value(), middle);
}

QTEST_MAIN(SshCommandParserTest)

#include "SshCommandParserTest.moc"
