#include "app/MainWindow.h"

#include "forwarding/PortForwardManagerWidget.h"
#include "productivity/ProductivityModel.h"
#include "productivity/ProductivityWidget.h"
#include "security/Vault.h"
#include "settings/TerminalSettings.h"
#include "storage/ApplicationDatabase.h"
#include "terminal/TerminalPane.h"
#include "terminal/TerminalWorkspace.h"
#include "ui/ConnectionDialog.h"
#include "ui/HostManagerWidget.h"
#include "ui/SettingsDialog.h"
#include "ui/SshKeyManagerDialog.h"
#include "ui/VaultDialog.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QDockWidget>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMenuBar>
#include <QPointer>
#include <QStatusBar>
#include <QSaveFile>
#include <QSet>
#include <QTabWidget>
#include <QToolBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QUuid>

#include <algorithm>
#include <utility>

MainWindow::MainWindow(ApplicationDatabase *database,
                       CredentialVault *credentialVault,
                       QWidget *parent)
    : QMainWindow(parent)
    , m_tabs(new QTabWidget(this))
    , m_database(database)
    , m_credentialVault(credentialVault)
    , m_vaultIdleTimer(new QTimer(this))
{
    Q_ASSERT(m_database != nullptr);
    Q_ASSERT(m_credentialVault != nullptr);

    // 设置服务要在创建终端控件前加载并绑定应用主题，避免窗口先按默认深色绘制一帧再跳到
    // 用户配置；损坏的单项由服务安全回退，完整数据库错误则在状态栏持续展示。
    m_settingsService = new TerminalSettingsService(m_database, this);
    m_settingsService->bindApplicationTheme(qApp);
    connect(m_settingsService,
            &TerminalSettingsService::warningOccurred,
            this,
            [this](const QString &message) { statusBar()->showMessage(message, 10000); });
    QString settingsLoadError;
    if (!m_settingsService->load(&settingsLoadError)) {
        statusBar()->showMessage(tr("读取设置失败：%1").arg(settingsLoadError), 0);
    }
    m_productivityModel = new ProductivityModel(m_database, this);

    setWindowTitle(tr("Shellium"));
    resize(1380, 860);
    m_tabs->setDocumentMode(true);
    m_tabs->setMovable(true);
    m_tabs->setTabsClosable(true);
    setCentralWidget(m_tabs);

    // 主机管理使用可停靠面板：桌面窗口较窄时用户可以临时隐藏或移动，但其数据模型始终是
    // 应用级数据库，不会随标签页切换而重复加载多个副本。
    auto *hostDock = new QDockWidget(tr("主机管理"), this);
    hostDock->setObjectName(QStringLiteral("HostManagerDock"));
    hostDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_hostManager = new HostManagerWidget(m_database, m_credentialVault, hostDock);
    hostDock->setWidget(m_hostManager);
    addDockWidget(Qt::LeftDockWidgetArea, hostDock);
    resizeDocks({hostDock}, {330}, Qt::Horizontal);

    // 效率工具与主机树共享左侧停靠区并以标签切换，避免同时挤压终端和每个标签独占的
    // SFTP 双栏。其模型是应用级对象，但执行路由仍由主窗口按当前打开会话动态决定。
    auto *productivityDock = new QDockWidget(tr("效率工具"), this);
    productivityDock->setObjectName(QStringLiteral("ProductivityDock"));
    productivityDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_productivityWidget = new ProductivityWidget(m_productivityModel, productivityDock);
    productivityDock->setWidget(m_productivityWidget);
    addDockWidget(Qt::LeftDockWidgetArea, productivityDock);
    tabifyDockWidget(hostDock, productivityDock);
    hostDock->raise();

    // 端口转发属于应用级规则，但运行时绑定某个标签的根 SSH 会话；单独侧栏可在不切换
    // 终端布局的情况下管理 -L/-R/-D，并实时查看连接数和流量。
    auto *forwardingDock = new QDockWidget(tr("端口转发"), this);
    forwardingDock->setObjectName(QStringLiteral("PortForwardingDock"));
    forwardingDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_portForwardManager = new PortForwardManagerWidget(m_database, forwardingDock);
    forwardingDock->setWidget(m_portForwardManager);
    addDockWidget(Qt::LeftDockWidgetArea, forwardingDock);
    tabifyDockWidget(hostDock, forwardingDock);
    hostDock->raise();

    m_workspace = createWorkspaceTab(tr("终端 1"));

    auto *connectAction = new QAction(tr("连接服务器"), this);
    connectAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")));
    connectAction->setToolTip(tr("在当前终端面板中连接 SSH 服务器"));

    auto *nestedConnectAction = new QAction(tr("嵌套连接到…"), this);
    nestedConnectAction->setToolTip(
        tr("经当前服务器建立到内网主机的连接。层级由本程序自己维护，"
           "文件管理器可以准确地在各层之间切换。"));

    auto *localShellAction = new QAction(tr("本地 zsh"), this);
    localShellAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+L")));
    localShellAction->setToolTip(tr("在当前终端面板中启动本地登录式 zsh"));

    auto *newTabAction = new QAction(tr("新建标签页"), this);
    newTabAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));

    auto *closeTabAction = new QAction(tr("关闭标签页"), this);
    closeTabAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));

    auto *renameTabAction = new QAction(tr("重命名标签页"), this);

    auto *toggleFilesAction = new QAction(tr("显示远程文件管理"), this);
    toggleFilesAction->setCheckable(true);
    toggleFilesAction->setChecked(true);
    toggleFilesAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    toggleFilesAction->setToolTip(tr("显示或隐藏当前终端标签页内部的远程文件管理区域"));

    auto *searchTerminalAction = new QAction(tr("搜索终端输出…"), this);
    auto *exportTerminalAction = new QAction(tr("导出终端输出…"), this);
    auto *settingsAction = new QAction(tr("设置…"), this);
    auto *commandPaletteAction = new QAction(tr("命令面板…"), this);
    commandPaletteAction->setObjectName(QStringLiteral("CommandPaletteAction"));
    auto *saveWorkspaceAction = new QAction(tr("保存当前工作区…"), this);
    auto *openWorkspaceAction = new QAction(tr("打开工作区…"), this);
    auto *removeWorkspaceAction = new QAction(tr("删除工作区…"), this);

    QToolBar *toolbar = addToolBar(tr("主工具栏"));
    toolbar->setObjectName(QStringLiteral("MainToolBar"));
    toolbar->setMovable(false);
    toolbar->addAction(connectAction);
    toolbar->addAction(nestedConnectAction);
    toolbar->addAction(localShellAction);
    toolbar->addAction(newTabAction);
    toolbar->addSeparator();
    toolbar->addAction(toggleFilesAction);
    toolbar->addSeparator();
    toolbar->addAction(settingsAction);

    QMenu *connectionMenu = menuBar()->addMenu(tr("连接"));
    connectionMenu->addAction(connectAction);
    connectionMenu->addAction(nestedConnectAction);
    connectionMenu->addAction(localShellAction);
    connectionMenu->addAction(newTabAction);
    connectionMenu->addAction(closeTabAction);
    connectionMenu->addAction(renameTabAction);

    QMenu *workspaceMenu = menuBar()->addMenu(tr("工作区"));
    workspaceMenu->addAction(saveWorkspaceAction);
    workspaceMenu->addAction(openWorkspaceAction);
    workspaceMenu->addSeparator();
    workspaceMenu->addAction(removeWorkspaceAction);

    QMenu *viewMenu = menuBar()->addMenu(tr("视图"));
    viewMenu->addAction(toggleFilesAction);
    viewMenu->addAction(hostDock->toggleViewAction());
    viewMenu->addAction(productivityDock->toggleViewAction());
    viewMenu->addAction(forwardingDock->toggleViewAction());

    QMenu *terminalMenu = menuBar()->addMenu(tr("终端"));
    terminalMenu->addAction(searchTerminalAction);
    terminalMenu->addAction(exportTerminalAction);
    terminalMenu->addSeparator();
    terminalMenu->addAction(commandPaletteAction);
    terminalMenu->addAction(settingsAction);

    auto *unlockVaultAction = new QAction(tr("解锁凭据保险库…"), this);
    auto *lockVaultAction = new QAction(tr("锁定凭据保险库"), this);
    auto *sshKeyManagerAction = new QAction(tr("SSH 密钥管理…"), this);
    lockVaultAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+K")));
    QMenu *securityMenu = menuBar()->addMenu(tr("安全"));
    securityMenu->addAction(unlockVaultAction);
    securityMenu->addAction(lockVaultAction);
    securityMenu->addSeparator();
    securityMenu->addAction(sshKeyManagerAction);

    m_configurableActions = {
        {QStringLiteral("connection.new"), connectAction},
        {QStringLiteral("terminal.local"), localShellAction},
        {QStringLiteral("tab.new"), newTabAction},
        {QStringLiteral("tab.close"), closeTabAction},
        {QStringLiteral("files.toggle"), toggleFilesAction},
        {QStringLiteral("vault.lock"), lockVaultAction},
        {QStringLiteral("terminal.search"), searchTerminalAction},
        {QStringLiteral("terminal.export"), exportTerminalAction},
        {QStringLiteral("command.palette"), commandPaletteAction},
        {QStringLiteral("settings.open"), settingsAction},
    };
    applyConfiguredShortcuts();

    statusBar()->showMessage(tr("就绪"));
    m_terminalStatusLabel = new QLabel(tr("未连接  |  UTF-8  |  80×24"), this);
    m_terminalStatusLabel->setContentsMargins(8, 0, 8, 0);
    m_terminalStatusLabel->setToolTip(
        tr("当前标签页的服务器、终端编码和列×行尺寸；推测服务器会被明确标注"));
    statusBar()->addPermanentWidget(m_terminalStatusLabel);

    connect(connectAction, &QAction::triggered, this, &MainWindow::connectCurrentPane);
    connect(nestedConnectAction, &QAction::triggered,
            this, &MainWindow::connectNestedFromCurrentPane);
    connect(localShellAction, &QAction::triggered, this, [this]() {
        TerminalWorkspace *workspace = activeWorkspace();
        TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
        if (pane != nullptr) {
            pane->openLocalShell();
        }
    });
    connect(newTabAction, &QAction::triggered, this, [this]() {
        TerminalWorkspace *workspace = createWorkspaceTab(
            tr("终端 %1").arg(m_tabs->count() + 1));
        m_tabs->setCurrentWidget(workspace);
        workspace->activePane()->openLocalShell();
    });
    connect(closeTabAction, &QAction::triggered, this, [this]() {
        const int index = m_tabs->currentIndex();
        if (index >= 0 && m_tabs->count() > 1) {
            QWidget *page = m_tabs->widget(index);
            m_tabs->removeTab(index);
            page->deleteLater();
        }
    });
    connect(renameTabAction, &QAction::triggered, this, [this]() {
        const int index = m_tabs->currentIndex();
        if (index < 0) {
            return;
        }
        bool accepted = false;
        const QString name = QInputDialog::getText(this,
                                                    tr("重命名标签页"),
                                                    tr("标签名称："),
                                                    QLineEdit::Normal,
                                                    m_tabs->tabText(index),
                                                    &accepted).trimmed();
        if (accepted && !name.isEmpty()) {
            m_tabs->setTabText(index, name);
        }
    });
    connect(saveWorkspaceAction, &QAction::triggered,
            this, &MainWindow::saveCurrentWorkspace);
    connect(openWorkspaceAction, &QAction::triggered,
            this, &MainWindow::openSavedWorkspace);
    connect(removeWorkspaceAction, &QAction::triggered,
            this, &MainWindow::removeSavedWorkspace);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](const int index) {
        if (m_tabs->count() <= 1) {
            return;
        }
        QWidget *page = m_tabs->widget(index);
        m_tabs->removeTab(index);
        page->deleteLater();
    });
    connect(m_tabs,
            &QTabWidget::currentChanged,
            this,
            [this, toggleFilesAction](const int) {
        m_workspace = activeWorkspace();
        // 每个标签页保存自己的显隐状态；切换标签时只同步工具栏勾选，不迁移任何 SFTP
        // 状态或凭据。阻断信号可避免 setChecked 反向修改刚切入的工作区。
        toggleFilesAction->blockSignals(true);
        toggleFilesAction->setChecked(m_workspace != nullptr
                                      && m_workspace->isFileManagerVisible());
        toggleFilesAction->blockSignals(false);
        m_terminalStatusLabel->setText(
            m_terminalStatusByWorkspace.value(m_workspace,
                                              tr("未连接  |  UTF-8  |  80×24")));
        refreshProductivityTargets();
    });
    connect(toggleFilesAction, &QAction::toggled, this, [this](const bool visible) {
        TerminalWorkspace *workspace = activeWorkspace();
        if (workspace != nullptr) {
            workspace->setFileManagerVisible(visible);
        }
    });
    connect(m_hostManager,
            &HostManagerWidget::hostConnectionRequested,
            this,
            &MainWindow::connectSavedHost);
    connect(m_hostManager,
            &HostManagerWidget::hostsChanged,
            m_portForwardManager,
            &PortForwardManagerWidget::refresh);
    connect(m_portForwardManager,
            &PortForwardManagerWidget::statusMessage,
            this,
            [this](const QString &message) { statusBar()->showMessage(message, 8000); });
    connect(lockVaultAction, &QAction::triggered, this, [this]() {
        m_credentialVault->lock();
        statusBar()->showMessage(tr("凭据保险库已锁定"), 5000);
    });
    connect(unlockVaultAction, &QAction::triggered, this, [this]() {
        if (ensureVaultUnlocked()) {
            statusBar()->showMessage(tr("凭据保险库已解锁"), 5000);
        }
    });
    connect(sshKeyManagerAction, &QAction::triggered, this, [this]() {
        SshKeyManagerDialog dialog(this);
        dialog.exec();
    });
    connect(searchTerminalAction, &QAction::triggered,
            this, &MainWindow::searchCurrentTerminal);
    connect(exportTerminalAction, &QAction::triggered,
            this, &MainWindow::exportCurrentTerminal);
    connect(commandPaletteAction, &QAction::triggered,
            this, &MainWindow::openCommandPalette);
    connect(settingsAction, &QAction::triggered, this, [this]() {
        SettingsDialog dialog(m_settingsService, this);
        dialog.exec();
    });
    connect(m_settingsService,
            &TerminalSettingsService::settingsChanged,
            this,
            [this](const TerminalSettings &settings) {
                // 对已打开标签逐个应用，保证设置对话框的实时预览和系统主题切换不会只影响
                // 新建终端；动作快捷键同样以这一份快照为唯一来源。
                for (TerminalPane *pane : allTerminalPanes()) {
                    pane->applyTerminalSettings(settings);
                }
                applyConfiguredShortcuts();
            });

    connect(m_productivityWidget,
            &ProductivityWidget::statusMessage,
            this,
            [this](const QString &message) { statusBar()->showMessage(message, 8000); });
    connect(m_productivityWidget,
            &ProductivityWidget::commandExecutionRequested,
            this,
            &MainWindow::executeOnTargets);
    connect(m_productivityWidget,
            &ProductivityWidget::broadcastInputRequested,
            this,
            [this](const QByteArray &bytes, const QStringList &targetIds) {
                sendBytesToTargets(bytes, targetIds);
            });
    connect(m_productivityWidget,
            &ProductivityWidget::broadcastModeChanged,
            this,
            [this](const bool enabled, const QStringList &targetIds) {
                m_broadcastEnabled = enabled && !targetIds.isEmpty();
                m_broadcastTargetIds = m_broadcastEnabled ? targetIds : QStringList{};
                statusBar()->showMessage(
                    m_broadcastEnabled
                        ? tr("实时广播已开启：%1 个目标").arg(m_broadcastTargetIds.size())
                        : tr("实时广播已关闭"),
                    5000);
            });
    refreshProductivityTargets();

    // 默认使用 CredentialVault 的 15 分钟阈值。每 15 秒检查一次只决定锁定延迟上界，
    // 不持有敏感数据，也不会阻塞终端或 SFTP 工作线程。
    m_vaultIdleTimer->setInterval(15000);
    connect(m_vaultIdleTimer, &QTimer::timeout, this, [this]() {
        if (m_credentialVault->lockIfIdle()) {
            statusBar()->showMessage(tr("凭据保险库因闲置已自动锁定"), 5000);
        }
    });
    m_vaultIdleTimer->start();
    qApp->installEventFilter(this);
}

