#include "app/MainWindow.h"
#include "i18n/LocalizationService.h"
#include "core/ConnectionTarget.h"
#include "core/HostRecord.h"
#include "security/Vault.h"
#include "storage/ApplicationDatabase.h"
#include "terminal/TerminalPane.h"
#include "terminal/TerminalView.h"
#include "terminal/TerminalWorkspace.h"
#include "terminal/TerminalWorkspaceGroup.h"
#include "ui/HostDistributionIcon.h"
#include "ui/HostManagerWidget.h"
#include "ui/SettingsDialog.h"
#include "ui/SettingsWidgets.h"
#include "ui/SessionTabBar.h"
#include "ui/SessionTabDropOverlay.h"
#include "ui/ButtonCursorPolicy.h"

#include <QAbstractButton>
#include <QAbstractItemDelegate>
#include <QAction>
#include <QMouseEvent>
#include <QStyleOptionViewItem>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QSignalSpy>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStandardPaths>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVariantAnimation>

#include <algorithm>
#include <memory>
#include <optional>

/**
 * @brief 覆盖主窗口外壳的三条会话入口规则：启动空态、"+"新建、"+"的位置。
 *
 * 这些行为全部由窗口构造和标签生命周期决定，不依赖任何服务器；用例使用内存数据库并开启
 * QStandardPaths 测试模式，因此既不读也不写用户真实的主机、凭据与日志。
 */
class MainWindowShellTest final : public QObject {
    Q_OBJECT

private slots:
    /** @brief 真实窗口应用主题时不得输出未知样式属性警告，且所有按钮仍保持手型。 */
    void themeStylesDoNotWarnAboutUnknownProperties();
    /** @brief 各语言的连接提示之后，下一段输出必须从行首开始。 */
    void translatedConnectionNoticeReturnsToFirstColumn();
    /** @brief 终端右键菜单的复制、粘贴、全选必须跟随界面语言，不能依赖未分发的 Qt 自带译文。 */
    void terminalContextMenuFollowsInterfaceLanguage();
    void initTestCase();
    void startsWithBrandPlaceholderInsteadOfTerminal();
    void newTabButtonOpensDefaultTerminal();
    void newTabButtonFollowsTheLastTab();
    void manualRecoveryCloseKeyClosesTheTab();
    void destroyingWindowWithOpenTabsIsSafe();
    void emptyStateKeepsNewTabButtonNextToTheTitleIdentity();
    /** @brief Windows/Linux 产品名、标签与自绘窗口按钮必须共用唯一的无边框标题行。 */
    void desktopPlatformsUseProductTitleAndCustomWindowControls();
    void sidebarWidthIsRestoredOnNextLaunch();
    /** @brief 侧边栏显隐使用连续尺寸动画，且收起后再次展开能恢复用户宽度。 */
    void sidebarToggleAnimatesAndRestoresWidth();
    /** @brief 文件管理从右侧连续滑入滑出，末段不受内部最小宽度约束，反向操作保留宽度。 */
    void remoteFilesSlideContinuously();
    /** @brief 关闭确认提示后，双击分隔条直接收起侧边栏而不弹出模态框。 */
    void sidebarDividerDoubleClickHidesWithoutPromptWhenDisabled();
    /** @brief 菜单切换即时缩小侧栏并持久化，重启恢复紧凑模式且可切回宽松。 */
    void sidebarDensityPersistsAndRestores();
    /** @brief 导航轨中工作空间入口紧跟主机入口，页面索引与点击路由保持不变。 */
    void workspacesNavigationButtonFollowsHosts();
    void duplicateMenuItemOpensASecondCopyOfTheTab();

    /** @brief 把终端拖入此前选中的标签后必须原位合并为可重命名的工作空间标签；
     *         拖出标签栏期间必须亮出跟随光标的浮动快照与中央落点提示层。 */
    void draggingTerminalIntoSelectedTabCreatesWorkspace();
    void droppingTerminalOnLowerHalfStacksWorkspaceVertically();

    /** @brief 分屏模式必须禁用远程文件管理，切换模式则恢复单终端完整能力。 */
    void workspaceLayoutControlsRemoteFileAvailability();

    /** @brief 把成员停靠到另一成员的某条边必须构建/解包递归 QSplitter 树。 */
    void dockingTerminalBuildsRecursiveSplitterTree();

    /** @brief 拖动组内某终端的会话状态栏必须亮出浮动快照与落点高亮，并在松手后重排分屏。 */
    void draggingTerminalHeaderReDocksWithinWorkspace();

    /** @brief 从标签栏拖入既有工作空间时，落点半区必须走与内部拖动相同的命中逻辑。 */
    void externalDropHintResolvesToDockTarget();

    /** @brief 工作空间成员标题栏必须有广播/布局切换/关闭三个按钮，广播开关整体同步。 */
    void workspaceMemberHeaderControls();
    void workspaceSidebarListsMembersNumbersDuplicatesAndCloses();

    /** @brief 终端上报的发行版必须落库并让侧栏立刻换成对应图标，重启后依然保持。 */
    void detectedDistributionIsPersistedAndShownInSidebar();

    /** @brief 切换界面语言后，常驻菜单栏必须就地重译，切回来也要恢复。 */
    void languageChangeRetranslatesMenuBar();

private:
    ApplicationDatabase *m_database = nullptr;
    CredentialVault *m_vault = nullptr;
};

/**
 * @brief 通过真实连接入口和已编译译文复现换行丢失回车的问题。
 * 无参数、无返回值；断言失败时报告语言和实际排版。只使用本机端口，且不等待认证。
 */
void MainWindowShellTest::translatedConnectionNoticeReturnsToFirstColumn()
{
    LocalizationService localization;
    // 优先覆盖本次截图对应的简体中文，再遍历其他语言，避免其他译文先失败而掩盖复现。
    QStringList languages = LocalizationService::supportedLanguages();
    languages.removeAll(QStringLiteral("zh_CN"));
    languages.prepend(QStringLiteral("zh_CN"));
    for (const QString &language : languages) {
        QVERIFY(localization.apply(language));
        TerminalPane pane;
        ConnectionTarget target;
        // 本用例只检查同步输出的连接提示，不处理事件队列中的网络结果；面板析构会停止线程。
        target.host = QStringLiteral("127.0.0.1");
        target.port = 1;
        target.username = QStringLiteral("test");
        pane.connectToHost(target);
        auto *view = pane.findChild<TerminalView *>();
        QVERIFY(view != nullptr);
        view->appendTerminalOutput(QByteArrayLiteral("NEXT"));
        // terminalPlainText 会同步刷新文档，无需等待远端响应或定时器，避免网络状态干扰断言。
        const QStringList lines = pane.terminalPlainText().split(QLatin1Char('\n'));
        QVERIFY2(lines.size() >= 2, qPrintable(language));
        QVERIFY2(lines.at(1).startsWith(QStringLiteral("NEXT")),
                 qPrintable(language + QStringLiteral(": ") + lines.at(1)));
    }
}

void MainWindowShellTest::terminalContextMenuFollowsInterfaceLanguage()
{
    // 菜单文字形如"复制\t⌘C"，只比较制表符前的标签。
    const auto menuLabels = []() {
        TerminalView view;
        std::unique_ptr<QMenu> menu(view.createTerminalContextMenu());
        QStringList labels;
        for (const QString &name : {QStringLiteral("edit-copy"), QStringLiteral("terminal-paste"),
                                    QStringLiteral("select-all")}) {
            const QAction *action = menu->findChild<QAction *>(name);
            labels.append(action == nullptr ? QString() : action->text().section(QLatin1Char('\t'), 0, 0));
        }
        return labels;
    };

    LocalizationService localization;
    QVERIFY(localization.apply(QStringLiteral("en")));
    const QStringList english = menuLabels();
    QCOMPARE(english, (QStringList{QStringLiteral("Copy"), QStringLiteral("Paste"),
                                   QStringLiteral("Select All")}));

    for (const QString &language : LocalizationService::supportedLanguages()) {
        if (language == LocalizationService::sourceLanguage()) {
            continue;
        }
        QVERIFY(localization.apply(language));
        const QStringList translated = menuLabels();
        for (qsizetype index = 0; index < english.size(); ++index) {
            QVERIFY2(!translated.at(index).isEmpty() && translated.at(index) != english.at(index),
                     qPrintable(language + QStringLiteral(": ") + translated.join(QLatin1Char('|'))));
        }
    }
    QVERIFY(localization.apply(QStringLiteral("en")));
}

void MainWindowShellTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    m_database = new ApplicationDatabase(ApplicationDatabase::inMemoryPath());
    QString error;
    QVERIFY2(m_database->initialize(&error), qPrintable(error));
    m_vault = new CredentialVault(*m_database);
}

/**
 * @brief 通过浅色和深色窗口构造、显示路径验证 QSS 与全局按钮策略共同生效。
 * 无参数、无返回值；未知属性警告或光标不符由 QtTest 报告为失败。
 * 使用独立内存数据库，避免主题配置影响其他用例，更不会写入用户真实设置。
 */
