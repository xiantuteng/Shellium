#include "core/ConnectionTarget.h"
#include "sftp/SftpService.h"
#include "terminal/TerminalPane.h"
#include "terminal/TerminalView.h"
#include "ssh/ShellShim.h"
#include "ssh/SshSessionStack.h"
#include "platform/NativeSocket.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <limits>


/**
 * @brief 针对真实服务器的 SshSessionStack 连接路径回归测试。
 *
 * 非阻塞握手状态机无法用桩替身覆盖：它的价值恰恰在于与真实 libssh 和真实服务器的时序
 * 交互。因此本测试连接真实主机，并在缺少环境变量时整体跳过，保证默认构建和 CI 不依赖
 * 特定网络。
 *
 * 凭据只经由 SHELLIUM_IT_* 环境变量传入，绝不写入源码、日志或断言消息，避免测试产物
 * 成为凭据泄漏渠道。
 */
class SshSessionStackIntegrationTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    /** @brief 完整走通连接、认证、PTY 建立，并确认远端 Shell 真的在执行命令。 */
    void establishesRootLayerAndRunsCommand();

    /** @brief 错误密码必须内联索要重试，用户放弃后才报失败，且不得挂起到超时。 */
    void reportsAuthenticationFailurePromptly();

    /** @brief 不存在的主机必须快速失败，不得静默卡死。 */
    void reportsUnreachableHostPromptly();

    /** @brief 经第一层会话的 direct-tcpip 隧道抵达第二层，验证嵌套路径的传输基础。 */
    void opensForwardSocketThroughRootLayer();

    /** @brief 受管主机链应自动逐跳认证，只在最终目标就绪后对界面宣布连接成功。 */
    void establishesManagedHostChainDirectly();

    /** @brief 压入真实的第二层，在其中执行命令，再通过远端 exit 确定性地弹回第一层。 */
    void pushesAndPopsNestedLayer();

    /** @brief 内层目标不可达时只弹回外层，外层会话必须保持可用。 */
    void keepsOuterLayerAliveWhenNestedLayerFails();

    /** @brief 建层隧道尚未就绪时，外层终端必须依然可交互，不得被同步等待冻结。 */
    void keepsTerminalResponsiveWhileNestedLayerConnects();

    /** @brief 注入远端的 shim 必须能在真实 Shell 中定义成功并上报可解析的事件。 */
    void shellShimReportsSshInvocationFromRealShell();

    /**
     * @brief 接管之后 shim 必须继续挂起，直到被显式放行。
     *
     * shim 若在收到 take 后立刻返回，外层 Shell 会当场重画提示符，Shellium 紧接着写出的
     * password 提示前面就凭空多出一段旧提示符；行为上外层也不该在内层会话期间继续执行
     * 命令。这两点只有在真实 Shell 里才谈得上验证。
     */
    void shellShimSuspendsOuterShellUntilResumed();

    /**
     * @brief 服务器禁用 TCP 转发时，必须自动改用 exec 中继并照常建立内层会话。
     *
     * 只有真实的 AllowTcpForwarding no 服务器才能验证这条路径：桩替身无法复现
     * SSH_OPEN_ADMINISTRATIVELY_PROHIBITED 的时序与错误码。
     */
    void fallsBackToExecRelayWhenForwardingProhibited();

    /** @brief 第 2 层的 SFTP 必须经外层隧道抵达，而不是从本机直连内网地址。 */
    void routesNestedSftpThroughOuterLayer();

    /** @brief 空路径必须由服务端解析成该账号的主目录，而不是写死根目录。 */
    void resolvesHomeDirectoryForEmptyPath();

    /** @brief SFTP 连接池必须让后续操作免去重复握手，而不只是"看起来能用"。 */
    void reusesPooledSftpConnectionAcrossRequests();

    /** @brief 传输占用连接期间，目录浏览必须照常返回，否则用户在上传期间无法切换目录。 */
    void listsDirectoryWhileTransferHoldsConnection();

    /** @brief 无密码的嵌套目标必须索要凭据而非直接失败，且凭据要能传给 SFTP。 */
    void requestsCredentialForNestedLayerWithoutPassword();

    /**
     * @brief 首次登录必须在后台读出真实服务器的发行版，且与远端自报的 ID 完全一致。
     *
     * 探测走的是独立 exec 通道并与 Shell 集成安装串行，只有真实服务器才能验证它既不会
     * 抢占交互 Shell，也确实拿得到 /etc/os-release 的内容。
     */
    void detectsRemoteDistributionOnFirstLogin();

    /** @brief 取消一次认证后，下一次建层必须重新索要凭据而不是沿用遗留的空串。 */
    void doesNotReuseStaleCredentialAfterCancel();

    /**
     * @brief 端到端驱动真实 TerminalPane：内联提示 → 键盘输入密码 → 第二层建成。
     *
     * 这条路径此前只被拆成"栈"和"视图"两半分别验证过，中间的接线从未被端到端覆盖，
     * 而用户遇到的正是接线问题。
     */
    void terminalPaneCompletesNestedLoginFromKeyboard();

    /**
     * @brief 用户在终端里手打 ssh 的完整真实路径：shim 上报 → 接管 → 内联密码 → 建层。
     *
     * 这是用户实际操作的形态，也是唯一同时经过命令解析、shim 回执、接管与凭据接线的
     * 路径；分段验证全绿并不能保证它通。
     */
    void terminalPaneHandlesTypedSshEndToEnd();

    /** @brief Shellium 自己写给远端的字节不得出现在用户屏幕上。 */
    void suppressesOwnEchoInTerminal();

    /**
     * @brief 用户在远端敲 exit 主动结束 SSH 后，面板必须停在手动恢复提示。
     *
     * 正常退出曾被直接接回本地 Shell：刚断开的服务器与本机毫无关系，顶上来的本地进程
     * 会静默吞掉现场，用户下一条命令很可能打在自己电脑上。
     */
    void normalExitOffersManualRecoveryInsteadOfLocalShell();

    /**
     * @brief 拖拽上传须落到"此刻真实所在"的层与目录，而不是根连接或建连时的主目录。
     *
     * 压入真实第二层后在其中 cd 到一个已知目录，验证 TerminalPane::activeWorkingDirectory()
     * 跟随内层；弹回外层后必须换回外层自己此前记录的目录，而不是继续停留在已不再活动的
     * 内层路径上——后者正是拖拽上传功能会把文件传错服务器的根因。
     */
    void activeWorkingDirectoryTracksInnermostLayerAfterNestedPush();

private:
    /** @brief 从环境变量装配第一层目标。 */
    [[nodiscard]] static ConnectionTarget firstLayerTarget();

    /** @brief 从环境变量装配第二层目标；host 由第一层视角解析。 */
    [[nodiscard]] ConnectionTarget secondLayerTarget() const;

    /** @brief 反复处理事件直到断言条件成立或超时。 */
    static bool waitFor(const std::function<bool()> &predicate, int timeoutMs = 30000);

    bool m_configured = false;
    QString m_secondLayerHost;
};

void SshSessionStackIntegrationTest::initTestCase()
{
    const QByteArray host = qgetenv("SHELLIUM_IT_HOST");
    const QByteArray user = qgetenv("SHELLIUM_IT_USER");
    const QByteArray password = qgetenv("SHELLIUM_IT_PASSWORD");
    m_secondLayerHost = QString::fromUtf8(qgetenv("SHELLIUM_IT_HOST2"));
    m_configured = !host.isEmpty() && !user.isEmpty() && !password.isEmpty();
    if (!m_configured) {
        QSKIP("未设置 SHELLIUM_IT_HOST / SHELLIUM_IT_USER / SHELLIUM_IT_PASSWORD，跳过集成测试");
    }
}

ConnectionTarget SshSessionStackIntegrationTest::firstLayerTarget()
{
    ConnectionTarget target;
    target.id = QStringLiteral("integration-layer-1");
    target.host = QString::fromUtf8(qgetenv("SHELLIUM_IT_HOST"));
    target.username = QString::fromUtf8(qgetenv("SHELLIUM_IT_USER"));
    target.password = QString::fromUtf8(qgetenv("SHELLIUM_IT_PASSWORD"));
    const uint port = qgetenv("SHELLIUM_IT_PORT").toUInt();
    target.port = port == 0 ? 22 : static_cast<quint16>(port);
    target.displayName = target.connectionSpecifier();
    return target;
}

