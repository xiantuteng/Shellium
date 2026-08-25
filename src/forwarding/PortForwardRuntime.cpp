#include "forwarding/PortForwardRuntime.h"

#include "platform/NativeSocket.h"

#include "forwarding/Socks5Protocol.h"
#include "ssh/SshSessionStack.h"

#include <QAbstractSocket>
#include <QHostAddress>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <algorithm>
#include <functional>


namespace {

/** @brief 单方向尚未写出的最大缓冲；超过时断开连接，避免慢对端造成无限内存增长。 */
constexpr qint64 kMaximumPendingBytes = 4 * 1024 * 1024;

/**
 * @brief 两个已连接 QTcpSocket 之间的双向、带背压保护的数据桥。
 *
 * endpointA 表示监听器接受的一侧：-L/-D 中是本地客户端，-R 中是远端客户端的 SSH
 * socketpair；endpointB 是目标侧。A→B 统一计为 bytesToTarget，B→A 计为
 * bytesFromTarget，使三种转发的统计语义一致。
 */
class SocketBridge final : public QObject {
public:
    using TrafficCallback = std::function<void(quint64, quint64)>;
    using ClosedCallback = std::function<void()>;

    SocketBridge(QTcpSocket *endpointA,
                 QTcpSocket *endpointB,
                 TrafficCallback trafficCallback,
                 ClosedCallback closedCallback,
                 QObject *parent)
        : QObject(parent)
        , m_endpointA(endpointA)
        , m_endpointB(endpointB)
        , m_trafficCallback(std::move(trafficCallback))
        , m_closedCallback(std::move(closedCallback))
    {
        Q_ASSERT(m_endpointA != nullptr);
        Q_ASSERT(m_endpointB != nullptr);
        m_endpointA->setParent(this);
        m_endpointB->setParent(this);

        connect(m_endpointA, &QTcpSocket::readyRead, this, [this]() {
            transfer(m_endpointA, m_endpointB, true);
        });
        connect(m_endpointB, &QTcpSocket::readyRead, this, [this]() {
            transfer(m_endpointB, m_endpointA, false);
        });
        const auto closeBoth = [this]() { finish(); };
        connect(m_endpointA, &QTcpSocket::disconnected, this, closeBoth);
        connect(m_endpointB, &QTcpSocket::disconnected, this, closeBoth);
        connect(m_endpointA, &QTcpSocket::errorOccurred, this,
                [closeBoth](QAbstractSocket::SocketError) { closeBoth(); });
        connect(m_endpointB, &QTcpSocket::errorOccurred, this,
                [closeBoth](QAbstractSocket::SocketError) { closeBoth(); });

        // 连接建立前 QTcpSocket 可能已经缓存数据；立即搬运可避免必须等下一次 readyRead。
        transfer(m_endpointA, m_endpointB, true);
        transfer(m_endpointB, m_endpointA, false);
    }

    /** @brief 主动停止桥并只触发一次关闭统计。 */
    void stop() { finish(); }

private:
    void transfer(QTcpSocket *source, QTcpSocket *destination, const bool toTarget)
    {
        if (m_finished || source == nullptr || destination == nullptr) {
            return;
        }
        const QByteArray bytes = source->readAll();
        if (bytes.isEmpty()) {
            return;
        }
        if (destination->bytesToWrite() + bytes.size() > kMaximumPendingBytes
            || destination->write(bytes) != bytes.size()) {
            // QTcpSocket::write 只把数据放入 Qt 缓冲，仍可能因资源限制拒绝。继续读取会让
            // 本进程成为无界缓冲，因此一旦越过安全上限就关闭这一条连接而非整条规则。
            finish();
            return;
        }
        if (m_trafficCallback) {
            m_trafficCallback(toTarget ? static_cast<quint64>(bytes.size()) : 0,
                              toTarget ? 0 : static_cast<quint64>(bytes.size()));
        }
    }