void MainWindowShellTest::themeStylesDoNotWarnAboutUnknownProperties()
{
    // 将真实样式引擎的警告升级为测试失败，防止只测光标却遗漏 QSS 控制台刷屏。
    QTest::failOnWarning(QRegularExpression(QStringLiteral("Unknown property .*")));
    ButtonCursorPolicy policy(*qApp);
    for (const QString &theme : {QStringLiteral("light"), QStringLiteral("dark")}) {
        ApplicationDatabase database(ApplicationDatabase::inMemoryPath());
        QString error;
        QVERIFY2(database.initialize(&error), qPrintable(error));
        QVERIFY2(database.setSetting(QStringLiteral("appearance/theme"), theme, &error),
                 qPrintable(error));
        CredentialVault vault(database);
        MainWindow window(&database, &vault);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto buttons = window.findChildren<QAbstractButton *>();
        QVERIFY(!buttons.isEmpty());
        for (QAbstractButton *button : buttons) {
            // 隐藏页面中的按钮也主动初始化，覆盖尚未被用户切换到的页面。
            button->ensurePolished();
            QCOMPARE(button->cursor().shape(), Qt::PointingHandCursor);
        }
    }
}

void MainWindowShellTest::startsWithBrandPlaceholderInsteadOfTerminal()
{
    MainWindow window(m_database, m_vault);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("TerminalPages"));
    auto *stack = window.findChild<QStackedWidget *>(QStringLiteral("WorkspaceStack"));
    auto *placeholder = window.findChild<QWidget *>(QStringLiteral("WorkspacePlaceholder"));
    QVERIFY(tabs != nullptr && stack != nullptr && placeholder != nullptr);

    QCOMPARE(tabs->count(), 0);
    QCOMPARE(stack->currentWidget(), placeholder);
    auto *title = window.findChild<QLabel *>(QStringLiteral("WorkspacePlaceholderTitle"));
    QVERIFY(title != nullptr);
    QCOMPARE(title->text(), QStringLiteral("Shellium"));
    auto *icon = window.findChild<QLabel *>(QStringLiteral("WorkspacePlaceholderIcon"));
    QVERIFY2(icon != nullptr && !icon->pixmap().isNull(),
             "空态页必须显示产品图标，而不是只有一行文字");
}

void MainWindowShellTest::newTabButtonOpensDefaultTerminal()
{
    MainWindow window(m_database, m_vault);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("TerminalPages"));
    auto *tabBar = window.findChild<QTabBar *>(QStringLiteral("SessionTitleTabs"));
    auto *newTabButton = window.findChild<QToolButton *>(QStringLiteral("TitleNewTabButton"));
    QVERIFY(tabs != nullptr && tabBar != nullptr && newTabButton != nullptr);

    QTest::mouseClick(newTabButton, Qt::LeftButton);

    QCOMPARE(tabs->count(), 1);
    QCOMPARE(tabBar->count(), 1);
    auto *stack = window.findChild<QStackedWidget *>(QStringLiteral("WorkspaceStack"));
    QCOMPARE(stack->currentWidget(), tabs);
    auto *workspace = qobject_cast<TerminalWorkspace *>(tabs->widget(0));
    QVERIFY(workspace != nullptr);
    // 新标签必须真的启动当前平台默认交互式 Shell，而不是留下一个空面板。
    QTRY_VERIFY_WITH_TIMEOUT(workspace->activePane()->isConnected(), 8000);
}

void MainWindowShellTest::newTabButtonFollowsTheLastTab()
{
    MainWindow window(m_database, m_vault);
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *tabBar = window.findChild<QTabBar *>(QStringLiteral("SessionTitleTabs"));
    auto *newTabButton = window.findChild<QToolButton *>(QStringLiteral("TitleNewTabButton"));
    QVERIFY(tabBar != nullptr && newTabButton != nullptr);
    QWidget *chrome = newTabButton->parentWidget();
    QVERIFY(chrome != nullptr);

    QTest::mouseClick(newTabButton, Qt::LeftButton);
    QTest::mouseClick(newTabButton, Qt::LeftButton);
    QCoreApplication::processEvents();

    // "+"必须紧跟最后一个标签，而不是被推到窗口最右端：两个标签只占标题行左侧一小段，
    // 此时按钮左边缘与最后一个标签右边缘之间只允许留下滚动箭头和间距的宽度。
    const int lastTabRight =
        tabBar->mapTo(chrome, tabBar->tabRect(tabBar->count() - 1).topRight()).x();
    const int buttonLeft = newTabButton->mapTo(chrome, QPoint(0, 0)).x();
    QVERIFY2(buttonLeft - lastTabRight < 80,
             qPrintable(QStringLiteral("新建按钮距最后一个标签 %1 像素，未跟随标签")
                            .arg(buttonLeft - lastTabRight)));
    QVERIFY2(buttonLeft > lastTabRight, "新建按钮必须位于最后一个标签右侧");

    // 标签多到溢出时，标签栏被压缩到可用宽度，"+"随之停在标题行末尾并保持可见——这正是
    // "固定在最右边"所要求的结果，无需为它单独写一条布局分支。
    for (int extra = 0; extra < 14; ++extra) {
        QTest::mouseClick(newTabButton, Qt::LeftButton);
    }
    QCoreApplication::processEvents();
    QVERIFY(newTabButton->isVisible());
    const int buttonRight = newTabButton->mapTo(chrome, QPoint(newTabButton->width(), 0)).x();
    QVERIFY2(buttonRight <= chrome->width(),
             "标签溢出时新建按钮不能被挤出标题行");
#ifdef Q_OS_MACOS
    QVERIFY2(chrome->width() - buttonRight < 80,
             "macOS 标签溢出时新建按钮必须固定在标题行最右端");
#else
    auto *minimizeButton = window.findChild<QToolButton *>(
        QStringLiteral("WindowMinimizeButton"));
    QVERIFY(minimizeButton != nullptr);
    const int controlsLeft = minimizeButton->mapTo(chrome, QPoint(0, 0)).x();
    QVERIFY2(controlsLeft - buttonRight < 80,
             "Windows/Linux 标签溢出时新建按钮必须紧靠右侧窗口按钮");
#endif
}