ConnectionTarget SshSessionStackIntegrationTest::secondLayerTarget() const
{
    ConnectionTarget target;
    target.id = QStringLiteral("integration-layer-2");
    target.host = m_secondLayerHost;
    target.username = QString::fromUtf8(qgetenv("SHELLIUM_IT_USER2"));
    target.password = QString::fromUtf8(qgetenv("SHELLIUM_IT_PASSWORD2"));
    const uint port = qgetenv("SHELLIUM_IT_PORT2").toUInt();
    target.port = port == 0 ? 22 : static_cast<quint16>(port);
    target.displayName = target.connectionSpecifier();
    return target;
}

bool SshSessionStackIntegrationTest::waitFor(const std::function<bool()> &predicate,
                                             const int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (predicate()) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QTest::qSleep(10);
    }
    return predicate();
}

void SshSessionStackIntegrationTest::establishesRootLayerAndRunsCommand()
{
    SshSessionStack stack(firstLayerTarget());
    QSignalSpy connected(&stack, &SshSessionStack::shellConnected);
    QSignalSpy failed(&stack, &SshSessionStack::connectionError);

    QByteArray output;
    connect(&stack, &SshSessionStack::outputReady, this, [&output](const QByteArray &bytes) {
        output.append(bytes);
    });

    stack.updateTerminalSize(120, 40);
    stack.start();

    QVERIFY2(waitFor([&]() { return connected.count() > 0 || failed.count() > 0; }),
             "连接既未成功也未报错，非阻塞状态机可能挂起");
    QVERIFY2(failed.isEmpty(),
             qPrintable(QStringLiteral("连接报错：%1")
                            .arg(failed.value(0).value(0).toString())));
    QCOMPARE(connected.count(), 1);

    // 让远端 Shell 完成启动并绘制首个提示符，避免命令被登录脚本的输出淹没。
    QVERIFY(waitFor([&]() { return !output.isEmpty(); }, 15000));

    // 用一个唯一标记确认这确实是一个能执行命令的交互式 Shell，而不仅仅是通道建立成功。
    const QByteArray marker = QByteArrayLiteral("SHELLIUM_PROBE_9137");
    output.clear();
    stack.enqueueInput("echo " + marker + "\n");
    QVERIFY2(waitFor([&]() { return output.count(marker) >= 2; }, 15000),
             "未在远端回显与命令输出中同时看到探针标记");

    stack.requestStop();
    QVERIFY(stack.wait(10000));
}

void SshSessionStackIntegrationTest::reportsAuthenticationFailurePromptly()
{
    ConnectionTarget target = firstLayerTarget();
    target.password = QStringLiteral("definitely-not-the-password-9137");

    SshSessionStack stack(target);
    QSignalSpy connected(&stack, &SshSessionStack::shellConnected);
    QSignalSpy failed(&stack, &SshSessionStack::connectionError);

    QElapsedTimer timer;
    timer.start();
    stack.start();

    QSignalSpy credentialAsked(&stack, &SshSessionStack::layerCredentialRequired);

    // 密码错误不再直接断开：与 OpenSSH 一致地在终端内联重试。输错一次就得从头重连是很差
    // 的体验，而且用户根本没机会更正。
    QVERIFY2(waitFor([&]() {
                 return credentialAsked.count() > 0 || failed.count() > 0
                     || connected.count() > 0;
             }),
             "错误密码既未索要重试也未报错，认证路径可能卡在 SSH_AUTH_AGAIN");
    QVERIFY2(connected.isEmpty(), "错误密码不应建立 Shell");
    QCOMPARE(credentialAsked.count(), 1);
    QVERIFY2(credentialAsked.at(0).at(1).toBool(),
             "首次提示应标记为重试：用户提供过密码且被拒绝了");
    QVERIFY2(timer.elapsed() < 28000, "索要重试耗时过长，疑似退化为等待整体超时");

    // 提交空串代表用户放弃，此时才应判定该层失败。
    stack.provideLayerCredential(QString());
    QVERIFY2(waitFor([&]() { return failed.count() > 0; }, 20000),
             "用户放弃后未报告连接失败");
    QVERIFY(connected.isEmpty());

    stack.requestStop();
    QVERIFY(stack.wait(10000));
}

void SshSessionStackIntegrationTest::reportsUnreachableHostPromptly()
{
    ConnectionTarget target = firstLayerTarget();
    // TEST-NET-1（RFC 5737）保证不可路由，用于观察连接阶段的超时行为。
    target.host = QStringLiteral("192.0.2.1");

    SshSessionStack stack(target);
    QSignalSpy connected(&stack, &SshSessionStack::shellConnected);
    QSignalSpy failed(&stack, &SshSessionStack::connectionError);
    stack.start();

    QVERIFY2(waitFor([&]() { return failed.count() > 0 || connected.count() > 0; }, 40000),
             "不可达主机未能在限时内失败，超时保护可能失效");
    QVERIFY(connected.isEmpty());
    QCOMPARE(failed.count(), 1);

    stack.requestStop();
    QVERIFY(stack.wait(10000));
}

void SshSessionStackIntegrationTest::opensForwardSocketThroughRootLayer()
{
    if (m_secondLayerHost.isEmpty()) {
        QSKIP("未设置 SHELLIUM_IT_HOST2，跳过隧道测试");
    }

    SshSessionStack stack(firstLayerTarget());
    QSignalSpy connected(&stack, &SshSessionStack::shellConnected);
    QSignalSpy failed(&stack, &SshSessionStack::connectionError);
    stack.start();

    QVERIFY(waitFor([&]() { return connected.count() > 0 || failed.count() > 0; }));
    QVERIFY2(failed.isEmpty(), "第一层连接失败，无法测试隧道");

    // openForwardSocket 会阻塞调用线程直到工作线程完成通道创建，因此必须在后台线程发起，
    // 同时让本线程继续泵送事件循环——这正是 SFTP 服务的真实调用形态。
    NativeSocketHandle tunnelFd = InvalidNativeSocket;
    QString tunnelError;
    bool done = false;
    auto *probe = QThread::create([&]() {
        tunnelFd = stack.openForwardSocket(QString(), m_secondLayerHost, 22, &tunnelError);
        done = true;
    });
    probe->start();

    QVERIFY2(waitFor([&]() { return done; }, 30000), "direct-tcpip 隧道请求未返回");
    probe->wait();
    delete probe;

    QVERIFY2(tunnelFd >= 0, qPrintable(QStringLiteral("隧道建立失败：%1").arg(tunnelError)));

    // 第二层的 SSH 服务端会主动送出版本横幅。读到它即证明字节确实穿过了第一层会话的
    // direct-tcpip 通道，而不只是本地 socketpair 创建成功。
    QByteArray banner;
    QVERIFY2(waitFor([&]() {
                 char buffer[256];
                 const qint64 count = readNativeSocket(tunnelFd, buffer, sizeof(buffer));
                 if (count > 0) {
                     banner.append(buffer, count);
                 }
                 return banner.contains("SSH-");
             }, 20000),
             "未从隧道另一端读到 SSH 版本横幅，隧道可能未真正连通");

    closeNativeSocket(tunnelFd);
    stack.requestStop();
    QVERIFY(stack.wait(10000));
}

