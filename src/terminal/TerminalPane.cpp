#include "terminal/TerminalPane.h"

#include "i18n/Retranslate.h"
#include "ssh/ShellShim.h"
#include "ssh/SshCommandParser.h"
#include "ssh/SshSessionStack.h"
#include "ssh/SshTunnelBroker.h"
#include "settings/TerminalSettings.h"
#include "storage/ApplicationDatabase.h"
#include "terminal/LocalShellWorker.h"
#include "terminal/TerminalView.h"
#include "ui/UiIcons.h"

#include <QHBoxLayout>
#include <QLoggingCategory>
#include <QLabel>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {
/** @brief 与 TerminalView 共用的认证诊断通道；见该文件中的说明。 */
Q_LOGGING_CATEGORY(authLog, "shellium.auth", QtWarningMsg)
} // namespace

TerminalPane::TerminalPane(QWidget *parent)
    : QWidget(parent)
    , m_headerBar(new QWidget(this))
    , m_headerLabel(new QLabel(tr("Not connected"), this))
    , m_statusMessageLabel(new QLabel(this))
    , m_statusMessageIcon(new QLabel(this))
    , m_statusMessageTimer(new QTimer(this))
    , m_fileManagerButton(new QToolButton(this))
    , m_headerExtras(new QWidget(this))
    , m_terminalView(new TerminalView(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // 会话顶部使用单独的状态栏容器：左侧服务器信息可伸缩，编码/尺寸和文件开关始终停靠
    // 在右侧。这样窗口变窄时只会省略长主机名，不会把关键开关挤出可视区域。
    m_headerBar->setObjectName(QStringLiteral("terminalSessionStatusBar"));
    auto *headerLayout = new QHBoxLayout(m_headerBar);
    headerLayout->setContentsMargins(10, 4, 6, 4);
    headerLayout->setSpacing(8);
    m_headerLabel->setObjectName(QStringLiteral("terminalSessionServer"));
    m_headerLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    // 与服务器名同一行、紧随其后：SFTP 目录读取、命令历史写入这类瞬时提示只与当前标签页
    // 有关，放在这里比塞进窗口标题栏的全局提示区更贴近用户正在看的上下文。
    m_statusMessageLabel->setObjectName(QStringLiteral("terminalSessionTransientStatus"));
    m_statusMessageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // 瞬时提示也让位给右侧按钮：窄宽度时它先被压缩，绝不把关键开关顶出可视区。
    m_statusMessageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_statusMessageIcon->setObjectName(QStringLiteral("terminalSessionTransientStatusIcon"));
    m_statusMessageIcon->setFixedSize(14, 14);
    m_statusMessageIcon->setVisible(false);
    m_statusMessageTimer->setSingleShot(true);
    connect(m_statusMessageTimer, &QTimer::timeout, this, [this]() {
        m_statusMessageLabel->clear();
        m_statusMessageIcon->clear();
        m_statusMessageIcon->setVisible(false);
    });
    m_fileManagerButton->setObjectName(QStringLiteral("terminalFileManagerToggle"));
    m_fileManagerButton->setCheckable(true);
    // 远程文件管理默认收起：多数会话只用终端，一进来就占掉三分之一宽度并立刻发起 SFTP
    // 连接既浪费空间也浪费一条链路。
    m_fileManagerButton->setChecked(false);
    m_fileManagerButton->setAutoRaise(true);
    m_fileManagerButton->setIconSize(QSize(18, 18));
    m_fileManagerButton->setToolTip(tr("Show the remote file manager for the current session (Ctrl+Shift+F)"));
    m_fileManagerButton->setAccessibleName(tr("Remote file manager"));
    updateFileManagerButtonIcon();
    // 会话状态栏最右侧的外挂动作容器：单终端标签为空、零占位；工作空间给成员挂上
    // 广播 / 布局切换 / 关闭按钮。Fixed 策略保证宽度不够时它整体不被压缩——右上角
    // 的按钮必须永远完整可见，被牺牲的只能是左侧可省略的服务器名和瞬时提示。
    auto *headerExtrasLayout = new QHBoxLayout(m_headerExtras);
    headerExtrasLayout->setContentsMargins(0, 0, 0, 0);
    headerExtrasLayout->setSpacing(2);
    m_headerExtras->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    headerLayout->addWidget(m_headerLabel, 1);
    headerLayout->addWidget(m_statusMessageIcon);
    headerLayout->addWidget(m_statusMessageLabel);
    headerLayout->addWidget(m_fileManagerButton);
    headerLayout->addWidget(m_headerExtras, 0, Qt::AlignRight);
    layout->addWidget(m_headerBar);
    layout->addWidget(m_terminalView, 1);

    connect(m_fileManagerButton, &QToolButton::toggled, this, [this](const bool visible) {
        updateFileManagerButtonIcon();
        emit fileManagerVisibilityRequested(visible);
    });
    connect(m_terminalView, &TerminalView::inputBytes, this, [this](const QByteArray &bytes) {
        if (m_awaitingManualRecovery) {
            // 断线提示期间的按键属于本地选择，不是会话输入：既不能写入任何 PTY，也绝不能
            // 广播给其他面板——那会把一次"重连还是关闭"的抉择变成对所有服务器的误操作。
            handleManualRecoveryInput(bytes);
            return;
        }
        // 先广播源输入，再写本终端；上层把相同字节送给其他面板时调用 sendInputBytes，后者
        // 不再次发信号，因此不会形成 A→B→A 的循环广播。
        emit broadcastSourceInput(this, bytes);
        sendInputBytes(bytes);
    });
    connect(m_terminalView,
            &TerminalView::commandSubmitted,
            this,
            &TerminalPane::inspectSubmittedCommand);
    connect(m_terminalView,
            &TerminalView::authenticationPromptDetected,
            this,
            [this]() {
                if (!m_pendingNestedCredentialTargetId.isEmpty()) {
                    // 只有刚提交过嵌套 ssh 命令时才允许捕获下一行，普通 sudo、数据库或应用
                    // 密码提示不会进入 SFTP 凭据缓存。
                    m_captureNextLineAsCredential = true;
                    emit statusMessage(tr("Waiting for the inner SSH password; this entry is also used for the SFTP session"));
                }
            });
    connect(m_terminalView,
            &TerminalView::localLineSubmitted,
            this,
            [this](const QString &line, const bool cancelled) {
                qCDebug(authLog, "localLineSubmitted len=%lld cancelled=%d stackRunning=%d",
                        static_cast<long long>(line.size()), int(cancelled),
                        int(m_sessionStack != nullptr && m_sessionStack->isRunning()));
                if (m_sessionStack == nullptr || !m_sessionStack->isRunning()) {
                    return;
                }
                if (!cancelled && line.isEmpty() && !m_pendingCredentialPrompt.isEmpty()) {
                    // 在密码提示处直接回车，OpenSSH 的行为是重新提示而不是中止。这样任何
                    // 一次输入丢失都只是多提示一遍，而不会升级成不可恢复的连接失败。
                    m_terminalView->appendTerminalOutput(m_pendingCredentialPrompt.toUtf8());
                    m_terminalView->beginLocalLineCapture();
                    return;
                }
                m_pendingCredentialPrompt.clear();
                m_sessionStack->provideLayerCredential(line);
            });
    connect(m_terminalView,
            &TerminalView::shellIntegrationEvent,
            this,
            &TerminalPane::handleShellIntegrationEvent);
    connect(m_terminalView,
            &TerminalView::nestedSshFailed,
            this,
            [this]() { rollBackPendingNestedTarget(tr("Inner SSH connection failed")); });
    connect(m_terminalView,
            &TerminalView::nestedSshAborted,
            this,
            [this]() {
                // 只有确实存在未确认的嵌套目标时，Ctrl+C 才被理解为"取消这次 ssh"；
                // 否则它就是普通的命令中断，不该影响连接链。
                if (!m_pendingNestedCredentialTargetId.isEmpty()) {
                    rollBackPendingNestedTarget(tr("Inner SSH connection cancelled"));
                }
            });
    connect(m_terminalView,
            &TerminalView::shellPromptDetected,
            this,
            [this]() {
                if (m_pendingNestedCredentialTargetId.isEmpty()
                    || m_captureNextLineAsCredential) {
                    // password 提示已出现但用户尚未按回车时，屏幕中任何旧提示符都不能被
                    // 当作认证成功，否则 SFTP 会抢先尝试公钥认证并弹出第二个密码框。
                    return;
                }

                bool credentialWasSubmitted = false;
                for (ConnectionTarget &target : m_inferredTargets) {
                    if (target.id == m_pendingNestedCredentialTargetId) {
                        credentialWasSubmitted = !target.password.isEmpty();
                        if (!credentialWasSubmitted) {
                            // 没有捕获过密码的目标可能使用公钥或 Agent；这类连接由五秒免密
                            // 观察定时器开放，不能被命令提交瞬间残留的外层提示符提前确认。
                            break;
                        }
                        // 只有内层 Shell 提示符已经出现，才把目标交给文件管理器。这样既不会
                        // 在密码回车尚未送达远端时抢先建立 SFTP，也允许错误密码继续重试。
                        target.interactiveAuthenticationPending = false;
                        break;
                    }
                }
                if (!credentialWasSubmitted) {
                    return;
                }
                m_pendingNestedCredentialTargetId.clear();
                m_captureNextLineAsCredential = false;
                emit targetsChanged(this);
                emit statusMessage(tr("Inner SSH authentication succeeded; the file manager has switched to the current server"));
            });

    // 语言切换时重设终端标题栏的固定文案。已连接会话的标题是 host:port（与语言无关），
    // 由 refreshHeaderLabel() 兜底；连接中/失败等瞬时提示会在下一次状态变化时转为新语言。
    i18n::onRetranslate(this, [this] {
        m_fileManagerButton->setToolTip(
            tr("Show the remote file manager for the current session (Ctrl+Shift+F)"));
        m_fileManagerButton->setAccessibleName(tr("Remote file manager"));
        refreshHeaderLabel();
    });
}

void TerminalPane::openLocalShell(const QString &shellPath)
{
    // 用户主动打开本地终端或重连次数耗尽都终止恢复周期；递增令牌可让尚在事件队列中的
    // 旧重连 lambda 只退出而不覆盖新本地 Shell。
    m_reconnectCycleActive = false;
    m_reconnectAttempts = 0;
    ++m_reconnectToken;
    m_awaitingManualRecovery = false;
    // 主动打开本地终端代表开始新会话；递增代次后，旧 SSH 已排队的断开回调无法再触发
    // 恢复流程，从而不会重复创建本地 Shell。
    const quint64 sessionGeneration = ++m_sessionGeneration;
    stopWorker();
    m_ownedTargets.clear();
    m_inferredTargets.clear();
    refreshCommandCompletions();
    m_echoFilter.reset();
    m_shellReady = false;
    // 切换成本地 Shell 后当前终端不再具备远程目标，立即通知所属标签页清空自己的文件
    // 管理器，不能等 zsh 启动完成后继续短暂显示上一条 SSH 会话的目录。
    emit targetsChanged(this);
    m_headerLabel->setText(tr("Starting local terminal…"));
    m_terminalView->appendTerminalOutput(tr("Starting local terminal…\r\n").toUtf8());

    m_localWorker = new LocalShellWorker(shellPath, this);
    connect(m_localWorker,
            &LocalShellWorker::outputReady,
            this,
            [this, sessionGeneration](const QByteArray &bytes) {
                // 旧 PTY 在线程结束前可能仍有少量输出排队；会话代次不匹配时丢弃，避免
                // 污染用户已经切换到的新 SSH 或本地终端画面。
                if (sessionGeneration == m_sessionGeneration) {
                    m_terminalView->appendTerminalOutput(bytes);
                }
            });
    connect(m_localWorker,
            &LocalShellWorker::shellStarted,
            this,
            [this, sessionGeneration]() {
        if (sessionGeneration != m_sessionGeneration) {
            return;
        }
        m_shellReady = true;
        m_headerLabel->setText(tr("Local terminal"));
        emit targetsChanged(this);
        emit statusMessage(tr("Local terminal started"));
    });
    connect(m_localWorker,
            &LocalShellWorker::shellError,
            this,
            [this, sessionGeneration](const QString &message) {
        if (sessionGeneration != m_sessionGeneration) {
            return;
        }
        m_shellReady = false;
        m_headerLabel->setText(tr("Local terminal error"));
        m_terminalView->appendTerminalOutput(
            (QStringLiteral("\r\n") + message + QStringLiteral("\r\n")).toUtf8());
        emit statusMessage(message);
    });
    connect(m_localWorker,
            &LocalShellWorker::shellFinished,
            this,
            [this, sessionGeneration](const int exitCode) {
        if (sessionGeneration != m_sessionGeneration) {
            return;
        }
        m_shellReady = false;
        const QString message = tr("Local terminal ended (exit code %1)").arg(exitCode);
        m_headerLabel->setText(message);
        emit targetsChanged(this);
        emit statusMessage(message);
    });
    connect(m_terminalView,
            &TerminalView::terminalSizeChanged,
            m_localWorker,
            &LocalShellWorker::updateTerminalSize);
    // TerminalView 可能在本地 Shell 创建前就完成了布局，此时不会为了新 worker 人为再发
    // 一次 resize 信号。启动前显式注入当前稳定尺寸，确保 forkpty 从第一帧就使用正确网格。
    m_localWorker->updateTerminalSize(m_terminalView->terminalColumns(),
                                      m_terminalView->terminalRows());
    m_localWorker->start();
    focusTerminal();
}

TerminalPane::~TerminalPane()
{
    stopWorker();
}

void TerminalPane::connectToHost(const ConnectionTarget &target)
{
    // 连接对话框创建的是临时目标，不应把上一次主机树连接遗留的“首次探测”开关带过去。
    m_remoteDistributionDetectionEnabled = false;
    connectToHosts(QList<ConnectionTarget>{target});
}

void TerminalPane::setRemoteDistributionDetectionEnabled(const bool enabled)
{
    // 开关只由 GUI 线程在发起连接前设置，并由紧随其后的 connectToHosts() 消费；不需要额外
    // 锁。真正跨线程的请求交接由 SshSessionStack::requestRemoteDistributionDetection() 加锁。
    m_remoteDistributionDetectionEnabled = enabled;
}

void TerminalPane::connectToHosts(const QList<ConnectionTarget> &connectionPlan)
{
    if (connectionPlan.isEmpty() || !connectionPlan.constLast().isValid()) {
        return;
    }
    const ConnectionTarget target = connectionPlan.constLast();
    // 把一次性配置在创建 worker 前取走。自动重连会直接复用连接计划，但不应在识别结果已
    // 写入或一次探测失败后继续额外执行远端命令。
    const bool shouldDetectRemoteDistribution = m_remoteDistributionDetectionEnabled;
    m_remoteDistributionDetectionEnabled = false;
    // 无论重连由提示、菜单还是退避定时器发起，提示都已经完成使命；继续拦截键盘会让新
    // 会话第一个字符消失。
    m_awaitingManualRecovery = false;
    if (!m_automaticReconnectInvocation) {
        // 菜单、主机树或工作区发起的新连接代表用户改变意图，必须取消旧异常恢复周期。
        m_reconnectCycleActive = false;
        m_reconnectAttempts = 0;
        ++m_reconnectToken;
        // 用户主动连接时替换整条计划；自动重连调用则保留原快照，防止中途只剩最终目标。
        m_connectionPlan = connectionPlan;
    }
    // 在停止旧工作线程前先推进代次，保证旧线程随后发出的 shellDisconnected 被识别为
    // 过期事件，不能中断这次新连接。
    const quint64 sessionGeneration = ++m_sessionGeneration;
    stopWorker();

    m_shellReady = false;
    m_ownedTargets.clear();
    m_inferredTargets.clear();
    refreshCommandCompletions();
    // 旧会话登记的期望串永远不会再出现；继续持有只会让新会话的第一段输出被误扣。
    m_echoFilter.reset();
    // 新 SSH 尚未完成主机校验和认证，此时先让所属标签页清空旧 SFTP 内容；shellConnected
    // 会在认证成功后再次发出 targetsChanged 并开放该目标。
    emit targetsChanged(this);
    m_headerLabel->setText(tr("Connecting to %1…").arg(target.connectionSpecifier()));
    // 终端控制字符由代码追加，不能放入译文：翻译工具可能丢失 CR，导致 LF 只下移
    // 一行却不回到第 0 列，连接期间的光标和远端首行因此停在提示末尾的列。复用标题的
    // 纯文本翻译，让所有语言都获得相同的 CRLF 语义。
    m_terminalView->appendTerminalOutput(
        tr("Connecting to %1…").arg(target.connectionSpecifier()).toUtf8()
        + QByteArrayLiteral("\r\n"));

    m_sessionStack = new SshSessionStack(connectionPlan, true, this);
    connect(m_sessionStack,
            &SshSessionStack::outputReady,
            this,
            [this, sessionGeneration](const QByteArray &bytes) {
                if (sessionGeneration == m_sessionGeneration) {
                    m_terminalView->appendTerminalOutput(m_echoFilter.filter(bytes));
                }
            });
    connect(m_sessionStack,
            &SshSessionStack::shellConnected,
            this,
            [this, target, sessionGeneration]() {
        if (sessionGeneration != m_sessionGeneration) {
            return;
        }
        m_shellReady = true;
        m_remoteSessionEstablished = true;
        // 一旦重连真正完成认证，本轮故障已经恢复；后续再次异常断线应获得全新的尝试额度。
        m_reconnectCycleActive = false;
        m_reconnectAttempts = 0;
        ++m_reconnectToken;
        m_headerLabel->setText(target.connectionSpecifier());
        emit statusMessage(tr("Connected to %1").arg(target.connectionSpecifier()));
        emit rootConnectionSucceeded(target);
        // 这里刻意不发 targetsChanged：此刻 m_ownedTargets 仍为空，发出去会让文件管理器
        // 先清空再被紧随其后的 layersChanged 重新填充，用户能看到一次闪烁。栈保证
        // layersChanged 紧跟在 shellConnected 之后到达，由它统一通知即可。
    });
    connect(m_sessionStack,
            &SshSessionStack::remoteDistributionDetected,
            this,
            [this, target, sessionGeneration](const QString &distributionId,
                                               const QString &versionId) {
                if (sessionGeneration == m_sessionGeneration) {
                    emit remoteDistributionDetected(target, distributionId, versionId);
                }
            });
    connect(m_sessionStack,
            &SshSessionStack::layersChanged,
            this,
            [this, sessionGeneration](const QList<ConnectionTarget> &layers) {
        if (sessionGeneration != m_sessionGeneration) {
            return;
        }
        // 自持层是权威事实，直接替换。底座一变，叠在上面的推测层就失去了依据——它们描述的
        // 是"从旧的栈顶又 ssh 到了哪里"，而栈顶已经不是原来那个了。
        const bool grew = layers.size() > m_ownedTargets.size();
        m_ownedTargets = QVector<ConnectionTarget>(layers.cbegin(), layers.cend());
        registerOwnedLayerRoutes();
        m_inferredTargets.clear();
        m_pendingNestedCredentialTargetId.clear();
        m_captureNextLineAsCredential = false;
        refreshHeaderLabel();
        refreshCommandCompletions();
        emit targetsChanged(this);


    });
    connect(m_sessionStack,
            &SshSessionStack::shellIntegrationPathReady,
            this,
            [this, sessionGeneration](const QByteArray &path) {
        if (sessionGeneration != m_sessionGeneration) {
            return;
        }
        // 脚本正文走独立 exec 通道，不经过终端；交互式 Shell 里只会出现这条极短的 source
        // 命令，登记后由过滤器扣掉，用户什么也看不到。回车的回显 \r\n 一并登记，否则会在
        // 登录横幅和第一个提示符之间留下一个来路不明的空行。
        //
        // 但只扣回显还不够：远端执行完这条命令必然再打印一个提示符，于是登录时的提示符和
        // 它并排出现，用户看到的就是两个 `[root@host ~]#`。用擦行序列替换被扣掉的回显，让
        // 新提示符补在旧提示符原位，净效果仍是一个。
        const QByteArray sourceEcho =
            ShellShim::sourceCommand(path) + QByteArrayLiteral("\r\n");
        m_echoFilter.expect(sourceEcho, ShellShim::sourceCommandEchoReplacement());
        // 兜底第二次回显：会话栈会等远端 Shell 静默后才下发，正常情况下 readline 已经
        // 接管、只回显一次；但登录脚本中途停顿超过静默阈值时，这条命令仍可能赶在 readline
        // 之前抵达，于是行规程回显一次、readline 连同提示符再重画一次。第二次登记成可选，
        // 没发生就悄无声息地过期，发生了才把它连同被重画的那个提示符一起收拾掉。
        m_echoFilter.expectOptional(sourceEcho, ShellShim::sourceCommandEchoReplacement());
    });
    connect(m_sessionStack,
            &SshSessionStack::shellIntegrationFailed,
            this,
            [this, sessionGeneration](const ConnectionTarget &failedTarget,
                                      const QString &message) {
        if (sessionGeneration != m_sessionGeneration) {
            return;
        }
        // Shell 集成失败不会中断 SSH；清楚告知已进入降级识别，避免用户把后续推测层当成
        // 已由程序认证的确定层。完整原因保留在状态消息中，便于定位权限或 Shell 兼容问题。
        emit statusMessage(
            tr("Shell integration for %1 is not enabled; nested detection is degraded: %2")
                .arg(failedTarget.connectionSpecifier(), message));
    });
    connect(m_sessionStack,
            &SshSessionStack::layerCredentialRequired,
            this,
            [this, sessionGeneration](const ConnectionTarget &credentialTarget,
                                      const bool retry) {
        if (sessionGeneration != m_sessionGeneration) {
            return;
        }
        // 在终端里内联提示，形式与 OpenSSH 一致。绝不弹对话框：用户刚敲完 ssh，预期的是
        // 终端里的 password 提示，一个窗口会彻底打断这个心智模型。
        const QString prompt = retry
            ? tr("Permission denied, please try again.\r\n%1's password: ")
                  .arg(credentialTarget.connectionSpecifier())
            : tr("%1's password: ").arg(credentialTarget.connectionSpecifier());
        qCDebug(authLog, "layerCredentialRequired host=%s retry=%d",
                qPrintable(credentialTarget.connectionSpecifier(true)), int(retry));
        // 记住提示语：用户直接回车时要原样再问一遍，而不是把空行当成放弃。
        m_pendingCredentialPrompt = tr("%1's password: ")
                                        .arg(credentialTarget.connectionSpecifier());
        m_terminalView->appendTerminalOutput(prompt.toUtf8());
        m_terminalView->beginLocalLineCapture();
    });
    connect(m_sessionStack,
            &SshSessionStack::layerFailed,
            this,
            [this, sessionGeneration](const ConnectionTarget &target, const QString &message) {
        if (sessionGeneration != m_sessionGeneration) {
            return;
        }
        if (target.id != m_takeoverTargetId) {
            // 内层失败不影响外层：这里只报告，不改变任何连接状态。
            m_terminalView->appendTerminalOutput(
                tr("\r\nCould not establish the nested connection to %1: %2\r\n")
                    .arg(target.connectionSpecifier(), message)
                    .toUtf8());
            emit statusMessage(tr("Nested connection failed: %1").arg(message));
            return;
        }

        // 这次失败源于我们接管了用户手打的 ssh。外层服务器若禁用了 TCP 转发
        // （AllowTcpForwarding no），本程序就无法为它建立内层会话——记下来，之后不再
        // 对这台服务器尝试接管，用户不必反复经历同一个失败。
        QString reason = message;
        if (message.contains(QStringLiteral("error (1)"))) {
            // RFC 4254 的 SSH_OPEN_ADMINISTRATIVELY_PROHIBITED：服务端按策略拒绝，
            // 而非目标不可达。
            reason = tr("This server has TCP forwarding disabled");
            if (!m_takeoverOuterKey.isEmpty()) {
                m_forwardingProhibitedHosts.insert(m_takeoverOuterKey);
            }
        }
        emit statusMessage(tr("The nested connection was not taken over: %1").arg(reason));
        runTakeoverFallback(reason);
    });
    connect(m_sessionStack,
            &SshSessionStack::connectionError,
            this,
            [this, target, sessionGeneration](const QString &message) {
        if (sessionGeneration != m_sessionGeneration) {
            return;
        }
        const bool wasEstablished = m_remoteSessionEstablished;
        m_shellReady = false;
        m_remoteSessionEstablished = false;
        m_headerLabel->setText(tr("Connection failed"));
        m_terminalView->appendTerminalOutput((QStringLiteral("\r\n") + message
                                              + QStringLiteral("\r\n")).toUtf8());
        emit statusMessage(message);
        if (wasEstablished) {
            // 运行期写失败不会再触发 shellDisconnected，但已成功历史仍必须补齐结束时间，
            // 端口转发也必须解绑；用 -1 结构化表示网络异常，不伪造远端退出码。
            emit targetsChanged(this);
            emit rootConnectionDisconnected(target, -1);
            if (m_automaticReconnectEnabled && m_reconnectMaxAttempts > 0) {
                m_reconnectCycleActive = true;
                m_reconnectAttempts = 0;
                if (scheduleAutomaticReconnect(target, sessionGeneration)) {
                    return;
                }
            }
            QTimer::singleShot(0, this, [this, target, sessionGeneration]() {
                if (sessionGeneration == m_sessionGeneration
                    && !m_remoteSessionEstablished) {
                    // 错误文本已在上面写入终端，提示只补按键说明。
                    beginManualRecovery(target, QString());
                }
            });
            return;
        }

        emit rootConnectionFailed(target, message);
        if (m_reconnectCycleActive) {
            scheduleAutomaticReconnect(target, sessionGeneration);
        }
    });
    connect(m_sessionStack,
            &SshSessionStack::legacyHmacRequired,
            this,
            [this, target, connectionPlan, sessionGeneration](const QString &error) {
        if (sessionGeneration != m_sessionGeneration) {
            return;
        }
        const QMessageBox::StandardButton answer = QMessageBox::warning(
            this,
            tr("The server only supports legacy MAC algorithms"),
            tr("Server %1 offers only hmac-sha1, which the default security policy cannot use to complete the connection.\n\nhmac-sha1 is obsolete; upgrading the server's SSH configuration is recommended. Enable this algorithm for this connection only and retry?\n\nDetails: %2")
                .arg(target.connectionSpecifier(true), error),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer == QMessageBox::Yes) {
            QList<ConnectionTarget> compatiblePlan = connectionPlan;
            // libssh 的协商错误信号目前不携带具体层；最终层是用户确认兼容的业务目标。若是
            // 中间跳板需要旧算法，错误文本仍会明确该节点，用户应编辑对应主机长期配置。
            compatiblePlan.last().allowLegacyHmacSha1 = true;
            // 当前槽由工作线程结束信号排队触发；延迟重连可让旧线程先完整退出并释放 libssh 句柄。
            QTimer::singleShot(0, this, [this, compatiblePlan, sessionGeneration]() {
                if (sessionGeneration == m_sessionGeneration) {
                    connectToHosts(compatiblePlan);
                }
            });
        } else {
            const QString message = tr("Connection cancelled: the server requires hmac-sha1, which is disabled. Upgrade the server configuration.");
            m_headerLabel->setText(tr("Connection failed"));
            m_terminalView->appendTerminalOutput(
                (QStringLiteral("\r\n") + message + QStringLiteral("\r\n")).toUtf8());
            emit statusMessage(message);
            emit rootConnectionFailed(target, message);
        }
    });
    connect(m_sessionStack,
            &SshSessionStack::shellDisconnected,
            this,
            [this, target, sessionGeneration](const int exitStatus) {
        if (sessionGeneration != m_sessionGeneration || !m_remoteSessionEstablished) {
            // 主动切换会话会先递增 generation 并清除 established；此类旧断开事件不能再
            // 修改标题、文件管理目标或启动额外的本地 Shell。
            return;
        }

        m_remoteSessionEstablished = false;
        m_shellReady = false;
        // 退出状态 -1 表示链路异常断开，而不是用户在远端敲了 exit：前者可以自动重连，后者
        // 不能。但两者都不再擅自退回本地 Shell——刚断开的服务器与本机毫无关系，顶上来的本地
        // Shell 会让用户下一条命令打在错误的机器上，因此一律停在手动恢复提示。
        const bool abnormal = exitStatus == -1;
        const bool shouldReconnect = abnormal && m_automaticReconnectEnabled
            && m_reconnectMaxAttempts > 0;
        const QString message = shouldReconnect
            ? tr("The SSH connection was interrupted by a network error; preparing to reconnect automatically")
            : abnormal
                ? tr("The SSH connection was interrupted by a network error")
                : tr("The SSH connection has ended (exit status %1)").arg(exitStatus);
        m_headerLabel->setText(message);
        m_terminalView->appendTerminalOutput((QStringLiteral("\r\n") + message
                                              + QStringLiteral("\r\n")).toUtf8());
        emit statusMessage(message);
        emit targetsChanged(this);
        emit rootConnectionDisconnected(target, exitStatus);

        if (shouldReconnect) {
            m_reconnectCycleActive = true;
            m_reconnectAttempts = 0;
            if (scheduleAutomaticReconnect(target, sessionGeneration)) {
                return;
            }
        }

        // shellDisconnected 在工作线程 run() 返回前发出。延迟到下一轮事件循环后再停止并
        // 销毁旧 worker，可避免 GUI 线程在信号处理期间等待尚未完成的发射栈。
        QTimer::singleShot(0, this, [this, target, sessionGeneration]() {
            if (sessionGeneration != m_sessionGeneration || m_remoteSessionEstablished) {
                return;
            }
            // 断开原因刚刚已经写进终端，这里再传一次只会让同一句话连着出现两遍。
            beginManualRecovery(target, QString());
        });
    });
    connect(m_sessionStack,
            &SshSessionStack::hostKeyVerificationRequired,
            this,
            [this, sessionGeneration](const ConnectionTarget &verificationTarget,
                                      const QString &fingerprint) {
                if (sessionGeneration != m_sessionGeneration) {
                    return;
                }
                const QMessageBox::StandardButton answer = QMessageBox::question(
                    this,
                    tr("Confirm the server host key"),
                    tr("Connecting to %1 for the first time.\n\nSHA-256 fingerprint:\n%2\n\nVerify the fingerprint through a trusted channel. Trust it and write it to known_hosts?")
                        .arg(verificationTarget.connectionSpecifier(true), fingerprint),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No);
                if (m_sessionStack != nullptr) {
                    m_sessionStack->confirmUnknownHost(answer == QMessageBox::Yes);
                }
            });
    connect(m_sessionStack,
            &SshSessionStack::hostKeyChangeVerificationRequired,
            this,
            [this, sessionGeneration](const ConnectionTarget &verificationTarget,
                                      const QString &fingerprint) {
                if (sessionGeneration != m_sessionGeneration) {
                    return;
                }
                // 变更提示必须明显区别于首次连接：用户只有在确认服务器重装或管理员主动
                // 轮换密钥后才能继续，不能把潜在的中间人攻击包装成普通确认框。
                const QMessageBox::StandardButton answer = QMessageBox::warning(
                    this,
                    tr("The server host key has changed"),
                    tr("The host key returned by %1 does not match the local record.\n\nNew SHA-256 fingerprint:\n%2\n\nThis may be a server reinstall or key rotation, or a man-in-the-middle attack. Verify the new fingerprint with the server administrator through a trusted channel first.\n\nConfirm the key has changed and replace the old record in known_hosts?")
                        .arg(verificationTarget.connectionSpecifier(true), fingerprint),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No);
                if (m_sessionStack != nullptr) {
                    m_sessionStack->confirmUnknownHost(answer == QMessageBox::Yes);
                }
            });
    connect(m_terminalView,
            &TerminalView::terminalSizeChanged,
            m_sessionStack,
            &SshSessionStack::updateTerminalSize);
    // 连接建立期间 beginEstablish 会直接读取栈内缓存尺寸；必须在 start() 前写入当前值，
    // 否则稳定窗口中打开的新会话会以默认 80×24 请求 PTY，直到下一次人为改变界面尺寸。
    m_sessionStack->updateTerminalSize(m_terminalView->terminalColumns(),
                                       m_terminalView->terminalRows());
    if (shouldDetectRemoteDistribution) {
        // 先登记再启动线程，确保极快的本地或跳板连接不会在 GUI 线程来得及排队请求前错过
        // 首个可用循环；SshSessionStack 仍会等认证完成并与 Shell 集成安装串行执行。
        m_sessionStack->requestRemoteDistributionDetection();
    }
    m_sessionStack->start();
    emit statusMessage(tr("Connecting to %1").arg(target.connectionSpecifier()));
    focusTerminal();
}

QList<ConnectionTarget> TerminalPane::targets() const
{
    // 自持层在下、推测层在上：这正是用户看到的外层→内层顺序，也保证文件管理器"跟随最内层"
    // 时选中的是当前终端实际所在的那一层。
    QList<ConnectionTarget> combined;
    for (const ConnectionTarget &target : m_ownedTargets) {
        // 受管跳板层只承担传输，不是用户终端实际所在的服务器。隐藏它们可避免文件管理器
        // 自动选择公网跳板并把上传、删除等操作发往错误主机；完整层仍保留在内部用于路由。
        if (!target.transit) {
            combined.append(target);
        }
    }
    combined.append(m_inferredTargets.toList());
    return combined;
}

QList<ConnectionTarget> TerminalPane::connectionPlan() const
{
    // 面板可能已被用户切回本地 Shell，但整条计划仍留给手动重连复用。若只看计划非空，
    // 复制一个早已回到本机的标签会凭空再拨一条 SSH，因此以当前是否跑着本地 Shell 为准。
    return m_localWorker != nullptr ? QList<ConnectionTarget>{} : m_connectionPlan;
}

ConnectionTarget TerminalPane::activeTarget() const
{
    if (!m_inferredTargets.isEmpty()) {
        return m_inferredTargets.constLast();
    }
    return m_ownedTargets.isEmpty() ? ConnectionTarget{} : m_ownedTargets.constLast();
}

QString TerminalPane::activeWorkingDirectory() const
{
    return m_lastKnownCwd.value(activeTarget().stableKey());
}

void TerminalPane::connectNestedHost(const ConnectionTarget &target,
                                     const bool outerShimSuspended)
{
    if (!canPushNestedLayer() || !target.isValid()) {
        return;
    }
    // 推测层是对"终端当前在哪台机器"的猜测；一旦由本程序亲自建层，这些猜测的依据就没了。
    m_inferredTargets.clear();
    m_sessionStack->requestPushLayer(target, outerShimSuspended);
    emit statusMessage(tr("Establishing a nested connection to %1 through the current server…")
                           .arg(target.connectionSpecifier()));
}

bool TerminalPane::canPushNestedLayer() const
{
    return m_sessionStack != nullptr && m_sessionStack->isRunning() && m_shellReady
        && !m_ownedTargets.isEmpty();
}

void TerminalPane::refreshHeaderLabel()
{
    const ConnectionTarget target = activeTarget();
    if (!target.isValid()) {
        // 没有任何自持层或推测层时不覆盖连接阶段文字，例如“正在连接”或失败原因。
        return;
    }
    // 推测/确认仍作为文件操作与路由的内部安全状态保留，但不再在终端标题重复展示；用户
    // 已能从服务器层级和实际 Shell 提示符判断所在位置，额外警示只会挤占横向空间。
    m_headerLabel->setText(target.connectionSpecifier());
    refreshCommandCompletions();
}

bool TerminalPane::isConnected() const
{
    return m_shellReady;
}

void TerminalPane::focusTerminal()
{
    m_terminalView->setFocus(Qt::OtherFocusReason);
}

SshSessionStack *TerminalPane::forwardingSessionStack() const
{
    // 断线处理先把 m_shellReady 清为 false，再通知上层。这里若按“可交互”过滤，管理器在
    // rootConnectionDisconnected 中只能拿到 nullptr，无法识别并删除绑定旧栈的 Runtime。
    // QObject 在 stopWorker 完成前仍有效，按身份返回它正是清理生命周期所需的语义。
    return m_sessionStack;
}

void TerminalPane::setCommandHistoryDatabase(ApplicationDatabase *database)
{
    m_commandHistoryDatabase = database;
    refreshCommandCompletions();
}

void TerminalPane::applyTerminalSettings(const TerminalSettings &settings)
{
    const TerminalSettings normalized = settings.normalized();
    m_automaticReconnectEnabled = normalized.automaticReconnect;
    m_reconnectMaxAttempts = normalized.reconnectMaxAttempts;
    m_reconnectBaseDelaySeconds = normalized.reconnectBaseDelaySeconds;
    if ((!m_automaticReconnectEnabled || m_reconnectMaxAttempts == 0)
        && m_reconnectCycleActive) {
        // 设置对话框可实时关闭自动重连。取消令牌后，已经安排但尚未触发的任务不会再连接；
        // 当前终端保持断开状态，用户可明确选择本地 Shell 或其他主机。
        m_reconnectCycleActive = false;
        m_reconnectAttempts = 0;
        ++m_reconnectToken;
    }
    m_terminalView->applySettings(normalized);
    const TerminalPalette palette = normalized.effectivePalette();

    // 标题栏沿用主题配色但略微向窗口色混合，以与终端正文形成层次；动态属性写法比固定
    // 深色样式更容易在设置预览时即时更新，也不会覆盖标题文本本身。
    const bool dark = normalized.effectiveThemeMode() == ThemeMode::Dark;
    const QColor headerBackground = dark
        ? palette.background.lighter(145)
        : palette.background.darker(108);
    // 样式施加在整个容器而不是单个服务器标签上，保证尺寸文字和文件按钮属于同一条视觉
    // 状态栏。按钮悬停仍保留边框反馈，避免纯图标控件看起来不可点击。
    //
    m_headerBar->setStyleSheet(
        QStringLiteral(
            "QWidget#terminalSessionStatusBar { background: %1; color: %2; }"
            "QLabel#terminalSessionServer { color: %2; font-weight: 600; }"
            "QLabel#terminalSessionTransientStatus { color: %2; }"
            "QLabel#terminalSessionMeta { color: %2; }"
            "QToolButton#terminalFileManagerToggle { border: 1px solid transparent; "
            "border-radius: 5px; padding: 3px; }"
            "QToolButton#terminalFileManagerToggle:hover { border-color: %2; }"
            "QToolButton#terminalFileManagerToggle:checked { background: %3; }")
            .arg(headerBackground.name(),
                 palette.foreground.name(),
                 palette.selection.name()));

    // 自绘图标的线条色不受样式表 color 影响，必须随主题显式重绘，否则深色配色下会残留
    // 上一套主题的浅色线条。
    m_headerIconColor = palette.foreground;
    updateFileManagerButtonIcon();
}

void TerminalPane::setFileManagerVisible(const bool visible)
{
    // 工作区调用该方法同步外部状态时必须阻断 toggled，否则会再次请求工作区修改同一个
    // 状态。虽然 QWidget::setVisible 本身幂等，阻断往返信号仍能避免未来接入动画后重复执行。
    const QSignalBlocker blocker(m_fileManagerButton);
    m_fileManagerButton->setChecked(visible);
    m_fileManagerButton->setToolTip(
        visible
            ? tr("Hide the remote file manager for the current session (Ctrl+Shift+F)")
            : tr("Show the remote file manager for the current session (Ctrl+Shift+F)"));
    // 阻断 toggled 的同时也阻断了图标刷新，这里必须显式补上。
    updateFileManagerButtonIcon();
}

void TerminalPane::setFileManagerEnabled(const bool enabled)
{
    m_fileManagerButton->setEnabled(enabled);
    // 布局不允许时直接隐藏按钮，而不是留一个灰按钮：分屏模式下它永远点不动，只会占位。
    m_fileManagerButton->setVisible(enabled);
    if (!enabled) {
        return;
    }
    // 恢复可用时根据真实勾选态重建提示。
    m_fileManagerButton->setToolTip(
        m_fileManagerButton->isChecked()
            ? tr("Hide the remote file manager for the current session (Ctrl+Shift+F)")
            : tr("Show the remote file manager for the current session (Ctrl+Shift+F)"));
    m_fileManagerButton->setAccessibleName(tr("Remote file manager"));
}

QWidget *TerminalPane::headerExtrasContainer() const
{
    return m_headerExtras;
}

QColor TerminalPane::headerIconColor() const
{
    return m_headerIconColor;
}

void TerminalPane::clearHeaderExtras()
{
    const QList<QWidget *> children =
        m_headerExtras->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget *child : children) {
        child->deleteLater();
    }
}