void MainWindowShellTest::draggingTerminalIntoSelectedTabCreatesWorkspace()
{
    MainWindow window(m_database, m_vault);
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("TerminalPages"));
    auto *tabBar = window.findChild<SessionTabBar *>(QStringLiteral("SessionTitleTabs"));
    auto *newTabButton = window.findChild<QToolButton *>(QStringLiteral("TitleNewTabButton"));
    QVERIFY(tabs != nullptr && tabBar != nullptr && newTabButton != nullptr);
    QTest::mouseClick(newTabButton, Qt::LeftButton);
    QTest::mouseClick(newTabButton, Qt::LeftButton);
    QCOMPARE(tabs->count(), 2);

    // 先选第一个标签作为接收目标，再把第二个标签拖入该页面内真正的 TerminalPane。
    // 测试故意不使用 QTabWidget 的泛化中心点，防止文件管理器、分隔条或其他中央空白
    // 被误判为终端会话落点；同时覆盖 QTabBar 的鼠标抓取与稳定标识反查。
    QTest::mouseClick(tabBar, Qt::LeftButton, Qt::NoModifier, tabBar->tabRect(0).center());
    QCOMPARE(tabs->currentIndex(), 0);
    auto *targetWorkspace = qobject_cast<TerminalWorkspace *>(tabs->widget(0));
    QVERIFY(targetWorkspace != nullptr && targetWorkspace->activePane() != nullptr);
    auto *overlay = window.findChild<SessionTabDropOverlay *>(
        QStringLiteral("SessionTabDropOverlay"));
    QVERIFY(overlay != nullptr);
    QVERIFY2(overlay->isHidden(), "空闲时中央落点提示层必须隐藏");
    const auto dragGhostExists = [] {
        const QList<QWidget *> topLevels = QApplication::topLevelWidgets();
        return std::any_of(topLevels.cbegin(), topLevels.cend(), [](const QWidget *widget) {
            return widget->objectName() == QStringLiteral("SessionTabDragGhost");
        });
    };
    QSignalSpy dragStartedSpy(tabBar, &SessionTabBar::externalDragStarted);

    const QPoint sourceCenter = tabBar->tabRect(1).center();
    const QPoint terminalDropPoint = tabBar->mapFromGlobal(
        targetWorkspace->activePane()->mapToGlobal(targetWorkspace->activePane()->rect().center()));
    QTest::mousePress(tabBar, Qt::LeftButton, Qt::NoModifier, sourceCenter);
    // 按下非当前标签不得立即换页；否则接收终端会从眼前消失，用户只能看到源终端。
    QCOMPARE(tabs->currentIndex(), 0);
    // 先稍微向下移动以跨过系统拖拽阈值，再进入终端面板中心，近似真实连续手势。
    QTest::mouseMove(tabBar, QPoint(sourceCenter.x(), tabBar->height() + 12), 40);
    QCOMPARE(tabs->currentIndex(), 0);
    QTest::mouseMove(tabBar, terminalDropPoint, 80);

    // 指针离开标签栏下沿即进入"拖出"手势：亮出跟随光标的浮动标签快照与中央落点提示层。
    QTRY_COMPARE(dragStartedSpy.count(), 1);
    QVERIFY2(dragGhostExists(), "拖出标签栏后必须存在跟随光标的浮动标签快照");
    QVERIFY2(!overlay->isHidden(), "拖出标签栏后中央落点提示层必须可见");

    QTest::mouseRelease(tabBar, Qt::LeftButton, Qt::NoModifier, terminalDropPoint);
    QTRY_VERIFY2(!dragGhostExists(), "释放后浮动标签快照必须销毁");
    QVERIFY2(overlay->isHidden(), "释放后中央落点提示层必须隐藏");

    QTRY_COMPARE(tabs->count(), 1);
    auto *group = qobject_cast<TerminalWorkspaceGroup *>(tabs->widget(0));
    QVERIFY2(group != nullptr, "合并后的顶层页面必须是多终端工作空间，而不是普通终端标签");
    QCOMPARE(group->terminals().size(), 2);
    QCOMPARE(tabBar->tabKind(0), SessionTabBar::TabKind::Workspace);
    QVERIFY(window.findChild<WorkspaceSidebarWidget *>(QStringLiteral("WorkspaceSidebar")) != nullptr);

    // 同一拖放语义也必须能通过“工作空间”侧栏向既有组追加终端，不能每两个
    // 终端又创建一个新组。先打开该侧栏，后续落点使用侧栏自身的真实全局几何。
    auto *navigationRail = window.findChild<QWidget *>(QStringLiteral("NavigationRail"));
    QVERIFY(navigationRail != nullptr);
    QToolButton *workspacesNavigationButton = nullptr;
    const QList<QToolButton *> navigationButtons =
        navigationRail->findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly);
    for (QToolButton *button : navigationButtons) {
        if (button->property("navButton").toBool()
            && button->accessibleName() == QStringLiteral("Workspaces")) {
            workspacesNavigationButton = button;
            break;
        }
    }
    QVERIFY(workspacesNavigationButton != nullptr);
    QTest::mouseClick(workspacesNavigationButton, Qt::LeftButton);
    auto *workspaceSidebar = window.findChild<WorkspaceSidebarWidget *>(
        QStringLiteral("WorkspaceSidebar"));
    QVERIFY(workspaceSidebar != nullptr && workspaceSidebar->isVisible());

    QTest::mouseClick(newTabButton, Qt::LeftButton);
    QCOMPARE(tabs->count(), 2);
    QTest::mouseClick(tabBar, Qt::LeftButton, Qt::NoModifier, tabBar->tabRect(0).center());
    QCOMPARE(tabs->currentWidget(), static_cast<QWidget *>(group));
    const QPoint appendedSourceCenter = tabBar->tabRect(1).center();
    const QPoint workspaceSidebarDropPoint = tabBar->mapFromGlobal(
        workspaceSidebar->mapToGlobal(workspaceSidebar->rect().center()));
    QTest::mousePress(tabBar, Qt::LeftButton, Qt::NoModifier, appendedSourceCenter);
    QCOMPARE(tabs->currentWidget(), static_cast<QWidget *>(group));
    QTest::mouseMove(tabBar,
                     QPoint(appendedSourceCenter.x(), tabBar->height() + 12),
                     40);
    QTest::mouseMove(tabBar, workspaceSidebarDropPoint, 80);
    QTest::mouseRelease(tabBar, Qt::LeftButton, Qt::NoModifier, workspaceSidebarDropPoint);
    QTRY_COMPARE(tabs->count(), 1);
    QCOMPARE(group->terminals().size(), 3);

    // 除了侧栏，既有工作空间中任一当前可见的终端页面也是合法接收区。再追加
    // 一个单终端并拖入活动成员的 TerminalPane，可防止未来只保留侧栏路径却破坏页面路径。
    QTest::mouseClick(newTabButton, Qt::LeftButton);
    QCOMPARE(tabs->count(), 2);
    QTest::mouseClick(tabBar, Qt::LeftButton, Qt::NoModifier, tabBar->tabRect(0).center());
    TerminalWorkspace *activeGroupMember = group->activeTerminal();
    QVERIFY(activeGroupMember != nullptr && activeGroupMember->activePane() != nullptr);
    const QPoint fourthSourceCenter = tabBar->tabRect(1).center();
    const QPoint groupTerminalDropPoint = tabBar->mapFromGlobal(
        activeGroupMember->activePane()->mapToGlobal(
            activeGroupMember->activePane()->rect().center()));
    QVERIFY2(activeGroupMember->activePane()->isVisible(),
             "作为接收目标的工作空间终端必须当前可见");
    const QPoint reconstructedPanePoint = activeGroupMember->activePane()->mapFromGlobal(
        tabBar->mapToGlobal(groupTerminalDropPoint));
    QVERIFY2(activeGroupMember->activePane()->rect().contains(reconstructedPanePoint),
             "用标签栏局部坐标重建全局落点后必须仍命中目标 TerminalPane");
    QSignalSpy workspaceMergeSpy(tabBar, &SessionTabBar::workspaceMergeRequested);
    QTest::mousePress(tabBar, Qt::LeftButton, Qt::NoModifier, fourthSourceCenter);
    QCOMPARE(tabs->currentWidget(), static_cast<QWidget *>(group));
    QTest::mouseMove(tabBar,
                     QPoint(fourthSourceCenter.x(), tabBar->height() + 12),
                     40);
    QTest::mouseMove(tabBar, groupTerminalDropPoint, 80);
    QTest::mouseRelease(tabBar, Qt::LeftButton, Qt::NoModifier, groupTerminalDropPoint);
    QCOMPARE(workspaceMergeSpy.count(), 1);
    QTRY_COMPARE(tabs->count(), 1);
    QCOMPARE(group->terminals().size(), 4);
}

void MainWindowShellTest::droppingTerminalOnLowerHalfStacksWorkspaceVertically()
{
    MainWindow window(m_database, m_vault);
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("TerminalPages"));
    auto *tabBar = window.findChild<SessionTabBar *>(QStringLiteral("SessionTitleTabs"));
    auto *newTabButton = window.findChild<QToolButton *>(QStringLiteral("TitleNewTabButton"));
    QVERIFY(tabs != nullptr && tabBar != nullptr && newTabButton != nullptr);
    QTest::mouseClick(newTabButton, Qt::LeftButton);
    QTest::mouseClick(newTabButton, Qt::LeftButton);
    QCOMPARE(tabs->count(), 2);

    QTest::mouseClick(tabBar, Qt::LeftButton, Qt::NoModifier, tabBar->tabRect(0).center());
    QCOMPARE(tabs->currentIndex(), 0);
    auto *targetWorkspace = qobject_cast<TerminalWorkspace *>(tabs->widget(0));
    QVERIFY(targetWorkspace != nullptr && targetWorkspace->activePane() != nullptr);
    auto *overlay = window.findChild<SessionTabDropOverlay *>(
        QStringLiteral("SessionTabDropOverlay"));
    QVERIFY(overlay != nullptr);

    // 落点选在目标终端面板的下四分之一处：既落在真正的 TerminalPane 内（合并判定通过），
    // 又稳定命中中央落点提示层的“下半区”，因此新会话应叠在原会话下方（垂直排列）。
    const QRect paneRect = targetWorkspace->activePane()->rect();
    const QPoint paneLowerPoint(paneRect.center().x(),
                                paneRect.top() + paneRect.height() * 7 / 8);
    const QPoint sourceCenter = tabBar->tabRect(1).center();
    const QPoint lowerDropPoint = tabBar->mapFromGlobal(
        targetWorkspace->activePane()->mapToGlobal(paneLowerPoint));

    QTest::mousePress(tabBar, Qt::LeftButton, Qt::NoModifier, sourceCenter);
    QTest::mouseMove(tabBar, QPoint(sourceCenter.x(), tabBar->height() + 12), 40);
    QTest::mouseMove(tabBar, lowerDropPoint, 80);
    QTRY_VERIFY2(!overlay->isHidden(), "拖出标签栏后中央落点提示层必须可见");

    QTest::mouseRelease(tabBar, Qt::LeftButton, Qt::NoModifier, lowerDropPoint);
    QTRY_COMPARE(tabs->count(), 1);
    auto *group = qobject_cast<TerminalWorkspaceGroup *>(tabs->widget(0));
    QVERIFY(group != nullptr);
    QCOMPARE(group->terminals().size(), 2);

    // terminals() 按加入顺序返回 [目标, 源]；下半区落点应让“源”整体位于“目标”下方，
    // 而不是默认的左右等分。
    TerminalWorkspace *upper = group->terminals().at(0);
    TerminalWorkspace *lower = group->terminals().at(1);
    QVERIFY(upper->activePane() != nullptr && lower->activePane() != nullptr);
    const QRect upperGlobal(upper->activePane()->mapToGlobal(QPoint(0, 0)),
                            upper->activePane()->size());
    const QRect lowerGlobal(lower->activePane()->mapToGlobal(QPoint(0, 0)),
                            lower->activePane()->size());
    QVERIFY2(lowerGlobal.top() >= upperGlobal.bottom() - 4,
             "下半区落点必须把新会话垂直叠在原会话下方");
    QVERIFY2(std::abs(lowerGlobal.left() - upperGlobal.left()) < 4
                 && std::abs(lowerGlobal.right() - upperGlobal.right()) < 4,
             "垂直排列时两个终端应等宽对齐，而不是左右分栏");
}

