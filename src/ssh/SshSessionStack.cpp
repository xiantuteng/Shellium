#include "ssh/SshSessionStack.h"

#include "ssh/SshSessionNode.h"

#include <QMutexLocker>

#include <libssh/libssh.h>

#include <algorithm>

#include <unistd.h>

SshSessionStack::SshSessionStack(const ConnectionTarget &rootTarget, QObject *parent)
    : QThread(parent)
    , m_rootTarget(rootTarget)
{
}

SshSessionStack::~SshSessionStack()
{
    requestStop();
    // 析构时必须等待工作线程离开 libssh 调用，避免 QObject 销毁后线程继续发信号。
    wait(3000);
}

void SshSessionStack::enqueueInput(const QByteArray &bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    QMutexLocker locker(&m_mutex);
    m_pendingInput.append(bytes);
}

void SshSessionStack::updateTerminalSize(const int columns, const int rows)
{
    QMutexLocker locker(&m_mutex);
    const int normalizedColumns = std::max(columns, 1);
    const int normalizedRows = std::max(rows, 1);
    if (m_columns != normalizedColumns || m_rows != normalizedRows) {
        m_columns = normalizedColumns;
        m_rows = normalizedRows;
        m_sizeDirty = true;
    }
}

void SshSessionStack::requestStop()
{
    QMutexLocker locker(&m_mutex);
    m_stopRequested = true;
}

void SshSessionStack::confirmUnknownHost(const bool trusted)
{
    QMutexLocker locker(&m_mutex);
    // 决定只是记录下来，由工作线程在下一轮推进时读取；GUI 线程绝不直接触碰 libssh 会话。
    m_hostKeyDecision = trusted ? HostKeyDecision::Trusted : HostKeyDecision::Rejected;
}

void SshSessionStack::requestPushLayer(const ConnectionTarget &target)
{
    if (!target.isValid()) {
        return;
    }
    QMutexLocker locker(&m_mutex);
    m_pendingPushes.append(target);
}

void SshSessionStack::requestPopLayer()
{
    QMutexLocker locker(&m_mutex);
    ++m_pendingPops;
}

int SshSessionStack::openForwardSocket(const QString &host,
                                       const quint16 port,
                                       QString *errorMessage)
{
    if (!isRunning() || shouldStop()) {
        if (errorMessage != nullptr) {
            *errorMessage = tr("外层 SSH 会话已经结束");
        }
        return -1;
    }

    auto request = QSharedPointer<ForwardRequest>::create();
    request->host = host;
    request->port = port;
    {
        QMutexLocker locker(&m_mutex);
        m_forwardRequests.append(request);
    }

    QMutexLocker requestLocker(&request->mutex);
    if (!request->condition.wait(&request->mutex, 15000) || !request->completed) {
        request->cancelled = true;
        if (errorMessage != nullptr) {
            *errorMessage = tr("创建内层 SSH 转发通道超时");
        }
        return -1;
    }
    if (errorMessage != nullptr) {
        *errorMessage = request->error;
    }
    return request->clientFd;
}

void SshSessionStack::run()
{
    int columns = 80;
    int rows = 24;
    {
        QMutexLocker locker(&m_mutex);
        columns = m_columns;
        rows = m_rows;
        m_sizeDirty = false;
    }

    ConnectionTarget rootTarget = m_rootTarget;
    // 栈底由本程序直接建立，其身份同样是确定事实。
    rootTarget.confidence = ConnectionTarget::Confidence::Confirmed;
    auto root = std::make_unique<SshSessionNode>(rootTarget);
    const SshSessionNode::StepResult start = root->beginEstablish(-1, columns, rows);
    if (!start.ok) {
        emit connectionError(start.message);
        return;
    }
    m_layers.push_back(std::move(root));

    const bool remoteShellEnded = pumpLoop();

    abortInFlightForwards(tr("SSH 会话已经结束"));
    const int exitStatus = m_layers.empty() ? -1 : m_layers.back()->shellExitStatus();
    // 逐层析构会关闭各自的 Shell、转发通道和 socket；栈顶先释放，保证内层通道在其承载的
    // 外层会话之前关闭。
    while (!m_layers.empty()) {
        m_layers.pop_back();
    }

    if (remoteShellEnded) {
        emit shellDisconnected(exitStatus);
    }
}