void TerminalPane::showTransientStatus(const QString &message)
{
    m_statusMessageIcon->clear();
    m_statusMessageIcon->setVisible(false);
    m_statusMessageLabel->setText(message.isEmpty() ? message : QStringLiteral("· %1").arg(message));
    m_statusMessageLabel->setToolTip(message);
    m_statusMessageTimer->stop();
    if (!message.isEmpty()) {
        // 8 秒与窗口标题栏原有的全局提示区一致，不给用户带来新的等待时长预期。
        m_statusMessageTimer->start(8000);
    }
}

void TerminalPane::showTransientTransferResult(const bool success, const QString &message)
{
    if (message.isEmpty()) {
        showTransientStatus(message);
        return;
    }
    // Check/Warning 用固定的语义色而不是当前主题的强调色：传输结果的对错必须一眼分辨，
    // 不能被某个终端主题恰好把强调色调成和背景太接近的颜色而看不出来。
    const QColor color = success ? QColor(QStringLiteral("#3fb950"))
                                 : QColor(QStringLiteral("#f85149"));
    m_statusMessageIcon->setPixmap(UiIcons::renderSymbolPixmap(
        success ? UiIcons::Symbol::Check : UiIcons::Symbol::Warning,
        color,
        QSize(14, 14),
        devicePixelRatioF()));
    m_statusMessageIcon->setVisible(true);
    m_statusMessageLabel->setText(QStringLiteral("· %1").arg(message));
    m_statusMessageLabel->setToolTip(message);
    m_statusMessageTimer->stop();
    m_statusMessageTimer->start(8000);
}