void MainWindowShellTest::workspaceLayoutControlsRemoteFileAvailability()
{
    TerminalWorkspaceGroup group;
    auto *first = new TerminalWorkspace;
    auto *second = new TerminalWorkspace;
    group.addTerminal(first, QStringLiteral("one"));
    group.addTerminal(second, QStringLiteral("two"));

    auto *firstFileToggle = first->activePane()->findChild<QToolButton *>(
        QStringLiteral("terminalFileManagerToggle"));
    QVERIFY(firstFileToggle != nullptr);
    // 文件夹与广播按钮位于同一会话标题栏；尺寸、悬停/选中背景和指针应使用同一套规则。
    auto *broadcastToggle = first->findChild<QToolButton *>(QStringLiteral("workspaceBroadcastToggle"));
    QVERIFY(broadcastToggle != nullptr);
    QCOMPARE(firstFileToggle->iconSize(), broadcastToggle->iconSize());
    QCOMPARE(firstFileToggle->styleSheet(), broadcastToggle->styleSheet());
    QCOMPARE(firstFileToggle->cursor().shape(), broadcastToggle->cursor().shape());
    QVERIFY2(first->activePane()->findChild<QLabel *>(QStringLiteral("terminalSessionMeta")) == nullptr,
             "终端会话状态栏不再显示 UTF-8·列×行 字样");

    // 默认分屏模式：多块同时显示，所有文件管理器必须先收起、入口禁用并隐藏。
    QCOMPARE(group.layoutMode(), TerminalWorkspaceGroup::LayoutMode::Tiled);
    QVERIFY(!first->isFileManagerAllowed());
    QVERIFY(!second->isFileManagerAllowed());
    QVERIFY2(firstFileToggle->isHidden(), "分屏模式下文件管理按钮必须隐藏而不是灰显");
    first->setFileManagerVisible(true);
    QVERIFY(!first->isFileManagerVisible());

    group.setLayoutMode(TerminalWorkspaceGroup::LayoutMode::Switch);
    QVERIFY(first->isFileManagerAllowed());
    QVERIFY(second->isFileManagerAllowed());
    QVERIFY2(!firstFileToggle->isHidden(), "切换模式下文件管理按钮必须恢复显示");
    first->setFileManagerVisible(true);
    QVERIFY(first->isFileManagerVisible());

    group.setLayoutMode(TerminalWorkspaceGroup::LayoutMode::Tiled);
    QVERIFY(!first->isFileManagerAllowed());
    QVERIFY(!first->isFileManagerVisible());

    const QList<QWidget *> frames = group.findChildren<QWidget *>(
        QStringLiteral("WorkspaceTerminalFrame"));
    QCOMPARE(frames.size(), 2);
    QCOMPARE(std::count_if(frames.cbegin(), frames.cend(), [](QWidget *frame) {
        return frame->property("workspaceSelected").toBool();
    }), 1);
    group.setActiveTerminal(second);
    QCOMPARE(std::count_if(frames.cbegin(), frames.cend(), [](QWidget *frame) {
        return frame->property("workspaceSelected").toBool();
    }), 1);
    QVERIFY(std::any_of(frames.cbegin(), frames.cend(), [second](QWidget *frame) {
        return frame->property("workspaceSelected").toBool()
            && frame->isAncestorOf(second);
    }));

    const auto firstFrameIterator = std::find_if(
        frames.cbegin(), frames.cend(), [first](QWidget *frame) { return frame->isAncestorOf(first); });
    QVERIFY(firstFrameIterator != frames.cend());
    QWidget *firstFrame = *firstFrameIterator;
    QEvent enterEvent(QEvent::Enter);
    QCoreApplication::sendEvent(first, &enterEvent);
    QVERIFY2(firstFrame->property("workspaceHovered").toBool(),
             "鼠标进入未选中终端后必须进入虚线 Hover 状态");
}

void MainWindowShellTest::dockingTerminalBuildsRecursiveSplitterTree()
{
    TerminalWorkspaceGroup group;
    auto *a = new TerminalWorkspace;
    auto *b = new TerminalWorkspace;
    auto *c = new TerminalWorkspace;
    group.addTerminal(a, QStringLiteral("a"));
    group.addTerminal(b, QStringLiteral("b"));
    group.addTerminal(c, QStringLiteral("c"));

    auto *root = group.findChild<QSplitter *>(QStringLiteral("WorkspaceSplitRoot"));
    QVERIFY(root != nullptr);
    QCOMPARE(root->orientation(), Qt::Horizontal);
    QCOMPARE(root->count(), 3);

    const auto frameFor = [&group](TerminalWorkspace *terminal) -> QWidget * {
        const QList<QWidget *> frames = group.findChildren<QWidget *>(
            QStringLiteral("WorkspaceTerminalFrame"));
        const auto it = std::find_if(frames.cbegin(), frames.cend(), [terminal](QWidget *f) {
            return f->isAncestorOf(terminal) || f == terminal->parentWidget();
        });
        return it == frames.cend() ? nullptr : *it;
    };

    // 把 c 停靠到 a 的下方：a、c 进入一个新的垂直 QSplitter，a 在前。
    group.dockTerminal(c, a, TerminalWorkspaceGroup::DockEdge::Bottom);
    QWidget *frameA = frameFor(a);
    QWidget *frameC = frameFor(c);
    QVERIFY(frameA != nullptr && frameC != nullptr);
    auto *nested = qobject_cast<QSplitter *>(frameA->parentWidget());
    QVERIFY2(nested != nullptr && nested != root, "a 必须落进一个新的内层 QSplitter");
    QCOMPARE(nested->orientation(), Qt::Vertical);
    QCOMPARE(frameC->parentWidget(), nested);
    QCOMPARE(nested->indexOf(frameA), 0);
    QCOMPARE(nested->indexOf(frameC), 1);
    QCOMPARE(nested->objectName(), QStringLiteral("WorkspaceSplitNode"));
    QCOMPARE(root->orientation(), Qt::Horizontal);
    // 只数工作空间自己的分屏节点（终端面板内部各自也有一个无名 QSplitter）。
    QCOMPARE(group.findChildren<QSplitter *>(QStringLiteral("WorkspaceSplitNode")).size(), 1);

    // 抽走 c：只剩 a 的内层 splitter 必须被解包，a 直接回到根。
    QVERIFY(group.takeTerminal(c));
    QTRY_COMPARE(group.findChildren<QSplitter *>(QStringLiteral("WorkspaceSplitNode")).size(), 0);
    QCOMPARE(frameFor(a)->parentWidget(), root);
    QCOMPARE(root->count(), 2);

    delete c;
}

void MainWindowShellTest::draggingTerminalHeaderReDocksWithinWorkspace()
{
    TerminalWorkspaceGroup group;
    group.resize(1000, 700);
    group.show();
    QVERIFY(QTest::qWaitForWindowExposed(&group));
    auto *a = new TerminalWorkspace;
    auto *b = new TerminalWorkspace;
    group.addTerminal(a, QStringLiteral("a"));
    group.addTerminal(b, QStringLiteral("b"));
    QTest::qWait(30);

    auto *overlay = group.findChild<QWidget *>(QStringLiteral("WorkspaceDockOverlay"));
    QVERIFY(overlay != nullptr && overlay->isHidden());
    const auto dockGhostExists = [] {
        const QList<QWidget *> topLevels = QApplication::topLevelWidgets();
        return std::any_of(topLevels.cbegin(), topLevels.cend(), [](const QWidget *w) {
            return w->objectName() == QStringLiteral("WorkspaceDockDragGhost");
        });
    };

    auto *headerA = a->findChild<QWidget *>(QStringLiteral("terminalSessionStatusBar"));
    QVERIFY(headerA != nullptr);
    const QPoint pressGlobal = headerA->mapToGlobal(headerA->rect().center());
    const QPoint overBGlobal = b->mapToGlobal(b->rect().center());

    QCursor::setPos(pressGlobal);
    QTest::mousePress(headerA, Qt::LeftButton, Qt::NoModifier, headerA->rect().center());
    // 分几步移动跨过拖拽阈值并进入 b：位置以 headerA 为参照，允许越界为负/超出。
    QTest::mouseMove(headerA, headerA->rect().center() + QPoint(0, 40));
    QTest::mouseMove(headerA, headerA->mapFromGlobal(overBGlobal));

    QTRY_VERIFY2(dockGhostExists(), "拖动会话状态栏必须出现跟随光标的浮动快照");
    QTRY_VERIFY2(!overlay->isHidden(), "拖动会话状态栏必须显示落点高亮层");

    QTest::mouseRelease(headerA, Qt::LeftButton, Qt::NoModifier,
                        headerA->mapFromGlobal(overBGlobal));
    QTRY_VERIFY2(!dockGhostExists(), "松手后浮动快照必须销毁");
    QVERIFY2(overlay->isHidden(), "松手后落点高亮层必须隐藏");
}