bool SshSessionStack::pumpLoop()
{
    QByteArray readBuffer(64 * 1024, Qt::Uninitialized);

    // 显式判空而不是只依赖 Q_ASSERT：弹栈后最后一层被移除的那一轮循环必须干净退出，
    // 而 Q_ASSERT 在 Release 构建中会被消除并留下越界访问。
    while (!shouldStop() && !m_layers.empty()) {
        // 建立中的层必须先于终端 IO 推进：栈顶尚未就绪时整个循环的意义就是把它推到就绪。
        if (!advanceEstablishingLayers()) {
            return false;
        }
        if (m_layers.empty()) {
            return false;
        }

        // 所有层的隧道都要泵送：内层会话的传输本身就架在外层的转发通道之上，只泵送栈顶
        // 会让内层连接失去承载，这也是建立阶段不能阻塞的根本原因。
        pumpAllForwards();

        if (!m_layers.back()->isReady()) {
            // 握手期间没有终端数据可搬运；短暂让出 CPU 后继续推进状态机。
            QThread::msleep(5);
            continue;
        }

        if (!m_rootAnnounced) {
            m_rootAnnounced = true;
            emit shellConnected();
            emit layersChanged(layerTargets());
        }

        if (!m_layers.back()->isShellAlive()) {
            if (m_layers.size() > 1) {
                // 内层 Shell 结束（用户 exit、远端 kill、网络中断）是一个确定事件，不需要
                // 也不应该去解析终端输出。弹栈后终端自动回到外层，这正是自持会话相对于
                // 观察式识别的根本优势：出栈与入栈同样可靠。
                popTopLayer();
                abortInFlightForwards(tr("承载该请求的 SSH 会话层已经关闭"));
                emit layersChanged(layerTargets());
                continue;
            }
            return true;
        }

        serviceForwardRequests();
        servicePendingLayerRequests();
        if (m_layers.empty() || !m_layers.back()->isReady()) {
            // 本轮刚压入新层或弹出了旧层，栈顶已经变化；不再沿用变更前的引用搬运终端
            // 数据，下一轮重新取栈顶即可。
            continue;
        }

        // 至此栈顶确定未变且已就绪，才可以安全地长期持有引用。
        SshSessionNode &top = *m_layers.back();

        QByteArray pendingInput;
        int columns = 0;
        int rows = 0;
        bool resizeRequired = false;
        {
            QMutexLocker locker(&m_mutex);
            pendingInput.swap(m_pendingInput);
            resizeRequired = m_sizeDirty;
            columns = m_columns;
            rows = m_rows;
            m_sizeDirty = false;
        }

        if (resizeRequired) {
            top.resizePty(columns, rows);
        }

        qsizetype writeOffset = 0;
        while (writeOffset < pendingInput.size() && !shouldStop()) {
            const int written = top.writeToShell(
                pendingInput.constData() + writeOffset,
                static_cast<int>(pendingInput.size() - writeOffset));
            if (written == SSH_AGAIN || written == 0) {
                // 非阻塞通道暂时不可写不是失败；把 SSH_AGAIN(-2) 加到偏移量会让下一次写入
                // 从缓冲区之前读取，表现为回车偶尔有效而普通命令丢失。等待期间仍要泵送
                // 隧道，否则本层传输可能因外层饥饿而永远不可写。
                pumpAllForwards();
                QThread::msleep(5);
                continue;
            }
            if (written < 0) {
                emit connectionError(tr("向远端终端写入数据失败：%1").arg(top.lastError()));
                return false;
            }
            writeOffset += written;
        }

        bool receivedData = false;
        for (const bool stderrStream : {false, true}) {
            while (true) {
                const int bytesRead = top.readFromShell(
                    readBuffer.data(), static_cast<int>(readBuffer.size()), stderrStream);
                if (bytesRead > 0) {
                    receivedData = true;
                    emit outputReady(QByteArray(readBuffer.constData(), bytesRead));
                } else {
                    break;
                }
            }
        }
        if (!receivedData) {
            // 10ms 轮询在交互延迟和空闲 CPU 占用之间取得平衡，且停止请求能很快生效。
            QThread::msleep(10);
        }
    }
    return false;
}

