#include "ssh/SshSessionStack.h"

#include "ssh/SshSessionNode.h"

#include <QMutexLocker>

#include <libssh/libssh.h>

#include <algorithm>

#include <unistd.h>

namespace {

/** @brief 直连模式下的 SSH 握手超时；复用隧道时由外层会话的存活状态决定。 */
constexpr long kConnectTimeoutSeconds = 15;

} // namespace

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
    // 如果线程正等待用户确认指纹，必须同时唤醒，否则关闭窗口会一直等待对话框结果。
    m_hostKeyCondition.wakeAll();
}

void SshSessionStack::confirmUnknownHost(const bool trusted)
{
    QMutexLocker locker(&m_mutex);
    if (!m_waitingForHostKey) {
        return;
    }
    m_hostKeyTrusted = trusted;
    m_waitingForHostKey = false;
    m_hostKeyCondition.wakeAll();
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
    auto root = std::make_unique<SshSessionNode>(m_rootTarget);
    if (!establishLayer(*root, -1)) {
        // establishLayer 已经发出具体的失败或降级信号。此路径不发 shellDisconnected：
        // 连接从未建立，界面不应据此自动退回本地 Shell。
        return;
    }
    m_layers.push_back(std::move(root));
    emit shellConnected();

    const bool remoteShellEnded = pumpLoop();

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

bool SshSessionStack::establishLayer(SshSessionNode &node, const int transportFd)
{
    SshSessionNode::StepResult step = node.connectTransport(transportFd, kConnectTimeoutSeconds);
    if (!step.ok) {
        if (step.legacyHmacRequired) {
            emit legacyHmacRequired(step.message);
        } else {
            emit connectionError(step.message);
        }
        return false;
    }

    step = node.verifyHostKey();
    if (step.hostKeyUnknown) {
        {
            QMutexLocker locker(&m_mutex);
            m_waitingForHostKey = true;
            m_hostKeyTrusted = false;
        }
        emit hostKeyVerificationRequired(node.target(), step.fingerprint);

        bool trusted = false;
        {
            QMutexLocker locker(&m_mutex);
            while (m_waitingForHostKey && !m_stopRequested) {
                m_hostKeyCondition.wait(&m_mutex);
            }
            trusted = m_hostKeyTrusted && !m_stopRequested;
        }
        if (!trusted) {
            emit connectionError(tr("用户拒绝了未知服务器主机密钥。"));
            return false;
        }
        step = node.acceptHostKey();
    }
    if (!step.ok) {
        emit connectionError(step.message);
        return false;
    }

    step = node.authenticate();
    if (!step.ok) {
        emit connectionError(step.message);
        return false;
    }

    int columns = 80;
    int rows = 24;
    {
        QMutexLocker locker(&m_mutex);
        columns = m_columns;
        rows = m_rows;
        m_sizeDirty = false;
    }
    step = node.openShell(columns, rows);
    if (!step.ok) {
        emit connectionError(step.message);
        return false;
    }
    return true;
}

bool SshSessionStack::pumpLoop()
{
    QByteArray readBuffer(64 * 1024, Qt::Uninitialized);

    // 显式判空而不是只依赖 Q_ASSERT：PR-2 引入弹栈后，最后一层被移除的那一轮循环必须
    // 干净退出，而 Q_ASSERT 在 Release 构建中会被消除并留下越界访问。
    while (!shouldStop() && !m_layers.empty() && activeLayer().isShellAlive()) {
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

        serviceForwardRequests();

        if (resizeRequired) {
            activeLayer().resizePty(columns, rows);
        }

        qsizetype writeOffset = 0;
        while (writeOffset < pendingInput.size() && !shouldStop()) {
            const int written = activeLayer().writeToShell(
                pendingInput.constData() + writeOffset,
                static_cast<int>(pendingInput.size() - writeOffset));
            if (written == SSH_AGAIN || written == 0) {
                // 非阻塞通道暂时不可写不是失败；把 SSH_AGAIN(-2) 加到偏移量会让下一次写入
                // 从缓冲区之前读取，表现为回车偶尔有效而普通命令丢失。
                QThread::msleep(5);
                continue;
            }
            if (written < 0) {
                emit connectionError(tr("向远端终端写入数据失败：%1")
                                         .arg(activeLayer().lastError()));
                return false;
            }
            writeOffset += written;
        }

        bool receivedData = false;
        for (const bool stderrStream : {false, true}) {
            while (true) {
                const int bytesRead = activeLayer().readFromShell(
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

        // 所有层的隧道都要泵送：内层会话的传输本身就架在外层的转发通道之上，只泵送栈顶
        // 会让内层连接失去承载。
        for (const std::unique_ptr<SshSessionNode> &layer : m_layers) {
            layer->pumpForwards();
        }
    }
    return true;
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

    const auto abortPredicate = [this]() { return shouldStop(); };
    for (const QSharedPointer<ForwardRequest> &request : requests) {
        int callerFd = -1;
        QString error;
        const bool opened = activeLayer().openForward(
            request->host, request->port, &callerFd, abortPredicate, &error);

        QMutexLocker requestLocker(&request->mutex);
        if (!opened || request->cancelled) {
            if (opened) {
                // 请求方已超时离开，通道无人认领，立即关闭调用方一端避免 fd 泄漏。
                ::close(callerFd);
            }
            request->error = request->cancelled ? tr("SFTP 隧道请求已取消") : error;
            request->clientFd = -1;
        } else {
            request->clientFd = callerFd;
        }
        request->completed = true;
        request->condition.wakeAll();
    }
}

SshSessionNode &SshSessionStack::activeLayer()
{
    Q_ASSERT(!m_layers.empty());
    return *m_layers.back();
}

bool SshSessionStack::shouldStop() const
{
    QMutexLocker locker(&m_mutex);
    return m_stopRequested;
}