void MainWindowShellTest::externalDropHintResolvesToDockTarget()
{
    TerminalWorkspaceGroup group;
    group.resize(1000, 700);
    group.show();
    QVERIFY(QTest::qWaitForWindowExposed(&group));
    auto *a = new TerminalWorkspace;
    auto *b = new TerminalWorkspace;
    group.addTerminal(a, QStringLiteral("a"));
    group.addTerminal(b, QStringLiteral("b"));
    QTest::qWait(30);

    auto *overlay = group.findChild<QWidget *>(QStringLiteral("WorkspaceDockOverlay"));
    QVERIFY(overlay != nullptr && overlay->isHidden());

    group.beginExternalDropHint();
    QVERIFY2(!overlay->isHidden(), "外部拖入预览必须亮出落点高亮层");

    // 指向 a 的右半区（远离中心，确保是水平方向）。
    const QPoint rightOfA =
        a->mapToGlobal(QPoint(a->width() * 9 / 10, a->height() / 2));
    group.updateExternalDropHint(rightOfA);
    group.endExternalDropHint();
    QVERIFY2(overlay->isHidden(), "松手后落点高亮层必须隐藏");

    TerminalWorkspace *target = nullptr;
    TerminalWorkspaceGroup::DockEdge edge{};
    QVERIFY(group.consumeExternalDropTarget(&target, &edge));
    QCOMPARE(target, a);
    QCOMPARE(edge, TerminalWorkspaceGroup::DockEdge::Right);
    // 命中结果只能取一次。
    QVERIFY(!group.consumeExternalDropTarget(&target, &edge));
}

void MainWindowShellTest::workspaceMemberHeaderControls()
{
    TerminalWorkspaceGroup group;
    auto *a = new TerminalWorkspace;
    auto *b = new TerminalWorkspace;
    group.addTerminal(a, QStringLiteral("a"));
    group.addTerminal(b, QStringLiteral("b"));

    const auto controlsOf = [](TerminalWorkspace *w) {
        return std::make_tuple(
            w->activePane()->findChild<QToolButton *>(QStringLiteral("workspaceBroadcastToggle")),
            w->activePane()->findChild<QToolButton *>(QStringLiteral("workspaceModeToggle")),
            w->activePane()->findChild<QToolButton *>(QStringLiteral("workspaceMemberClose")));
    };
    auto [bcA, modeA, closeA] = controlsOf(a);
    auto [bcB, modeB, closeB] = controlsOf(b);
    QVERIFY(bcA && modeA && closeA && bcB && modeB && closeB);
    QVERIFY(bcA->isCheckable() && !modeA->isCheckable() && !closeA->isCheckable());

    // 广播开关整体同步：在 a 上打开，b 的按钮也应变为选中，组状态为 true。
    QVERIFY(!group.isBroadcastEnabled());
    bcA->setChecked(true);
    QVERIFY(group.isBroadcastEnabled());
    QVERIFY(bcB->isChecked());
    bcB->setChecked(false);
    QVERIFY(!group.isBroadcastEnabled());
    QVERIFY(!bcA->isChecked());

    // 布局切换按钮等价于侧栏开关。
    QCOMPARE(group.layoutMode(), TerminalWorkspaceGroup::LayoutMode::Tiled);
    modeA->click();
    QCOMPARE(group.layoutMode(), TerminalWorkspaceGroup::LayoutMode::Switch);

    // 关闭按钮走 TerminalWorkspace::closeRequested。
    QSignalSpy closeSpy(b, &TerminalWorkspace::closeRequested);
    closeB->click();
    QCOMPARE(closeSpy.count(), 1);

    // 分屏（组成员）模式必须放宽面板最小宽度，否则多块并列时递归分屏树的最小总宽超过
    // 工作空间可用宽度，QSplitter 只能向右溢出，把最右那块连同右上角按钮挤出可视区。
    modeA->click(); // 回到分屏
    QCOMPARE(group.layoutMode(), TerminalWorkspaceGroup::LayoutMode::Tiled);
    QVERIFY2(a->activePane()->minimumWidth() <= 200,
             "分屏组成员的最小宽度必须放宽，避免多块并列时溢出裁切");
    group.setLayoutMode(TerminalWorkspaceGroup::LayoutMode::Switch);
    QVERIFY2(a->activePane()->minimumWidth() >= 360,
             "切换模式单块显示，最小宽度恢复到 360");

    // 从组里摘除后，标题栏按钮与广播接线都要清掉（终端会作为独立标签继续存在）。
    QVERIFY(group.takeTerminal(a));
    QTRY_VERIFY(a->activePane()->findChild<QToolButton *>(
                    QStringLiteral("workspaceBroadcastToggle")) == nullptr);
    delete a;
    delete b;
}

void MainWindowShellTest::workspaceSidebarListsMembersNumbersDuplicatesAndCloses()
{
    TerminalWorkspaceGroup group;
    auto *a = new TerminalWorkspace;
    auto *b = new TerminalWorkspace;
    auto *c = new TerminalWorkspace;
    group.addTerminal(a, QStringLiteral("host-a"));
    group.addTerminal(b, QStringLiteral("8.148.83.73"));
    group.addTerminal(c, QStringLiteral("8.148.83.73"));

    WorkspaceSidebarWidget sidebar;
    sidebar.setWorkspace(&group);

    auto *list = sidebar.findChild<QListWidget *>(QStringLiteral("WorkspaceTerminalThumbnails"));
    QVERIFY(list != nullptr);
    QCOMPARE(list->count(), 3);

    // 同一主机开了多个终端时，所有同名行都带序号；唯一名称保持原样（与 Termius 一致）。
    QCOMPARE(list->item(0)->text(), QStringLiteral("host-a"));
    QCOMPARE(list->item(1)->text(), QStringLiteral("8.148.83.73 (1)"));
    QCOMPARE(list->item(2)->text(), QStringLiteral("8.148.83.73 (2)"));

    auto *title = sidebar.findChild<QLabel *>(QStringLiteral("WorkspaceSidebarTitle"));
    QVERIFY(title != nullptr);
    QVERIFY2(title->text().contains(QStringLiteral("3")), "标题需要显示成员数量");

    // setMemberRows 下发的副标题与连接状态写入自定义数据角色，供委托绘制。
    QList<WorkspaceSidebarRow> rows;
    for (int index = 0; index < 3; ++index) {
        WorkspaceSidebarRow row;
        row.title = group.terminalTitle(group.terminals().at(index));
        row.subtitle = QStringLiteral("ssh, root");
        row.connectionState = 2;
        rows.append(row);
    }
    sidebar.setMemberRows(rows);
    QCOMPARE(list->item(1)->data(Qt::UserRole + 1).toString(), QStringLiteral("ssh, root"));
    QCOMPARE(list->item(1)->data(Qt::UserRole + 3).toInt(), 2);

    // 点选某行 → 组的活动终端跟随。
    list->setCurrentRow(2);
    QCOMPARE(group.activeTerminal(), c);

    // 关闭按钮：委托在关闭命中矩形上收到释放事件时，通过侧栏发出 closeTerminalRequested。
    QSignalSpy closeSpy(&sidebar, &WorkspaceSidebarWidget::closeTerminalRequested);
    QAbstractItemDelegate *delegate = list->itemDelegate();
    QVERIFY(delegate != nullptr);
    QStyleOptionViewItem option;
    option.rect = QRect(0, 54, 320, 54);
    const QModelIndex rowIndex = list->model()->index(1, 0);
    const QPointF hit(option.rect.left() + 12, option.rect.center().y());
    QMouseEvent press(QEvent::MouseButtonPress, hit, hit,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, hit, hit,
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    delegate->editorEvent(&press, list->model(), option, rowIndex);
    delegate->editorEvent(&release, list->model(), option, rowIndex);
    QCOMPARE(closeSpy.count(), 1);
    QCOMPARE(closeSpy.first().first().value<TerminalWorkspace *>(), b);

    delete a;
    delete b;
    delete c;
}

void MainWindowShellTest::desktopPlatformsUseProductTitleAndCustomWindowControls()
{
#ifdef Q_OS_MACOS
    // macOS 菜单由系统屏幕菜单栏承载，窗口控制继续使用原生交通灯；本用例只约束需要自绘
    // 标题栏的桌面平台，避免把 Cocoa 的正确原生行为误判为缺少窗口内控件。
    QVERIFY(true);
#else
    MainWindow window(m_database, m_vault);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    QWidget *chrome = window.findChild<QWidget *>(QStringLiteral("WindowChrome"));
    auto *menuModel = window.findChild<QMenuBar *>(QStringLiteral("ApplicationMenuModel"));
    auto *productName = window.findChild<QLabel *>(QStringLiteral("WindowProductName"));
    auto *tabs = window.findChild<QTabBar *>(QStringLiteral("SessionTitleTabs"));
    auto *minimizeButton = window.findChild<QToolButton *>(
        QStringLiteral("WindowMinimizeButton"));
    auto *maximizeButton = window.findChild<QToolButton *>(
        QStringLiteral("WindowMaximizeButton"));
    auto *closeButton = window.findChild<QToolButton *>(QStringLiteral("WindowCloseButton"));

    QVERIFY(chrome != nullptr && menuModel != nullptr && productName != nullptr && tabs != nullptr);
    QVERIFY(minimizeButton != nullptr && maximizeButton != nullptr && closeButton != nullptr);
    QVERIFY2(window.windowFlags().testFlag(Qt::FramelessWindowHint),
             "Windows/Linux 必须移除独立系统标题栏，避免标签栏下方再出现第二层标题");
    QVERIFY2(window.findChild<QMenuBar *>(QStringLiteral("WindowMenuBar")) == nullptr,
             "Windows/Linux 标题栏不能继续显示传统平铺菜单");
    QCOMPARE(productName->parentWidget(), chrome);
    QCOMPARE(productName->text(), QStringLiteral("Shellium"));
    QVERIFY(productName->isVisible());
    QCOMPARE(tabs->parentWidget(), chrome);
    QCOMPARE(minimizeButton->parentWidget(), chrome);
    QCOMPARE(maximizeButton->parentWidget(), chrome);
    QCOMPARE(closeButton->parentWidget(), chrome);
    QVERIFY2(!menuModel->isVisible(), "菜单模型只保留动作关系，不能占据标题栏空间");
    QCOMPARE(menuModel->actions().size(), 5);
#endif
}

void MainWindowShellTest::manualRecoveryCloseKeyClosesTheTab()
{
    MainWindow window(m_database, m_vault);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("TerminalPages"));
    auto *newTabButton = window.findChild<QToolButton *>(QStringLiteral("TitleNewTabButton"));
    QVERIFY(tabs != nullptr && newTabButton != nullptr);
    QTest::mouseClick(newTabButton, Qt::LeftButton);
    QCOMPARE(tabs->count(), 1);

    auto *workspace = qobject_cast<TerminalWorkspace *>(tabs->widget(0));
    QVERIFY(workspace != nullptr);
    TerminalPane *pane = workspace->activePane();
    ConnectionTarget target;
    target.host = QStringLiteral("198.51.100.7");
    target.port = 22;
    target.username = QStringLiteral("root");
    pane->beginManualRecovery(target, QStringLiteral("自动重连已达到 3 次上限"));

    auto *view = pane->findChild<TerminalView *>();
    QVERIFY(view != nullptr);
    QTest::keyClick(view, Qt::Key_X);

    // 关闭最后一个标签不再被拒绝：窗口回到品牌空态页，这与启动时的状态完全一致。
    QTRY_COMPARE(tabs->count(), 0);
    auto *stack = window.findChild<QStackedWidget *>(QStringLiteral("WorkspaceStack"));
    auto *placeholder = window.findChild<QWidget *>(QStringLiteral("WorkspacePlaceholder"));
    QCOMPARE(stack->currentWidget(), placeholder);
}

