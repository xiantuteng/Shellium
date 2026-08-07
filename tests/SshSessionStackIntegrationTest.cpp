#include "core/ConnectionTarget.h"
#include "sftp/SftpService.h"
#include "terminal/TerminalPane.h"
#include "terminal/TerminalView.h"
#include "ssh/ShellShim.h"
#include "ssh/SshSessionStack.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

#include <unistd.h>

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

    /** @brief 压入真实的第二层，在其中执行命令，再通过远端 exit 确定性地弹回第一层。 */
    void pushesAndPopsNestedLayer();

    /** @brief 内层目标不可达时只弹回外层，外层会话必须保持可用。 */
    void keepsOuterLayerAliveWhenNestedLayerFails();

    /** @brief 建层隧道尚未就绪时，外层终端必须依然可交互，不得被同步等待冻结。 */
    void keepsTerminalResponsiveWhileNestedLayerConnects();

    /** @brief 注入远端的 shim 必须能在真实 Shell 中定义成功并上报可解析的事件。 */
    void shellShimReportsSshInvocationFromRealShell();

    /** @brief 空路径必须由服务端解析成该账号的主目录，而不是写死根目录。 */
    void resolvesHomeDirectoryForEmptyPath();

    /** @brief SFTP 连接池必须让后续操作免去重复握手，而不只是"看起来能用"。 */
    void reusesPooledSftpConnectionAcrossRequests();

    /** @brief 无密码的嵌套目标必须索要凭据而非直接失败，且凭据要能传给 SFTP。 */
    void requestsCredentialForNestedLayerWithoutPassword();

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
    int tunnelFd = -1;
    QString tunnelError;
    bool done = false;
    auto *probe = QThread::create([&]() {
        tunnelFd = stack.openForwardSocket(m_secondLayerHost, 22, &tunnelError);
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
                 const ssize_t count = ::read(tunnelFd, buffer, sizeof(buffer));
                 if (count > 0) {
                     banner.append(buffer, count);
                 }
                 return banner.contains("SSH-");
             }, 20000),
             "未从隧道另一端读到 SSH 版本横幅，隧道可能未真正连通");

    ::close(tunnelFd);
    stack.requestStop();
    QVERIFY(stack.wait(10000));
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

    // 注入 shim。这一步在真实 Shell 里跑：语法错误、base64 缺失、引号转义问题都只有
    // 在这里才会暴露，桩替身完全测不出来。
    stack.enqueueInput(ShellShim::scriptBody() + "\n");
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
    qint64 warmTotalMs = 0;
    for (int round = 0; round < 3; ++round) {
        const int before = listed.count();
        QElapsedTimer warmTimer;
        warmTimer.start();
        service.listDirectory(target, QStringLiteral("/"));
        QVERIFY(waitFor([&]() { return listed.count() > before || failed.count() > 0; }, 30000));
        QVERIFY2(failed.isEmpty(), "复用池化连接时不应失败");
        warmTotalMs += warmTimer.elapsed();
    }
    const qint64 warmAverageMs = warmTotalMs / 3;

    qInfo("SFTP 目录读取：冷 %lldms，热平均 %lldms", coldMs, warmAverageMs);
    // 阈值取一半而非某个绝对毫秒数：网络快慢因环境而异，但"省掉一整套握手"这个事实不变。
    QVERIFY2(warmAverageMs * 2 < coldMs,
             qPrintable(QStringLiteral("热请求未显著快于冷请求（冷 %1ms，热平均 %2ms），"
                                       "连接池可能没有生效")
                            .arg(coldMs).arg(warmAverageMs)));
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

    // 真正的远端输出必须完好——过滤不能把用户内容一起吞掉。
    QVERIFY2(screen.contains(QStringLiteral("ssh root@") + m_secondLayerHost),
             "用户自己敲的命令被过滤掉了");
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

QTEST_MAIN(SshSessionStackIntegrationTest)

#include "SshSessionStackIntegrationTest.moc"
