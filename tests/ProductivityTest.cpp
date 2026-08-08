#include "productivity/ProductivityModel.h"
#include "productivity/ProductivityWidget.h"
#include "storage/ApplicationDatabase.h"
#include "terminal/TerminalPane.h"
#include "terminal/TerminalView.h"

#include <QCheckBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QTreeWidget>

#include <memory>

/**
 * @brief 验证效率工具的持久化、补全、终端接线和多目标信号契约。
 *
 * 所有数据使用独立内存数据库；终端接线测试不启动真实 Shell 或 SSH，只调用命令提交槽，
 * 因而不会依赖本机环境、网络或用户凭据。
 */
class ProductivityTest final : public QObject {
    Q_OBJECT

private slots:
    /** @brief 每个用例创建全新内存数据库，避免测试间历史和 Snippet 相互污染。 */
    void init();

    /** @brief 用例结束后先销毁模型再关闭数据库，满足非拥有指针生命周期约束。 */
    void cleanup();

    /** @brief Snippet 分组、更新、环检测和递归删除必须形成完整 CRUD 闭环。 */
    void snippetCrudGroupingAndRecursiveDelete();

    /** @brief 历史应按主机隔离、最近优先去重，并尊重前导空格的隐私约定。 */
    void historyCompletionIsScopedDeduplicatedAndPrivate();

    /** @brief TerminalPane 收到提交信号后应直接调用数据库历史接口，且不能记录控制伪命令。 */
    void terminalPanePersistsSubmittedCommands();

    /** @brief Widget 应发出多主机执行、广播模式和一次性原始输入信号。 */
    void widgetExposesMultiTargetExecutionAndBroadcastContracts();

private:
    /** @brief 递归查找树中保存指定稳定 id 的节点。 */
    static QTreeWidgetItem *findTreeItem(QTreeWidgetItem *parent, const QString &id);

    std::unique_ptr<ApplicationDatabase> m_database;
    std::unique_ptr<ProductivityModel> m_model;
};

void ProductivityTest::init()
{
    m_database = std::make_unique<ApplicationDatabase>(ApplicationDatabase::inMemoryPath());
    QString error;
    QVERIFY2(m_database->initialize(&error), qPrintable(error));
    m_model = std::make_unique<ProductivityModel>(m_database.get());
}

void ProductivityTest::cleanup()
{
    m_model.reset();
    m_database.reset();
}

