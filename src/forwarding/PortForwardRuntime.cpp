#include "forwarding/PortForwardRuntime.h"

#include "platform/NativeSocket.h"

#include "forwarding/ProxyNetwork.h"
#include "forwarding/Socks5Protocol.h"
#include "ssh/SshSessionStack.h"

#include <QAbstractSocket>
#include <QHostAddress>
#include <QHostInfo>
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
            fail(tr("The SSH session is not ready; cannot start forwarding '%1'").arg(m_rule.name));
            return;
        }
        if (m_rule.type == PortForwardType::Dynamic) {
            const ProxyNetwork::ParseResult parsed = ProxyNetwork::normalize(m_rule.proxyNetworks);
            if (parsed.tooMany || !parsed.invalidEntry.isEmpty()) {
                const QString detail = parsed.tooMany
                    ? tr("At most %1 proxied networks are allowed").arg(ProxyNetwork::kMaxProxyNetworks)
                    : tr("Invalid proxied network '%1'. Use an IP or CIDR such as 10.0.0.0/8")
                          .arg(parsed.invalidEntry);
                fail(tr("The proxied network list for '%1' is invalid: %2").arg(m_rule.name, detail));
                return;
            }
            m_rule.proxyNetworks = parsed.networks;
        }

        if (m_rule.type == PortForwardType::Remote) {
            QString error;
            if (!m_sessionStack->startRemoteForward(m_rule.id,
                                                    m_rule.bindHost,
                                                    m_rule.bindPort,
                                                    &error)) {
                fail(tr("Failed to start remote forwarding '%1': %2").arg(m_rule.name, error));
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
                fail(tr("The listen address '%1' is not a bindable local IP address").arg(normalizedBindHost));
                return;
            }
        }
        if (!m_server->listen(address, m_rule.bindPort)) {
            fail(tr("Failed to listen on %1:%2: %3")
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
            reportConnectionError(tr("Failed to take over the remote forwarding SSH channel"));
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
                        tr("Remote forwarding could not connect to the local target %1:%2: %3")
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
    enum class SocksStage { Greeting, ConnectRequest, Resolving, Connecting };
    struct DynamicClientState {
        SocksStage stage = SocksStage::Greeting;
        QByteArray buffer;
        QTcpSocket *directSocket = nullptr; ///< 网段外目标的本机直连；未开始直连时为空。
        int lookupId = -1; ///< 系统 DNS 查询 id；-1 表示当前没有进行中的查询。
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
                    const auto iterator = m_dynamicClients.find(client);
                    QTcpSocket *directSocket = nullptr;
                    if (iterator != m_dynamicClients.end()) {
                        if (iterator->lookupId >= 0) {
                            QHostInfo::abortHostLookup(iterator->lookupId);
                        }
                        directSocket = iterator->directSocket;
                        iterator->directSocket = nullptr;
                        m_dynamicClients.remove(client);
                    }
                    if (directSocket != nullptr) {
                        directSocket->abort();
                        directSocket->deleteLater();
                    }
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
        if (iterator == m_dynamicClients.end()
            || iterator->stage == SocksStage::Resolving
            || iterator->stage == SocksStage::Connecting) {
            // 正在解析域名、直连或已经交给数据桥时，不能再把套接字里的字节读进握手缓冲。
            return;
        }
        iterator->buffer.append(client->readAll());
        if (iterator->buffer.size() > 64 * 1024) {
            reportConnectionError(tr("SOCKS5 handshake exceeded the 64 KiB safety limit"));
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

        // 空网段表示全部走 SSH，域名留给远端解析。只有配置了网段、且目标不是 IP 时才用
        // 系统 DNS 判断该地址落在网段内还是该走本机默认路由。查询是异步的，避免
        // QHostInfo::fromName 在工作线程里再开一层事件循环，把停止或其它连接重入进来。
        if (!m_rule.proxyNetworks.isEmpty() && !ProxyNetwork::isIpLiteral(request.host)) {
            const QString host = request.host;
            const quint16 port = request.port;
            iterator->stage = SocksStage::Resolving;
            // 结果回调可能在 lookupHost 返回前就同步跑完并改掉这条状态，所以不能把
            // 返回值写回调用前拿到的迭代器。
            const int lookupId = QHostInfo::lookupHost(
                host, this, [this, client, host, port](const QHostInfo &info) {
                    applyResolvedEgress(client, host, port, info);
                });
            const auto current = m_dynamicClients.find(client);
            if (current != m_dynamicClients.end() && current->stage == SocksStage::Resolving) {
                current->lookupId = lookupId;
            } else if (lookupId >= 0) {
                QHostInfo::abortHostLookup(lookupId);
            }
            return;
        }
        applyResolvedEgress(client, request.host, request.port, QHostInfo());
    }

    /**
     * @brief 按网段决定这次 SOCKS 连接走 SSH 还是本机直连。
     * @param info 域名解析结果；目标本身是 IP，或规则没有网段时可以是空对象。
     */
    void applyResolvedEgress(QTcpSocket *client,
                             const QString &host,
                             const quint16 port,
                             const QHostInfo &info)
    {
        auto iterator = m_dynamicClients.find(client);
        if (iterator == m_dynamicClients.end() || m_stopping) {
            return;
        }
        iterator->lookupId = -1;

        QStringList resolvedAddresses;
        bool resolutionFailed = false;
        if (iterator->stage == SocksStage::Resolving) {
            resolutionFailed = info.error() != QHostInfo::NoError || info.addresses().isEmpty();
            if (!resolutionFailed) {
                for (const QHostAddress &address : info.addresses()) {
                    resolvedAddresses.append(address.toString());
                }
            }
        }
        const ProxyNetwork::EgressDecision decision = ProxyNetwork::selectEgress(
            m_rule.proxyNetworks, host, resolvedAddresses, resolutionFailed);
        if (decision.egress == ProxyNetwork::Egress::Reject) {
            client->write(Socks5Protocol::reply(0x01));
            const QString detail = decision.tooMany
                ? tr("At most %1 proxied networks are allowed").arg(ProxyNetwork::kMaxProxyNetworks)
                : tr("Invalid proxied network '%1'. Use an IP or CIDR such as 10.0.0.0/8")
                      .arg(decision.invalidEntry);
            reportConnectionError(tr("The proxied network list for '%1' is invalid: %2")
                                      .arg(m_rule.name, detail));
            client->disconnectFromHost();
            return;
        }
        if (decision.egress == ProxyNetwork::Egress::Direct) {
            openDirect(client, decision.targetHost, port);
            return;
        }
        const QByteArray pending = iterator->buffer;
        iterator->buffer.clear();
        openTunnel(client, decision.targetHost, port, true, pending);
    }

    /** @brief 把 SOCKS 套接字错误映射成 RFC 1928 的回复码。 */
    static quint8 socksReplyFor(const QAbstractSocket::SocketError error)
    {
        switch (error) {
        case QAbstractSocket::ConnectionRefusedError:
            return 0x05;
        case QAbstractSocket::HostNotFoundError:
        case QAbstractSocket::SocketTimeoutError:
            return 0x04;
        case QAbstractSocket::NetworkError:
            return 0x03;
        default:
            return 0x01;
        }
    }

    /**
     * @brief 网段外的目标不进 SSH，改由本机套接字按系统路由直接连接。
     * 握手阶段已经读走的字节留在 DynamicClientState::buffer，连通后再写给目标。
     */
    void openDirect(QTcpSocket *client, const QString &targetHost, const quint16 targetPort)
    {
        auto iterator = m_dynamicClients.find(client);
        if (client == nullptr || iterator == m_dynamicClients.end()) {
            return;
        }
        auto *targetSocket = new QTcpSocket(this);
        iterator->stage = SocksStage::Connecting;
        iterator->directSocket = targetSocket;

        connect(targetSocket, &QTcpSocket::connected, this, [this, client, targetSocket]() {
            auto state = m_dynamicClients.find(client);
            if (state == m_dynamicClients.end() || m_stopping) {
                targetSocket->deleteLater();
                return;
            }
            const QByteArray pending = state->buffer;
            state->directSocket = nullptr;
            m_dynamicClients.remove(client);
            disconnect(client, nullptr, this, nullptr);
            disconnect(targetSocket, nullptr, this, nullptr);
            client->write(Socks5Protocol::reply(0x00));
            if (!pending.isEmpty()) {
                targetSocket->write(pending);
            }
            createBridge(client, targetSocket);
        });
        connect(targetSocket, &QTcpSocket::errorOccurred, this,
                [this, client, targetSocket, targetHost, targetPort](const QAbstractSocket::SocketError error) {
                    auto state = m_dynamicClients.find(client);
                    if (state == m_dynamicClients.end()) {
                        return;
                    }
                    state->directSocket = nullptr;
                    m_dynamicClients.remove(client);
                    client->write(Socks5Protocol::reply(socksReplyFor(error)));
                    reportConnectionError(
                        tr("Could not connect directly to %1:%2: %3")
                            .arg(targetHost)
                            .arg(targetPort)
                            .arg(targetSocket->errorString()));
                    targetSocket->abort();
                    targetSocket->deleteLater();
                    client->disconnectFromHost();
                    client->deleteLater();
                });
        targetSocket->connectToHost(targetHost, targetPort);
    }

    void openTunnel(QTcpSocket *client,
                    const QString &targetHost,
                    const quint16 targetPort,
                    const bool socksClient,
                    const QByteArray &pendingFromClient = {})
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
                tr("Could not connect to target %1:%2 over SSH: %3").arg(targetHost).arg(targetPort).arg(error));
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
            reportConnectionError(tr("Failed to take over the local SSH forwarding channel"));
            client->disconnectFromHost();
            return;
        }

        if (socksClient) {
            m_dynamicClients.remove(client);
            disconnect(client, nullptr, this, nullptr);
            client->write(Socks5Protocol::reply(0x00));
            if (!pendingFromClient.isEmpty()) {
                tunnelSocket->write(pendingFromClient);
            }
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