void TerminalPane::updateFileManagerButtonIcon()
{
    m_fileManagerButton->setIcon(
        UiIcons::symbol(this,
                        m_fileManagerButton->isChecked() ? UiIcons::Symbol::Folder
                                                          : UiIcons::Symbol::FolderHidden,
                        m_headerIconColor,
                        QSize(18, 18)));
}

void TerminalPane::beginManualRecovery(const ConnectionTarget &target,
                                      const QString &reason)
{
    // 进入提示即视为本轮自动恢复结束：取消令牌可让仍排在事件队列里的旧退避任务直接退出，
    // 否则它们会在用户还没做选择时突然发起一次连接。
    m_reconnectCycleActive = false;
    m_reconnectAttempts = 0;
    ++m_reconnectToken;
    m_shellReady = false;
    m_remoteSessionEstablished = false;
    m_awaitingManualRecovery = true;
    m_manualRecoveryTarget = target;

    const QString hint = tr("Press r to reconnect to %1, press x to close this tab")
                             .arg(target.connectionSpecifier());
    m_headerLabel->setText(hint);
    // 终端里必须使用 CRLF：裸 LF 只下移一行而不回到第 0 列，提示会从上一行末尾的列开始。
    // 调用方若已把断开原因写进终端会传空字符串，此时不再重复一行，只补按键说明。
    const QString banner = reason.isEmpty()
        ? (QStringLiteral("\r\n") + hint + QStringLiteral("\r\n"))
        : (QStringLiteral("\r\n") + reason + QStringLiteral("\r\n") + hint
           + QStringLiteral("\r\n"));
    m_terminalView->appendTerminalOutput(banner.toUtf8());
    emit statusMessage(hint);
    emit targetsChanged(this);
    focusTerminal();
}