QTreeWidgetItem *ProductivityTest::findTreeItem(QTreeWidgetItem *parent, const QString &id)
{
    if (parent->data(0, Qt::UserRole).toString() == id) {
        return parent;
    }
    for (int index = 0; index < parent->childCount(); ++index) {
        if (QTreeWidgetItem *found = findTreeItem(parent->child(index), id); found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

void ProductivityTest::snippetCrudGroupingAndRecursiveDelete()
{
    SnippetRecord operations;
    operations.id = QStringLiteral("folder-operations");
    operations.kind = SnippetNodeKind::Folder;
    operations.name = QStringLiteral("日常运维");
    QVERIFY(m_model->saveSnippet(operations));

    SnippetRecord databaseGroup;
    databaseGroup.id = QStringLiteral("folder-database");
    databaseGroup.parentId = operations.id;
    databaseGroup.kind = SnippetNodeKind::Folder;
    databaseGroup.name = QStringLiteral("数据库");
    QVERIFY(m_model->saveSnippet(databaseGroup));

    SnippetRecord statusSnippet;
    statusSnippet.id = QStringLiteral("snippet-status");
    statusSnippet.parentId = databaseGroup.id;
    statusSnippet.kind = SnippetNodeKind::Snippet;
    statusSnippet.name = QStringLiteral("查看状态");
    statusSnippet.command = QStringLiteral("systemctl status postgresql");
    statusSnippet.description = QStringLiteral("只读检查，不修改服务状态");
    statusSnippet.sortOrder = 5;
    QVERIFY(m_model->saveSnippet(statusSnippet));

    bool ok = false;
    const QList<SnippetRecord> children = m_model->snippetsInGroup(databaseGroup.id, &ok);
    QVERIFY(ok);
    QCOMPARE(children.size(), 1);
    QCOMPARE(children.constFirst().command, statusSnippet.command);

    statusSnippet.command = QStringLiteral("systemctl --no-pager status postgresql");
    QVERIFY(m_model->saveSnippet(statusSnippet));
    const std::optional<SnippetRecord> updated = m_model->snippet(statusSnippet.id);
    QVERIFY(updated.has_value());
    QCOMPARE(updated->command, statusSnippet.command);

    // 把祖先放进后代会形成 operations→database→operations 环，数据库必须拒绝而不是等
    // 到界面递归加载时才崩溃或挂死。
    operations.parentId = databaseGroup.id;
    QSignalSpy errorSpy(m_model.get(), &ProductivityModel::errorOccurred);
    QVERIFY(!m_model->saveSnippet(operations));
    QCOMPARE(errorSpy.count(), 1);

    int removedCount = 0;
    QVERIFY(m_model->removeSnippet(QStringLiteral("folder-operations"), &removedCount));
    QCOMPARE(removedCount, 3);
    QVERIFY(!m_model->snippet(statusSnippet.id).has_value());
}

void ProductivityTest::historyCompletionIsScopedDeduplicatedAndPrivate()
{
    QVERIFY(m_model->recordCommand(QStringLiteral("host-a"), QStringLiteral("git status")));
    QVERIFY(m_model->recordCommand(QStringLiteral("host-a"), QStringLiteral("git log --oneline")));
    QVERIFY(m_model->recordCommand(QStringLiteral("host-a"), QStringLiteral("git status")));
    QVERIFY(m_model->recordCommand(QStringLiteral("host-b"), QStringLiteral("git clean -fd")));
    QVERIFY(m_model->recordCommand(QString(), QStringLiteral("git config --list")));

    // 前导空格表示用户要求像 HISTCONTROL=ignorespace 一样不留痕；即使命令含令牌也应
    // 返回成功但不进入数据库，避免调用方因“安全忽略”显示误导性错误。
    QVERIFY(m_model->recordCommand(QStringLiteral("host-a"),
                                   QStringLiteral(" curl -H 'Token: secret' example")));

    const QStringList hostACandidates = m_model->completionCandidates(
        QStringLiteral("host-a"), QStringLiteral("git"), 10);
    QCOMPARE(hostACandidates,
             QStringList({QStringLiteral("git status"), QStringLiteral("git log --oneline")}));
    QVERIFY(!hostACandidates.contains(QStringLiteral("git clean -fd")));

    bool ok = false;
    const QList<CommandHistoryEntry> hostAHistory = m_model->recentHistory(
        QStringLiteral("host-a"), 20, &ok);
    QVERIFY(ok);
    QCOMPARE(hostAHistory.size(), 3);
    for (const CommandHistoryEntry &entry : hostAHistory) {
        QVERIFY(!entry.command.contains(QStringLiteral("secret")));
    }
    QCOMPARE(m_model->completionCandidates(QString(), QStringLiteral("git"), 10),
             QStringList({QStringLiteral("git config --list")}));
}

void ProductivityTest::terminalPanePersistsSubmittedCommands()
{
    TerminalPane pane;
    pane.setCommandHistoryDatabase(m_database.get());

    // 直接调用私有槽模拟 TerminalView 的 commandSubmitted；即使当前还没有 worker，本地
    // 命令也必须按 host_id=NULL 持久化，历史能力不能依赖网络是否已经连接。
    QVERIFY(QMetaObject::invokeMethod(&pane,
                                      "inspectSubmittedCommand",
                                      Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("uptime"))));
    QVERIFY(QMetaObject::invokeMethod(&pane,
                                      "inspectSubmittedCommand",
                                      Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("Ctrl-D"))));
    QVERIFY(QMetaObject::invokeMethod(&pane,
                                      "inspectSubmittedCommand",
                                      Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral(" secret-command"))));

    // 程序化 Snippet 提交走 TerminalView 的统一入口，同样要穿过 Pane 的数据库接线。
    TerminalView *view = pane.findChild<TerminalView *>();
    QVERIFY(view != nullptr);
    view->submitCommand(QStringLiteral("pwd"));

    bool ok = false;
    const QList<CommandHistoryEntry> history = m_database->recentCommands(QString(), 10, &ok);
    QVERIFY(ok);
    QCOMPARE(history.size(), 2);
    QCOMPARE(history.at(0).command, QStringLiteral("pwd"));
    QCOMPARE(history.at(1).command, QStringLiteral("uptime"));
}