void SshSessionStackIntegrationTest::establishesManagedHostChainDirectly()
{
    if (m_secondLayerHost.isEmpty()) {
        QSKIP("未设置 SHELLIUM_IT_HOST2，无法验证受管主机链");
    }

    const QList<ConnectionTarget> plan{firstLayerTarget(), secondLayerTarget()};
    SshSessionStack stack(plan, false, nullptr);
    QSignalSpy connectedSpy(&stack, &SshSessionStack::shellConnected);
    QSignalSpy layersSpy(&stack, &SshSessionStack::layersChanged);
    QSignalSpy errorSpy(&stack, &SshSessionStack::connectionError);
    stack.start();

    QVERIFY2(waitFor([&connectedSpy, &errorSpy]() {
                 return !connectedSpy.isEmpty() || !errorSpy.isEmpty();
             }),
             "受管主机链在时限内既未成功也未返回错误");
    QVERIFY2(errorSpy.isEmpty(),
             errorSpy.isEmpty() ? "" : qPrintable(errorSpy.constFirst().constFirst().toString()));
    QCOMPARE(connectedSpy.size(), 1);
    QVERIFY(!layersSpy.isEmpty());
    const QList<ConnectionTarget> layers =
        qvariant_cast<QList<ConnectionTarget>>(layersSpy.constLast().constFirst());
    QCOMPARE(layers.size(), 2);
    QVERIFY(layers.at(0).transit);
    QVERIFY(!layers.at(1).transit);
    QCOMPARE(layers.at(1).id, secondLayerTarget().id);

    stack.requestStop();
    QVERIFY(stack.wait(5000));
}

void SshSessionStackIntegrationTest::pushesAndPopsNestedLayer()
{
    if (m_secondLayerHost.isEmpty() || qgetenv("SHELLIUM_IT_PASSWORD2").isEmpty()) {
        QSKIP("未设置第二层服务器信息，跳过嵌套层测试");
    }

    SshSessionStack stack(firstLayerTarget());
    QSignalSpy connected(&stack, &SshSessionStack::shellConnected);
    QSignalSpy failed(&stack, &SshSessionStack::connectionError);
    QSignalSpy layerFailed(&stack, &SshSessionStack::layerFailed);

    QList<ConnectionTarget> layers;
    connect(&stack, &SshSessionStack::layersChanged, this,
            [&layers](const QList<ConnectionTarget> &snapshot) { layers = snapshot; });

    QByteArray output;
    connect(&stack, &SshSessionStack::outputReady, this,
            [&output](const QByteArray &bytes) { output.append(bytes); });

    stack.updateTerminalSize(120, 40);
    stack.start();

    QVERIFY(waitFor([&]() { return connected.count() > 0 || failed.count() > 0; }));
    QVERIFY2(failed.isEmpty(), "第一层连接失败");
    QVERIFY(waitFor([&]() { return layers.size() == 1; }));
    QCOMPARE(layers.at(0).confidence, ConnectionTarget::Confidence::Confirmed);

    // 压入第二层。传输必须经第一层的 direct-tcpip 通道建立，非阻塞握手期间工作线程要能
    // 回去泵送第一层，否则这里会挂到超时——这正是本用例最核心的回归点。
    output.clear();
    stack.requestPushLayer(secondLayerTarget());

    QVERIFY2(waitFor([&]() { return layers.size() == 2 || layerFailed.count() > 0; }, 40000),
             "压栈请求既未成功也未失败，嵌套握手疑似死锁");
    QVERIFY2(layerFailed.isEmpty(),
             qPrintable(QStringLiteral("嵌套层建立失败：%1")
                            .arg(layerFailed.value(0).value(1).toString())));
    QCOMPARE(layers.size(), 2);
    QCOMPARE(layers.at(1).host, m_secondLayerHost);
    QCOMPARE(layers.at(1).confidence, ConnectionTarget::Confidence::Confirmed);
    QCOMPARE(layers.at(1).parentId, layers.at(0).id);

    // 终端此时应当接在第二层：用 hostname 确认我们真的换了机器，而不只是多了一条记录。
    QVERIFY(waitFor([&]() { return !output.isEmpty(); }, 15000));
    output.clear();
    stack.enqueueInput("hostname -I 2>/dev/null || hostname\n");
    QVERIFY2(waitFor([&]() { return output.contains(m_secondLayerHost.toUtf8()); }, 20000),
             "第二层终端未回报第二层的地址，PTY 可能仍连在第一层");

    // 远端 exit 会让第二层 Shell 通道 EOF。栈据此确定性地弹出，不解析任何终端文本。
    stack.enqueueInput("exit\n");
    QVERIFY2(waitFor([&]() { return layers.size() == 1; }, 20000),
             "内层 exit 后未自动弹回第一层");
    QVERIFY2(failed.isEmpty(), "弹出内层不应影响外层会话");

    // 弹回后第一层必须仍然可用。
    output.clear();
    stack.enqueueInput("echo BACK_ON_LAYER_ONE\n");
    QVERIFY2(waitFor([&]() { return output.count("BACK_ON_LAYER_ONE") >= 2; }, 15000),
             "弹回第一层后终端不可交互");

    stack.requestStop();
    QVERIFY(stack.wait(10000));
}

void SshSessionStackIntegrationTest::keepsOuterLayerAliveWhenNestedLayerFails()
{
    SshSessionStack stack(firstLayerTarget());
    QSignalSpy connected(&stack, &SshSessionStack::shellConnected);
    QSignalSpy failed(&stack, &SshSessionStack::connectionError);
    QSignalSpy disconnected(&stack, &SshSessionStack::shellDisconnected);
    QSignalSpy layerFailed(&stack, &SshSessionStack::layerFailed);

    QByteArray output;
    connect(&stack, &SshSessionStack::outputReady, this,
            [&output](const QByteArray &bytes) { output.append(bytes); });

    stack.start();
    QVERIFY(waitFor([&]() { return connected.count() > 0 || failed.count() > 0; }));
    QVERIFY2(failed.isEmpty(), "第一层连接失败");
    QVERIFY(waitFor([&]() { return !output.isEmpty(); }, 15000));

    ConnectionTarget unreachable;
    unreachable.id = QStringLiteral("integration-layer-2-bad");
    // 用第一层自身的关闭端口：服务器立刻回 CHANNEL_OPEN_FAILURE，用例因此在一秒内完成。
    // 选择快速失败而非不可路由地址，是为了让这条回归常驻测试套件而不拖慢每次运行；
    // openForward 的 20 秒超时路径需要单独手工验证。
    unreachable.host = QStringLiteral("127.0.0.1");
    unreachable.port = 1;
    unreachable.username = QStringLiteral("nobody");
    unreachable.password = QStringLiteral("nobody");
    stack.requestPushLayer(unreachable);

    QVERIFY2(waitFor([&]() { return layerFailed.count() > 0; }, 60000),
             "不可达的内层目标未报告 layerFailed");
    QVERIFY2(failed.isEmpty(), "内层失败不得触发外层 connectionError");
    QVERIFY2(disconnected.isEmpty(), "内层失败不得断开整个分屏");

    // 外层必须仍然可交互——这正是"嵌套层失败只弹一层"的意义。
    output.clear();
    stack.enqueueInput("echo OUTER_STILL_ALIVE\n");
    QVERIFY2(waitFor([&]() { return output.count("OUTER_STILL_ALIVE") >= 2; }, 15000),
             "内层失败后外层会话不可用");

    stack.requestStop();
    QVERIFY(stack.wait(10000));
}

void SshSessionStackIntegrationTest::keepsTerminalResponsiveWhileNestedLayerConnects()
{
    SshSessionStack stack(firstLayerTarget());
    QSignalSpy connected(&stack, &SshSessionStack::shellConnected);
    QSignalSpy failed(&stack, &SshSessionStack::connectionError);
    QSignalSpy layerFailed(&stack, &SshSessionStack::layerFailed);

    QByteArray output;
    connect(&stack, &SshSessionStack::outputReady, this,
            [&output](const QByteArray &bytes) { output.append(bytes); });

    stack.start();
    QVERIFY(waitFor([&]() { return connected.count() > 0 || failed.count() > 0; }));
    QVERIFY2(failed.isEmpty(), "第一层连接失败");
    QVERIFY(waitFor([&]() { return !output.isEmpty(); }, 15000));

    ConnectionTarget stalling;
    stalling.id = QStringLiteral("integration-layer-2-stalling");
    // TEST-NET-1（RFC 5737）保证不可路由：第一层向它转发会长时间没有任何应答，
    // 从而稳定地制造出一个"隧道长时间在途"的窗口。
    stalling.host = QStringLiteral("192.0.2.1");
    stalling.username = QStringLiteral("nobody");
    stalling.password = QStringLiteral("nobody");
    stack.requestPushLayer(stalling);

    // 关键断言：隧道还挂着的时候，外层终端必须照常回显和执行命令。此前 servicePendingLayer
    // Requests 同步等待转发建立，这里会整整卡住 20 秒——用户看到的是整个分屏无响应。
    output.clear();
    stack.enqueueInput("echo STILL_RESPONSIVE\n");
    QVERIFY2(waitFor([&]() { return output.count("STILL_RESPONSIVE") >= 2; }, 10000),
             "建层隧道在途期间外层终端无响应，转发建立疑似退回了同步等待");
    QVERIFY2(layerFailed.isEmpty(), "此时隧道应当仍在途，不该已经得出结论");

    // 停止时必须唤醒并清理仍在途的建层请求，不能把线程留在那里等满超时。
    QElapsedTimer stopTimer;
    stopTimer.start();
    stack.requestStop();
    QVERIFY2(stack.wait(10000), "存在在途转发时停止超时");
    QVERIFY2(stopTimer.elapsed() < 9000, "停止耗时过长，疑似在等待在途转发自然超时");
}

