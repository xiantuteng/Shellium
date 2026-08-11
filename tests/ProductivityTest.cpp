#include "productivity/ProductivityModel.h"
#include "productivity/ProductivityWidget.h"
#include "ssh/SshSessionStack.h"
#include "ssh/SshTunnelBroker.h"
#include "storage/ApplicationDatabase.h"
#include "terminal/TerminalPane.h"
#include "terminal/TerminalView.h"

#include <QCheckBox>
#include <QLabel>
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

    /**
     * @brief 会话状态栏的瞬时提示必须紧邻服务器名，且到期后自动清空。
     *
     * 这条消息此前会被送到窗口标题栏的全局提示区，与产生它的标签页在视觉上脱节；
     * 现在它必须落在 TerminalPane 自己的状态栏里，因此本用例直接检查该控件。
     */
    void terminalPaneShowsAndExpiresTransientStatus();

    /**
     * @brief "推测嵌套目标"徽章默认隐藏，且没有自持层时不会被形似 ssh 的命令误触发。
     *
     * 徽章完整的"显示"路径需要一次真实 SSH 根连接（自持层）才能触发，不在本隔离测试
     * 套件范围内；这里覆盖的是不需要网络也该成立的部分——默认隐藏、守卫没被打开、切回
     * 本地终端后保持隐藏。
     */
    void terminalPaneShowsNestedBadgeOnlyWhileInferredLayerIsActive();

    /**
     * @brief 320px 侧栏放不下"名称/说明"两列，片段树必须收窄成单列，说明改到第二行角色里；
     * 没有说明时要退化显示命令，分组节点则完全不设置这个角色。
     */
    void snippetTreeIsSingleColumnWithDescriptionFallbackToCommand();

    /** @brief 嵌套路由必须保留远端密钥 SFTP 兜底所需的外层认证配置与内层端口。 */
    void tunnelBrokerReturnsRemoteSftpRelayEndpoints();

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

void ProductivityTest::terminalPaneShowsAndExpiresTransientStatus()
{
    TerminalPane pane;
    auto *statusLabel = pane.findChild<QLabel *>(
        QStringLiteral("terminalSessionTransientStatus"));
    QVERIFY2(statusLabel != nullptr,
             "会话状态栏必须存在一个专门承载瞬时提示的 QLabel，紧邻服务器名之后");
    QVERIFY(statusLabel->text().isEmpty());

    pane.showTransientStatus(QStringLiteral("正在读取主目录…"));
    QVERIFY2(statusLabel->text().contains(QStringLiteral("正在读取主目录…")),
             "调用后必须立即显示传入的消息文字");

    // 到期后必须自动清空，不能永久占用状态栏空间——这条提示本质上是转瞬即逝的操作反馈。
    QTRY_VERIFY_WITH_TIMEOUT(statusLabel->text().isEmpty(), 9000);
}

void ProductivityTest::terminalPaneShowsNestedBadgeOnlyWhileInferredLayerIsActive()
{
    // 徽章真正显示的那条路径（推测层建立在一个已认证的自持层之上）需要一次真实 SSH
    // 根连接才能触发——inspectSubmittedCommand 在 m_ownedTargets 为空时直接短路返回，
    // 这是刻意的：没有人已经连上任何机器时，解析器不该把终端里出现的字符串当成"进入了
    // 嵌套服务器"。真实网络连接不在这个隔离测试套件的覆盖范围内（见类注释），因此这里
    // 只覆盖能在没有网络的前提下验证的部分：徽章存在、默认隐藏，且这个短路守卫本身没有
    // 被意外打开——传一条形似 ssh 的命令进去，在没有自持层时绝不能让徽章冒出来。
    TerminalPane pane;
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));

    auto *badge = pane.findChild<QLabel *>(QStringLiteral("terminalSessionNestedBadge"));
    QVERIFY2(badge != nullptr, "状态栏必须有一个专门的推测层徽章控件");
    QVERIFY2(!badge->isVisibleTo(&pane), "没有任何推测层时徽章不能显示");

    QVERIFY(QMetaObject::invokeMethod(&pane,
                                      "inspectSubmittedCommand",
                                      Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("ssh demo@example.com"))));
    QVERIFY2(!badge->isVisibleTo(&pane),
             "还没有任何自持层（从未真正连接过）时，徽章不能因为一条形似 ssh 的命令就冒出来");

    // 本地 Shell 是徽章"必须隐藏"的另一类状态：即便未来某处误把它设成可见，切回本地
    // 终端也必须把它带回隐藏状态。
    pane.openLocalShell();
    QVERIFY2(!badge->isVisibleTo(&pane), "本地终端状态下徽章必须保持隐藏");
}

