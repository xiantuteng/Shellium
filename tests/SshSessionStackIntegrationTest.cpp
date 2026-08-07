#include "core/ConnectionTarget.h"
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

    /** @brief 错误密码必须报认证失败，且不得挂起到超时。 */
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

    QVERIFY2(waitFor([&]() { return failed.count() > 0 || connected.count() > 0; }),
             "错误密码既未报错也未成功，认证重试路径可能卡在 SSH_AUTH_AGAIN");
    QVERIFY2(connected.isEmpty(), "错误密码不应建立 Shell");
    QCOMPARE(failed.count(), 1);
    // 认证失败必须在建立超时（30 秒）之前返回，否则说明状态机没有正确识别 DENIED。
    QVERIFY2(timer.elapsed() < 28000, "认证失败耗时过长，疑似退化为等待整体超时");

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

QTEST_MAIN(SshSessionStackIntegrationTest)

#include "SshSessionStackIntegrationTest.moc"