void SshSessionStackIntegrationTest::shellShimReportsSshInvocationFromRealShell()
{
    SshSessionStack stack(firstLayerTarget());
    QSignalSpy connected(&stack, &SshSessionStack::shellConnected);
    QSignalSpy failed(&stack, &SshSessionStack::connectionError);

    QByteArray output;
    connect(&stack, &SshSessionStack::outputReady, this,
            [&output](const QByteArray &bytes) { output.append(bytes); });

    stack.start();
    QVERIFY(waitFor([&]() { return connected.count() > 0 || failed.count() > 0; }));
    QVERIFY2(failed.isEmpty(), "第一层连接失败");
    QVERIFY(waitFor([&]() { return !output.isEmpty(); }, 15000));

    // 注入前设置一条用户策略，再在真实 Shell 里执行 shim。除语法、base64 和引号问题外，
    // 还要确认注入不会像旧实现那样把 HISTCONTROL 无条件覆盖为 ignorespace。
    stack.enqueueInput("HISTCONTROL=erasedups\n");
    stack.enqueueInput(ShellShim::scriptBody() + "\n");
    output.clear();
    stack.enqueueInput("printf '<SHELLIUM_HC>%s</SHELLIUM_HC>\\n' \"$HISTCONTROL\"\n");
    QVERIFY2(waitFor([&]() {
                 return output.contains("<SHELLIUM_HC>erasedups:ignorespace</SHELLIUM_HC>");
             }, 15000),
             "shim 覆盖或丢失了用户已有的 HISTCONTROL 策略");
    output.clear();
    stack.enqueueInput("type ssh | head -1\n");
    QVERIFY2(waitFor([&]() { return output.contains("function"); }, 15000),
             "shim 未能在远端 Shell 中定义成 ssh 函数（可能是语法或引号转义问题）");

    // 触发一次 ssh：shim 应当发出 OSC 并阻塞等待回执。这里不接管，直接回 pass，
    // 从而验证降级路径也能让用户正常拿回原生 ssh。
    output.clear();
    stack.enqueueInput("ssh deploy@10.0.0.222\n");
    QVERIFY2(waitFor([&]() { return output.contains("\033]6773;connect;"); }, 15000),
             "shim 未上报 OSC 事件");

    // 从原始字节流里取出完整 OSC 载荷并解析，确认 argv 能被还原成真实目标。
    // 载荷从 OSC 序号之后开始：ESC ] 6 7 7 3 ; 共 7 字节，VtermBridge 交给上层的也正是
    // 这之后的部分。
    static const QByteArray oscPrefix = QByteArrayLiteral("\033]6773;");
    const qsizetype begin = output.indexOf(oscPrefix) + oscPrefix.size();
    const qsizetype end = output.indexOf('\007', begin);
    QVERIFY2(end > begin, "OSC 序列没有以 BEL 结束");
    const ShellShim::Event event = ShellShim::parseEvent(output.mid(begin, end - begin));
    QCOMPARE(event.kind, ShellShim::Event::Kind::Connect);
    QCOMPARE(event.arguments, QStringList{QStringLiteral("deploy@10.0.0.222")});

    QString reason;
    const ConnectionTarget resolved =
        ShellShim::resolveTarget(event.arguments, firstLayerTarget(), &reason);
    QVERIFY(resolved.isValid());
    QCOMPARE(resolved.host, QStringLiteral("10.0.0.222"));
    QCOMPARE(resolved.username, QStringLiteral("deploy"));

    // 回 pass 让 shim 交还给原生 ssh；随后终端必须恢复可交互，证明 read 确实被解除阻塞。
    stack.enqueueInput(ShellShim::declineResponse());
    output.clear();
    stack.enqueueInput("echo SHIM_RELEASED\n");
    QVERIFY2(waitFor([&]() { return output.contains("SHIM_RELEASED"); }, 30000),
             "回执后终端未恢复，shim 可能仍阻塞在 read 上");

    stack.requestStop();
    QVERIFY(stack.wait(10000));
}

void SshSessionStackIntegrationTest::shellShimSuspendsOuterShellUntilResumed()
{
    SshSessionStack stack(firstLayerTarget());
    QSignalSpy connected(&stack, &SshSessionStack::shellConnected);
    QSignalSpy failed(&stack, &SshSessionStack::connectionError);

    QByteArray output;
    connect(&stack, &SshSessionStack::outputReady, this,
            [&output](const QByteArray &bytes) { output.append(bytes); });

    stack.start();
    QVERIFY(waitFor([&]() { return connected.count() > 0 || failed.count() > 0; }));
    QVERIFY2(failed.isEmpty(), "第一层连接失败");
    QVERIFY(waitFor([&]() { return !output.isEmpty(); }, 15000));

    stack.enqueueInput(ShellShim::scriptBody() + "\n");
    output.clear();
    stack.enqueueInput("type ssh | head -1\n");
    QVERIFY2(waitFor([&]() { return output.contains("function"); }, 15000),
             "shim 未能在远端 Shell 中定义成 ssh 函数");

    // 第一轮：接管之后不放行，外层必须一直挂起。
    output.clear();
    stack.enqueueInput("ssh deploy@10.0.0.222\n");
    QVERIFY2(waitFor([&]() { return output.contains("\033]6773;connect;"); }, 15000),
             "shim 未上报 OSC 事件");
    output.clear();
    stack.enqueueInput(ShellShim::acceptResponse());

    // 这一行会被仍然阻塞的 read 当成普通输入吃掉，Shell 绝不会执行它。shim 若在 take 之后
    // 立刻返回，Shell 就会执行并打印出 SHIM_ASLEEP —— 那正是提示符被提前重画的病根。
    // 断言的是展开后的结果，命令自身的回显里不含该字符串。
    stack.enqueueInput("printf 'SHIM_%s\\n' ASLEEP\n");
    QTest::qWait(4000);
    QVERIFY2(!output.contains("SHIM_ASLEEP"),
             "take 之后外层 Shell 仍在执行命令：shim 没有挂起，提示符会被提前重画");

    // 第二轮：验证放行确实能让 shim 返回，外层重新取得提示符。
    output.clear();
    stack.enqueueInput("ssh deploy@10.0.0.222\n");
    QVERIFY2(waitFor([&]() { return output.contains("\033]6773;connect;"); }, 15000),
             "第二次 ssh 未上报 OSC 事件（上一轮的 shim 可能没能返回）");
    stack.enqueueInput(ShellShim::acceptResponse());
    QTest::qWait(500);
    stack.enqueueInput(ShellShim::resumeResponse());
    output.clear();
    stack.enqueueInput("printf 'SHIM_%s\\n' RESUMED\n");
    QVERIFY2(waitFor([&]() { return output.contains("SHIM_RESUMED"); }, 15000),
             "放行后外层 Shell 未恢复：shim 仍阻塞在 read 上");

    stack.requestStop();
    QVERIFY(stack.wait(10000));
}

