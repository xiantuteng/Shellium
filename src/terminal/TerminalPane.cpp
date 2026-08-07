#include "terminal/TerminalPane.h"

#include "ssh/ShellShim.h"
#include "ssh/SshCommandParser.h"
#include "ssh/SshSessionStack.h"
#include "ssh/SshTunnelBroker.h"
#include "terminal/LocalShellWorker.h"
#include "terminal/TerminalView.h"

#include <QHBoxLayout>
#include <QLoggingCategory>
#include <QLabel>
#include <QMessageBox>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

namespace {
/** @brief 与 TerminalView 共用的认证诊断通道；见该文件中的说明。 */
Q_LOGGING_CATEGORY(authLog, "shellium.auth", QtWarningMsg)
} // namespace

TerminalPane::TerminalPane(QWidget *parent)
    : QWidget(parent)
    , m_headerLabel(new QLabel(tr("未连接"), this))
    , m_terminalView(new TerminalView(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    m_headerLabel->setContentsMargins(8, 4, 8, 4);
    m_headerLabel->setStyleSheet(QStringLiteral(
        "QLabel { background: #252932; color: #d8dee9; font-weight: 600; }"));
    layout->addWidget(m_headerLabel);
    layout->addWidget(m_terminalView, 1);

    connect(m_terminalView, &TerminalView::inputBytes, this, [this](const QByteArray &bytes) {
        if (m_sessionStack != nullptr && m_sessionStack->isRunning()) {
            m_sessionStack->enqueueInput(bytes);
        } else if (m_localWorker != nullptr && m_localWorker->isRunning()) {
            m_localWorker->enqueueInput(bytes);
        }
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
                    emit statusMessage(tr("等待内层 SSH 密码；本次输入将同时用于 SFTP 会话"));
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
            [this]() { rollBackPendingNestedTarget(tr("内层 SSH 连接失败")); });
    connect(m_terminalView,
            &TerminalView::nestedSshAborted,
            this,
            [this]() {
                // 只有确实存在未确认的嵌套目标时，Ctrl+C 才被理解为"取消这次 ssh"；
                // 否则它就是普通的命令中断，不该影响连接链。
                if (!m_pendingNestedCredentialTargetId.isEmpty()) {
                    rollBackPendingNestedTarget(tr("已取消内层 SSH 连接"));
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
                emit statusMessage(tr("内层 SSH 认证成功，文件管理已切换到当前服务器"));
            });
}

void TerminalPane::openLocalShell(const QString &shellPath)
{
    // 主动打开本地终端代表开始新会话；递增代次后，旧 SSH 已排队的断开回调无法再触发
    // 自动回退，从而不会重复创建本地 zsh。
    const quint64 sessionGeneration = ++m_sessionGeneration;
    stopWorker();
    m_ownedTargets.clear();
    m_inferredTargets.clear();
    m_echoFilter.reset();
    m_shellReady = false;
    // 切换成本地 Shell 后当前终端不再具备远程目标，立即通知所属标签页清空自己的文件
    // 管理器，不能等 zsh 启动完成后继续短暂显示上一条 SSH 会话的目录。
    emit targetsChanged(this);
    m_headerLabel->setText(tr("正在启动本地 zsh…"));
    m_terminalView->appendTerminalOutput(tr("正在启动本地 zsh…\r\n").toUtf8());

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
        m_headerLabel->setText(tr("本地 zsh"));
        emit targetsChanged(this);
        emit statusMessage(tr("本地 zsh 已启动"));
    });
    connect(m_localWorker,
            &LocalShellWorker::shellError,
            this,
            [this, sessionGeneration](const QString &message) {
        if (sessionGeneration != m_sessionGeneration) {
            return;
        }
        m_shellReady = false;
        m_headerLabel->setText(tr("本地终端错误"));
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
        const QString message = tr("本地 zsh 已结束（退出码 %1）").arg(exitCode);
        m_headerLabel->setText(message);
        emit targetsChanged(this);
        emit statusMessage(message);
    });
    connect(m_terminalView,
            &TerminalView::terminalSizeChanged,
            m_localWorker,
            &LocalShellWorker::updateTerminalSize);
    m_localWorker->start();
    focusTerminal();
}

TerminalPane::~TerminalPane()
{
    stopWorker();
}

void TerminalPane::connectToHost(const ConnectionTarget &target)
{
    if (!target.isValid()) {
        return;
    }
    // 在停止旧工作线程前先推进代次，保证旧线程随后发出的 shellDisconnected 被识别为
    // 过期事件，不能中断这次新连接。
    const quint64 sessionGeneration = ++m_sessionGeneration;
    stopWorker();

    m_shellReady = false;
    m_ownedTargets.clear();
    m_inferredTargets.clear();
    // 旧会话登记的期望串永远不会再出现；继续持有只会让新会话的第一段输出被误扣。
    m_echoFilter.reset();
    // 新 SSH 尚未完成主机校验和认证，此时先让所属标签页清空旧 SFTP 内容；shellConnected
    // 会在认证成功后再次发出 targetsChanged 并开放该目标。
    emit targetsChanged(this);
    m_headerLabel->setText(tr("正在连接 %1…").arg(target.connectionSpecifier()));
    m_terminalView->appendTerminalOutput(
        tr("正在连接 %1…\n").arg(target.connectionSpecifier()).toUtf8());

    m_sessionStack = new SshSessionStack(target, this);
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
        m_headerLabel->setText(target.connectionSpecifier());
        emit statusMessage(tr("已连接 %1").arg(target.connectionSpecifier()));
        // 这里刻意不发 targetsChanged：此刻 m_ownedTargets 仍为空，发出去会让文件管理器
        // 先清空再被紧随其后的 layersChanged 重新填充，用户能看到一次闪烁。栈保证
        // layersChanged 紧跟在 shellConnected 之后到达，由它统一通知即可。
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
        m_inferredTargets.clear();
        m_pendingNestedCredentialTargetId.clear();
        m_captureNextLineAsCredential = false;
        refreshHeaderLabel();
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
        // 命令，登记后由过滤器扣掉，用户什么也看不到。
        m_echoFilter.expect(ShellShim::sourceCommand(path));
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
        // 内层失败不影响外层：这里只报告，不改变任何连接状态。
        m_terminalView->appendTerminalOutput(
            tr("\r\n无法建立到 %1 的嵌套连接：%2\r\n")
                .arg(target.connectionSpecifier(), message)
                .toUtf8());
        emit statusMessage(tr("嵌套连接失败：%1").arg(message));
    });
    connect(m_sessionStack,
            &SshSessionStack::connectionError,
            this,
            [this, sessionGeneration](const QString &message) {
        if (sessionGeneration != m_sessionGeneration) {
            return;
        }
        m_shellReady = false;
        m_remoteSessionEstablished = false;
        m_headerLabel->setText(tr("连接失败"));
        m_terminalView->appendTerminalOutput((QStringLiteral("\n") + message
                                              + QLatin1Char('\n')).toUtf8());
        emit statusMessage(message);
    });
    connect(m_sessionStack,
            &SshSessionStack::legacyHmacRequired,
            this,
            [this, target, sessionGeneration](const QString &error) {
        if (sessionGeneration != m_sessionGeneration) {
            return;
        }
        const QMessageBox::StandardButton answer = QMessageBox::warning(
            this,
            tr("服务器仅支持旧版 MAC 算法"),
            tr("服务器 %1 仅提供 hmac-sha1，默认安全策略无法完成连接。\n\n"
               "hmac-sha1 已过时，建议优先升级服务器 SSH 配置。是否仅为本次连接启用该算法并重试？\n\n"
               "详细信息：%2")
                .arg(target.connectionSpecifier(true), error),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer == QMessageBox::Yes) {
            ConnectionTarget compatibleTarget = target;
            compatibleTarget.allowLegacyHmacSha1 = true;
            // 当前槽由工作线程结束信号排队触发；延迟重连可让旧线程先完整退出并释放 libssh 句柄。
            QTimer::singleShot(0, this, [this, compatibleTarget, sessionGeneration]() {
                if (sessionGeneration == m_sessionGeneration) {
                    connectToHost(compatibleTarget);
                }
            });
        } else {
            const QString message = tr("已取消连接：服务器需要已禁用的 hmac-sha1。请升级服务器配置。");
            m_headerLabel->setText(tr("连接失败"));
            m_terminalView->appendTerminalOutput(
                (QStringLiteral("\n") + message + QLatin1Char('\n')).toUtf8());
            emit statusMessage(message);
        }
    });
    connect(m_sessionStack,
            &SshSessionStack::shellDisconnected,
            this,
            [this, sessionGeneration](const int exitStatus) {
        if (sessionGeneration != m_sessionGeneration || !m_remoteSessionEstablished) {
            // 主动切换会话会先递增 generation 并清除 established；此类旧断开事件不能再
            // 修改标题、文件管理目标或启动额外的本地 Shell。
            return;
        }

        m_remoteSessionEstablished = false;
        m_shellReady = false;
        const QString message = tr("SSH 连接已结束（退出状态 %1），正在退回本地 zsh")
                                    .arg(exitStatus);
        m_headerLabel->setText(message);
        m_terminalView->appendTerminalOutput((QStringLiteral("\r\n") + message
                                              + QStringLiteral("\r\n")).toUtf8());
        emit statusMessage(message);
        emit targetsChanged(this);

        // shellDisconnected 在工作线程 run() 返回前发出。延迟到下一轮事件循环后再停止并
        // 销毁旧 worker，可避免 GUI 线程在信号处理期间等待尚未完成的发射栈。
        QTimer::singleShot(0, this, [this, sessionGeneration]() {
            if (sessionGeneration == m_sessionGeneration
                && !m_remoteSessionEstablished) {
                openLocalShell();
            }
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
                    tr("确认服务器主机密钥"),
                    tr("首次连接 %1。\n\nSHA-256 指纹：\n%2\n\n"
                       "请通过可信渠道核对指纹。确认信任并写入 known_hosts 吗？")
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
    m_sessionStack->start();
    emit statusMessage(tr("正在连接 %1").arg(target.connectionSpecifier()));
    focusTerminal();
}

QList<ConnectionTarget> TerminalPane::targets() const
{
    // 自持层在下、推测层在上：这正是用户看到的外层→内层顺序，也保证文件管理器"跟随最内层"
    // 时选中的是当前终端实际所在的那一层。
    QList<ConnectionTarget> combined = m_ownedTargets.toList();
    combined.append(m_inferredTargets.toList());
    return combined;
}

ConnectionTarget TerminalPane::activeTarget() const
{
    if (!m_inferredTargets.isEmpty()) {
        return m_inferredTargets.constLast();
    }
    return m_ownedTargets.isEmpty() ? ConnectionTarget{} : m_ownedTargets.constLast();
}

void TerminalPane::connectNestedHost(const ConnectionTarget &target)
{
    if (!canPushNestedLayer() || !target.isValid()) {
        return;
    }
    // 推测层是对"终端当前在哪台机器"的猜测；一旦由本程序亲自建层，这些猜测的依据就没了。
    m_inferredTargets.clear();
    m_sessionStack->requestPushLayer(target);
    emit statusMessage(tr("正在经当前服务器建立到 %1 的嵌套连接…")
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
        return;
    }
    const bool inferred = target.confidence == ConnectionTarget::Confidence::Inferred;
    m_headerLabel->setText(inferred
                               ? tr("%1（推测）").arg(target.connectionSpecifier())
                               : target.connectionSpecifier());
}

bool TerminalPane::isConnected() const
{
    return m_shellReady;
}

void TerminalPane::focusTerminal()
{
    m_terminalView->setFocus(Qt::OtherFocusReason);
}

void TerminalPane::inspectSubmittedCommand(const QString &command)
{
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
                emit statusMessage(tr("已同步内层 SSH 凭据，文件管理正在连接当前服务器"));
            }
        });
        emit statusMessage(tr("已接收内层 SSH 密码，正在等待服务器确认"));
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
        ConnectionTarget nestedTarget = parsed.target;
        if (m_sessionStack != nullptr && m_sessionStack->isRunning()) {
            nestedTarget.tunnelRouteId = nestedTarget.id;
            SshTunnelBroker::instance().registerRoute(nestedTarget.tunnelRouteId,
                                                       m_sessionStack,
                                                       nestedTarget.host,
                                                       nestedTarget.port);
            m_tunnelRouteIds.append(nestedTarget.tunnelRouteId);
        }
        nestedTarget.interactiveAuthenticationPending = true;
        m_inferredTargets.append(nestedTarget);
        m_pendingNestedCredentialTargetId = nestedTarget.id;
        m_headerLabel->setText(tr("%1（嵌套目标）").arg(nestedTarget.connectionSpecifier()));
        emit statusMessage(tr("已识别嵌套 SSH 目标：%1").arg(nestedTarget.connectionSpecifier()));

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
        emit statusMessage(tr("已返回服务器：%1")
                               .arg(activeTarget().connectionSpecifier()));
    } else if (finalizedPendingAuthentication) {
        // 普通命令确认内层 Shell 已可用后，把“认证完成”状态同步给文件管理器。
        emit targetsChanged(this);
    }
}