MainWindow::~MainWindow()
{
    // 显式移除可以保证析构期间后续控件事件不会回调一个已开始释放成员的 MainWindow。
    qApp->removeEventFilter(this);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched)
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
        m_credentialVault->recordActivity();
        break;
    default:
        break;
    }
    return false;
}

void MainWindow::connectCurrentPane()
{
    TerminalWorkspace *workspace = activeWorkspace();
    TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
    if (pane == nullptr) {
        return;
    }

    ConnectionDialog dialog(this);
    if (pane->activeTarget().isValid()) {
        dialog.setInitialTarget(pane->activeTarget());
    }
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    pane->connectToHost(dialog.target());
}

void MainWindow::connectNestedFromCurrentPane()
{
    TerminalWorkspace *workspace = activeWorkspace();
    TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
    if (pane == nullptr) {
        return;
    }
    if (!pane->canPushNestedLayer()) {
        QMessageBox::information(
            this,
            tr("无法建立嵌套连接"),
            tr("当前标签页尚未连接到远程服务器。请先连接一台服务器，再从它继续连接内网主机。"));
        return;
    }

    ConnectionDialog dialog(this);
    // 内层地址由当前服务器的视角解析，因此不预填当前主机名——那几乎肯定不是用户想去的
    // 下一跳，预填反而容易被误当成默认值直接确认。
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    pane->connectNestedHost(dialog.target());
}