    void finish()
    {
        if (m_finished) {
            return;
        }
        m_finished = true;
        if (m_endpointA != nullptr) {
            m_endpointA->abort();
        }
        if (m_endpointB != nullptr) {
            m_endpointB->abort();
        }
        if (m_closedCallback) {
            m_closedCallback();
        }
        deleteLater();
    }

    QTcpSocket *m_endpointA = nullptr;
    QTcpSocket *m_endpointB = nullptr;
    TrafficCallback m_trafficCallback;
    ClosedCallback m_closedCallback;
    bool m_finished = false;
};

} // namespace

/**
 * @brief PortForwardRuntime 的网络线程实现。
 *
 * 类定义保留在 cpp 内，避免把 QTcpSocket 等传输细节泄漏到公开 API。由于含 Q_OBJECT，
 * 文件末尾显式包含 automoc 输出。
 */
class PortForwardWorker final : public QObject {
    Q_OBJECT

public:
    PortForwardWorker(PortForwardRule rule, SshSessionStack *sessionStack)
        : m_rule(std::move(rule))
        , m_sessionStack(sessionStack)
    {
    }

signals:
    void statisticsChanged(const PortForwardStatistics &statistics);
    void statusMessage(const QString &message);
    void stopped();

public slots:
    void startRuntime()
    {
        if (m_statistics.state == PortForwardState::Running
            || m_statistics.state == PortForwardState::Starting) {
            return;
        }
        m_stopping = false;
        setState(PortForwardState::Starting);
        if (m_sessionStack == nullptr || !m_sessionStack->isRunning()) {
            fail(tr("SSH 会话尚未就绪，无法启动转发“%1”").arg(m_rule.name));
            return;
        }

        if (m_rule.type == PortForwardType::Remote) {
            QString error;
            if (!m_sessionStack->startRemoteForward(m_rule.id,
                                                    m_rule.bindHost,
                                                    m_rule.bindPort,
                                                    &error)) {
                fail(tr("启动远程转发“%1”失败：%2").arg(m_rule.name, error));
                return;
            }
            setState(PortForwardState::Running);
            return;
        }

        if (m_server != nullptr) {
            // 上一次监听失败后允许用户修正系统占用并重试；旧 QTcpServer 没有继续存在的
            // 价值，先释放可避免重复 start 泄漏对象和残留错误状态。
            m_server->close();
            m_server->deleteLater();
        }
        m_server = new QTcpServer(this);
        connect(m_server, &QTcpServer::newConnection,
                this, &PortForwardWorker::acceptLocalConnections);
        QHostAddress address;
        const QString normalizedBindHost = m_rule.bindHost.trimmed();
        if (!address.setAddress(normalizedBindHost)) {
            if (normalizedBindHost.compare(QStringLiteral("localhost"),
                                           Qt::CaseInsensitive) == 0) {
                address = QHostAddress::LocalHost;
            } else {
                fail(tr("监听地址“%1”不是可绑定的本机 IP 地址").arg(normalizedBindHost));
                return;
            }
        }
        if (!m_server->listen(address, m_rule.bindPort)) {
            fail(tr("监听 %1:%2 失败：%3")
                     .arg(m_rule.bindHost)
                     .arg(m_rule.bindPort)
                     .arg(m_server->errorString()));
            return;
        }
        setState(PortForwardState::Running);
    }

    void stopRuntime()
    {
        if (m_statistics.state == PortForwardState::Stopped) {
            emit stopped();
            return;
        }
        m_stopping = true;
        setState(PortForwardState::Stopping);
        if (m_server != nullptr) {
            m_server->close();
            m_server->deleteLater();
            m_server = nullptr;
        }
        for (QTcpSocket *client : m_dynamicClients.keys()) {
            client->abort();
            client->deleteLater();
        }
        m_dynamicClients.clear();
        const QList<SocketBridge *> bridges = m_bridges.values();
        for (SocketBridge *bridge : bridges) {
            bridge->stop();
        }
        m_bridges.clear();
        if (m_rule.type == PortForwardType::Remote && m_sessionStack != nullptr) {
            m_sessionStack->stopRemoteForward(m_rule.id);
        }
        m_statistics.activeConnections = 0;
        setState(PortForwardState::Stopped);
        emit stopped();
    }