void MainWindowShellTest::destroyingWindowWithOpenTabsIsSafe()
{
    auto *window = new MainWindow(m_database, m_vault);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto *newTabButton = window->findChild<QToolButton *>(QStringLiteral("TitleNewTabButton"));
    QVERIFY(newTabButton != nullptr);
    QTest::mouseClick(newTabButton, Qt::LeftButton);
    auto *tabs = window->findChild<QTabWidget *>(QStringLiteral("TerminalPages"));
    QCOMPARE(tabs->count(), 1);

    // 关闭窗口时终端面板作为子控件被删除，它们发出的 destroyed 会回调主窗口的槽——而此时
    // 主窗口自己的连接历史哈希已经析构。用例本身不做断言：一旦回调重新访问已释放的成员，
    // 进程会在这里直接中止。
    delete window;
    QVERIFY(true);
}

void MainWindowShellTest::emptyStateKeepsNewTabButtonNextToTheTitleIdentity()
{
    MainWindow window(m_database, m_vault);
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *newTabButton = window.findChild<QToolButton *>(QStringLiteral("TitleNewTabButton"));
    QVERIFY(newTabButton != nullptr);
    QWidget *chrome = newTabButton->parentWidget();
    QToolButton *sidebarButton = nullptr;
    for (QToolButton *candidate : chrome->findChildren<QToolButton *>()) {
        if (candidate->property("chromeRole").toString() == QStringLiteral("sidebar")) {
            sidebarButton = candidate;
        }
    }
    QVERIFY(sidebarButton != nullptr);

    const int sidebarRight = sidebarButton->geometry().right();
    const int buttonLeft = newTabButton->geometry().left();
#ifdef Q_OS_MACOS
    // macOS 没有窗口内产品名，空态标签入口仍应紧跟交通灯留白后的侧栏按钮。
    QVERIFY2(buttonLeft - sidebarRight < 24,
             qPrintable(QStringLiteral("空态下新建按钮与侧栏开关之间空出 %1 像素")
                            .arg(buttonLeft - sidebarRight)));
#else
    // Windows/Linux 按参考样式依次显示“侧栏按钮 → Shellium → 新建标签”。分别约束两段
    // 间距，既允许产品名占据合理宽度，也防止任一布局项被伸缩项推离形成大片空白。
    auto *productName = window.findChild<QLabel *>(QStringLiteral("WindowProductName"));
    QVERIFY(productName != nullptr);
    const int productLeft = productName->geometry().left();
    const int productRight = productName->geometry().right();
    QVERIFY2(productLeft - sidebarRight < 24,
             "产品名称必须紧跟在侧栏按钮右侧");
    QVERIFY2(buttonLeft - productRight < 24,
             "空态下新建按钮必须紧跟在 Shellium 产品名称右侧");
#endif
}