void MainWindow::connectSavedHost(const QString &hostId,
                                  const ConnectionTarget &sessionTarget)
{
    TerminalWorkspace *workspace = activeWorkspace();
    TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
    if (pane == nullptr || !sessionTarget.isValid()) {
        return;
    }

    ConnectionTarget target = sessionTarget;
    if (target.password.isEmpty()) {
        QString lookupError;
        const std::optional<HostRecord> savedHost = m_database->resolveHost(hostId, &lookupError);
        if (!savedHost.has_value()) {
            QMessageBox::critical(this,
                                  tr("无法连接保存的主机"),
                                  lookupError.isEmpty() ? tr("主机记录不存在") : lookupError);
            return;
        }
        if (!savedHost->credentialId.isEmpty()) {
            if (!ensureVaultUnlocked()) {
                return;
            }
            QString credentialError;
            std::optional<QByteArray> secret =
                m_credentialVault->loadSecret(savedHost->credentialId, &credentialError);
            if (!secret.has_value()) {
                QMessageBox::critical(this,
                                      tr("无法读取主机凭据"),
                                      credentialError.isEmpty() ? tr("凭据不存在") : credentialError);
                return;
            }
            target.password = QString::fromUtf8(*secret);
            Vault::secureErase(*secret);
        }
    }
    // 持久化主机 id 必须进入终端根层快照。ConnectionDialog 会为临时会话生成 UUID，如果不
    // 覆盖，连接历史、命令历史与凭据引用就无法回到主机树中的同一条记录。
    target.id = hostId;
    target.parentId.clear();
    target.confidence = ConnectionTarget::Confidence::Confirmed;
    m_pendingHistoryHosts.insert(pane, hostId);
    m_pendingHistoryStartedAt.insert(pane, QDateTime::currentDateTimeUtc());
    pane->connectToHost(target);

    const int tabIndex = m_tabs->indexOf(workspace);
    if (tabIndex >= 0) {
        m_tabs->setTabText(tabIndex,
                           target.displayName.isEmpty() ? target.connectionSpecifier()
                                                        : target.displayName);
    }
}