bool SshSessionStack::advanceEstablishingLayers()
{
    if (m_layers.empty()) {
        return false;
    }
    SshSessionNode &node = *m_layers.back();
    if (node.isReady()) {
        return true;
    }

    // 主机指纹确认由 GUI 线程异步作答；这里只做转交，绝不阻塞工作线程。
    {
        QMutexLocker locker(&m_mutex);
        if (m_hostKeyDecision != HostKeyDecision::None) {
            const bool trusted = m_hostKeyDecision == HostKeyDecision::Trusted;
            m_hostKeyDecision = HostKeyDecision::None;
            locker.unlock();
            node.provideHostKeyDecision(trusted);
        }
    }

    const SshSessionNode::StepResult step = node.advanceEstablish();
    if (step.hostKeyUnknown) {
        emit hostKeyVerificationRequired(node.target(), step.fingerprint);
        return true;
    }
    if (step.pending) {
        return true;
    }
    if (step.ok) {
        // 该层刚刚就绪。栈底的通知留给 pumpLoop，以便 shellConnected 先于 layersChanged
        // 到达界面，避免文件管理器先收到一份“尚未连接”的空列表再被立刻覆盖。
        if (m_layers.size() > 1) {
            emit layersChanged(layerTargets());
        }
        return true;
    }

    // 嵌套层失败只影响它自己：外层会话已经认证完毕且仍在正常工作，把整个终端断开会让
    // 用户白白丢掉外层的工作现场。只有栈底失败才是致命的。
    if (m_layers.size() > 1) {
        const ConnectionTarget failedTarget = node.target();
        m_layers.pop_back();
        emit layerFailed(failedTarget, step.message);
        emit layersChanged(layerTargets());
        return true;
    }

    if (step.legacyHmacRequired) {
        emit legacyHmacRequired(step.message);
    } else {
        emit connectionError(step.message);
    }
    m_layers.pop_back();
    return false;
}

void SshSessionStack::servicePendingLayerRequests()
{
    int pops = 0;
    ConnectionTarget push;
    bool hasPush = false;
    {
        QMutexLocker locker(&m_mutex);
        pops = m_pendingPops;
        m_pendingPops = 0;
        // 已有在途建层时不再取新的：新层必须先建成才能承载下一层，剩余请求留在队列里。
        if (!m_pendingPushes.isEmpty() && m_layerPushForwardId == 0) {
            push = m_pendingPushes.takeFirst();
            hasPush = true;
        }
    }

    bool changed = false;
    while (pops > 0 && m_layers.size() > 1) {
        popTopLayer();
        --pops;
        changed = true;
    }
    if (changed) {
        // 被弹掉的那一层上可能还挂着在途请求，它们的结果永远不会再产生。
        abortInFlightForwards(tr("承载该请求的 SSH 会话层已经关闭"));
        emit layersChanged(layerTargets());
    }

    if (!hasPush) {
        return;
    }
    if (m_layers.empty() || !m_layers.back()->isReady()) {
        emit layerFailed(push, tr("当前没有可用于建立内层连接的已就绪会话"));
        return;
    }

    // 建层的转发时限刻意短于 SFTP：用户正盯着终端等待结果，20 秒内必须给出反馈。
    constexpr int kNestedForwardTimeoutMs = 20000;
    QString error;
    const quint64 forwardId = m_layers.back()->beginForward(
        push.host, push.port, true, kNestedForwardTimeoutMs, &error);
    if (forwardId == 0) {
        emit layerFailed(push, error);
        return;
    }

    m_layerPushTarget = push;
    m_layerPushForwardId = forwardId;
}

