#include "terminal/TerminalWorkspace.h"

#include "sftp/RemoteFileManagerWidget.h"
#include "terminal/TerminalPane.h"

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

    // 终端在左、文件管理在右。分隔器只负责两者的宽度比例，由 Qt 原生分隔条处理拖动和
    // 窗口缩放。
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
        emit activeTargetsChanged(pane);
    });
    connect(m_pane, &TerminalPane::statusMessage, this, &TerminalWorkspace::statusMessage);

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
}

bool TerminalWorkspace::isFileManagerVisible() const
{
    // 不能使用 isVisible()：非当前标签的所有子控件都会临时返回 false。isHidden() 只反映
    // 用户是否显式隐藏了该控件，适合在标签切换时恢复工具栏开关状态。
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