bool MainWindow::ensureVaultUnlocked()
{
    return VaultDialog::ensureUnlocked(m_credentialVault, this, true);
}

void MainWindow::recordConnectionSucceeded(TerminalPane *pane,
                                           const ConnectionTarget &target)
{
    Q_UNUSED(target)
    if (pane == nullptr || !m_pendingHistoryHosts.contains(pane)) {
        return;
    }

    ConnectionHistoryEntry entry;
    entry.hostId = m_pendingHistoryHosts.take(pane);
    entry.connectedAt = m_pendingHistoryStartedAt.take(pane);
    entry.succeeded = true;

    QString error;
    const std::optional<qint64> historyId =
        m_database->appendConnectionHistory(entry, &error);
    if (historyId.has_value()) {
        m_openHistoryRows.insert(pane, *historyId);
        m_hostManager->refresh();
    } else {
        statusBar()->showMessage(tr("连接成功，但写入连接历史失败：%1").arg(error), 0);
    }
}

void MainWindow::recordConnectionFailed(TerminalPane *pane,
                                        const ConnectionTarget &target,
                                        const QString &message)
{
    Q_UNUSED(target)
    if (pane == nullptr || !m_pendingHistoryHosts.contains(pane)) {
        return;
    }

    ConnectionHistoryEntry entry;
    entry.hostId = m_pendingHistoryHosts.take(pane);
    entry.connectedAt = m_pendingHistoryStartedAt.take(pane);
    entry.disconnectedAt = QDateTime::currentDateTimeUtc();
    entry.succeeded = false;
    entry.error = message;

    QString error;
    if (!m_database->appendConnectionHistory(entry, &error).has_value()) {
        statusBar()->showMessage(tr("连接失败，且写入连接历史失败：%1").arg(error), 0);
    }
    m_hostManager->refresh();
}

void MainWindow::recordConnectionDisconnected(TerminalPane *pane,
                                              const ConnectionTarget &target,
                                              const int exitStatus)
{
    Q_UNUSED(target)
    Q_UNUSED(exitStatus)
    if (pane == nullptr || !m_openHistoryRows.contains(pane)) {
        return;
    }

    const qint64 historyId = m_openHistoryRows.take(pane);
    bool updated = false;
    QString error;
    if (!m_database->finishConnectionHistory(historyId,
                                             QDateTime::currentDateTimeUtc(),
                                             &updated,
                                             &error)) {
        statusBar()->showMessage(tr("写入断开时间失败：%1").arg(error), 0);
    }
}

