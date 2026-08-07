#include "app/MainWindow.h"

#include "terminal/TerminalPane.h"
#include "terminal/TerminalWorkspace.h"
#include "ui/ConnectionDialog.h"

#include <QAction>
#include <QKeySequence>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_tabs(new QTabWidget(this))
{
    setWindowTitle(tr("Shellium"));
    resize(1380, 860);
    m_tabs->setDocumentMode(true);
    m_tabs->setMovable(true);
    m_tabs->setTabsClosable(true);
    setCentralWidget(m_tabs);
    m_workspace = createWorkspaceTab(tr("终端 1"));

    auto *connectAction = new QAction(tr("连接服务器"), this);
    connectAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")));
    connectAction->setToolTip(tr("在当前终端面板中连接 SSH 服务器"));

    auto *localShellAction = new QAction(tr("本地 zsh"), this);
    localShellAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+L")));
    localShellAction->setToolTip(tr("在当前终端面板中启动本地登录式 zsh"));

    auto *newTabAction = new QAction(tr("新建标签页"), this);
    newTabAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));

    auto *closeTabAction = new QAction(tr("关闭标签页"), this);
    closeTabAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));

    auto *renameTabAction = new QAction(tr("重命名标签页"), this);

    auto *horizontalSplitAction = new QAction(tr("左右分屏"), this);
    horizontalSplitAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+D")));
    horizontalSplitAction->setToolTip(tr("在当前终端右侧创建独立 SSH 终端"));

    auto *verticalSplitAction = new QAction(tr("上下分屏"), this);
    verticalSplitAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));
    verticalSplitAction->setToolTip(tr("在当前终端下方创建独立 SSH 终端"));

    auto *closeSplitAction = new QAction(tr("关闭当前分屏"), this);
    closeSplitAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+W")));

    auto *toggleFilesAction = new QAction(tr("显示远程文件管理"), this);
    toggleFilesAction->setCheckable(true);
    toggleFilesAction->setChecked(true);
    toggleFilesAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    toggleFilesAction->setToolTip(tr("显示或隐藏当前终端标签页内部的远程文件管理区域"));

    QToolBar *toolbar = addToolBar(tr("主工具栏"));
    toolbar->setObjectName(QStringLiteral("MainToolBar"));
    toolbar->setMovable(false);
    toolbar->addAction(connectAction);
    toolbar->addAction(localShellAction);
    toolbar->addAction(newTabAction);
    toolbar->addSeparator();
    toolbar->addAction(horizontalSplitAction);
    toolbar->addAction(verticalSplitAction);
    toolbar->addAction(closeSplitAction);
    toolbar->addSeparator();
    toolbar->addAction(toggleFilesAction);

    QMenu *connectionMenu = menuBar()->addMenu(tr("连接"));
    connectionMenu->addAction(connectAction);
    connectionMenu->addAction(localShellAction);
    connectionMenu->addAction(newTabAction);
    connectionMenu->addAction(closeTabAction);
    connectionMenu->addAction(renameTabAction);
    connectionMenu->addSeparator();
    connectionMenu->addAction(horizontalSplitAction);
    connectionMenu->addAction(verticalSplitAction);
    connectionMenu->addAction(closeSplitAction);

    QMenu *viewMenu = menuBar()->addMenu(tr("视图"));
    viewMenu->addAction(toggleFilesAction);

    statusBar()->showMessage(tr("就绪"));

    connect(connectAction, &QAction::triggered, this, &MainWindow::connectCurrentPane);
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
    });
    connect(horizontalSplitAction, &QAction::triggered, this, [this]() {
        // Qt::Horizontal 的 QSplitter 把子控件左右排列，对应产品语义“左右分屏”。
        splitCurrentPane(Qt::Horizontal);
    });
    connect(verticalSplitAction, &QAction::triggered, this, [this]() {
        // Qt::Vertical 的 QSplitter 把子控件上下排列，对应产品语义“上下分屏”。
        splitCurrentPane(Qt::Vertical);
    });
    connect(closeSplitAction, &QAction::triggered, this, [this]() {
        // 动作属于主窗口，但关闭目标必须在触发时解析，不能永久绑定构造阶段的第一个标签页。
        TerminalWorkspace *workspace = activeWorkspace();
        if (workspace != nullptr) {
            workspace->closeActivePane();
        }
    });
    connect(toggleFilesAction, &QAction::toggled, this, [this](const bool visible) {
        TerminalWorkspace *workspace = activeWorkspace();
        if (workspace != nullptr) {
            workspace->setFileManagerVisible(visible);
        }
    });
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

void MainWindow::splitCurrentPane(const Qt::Orientation orientation)
{
    TerminalWorkspace *workspace = activeWorkspace();
    TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
    if (pane == nullptr) {
        return;
    }

    ConnectionDialog dialog(this);
    if (pane->activeTarget().isValid()) {
        // 分屏的最常见场景是同服务器并行操作，因此预填当前服务器；用户仍可改为其他服务器。
        dialog.setInitialTarget(pane->activeTarget());
    }
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    workspace->splitActive(orientation, dialog.target());
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
    return workspace;
}

TerminalWorkspace *MainWindow::activeWorkspace() const
{
    return qobject_cast<TerminalWorkspace *>(m_tabs->currentWidget());
}