void SshSessionStack::handleForwardOutcome(const SshSessionNode::ForwardOutcome &outcome)
{
    if (outcome.id == m_layerPushForwardId && m_layerPushForwardId != 0) {
        ConnectionTarget target = m_layerPushTarget;
        m_layerPushForwardId = 0;
        m_layerPushTarget = {};

        if (!outcome.ok) {
            emit layerFailed(target, outcome.error);
            return;
        }
        if (m_layers.empty()) {
            ::close(outcome.callerFd);
            emit layerFailed(target, tr("承载该内层连接的会话已经关闭"));
            return;
        }

        // 这一层由本程序亲自建立并即将完成认证，它的存在与身份是确定事实。
        target.confidence = ConnectionTarget::Confidence::Confirmed;
        target.parentId = m_layers.back()->target().id;

        int columns = 80;
        int rows = 24;
        {
            QMutexLocker locker(&m_mutex);
            columns = m_columns;
            rows = m_rows;
        }

        auto node = std::make_unique<SshSessionNode>(target);
        const SshSessionNode::StepResult start =
            node->beginEstablish(outcome.callerFd, columns, rows);
        if (!start.ok) {
            // 不手工关闭 callerFd：它已经交给 libssh 会话，由节点析构中的 ssh_free 统一
            // 释放，重复关闭会误伤后续复用同一编号的 fd。
            emit layerFailed(target, start.message);
            return;
        }
        m_layers.push_back(std::move(node));
        emit layersChanged(layerTargets());
        return;
    }

    const auto request = m_forwardsInFlight.take(outcome.id);
    if (request.isNull()) {
        // 无人认领的成功通道必须立即关闭，否则会泄漏一个 fd。
        if (outcome.ok) {
            ::close(outcome.callerFd);
        }
        return;
    }

    QMutexLocker requestLocker(&request->mutex);
    if (outcome.ok && request->cancelled) {
        // 请求方已超时离开，通道无人认领。
        ::close(outcome.callerFd);
        request->error = tr("SFTP 隧道请求已取消");
        request->clientFd = -1;
    } else if (outcome.ok) {
        request->clientFd = outcome.callerFd;
    } else {
        request->error = outcome.error;
        request->clientFd = -1;
    }
    request->completed = true;
    request->condition.wakeAll();
}

void SshSessionStack::pumpAllForwards()
{
    QList<SshSessionNode::ForwardOutcome> outcomes;
    for (const std::unique_ptr<SshSessionNode> &layer : m_layers) {
        outcomes.append(layer->pumpForwards());
    }
    // 派发必须在遍历之后：handleForwardOutcome 可能压入新层，而 std::vector 的 push_back
    // 会让上面正在使用的迭代器失效。
    for (const SshSessionNode::ForwardOutcome &outcome : outcomes) {
        handleForwardOutcome(outcome);
    }
}

void SshSessionStack::serviceForwardRequests()
{
    QList<QSharedPointer<ForwardRequest>> requests;
    {
        QMutexLocker locker(&m_mutex);
        requests.swap(m_forwardRequests);
    }
    if (requests.isEmpty()) {
        return;
    }

    for (const QSharedPointer<ForwardRequest> &request : requests) {
        QString error;
        quint64 forwardId = 0;
        if (!m_layers.empty() && m_layers.back()->isReady()) {
            // SFTP 侧调用方自身有 15 秒等待上限，这里略短以便先返回结构化错误而非超时。
            constexpr int kSftpForwardTimeoutMs = 12000;
            forwardId = m_layers.back()->beginForward(
                request->host, request->port, false, kSftpForwardTimeoutMs, &error);
        } else {
            error = tr("当前没有可用的 SSH 会话层");
        }

        if (forwardId != 0) {
            m_forwardsInFlight.insert(forwardId, request);
            continue;
        }

        QMutexLocker requestLocker(&request->mutex);
        request->error = error;
        request->clientFd = -1;
        request->completed = true;
        request->condition.wakeAll();
    }
}

void SshSessionStack::abortInFlightForwards(const QString &reason)
{
    m_layerPushForwardId = 0;
    m_layerPushTarget = {};

    const QList<QSharedPointer<ForwardRequest>> orphans = m_forwardsInFlight.values();
    m_forwardsInFlight.clear();
    for (const QSharedPointer<ForwardRequest> &request : orphans) {
        QMutexLocker requestLocker(&request->mutex);
        request->error = reason;
        request->clientFd = -1;
        request->completed = true;
        request->condition.wakeAll();
    }
}

ConnectionTarget SshSessionStack::popTopLayer()
{
    if (m_layers.empty()) {
        return {};
    }
    ConnectionTarget target = m_layers.back()->target();
    m_layers.back()->closeShell();
    m_layers.pop_back();
    return target;
}

QList<ConnectionTarget> SshSessionStack::layerTargets() const
{
    QList<ConnectionTarget> targets;
    targets.reserve(static_cast<qsizetype>(m_layers.size()));
    for (const std::unique_ptr<SshSessionNode> &layer : m_layers) {
        // 快照仍然携带密码：当前 SFTP 会为选中层另建一条独立连接，需要凭据完成认证。
        // PR-4 让 SFTP 直接复用本栈已认证的会话之后，这里应改为擦除密码再对外发布。
        targets.append(layer->target());
    }
    return targets;
}

bool SshSessionStack::shouldStop() const
{
    QMutexLocker locker(&m_mutex);
    return m_stopRequested;
}