TerminalWorkspace *MainWindow::createWorkspaceTab(const QString &title)
{
    auto *workspace = new TerminalWorkspace(m_tabs);
    const int index = m_tabs->addTab(workspace, title);
    m_tabs->setCurrentIndex(index);

    connect(workspace,
            &TerminalWorkspace::statusMessage,
            this,
            [this, workspace](const QString &message) {
                if (workspace == activeWorkspace()) {
                    statusBar()->showMessage(message, 0);
                }
            });
    connect(workspace,
            &TerminalWorkspace::terminalStatusChanged,
            this,
            [this, workspace](const QString &server,
                              const QString &encoding,
                              const int columns,
                              const int rows) {
                const QString status = tr("%1  |  %2  |  %3×%4")
                                           .arg(server, encoding)
                                           .arg(columns)
                                           .arg(rows);
                m_terminalStatusByWorkspace.insert(workspace, status);
                if (workspace == activeWorkspace() && m_terminalStatusLabel != nullptr) {
                    m_terminalStatusLabel->setText(status);
                }
            });
    TerminalPane *pane = workspace->activePane();
    pane->setCommandHistoryDatabase(m_database);
    pane->applyTerminalSettings(m_settingsService->currentSettings());
    connect(workspace,
            &TerminalWorkspace::activeTargetsChanged,
            this,
            [this](TerminalPane *) { refreshProductivityTargets(); });
    connect(pane,
            &TerminalPane::commandHistoryChanged,
            this,
            [this](const QString &) {
                // TerminalPane 直接使用数据库以保持终端层可独立测试，因此历史写入不会经过
                // ProductivityModel；显式刷新侧栏可让刚执行的命令立即出现。
                m_productivityWidget->refresh();
            });
    connect(pane,
            &TerminalPane::broadcastSourceInput,
            this,
            [this](TerminalPane *source, const QByteArray &bytes) {
                TerminalWorkspace *workspace = activeWorkspace();
                if (!m_broadcastEnabled || workspace == nullptr
                    || workspace->activePane() != source) {
                    return;
                }
                sendBytesToTargets(bytes, m_broadcastTargetIds, source);
            });
    connect(pane,
            &TerminalPane::forwardingSessionAboutToClose,
            this,
            [this, pane](SshSessionStack *sessionStack) {
                const QList<ConnectionTarget> targets = pane->targets();
                const QString rootHostId = targets.isEmpty() ? QString() : targets.constFirst().id;
                m_portForwardManager->unregisterSession(rootHostId, sessionStack);
            });
    connect(pane,
            &TerminalPane::rootConnectionSucceeded,
            this,
            [this, pane](const ConnectionTarget &target) {
                recordConnectionSucceeded(pane, target);
                if (!target.id.isEmpty() && pane->forwardingSessionStack() != nullptr) {
                    // 只有完成认证后的根栈才可承载规则；临时连接 id 不在数据库中，因此
                    // 管理器即使收到也不会找到任何持久化规则。
                    m_portForwardManager->registerSession(target.id,
                                                          pane->forwardingSessionStack());
                }
            });
    connect(pane,
            &TerminalPane::automaticReconnectStarted,
            this,
            [this, pane](const ConnectionTarget &target,
                         const int attempt,
                         const int maximumAttempts) {
                // 只有主机树中的持久化 id 才写历史；临时连接仍可自动恢复，但不能制造无法在
                // 主机管理中归属的孤立历史行。
                QString lookupError;
                const std::optional<HostRecord> saved =
                    m_database->findHost(target.id, &lookupError);
                if (saved.has_value() && saved->kind == HostNodeKind::Host) {
                    m_pendingHistoryHosts.insert(pane, target.id);
                    m_pendingHistoryStartedAt.insert(pane, QDateTime::currentDateTimeUtc());
                }
                statusBar()->showMessage(
                    tr("正在自动重连 %1（第 %2/%3 次）")
                        .arg(target.connectionSpecifier())
                        .arg(attempt)
                        .arg(maximumAttempts),
                    5000);
            });
    connect(pane,
            &TerminalPane::rootConnectionFailed,
            this,
            [this, pane](const ConnectionTarget &target, const QString &message) {
                recordConnectionFailed(pane, target, message);
                m_portForwardManager->unregisterSession(target.id,
                                                        pane->forwardingSessionStack());
            });
    connect(pane,
            &TerminalPane::rootConnectionDisconnected,
            this,
            [this, pane](const ConnectionTarget &target, const int exitStatus) {
                recordConnectionDisconnected(pane, target, exitStatus);
                m_portForwardManager->unregisterSession(target.id,
                                                        pane->forwardingSessionStack());
            });
    connect(pane, &QObject::destroyed, this, [this, pane]() {
        // 标签关闭时先补齐已成功连接记录的断开时间，再清除尚未成功的连接请求，避免哈希表
        // 长期保存悬空 QObject 地址。
        if (m_openHistoryRows.contains(pane)) {
            bool updated = false;
            QString error;
            m_database->finishConnectionHistory(m_openHistoryRows.take(pane),
                                                QDateTime::currentDateTimeUtc(),
                                                &updated,
                                                &error);
        }
        m_pendingHistoryHosts.remove(pane);
        m_pendingHistoryStartedAt.remove(pane);
    });
    connect(workspace, &QObject::destroyed, this, [this, workspace]() {
        m_terminalStatusByWorkspace.remove(workspace);
        // QObject::destroyed 发出时标签页对象尚在析构栈上；排到下一轮事件循环再枚举，避免
        // 把即将释放的页面重新加入执行目标。
        QTimer::singleShot(0, this, [this]() { refreshProductivityTargets(); });
    });
    return workspace;
}

TerminalWorkspace *MainWindow::activeWorkspace() const
{
    return qobject_cast<TerminalWorkspace *>(m_tabs->currentWidget());
}

QList<TerminalPane *> MainWindow::allTerminalPanes() const
{
    QList<TerminalPane *> panes;
    panes.reserve(m_tabs->count());
    for (int index = 0; index < m_tabs->count(); ++index) {
        auto *workspace = qobject_cast<TerminalWorkspace *>(m_tabs->widget(index));
        if (workspace != nullptr && workspace->activePane() != nullptr) {
            panes.append(workspace->activePane());
        }
    }
    return panes;
}

void MainWindow::refreshProductivityTargets()
{
    if (m_productivityWidget == nullptr) {
        return;
    }
    QList<ConnectionTarget> targets;
    QSet<QString> seenHostIds;
    for (TerminalPane *pane : allTerminalPanes()) {
        ConnectionTarget target = pane->activeTarget();
        if (!pane->isConnected() || !target.isValid() || target.id.isEmpty()
            || seenHostIds.contains(target.id)) {
            continue;
        }
        // 效率侧栏按稳定主机 id 路由，同一主机打开多个标签时只显示一次；真正执行时会
        // 投递到全部匹配标签，避免界面出现无法区分的重复名称。侧栏不需要凭据，主动
        // 擦除副本可避免长期界面模型延长密码的内存驻留时间。
        seenHostIds.insert(target.id);
        target.password.fill(QChar::Null);
        target.password.clear();
        targets.append(std::move(target));
    }
    m_productivityWidget->setExecutionTargets(targets);
}