void ProductivityTest::widgetExposesMultiTargetExecutionAndBroadcastContracts()
{
    SnippetRecord snippet;
    snippet.id = QStringLiteral("snippet-disk");
    snippet.kind = SnippetNodeKind::Snippet;
    snippet.name = QStringLiteral("磁盘使用率");
    snippet.command = QStringLiteral("df -h");
    QVERIFY(m_model->saveSnippet(snippet));

    ConnectionTarget first;
    first.id = QStringLiteral("host-a");
    first.displayName = QStringLiteral("应用一");
    first.host = QStringLiteral("app-a.internal");
    first.confidence = ConnectionTarget::Confidence::Confirmed;
    ConnectionTarget second = first;
    second.id = QStringLiteral("host-b");
    second.displayName = QStringLiteral("应用二");
    second.host = QStringLiteral("app-b.internal");

    ProductivityWidget widget(m_model.get());
    widget.setExecutionTargets({first, second});

    QTreeWidget *tree = widget.findChild<QTreeWidget *>(
        QStringLiteral("productivitySnippetTree"));
    QListWidget *executionTargets = widget.findChild<QListWidget *>(
        QStringLiteral("productivityExecutionTargets"));
    QPushButton *executeButton = widget.findChild<QPushButton *>(
        QStringLiteral("productivityExecuteSnippet"));
    QVERIFY(tree != nullptr);
    QVERIFY(executionTargets != nullptr);
    QVERIFY(executeButton != nullptr);
    QCOMPARE(executionTargets->count(), 2);
    executionTargets->item(0)->setCheckState(Qt::Checked);
    executionTargets->item(1)->setCheckState(Qt::Checked);

    QTreeWidgetItem *snippetItem = nullptr;
    for (int index = 0; index < tree->topLevelItemCount() && snippetItem == nullptr; ++index) {
        snippetItem = findTreeItem(tree->topLevelItem(index), snippet.id);
    }
    QVERIFY(snippetItem != nullptr);
    tree->setCurrentItem(snippetItem);

    QSignalSpy executionSpy(&widget, &ProductivityWidget::commandExecutionRequested);
    executeButton->click();
    QCOMPARE(executionSpy.count(), 1);
    QCOMPARE(executionSpy.constFirst().at(0).toString(), QStringLiteral("df -h"));
    QCOMPARE(executionSpy.constFirst().at(1).toStringList(),
             QStringList({QStringLiteral("host-a"), QStringLiteral("host-b")}));

    QListWidget *broadcastTargets = widget.findChild<QListWidget *>(
        QStringLiteral("productivityBroadcastTargets"));
    QCheckBox *broadcastEnabled = widget.findChild<QCheckBox *>(
        QStringLiteral("productivityBroadcastEnabled"));
    QLineEdit *broadcastInput = widget.findChild<QLineEdit *>(
        QStringLiteral("productivityBroadcastInput"));
    QPushButton *broadcastSend = widget.findChild<QPushButton *>(
        QStringLiteral("productivityBroadcastSend"));
    QVERIFY(broadcastTargets != nullptr);
    QVERIFY(broadcastEnabled != nullptr);
    QVERIFY(broadcastInput != nullptr);
    QVERIFY(broadcastSend != nullptr);
    broadcastTargets->item(1)->setCheckState(Qt::Checked);

    QSignalSpy modeSpy(&widget, &ProductivityWidget::broadcastModeChanged);
    broadcastEnabled->setChecked(true);
    QVERIFY(modeSpy.count() >= 1);
    QCOMPARE(modeSpy.constLast().at(0).toBool(), true);
    QCOMPARE(modeSpy.constLast().at(1).toStringList(), QStringList({QStringLiteral("host-b")}));

    QSignalSpy inputSpy(&widget, &ProductivityWidget::broadcastInputRequested);
    broadcastInput->setText(QStringLiteral("hostname"));
    broadcastSend->click();
    QCOMPARE(inputSpy.count(), 1);
    QCOMPARE(inputSpy.constFirst().at(0).toByteArray(), QByteArrayLiteral("hostname\r"));
    QCOMPARE(inputSpy.constFirst().at(1).toStringList(), QStringList({QStringLiteral("host-b")}));
}

QTEST_MAIN(ProductivityTest)

#include "ProductivityTest.moc"