bool TerminalPane::isAwaitingManualRecovery() const
{
    return m_awaitingManualRecovery;
}

void TerminalPane::handleManualRecoveryInput(const QByteArray &bytes)
{
    for (const char byte : bytes) {
        if (byte == 'r' || byte == 'R') {
            const ConnectionTarget target = m_manualRecoveryTarget;
            const QList<ConnectionTarget> plan = m_connectionPlan.isEmpty()
                ? QList<ConnectionTarget>{target}
                : m_connectionPlan;
            if (plan.isEmpty() || !plan.constLast().isValid()) {
                // 没有可复用的目标时继续停留在提示上：静默什么都不做会让用户以为按键失灵。
                m_terminalView->appendTerminalOutput(
                    tr("\r\nNo server to reconnect to; press x to close this tab\r\n").toUtf8());
                return;
            }
            // connectToHosts 会清除提示状态；重连必须走整条受管计划，否则断线后会绕过跳板
            // 直接从本机连同名私网地址。
            m_terminalView->appendTerminalOutput(
                tr("\r\nReconnecting manually…\r\n").toUtf8());
            connectToHosts(plan);
            return;
        }
        if (byte == 'x' || byte == 'X') {
            m_awaitingManualRecovery = false;
            emit closeRequested(this);
            return;
        }
    }
}