void MainWindow::applyConfiguredShortcuts()
{
    if (m_settingsService == nullptr) {
        return;
    }
    for (auto iterator = m_configurableActions.cbegin();
         iterator != m_configurableActions.cend(); ++iterator) {
        if (iterator.value() != nullptr) {
            iterator.value()->setShortcut(m_settingsService->shortcut(iterator.key()));
        }
    }
}

void MainWindow::openCommandPalette()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("命令面板"));
    dialog.resize(560, 420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *filterEdit = new QLineEdit(&dialog);
    filterEdit->setPlaceholderText(tr("输入命令名称筛选…"));
    auto *list = new QListWidget(&dialog);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(filterEdit);
    layout->addWidget(list, 1);
    layout->addWidget(new QLabel(tr("Enter 执行，Esc 关闭；这里只显示当前可用动作。"), &dialog));

    QList<QAction *> candidates;
    QSet<QAction *> seen;
    for (QAction *action : findChildren<QAction *>()) {
        if (action == nullptr || seen.contains(action) || action->isSeparator()
            || action->menu() != nullptr || !action->isVisible() || !action->isEnabled()
            || action->objectName() == QStringLiteral("CommandPaletteAction")) {
            continue;
        }
        QString text = action->text();
        text.remove(QLatin1Char('&'));
        text = text.trimmed();
        if (text.isEmpty()) {
            continue;
        }
        seen.insert(action);
        const int actionIndex = candidates.size();
        candidates.append(action);
        QString display = text;
        if (!action->shortcut().isEmpty()) {
            display += QStringLiteral("\t%1").arg(
                action->shortcut().toString(QKeySequence::NativeText));
        }
        auto *item = new QListWidgetItem(display, list);
        // 只存本次对话框局部数组下标，不把 QObject 指针塞入 QVariant；动作始终由主窗口
        // 拥有，但下标形式仍能避免元类型注册和跨架构指针整数转换。
        item->setData(Qt::UserRole, actionIndex);
        item->setToolTip(action->toolTip());
    }
    if (list->count() > 0) {
        list->setCurrentRow(0);
    }

    int selectedActionIndex = -1;
    const auto acceptCurrent = [&]() {
        QListWidgetItem *item = list->currentItem();
        if (item == nullptr || item->isHidden()) {
            return;
        }
        selectedActionIndex = item->data(Qt::UserRole).toInt();
        dialog.accept();
    };
    connect(filterEdit, &QLineEdit::textChanged, &dialog, [list](const QString &text) {
        const QString keyword = text.trimmed();
        int firstVisibleRow = -1;
        for (int row = 0; row < list->count(); ++row) {
            QListWidgetItem *item = list->item(row);
            const bool matches = keyword.isEmpty()
                || item->text().contains(keyword, Qt::CaseInsensitive);
            item->setHidden(!matches);
            if (matches && firstVisibleRow < 0) {
                firstVisibleRow = row;
            }
        }
        list->setCurrentRow(firstVisibleRow);
    });
    connect(filterEdit, &QLineEdit::returnPressed, &dialog, acceptCurrent);
    connect(list, &QListWidget::itemActivated, &dialog,
            [&acceptCurrent](QListWidgetItem *) { acceptCurrent(); });
    filterEdit->setFocus();

    if (dialog.exec() == QDialog::Accepted && selectedActionIndex >= 0
        && selectedActionIndex < candidates.size()) {
        // 对话框已经关闭后再触发，避免目标动作弹出的模态窗口与命令面板争夺父窗口和焦点。
        QPointer<QAction> selectedAction = candidates.at(selectedActionIndex);
        QTimer::singleShot(0, this, [selectedAction]() {
            if (selectedAction != nullptr && selectedAction->isEnabled()) {
                selectedAction->trigger();
            }
        });
    }
}

void MainWindow::searchCurrentTerminal()
{
    TerminalWorkspace *workspace = activeWorkspace();
    TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
    if (pane == nullptr) {
        return;
    }

    bool accepted = false;
    const QString text = QInputDialog::getText(this,
                                               tr("搜索终端输出"),
                                               tr("查找文本："),
                                               QLineEdit::Normal,
                                               m_lastTerminalSearch,
                                               &accepted);
    if (!accepted || text.isEmpty()) {
        return;
    }
    m_lastTerminalSearch = text;
    if (!pane->findTerminalText(text)) {
        statusBar()->showMessage(tr("终端输出中未找到“%1”").arg(text), 5000);
    }
}

void MainWindow::exportCurrentTerminal()
{
    TerminalWorkspace *workspace = activeWorkspace();
    TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
    if (pane == nullptr) {
        return;
    }
    const QString defaultName = QStringLiteral("shellium-terminal-%1.txt")
                                    .arg(QDateTime::currentDateTime()
                                             .toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this,
        tr("导出终端输出"),
        QDir::home().filePath(defaultName),
        tr("文本文件 (*.txt);;所有文件 (*)"));
    if (path.isEmpty()) {
        return;
    }

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this,
                              tr("导出失败"),
                              tr("无法打开 %1：%2").arg(path, output.errorString()));
        return;
    }
    const QByteArray content = pane->terminalPlainText().toUtf8();
    if (output.write(content) != content.size() || !output.commit()) {
        QMessageBox::critical(this,
                              tr("导出失败"),
                              tr("写入 %1 失败：%2").arg(path, output.errorString()));
        return;
    }
    statusBar()->showMessage(tr("终端输出已导出到 %1").arg(path), 5000);
}

