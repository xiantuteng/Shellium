#include "app/MainWindow.h"
#include "security/Vault.h"
#include "storage/ApplicationDatabase.h"
#include "terminal/TerminalPane.h"
#include "terminal/TerminalView.h"
#include "terminal/TerminalWorkspace.h"

#include <QLabel>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>
#include <QToolButton>

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
    void emptyStateKeepsNewTabButtonNextToTheSidebarToggle();

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
    // 默认终端就是本地 zsh：新标签必须真的启动一个可交互 Shell，而不是留下一个空面板。
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
    QVERIFY2(chrome->width() - buttonRight < 80,
             "标签溢出时新建按钮必须固定在标题行最右端");
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

void MainWindowShellTest::emptyStateKeepsNewTabButtonNextToTheSidebarToggle()
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

    // 一个标签都没有时，标签栏不占宽度，"+"必须紧跟在侧栏开关右边；否则标题行左侧会空出
    // 一大块无法解释的留白，用户找不到新建入口。
    const int sidebarRight = sidebarButton->geometry().right();
    const int buttonLeft = newTabButton->geometry().left();
    QVERIFY2(buttonLeft - sidebarRight < 24,
             qPrintable(QStringLiteral("空态下新建按钮与侧栏开关之间空出 %1 像素")
                            .arg(buttonLeft - sidebarRight)));
}

QTEST_MAIN(MainWindowShellTest)

#include "MainWindowShellTest.moc"
