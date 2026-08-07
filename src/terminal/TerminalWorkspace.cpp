#include "terminal/TerminalWorkspace.h"

#include "sftp/RemoteFileManagerWidget.h"
#include "terminal/TerminalPane.h"

#include <QBoxLayout>
#include <QSplitter>

TerminalWorkspace::TerminalWorkspace(QWidget *parent)
    : QWidget(parent)
    , m_pageSplitter(new QSplitter(Qt::Horizontal, this))
    , m_terminalArea(new QWidget(m_pageSplitter))
    , m_rootLayout(new QVBoxLayout(m_terminalArea))
    , m_fileManager(new RemoteFileManagerWidget(m_pageSplitter))
{
    auto *pageLayout = new QHBoxLayout(this);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);
    pageLayout->addWidget(m_pageSplitter);

    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(0);
    m_activePane = createPane();
    m_rootLayout->addWidget(m_activePane);

    // 文件管理器是标签页内容的一部分。外层分隔器只负责终端区域与文件区域的宽度比例，
    // 内层终端分屏树仍由 splitActive 独立维护，二者不会互相干扰。
    m_terminalArea->setMinimumWidth(360);
    m_fileManager->setMinimumWidth(420);
    m_pageSplitter->setChildrenCollapsible(false);
    m_pageSplitter->setStretchFactor(0, 3);
    m_pageSplitter->setStretchFactor(1, 1);
    m_pageSplitter->setSizes({920, 460});

    connect(m_fileManager,
            &RemoteFileManagerWidget::statusMessage,
            this,
            &TerminalWorkspace::statusMessage);
    synchronizeFileManager(m_activePane, false);
}

TerminalPane *TerminalWorkspace::activePane() const
{
    return m_activePane;
}

TerminalPane *TerminalWorkspace::splitActive(const Qt::Orientation orientation,
                                             const ConnectionTarget &target)
{
    if (m_activePane == nullptr) {
        return nullptr;
    }

    TerminalPane *newPane = createPane();
    QWidget *oldPane = m_activePane;
    auto *parentSplitter = qobject_cast<QSplitter *>(oldPane->parentWidget());

    if (parentSplitter != nullptr && parentSplitter->orientation() == orientation) {
        const int oldIndex = parentSplitter->indexOf(oldPane);
        parentSplitter->insertWidget(oldIndex + 1, newPane);
    } else {
        auto *newSplitter = new QSplitter(orientation);
        newSplitter->setChildrenCollapsible(false);

        if (parentSplitter != nullptr) {
            const int oldIndex = parentSplitter->indexOf(oldPane);
            // insertWidget 会先把 oldPane 从旧父级移走；随后把新分隔器放回相同索引，
            // 从而保持旧布局中其他分屏的相对顺序。
            oldPane->setParent(newSplitter);
            newSplitter->addWidget(oldPane);
            newSplitter->addWidget(newPane);
            parentSplitter->insertWidget(oldIndex, newSplitter);
        } else {
            m_rootLayout->removeWidget(oldPane);
            newSplitter->addWidget(oldPane);
            newSplitter->addWidget(newPane);
            m_rootLayout->addWidget(newSplitter);
        }
        newSplitter->setSizes({1, 1});
    }

    m_activePane = newPane;
    newPane->connectToHost(target);
    newPane->focusTerminal();
    synchronizeFileManager(newPane, true);
    emit activePaneChanged(newPane);
    return newPane;
}

void TerminalWorkspace::closeActivePane()
{
    if (m_activePane == nullptr) {
        return;
    }

    const QList<TerminalPane *> allPanes = panes();
    if (allPanes.size() <= 1) {
        // 最后一个面板承载工作区生命周期，不直接删除，避免中心区域变成无法恢复的空布局。
        return;
    }

    TerminalPane *closingPane = m_activePane;
    auto *parentSplitter = qobject_cast<QSplitter *>(closingPane->parentWidget());
    TerminalPane *nextPane = nullptr;
    for (TerminalPane *candidate : allPanes) {
        if (candidate != closingPane) {
            nextPane = candidate;
            break;
        }
    }

    closingPane->deleteLater();
    m_activePane = nextPane;
    if (parentSplitter != nullptr) {
        // 延迟到 deleteLater 真正移除控件之后再折叠分隔器，避免读取过期的 count。
        QMetaObject::invokeMethod(this, [this, parentSplitter]() {
            collapseRedundantSplitter(parentSplitter);
        }, Qt::QueuedConnection);
    }
    if (nextPane != nullptr) {
        nextPane->focusTerminal();
        synchronizeFileManager(nextPane, false);
        emit activePaneChanged(nextPane);
    }
}

QList<TerminalPane *> TerminalWorkspace::panes() const
{
    return findChildren<TerminalPane *>();
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

TerminalPane *TerminalWorkspace::createPane()
{
    auto *pane = new TerminalPane(this);
    connect(pane, &TerminalPane::activated, this, [this](TerminalPane *activatedPane) {
        if (m_activePane != activatedPane) {
            m_activePane = activatedPane;
            synchronizeFileManager(activatedPane, false);
            emit activePaneChanged(activatedPane);
        }
    });
    connect(pane, &TerminalPane::targetsChanged, this, [this](TerminalPane *changedPane) {
        if (m_activePane == changedPane) {
            synchronizeFileManager(changedPane, true);
            emit activeTargetsChanged(changedPane);
        }
    });
    connect(pane, &TerminalPane::statusMessage, this, &TerminalWorkspace::statusMessage);
    return pane;
}

void TerminalWorkspace::synchronizeFileManager(TerminalPane *pane,
                                               const bool followInnermost)
{
    // SFTP 会立即建立独立连接，因此必须等终端完成主机密钥确认和认证。连接中、已断开或
    // 本地 zsh 面板都传入空列表，避免本标签页继续显示上一条会话的远程文件内容。
    m_fileManager->setAvailableTargets(pane == nullptr || !pane->isConnected()
                                           ? QList<ConnectionTarget>{}
                                           : pane->targets(),
                                       followInnermost);
}

void TerminalWorkspace::collapseRedundantSplitter(QSplitter *splitter)
{
    if (splitter == nullptr || splitter->count() != 1) {
        return;
    }

    QWidget *remaining = splitter->widget(0);
    auto *parentSplitter = qobject_cast<QSplitter *>(splitter->parentWidget());
    if (parentSplitter != nullptr) {
        const int index = parentSplitter->indexOf(splitter);
        remaining->setParent(parentSplitter);
        parentSplitter->insertWidget(index, remaining);
        splitter->deleteLater();
        collapseRedundantSplitter(parentSplitter);
    } else {
        m_rootLayout->removeWidget(splitter);
        // 顶层终端分屏必须回到终端容器，而不是整个标签页；否则它会越过页面分隔器并覆盖
        // 同一标签中的远程文件管理区域。
        remaining->setParent(m_terminalArea);
        m_rootLayout->addWidget(remaining);
        splitter->deleteLater();
    }
}