bool TerminalPane::scheduleAutomaticReconnect(const ConnectionTarget &target,
                                              const quint64 sessionGeneration)
{
    if (!m_reconnectCycleActive || !m_automaticReconnectEnabled
        || m_reconnectMaxAttempts <= 0) {
        return false;
    }
    if (m_reconnectAttempts >= m_reconnectMaxAttempts) {
        m_reconnectCycleActive = false;
        const QString exhausted = tr("Automatic reconnect reached its limit of %1 attempts")
                                      .arg(m_reconnectMaxAttempts);
        QTimer::singleShot(0, this, [this, target, exhausted, sessionGeneration]() {
            if (sessionGeneration == m_sessionGeneration && !m_remoteSessionEstablished) {
                beginManualRecovery(target, exhausted);
            }
        });
        return false;
    }

    ++m_reconnectAttempts;
    const int attempt = m_reconnectAttempts;
    // 最大 10 次时位移仍在 int 安全范围；最终再封顶 60 秒，网络长期故障时不会高频刷服务器。
    const int exponentialDelay = m_reconnectBaseDelaySeconds * (1 << (attempt - 1));
    const int delaySeconds = std::min(exponentialDelay, 60);
    const quint64 reconnectToken = m_reconnectToken;
    const QString waiting = tr("Reconnecting to %2 automatically in %1 s (attempt %3/%4)")
                                .arg(delaySeconds)
                                .arg(target.connectionSpecifier())
                                .arg(attempt)
                                .arg(m_reconnectMaxAttempts);
    m_headerLabel->setText(waiting);
    emit statusMessage(waiting);

    QTimer::singleShot(delaySeconds * 1000,
                       this,
                       [this, target, sessionGeneration, reconnectToken, attempt]() {
        if (!m_reconnectCycleActive || reconnectToken != m_reconnectToken
            || sessionGeneration != m_sessionGeneration || m_remoteSessionEstablished) {
            return;
        }
        emit automaticReconnectStarted(target, attempt, m_reconnectMaxAttempts);
        // 该标志只覆盖同步的 connectToHost 设置阶段；真正网络连接随后异步运行。这样内部调用
        // 不会清空恢复次数，而用户从菜单发起的新连接仍会取消本周期。
        m_automaticReconnectInvocation = true;
        connectToHosts(m_connectionPlan.isEmpty()
                           ? QList<ConnectionTarget>{target}
                           : m_connectionPlan);
        m_automaticReconnectInvocation = false;
    });
    return true;
}