void MainWindowShellTest::sidebarDensityPersistsAndRestores()
{
    QString error;
    QVERIFY(m_database->setSetting(QStringLiteral("layout/sidebar_compact"),
                                   QStringLiteral("relaxed"), &error));
    QVERIFY(m_database->setIntSetting(QStringLiteral("layout/sidebar_width"), 360, &error));
    QVERIFY(m_database->setIntSetting(QStringLiteral("layout/sidebar_width_compact"), 260, &error));
    {
        MainWindow window(m_database, m_vault);
        window.resize(1400, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *compact = window.findChild<QAction *>(QStringLiteral("CompactSidebarAction"));
        auto *relaxed = window.findChild<QAction *>(QStringLiteral("RelaxedSidebarAction"));
        auto *minimal = window.findChild<QAction *>(QStringLiteral("MinimalSidebarAction"));
        auto *panel = window.findChild<QWidget *>(QStringLiteral("SidebarPanel"));
        auto *rail = window.findChild<QWidget *>(QStringLiteral("NavigationRail"));
        auto *tree = window.findChild<QTreeWidget *>(QStringLiteral("HostTree"));
        QVERIFY(compact && relaxed && minimal && panel && rail && tree);
        QVERIFY(relaxed->isChecked());
        // 让设置页保持打开，检验菜单与设置使用同一个服务，两个方向都应即时同步。
        auto *service = window.findChild<TerminalSettingsService *>();
        QVERIFY(service != nullptr);
        SettingsDialog settingsDialog(service);
        auto *selector = settingsDialog.findChild<SegmentedControl *>(QStringLiteral("sidebarDensitySelector"));
        QVERIFY(selector != nullptr);
        const int relaxedWidth = panel->width();
        compact->trigger();
        QCoreApplication::processEvents();
        QVERIFY(compact->isChecked());
        QCOMPARE(selector->currentData(), 1);
        QVERIFY(!relaxed->isChecked());
        QVERIFY(panel->width() < relaxedWidth);
        QCOMPARE(rail->width(), 56);
        QCOMPARE(tree->indentation(), 24);

        // 精简是第三档：导航栏尺寸和紧凑一样，但主机树缩进要比紧凑更紧。
        minimal->trigger();
        QCoreApplication::processEvents();
        QVERIFY(minimal->isChecked());
        QCOMPARE(selector->currentData(), 2);
        QCOMPARE(rail->width(), 56);
        QVERIFY(tree->indentation() < 24);

        // 来回切换应恢复用户此前实际宽度，不能把像素值当成比例反复缩放。
        selector->setCurrentData(0);
        QCoreApplication::processEvents();
        QVERIFY(relaxed->isChecked());
        QCOMPARE(panel->width(), relaxedWidth);
        compact->trigger();
        QCOMPARE(m_database->setting(QStringLiteral("layout/sidebar_compact")).value_or(QString()),
                 QStringLiteral("compact"));
    }
    // 复用测试数据库模拟重启，检验真实设置读取，而不是只检查内存布尔变量。
    MainWindow restarted(m_database, m_vault);
    auto *compact = restarted.findChild<QAction *>(QStringLiteral("CompactSidebarAction"));
    auto *relaxed = restarted.findChild<QAction *>(QStringLiteral("RelaxedSidebarAction"));
    auto *tree = restarted.findChild<QTreeWidget *>(QStringLiteral("HostTree"));
    QVERIFY(compact && relaxed && tree);
    QVERIFY(compact->isChecked());
    QCOMPARE(tree->indentation(), 24);
    relaxed->trigger();
    QCOMPARE(tree->indentation(), 32);
    QCOMPARE(m_database->setting(QStringLiteral("layout/sidebar_compact")).value_or(QString()),
             QStringLiteral("relaxed"));
}

void MainWindowShellTest::workspacesNavigationButtonFollowsHosts()
{
    MainWindow window(m_database, m_vault);
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *rail = window.findChild<QWidget *>(QStringLiteral("NavigationRail"));
    QVERIFY(rail != nullptr);
    QToolButton *hostsButton = nullptr;
    QToolButton *workspacesButton = nullptr;
    QToolButton *productivityButton = nullptr;
    for (QToolButton *button : rail->findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (!button->property("navButton").toBool()) {
            continue;
        }
        if (button->accessibleName() == QStringLiteral("Hosts")) {
            hostsButton = button;
        } else if (button->accessibleName() == QStringLiteral("Workspaces")) {
            workspacesButton = button;
        } else if (button->accessibleName() == QStringLiteral("Productivity")) {
            productivityButton = button;
        }
    }
    QVERIFY(hostsButton != nullptr && workspacesButton != nullptr && productivityButton != nullptr);
    QVERIFY2(workspacesButton->geometry().top() > hostsButton->geometry().top(),
             "工作空间入口必须位于主机入口下方");
    QVERIFY2(workspacesButton->geometry().bottom() < productivityButton->geometry().top(),
             "工作空间入口必须位于效率工具入口之前");

    // 视觉顺序变化不应改变稳定页面索引；点击后仍打开工作空间页并高亮对应按钮。
    QTest::mouseClick(workspacesButton, Qt::LeftButton);
    QVERIFY(workspacesButton->isChecked());
    QVERIFY(!hostsButton->isChecked());
}

void MainWindowShellTest::sidebarWidthIsRestoredOnNextLaunch()
{
    int draggedWidth = 0;
    {
        MainWindow window(m_database, m_vault);
        window.resize(1400, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto *splitter = window.findChild<QSplitter *>(QStringLiteral("SidebarSplitter"));
        QVERIFY(splitter != nullptr);
        QSplitterHandle *handle = splitter->handle(1);
        QVERIFY(handle != nullptr);

        // 通过真实拖动把手改变宽度，而不是直接 setSizes：只有拖动才会发出 splitterMoved，
        // 用例必须覆盖"用户拖动 → 落库"这条完整路径，否则漏掉信号连接照样能通过。
        const QPoint grabPoint = handle->rect().center();
        QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, grabPoint);
        QTest::mouseMove(handle, grabPoint + QPoint(90, 0));
        QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, grabPoint + QPoint(90, 0));

        draggedWidth = splitter->sizes().constFirst();
        QVERIFY2(draggedWidth > 320,
                 qPrintable(QStringLiteral("拖动后侧栏宽度仍是 %1，用例没有真正改变宽度")
                                .arg(draggedWidth)));

        // 析构会补写尚未超时的防抖定时器，这里同时覆盖"拖完立刻关窗"的路径。
    }

    QString error;
    const std::optional<int> stored =
        m_database->intSetting(QStringLiteral("layout/sidebar_width"), &error);
    QVERIFY2(stored.has_value(), qPrintable(error));
    QCOMPARE(*stored, draggedWidth);

    MainWindow restarted(m_database, m_vault);
    restarted.resize(1400, 800);
    restarted.show();
    QVERIFY(QTest::qWaitForWindowExposed(&restarted));
    auto *restoredSplitter = restarted.findChild<QSplitter *>(QStringLiteral("SidebarSplitter"));
    QVERIFY(restoredSplitter != nullptr);
    QCOMPARE(restoredSplitter->sizes().constFirst(), draggedWidth);
}

void MainWindowShellTest::remoteFilesSlideContinuously()
{
    TerminalWorkspace workspace;
    workspace.resize(1200, 700);
    workspace.show();
    QVERIFY(QTest::qWaitForWindowExposed(&workspace));
    workspace.setFileManagerVisible(true);
    auto *viewport = workspace.findChild<QWidget *>(QStringLiteral("RemoteFileViewport"));
    auto *animation = workspace.findChild<QVariantAnimation *>(QStringLiteral("RemoteFileSlideAnimation"));
    QVERIFY(viewport && animation);
    // 主动定位动画时刻，避免依赖机器帧率；开头只应露出窄条而不是直接跳到最小宽度。
    animation->setCurrentTime(55);
    QVERIFY(viewport->width() > 0 && viewport->width() < 100);
    animation->setCurrentTime(220);
    const int expanded = viewport->width();
    workspace.setFileManagerVisible(false);
    const auto animations = workspace.findChildren<QVariantAnimation *>(QStringLiteral("RemoteFileSlideAnimation"));
    animation = animations.last();
    animation->setCurrentTime(165);
    QVERIFY(viewport->width() > 0 && viewport->width() < 100);
    const int intermediate = viewport->width();
    workspace.setFileManagerVisible(true);
    QCOMPARE(viewport->width(), intermediate);
    QTRY_COMPARE(viewport->width(), expanded);
    workspace.setFileManagerVisible(false);
    QTRY_VERIFY(viewport->isHidden());
    QVERIFY(!workspace.isFileManagerVisible());
}