    /** @brief 接收 SshSessionStack 交出的 -R 入站 socket fd；调用后本 Worker 取得所有权。 */
    void acceptRemoteConnection(const NativeSocketHandle tunnelFd)
    {
        if (!isNativeSocketValid(tunnelFd)) {
            return;
        }
        if (m_stopping || m_rule.type != PortForwardType::Remote
            || m_statistics.state != PortForwardState::Running) {
            closeNativeSocket(tunnelFd);
            return;
        }
        auto *tunnelSocket = new QTcpSocket(this);
        if (!tunnelSocket->setSocketDescriptor(tunnelFd,
                                               QAbstractSocket::ConnectedState,
                                               QIODevice::ReadWrite)) {
            closeNativeSocket(tunnelFd);
            tunnelSocket->deleteLater();
            reportConnectionError(tr("接管远程转发 SSH 通道失败"));
            return;
        }

        auto *targetSocket = new QTcpSocket(this);
        connect(targetSocket, &QTcpSocket::connected, this,
                [this, tunnelSocket, targetSocket]() {
                    createBridge(tunnelSocket, targetSocket);
                });
        connect(targetSocket, &QTcpSocket::errorOccurred, this,
                [this, tunnelSocket, targetSocket](QAbstractSocket::SocketError) {
                    reportConnectionError(
                        tr("远程转发无法连接本机目标 %1:%2：%3")
                            .arg(m_rule.targetHost)
                            .arg(m_rule.targetPort)
                            .arg(targetSocket->errorString()));
                    tunnelSocket->abort();
                    tunnelSocket->deleteLater();
                    targetSocket->deleteLater();
                });
        targetSocket->connectToHost(m_rule.targetHost, m_rule.targetPort);
    }

private:
    enum class SocksStage { Greeting, ConnectRequest };
    struct DynamicClientState {
        SocksStage stage = SocksStage::Greeting;
        QByteArray buffer;
    };

    void acceptLocalConnections()
    {
        while (m_server != nullptr && m_server->hasPendingConnections()) {
            QTcpSocket *client = m_server->nextPendingConnection();
            if (client == nullptr) {
                continue;
            }
            client->setParent(this);
            if (m_rule.type == PortForwardType::Dynamic) {
                m_dynamicClients.insert(client, {});
                connect(client, &QTcpSocket::readyRead,
                        this, [this, client]() { processSocksClient(client); });
                connect(client, &QTcpSocket::disconnected, this, [this, client]() {
                    m_dynamicClients.remove(client);
                    client->deleteLater();
                });
                processSocksClient(client);
            } else {
                openTunnel(client, m_rule.targetHost, m_rule.targetPort, false);
            }
        }
    }

    void processSocksClient(QTcpSocket *client)
    {
        auto iterator = m_dynamicClients.find(client);
        if (iterator == m_dynamicClients.end()) {
            return;
        }
        iterator->buffer.append(client->readAll());
        if (iterator->buffer.size() > 64 * 1024) {
            reportConnectionError(tr("SOCKS5 握手超过 64 KiB 安全上限"));
            client->disconnectFromHost();
            return;
        }

        if (iterator->stage == SocksStage::Greeting) {
            const Socks5Protocol::GreetingResult greeting =
                Socks5Protocol::parseGreeting(iterator->buffer);
            if (greeting.status == Socks5Protocol::ParseStatus::NeedMore) {
                return;
            }
            client->write(greeting.response);
            iterator->buffer.remove(0, greeting.consumed);
            if (greeting.status == Socks5Protocol::ParseStatus::Reject) {
                reportConnectionError(greeting.error);
                client->disconnectFromHost();
                return;
            }
            iterator->stage = SocksStage::ConnectRequest;
        }

        if (iterator->stage != SocksStage::ConnectRequest) {
            return;
        }
        const Socks5Protocol::ConnectResult request =
            Socks5Protocol::parseConnectRequest(iterator->buffer);
        if (request.status == Socks5Protocol::ParseStatus::NeedMore) {
            return;
        }
        iterator->buffer.remove(0, request.consumed);
        if (request.status == Socks5Protocol::ParseStatus::Reject) {
            client->write(Socks5Protocol::reply(request.replyCode));
            reportConnectionError(request.error);
            client->disconnectFromHost();
            return;
        }
        openTunnel(client, request.host, request.port, true);
    }