void SshSessionStackIntegrationTest::reusesPooledSftpConnectionAcrossRequests()
{
    SftpService service;
    const ConnectionTarget target = firstLayerTarget();

    QSignalSpy listed(&service, &SftpService::directoryListed);
    QSignalSpy failed(&service, &SftpService::operationFailed);

    // 第一次请求包含完整握手：TCP、密钥交换、指纹校验、认证、sftp_init。
    QElapsedTimer coldTimer;
    coldTimer.start();
    service.listDirectory(target, QStringLiteral("/"));
    QVERIFY2(waitFor([&]() { return listed.count() > 0 || failed.count() > 0; }, 30000),
             "首次目录读取未返回");
    QVERIFY2(failed.isEmpty(),
             qPrintable(QStringLiteral("目录读取失败：%1")
                            .arg(failed.value(0).value(3).toString())));
    const qint64 coldMs = coldTimer.elapsed();

    // 后续请求应当直接复用同一条已认证连接。这里断言的是行为而非绝对耗时：只要没有重做
    // 握手，热请求必然显著快于冷请求。
    // 取最小值而不是平均值：要断言的是"没有重做握手"这一事实，最小值最能代表无握手时的
    // 开销。平均值会被网络抖动的单次尖峰拉高，让用例在链路不稳时假失败。
    qint64 warmBestMs = std::numeric_limits<qint64>::max();
    for (int round = 0; round < 5; ++round) {
        const int before = listed.count();
        QElapsedTimer warmTimer;
        warmTimer.start();
        service.listDirectory(target, QStringLiteral("/"));
        QVERIFY(waitFor([&]() { return listed.count() > before || failed.count() > 0; }, 30000));
        QVERIFY2(failed.isEmpty(), "复用池化连接时不应失败");
        warmBestMs = std::min(warmBestMs, warmTimer.elapsed());
    }

    qInfo("SFTP 目录读取：冷 %lldms，热最快 %lldms", coldMs, warmBestMs);
    // 阈值取一半而非某个绝对毫秒数：网络快慢因环境而异，但"省掉一整套握手"这个事实不变。
    QVERIFY2(warmBestMs * 2 < coldMs,
             qPrintable(QStringLiteral("热请求未显著快于冷请求（冷 %1ms，热最快 %2ms），"
                                       "连接池可能没有生效")
                            .arg(coldMs).arg(warmBestMs)));
}

void SshSessionStackIntegrationTest::listsDirectoryWhileTransferHoldsConnection()
{
    SftpService service;
    const ConnectionTarget target = firstLayerTarget();

    QSignalSpy listed(&service, &SftpService::directoryListed);
    QSignalSpy failed(&service, &SftpService::operationFailed);
    QSignalSpy succeeded(&service, &SftpService::operationSucceeded);
    QSignalSpy progressed(&service, &SftpService::transferProgress);

    service.listDirectory(target, QString());
    QVERIFY2(waitFor([&]() { return listed.count() > 0 || failed.count() > 0; }, 30000),
             "主目录读取未返回");
    QVERIFY2(failed.isEmpty(),
             qPrintable(QStringLiteral("主目录读取失败：%1")
                            .arg(failed.value(0).value(3).toString())));
    const QString home = listed.at(0).at(2).toString();

    // 文件必须大到能在传输途中被暂停：单块写完就结束的小文件根本走不到暂停边界。
    QTemporaryDir localDir;
    QVERIFY(localDir.isValid());
    const QString localPath = localDir.filePath(QStringLiteral("shellium-concurrency-probe.bin"));
    {
        QFile payload(localPath);
        QVERIFY(payload.open(QIODevice::WriteOnly));
        const QByteArray block(1 << 20, '\0');
        for (int index = 0; index < 64; ++index) {
            QCOMPARE(payload.write(block), static_cast<qint64>(block.size()));
        }
    }
    const QString remotePath = home.endsWith(QLatin1Char('/'))
        ? home + QStringLiteral("shellium-concurrency-probe.bin")
        : home + QStringLiteral("/shellium-concurrency-probe.bin");

    const quint64 uploadId =
        service.uploadFile(target, localPath, remotePath, SftpTransfer::ExistingFilePolicy::Overwrite);
    QVERIFY2(waitFor([&]() { return progressed.count() > 0 || failed.count() > 0; }, 60000),
             "上传未开始");
    QVERIFY2(failed.isEmpty(),
             qPrintable(QStringLiteral("上传失败：%1").arg(failed.value(0).value(3).toString())));

    // 暂停会让传输线程停在分块边界上，并持续占用它那条池化连接。用户在上传期间切换目录
    // 时遇到的正是这种"连接被占住"的状态，只不过那时占用它的是仍在写字节的循环。
    service.pauseRequest(uploadId);
    QVERIFY2(!waitFor([&]() { return succeeded.count() > 0; }, 3000),
             "上传在暂停前就已完成，用例无法覆盖传输占用连接的场景");

    const int listedBefore = listed.count();
    QElapsedTimer browseTimer;
    browseTimer.start();
    service.listDirectory(target, home);
    const bool browsed = waitFor(
        [&]() { return listed.count() > listedBefore || failed.count() > 0; }, 30000);

    // 先恢复并取消，避免断言失败时把暂停中的传输线程留在后台。
    service.resumeRequest(uploadId);
    service.cancelRequest(uploadId);
    waitFor([&]() { return succeeded.count() > 0 || failed.count() > 0; }, 30000);
    const int succeededBeforeCleanup = succeeded.count();
    service.removePath(target, remotePath, false);
    waitFor([&]() { return succeeded.count() > succeededBeforeCleanup; }, 30000);

    QVERIFY2(browsed,
             "传输占用连接期间目录浏览没有返回：浏览与传输必须走各自独立的连接");
    qInfo("传输占用连接期间的目录浏览耗时：%lldms", browseTimer.elapsed());
}

void SshSessionStackIntegrationTest::requestsCredentialForNestedLayerWithoutPassword()
{
    if (m_secondLayerHost.isEmpty() || qgetenv("SHELLIUM_IT_PASSWORD2").isEmpty()) {
        QSKIP("未设置第二层服务器信息，跳过凭据流程测试");
    }

    SshSessionStack stack(firstLayerTarget());
    QSignalSpy connected(&stack, &SshSessionStack::shellConnected);
    QSignalSpy failed(&stack, &SshSessionStack::connectionError);
    QSignalSpy layerFailed(&stack, &SshSessionStack::layerFailed);
    QSignalSpy credentialAsked(&stack, &SshSessionStack::layerCredentialRequired);

    QList<ConnectionTarget> layers;
    connect(&stack, &SshSessionStack::layersChanged, this,
            [&layers](const QList<ConnectionTarget> &snapshot) { layers = snapshot; });

    stack.start();
    QVERIFY(waitFor([&]() { return connected.count() > 0 || failed.count() > 0; }));
    QVERIFY2(failed.isEmpty(), "第一层连接失败");

    // 这正是 shim 接管时的形态：ShellShim::resolveTarget 只能从命令行还原出主机和用户名，
    // 拿不到密码。此前这里会直接判定认证失败，并连带让文件管理器弹出密码对话框。
    ConnectionTarget passwordless = secondLayerTarget();
    passwordless.password.clear();
    stack.requestPushLayer(passwordless);

    QVERIFY2(waitFor([&]() {
                 return credentialAsked.count() > 0 || layerFailed.count() > 0
                     || layers.size() == 2;
             }, 40000),
             "压栈既未索要凭据也未得出结论");
    QVERIFY2(layerFailed.isEmpty(),
             qPrintable(QStringLiteral("不应直接失败，应当索要密码：%1")
                            .arg(layerFailed.value(0).value(1).toString())));
    QCOMPARE(credentialAsked.count(), 1);
    // 首次索要不得标记为重试，否则终端里会出现莫名其妙的 "Permission denied"。
    QCOMPARE(credentialAsked.at(0).at(1).toBool(), false);

    stack.provideLayerCredential(QString::fromUtf8(qgetenv("SHELLIUM_IT_PASSWORD2")));
    QVERIFY2(waitFor([&]() { return layers.size() == 2 || layerFailed.count() > 0; }, 40000),
             "提交密码后未能建立第二层");
    QVERIFY2(layerFailed.isEmpty(), "提交正确密码后不应失败");
    QCOMPARE(layers.size(), 2);

    // 凭据必须随快照交给文件管理器，否则 SFTP 会再弹一次密码框——这正是用户看到两个
    // 对话框叠在一起的根因。
    QVERIFY2(!layers.at(1).password.isEmpty(),
             "第二层快照未携带用户刚输入的密码，SFTP 将被迫重新询问");

    stack.requestStop();
    QVERIFY(stack.wait(10000));
}