bool TerminalPane::findTerminalText(const QString &text, const bool backward)
{
    return m_terminalView->findTerminalText(text, backward);
}

QString TerminalPane::terminalPlainText() const
{
    return m_terminalView->terminalPlainText();
}

void TerminalPane::sendInputBytes(const QByteArray &bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    if (m_sessionStack != nullptr && m_sessionStack->isRunning()) {
        m_sessionStack->enqueueInput(bytes);
    } else if (m_localWorker != nullptr && m_localWorker->isRunning()) {
        m_localWorker->enqueueInput(bytes);
    }
}

void TerminalPane::executeCommand(const QString &command)
{
    if (!m_shellReady || command.trimmed().isEmpty()) {
        emit statusMessage(tr("The current terminal is not ready; cannot run the command"));
        return;
    }
    m_terminalView->submitCommand(command);
}

void TerminalPane::insertCommandText(const QString &command)
{
    if (!m_shellReady || command.isEmpty()) {
        emit statusMessage(tr("The current terminal is not ready; cannot insert the command"));
        return;
    }
    m_terminalView->insertCommandText(command);
    focusTerminal();
}

void TerminalPane::recordSubmittedCommand(const QString &command)
{
    if (m_commandHistoryDatabase == nullptr || command.trimmed().isEmpty()
        || command == QStringLiteral("Ctrl-D") || command.front().isSpace()) {
        // 以空白开头时尊重常见 Shell 的 HISTCONTROL=ignorespace 约定。用户通常用这一路径
        // 执行带令牌的临时命令，本应用不应把 Shell 刻意忽略的秘密重新写入自己的数据库。
        return;
    }

    CommandHistoryEntry entry;
    const ConnectionTarget target = activeTarget();
    entry.hostId = target.isValid() ? target.id : QString();
    entry.command = command.trimmed();
    QString error;
    if (!m_commandHistoryDatabase->appendCommandHistory(entry, &error).has_value()) {
        emit statusMessage(tr("Failed to write command history: %1").arg(error));
        return;
    }
    emit commandHistoryChanged(entry.hostId);
    refreshCommandCompletions();
}

