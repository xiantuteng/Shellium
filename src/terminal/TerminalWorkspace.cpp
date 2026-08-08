#include "terminal/TerminalWorkspace.h"

#include "sftp/RemoteFileManagerWidget.h"
#include "terminal/TerminalPane.h"
#include "terminal/TerminalView.h"

#include <QBoxLayout>
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

    connect(m_fileManager,
            &RemoteFileManagerWidget::statusMessage,
            this,
            &TerminalWorkspace::statusMessage);
    connect(m_pane, &TerminalPane::targetsChanged, this, [this](TerminalPane *pane) {
        synchronizeFileManager(true);
        emitTerminalStatus();
        emit activeTargetsChanged(pane);
    });
    connect(m_pane, &TerminalPane::statusMessage, this, &TerminalWorkspace::statusMessage);
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

void TerminalWorkspace::synchronizeFileManager(const bool followInnermost)
{
    // SFTP 会立即建立独立连接，因此必须等终端完成主机密钥确认和认证。连接中、已断开或
    // 本地 zsh 都传入空列表，避免本标签页继续显示上一条会话的远程文件内容。
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
            server.prepend(tr("推测 · "));
        }
    } else {
        server = m_pane->isConnected() ? tr("本地终端") : tr("未连接");
    }

    // libvterm 与所有 SSH/PTY 输入输出均以 UTF-8 工作；在真正支持可切换编码前，明确发出
    // 常量比让主窗口猜测更可靠，也为以后增加编码设置预留稳定信号形状。
    emit terminalStatusChanged(server,
                               QStringLiteral("UTF-8"),
                               m_terminalColumns,
                               m_terminalRows);
}