void SshSessionStackIntegrationTest::doesNotReuseStaleCredentialAfterCancel()
{
    if (m_secondLayerHost.isEmpty() || qgetenv("SHELLIUM_IT_PASSWORD2").isEmpty()) {
        QSKIP("未设置第二层服务器信息，跳过陈旧凭据测试");
    }

    SshSessionStack stack(firstLayerTarget());
    QSignalSpy connected(&stack, &SshSessionStack::shellConnected);
    QSignalSpy failed(&stack, &SshSessionStack::connectionError);
    QSignalSpy layerFailed(&stack, &SshSessionStack::layerFailed);
    QSignalSpy credentialAsked(&stack, &SshSessionStack::layerCredentialRequired);

    QList<ConnectionTarget> layers;
    connect(&stack, &SshSessionStack::layersChanged, this,
            [&layers](const QList<ConnectionTarget> &snapshot) { layers = snapshot; });

    stack.start();
    QVERIFY(waitFor([&]() { return connected.count() > 0 || failed.count() > 0; }));
    QVERIFY2(failed.isEmpty(), "第一层连接失败");

    ConnectionTarget passwordless = secondLayerTarget();
    passwordless.password.clear();

    // 第一次：索要凭据后用空串取消，模拟用户在密码提示处按 Ctrl+C。
    stack.requestPushLayer(passwordless);
    QVERIFY(waitFor([&]() { return credentialAsked.count() > 0; }, 40000));
    stack.provideLayerCredential(QString());
    QVERIFY2(waitFor([&]() { return layerFailed.count() > 0; }, 30000),
             "取消认证后未报告该层失败");

    // 第二次：必须重新索要凭据。此前遗留的空串会被新层立刻消费，表现为"再也连不上"——
    // 用户每敲一次 ssh 就立刻收到一次"用户取消了认证"，且无从察觉原因。
    stack.requestPushLayer(passwordless);
    QVERIFY2(waitFor([&]() {
                 return credentialAsked.count() > 1 || layerFailed.count() > 1;
             }, 40000),
             "第二次建层既未索要凭据也未失败");
    QVERIFY2(layerFailed.count() == 1,
             qPrintable(QStringLiteral("第二次建层不应直接失败，应重新索要密码：%1")
                            .arg(layerFailed.value(1).value(1).toString())));
    QCOMPARE(credentialAsked.count(), 2);

    // 这次给出正确密码，必须能真正建成。
    stack.provideLayerCredential(QString::fromUtf8(qgetenv("SHELLIUM_IT_PASSWORD2")));
    QVERIFY2(waitFor([&]() { return layers.size() == 2 || layerFailed.count() > 1; }, 40000),
             "提交正确密码后未能建立第二层");
    QCOMPARE(layers.size(), 2);

    stack.requestStop();
    QVERIFY(stack.wait(10000));
}

void SshSessionStackIntegrationTest::terminalPaneCompletesNestedLoginFromKeyboard()
{
    if (m_secondLayerHost.isEmpty() || qgetenv("SHELLIUM_IT_PASSWORD2").isEmpty()) {
        QSKIP("未设置第二层服务器信息，跳过端到端键盘测试");
    }

    TerminalPane pane;
    pane.resize(900, 500);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));

    auto *view = pane.findChild<TerminalView *>();
    QVERIFY2(view != nullptr, "未能取得面板内的终端视图");
    view->setFocus(Qt::OtherFocusReason);

    QSignalSpy targetsSpy(&pane, &TerminalPane::targetsChanged);
    pane.connectToHost(firstLayerTarget());
    QVERIFY2(waitFor([&]() { return pane.isConnected(); }, 40000), "第一层连接失败");
    QVERIFY(waitFor([&]() { return pane.targets().size() == 1; }, 20000));

    // 模拟 shim 接管：目标不带密码，正如从命令行还原出来的那样。
    ConnectionTarget passwordless = secondLayerTarget();
    passwordless.password.clear();
    pane.connectNestedHost(passwordless);

    // 面板应当进入本地行捕获，等待用户在终端里输入密码。
    QVERIFY2(waitFor([&]() { return view->isCapturingLocalLine(); }, 40000),
             "未进入本地密码捕获：内联提示流程没有接通");

    const QString password = QString::fromUtf8(qgetenv("SHELLIUM_IT_PASSWORD2"));
    QTest::keyClicks(view, password);
    QTest::keyClick(view, Qt::Key_Return);

    QVERIFY2(waitFor([&]() { return pane.targets().size() == 2; }, 40000),
             "键盘输入密码后第二层仍未建成");
    const QList<ConnectionTarget> chain = pane.targets();
    QCOMPARE(chain.size(), 2);
    QCOMPARE(chain.at(1).host, m_secondLayerHost);
    QCOMPARE(chain.at(1).confidence, ConnectionTarget::Confidence::Confirmed);
    QVERIFY2(!chain.at(1).password.isEmpty(),
             "第二层未携带刚输入的密码，SFTP 会被迫再问一次");
}

void SshSessionStackIntegrationTest::terminalPaneHandlesTypedSshEndToEnd()
{
    if (m_secondLayerHost.isEmpty() || qgetenv("SHELLIUM_IT_PASSWORD2").isEmpty()) {
        QSKIP("未设置第二层服务器信息，跳过手打 ssh 端到端测试");
    }

    TerminalPane pane;
    pane.resize(900, 500);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));

    auto *view = pane.findChild<TerminalView *>();
    QVERIFY(view != nullptr);
    view->setFocus(Qt::OtherFocusReason);

    pane.connectToHost(firstLayerTarget());
    QVERIFY2(waitFor([&]() { return pane.isConnected(); }, 40000), "第一层连接失败");
    QVERIFY(waitFor([&]() { return pane.targets().size() == 1; }, 20000));

    // shim 在层就绪 300ms 后注入；再留出一轮往返让远端 Shell 真正定义好函数。
    QTest::qWait(2500);

    // 完全按用户的操作来：在终端里逐字敲出 ssh 命令并回车。
    QTest::keyClicks(view, QStringLiteral("ssh root@") + m_secondLayerHost);
    QTest::keyClick(view, Qt::Key_Return);

    QVERIFY2(waitFor([&]() { return view->isCapturingLocalLine(); }, 40000),
             "手打 ssh 后未进入本地密码捕获：shim 接管或凭据接线断了");

    QTest::keyClicks(view, QString::fromUtf8(qgetenv("SHELLIUM_IT_PASSWORD2")));
    QTest::keyClick(view, Qt::Key_Return);

    QVERIFY2(waitFor([&]() { return pane.targets().size() == 2; }, 40000),
             "输入密码后第二层仍未建成");
    const QList<ConnectionTarget> chain = pane.targets();
    QCOMPARE(chain.at(1).host, m_secondLayerHost);
    QCOMPARE(chain.at(1).confidence, ConnectionTarget::Confidence::Confirmed);
}