void TerminalPane::refreshCommandCompletions()
{
    if (m_commandHistoryDatabase == nullptr) {
        m_terminalView->setCommandCompletions({});
        return;
    }
    const ConnectionTarget target = activeTarget();
    const QString hostId = target.isValid() ? target.id : QString();
    bool ok = false;
    QString error;
    const QList<CommandHistoryEntry> history = m_commandHistoryDatabase->recentCommands(
        hostId, 500, &ok, &error);
    if (!ok) {
        m_terminalView->setCommandCompletions({});
        emit statusMessage(tr("Failed to read command history: %1").arg(error));
        return;
    }

    QStringList candidates;
    QSet<QString> seen;
    for (const CommandHistoryEntry &entry : history) {
        if (entry.command.isEmpty() || seen.contains(entry.command)) {
            continue;
        }
        seen.insert(entry.command);
        candidates.append(entry.command);
    }
    m_terminalView->setCommandCompletions(candidates);
}

void TerminalPane::inspectSubmittedCommand(const QString &command)
{
    const bool isNestedCredential = m_captureNextLineAsCredential
        && !m_pendingNestedCredentialTargetId.isEmpty();
    if (!isNestedCredential) {
        // 必须在解析 ssh/exit 改变连接链之前记录：命令属于用户按下回车时所在的服务器，
        // 而不是命令执行后可能进入的新服务器。
        recordSubmittedCommand(command);
    }

    if (m_ownedTargets.isEmpty()) {
        return;
    }

    if (m_captureNextLineAsCredential && !m_pendingNestedCredentialTargetId.isEmpty()) {
        for (ConnectionTarget &target : m_inferredTargets) {
            if (target.id == m_pendingNestedCredentialTargetId) {
                // 密码原本就必须从本机键盘发送到内层 ssh；这里只在进程内复用同一份输入，
                // 不写入终端文档、数据库或日志。远端 PTY 已关闭 ECHO，因此界面也不会显示。
                // 赋值前主动覆盖上一份错误密码；QString 的隐式共享决定了这里无法提供操作
                // 系统级绝不留痕保证，但可以缩短本对象直接持有的旧凭据驻留时间。
                target.password.fill(QChar::Null);
                target.password = command;
                target.interactiveAuthenticationPending = true;
                break;
            }
        }
        m_captureNextLineAsCredential = false;

        // 不清除 pendingTargetId：服务器可能返回 Permission denied 后再次显示 password。
        // TerminalView 已保证密码回车先进入 PTY 队列；短暂延迟后即可把目标交给文件管理器。
        // interactiveAuthenticationPending 仍保持 true，因此错误密码导致的 SFTP 认证失败
        // 会静默等待终端重试，而不会另外弹出第二个密码框。
        const QString targetId = m_pendingNestedCredentialTargetId;
        QTimer::singleShot(250, this, [this, targetId]() {
            if (m_pendingNestedCredentialTargetId == targetId
                && !m_captureNextLineAsCredential) {
                emit targetsChanged(this);
                emit statusMessage(tr("Inner SSH credentials synced; the file manager is connecting to the current server"));
            }
        });
        emit statusMessage(tr("Inner SSH password received; waiting for the server to confirm"));
        return;
    }

    bool finalizedPendingAuthentication = false;
    if (!m_pendingNestedCredentialTargetId.isEmpty()) {
        // 密码提交后出现下一条普通命令，说明交互式 ssh 已经进入远端 Shell；此时结束认证
        // 重试窗口，避免后续 sudo 等无关密码提示覆盖当前服务器的 SFTP 凭据。
        for (ConnectionTarget &target : m_inferredTargets) {
            if (target.id == m_pendingNestedCredentialTargetId) {
                target.interactiveAuthenticationPending = false;
                finalizedPendingAuthentication = true;
                break;
            }
        }
        m_pendingNestedCredentialTargetId.clear();
    }

    const SshCommandParser::Result parsed =
        SshCommandParser::parse(command, activeTarget());
    if (parsed.action == SshCommandParser::Action::EnterServer && parsed.target.isValid()) {
        // 这里记录“候选已进入”状态。若命令连接失败，用户可通过 exit 或目标选择器回到外层；
        // 后续 Shell Integration 会用远端 preexec/precmd 事件进一步确认实际状态。
        // 路由注册前保存真实外层：推测目标加入列表后 activeTarget() 会变成第 2 层，而远端
        // OpenSSH SFTP 兜底需要重新认证并执行命令的是当前第 1 层，不能把两端写反。
        const ConnectionTarget relayOrigin = activeTarget();
        ConnectionTarget nestedTarget = parsed.target;
        if (m_sessionStack != nullptr && m_sessionStack->isRunning()) {
            nestedTarget.tunnelRouteId = nestedTarget.id;
            // 推测层不在栈内，没有对应的层 id，沿用"从栈顶发起"的旧语义。
            SshTunnelBroker::instance().registerRoute(nestedTarget.tunnelRouteId,
                                                       m_sessionStack,
                                                       QString(),
                                                       nestedTarget,
                                                       relayOrigin);
            m_tunnelRouteIds.append(nestedTarget.tunnelRouteId);
        }
        nestedTarget.interactiveAuthenticationPending = true;
        m_inferredTargets.append(nestedTarget);
        refreshCommandCompletions();
        m_pendingNestedCredentialTargetId = nestedTarget.id;
        m_headerLabel->setText(tr("%1 (nested target)").arg(nestedTarget.connectionSpecifier()));
        emit statusMessage(tr("Detected nested SSH target: %1").arg(nestedTarget.connectionSpecifier()));

        // 公钥/Agent 登录不会出现 password 提示。这里为高延迟网络保留五秒认证观察窗口：
        // 窗口内一旦收到密码提示，就必须先复用用户在终端提交的那一份密码；窗口结束仍无
        // 提示时才按免密登录开放文件管理。这样可避免慢服务器被过早误判，继而弹出第二个
        // SFTP 密码框。五秒仅影响免密嵌套目标首次出现在下拉框的时间，不阻塞终端交互。
        QTimer::singleShot(5000, this, [this, targetId = nestedTarget.id]() {
            if (m_pendingNestedCredentialTargetId == targetId
                && !m_captureNextLineAsCredential) {
                bool credentialWasSubmitted = false;
                for (ConnectionTarget &target : m_inferredTargets) {
                    if (target.id == targetId) {
                        credentialWasSubmitted = !target.password.isEmpty();
                        if (!credentialWasSubmitted) {
                            // 五秒内既没有密码提示也没有密码输入，按公钥或 Agent 免密登录处理。
                            target.interactiveAuthenticationPending = false;
                        }
                        break;
                    }
                }
                if (credentialWasSubmitted) {
                    // 已提交密码时必须继续保留重试状态，不能让初始免密计时器清除它。
                    return;
                }
                m_pendingNestedCredentialTargetId.clear();
                emit targetsChanged(this);
            }
        });
    } else if (parsed.action == SshCommandParser::Action::LeaveServer
               && !m_inferredTargets.isEmpty()) {
        // 只弹推测层。自持层的退出由 SshSessionStack 依据 Shell 通道 EOF 确定性地处理，
        // 绝不能让这里的命令解析去猜——猜错会让文件管理器指向错误的服务器。
        const QString routeId = m_inferredTargets.constLast().tunnelRouteId;
        if (!routeId.isEmpty()) {
            SshTunnelBroker::instance().unregisterRoute(routeId);
            m_tunnelRouteIds.removeAll(routeId);
        }
        m_inferredTargets.removeLast();
        refreshHeaderLabel();
        emit targetsChanged(this);
        emit statusMessage(tr("Returned to server: %1")
                               .arg(activeTarget().connectionSpecifier()));
    } else if (finalizedPendingAuthentication) {
        // 普通命令确认内层 Shell 已可用后，把“认证完成”状态同步给文件管理器。
        emit targetsChanged(this);
    }
}