void ProductivityTest::snippetTreeIsSingleColumnWithDescriptionFallbackToCommand()
{
    SnippetRecord folder;
    folder.id = QStringLiteral("folder-1");
    folder.kind = SnippetNodeKind::Folder;
    folder.name = QStringLiteral("常用运维");
    QVERIFY(m_model->saveSnippet(folder));

    SnippetRecord withDescription;
    withDescription.id = QStringLiteral("snippet-with-desc");
    withDescription.parentId = folder.id;
    withDescription.kind = SnippetNodeKind::Snippet;
    withDescription.name = QStringLiteral("查看磁盘占用");
    withDescription.command = QStringLiteral("df -h | sort -k5 -h -r");
    withDescription.description = QStringLiteral("按占用从高到低查看各分区使用率");
    QVERIFY(m_model->saveSnippet(withDescription));

    // 没有说明时必须退化显示命令本身，否则第二行会是空的，用户看不出这条片段做什么。
    SnippetRecord withoutDescription;
    withoutDescription.id = QStringLiteral("snippet-without-desc");
    withoutDescription.parentId = folder.id;
    withoutDescription.kind = SnippetNodeKind::Snippet;
    withoutDescription.name = QStringLiteral("重启 nginx");
    withoutDescription.command = QStringLiteral("systemctl restart nginx");
    QVERIFY(m_model->saveSnippet(withoutDescription));

    ProductivityWidget widget(m_model.get());
    QTreeWidget *tree = widget.findChild<QTreeWidget *>(
        QStringLiteral("productivitySnippetTree"));
    QVERIFY(tree != nullptr);

    // 320px 侧栏放不下两列表头，树必须是单列且不显示表头。
    QCOMPARE(tree->columnCount(), 1);
    QVERIFY(tree->isHeaderHidden());

    constexpr int kPreviewRole = Qt::UserRole + 2;

    QTreeWidgetItem *folderItem = nullptr;
    for (int index = 0; index < tree->topLevelItemCount(); ++index) {
        if (tree->topLevelItem(index)->data(0, Qt::UserRole).toString() == folder.id) {
            folderItem = tree->topLevelItem(index);
            break;
        }
    }
    QVERIFY(folderItem != nullptr);
    // 分组行不是"没有说明的片段"，不能拿命令去填第二行——分组本来就没有 command。
    QVERIFY(!folderItem->data(0, kPreviewRole).isValid());

    QTreeWidgetItem *withDescItem = findTreeItem(folderItem, withDescription.id);
    QVERIFY(withDescItem != nullptr);
    QCOMPARE(withDescItem->data(0, kPreviewRole).toString(), withDescription.description);

    QTreeWidgetItem *withoutDescItem = findTreeItem(folderItem, withoutDescription.id);
    QVERIFY(withoutDescItem != nullptr);
    QCOMPARE(withoutDescItem->data(0, kPreviewRole).toString(), withoutDescription.command);
}

void ProductivityTest::tunnelBrokerReturnsRemoteSftpRelayEndpoints()
{
    ConnectionTarget outer;
    outer.id = QStringLiteral("outer-layer");
    outer.host = QStringLiteral("115.159.35.31");
    outer.username = QStringLiteral("root");
    outer.port = 3822;
    outer.password = QStringLiteral("process-only-secret");

    ConnectionTarget inner;
    inner.id = QStringLiteral("inner-layer");
    inner.host = QStringLiteral("192.168.1.27");
    inner.username = QStringLiteral("root");
    inner.port = 52231;

    // 栈不启动网络线程；Broker 只需要一个生命周期有效的 QPointer 来验证路由归属。
    SshSessionStack stack(outer, false, nullptr);
    SshTunnelBroker &broker = SshTunnelBroker::instance();
    broker.registerRoute(inner.id, &stack, QString(), inner, outer);

    ConnectionTarget resolvedOuter;
    ConnectionTarget resolvedInner;
    QVERIFY(broker.remoteSftpRelay(inner.id, &resolvedOuter, &resolvedInner));
    QCOMPARE(resolvedOuter.connectionSpecifier(true), outer.connectionSpecifier(true));
    QCOMPARE(resolvedOuter.password, outer.password);
    QCOMPARE(resolvedInner.connectionSpecifier(true), inner.connectionSpecifier(true));

    // 注销后不能继续泄露或复用外层认证材料；后台任务必须看到路由已经失效。
    broker.unregisterRoute(inner.id);
    QVERIFY(!broker.remoteSftpRelay(inner.id, &resolvedOuter, &resolvedInner));
}

QTEST_MAIN(ProductivityTest)

#include "ProductivityTest.moc"