    void openTunnel(QTcpSocket *client,
                    const QString &targetHost,
                    const quint16 targetPort,
                    const bool socksClient)
    {
        if (client == nullptr || m_sessionStack == nullptr) {
            return;
        }
        // 该同步调用只阻塞本规则自己的网络线程，GUI、SSH 泵送以及其他规则不受影响。
        // 规则绑定根主机，因此必须使用根层入口，不能随用户进入嵌套服务器而改变出口。
        QString error;
        const NativeSocketHandle tunnelFd = m_sessionStack->openRootForwardSocket(
            targetHost, targetPort, &error);
        if (!isNativeSocketValid(tunnelFd)) {
            if (socksClient) {
                client->write(Socks5Protocol::reply(0x05));
            }
            reportConnectionError(
                tr("无法经 SSH 连接目标 %1:%2：%3").arg(targetHost).arg(targetPort).arg(error));
            client->disconnectFromHost();
            return;
        }

        auto *tunnelSocket = new QTcpSocket(this);
        if (!tunnelSocket->setSocketDescriptor(tunnelFd,
                                               QAbstractSocket::ConnectedState,
                                               QIODevice::ReadWrite)) {
            closeNativeSocket(tunnelFd);
            tunnelSocket->deleteLater();
            if (socksClient) {
                client->write(Socks5Protocol::reply(0x01));
            }
            reportConnectionError(tr("接管本地 SSH 转发通道失败"));
            client->disconnectFromHost();
            return;
        }

        if (socksClient) {
            m_dynamicClients.remove(client);
            disconnect(client, nullptr, this, nullptr);
            client->write(Socks5Protocol::reply(0x00));
        }
        createBridge(client, tunnelSocket);
    }

    void createBridge(QTcpSocket *endpointA, QTcpSocket *endpointB)
    {
        if (m_stopping || endpointA == nullptr || endpointB == nullptr) {
            if (endpointA != nullptr) {
                endpointA->deleteLater();
            }
            if (endpointB != nullptr) {
                endpointB->deleteLater();
            }
            return;
        }

        ++m_statistics.activeConnections;
        ++m_statistics.totalConnections;
        emit statisticsChanged(m_statistics);
        QPointer<PortForwardWorker> guard(this);
        auto *bridge = new SocketBridge(
            endpointA,
            endpointB,
            [guard](const quint64 toTarget, const quint64 fromTarget) {
                if (guard == nullptr) {
                    return;
                }
                guard->m_statistics.bytesToTarget += toTarget;
                guard->m_statistics.bytesFromTarget += fromTarget;
                emit guard->statisticsChanged(guard->m_statistics);
            },
            [guard]() {
                if (guard == nullptr) {
                    return;
                }
                guard->m_statistics.activeConnections =
                    guard->m_statistics.activeConnections > 0
                        ? guard->m_statistics.activeConnections - 1 : 0;
                emit guard->statisticsChanged(guard->m_statistics);
            },
            this);
        m_bridges.insert(bridge);
        connect(bridge, &QObject::destroyed, this,
                [this, bridge]() { m_bridges.remove(bridge); });
    }

    void setState(const PortForwardState state)
    {
        m_statistics.state = state;
        if (state != PortForwardState::Failed) {
            m_statistics.lastError.clear();
        }
        emit statisticsChanged(m_statistics);
    }

