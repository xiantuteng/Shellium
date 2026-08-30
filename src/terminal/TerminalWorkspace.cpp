#include "terminal/TerminalWorkspace.h"

#include "sftp/RemoteFileManagerWidget.h"
#include "terminal/TerminalPane.h"
#include "terminal/TerminalView.h"

#include <QBoxLayout>
#include <QMessageBox>
#include <QSplitter>

TerminalWorkspace::TerminalWorkspace(QWidget *parent)
    : QWidget(parent)
    , m_pageSplitter(new QSplitter(Qt::Horizontal, this))
    , m_fileManager(new RemoteFileManagerWidget(m_pageSplitter))
    , m_pane(new TerminalPane(m_pageSplitter))
{
    auto *pageLayout = new QHBoxLayout(this);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);
    pageLayout->addWidget(m_pageSplitter);

    // 终端在左、文件管理在右，两者并排分屏；文件管理器打开或关闭只改变自身的显隐和
    // 分隔器占宽，终端会话始终保持可见，不会被文件管理器盖住。
    m_pageSplitter->insertWidget(0, m_pane);
    m_pane->setMinimumWidth(360);
    m_fileManager->setMinimumWidth(420);
    m_pageSplitter->setChildrenCollapsible(false);
    m_pageSplitter->setStretchFactor(0, 3);
    m_pageSplitter->setStretchFactor(1, 1);
    m_pageSplitter->setSizes({920, 460});

    // 文件管理产生的提示（"正在读取主目录…"之类）只与本标签页有关，因此直接显示在本
    // 标签会话状态栏、服务器名旁边，不再经窗口标题栏的全局提示区绕一圈——那个位置离
    // 用户正在看的内容太远，容易被当成与当前标签无关的系统消息而忽略。终端本身的连接
    // 生命周期消息（"正在连接…""已连接"等）继续走原有的全局提示区，未作改动。
    connect(m_fileManager,
            &RemoteFileManagerWidget::statusMessage,
            m_pane,
            &TerminalPane::showTransientStatus);
    // 传输结果单独走带图标的展示入口，覆盖拖拽上传、右键菜单上传和面板自身按钮触发的
    // 全部上传/下载——三者共用同一条完成通知路径，图标和消失时机天然保持一致。
    connect(m_fileManager,
            &RemoteFileManagerWidget::transferResultAvailable,
            m_pane,
            &TerminalPane::showTransientTransferResult);
    // 全屏时让终端整体让出空间，由分隔器把宽度全部交给文件管理；终端进程和滚动缓冲
    // 都不受影响，退出全屏即原样恢复。
    connect(m_fileManager,
            &RemoteFileManagerWidget::fullScreenChanged,
            this,
            [this](const bool fullScreen) { m_pane->setVisible(!fullScreen); });
    // 面板自带的关闭按钮与终端顶部的文件管理按钮必须落到同一条路径上：只有经过
    // setFileManagerVisible，才会顺带退出全屏并把终端按钮的选中态改回未展开。
    connect(m_fileManager,
            &RemoteFileManagerWidget::closeRequested,
            this,
            [this] { setFileManagerVisible(false); });
    connect(m_pane, &TerminalPane::targetsChanged, this, [this](TerminalPane *pane) {
        synchronizeFileManager(true);
        emitTerminalStatus();
        emit activeTargetsChanged(pane);
    });
    connect(m_pane, &TerminalPane::statusMessage, this, &TerminalWorkspace::statusMessage);
    connect(m_pane, &TerminalPane::closeRequested, this, [this](TerminalPane *) {
        // 面板只知道"用户要关掉这里"，标签索引和 AI 助手页面都归主窗口管理；本工作区
        // 只负责把请求换成自己的身份继续上报。
        emit closeRequested(this);
    });
    connect(m_pane,
            &TerminalPane::remoteWorkingDirectoryChanged,
            m_fileManager,
            &RemoteFileManagerWidget::followTerminalWorkingDirectory);
    connect(m_pane,
            &TerminalPane::fileManagerVisibilityRequested,
            this,
            &TerminalWorkspace::setFileManagerVisible);

    // TerminalPane 对外只暴露业务层连接状态；字符尺寸由其唯一的 TerminalView 产生。
    // 通过 QObject 子树查找可保持 Pane 的网络接口不变，同时把纯展示状态提升到工作区层。
    if (TerminalView *terminalView = m_pane->findChild<TerminalView *>();
        terminalView != nullptr) {
        connect(terminalView,
                &TerminalView::terminalSizeChanged,
                this,
                [this](const int columns, const int rows) {
                    m_terminalColumns = columns;
                    m_terminalRows = rows;
                    emitTerminalStatus();
                });
        // 右键动作必须路由到当前 TerminalWorkspace 自己的文件管理器。不能经过 MainWindow
        // 查找“当前标签”，因为菜单弹出后用户可能用快捷键切换标签，异步文件选择器若再按
        // 全局当前项解析就可能把文件上传到另一台服务器。
        connect(terminalView,
                &TerminalView::uploadToCurrentDirectoryRequested,
                m_fileManager,
                &RemoteFileManagerWidget::requestUploadToCurrentDirectory);
        // 同样的“不查全局当前标签”理由：拖拽落点必然就在本工作区的终端视图上，直接处理
        // 而不经 MainWindow 转发，标签切换期间也不会有歧义。
        connect(terminalView,
                &TerminalView::filesDropped,
                this,
                &TerminalWorkspace::handleFilesDropped);
    }

    // 默认收起远程文件管理，终端独占整个标签页宽度；用户按需展开后该标签会记住选择。
    setFileManagerVisible(false);
    synchronizeFileManager(false);
}