void TerminalPane::handleShellIntegrationEvent(const QByteArray &payload)
{
    const ShellShim::Event event = ShellShim::parseEvent(payload);
    if (event.kind == ShellShim::Event::Kind::Cwd) {
        const QString stableKey = activeTarget().stableKey();
        m_lastKnownCwd.insert(stableKey, event.workingDirectory);
        emit remoteWorkingDirectoryChanged(stableKey, event.workingDirectory);
        return;
    }
    if (event.kind != ShellShim::Event::Kind::Connect) {
        return;
    }
    if (m_sessionStack == nullptr || !m_sessionStack->isRunning()) {
        return;
    }

    // shim 正阻塞在 read 上等回执，任何分支都必须给出答复，否则用户的终端会挂住。
    const auto respond = [this](const QByteArray &response) {
        // 回执由远端的 read 消费，但 PTY 会先把它原样回显出来。登记后由过滤器扣掉，
        // 用户不该在屏幕上看到 take/pass 这种内部协议文本。换行也必须一并登记：PTY 把
        // 发出去的 \n 回显成 \r\n，漏掉它就会在用户敲的 ssh 命令下方留下一个空行。
        m_echoFilter.expect(response.trimmed() + QByteArrayLiteral("\r\n"));
        m_sessionStack->enqueueInput(response);
    };

    const QString outerKey = activeTarget().stableKey();
    if (m_forwardingProhibitedHosts.contains(outerKey)) {
        // 这台服务器此前已经明确拒绝过转发（AllowTcpForwarding no）。不必再让用户经历一次
        // 失败：直接交还给原生 ssh，连接照常建立，只是这一层退回推测识别。
        respond(ShellShim::declineResponse());
        return;
    }

    QString declineReason;
    const ConnectionTarget target =
        ShellShim::resolveTarget(event.arguments, activeTarget(), &declineReason);
    if (!target.isValid() || !canPushNestedLayer()) {
        // 无法忠实复现的用法交还给真正的 ssh。此时会落回命令解析的降级路径，推测层仍会
        // 被标注为不确定——这比用一个错误的目标去建立会话安全得多。
        respond(ShellShim::declineResponse());
        if (!declineReason.isEmpty()) {
            emit statusMessage(tr("This ssh was not taken over (%1); it was left to the remote to run itself").arg(declineReason));
        }
        return;
    }

    // 记住这次接管对应的原始命令：一旦接管失败，必须把用户的命令原样交还给远端执行，
    // 否则他敲下的 ssh 就凭空消失了——重试多少次都一样，因为每次都会走同一条失败路径。
    m_takeoverTargetId = target.id;
    m_takeoverArguments = event.arguments;
    m_takeoverOuterKey = outerKey;

    respond(ShellShim::acceptResponse());
    // take 之后 shim 会一直挂在第二次 read 上；层结束时由会话栈负责放行。
    connectNestedHost(target, true);
}

void TerminalPane::registerOwnedLayerRoutes()
{
    for (const QString &routeId : std::as_const(m_ownedRouteIds)) {
        SshTunnelBroker::instance().unregisterRoute(routeId);
    }
    m_ownedRouteIds.clear();
    if (m_sessionStack == nullptr) {
        return;
    }

    // 栈底由本机直连，不需要隧道；第 2 层起必须经外层转发抵达。不注册的话 SFTP 会拿着一个
    // 内网地址从本机直连——而本机很可能就在同一个私有网段上，于是连到局域网里另一台碰巧
    // 同 IP 的机器，在错误的服务器上列目录甚至删文件。
    for (qsizetype index = 1; index < m_ownedTargets.size(); ++index) {
        ConnectionTarget &target = m_ownedTargets[index];
        target.tunnelRouteId = target.id;
        SshTunnelBroker::instance().registerRoute(target.tunnelRouteId,
                                                   m_sessionStack,
                                                   target.id,
                                                   target,
                                                   m_ownedTargets.at(index - 1));
        m_ownedRouteIds.append(target.tunnelRouteId);
    }
}

void TerminalPane::runTakeoverFallback(const QString &reason)
{
    if (m_takeoverArguments.isEmpty() || m_sessionStack == nullptr
        || !m_sessionStack->isRunning()) {
        return;
    }

    // 逐个参数用单引号包裹：主机名、路径或密码里的空格与元字符都不能被远端 Shell 再解释。
    QByteArray command = "command ssh";
    for (const QString &argument : std::as_const(m_takeoverArguments)) {
        QString quoted = argument;
        quoted.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
        command += " '" + quoted.toUtf8() + "'";
    }
    m_takeoverArguments.clear();
    m_takeoverTargetId.clear();

    m_terminalView->appendTerminalOutput(
        tr("Shellium could not take over this connection (%1); handing it to the remote ssh to run directly.\r\n")
            .arg(reason)
            .toUtf8());
    // 登记回显：这条命令是本程序补发的，用户已经敲过一次，不该在屏幕上看到第二遍。
    m_echoFilter.expect(command + QByteArrayLiteral("\r\n"));
    m_sessionStack->enqueueInput(command + "\n");
}

void TerminalPane::rollBackPendingNestedTarget(const QString &reason)
{
    if (m_pendingNestedCredentialTargetId.isEmpty()) {
        return;
    }

    qsizetype failedTargetIndex = -1;
    for (qsizetype index = 0; index < m_inferredTargets.size(); ++index) {
        if (m_inferredTargets.at(index).id == m_pendingNestedCredentialTargetId) {
            failedTargetIndex = index;
            break;
        }
    }
    m_pendingNestedCredentialTargetId.clear();
    m_captureNextLineAsCredential = false;
    if (failedTargetIndex < 0) {
        return;
    }

    // 必须删除从失败层开始的全部推测层，而不只是最后一层：用户可能连续尝试了几次，
    // 只弹一层会把更外面的幽灵层留在链上，之后再执行相同的 ssh 又会被解析成"幽灵层的
    // 下一层"，链就再也回不到真实状态。
    while (m_inferredTargets.size() > failedTargetIndex) {
        ConnectionTarget &failedTarget = m_inferredTargets.last();
        if (!failedTarget.tunnelRouteId.isEmpty()) {
            SshTunnelBroker::instance().unregisterRoute(failedTarget.tunnelRouteId);
            m_tunnelRouteIds.removeAll(failedTarget.tunnelRouteId);
        }
        failedTarget.password.fill(QChar::Null);
        m_inferredTargets.removeLast();
    }

    refreshHeaderLabel();
    emit targetsChanged(this);
    emit statusMessage(tr("%1, fell back to %2")
                           .arg(reason, activeTarget().connectionSpecifier()));
}

void TerminalPane::stopWorker()
{
    m_shellReady = false;
    m_remoteSessionEstablished = false;
    m_pendingNestedCredentialTargetId.clear();
    m_captureNextLineAsCredential = false;
    for (const QString &routeId : std::as_const(m_tunnelRouteIds)) {
        SshTunnelBroker::instance().unregisterRoute(routeId);
    }
    m_tunnelRouteIds.clear();
    for (const QString &routeId : std::as_const(m_ownedRouteIds)) {
        SshTunnelBroker::instance().unregisterRoute(routeId);
    }
    m_ownedRouteIds.clear();
    if (m_localWorker != nullptr) {
        m_localWorker->requestStop();
        if (!m_localWorker->wait(3000)) {
            // forkpty 工作线程会自行 SIGHUP/SIGKILL 子进程；不使用 QThread::terminate，避免
            // 中断 waitpid 后遗留僵尸进程。
            m_localWorker->setParent(nullptr);
            connect(m_localWorker, &QThread::finished, m_localWorker, &QObject::deleteLater);
        } else {
            delete m_localWorker;
        }
        m_localWorker = nullptr;
    }
    if (m_sessionStack == nullptr) {
        return;
    }
    // PortForwardRuntime 的网络线程可能正在等待 openRootForwardSocket。先置停止标志，
    // 让栈线程尽快中止并唤醒等待者；随后在对象仍有效时同步通知上层关闭所有运行时。
    // 不能只依赖 QObject::destroyed 的事后通知，否则 Runtime 可能继续解引用失效的栈。
    m_sessionStack->requestStop();
    emit forwardingSessionAboutToClose(m_sessionStack);
    if (!m_sessionStack->wait(3000)) {
        // libssh 连接阶段受 15 秒超时保护。这里不调用 QThread::terminate，因为异步终止可能
        // 让 libssh 在持锁状态下泄漏；工作线程会在网络调用返回后自行退出并被 deleteLater。
        m_sessionStack->setParent(nullptr);
        connect(m_sessionStack, &QThread::finished, m_sessionStack, &QObject::deleteLater);
        m_sessionStack = nullptr;
        return;
    }
    delete m_sessionStack;
    m_sessionStack = nullptr;
}