void MainWindowShellTest::sidebarToggleAnimatesAndRestoresWidth()
{
    MainWindow window(m_database, m_vault);
    window.resize(1400, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *panel = window.findChild<QWidget *>(QStringLiteral("SidebarPanel"));
    auto *splitter = window.findChild<QSplitter *>(QStringLiteral("SidebarSplitter"));
    QVERIFY(panel != nullptr && splitter != nullptr);
    QToolButton *sidebarButton = nullptr;
    for (QToolButton *button : window.findChildren<QToolButton *>()) {
        if (button->property("chromeRole").toString() == QStringLiteral("sidebar")) {
            sidebarButton = button;
            break;
        }
    }
    QVERIFY(sidebarButton != nullptr);

    const int expandedWidth = panel->width();
    QTest::mouseClick(sidebarButton, Qt::LeftButton);
    // 动画不是阻塞式 setVisible：事件循环返回时侧栏仍应有一段过渡宽度，而不是瞬间跳到目标。
    QVERIFY(panel->isVisible());
    QVERIFY(panel->width() > 0);
    // 固定动画时刻检查末段：剩余约 6% 时仍必须逐像素收起，不能卡在内容最小宽度。
    auto *animation = window.findChild<QVariantAnimation *>();
    QVERIFY(animation != nullptr);
    animation->setCurrentTime(animation->duration() * 3 / 4);
    QVERIFY2(splitter->sizes().first() < expandedWidth / 4,
             "收起末段必须小于原宽度的四分之一，不能到一半后突然隐藏");
    auto *content = panel->findChild<QWidget *>(QStringLiteral("SidebarSlidingContent"));
    QVERIFY(content != nullptr);
    QCOMPARE(content->width(), expandedWidth);
    QCOMPARE(content->x(), panel->width() - expandedWidth);
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isVisible(), 1000);

    QTest::mouseClick(sidebarButton, Qt::LeftButton);
    // 展开的前四分之一也必须从边缘连续滑入，内容宽度和方向与收起完全对称。
    animation = window.findChild<QVariantAnimation *>();
    QVERIFY(animation != nullptr);
    animation->setCurrentTime(animation->duration() / 4);
    QVERIFY(splitter->sizes().first() > 0);
    QVERIFY(splitter->sizes().first() < expandedWidth / 4);
    QCOMPARE(content->width(), expandedWidth);
    QCOMPARE(content->x(), panel->width() - expandedWidth);
    // 中途反向不应跳回起点，也不能把过渡宽度当成用户的展开宽度。
    const int intermediateWidth = panel->width();
    QTest::mouseClick(sidebarButton, Qt::LeftButton);
    QCOMPARE(panel->width(), intermediateWidth);
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isVisible(), 1000);
    QTest::mouseClick(sidebarButton, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(panel->isVisible(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(panel->width() >= expandedWidth - 2, 1000);
}

void MainWindowShellTest::sidebarDividerDoubleClickHidesWithoutPromptWhenDisabled()
{
    MainWindow window(m_database, m_vault);
    window.resize(1400, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *service = window.findChild<TerminalSettingsService *>();
    auto *splitter = window.findChild<QSplitter *>(QStringLiteral("SidebarSplitter"));
    auto *panel = window.findChild<QWidget *>(QStringLiteral("SidebarPanel"));
    QVERIFY(service != nullptr && splitter != nullptr && panel != nullptr);
    TerminalSettings settings = service->currentSettings();
    settings.sidebarDoubleClickPrompt = false;
    QString error;
    QVERIFY2(service->save(settings, &error), qPrintable(error));

    QSplitterHandle *handle = splitter->handle(1);
    QVERIFY(handle != nullptr);
    QTest::mouseDClick(handle, Qt::LeftButton, Qt::NoModifier, handle->rect().center());
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isVisible(), 1000);
    QVERIFY(!service->currentSettings().sidebarDoubleClickPrompt);
}

void MainWindowShellTest::duplicateMenuItemOpensASecondCopyOfTheTab()
{
    MainWindow window(m_database, m_vault);
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("TerminalPages"));
    auto *tabBar = window.findChild<QTabBar *>(QStringLiteral("SessionTitleTabs"));
    auto *newTabButton = window.findChild<QToolButton *>(QStringLiteral("TitleNewTabButton"));
    QVERIFY(tabs != nullptr && tabBar != nullptr && newTabButton != nullptr);
    QTest::mouseClick(newTabButton, Qt::LeftButton);
    auto *source = qobject_cast<TerminalWorkspace *>(tabs->widget(0));
    QVERIFY(source != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(source->activePane()->isConnected(), 8000);
    const QString sourceTitle = tabs->tabText(0);

    // 空白处右键只应给出"新建"，不能提供无从下手的"复制标签"。
    std::unique_ptr<QMenu> blankMenu(window.createSessionTabMenu(-1, &window));
    for (const QAction *action : blankMenu->actions()) {
        QVERIFY2(action->text() != QStringLiteral("Copy tag"),
                 "标题栏空白处的菜单不应包含复制标签");
    }

    std::unique_ptr<QMenu> menu(window.createSessionTabMenu(0, &window));
    QAction *duplicateAction = nullptr;
    for (QAction *action : menu->actions()) {
        if (action->text() == QStringLiteral("Copy tag")) {
            duplicateAction = action;
        }
    }
    QVERIFY2(duplicateAction != nullptr, "标签右键菜单缺少复制标签");
    duplicateAction->trigger();

    QCOMPARE(tabs->count(), 2);
    QCOMPARE(tabBar->count(), 2);
    QCOMPARE(tabs->tabText(1), sourceTitle);
    auto *copy = qobject_cast<TerminalWorkspace *>(tabs->widget(1));
    QVERIFY(copy != nullptr);
    QCOMPARE(tabs->currentWidget(), copy);
    QVERIFY(copy->activePane() != source->activePane());
    // 副本必须真的起了一个可交互的会话，而不是留下空面板等用户再点一次。
    QTRY_VERIFY_WITH_TIMEOUT(copy->activePane()->isConnected(), 8000);
    QVERIFY(source->activePane()->isConnected());
}

void MainWindowShellTest::detectedDistributionIsPersistedAndShownInSidebar()
{
    HostRecord host;
    host.id = QStringLiteral("main-window-distribution-host");
    host.kind = HostNodeKind::Host;
    host.name = QStringLiteral("待识别系统的主机");
    host.host = QStringLiteral("192.0.2.31");
    host.username = QStringLiteral("root");
    host.port = 22;
    QString error;
    QVERIFY2(m_database->saveHost(host, &error), qPrintable(error));

    MainWindow window(m_database, m_vault);
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("TerminalPages"));
    auto *newTabButton = window.findChild<QToolButton *>(QStringLiteral("TitleNewTabButton"));
    auto *hostManager = window.findChild<HostManagerWidget *>();
    QVERIFY(tabs != nullptr && newTabButton != nullptr && hostManager != nullptr);
    QTest::mouseClick(newTabButton, Qt::LeftButton);
    auto *workspace = qobject_cast<TerminalWorkspace *>(tabs->widget(0));
    QVERIFY(workspace != nullptr);
    TerminalPane *pane = workspace->activePane();
    QVERIFY(pane != nullptr);

    auto *tree = hostManager->findChild<QTreeWidget *>();
    QVERIFY(tree != nullptr);
    const QSize comparisonSize(20, 20);
    QList<QTreeWidgetItem *> matches =
        tree->findItems(host.name, Qt::MatchExactly | Qt::MatchRecursive);
    QVERIFY2(!matches.isEmpty(), "主机未出现在侧栏");
    const QImage beforePixels = matches.constFirst()->icon(0).pixmap(comparisonSize).toImage();

    // 直接触发终端层的上报信号，把主窗口的持久化与刷新这一段单独验证出来：真实探测依赖
    // 远端服务器，而“收到结果后是否写库并换图标”属于本地逻辑，必须能脱离网络回归。
    ConnectionTarget target;
    target.id = host.id;
    target.host = host.host;
    target.username = host.username;
    target.port = host.port.value_or(quint16{22});
    QVERIFY(QMetaObject::invokeMethod(pane,
                                      "remoteDistributionDetected",
                                      Qt::DirectConnection,
                                      Q_ARG(ConnectionTarget, target),
                                      Q_ARG(QString, QStringLiteral("ubuntu")),
                                      Q_ARG(QString, QStringLiteral("24.04"))));

    const std::optional<HostRecord> stored = m_database->findHost(host.id, &error);
    QVERIFY2(stored.has_value(), qPrintable(error));
    QCOMPARE(stored->distributionId, QStringLiteral("ubuntu"));
    QCOMPARE(stored->distributionVersionId, QStringLiteral("24.04"));

    matches = tree->findItems(host.name, Qt::MatchExactly | Qt::MatchRecursive);
    QVERIFY2(!matches.isEmpty(), "刷新后主机从侧栏消失");
    const QImage afterPixels = matches.constFirst()->icon(0).pixmap(comparisonSize).toImage();
    QCOMPARE(afterPixels,
             HostDistributionIcon::iconFor(QStringLiteral("ubuntu"))
                 .pixmap(comparisonSize)
                 .toImage());
    QVERIFY2(afterPixels != beforePixels, "识别结果没有让侧栏图标发生变化");
    QVERIFY2(matches.constFirst()->toolTip(0).contains(QStringLiteral("Ubuntu 24.04")),
             qPrintable(matches.constFirst()->toolTip(0)));
}

void MainWindowShellTest::languageChangeRetranslatesMenuBar()
{
    LocalizationService localization;
    MainWindow window(m_database, m_vault, &localization);

    QMenuBar *menuBar = window.menuBar();
    QVERIFY(menuBar != nullptr);

    const auto findMenu = [menuBar](const QStringList &acceptedTitles) -> QMenu * {
        const QList<QAction *> actions = menuBar->actions();
        for (QAction *action : actions) {
            if (action->menu() != nullptr && acceptedTitles.contains(action->menu()->title())) {
                return action->menu();
            }
        }
        return nullptr;
    };

    // 源语言是英文；zh_CN 通过 :/i18n/shellium_zh_CN.qm 提供翻译。
    QMenu *connectionMenu = findMenu({QStringLiteral("Connection"), QString::fromUtf8("连接")});
    QVERIFY(connectionMenu != nullptr);
    QCOMPARE(connectionMenu->title(), QStringLiteral("Connection"));
    QVERIFY(!connectionMenu->actions().isEmpty());
    QCOMPARE(connectionMenu->actions().constFirst()->text(), QStringLiteral("Connect to Server"));

    QVERIFY(localization.apply(QStringLiteral("zh_CN")));
    // 安装翻译器后 Qt 会向各顶层控件投递 LanguageChange；测试无事件循环，手动冲刷。
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LanguageChange);

    QCOMPARE(connectionMenu->title(), QString::fromUtf8("连接"));
    QCOMPARE(connectionMenu->actions().constFirst()->text(), QString::fromUtf8("连接服务器"));

    QVERIFY(localization.apply(QStringLiteral("en")));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LanguageChange);
    QCOMPARE(connectionMenu->title(), QStringLiteral("Connection"));
    QCOMPARE(connectionMenu->actions().constFirst()->text(), QStringLiteral("Connect to Server"));

    // 常驻侧栏控件也必须随 i18n::onRetranslate 立即热切换，而不是等重开窗口。
    auto *addHostButton = window.findChild<QAbstractButton *>(QStringLiteral("HostAddButton"));
    QVERIFY(addHostButton != nullptr);
    QCOMPARE(addHostButton->text(), QStringLiteral("New host"));

    QVERIFY(localization.apply(QStringLiteral("zh_CN")));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LanguageChange);
    QCOMPARE(addHostButton->text(), QString::fromUtf8("新建主机"));

    QVERIFY(localization.apply(QStringLiteral("en")));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LanguageChange);
    QCOMPARE(addHostButton->text(), QStringLiteral("New host"));

    // 停在非「主机」页时切换语言，侧栏标题必须跟随当前页（端口转发）而不是被 retranslateUi
    // 硬编码回「主机」，也不能残留上一次语言。
    auto *sidebarTitle = window.findChild<QLabel *>(QStringLiteral("SidebarHeader"));
    QVERIFY(sidebarTitle != nullptr);
    for (QToolButton *navButton : window.findChildren<QToolButton *>()) {
        if (navButton->property("navButton").toBool()
            && navButton->toolTip() == QStringLiteral("Port forwarding")) {
            navButton->setChecked(true);
            break;
        }
    }
    QCOMPARE(sidebarTitle->text(), QStringLiteral("Port forwarding"));
    QVERIFY(localization.apply(QStringLiteral("zh_CN")));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LanguageChange);
    QCOMPARE(sidebarTitle->text(), QString::fromUtf8("端口转发"));

    QVERIFY(localization.apply(QStringLiteral("en")));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LanguageChange);
    QCOMPARE(sidebarTitle->text(), QStringLiteral("Port forwarding"));
}

QTEST_MAIN(MainWindowShellTest)

#include "MainWindowShellTest.moc"