void TerminalPane::handleShellIntegrationEvent(const QByteArray &payload)
{
    const ShellShim::Event event = ShellShim::parseEvent(payload);
    if (event.kind != ShellShim::Event::Kind::Connect) {
        return;
    }
    if (m_sessionStack == nullptr || !m_sessionStack->isRunning()) {
        return;
    }

    // shim 正阻塞在 read 上等回执，任何分支都必须给出答复，否则用户的终端会挂住。
    const auto respond = [this](const QByteArray &response) {
        // 回执由远端的 read 消费，但 PTY 会先把它原样回显出来。登记后由过滤器扣掉，
        // 用户不该在屏幕上看到 take/pass 这种内部协议文本。
        m_echoFilter.expect(response.trimmed());
        m_sessionStack->enqueueInput(response);
    };

    QString declineReason;
    const ConnectionTarget target =
        ShellShim::resolveTarget(event.arguments, activeTarget(), &declineReason);
    if (!target.isValid() || !canPushNestedLayer()) {
        // 无法忠实复现的用法交还给真正的 ssh。此时会落回命令解析的降级路径，推测层仍会
        // 被标注为不确定——这比用一个错误的目标去建立会话安全得多。
        respond(ShellShim::declineResponse());
        if (!declineReason.isEmpty()) {
            emit statusMessage(tr("未接管本次 ssh（%1），已交由远端自行执行").arg(declineReason));
        }
        return;
    }

    respond(ShellShim::acceptResponse());
    connectNestedHost(target);
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
    emit statusMessage(tr("%1，已退回 %2")
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
    m_sessionStack->requestStop();
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
