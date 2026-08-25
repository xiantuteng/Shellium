#include "ssh/SshSessionStack.h"

#include "ssh/ShellShim.h"
#include "ssh/SshSessionNode.h"

#include <QLoggingCategory>
#include <QMutexLocker>
#include <QRegularExpression>

#include <libssh/libssh.h>

#include <algorithm>

#include <unistd.h>

namespace {
/** @brief 与 TerminalView 共用的认证诊断通道；见该文件中的说明。 */
Q_LOGGING_CATEGORY(authLog, "shellium.auth", QtWarningMsg)

} // namespace

SshSessionStack::SshSessionStack(const ConnectionTarget &rootTarget, QObject *parent)
    : SshSessionStack(rootTarget, true, parent)
{
}

SshSessionStack::SshSessionStack(const ConnectionTarget &rootTarget,
                                 const bool installShellIntegration,
                                 QObject *parent)
    : SshSessionStack(QList<ConnectionTarget>{rootTarget}, installShellIntegration, parent)
{
}

SshSessionStack::SshSessionStack(const QList<ConnectionTarget> &connectionPlan,
                                 const bool installShellIntegration,
                                 QObject *parent)
    : QThread(parent)
    , m_connectionPlan(connectionPlan)
    , m_installShellIntegration(installShellIntegration)
{
    // 中转标记由连接计划的实际位置决定，不能信任调用方可能过时的快照。最后一层始终是
    // 用户要交互的逻辑目标；单元素计划自然就是普通直连。
    for (qsizetype index = 0; index < m_connectionPlan.size(); ++index) {
        m_connectionPlan[index].transit = index + 1 < m_connectionPlan.size();
    }
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

bool SshSessionStack::parseRemoteDistributionOutput(const QByteArray &output,
                                                    QString *distributionId,
                                                    QString *versionId)
{
    Q_ASSERT(distributionId != nullptr);
    Q_ASSERT(versionId != nullptr);
    const QList<QByteArray> lines = output.split('\n');
    if (lines.isEmpty()) {
        return false;
    }
    const QString identifier = QString::fromUtf8(lines.constFirst()).trimmed().toLower();
    static const QRegularExpression validIdentifier(
        QStringLiteral("^[a-z0-9][a-z0-9_.-]{0,63}$"));
    if (!validIdentifier.match(identifier).hasMatch()) {
        return false;
    }
    const QString version = lines.size() > 1
        ? QString::fromUtf8(lines.at(1)).trimmed().left(64)
        : QString{};
    *distributionId = identifier;
    *versionId = version;
    return true;
}

void SshSessionStack::requestRemoteDistributionDetection()
{
    QMutexLocker locker(&m_mutex);
    // 连接尚未完成时也可安全登记；工作线程只会在逻辑最终层就绪、且主机已真正可交互后
    // 执行。布尔值而非任务队列确保重复点击、自动重连等不会创建多次相同探测。
    m_distributionDetectionRequested = true;
}

void SshSessionStack::provideLayerCredential(const QString &secret)
{
    qCDebug(authLog, "provideLayerCredential len=%lld",
            static_cast<long long>(secret.size()));
    QMutexLocker locker(&m_mutex);
    // 只做记录，由工作线程在下一轮推进时消费；GUI 线程绝不直接触碰 libssh 会话。
    m_pendingCredential = secret;
    m_hasPendingCredential = true;
}

void SshSessionStack::requestPushLayer(const ConnectionTarget &target,
                                       const bool outerShimSuspended)
{
    if (!target.isValid()) {
        return;
    }
    QMutexLocker locker(&m_mutex);
    m_pendingPushes.append(PendingPush{target, outerShimSuspended, false});
}

void SshSessionStack::requestPopLayer()
{
    QMutexLocker locker(&m_mutex);
    ++m_pendingPops;
}

int SshSessionStack::openForwardSocket(const QString &layerId,
                                       const QString &host,
                                       const quint16 port,
                                       QString *errorMessage)
{
    return openForwardSocketImpl(false, layerId, host, port, errorMessage);
}

int SshSessionStack::openRootForwardSocket(const QString &host,
                                           const quint16 port,
                                           QString *errorMessage)
{
    return openForwardSocketImpl(true, {}, host, port, errorMessage);
}

int SshSessionStack::openForwardSocketImpl(const bool rootOrigin,
                                           const QString &layerId,
                                           const QString &host,
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
    request->layerId = layerId;
    request->host = host;
    request->port = port;
    request->rootOrigin = rootOrigin;
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

bool SshSessionStack::startRemoteForward(const QString &ruleId,
                                         const QString &bindHost,
                                         const quint16 bindPort,
                                         QString *errorMessage)
{
    if (!isRunning() || shouldStop()) {
        if (errorMessage != nullptr) {
            *errorMessage = tr("根 SSH 会话已经结束");
        }
        return false;
    }
    auto request = QSharedPointer<RemoteListenRequest>::create();
    request->ruleId = ruleId;
    request->bindHost = bindHost;
    request->bindPort = bindPort;
    {
        QMutexLocker locker(&m_mutex);
        m_remoteListenRequests.append(request);
    }

    QMutexLocker requestLocker(&request->mutex);
    if (!request->condition.wait(&request->mutex, 20000) || !request->completed) {
        request->cancelled = true;
        if (errorMessage != nullptr) {
            *errorMessage = tr("等待服务器建立远程监听超时");
        }
        // 排队停止保证晚到的成功结果不会留下无人管理的远程监听。
        requestLocker.unlock();
        stopRemoteForward(ruleId);
        return false;
    }
    if (errorMessage != nullptr) {
        *errorMessage = request->error;
    }
    return request->succeeded;
}

void SshSessionStack::stopRemoteForward(const QString &ruleId)
{
    if (ruleId.trimmed().isEmpty()) {
        return;
    }
    QMutexLocker locker(&m_mutex);
    if (!m_pendingRemoteStops.contains(ruleId)) {
        m_pendingRemoteStops.append(ruleId);
    }
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

    if (m_connectionPlan.isEmpty() || !m_connectionPlan.constFirst().isValid()) {
        emit connectionError(tr("连接计划为空或第一跳无效"));
        return;
    }
    ConnectionTarget rootTarget = m_connectionPlan.constFirst();
    // 栈底由本程序直接建立，其身份同样是确定事实。
    rootTarget.confidence = ConnectionTarget::Confidence::Confirmed;
    auto root = std::make_unique<SshSessionNode>(
        rootTarget,
        rootTarget.transit ? SshSessionNode::Role::Transit
                           : SshSessionNode::Role::Interactive);
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

        if (!m_plannedFailure.isEmpty()) {
            emit connectionError(m_plannedFailure);
            return false;
        }

        // 保活要放在栈顶就绪判断之前：栈顶还在握手时，下面已经就绪的外层照样会空闲到被
        // 掐断，而外层一断，正在其上建立的内层也就白建了。
        serviceKeepAlive();

        for (const std::unique_ptr<SshSessionNode> &layer : m_layers) {
            layer->pumpExec();
            bool succeeded = false;
            QByteArray standardOutput;
            QString execError;
            if (layer->takeExecResult(&succeeded, &standardOutput, &execError)) {
                if (layer->target().id == m_distributionDetectionTargetId) {
                    m_distributionDetectionTargetId.clear();
                    QString distributionId;
                    QString versionId;
                    if (succeeded
                        && parseRemoteDistributionOutput(
                            standardOutput, &distributionId, &versionId)) {
                        emit remoteDistributionDetected(distributionId, versionId);
                    }
                    continue;
                }
                if (succeeded) {
                    // 只有远端明确返回退出码 0 才加载脚本。这里只登记意图：真正下发要等
                    // 交互 Shell 静默，理由见 serviceDeferredShellIntegration()。
                    m_pendingShellIntegration.insert(layer->target().id);
                    m_shellOutputClock.start();
                } else {
                    emit shellIntegrationFailed(
                        layer->target(),
                        execError.isEmpty() ? tr("远端未返回安装结果") : execError);
                }
            }
        }

        if (!m_layers.back()->isReady()) {
            // 握手期间没有终端数据可搬运；短暂让出 CPU 后继续推进状态机。
            QThread::msleep(5);
            continue;
        }

        // 受管主机链尚未到达最终目标时，只推进建层和隧道，不读取中间层 PTY，也不向界面
        // 宣布成功。这样登录横幅、提示符和 Shell Integration 都只属于最终目标。
        if (m_layers.size() < static_cast<std::size_t>(m_connectionPlan.size())) {
            servicePendingLayerRequests();
            QThread::msleep(5);
            continue;
        }

        if (!m_rootAnnounced) {
            m_rootAnnounced = true;
            emit shellConnected();
            emit layersChanged(layerTargets());
        }

        serviceRemoteDistributionDetection();

        if (!m_layers.back()->isShellAlive()) {
            if (m_layers.size() > static_cast<std::size_t>(m_connectionPlan.size())) {
                // 内层 Shell 结束（用户 exit、远端 kill、网络中断）是一个确定事件，不需要
                // 也不应该去解析终端输出。弹栈后终端自动回到外层，这正是自持会话相对于
                // 观察式识别的根本优势：出栈与入栈同样可靠。
                popTopLayer();
                discardPendingCredential();
                abortInFlightForwards(tr("承载该请求的 SSH 会话层已经关闭"));
                emit layersChanged(layerTargets());
                continue;
            }
            return true;
        }

        serviceForwardRequests();
        serviceRemoteForwardRequests();
        servicePendingLayerRequests();
        if (m_layers.empty() || !m_layers.back()->isReady()) {
            // 本轮刚压入新层或弹出了旧层，栈顶已经变化；不再沿用变更前的引用搬运终端
            // 数据，下一轮重新取栈顶即可。
            continue;
        }

        // 至此栈顶确定未变且已就绪，才可以安全地长期持有引用。
        SshSessionNode &top = *m_layers.back();

        serviceDeferredShellIntegration(top);

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

        // 内部排队的注入必须先写完再写用户输入，否则两者会交错，回显也就无法精确扣掉。
        if (!top.flushOutgoing()) {
            QMutexLocker locker(&m_mutex);
            m_pendingInput.prepend(pendingInput);
            continue;
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
                    // 只要还在出字节，远端就仍在跑登录脚本或上一条命令，注入必须继续等。
                    m_shellOutputClock.start();
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

    if (node.isAwaitingCredential()) {
        // 必须先确认节点确实在等：过早投递会被 provideCredential 静默丢弃，用户抢先输入的
        // 密码就此消失，界面上表现为"输了密码却毫无反应"。
        QString secret;
        bool hasSecret = false;
        {
            QMutexLocker locker(&m_mutex);
            if (m_hasPendingCredential) {
                secret = m_pendingCredential;
                m_pendingCredential.clear();
                m_hasPendingCredential = false;
                hasSecret = true;
            }
        }
        if (hasSecret) {
            qCDebug(authLog, "consume credential len=%lld -> %s",
                    static_cast<long long>(secret.size()),
                    qPrintable(node.target().connectionSpecifier(true)));
            node.provideCredential(secret);
        }
    }

    const SshSessionNode::StepResult step = node.advanceEstablish();
    if (step.credentialRequired) {
        // retry 由节点判定：只有确实提交过一份密码并被服务器拒绝才算重试。提示语必须
        // 区分开，否则用户看到一模一样的提示会以为是自己没按回车。
        emit layerCredentialRequired(node.target(), step.credentialRetry);
        return true;
    }
    if (step.hostKeyUnknown) {
        emit hostKeyVerificationRequired(node.target(), step.fingerprint);
        return true;
    }
    if (step.hostKeyChanged) {
        emit hostKeyChangeVerificationRequired(node.target(), step.fingerprint);
        return true;
    }
    if (step.pending) {
        return true;
    }
    if (step.ok) {
        if (m_layers.size() < static_cast<std::size_t>(m_connectionPlan.size())) {
            // 当前层只是计划中的中转节点。下一跳通过普通建层队列复用既有 direct-tcpip / exec
            // 中继状态机；planned 标记让任何失败都升级为整条逻辑连接失败。
            const ConnectionTarget next = m_connectionPlan.at(
                static_cast<qsizetype>(m_layers.size()));
            QMutexLocker locker(&m_mutex);
            m_pendingPushes.append(PendingPush{next, false, true});
            return true;
        }
        // 交互式终端依赖 Shell 集成获得确定性的目录和嵌套连接事件；连接测试则必须保持
        // 只读，认证成功本身已足以得出结论，不应为了测试向远端写入临时脚本。
        if (m_installShellIntegration) {
            beginShellIntegrationInstall(node);
        }
        // 该层刚刚就绪。栈底的通知留给 pumpLoop，以便 shellConnected 先于 layersChanged
        // 到达界面，避免文件管理器先收到一份“尚未连接”的空列表再被立刻覆盖。
        if (m_rootAnnounced && m_layers.size() > m_connectionPlan.size()) {
            emit layersChanged(layerTargets());
        }
        return true;
    }

    // 嵌套层失败只影响它自己：外层会话已经认证完毕且仍在正常工作，把整个终端断开会让
    // 用户白白丢掉外层的工作现场。只有栈底失败才是致命的。
    if (m_layers.size() > 1) {
        const ConnectionTarget failedTarget = node.target();
        const bool releaseOuterShim = node.outerShimSuspended();
        discardPendingShellIntegration(node);
        m_layers.pop_back();
        discardPendingCredential();
        if (releaseOuterShim && !m_layers.empty()) {
            // 接管失败后外层 Shell 必须重新取得提示符：用户接下来还要在那里继续操作，
            // 而 TerminalPane 也要把原始 ssh 命令交还给它执行。
            m_layers.back()->queueOutgoing(ShellShim::resumeResponse());
        }
        if (!m_rootAnnounced) {
            if (step.legacyHmacRequired && m_connectionPlan.size() == 1) {
                emit legacyHmacRequired(step.message);
            } else {
                emit connectionError(
                    tr("主机链在 %1 失败：%2")
                        .arg(failedTarget.connectionSpecifier(true), step.message));
            }
            return false;
        }
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

SshSessionNode *SshSessionStack::originLayerFor(const QString &layerId)
{
    if (m_layers.empty()) {
        return nullptr;
    }
    if (layerId.isEmpty()) {
        return m_layers.back().get();
    }
    for (std::size_t index = 0; index < m_layers.size(); ++index) {
        if (m_layers[index]->target().id != layerId) {
            continue;
        }
        // 栈底由本机直连即可，不需要经任何一层转发。
        return index == 0 ? nullptr : m_layers[index - 1].get();
    }
    return nullptr;
}

SshSessionNode *SshSessionStack::logicalRootLayer()
{
    if (m_connectionPlan.isEmpty()
        || m_layers.size() < static_cast<std::size_t>(m_connectionPlan.size())) {
        return nullptr;
    }
    return m_layers[m_connectionPlan.size() - 1].get();
}

void SshSessionStack::servicePendingLayerRequests()
{
    int pops = 0;
    PendingPush push;
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
    while (pops > 0 && m_layers.size() > static_cast<std::size_t>(m_connectionPlan.size())) {
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
        const QString error = tr("当前没有可用于建立内层连接的已就绪会话");
        if (push.planned) {
            m_plannedFailure = error;
        } else {
            emit layerFailed(push.target, error);
        }
        return;
    }

    // 建层的转发时限刻意短：用户正盯着终端等待结果。主机名打错时远端解析可能一直不返回，
    // 十秒已经足够区分"慢"和"不通"。
    constexpr int kNestedForwardTimeoutMs = 10000;
    QString error;
    const quint64 forwardId = m_layers.back()->beginForward(
        push.target.host, push.target.port, true, kNestedForwardTimeoutMs, &error);
    if (forwardId == 0) {
        releaseSuspendedOuterShim(push);
        if (push.planned) {
            m_plannedFailure = tr("主机链在 %1 失败：%2")
                                   .arg(push.target.connectionSpecifier(true), error);
        } else {
            emit layerFailed(push.target, error);
        }
        return;
    }

    m_layerPush = push;
    m_layerPushForwardId = forwardId;
}

void SshSessionStack::handleForwardOutcome(const SshSessionNode::ForwardOutcome &outcome)
{
    if (outcome.id == m_layerPushForwardId && m_layerPushForwardId != 0) {
        const PendingPush push = m_layerPush;
        ConnectionTarget target = push.target;
        m_layerPushForwardId = 0;
        m_layerPush = {};

        if (!outcome.ok) {
            releaseSuspendedOuterShim(push);
            if (push.planned) {
                m_plannedFailure = tr("主机链在 %1 失败：%2")
                                       .arg(target.connectionSpecifier(true), outcome.error);
            } else {
                emit layerFailed(target, outcome.error);
            }
            return;
        }
        if (m_layers.empty()) {
            ::close(outcome.callerFd);
            const QString error = tr("承载该内层连接的会话已经关闭");
            if (push.planned) {
                m_plannedFailure = error;
            } else {
                emit layerFailed(target, error);
            }
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

        // 新层从干净状态开始：任何为上一层输入、却没来得及被消费的凭据都必须作废。
        discardPendingCredential();
        auto node = std::make_unique<SshSessionNode>(
            target,
            target.transit ? SshSessionNode::Role::Transit
                           : SshSessionNode::Role::Interactive);
        node->setOuterShimSuspended(push.outerShimSuspended);
        const SshSessionNode::StepResult start =
            node->beginEstablish(outcome.callerFd, columns, rows);
        if (!start.ok) {
            // 不手工关闭 callerFd：它已经交给 libssh 会话，由节点析构中的 ssh_free 统一
            // 释放，重复关闭会误伤后续复用同一编号的 fd。
            releaseSuspendedOuterShim(push);
            if (push.planned) {
                m_plannedFailure = tr("主机链在 %1 失败：%2")
                                       .arg(target.connectionSpecifier(true), start.message);
            } else {
                emit layerFailed(target, start.message);
            }
            return;
        }
        // 这里**绝不能**发 layersChanged：此刻该层只完成了选项配置，握手、指纹校验和认证
        // 都还没开始。提前公布会让文件管理器立刻对一个尚未认证的层发起 SFTP，进而弹出
        // 本不该出现的密码对话框。层就绪的通知由 advanceEstablishingLayers 统一发出。
        m_layers.push_back(std::move(node));
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

QByteArray SshSessionStack::shimPathFor(const SshSessionNode &node)
{
    // 每层一个独立文件名，避免同一台主机上的多个会话互相覆盖。取 UUID 前 8 位即可，
    // 路径要尽量短——终端里那条 source 命令越短越不会折行，也就越容易被精确扣掉。
    return "/tmp/.shm-" + node.target().id.left(8).toUtf8();
}

void SshSessionStack::serviceDeferredShellIntegration(SshSessionNode &top)
{
    if (m_pendingShellIntegration.isEmpty()
        || !m_pendingShellIntegration.contains(top.target().id)) {
        return;
    }
    // 静默阈值：登录横幅与提示符之间的间隔远小于它，而 readline 打印完提示符后就不会
    // 再有输出，因此"安静这么久"实际等价于"提示符已经画完、远端正在等键盘"。
    constexpr qint64 kShellIdleBeforeSourceMs = 300;
    if (!m_shellOutputClock.isValid()
        || m_shellOutputClock.elapsed() < kShellIdleBeforeSourceMs) {
        return;
    }
    m_pendingShellIntegration.remove(top.target().id);

    const QByteArray path = shimPathFor(top);
    // 先发路径事件再排队 source：两条信号与后续 outputReady 同源有序，GUI 得以在回显
    // 到达前登记过滤规则。
    emit shellIntegrationPathReady(path);
    top.queueOutgoing(ShellShim::sourceCommand(path) + "\n");
}

void SshSessionStack::discardPendingShellIntegration(const SshSessionNode &node)
{
    m_pendingShellIntegration.remove(node.target().id);
}

void SshSessionStack::beginShellIntegrationInstall(SshSessionNode &node)
{
    const QByteArray path = shimPathFor(node);
    if (!node.beginDetachedExec(ShellShim::installCommand(path))) {
        // 发起失败不影响会话本身，只是这一层退回命令解析的降级路径；显式上报可避免用户
        // 误以为随后识别出的推测层仍具有自持层同等的确定性。
        emit shellIntegrationFailed(node.target(), tr("无法创建远端脚本安装通道"));
        return;
    }
}

void SshSessionStack::serviceRemoteDistributionDetection()
{
    bool requested = false;
    {
        QMutexLocker locker(&m_mutex);
        requested = m_distributionDetectionRequested;
        // 请求一旦被工作线程接手就不再重试。读取失败通常意味着目标不是 Linux 或禁用了
        // exec；反复执行只会给每次登录增加额外网络往返，用户手动刷新主机配置后可再触发。
        if (requested) {
            m_distributionDetectionRequested = false;
        }
    }
    if (!requested || !m_distributionDetectionTargetId.isEmpty()) {
        return;
    }

    SshSessionNode *logicalRoot = logicalRootLayer();
    if (logicalRoot == nullptr || !logicalRoot->isReady()) {
        return;
    }
    if (logicalRoot->hasDetachedExecInFlight()) {
        // Shell 集成安装和发行版探测严格串行。下一个泵送周期会在安装任务得出结果后重试，
        // 因此不能在这里永久消费请求。
        QMutexLocker locker(&m_mutex);
        m_distributionDetectionRequested = true;
        return;
    }

    // 只读并使用 BusyBox、Debian、RHEL 通常均提供的 awk。刻意不 `. /etc/os-release`：即使
    // 远端文件遭篡改，也不能把其中的 Shell 语法作为当前已认证账号的命令执行。字段以独立
    // 两行输出，避免 PrettyName 中的空格或本地化文字影响图标映射和数据库字段边界。
    constexpr auto kDistributionCommand =
        "test -r /etc/os-release && awk -F= '$1==\"ID\" { id=$2; gsub(/^\"|\"$/, \"\", id) } "
        "$1==\"VERSION_ID\" { version=$2; gsub(/^\"|\"$/, \"\", version) } "
        "END { if (id != \"\") printf \"%s\\n%s\\n\", id, version; else exit 1 }' /etc/os-release";
    if (logicalRoot->beginDetachedExec(kDistributionCommand)) {
        m_distributionDetectionTargetId = logicalRoot->target().id;
    }
}

void SshSessionStack::pumpAllForwards()
{
    QList<SshSessionNode::ForwardOutcome> outcomes;
    QList<SshSessionNode::RemoteListenOutcome> remoteListenOutcomes;
    QList<SshSessionNode::RemoteAcceptedConnection> remoteAcceptedConnections;
    for (const std::unique_ptr<SshSessionNode> &layer : m_layers) {
        outcomes.append(layer->pumpForwards());
        layer->pumpRemoteForwarding(remoteListenOutcomes, remoteAcceptedConnections);
    }
    // 派发必须在遍历之后：handleForwardOutcome 可能压入新层，而 std::vector 的 push_back
    // 会让上面正在使用的迭代器失效。
    for (const SshSessionNode::ForwardOutcome &outcome : outcomes) {
        handleForwardOutcome(outcome);
    }
    for (const SshSessionNode::RemoteListenOutcome &outcome : remoteListenOutcomes) {
        const QSharedPointer<RemoteListenRequest> request =
            m_remoteListensInFlight.take(outcome.requestId);
        if (request.isNull()) {
            if (outcome.ok && !m_layers.empty()) {
                // 发起方已超时或取消；立即撤销晚到的成功监听，不能在服务器上留下孤儿端口。
                m_layers.front()->cancelRemoteListen(outcome.ruleId);
            }
            continue;
        }
        QMutexLocker requestLocker(&request->mutex);
        request->completed = true;
        request->succeeded = outcome.ok && !request->cancelled;
        request->error = request->succeeded
            ? QString()
            : (request->cancelled ? tr("远程监听申请已取消") : outcome.error);
        request->condition.wakeAll();
        if (outcome.ok && request->cancelled && !m_layers.empty()) {
            m_layers.front()->cancelRemoteListen(outcome.ruleId);
        }
    }
    for (const SshSessionNode::RemoteAcceptedConnection &connection
         : remoteAcceptedConnections) {
        emit remoteForwardConnection(connection.ruleId, connection.callerFd);
    }
}

void SshSessionStack::serviceKeepAlive()
{
    QList<SshKeepAlive::LayerIdleState> states;
    states.reserve(static_cast<qsizetype>(m_layers.size()));
    for (const std::unique_ptr<SshSessionNode> &layer : m_layers) {
        states.append(layer->keepAliveState());
    }

    for (const int index : SshKeepAlive::layersNeedingKeepAlive(states,
                                                                SshKeepAlive::kIdleIntervalMs)) {
        // 返回值刻意忽略：本轮写不出去只说明发送缓冲暂时满，下一轮再试即可。把它当成
        // 错误去断开会话，反而会亲手制造保活本应避免的那种断线。
        m_layers[static_cast<size_t>(index)]->sendKeepAlive();
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
        SshSessionNode *origin = request->rootOrigin
            ? logicalRootLayer()
            : originLayerFor(request->layerId);
        if (origin != nullptr && origin->isReady()) {
            // SFTP 侧调用方自身有 15 秒等待上限，这里略短以便先返回结构化错误而非超时。
            constexpr int kSftpForwardTimeoutMs = 12000;
            forwardId = origin->beginForward(
                request->host, request->port, false, kSftpForwardTimeoutMs, &error);
        } else {
            error = tr("承载该目标的外层 SSH 会话不可用");
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

void SshSessionStack::serviceRemoteForwardRequests()
{
    QList<QSharedPointer<RemoteListenRequest>> starts;
    QStringList stops;
    {
        QMutexLocker locker(&m_mutex);
        starts.swap(m_remoteListenRequests);
        stops.swap(m_pendingRemoteStops);
    }

    SshSessionNode *root = logicalRootLayer();
    for (const QString &ruleId : std::as_const(stops)) {
        if (root != nullptr) {
            root->cancelRemoteListen(ruleId);
        }
        // 停止可能与尚在途的启动并发；先唤醒调用方，再让晚到 outcome 走无人认领清理分支。
        QList<quint64> cancelledIds;
        for (auto iterator = m_remoteListensInFlight.cbegin();
             iterator != m_remoteListensInFlight.cend(); ++iterator) {
            if (iterator.value()->ruleId == ruleId) {
                cancelledIds.append(iterator.key());
            }
        }
        for (const quint64 requestId : cancelledIds) {
            const QSharedPointer<RemoteListenRequest> request =
                m_remoteListensInFlight.take(requestId);
            QMutexLocker requestLocker(&request->mutex);
            request->cancelled = true;
            request->completed = true;
            request->error = tr("远程监听申请已停止");
            request->condition.wakeAll();
        }
    }

    for (const QSharedPointer<RemoteListenRequest> &request : std::as_const(starts)) {
        QMutexLocker requestLocker(&request->mutex);
        if (request->cancelled) {
            request->completed = true;
            request->error = tr("远程监听申请已取消");
            request->condition.wakeAll();
            continue;
        }
        if (root == nullptr || !root->isReady()) {
            request->completed = true;
            request->error = tr("根 SSH 会话尚未就绪");
            request->condition.wakeAll();
            continue;
        }
        QString error;
        const quint64 requestId = root->beginRemoteListen(request->ruleId,
                                                          request->bindHost,
                                                          request->bindPort,
                                                          &error);
        if (requestId == 0) {
            request->completed = true;
            request->error = error;
            request->condition.wakeAll();
            continue;
        }
        m_remoteListensInFlight.insert(requestId, request);
    }
}

void SshSessionStack::discardPendingCredential()
{
    qCDebug(authLog, "discardPendingCredential (had=%d)", int(m_hasPendingCredential));
    // 凭据只对提交时正在等待的那一层有意义。不清除的话，上一层被 Ctrl+C 取消时提交的空串
    // 会一直排在队列里，把下一次新建的层在刚进入等待状态时立刻判成"用户取消"——用户看到
    // 的现象是从此再也连不上任何内层服务器。
    QMutexLocker locker(&m_mutex);
    m_pendingCredential.clear();
    m_hasPendingCredential = false;
}

void SshSessionStack::abortInFlightForwards(const QString &reason)
{
    m_layerPushForwardId = 0;
    m_layerPush = {};

    const QList<QSharedPointer<ForwardRequest>> orphans = m_forwardsInFlight.values();
    m_forwardsInFlight.clear();
    for (const QSharedPointer<ForwardRequest> &request : orphans) {
        QMutexLocker requestLocker(&request->mutex);
        request->error = reason;
        request->clientFd = -1;
        request->completed = true;
        request->condition.wakeAll();
    }

    QList<QSharedPointer<RemoteListenRequest>> remoteRequests =
        m_remoteListensInFlight.values();
    m_remoteListensInFlight.clear();
    {
        QMutexLocker locker(&m_mutex);
        remoteRequests.append(m_remoteListenRequests);
        m_remoteListenRequests.clear();
        m_pendingRemoteStops.clear();
    }
    for (const QSharedPointer<RemoteListenRequest> &request : std::as_const(remoteRequests)) {
        QMutexLocker requestLocker(&request->mutex);
        request->completed = true;
        request->succeeded = false;
        request->error = reason;
        request->condition.wakeAll();
    }
}

ConnectionTarget SshSessionStack::popTopLayer()
{
    if (m_layers.empty()) {
        return {};
    }
    ConnectionTarget target = m_layers.back()->target();
    const bool releaseOuterShim = m_layers.back()->outerShimSuspended();
    discardPendingShellIntegration(*m_layers.back());
    m_layers.back()->closeShell();
    m_layers.pop_back();
    if (releaseOuterShim && !m_layers.empty()) {
        // 接管期间外层的 shim 一直挂在 read 上，正是它替代了原生 ssh 的"外层挂起"语义。
        // 内层结束后必须放行，外层 Shell 才会重新画出提示符。
        m_layers.back()->queueOutgoing(ShellShim::resumeResponse());
    }
    return target;
}

void SshSessionStack::releaseSuspendedOuterShim(const PendingPush &push)
{
    if (!push.outerShimSuspended || m_layers.empty()) {
        return;
    }
    // 建层在真正压栈之前就失败了，没有节点可以携带这个标志；外层的 shim 却已经挂起，
    // 必须就地放行，否则那个终端再也回不到提示符。
    m_layers.back()->queueOutgoing(ShellShim::resumeResponse());
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