void MainWindow::saveCurrentWorkspace()
{
    if (m_tabs == nullptr || m_tabs->count() == 0) {
        return;
    }
    QJsonArray tabs;
    for (int index = 0; index < m_tabs->count(); ++index) {
        auto *workspace = qobject_cast<TerminalWorkspace *>(m_tabs->widget(index));
        TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
        if (workspace == nullptr || pane == nullptr) {
            continue;
        }
        QString rootHostId;
        const QList<ConnectionTarget> targets = pane->targets();
        if (!targets.isEmpty()) {
            rootHostId = targets.constFirst().id;
            QString lookupError;
            const std::optional<HostRecord> savedHost =
                m_database->findHost(rootHostId, &lookupError);
            if (!savedHost.has_value() || savedHost->kind != HostNodeKind::Host) {
                // 临时连接的随机 UUID 无法在下次启动时还原地址、跳板链和凭据引用。静默把它
                // 保存成本地 Shell 会让用户误以为工作区完整，故在写库前整体拒绝。
                QMessageBox::warning(
                    this,
                    tr("工作区包含临时连接"),
                    lookupError.isEmpty()
                        ? tr("标签“%1”不是从主机树打开的持久化主机。请先把该连接保存到主机树，"
                             "再保存工作区。")
                              .arg(m_tabs->tabText(index))
                        : tr("检查标签“%1”的主机记录失败：%2")
                              .arg(m_tabs->tabText(index), lookupError));
                return;
            }
        }

        QJsonObject tab;
        tab.insert(QStringLiteral("title"), m_tabs->tabText(index));
        tab.insert(QStringLiteral("hostId"), rootHostId);
        tab.insert(QStringLiteral("filesVisible"), workspace->isFileManagerVisible());
        tabs.append(tab);
    }
    if (tabs.isEmpty()) {
        QMessageBox::information(this, tr("没有可保存标签"), tr("当前窗口没有有效终端标签。"));
        return;
    }

    bool accepted = false;
    const QString name = QInputDialog::getText(this,
                                               tr("保存工作区"),
                                               tr("工作区名称："),
                                               QLineEdit::Normal,
                                               QString(),
                                               &accepted).trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }

    QString workspaceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    QString error;
    const QList<WorkspaceRecord> existing = m_database->listWorkspaces(&ok, &error);
    if (!ok) {
        QMessageBox::critical(this, tr("保存工作区失败"), error);
        return;
    }
    const auto sameName = std::find_if(
        existing.cbegin(), existing.cend(), [&name](const WorkspaceRecord &record) {
            return record.name.compare(name, Qt::CaseInsensitive) == 0;
        });
    if (sameName != existing.cend()) {
        if (QMessageBox::question(this,
                                  tr("覆盖已有工作区"),
                                  tr("已存在名为“%1”的工作区，是否用当前标签组合覆盖？").arg(name),
                                  QMessageBox::Yes | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Yes) {
            return;
        }
        workspaceId = sameName->id;
    }

    QJsonObject layout;
    layout.insert(QStringLiteral("version"), 1);
    layout.insert(QStringLiteral("tabs"), tabs);
    WorkspaceRecord record;
    record.id = workspaceId;
    record.name = name;
    record.layoutJson = QString::fromUtf8(QJsonDocument(layout).toJson(QJsonDocument::Compact));
    record.updatedAt = QDateTime::currentDateTimeUtc();
    if (!m_database->saveWorkspace(record, &error)) {
        QMessageBox::critical(this, tr("保存工作区失败"), error);
        return;
    }
    statusBar()->showMessage(tr("工作区“%1”已保存（%2 个标签）").arg(name).arg(tabs.size()),
                             5000);
}

