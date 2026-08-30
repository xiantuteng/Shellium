#include "app/MainWindow.h"
#include "i18n/LocalizationService.h"
#include "core/ConnectionTarget.h"
#include "core/HostRecord.h"
#include "security/Vault.h"
#include "storage/ApplicationDatabase.h"
#include "terminal/TerminalPane.h"
#include "terminal/TerminalView.h"
#include "terminal/TerminalWorkspace.h"
#include "ui/HostDistributionIcon.h"
#include "ui/HostManagerWidget.h"

#include <QAbstractButton>
#include <QAction>
#include <QCoreApplication>
#include <QEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
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
    void duplicateMenuItemOpensASecondCopyOfTheTab();

    /** @brief 终端上报的发行版必须落库并让侧栏立刻换成对应图标，重启后依然保持。 */
    void detectedDistributionIsPersistedAndShownInSidebar();

    /** @brief 切换界面语言后，常驻菜单栏必须就地重译，切回来也要恢复。 */
    void languageChangeRetranslatesMenuBar();

private:
    ApplicationDatabase *m_database = nullptr;
    CredentialVault *m_vault = nullptr;
};

void MainWindowShellTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    m_database = new ApplicationDatabase(ApplicationDatabase::inMemoryPath());
    QString error;
    QVERIFY2(m_database->initialize(&error), qPrintable(error));
    m_vault = new CredentialVault(*m_database);
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
}

QTEST_MAIN(MainWindowShellTest)

#include "MainWindowShellTest.moc"