TerminalPane *TerminalWorkspace::activePane() const
{
    return m_pane;
}

void TerminalWorkspace::setFileManagerVisible(const bool visible)
{
    if (visible && !m_fileManagerAllowed) {
        // 快捷键和按钮最终都汇入这里。布局不允许时保持显式关闭，避免仅禁用按钮却被全局
        // Ctrl+Shift+F 绕过，从而在网格单元中意外展开远程文件管理。
        m_pane->setFileManagerVisible(false);
        return;
    }
    if (!visible) {
        // 必须先退出全屏：全屏状态下终端是被显式隐藏的，若此时又把文件管理一并隐藏，
        // 标签页里两块内容会同时消失，只剩一片空白且没有任何可点的恢复入口。
        m_fileManager->exitFullScreen();
    }
    // QWidget::setVisible 在标签页本身不可见时仍会正确维护显隐意图；切回标签页后 Qt 会
    // 根据该 hidden 状态恢复布局，因此每个标签可以拥有不同的文件管理显示设置。
    m_fileManager->setVisible(visible);
    // 顶部按钮既可能由用户点击，也可能由工作区恢复或快捷键间接改变。所有入口最终都在
    // 这里汇合并回写按钮，才能保证视觉状态与实际分隔器状态始终一致。
    m_pane->setFileManagerVisible(visible);
}

bool TerminalWorkspace::isFileManagerVisible() const
{
    // 不能使用 isVisible()：非当前标签的所有子控件都会临时返回 false。isHidden() 只反映
    // 用户是否显式隐藏了该控件，适合在标签切换和工作区恢复时同步顶部会话按钮状态。
    return !m_fileManager->isHidden();
}