void SshSessionStackIntegrationTest::normalExitOffersManualRecoveryInsteadOfLocalShell()
{
    TerminalPane pane;
    pane.resize(900, 500);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));

    auto *view = pane.findChild<TerminalView *>();
    QVERIFY(view != nullptr);
    view->setFocus(Qt::OtherFocusReason);

    pane.connectToHost(firstLayerTarget());
    QVERIFY2(waitFor([&]() { return pane.isConnected(); }, 40000), "第一层连接失败");
    QVERIFY(waitFor([&]() { return pane.targets().size() == 1; }, 20000));
    QTest::qWait(2500);

    // 完全按用户的操作来：在远端 Shell 里敲 exit 主动结束这条 SSH。
    QTest::keyClicks(view, QStringLiteral("exit"));
    QTest::keyClick(view, Qt::Key_Return);

    QVERIFY2(waitFor([&]() { return pane.isAwaitingManualRecovery(); }, 40000),
             "远端 exit 后未进入手动恢复提示");
    QVERIFY2(!pane.isConnected(), "提示状态下不应存在可交互会话");

    const QString screen = pane.terminalPlainText();
    QVERIFY2(screen.contains(QStringLiteral("Press r to reconnect")),
             "结束后必须给出手动重连按键");
    QVERIFY2(screen.contains(QStringLiteral("press x to close this tab")),
             "结束后必须给出关闭标签页按键");
    QVERIFY2(!screen.contains(QStringLiteral("Starting local terminal")),
             "主动结束 SSH 后不得擅自接回本地 Shell");
}

void SshSessionStackIntegrationTest::suppressesOwnEchoInTerminal()
{
    if (m_secondLayerHost.isEmpty() || qgetenv("SHELLIUM_IT_PASSWORD2").isEmpty()) {
        QSKIP("未设置第二层服务器信息，跳过回显抑制测试");
    }

    TerminalPane pane;
    pane.resize(1100, 600);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));

    auto *view = pane.findChild<TerminalView *>();
    QVERIFY(view != nullptr);
    view->setFocus(Qt::OtherFocusReason);

    pane.connectToHost(firstLayerTarget());
    QVERIFY2(waitFor([&]() { return pane.isConnected(); }, 40000), "第一层连接失败");
    QVERIFY(waitFor([&]() { return pane.targets().size() == 1; }, 20000));
    QTest::qWait(2500);

    QTest::keyClicks(view, QStringLiteral("ssh root@") + m_secondLayerHost);
    QTest::keyClick(view, Qt::Key_Return);
    QVERIFY2(waitFor([&]() { return view->isCapturingLocalLine(); }, 40000),
             "未进入密码捕获");
    QTest::keyClicks(view, QString::fromUtf8(qgetenv("SHELLIUM_IT_PASSWORD2")));
    QTest::keyClick(view, Qt::Key_Return);
    QVERIFY2(waitFor([&]() { return pane.targets().size() == 2; }, 40000), "第二层未建成");
    QTest::qWait(2500);

    const QString screen = view->toPlainText();

    // take/pass 是 Shellium 与 shim 之间的内部协议文本，用户不该看到。
    QVERIFY2(!screen.contains(QStringLiteral("take")),
             "终端里出现了发给 shim 的 take 回执");

    // 注入行是本程序写入的 Shell 代码，同样不该出现在屏幕上。抽取几段特征串检查。
    for (const QString &fragment : {QStringLiteral("_shellium_"),
                                    QStringLiteral("HISTCONTROL=ignorespace"),
                                    QStringLiteral("BASH_VERSION")}) {
        QVERIFY2(!screen.contains(fragment),
                 qPrintable(QStringLiteral("终端里出现了注入行片段：%1").arg(fragment)));
    }

    // source 命令是本程序注入的，同样一个字都不该出现。它在 readline 接管 PTY 之前抵达
    // 时会被远端回显两次，只扣一次就会在屏幕上留下一条 `. /tmp/.shm-xxxxxxxx`。
    QVERIFY2(!screen.contains(QStringLiteral("/tmp/.shm-")),
             qPrintable(QStringLiteral("终端里出现了 Shell 集成的 source 命令：[%1]")
                            .arg(screen)));

    // 真正的远端输出必须完好——过滤不能把用户内容一起吞掉。
    QVERIFY2(screen.contains(QStringLiteral("ssh root@") + m_secondLayerHost),
             "用户自己敲的命令被过滤掉了");

    // 密码提示必须紧贴用户敲的那一行。中间若冒出空行，说明 take 回执的换行没被扣掉；
    // 若冒出一段外层提示符，说明 shim 在接管后立刻返回、Shell 重画了提示符。
    const QStringList lines = screen.split(QLatin1Char('\n'));
    const QString command = QStringLiteral("ssh root@") + m_secondLayerHost;
    const qsizetype commandLine = [&]() {
        for (qsizetype index = 0; index < lines.size(); ++index) {
            if (lines.at(index).contains(command)) {
                return index;
            }
        }
        return qsizetype(-1);
    }();
    QVERIFY(commandLine >= 0 && commandLine + 1 < lines.size());
    const QString promptLine = lines.at(commandLine + 1);
    QVERIFY2(promptLine.startsWith(QStringLiteral("root@") + m_secondLayerHost
                                   + QStringLiteral("'s password:")),
             qPrintable(QStringLiteral("ssh 命令下一行不是密码提示，而是：[%1]")
                            .arg(promptLine)));
}

void SshSessionStackIntegrationTest::resolvesHomeDirectoryForEmptyPath()
{
    SftpService service;
    QSignalSpy listed(&service, &SftpService::directoryListed);
    QSignalSpy failed(&service, &SftpService::operationFailed);

    // 传空路径：服务端应当把 "." 规范化成该账号的主目录绝对路径。首次进入一台服务器时
    // 无从得知它的主目录，写死 / 既不符合直觉，也常常没有列目录权限。
    service.listDirectory(firstLayerTarget(), QString());
    QVERIFY2(waitFor([&]() { return listed.count() > 0 || failed.count() > 0; }, 30000),
             "主目录读取未返回");
    QVERIFY2(failed.isEmpty(),
             qPrintable(QStringLiteral("读取失败：%1").arg(failed.value(0).value(3).toString())));

    const QString resolved = listed.at(0).at(2).toString();
    QVERIFY2(resolved.startsWith(QLatin1Char('/')),
             qPrintable(QStringLiteral("返回的路径不是绝对路径：%1").arg(resolved)));
    // 必须回传解析结果，界面据此更新路径框；否则用户看不到自己在哪。
    QVERIFY2(!resolved.isEmpty(), "未回传解析后的主目录");
    qInfo("主目录解析为：%s", qPrintable(resolved));

    // 显式绝对路径仍按原样使用。
    const int before = listed.count();
    service.listDirectory(firstLayerTarget(), QStringLiteral("/"));
    QVERIFY(waitFor([&]() { return listed.count() > before || failed.count() > 0; }, 30000));
    QVERIFY(failed.isEmpty());
    QCOMPARE(listed.at(before).at(2).toString(), QStringLiteral("/"));
}

void SshSessionStackIntegrationTest::fallsBackToExecRelayWhenForwardingProhibited()
{
    const QByteArray host = qgetenv("SHELLIUM_IT_NOFWD_HOST");
    const QByteArray inner = qgetenv("SHELLIUM_IT_NOFWD_INNER");
    if (host.isEmpty() || inner.isEmpty()) {
        QSKIP("未设置 SHELLIUM_IT_NOFWD_*，跳过禁用转发服务器的中继测试");
    }

    ConnectionTarget outer;
    outer.id = QStringLiteral("integration-nofwd-outer");
    outer.host = QString::fromUtf8(host);
    outer.username = QString::fromUtf8(qgetenv("SHELLIUM_IT_NOFWD_USER"));
    outer.password = QString::fromUtf8(qgetenv("SHELLIUM_IT_NOFWD_PASSWORD"));
    const uint outerPort = qgetenv("SHELLIUM_IT_NOFWD_PORT").toUInt();
    outer.port = outerPort == 0 ? 22 : static_cast<quint16>(outerPort);

    SshSessionStack stack(outer);
    QSignalSpy connected(&stack, &SshSessionStack::shellConnected);
    QSignalSpy failed(&stack, &SshSessionStack::connectionError);
    stack.start();
    QVERIFY(waitFor([&]() { return connected.count() > 0 || failed.count() > 0; }, 40000));
    QVERIFY2(failed.isEmpty(), "外层连接失败");

    // 该服务器的 direct-tcpip 会被按策略拒绝；实现应当自动改用 exec 中继，调用方无感。
    const uint innerPort = qgetenv("SHELLIUM_IT_NOFWD_INNER_PORT").toUInt();
    NativeSocketHandle tunnelFd = InvalidNativeSocket;
    QString tunnelError;
    bool done = false;
    auto *probe = QThread::create([&]() {
        tunnelFd = stack.openForwardSocket(QString(),
                                           QString::fromUtf8(inner),
                                           innerPort == 0 ? 22
                                                          : static_cast<quint16>(innerPort),
                                           &tunnelError);
        done = true;
    });
    probe->start();
    QVERIFY2(waitFor([&]() { return done; }, 40000), "中继请求未返回");
    probe->wait();
    delete probe;

    QVERIFY2(tunnelFd >= 0,
             qPrintable(QStringLiteral("中继建立失败：%1").arg(tunnelError)));

    // 读到内层 sshd 的版本横幅，才说明字节确实穿过了中继抵达目标——仅仅建成 socketpair
    // 什么也证明不了。
    QByteArray banner;
    QVERIFY2(waitFor([&]() {
                 char buffer[256];
                 const qint64 count = readNativeSocket(tunnelFd, buffer, sizeof(buffer));
                 if (count > 0) {
                     banner.append(buffer, count);
                 }
                 return banner.contains("SSH-");
             }, 30000),
             qPrintable(QStringLiteral("未从中继另一端读到 SSH 横幅，收到：%1")
                            .arg(QString::fromUtf8(banner.left(200)))));
    qInfo("经 exec 中继读到内层横幅：%s", banner.trimmed().left(60).constData());

    closeNativeSocket(tunnelFd);
    stack.requestStop();
    QVERIFY(stack.wait(10000));
}

