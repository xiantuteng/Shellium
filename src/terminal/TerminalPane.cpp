#include "terminal/TerminalPane.h"

#include "ssh/SshCommandParser.h"
#include "ssh/SshTerminalWorker.h"
#include "ssh/SshTunnelBroker.h"
#include "terminal/LocalShellWorker.h"
#include "terminal/TerminalView.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

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
        if (m_worker != nullptr && m_worker->isRunning()) {
            m_worker->enqueueInput(bytes);
        } else if (m_localWorker != nullptr && m_localWorker->isRunning()) {
            m_localWorker->enqueueInput(bytes);
        }
    });
    connect(m_terminalView,
            &TerminalView::commandSubmitted,
            this,
            &TerminalPane::inspectSubmittedCommand);
    connect(m_terminalView, &TerminalView::activated, this, [this]() {
        emit activated(this);
    });
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
            &TerminalView::authenticationFailed,
            this,
            [this]() {
                if (m_pendingNestedCredentialTargetId.isEmpty()) {
                    return;
                }

                qsizetype failedTargetIndex = -1;
                for (qsizetype index = 1; index < m_targetChain.size(); ++index) {
                    if (m_targetChain.at(index).id == m_pendingNestedCredentialTargetId) {
                        failedTargetIndex = index;
                        break;
                    }
                }
                if (failedTargetIndex < 0) {
                    m_pendingNestedCredentialTargetId.clear();
                    m_captureNextLineAsCredential = false;
                    return;
                }

                // 最终 Permission denied 表示 OpenSSH 已退出并回到上一层 Shell。必须删除从
                // 失败目标开始的全部候选层级，否则用户再次执行相同 ssh 时会被错误解析为
                // “失败目标的下一层”，导致文件管理器始终无法得到真实第 2 层连接链。
                while (m_targetChain.size() > failedTargetIndex) {
                    ConnectionTarget &failedTarget = m_targetChain.last();
                    if (!failedTarget.tunnelRouteId.isEmpty()) {
                        SshTunnelBroker::instance().unregisterRoute(
                            failedTarget.tunnelRouteId);
                        m_tunnelRouteIds.removeAll(failedTarget.tunnelRouteId);
                    }
                    failedTarget.password.fill(QChar::Null);
                    m_targetChain.removeLast();
                }
                m_pendingNestedCredentialTargetId.clear();
                m_captureNextLineAsCredential = false;
                m_headerLabel->setText(m_targetChain.constLast().connectionSpecifier());
                emit targetsChanged(this);
                emit statusMessage(tr("内层 SSH 认证失败，已退回 %1；可重新执行 ssh 重试")
                                       .arg(m_targetChain.constLast().connectionSpecifier()));
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
                for (ConnectionTarget &target : m_targetChain) {
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
    m_targetChain.clear();
    m_shellReady = false;
    // 切换成本地 Shell 后当前分屏不再具备远程目标，立即通知所属标签页清空自己的文件
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
    m_targetChain = {target};
    // 新 SSH 尚未完成主机校验和认证，此时先让所属标签页清空旧 SFTP 内容；shellConnected
    // 会在认证成功后再次发出 targetsChanged 并开放该目标。
    emit targetsChanged(this);
    m_headerLabel->setText(tr("正在连接 %1…").arg(target.connectionSpecifier()));
    m_terminalView->appendTerminalOutput(
        tr("正在连接 %1…\n").arg(target.connectionSpecifier()).toUtf8());

    m_worker = new SshTerminalWorker(target, this);
    connect(m_worker,
            &SshTerminalWorker::outputReady,
            this,
            [this, sessionGeneration](const QByteArray &bytes) {
                if (sessionGeneration == m_sessionGeneration) {
                    m_terminalView->appendTerminalOutput(bytes);
                }
            });
    connect(m_worker,
            &SshTerminalWorker::shellConnected,
            this,
            [this, target, sessionGeneration]() {
        if (sessionGeneration != m_sessionGeneration) {
            return;
        }
        m_shellReady = true;
        m_remoteSessionEstablished = true;
        m_headerLabel->setText(target.connectionSpecifier());
        emit statusMessage(tr("已连接 %1").arg(target.connectionSpecifier()));
        // 只有终端已完成主机校验和认证后才开放文件管理，避免 SSH 与 SFTP 并发弹出重复提示。
        emit targetsChanged(this);
    });
    connect(m_worker,
            &SshTerminalWorker::connectionError,
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
    connect(m_worker,
            &SshTerminalWorker::legacyHmacRequired,
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
    connect(m_worker,
            &SshTerminalWorker::shellDisconnected,
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
    connect(m_worker,
            &SshTerminalWorker::hostKeyVerificationRequired,
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
                if (m_worker != nullptr) {
                    m_worker->confirmUnknownHost(answer == QMessageBox::Yes);
                }
            });
    connect(m_terminalView,
            &TerminalView::terminalSizeChanged,
            m_worker,
            &SshTerminalWorker::updateTerminalSize);
    m_worker->start();
    emit statusMessage(tr("正在连接 %1").arg(target.connectionSpecifier()));
    focusTerminal();
}

QList<ConnectionTarget> TerminalPane::targets() const
{
    return m_targetChain.toList();
}

ConnectionTarget TerminalPane::activeTarget() const
{
    return m_targetChain.isEmpty() ? ConnectionTarget{} : m_targetChain.constLast();
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
    if (m_targetChain.isEmpty()) {
        return;
    }

    if (m_captureNextLineAsCredential && !m_pendingNestedCredentialTargetId.isEmpty()) {
        for (ConnectionTarget &target : m_targetChain) {
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
        for (ConnectionTarget &target : m_targetChain) {
            if (target.id == m_pendingNestedCredentialTargetId) {
                target.interactiveAuthenticationPending = false;
                finalizedPendingAuthentication = true;
                break;
            }
        }
        m_pendingNestedCredentialTargetId.clear();
    }

    const SshCommandParser::Result parsed =
        SshCommandParser::parse(command, m_targetChain.constLast());
    if (parsed.action == SshCommandParser::Action::EnterServer && parsed.target.isValid()) {
        // 这里记录“候选已进入”状态。若命令连接失败，用户可通过 exit 或目标选择器回到外层；
        // 后续 Shell Integration 会用远端 preexec/precmd 事件进一步确认实际状态。
        ConnectionTarget nestedTarget = parsed.target;
        if (m_worker != nullptr && m_worker->isRunning()) {
            nestedTarget.tunnelRouteId = nestedTarget.id;
            SshTunnelBroker::instance().registerRoute(nestedTarget.tunnelRouteId,
                                                       m_worker,
                                                       nestedTarget.host,
                                                       nestedTarget.port);
            m_tunnelRouteIds.append(nestedTarget.tunnelRouteId);
        }
        nestedTarget.interactiveAuthenticationPending = true;
        m_targetChain.append(nestedTarget);
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
                for (ConnectionTarget &target : m_targetChain) {
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
    } else if (parsed.action == SshCommandParser::Action::LeaveServer && m_targetChain.size() > 1) {
        const QString routeId = m_targetChain.constLast().tunnelRouteId;
        if (!routeId.isEmpty()) {
            SshTunnelBroker::instance().unregisterRoute(routeId);
            m_tunnelRouteIds.removeAll(routeId);
        }
        m_targetChain.removeLast();
        m_headerLabel->setText(m_targetChain.constLast().connectionSpecifier());
        emit targetsChanged(this);
        emit statusMessage(tr("已返回服务器：%1")
                               .arg(m_targetChain.constLast().connectionSpecifier()));
    } else if (finalizedPendingAuthentication) {
        // 普通命令确认内层 Shell 已可用后，把“认证完成”状态同步给文件管理器。
        emit targetsChanged(this);
    }
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
    if (m_worker == nullptr) {
        return;
    }
    m_worker->requestStop();
    if (!m_worker->wait(3000)) {
        // libssh 连接阶段受 15 秒超时保护。这里不调用 QThread::terminate，因为异步终止可能
        // 让 libssh 在持锁状态下泄漏；工作线程会在网络调用返回后自行退出并被 deleteLater。
        m_worker->setParent(nullptr);
        connect(m_worker, &QThread::finished, m_worker, &QObject::deleteLater);
        m_worker = nullptr;
        return;
    }
    delete m_worker;
    m_worker = nullptr;
}