void TerminalWorkspace::setFileManagerAllowed(const bool allowed)
{
    if (m_fileManagerAllowed == allowed) {
        return;
    }
    if (!allowed) {
        // 先通过统一显隐入口退出文件管理全屏并恢复终端，再禁用按钮；反过来操作可能让
        // checked 状态被冻结在 true，切回允许布局后出现按钮与页面不一致。
        setFileManagerVisible(false);
    }
    m_fileManagerAllowed = allowed;
    m_pane->setFileManagerEnabled(allowed);
    // 分屏（!allowed）时放宽终端面板的最小宽度：否则多块并列会让递归分屏树的最小总宽
    // 超过工作空间可用宽度，QSplitter 只能向右溢出，把最靠右那块（连同右上角按钮）挤出
    // 可视区。180px 仍能容下标题栏的全部按钮和一段可读终端。切回单终端布局恢复 360。
    m_pane->setMinimumWidth(allowed ? 360 : 180);
}

bool TerminalWorkspace::isFileManagerAllowed() const
{
    return m_fileManagerAllowed;
}

void TerminalWorkspace::requestClose()
{
    emit closeRequested(this);
}

void TerminalWorkspace::synchronizeFileManager(const bool followInnermost)
{
    // SFTP 会立即建立独立连接，因此必须等终端完成主机密钥确认和认证。连接中、已断开或
    // 本地 Shell 都传入空列表，避免本标签页继续显示上一条会话的远程文件内容。
    m_fileManager->setAvailableTargets(m_pane->isConnected() ? m_pane->targets()
                                                             : QList<ConnectionTarget>{},
                                       followInnermost);
}

void TerminalWorkspace::emitTerminalStatus()
{
    const ConnectionTarget target = m_pane->activeTarget();
    QString server;
    if (target.isValid()) {
        server = target.connectionSpecifier(true);
        if (target.confidence == ConnectionTarget::Confidence::Inferred) {
            server.prepend(tr("Inferred · "));
        }
    } else {
        server = m_pane->isConnected() ? tr("Local terminal") : tr("Not connected");
    }

    // libvterm 与所有 SSH/PTY 输入输出均以 UTF-8 工作；在真正支持可切换编码前，明确发出
    // 常量比让主窗口猜测更可靠，也为以后增加编码设置预留稳定信号形状。
    emit terminalStatusChanged(server,
                               QStringLiteral("UTF-8"),
                               m_terminalColumns,
                               m_terminalRows);
}

void TerminalWorkspace::handleFilesDropped(const QStringList &localPaths)
{
    if (localPaths.isEmpty()) {
        return;
    }
    if (!m_pane->isConnected()) {
        emit statusMessage(tr("Not connected yet; cannot upload the dropped files."));
        return;
    }
    const ConnectionTarget target = m_pane->activeTarget();
    if (!target.isValid()) {
        // 本地 Shell 没有远程目标；虽然本地终端也在“已连接”状态，拖拽上传对它没有意义。
        emit statusMessage(tr("This is a local terminal; dropped files cannot be uploaded."));
        return;
    }
    const QString remoteDirectory = m_pane->activeWorkingDirectory();
    if (remoteDirectory.isEmpty()) {
        // cwd 缓存来自 Shell 集成上报，连接刚建立、提示符还没刷新过一次时可能仍是空的；
        // 此时没有可信目标目录，宁可让用户重试也不要猜一个根目录。
        QMessageBox::warning(this,
                             tr("Remote path unknown"),
                             tr("The current directory is not known yet; press Enter once in the terminal, then drop the files."));
        return;
    }

    QString serverDescription = target.connectionSpecifier(true);
    if (target.confidence == ConnectionTarget::Confidence::Inferred) {
        // 与 emitTerminalStatus 相同的显式标注：推测层的身份不是本程序亲自确认的，必须让
        // 用户在确认上传前就看到这一点。
        serverDescription.prepend(tr("Inferred · "));
    }
    const QMessageBox::StandardButton choice = QMessageBox::question(
        this,
        tr("Upload to the current directory?"),
        tr("Upload the %1 dropped local files to the current directory of %2:\n%3")
            .arg(localPaths.size())
            .arg(serverDescription, remoteDirectory),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (choice != QMessageBox::Yes) {
        return;
    }
    m_fileManager->uploadFilesToDirectory(target, remoteDirectory, localPaths);
}