void SshSessionStackIntegrationTest::routesNestedSftpThroughOuterLayer()
{
    if (m_secondLayerHost.isEmpty() || qgetenv("SHELLIUM_IT_PASSWORD2").isEmpty()) {
        QSKIP("未设置第二层服务器信息，跳过内层 SFTP 路由测试");
    }

    TerminalPane pane;
    pane.resize(900, 500);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));

    pane.connectToHost(firstLayerTarget());
    QVERIFY2(waitFor([&]() { return pane.isConnected(); }, 40000), "第一层连接失败");
    QVERIFY(waitFor([&]() { return pane.targets().size() == 1; }, 20000));

    pane.connectNestedHost(secondLayerTarget());
    QVERIFY2(waitFor([&]() { return pane.targets().size() == 2; }, 40000), "第二层未建成");

    const ConnectionTarget nested = pane.targets().at(1);
    // 没有 tunnelRouteId，SFTP 就会拿着内网地址从本机直连——而本机很可能就在同一个私有
    // 网段上，于是连到局域网里另一台碰巧同 IP 的机器，在错误的服务器上列目录甚至删文件。
    QVERIFY2(!nested.tunnelRouteId.isEmpty(),
             "自持的第 2 层未登记隧道路由，SFTP 会绕过外层直连内网地址");

    // 真正走一次 SFTP：能列出目录才说明隧道确实通到了第 2 层。
    SftpService service;
    QSignalSpy listed(&service, &SftpService::directoryListed);
    QSignalSpy failed(&service, &SftpService::operationFailed);
    service.listDirectory(nested, QString());
    QVERIFY2(waitFor([&]() { return listed.count() > 0 || failed.count() > 0; }, 40000),
             "内层 SFTP 请求未返回");
    QVERIFY2(failed.isEmpty(),
             qPrintable(QStringLiteral("内层 SFTP 失败：%1")
                            .arg(failed.value(0).value(3).toString())));
    qInfo("第 2 层主目录解析为：%s", qPrintable(listed.at(0).at(2).toString()));
}

void SshSessionStackIntegrationTest::activeWorkingDirectoryTracksInnermostLayerAfterNestedPush()
{
    if (m_secondLayerHost.isEmpty() || qgetenv("SHELLIUM_IT_PASSWORD2").isEmpty()) {
        QSKIP("未设置第二层服务器信息，跳过嵌套层工作目录跟踪测试");
    }

    TerminalPane pane;
    pane.resize(900, 500);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));

    pane.connectToHost(firstLayerTarget());
    QVERIFY2(waitFor([&]() { return pane.isConnected(); }, 40000), "第一层连接失败");
    QVERIFY(waitFor([&]() { return pane.targets().size() == 1; }, 20000));

    // 用 cd 制造一个与登录主目录不同的已知位置，避免"恰好没变"掩盖了跟踪逻辑没生效。
    pane.executeCommand(QStringLiteral("cd /tmp"));
    QVERIFY2(waitFor([&]() { return pane.activeWorkingDirectory() == QStringLiteral("/tmp"); }, 15000),
             "第一层 cd 后 activeWorkingDirectory 未跟上新的工作目录");

    pane.connectNestedHost(secondLayerTarget());
    QVERIFY2(waitFor([&]() { return pane.targets().size() == 2; }, 40000), "第二层未建成");

    pane.executeCommand(QStringLiteral("cd /var"));
    QVERIFY2(waitFor([&]() { return pane.activeWorkingDirectory() == QStringLiteral("/var"); }, 15000),
             "压入第二层后 activeWorkingDirectory 未跟随内层 cd，拖拽上传会落到错误的层");

    // 内层退出后必须换回外层自己此前记录的位置，而不是继续停留在已不再活动的内层路径上。
    pane.executeCommand(QStringLiteral("exit"));
    QVERIFY2(waitFor([&]() { return pane.targets().size() == 1; }, 20000), "内层 exit 后未弹回外层");
    QVERIFY2(waitFor([&]() { return pane.activeWorkingDirectory() == QStringLiteral("/tmp"); }, 15000),
             "弹回外层后 activeWorkingDirectory 未恢复外层自己的目录");
}

void SshSessionStackIntegrationTest::detectsRemoteDistributionOnFirstLogin()
{
    SshSessionStack stack(firstLayerTarget());
    QSignalSpy connected(&stack, &SshSessionStack::shellConnected);
    QSignalSpy failed(&stack, &SshSessionStack::connectionError);
    QSignalSpy detected(&stack, &SshSessionStack::remoteDistributionDetected);

    QByteArray output;
    connect(&stack, &SshSessionStack::outputReady, this, [&output](const QByteArray &bytes) {
        output.append(bytes);
    });

    // 与 MainWindow 的顺序一致：先登记一次性请求，再启动线程。
    stack.requestRemoteDistributionDetection();
    stack.updateTerminalSize(120, 40);
    stack.start();

    QVERIFY2(waitFor([&]() { return connected.count() > 0 || failed.count() > 0; }),
             "连接既未成功也未报错");
    QVERIFY2(failed.isEmpty(),
             qPrintable(QStringLiteral("连接报错：%1")
                            .arg(failed.value(0).value(0).toString())));

    QVERIFY2(waitFor([&]() { return detected.count() > 0; }, 30000),
             "首次登录后未在后台读出远端发行版");
    const QString distributionId = detected.at(0).at(0).toString();
    QVERIFY2(!distributionId.isEmpty(), "识别结果为空");

    // 与远端自报的 ID 逐字比对，确保拿到的是这台服务器的真实系统，而不是任何本地推断。
    QVERIFY(waitFor([&]() { return !output.isEmpty(); }, 15000));
    output.clear();
    stack.enqueueInput("echo SHELLIUM_OS_$(. /etc/os-release; echo $ID)_END\n");
    const QByteArray expected =
        QByteArrayLiteral("SHELLIUM_OS_") + distributionId.toUtf8() + QByteArrayLiteral("_END");
    QVERIFY2(waitFor([&]() { return output.contains(expected); }, 15000),
             qPrintable(QStringLiteral("后台识别到 %1，但远端 Shell 报告的 ID 不同")
                            .arg(distributionId)));

    // 探测只应发生一次：请求被工作线程接手后不得在每个泵送周期重复执行远端命令。
    QVERIFY(waitFor([&]() { return detected.count() > 1; }, 3000) == false);
    QCOMPARE(detected.count(), 1);

    stack.requestStop();
    QVERIFY(stack.wait(10000));
}

QTEST_MAIN(SshSessionStackIntegrationTest)

#include "SshSessionStackIntegrationTest.moc"