void MainWindow::openSavedWorkspace()
{
    bool ok = false;
    QString error;
    const QList<WorkspaceRecord> records = m_database->listWorkspaces(&ok, &error);
    if (!ok) {
        QMessageBox::critical(this, tr("读取工作区失败"), error);
        return;
    }
    if (records.isEmpty()) {
        QMessageBox::information(this, tr("没有工作区"), tr("请先保存当前工作区。"));
        return;
    }

    QStringList labels;
    QHash<QString, QString> idByLabel;
    for (const WorkspaceRecord &record : records) {
        const QString label = tr("%1  ·  %2")
                                  .arg(record.name,
                                       record.updatedAt.toLocalTime().toString(
                                           QStringLiteral("yyyy-MM-dd HH:mm")));
        // 名称和更新时间都可能相同，附加短 id 保证 QInputDialog 返回文本可无歧义映射。
        QString uniqueLabel = label;
        if (idByLabel.contains(uniqueLabel)) {
            uniqueLabel += QStringLiteral("  [%1]").arg(record.id.left(8));
        }
        labels.append(uniqueLabel);
        idByLabel.insert(uniqueLabel, record.id);
    }
    bool accepted = false;
    const QString selectedLabel = QInputDialog::getItem(this,
                                                        tr("打开工作区"),
                                                        tr("选择工作区："),
                                                        labels,
                                                        0,
                                                        false,
                                                        &accepted);
    if (!accepted || selectedLabel.isEmpty()) {
        return;
    }
    const std::optional<WorkspaceRecord> selected =
        m_database->findWorkspace(idByLabel.value(selectedLabel), &error);
    if (!selected.has_value()) {
        QMessageBox::critical(this,
                              tr("读取工作区失败"),
                              error.isEmpty() ? tr("所选工作区已被删除") : error);
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        selected->layoutJson.toUtf8(), &parseError);
    const QJsonObject layout = document.object();
    const QJsonArray serializedTabs = layout.value(QStringLiteral("tabs")).toArray();
    if (!document.isObject() || layout.value(QStringLiteral("version")).toInt(-1) != 1
        || serializedTabs.isEmpty() || serializedTabs.size() > 64) {
        QMessageBox::critical(
            this,
            tr("工作区布局损坏"),
            tr("工作区“%1”的版本、标签列表或数量无效，当前会话未被修改。")
                .arg(selected->name));
        return;
    }

    struct RestoredTab {
        QString title;
        QString hostId;
        bool filesVisible = true;
        ConnectionTarget target;
    };
    QList<RestoredTab> restoredTabs;
    restoredTabs.reserve(serializedTabs.size());
    for (const QJsonValue &value : serializedTabs) {
        if (!value.isObject()) {
            error = tr("标签项不是对象");
            break;
        }
        const QJsonObject tab = value.toObject();
        if (!tab.value(QStringLiteral("title")).isString()
            || !tab.value(QStringLiteral("hostId")).isString()
            || !tab.value(QStringLiteral("filesVisible")).isBool()) {
            error = tr("标签字段类型无效");
            break;
        }
        RestoredTab restored;
        restored.title = tab.value(QStringLiteral("title")).toString().trimmed();
        restored.hostId = tab.value(QStringLiteral("hostId")).toString().trimmed();
        restored.filesVisible = tab.value(QStringLiteral("filesVisible")).toBool();
        if (restored.title.isEmpty()) {
            restored.title = tr("终端");
        }
        if (!restored.hostId.isEmpty()) {
            QString hostError;
            const std::optional<HostRecord> host =
                m_database->resolveHost(restored.hostId, &hostError);
            if (!host.has_value() || host->kind != HostNodeKind::Host) {
                error = hostError.isEmpty()
                    ? tr("主机引用不存在：%1").arg(restored.hostId)
                    : hostError;
                break;
            }
            restored.target = host->toConnectionTarget();
        }
        restoredTabs.append(restored);
    }
    if (!error.isEmpty() || restoredTabs.size() != serializedTabs.size()) {
        QMessageBox::critical(
            this,
            tr("工作区布局损坏"),
            tr("工作区“%1”无法完整恢复：%2。当前会话未被修改。")
                .arg(selected->name, error.isEmpty() ? parseError.errorString() : error));
        return;
    }

    if (QMessageBox::question(
            this,
            tr("替换当前工作区"),
            tr("打开“%1”会关闭当前 %2 个标签及其连接，然后恢复 %3 个标签。是否继续？")
                .arg(selected->name)
                .arg(m_tabs->count())
                .arg(restoredTabs.size()),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    while (m_tabs->count() > 0) {
        QWidget *page = m_tabs->widget(0);
        m_tabs->removeTab(0);
        // 会话清理可能等待网络线程；deleteLater 让本次菜单回调先完成，同时被移除页面不会
        // 再出现在用户界面。其析构仍在 GUI 线程执行，不会跨线程删除 Qt 网络对象。
        page->deleteLater();
    }
    for (const RestoredTab &restored : std::as_const(restoredTabs)) {
        TerminalWorkspace *workspace = createWorkspaceTab(restored.title);
        workspace->setFileManagerVisible(restored.filesVisible);
        m_tabs->setCurrentWidget(workspace);
        if (restored.hostId.isEmpty()) {
            workspace->activePane()->openLocalShell();
        } else {
            connectSavedHost(restored.hostId, restored.target);
            const int index = m_tabs->indexOf(workspace);
            if (index >= 0) {
                // connectSavedHost 默认使用主机显示名；工作区显式保存的标签名优先。
                m_tabs->setTabText(index, restored.title);
            }
        }
    }
    m_tabs->setCurrentIndex(0);
    m_workspace = activeWorkspace();
    statusBar()->showMessage(tr("工作区“%1”已恢复").arg(selected->name), 5000);
}

void MainWindow::removeSavedWorkspace()
{
    bool ok = false;
    QString error;
    const QList<WorkspaceRecord> records = m_database->listWorkspaces(&ok, &error);
    if (!ok) {
        QMessageBox::critical(this, tr("读取工作区失败"), error);
        return;
    }
    if (records.isEmpty()) {
        QMessageBox::information(this, tr("没有工作区"), tr("当前没有可删除的工作区。"));
        return;
    }
    QStringList labels;
    QHash<QString, QString> idByLabel;
    for (const WorkspaceRecord &record : records) {
        QString label = QStringLiteral("%1  [%2]").arg(record.name, record.id.left(8));
        labels.append(label);
        idByLabel.insert(label, record.id);
    }
    bool accepted = false;
    const QString selectedLabel = QInputDialog::getItem(this,
                                                        tr("删除工作区"),
                                                        tr("选择要删除的工作区："),
                                                        labels,
                                                        0,
                                                        false,
                                                        &accepted);
    if (!accepted || selectedLabel.isEmpty()) {
        return;
    }
    if (QMessageBox::warning(this,
                             tr("确认删除工作区"),
                             tr("仅删除保存的布局，不会关闭当前终端。此操作不可撤销。"),
                             QMessageBox::Yes | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    int removed = 0;
    if (!m_database->removeWorkspace(idByLabel.value(selectedLabel), &removed, &error)) {
        QMessageBox::critical(this, tr("删除工作区失败"), error);
        return;
    }
    statusBar()->showMessage(removed > 0 ? tr("工作区已删除") : tr("工作区已经不存在"), 5000);
}

void MainWindow::executeOnTargets(const QString &command, const QStringList &targetIds)
{
    if (targetIds.isEmpty()) {
        TerminalWorkspace *workspace = activeWorkspace();
        TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
        if (pane != nullptr) {
            pane->executeCommand(command);
        }
        return;
    }

    int delivered = 0;
    for (TerminalPane *pane : allTerminalPanes()) {
        const ConnectionTarget target = pane->activeTarget();
        if (pane->isConnected() && targetIds.contains(target.id)) {
            pane->executeCommand(command);
            ++delivered;
        }
    }
    if (delivered == 0) {
        statusBar()->showMessage(tr("选中的执行目标已关闭或尚未连接"), 5000);
    }
}

void MainWindow::sendBytesToTargets(const QByteArray &bytes,
                                    const QStringList &targetIds,
                                    TerminalPane *excludedSource)
{
    if (bytes.isEmpty() || targetIds.isEmpty()) {
        return;
    }
    for (TerminalPane *pane : allTerminalPanes()) {
        if (pane == excludedSource || !pane->isConnected()) {
            continue;
        }
        if (targetIds.contains(pane->activeTarget().id)) {
            // 接收端直接写 worker，不重发 broadcastSourceInput；否则两个开启广播的标签会
            // 形成 A→B→A 的无限输入回路。
            pane->sendInputBytes(bytes);
        }
    }
}