    void fail(const QString &message)
    {
        m_statistics.state = PortForwardState::Failed;
        m_statistics.lastError = message;
        emit statisticsChanged(m_statistics);
        emit statusMessage(message);
    }

    void reportConnectionError(const QString &message)
    {
        // 单条客户端失败不停止监听；lastError 保留最新诊断，用户仍可观察后续成功连接。
        m_statistics.lastError = message;
        emit statisticsChanged(m_statistics);
        emit statusMessage(message);
    }

    PortForwardRule m_rule;
    SshSessionStack *m_sessionStack = nullptr;
    QTcpServer *m_server = nullptr;
    QHash<QTcpSocket *, DynamicClientState> m_dynamicClients;
    QSet<SocketBridge *> m_bridges;
    PortForwardStatistics m_statistics;
    bool m_stopping = false;
};

PortForwardRuntime::PortForwardRuntime(const PortForwardRule &rule,
                                       SshSessionStack *sessionStack,
                                       QObject *parent)
    : QObject(parent)
    , m_rule(rule)
    , m_sessionStack(sessionStack)
    , m_workerThread(new QThread(this))
    , m_worker(new PortForwardWorker(rule, sessionStack))
{
    Q_ASSERT(m_sessionStack != nullptr);
    m_workerThread->setObjectName(QStringLiteral("PortForward-%1").arg(rule.id));
    m_worker->moveToThread(m_workerThread);
    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker,
            &PortForwardWorker::statisticsChanged,
            this,
            [this](const PortForwardStatistics &statistics) {
                m_statistics = statistics;
                emit statisticsChanged(m_rule.id, statistics);
            });
    connect(m_worker, &PortForwardWorker::statusMessage,
            this, &PortForwardRuntime::statusMessage);
    connect(m_worker, &PortForwardWorker::stopped,
            this, [this]() { emit stopped(m_rule.id); });

    m_workerThread->start();
}

PortForwardRuntime::~PortForwardRuntime()
{
    if (m_workerThread->isRunning()) {
        // stopRuntime 只触碰网络线程对象和线程安全的栈接口；阻塞等待能保证 QTcpSocket 在
        // 所属线程内释放，避免应用退出时出现“socket notifier 从其他线程销毁”的告警。
        QMetaObject::invokeMethod(m_worker, "stopRuntime", Qt::BlockingQueuedConnection);
        m_workerThread->quit();
        m_workerThread->wait(22000);
    }
    m_worker = nullptr;
}

void PortForwardRuntime::start()
{
    if (m_worker != nullptr) {
        QMetaObject::invokeMethod(m_worker, "startRuntime", Qt::QueuedConnection);
    }
}

void PortForwardRuntime::stop()
{
    if (m_worker != nullptr) {
        QMetaObject::invokeMethod(m_worker, "stopRuntime", Qt::QueuedConnection);
    }
}

void PortForwardRuntime::acceptRemoteConnection(const NativeSocketHandle tunnelFd)
{
    if (!isNativeSocketValid(tunnelFd)) {
        return;
    }
    if (m_worker == nullptr || m_workerThread == nullptr || !m_workerThread->isRunning()) {
        // fd 所有权已经由管理器转移到本对象；即使工作线程恰好结束，也必须在此兜底关闭，
        // 不能把释放责任推回已经返回的信号发送方。
        closeNativeSocket(tunnelFd);
        return;
    }
    // 使用 Worker 作为队列接收者，保证 QTcpSocket 只在其所属网络线程创建和操作。
    // 若析构紧随其后发生，同一发送线程投递的停止事件排在本事件之后，连接会先被接管再
    // 由 stopRuntime 统一关闭，不会出现跨线程销毁 socket notifier 的问题。
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, tunnelFd]() { worker->acceptRemoteConnection(tunnelFd); },
        Qt::QueuedConnection);
}

#include "PortForwardRuntime.moc"
