#include "productivity/ProductivityModel.h"
#include "productivity/ProductivityWidget.h"
#include "ssh/SshSessionStack.h"
#include "ssh/SshTunnelBroker.h"
#include "storage/ApplicationDatabase.h"
#include "terminal/TerminalPane.h"
#include "terminal/TerminalView.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTest>
#include <QTextEdit>
#include <QToolButton>
#include <QTranslator>
#include <QVBoxLayout>

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

    /** @brief Snippet 分组、更新、环检测和递归删除在模型层仍必须形成完整 CRUD 闭环。 */
    void snippetCrudGroupingAndRecursiveDelete();

    /** @brief 历史应按主机隔离、最近优先去重，并尊重前导空格的隐私约定。 */
    void historyCompletionIsScopedDeduplicatedAndPrivate();

    /** @brief TerminalPane 收到提交信号后应直接调用数据库历史接口，且不能记录控制伪命令。 */
    void terminalPanePersistsSubmittedCommands();

    /**
     * @brief 界面层不再暴露分组功能，也不再提供多目标执行勾选框：Snippet 列表只显示扁平
     * 记录，且执行始终发出空 targetIds（由上层解释为当前活动终端）。
     *
     * "广播输入"标签页已整体移除（多终端同步输入只由工作空间标题栏开关负责），侧栏里不能
     * 再残留任何广播目标列表、开关或一次性发送入口。
     */
    void widgetHasNoGroupingNoBroadcastAndExecutesOnCurrentTerminal();

    /**
     * @brief 标题行动作条与主机侧栏同构：绿色"新建片段"主按钮 + 删除图标；删除只在片段页
     * 可用，切到历史页时置灰。
     */
    void headerActionsMatchHostSidebarAndFollowCurrentTab();

    /** @brief 片段搜索按名称/命令/说明不区分大小写过滤，筛选期间必须关闭拖拽排序。 */
    void snippetSearchFiltersRowsAndSuspendsReordering();

    /**
     * @brief 会话状态栏的瞬时提示必须紧邻服务器名，且到期后自动清空。
     *
     * 这条消息此前会被送到窗口标题栏的全局提示区，与产生它的标签页在视觉上脱节；
     * 现在它必须落在 TerminalPane 自己的状态栏里，因此本用例直接检查该控件。
     */
    void terminalPaneShowsAndExpiresTransientStatus();

    /**
     * @brief 传输结果必须带上成功/失败图标，且与普通瞬时提示共用同一条自动消失定时器。
     *
     * 拖拽上传等入口最终都通过 showTransientTransferResult 展示结果；图标本身、以及
     * "普通提示不该残留一个过期图标"这两点都必须验证，否则下一条不带图标的提示会被
     * 上一次传输结果的图标污染。
     */
    void terminalPaneShowsIconForTransferResultAndClearsOnPlainStatus();

    /** @brief 终端状态栏不应再创建“推测嵌套目标”徽章，以免重复展示内部判定。 */
    void terminalPaneDoesNotExposeInferredTargetBadge();

    /**
     * @brief 片段列表必须是扁平单列列表，没有说明时要退化显示命令；每行还要带一个
     * "执行/复制"按钮，前 10 行的执行按钮要在 Tooltip 中提示 Ctrl+1..Ctrl+0 快捷键。
     */
    void snippetListIsFlatWithDescriptionFallbackAndShortcutHints();

    /** @brief 新增/编辑片段改为弹出对话框；保存后数据落库、对话框关闭，且不再有分组字段。 */
    void snippetEditorDialogSavesWithoutGroupingField();

    /** @brief Ctrl+1 快捷键应执行列表第一个片段，并像按钮一样只作用于当前活动终端。 */
    void snippetShortcutExecutesFirstSnippetOnCurrentTerminal();

    /** @brief 复制按钮应把命令原样写入系统剪贴板，而不改动数据库。 */
    void snippetRowCopyButtonWritesCommandToClipboard();

    /**
     * @brief 切换程序语言后，标签页标题、行内“执行/复制”按钮等所有常驻文案必须立即
     * 跟随新语言，而不是残留上一次语言（行内按钮是每次 reloadSnippets 重建的，最容易漏）。
     */
    void snippetRowsAndTabsRetranslateOnLanguageChange();

    /** @brief 拖拽排序完成后，新的可见顺序必须回写为持久化的 sortOrder。 */
    void reorderingSnippetListPersistsNewSortOrder();

    /** @brief 嵌套路由必须保留远端密钥 SFTP 兜底所需的外层认证配置与内层端口。 */
    void tunnelBrokerReturnsRemoteSftpRelayEndpoints();

private:
    /** @brief 在 Snippet 列表中按稳定 id 查找对应的行。 */
    static QListWidgetItem *findSnippetItem(const QListWidget *list, const QString &id);

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

QListWidgetItem *ProductivityTest::findSnippetItem(const QListWidget *list, const QString &id)
{
    for (int index = 0; index < list->count(); ++index) {
        if (list->item(index)->data(Qt::UserRole).toString() == id) {
            return list->item(index);
        }
    }
    return nullptr;
}

void ProductivityTest::snippetCrudGroupingAndRecursiveDelete()
{
    // 界面已经不再提供分组入口，但模型/数据库仍然通用地支持树状结构（例如未来同步旧
    // 数据），因此这里继续直接对模型做完整 CRUD 校验，不依赖 ProductivityWidget。
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

void ProductivityTest::widgetHasNoGroupingNoBroadcastAndExecutesOnCurrentTerminal()
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

    // 分组和多目标执行勾选框都必须彻底消失，而不只是隐藏。
    QVERIFY2(widget.findChild<QToolButton *>(QStringLiteral("productivityNewGroup")) == nullptr,
             "不应再存在“新建分组”按钮");
    QVERIFY2(widget.findChild<QListWidget *>(QStringLiteral("productivityExecutionTargets"))
                 == nullptr,
             "Snippet 页不应再有可勾选的多目标执行列表");

    QListWidget *snippetList = widget.findChild<QListWidget *>(
        QStringLiteral("productivitySnippetList"));
    QVERIFY(snippetList != nullptr);
    QListWidgetItem *snippetItem = findSnippetItem(snippetList, snippet.id);
    QVERIFY(snippetItem != nullptr);
    auto *executeButton = snippetList->itemWidget(snippetItem)->findChild<QToolButton *>(
        QStringLiteral("productivitySnippetExecute"));
    QVERIFY(executeButton != nullptr);

    QSignalSpy executionSpy(&widget, &ProductivityWidget::commandExecutionRequested);
    executeButton->click();
    QCOMPARE(executionSpy.count(), 1);
    QCOMPARE(executionSpy.constFirst().at(0).toString(), QStringLiteral("df -h"));
    // 空 targetIds：执行始终落在当前活动终端，不再由界面选择多个目标。
    QVERIFY(executionSpy.constFirst().at(1).toStringList().isEmpty());

    // 广播输入页及其全部控件必须彻底消失，而不只是隐藏标签。
    auto *tabs = widget.findChild<QTabWidget *>(QStringLiteral("productivityTabs"));
    QVERIFY(tabs != nullptr);
    QCOMPARE(tabs->count(), 2);
    for (int index = 0; index < tabs->count(); ++index) {
        QVERIFY2(!tabs->tabText(index).contains(QStringLiteral("Broadcast")),
                 "效率工具不应再有广播输入标签页");
    }
    QVERIFY(widget.findChild<QWidget *>(QStringLiteral("productivityBroadcastPage")) == nullptr);
    QVERIFY(widget.findChild<QListWidget *>(QStringLiteral("productivityBroadcastTargets"))
            == nullptr);
    QVERIFY(widget.findChild<QCheckBox *>(QStringLiteral("productivityBroadcastEnabled"))
            == nullptr);
    QVERIFY(widget.findChild<QLineEdit *>(QStringLiteral("productivityBroadcastInput"))
            == nullptr);
    QVERIFY(widget.findChild<QPushButton *>(QStringLiteral("productivityBroadcastSend"))
            == nullptr);
    QVERIFY(widget.metaObject()->indexOfSignal("broadcastModeChanged(bool,QStringList)") < 0);
    QVERIFY(widget.metaObject()->indexOfSignal("broadcastInputRequested(QByteArray,QStringList)")
            < 0);

    // 目标快照仍然驱动历史页的主机筛选：本地项 + 两台主机。
    auto *historyHost = widget.findChild<QComboBox *>(QStringLiteral("productivityHistoryHost"));
    QVERIFY(historyHost != nullptr);
    QCOMPARE(historyHost->count(), 3);
    QCOMPARE(historyHost->itemData(2).toString(), QStringLiteral("host-b"));
}

void ProductivityTest::headerActionsMatchHostSidebarAndFollowCurrentTab()
{
    ProductivityWidget widget(m_model.get());
    QWidget *header = widget.headerActionsWidget();
    QVERIFY(header != nullptr);

    auto *newSnippet = header->findChild<QPushButton *>(QStringLiteral("productivityNewSnippet"));
    QVERIFY2(newSnippet != nullptr, "新建片段必须挂在标题行动作条里");
    QVERIFY(newSnippet->property("primaryAction").toBool());
    QVERIFY(newSnippet->property("hostPrimaryAction").toBool());
    auto *remove = header->findChild<QToolButton *>(QStringLiteral("productivityRemoveSnippet"));
    QVERIFY(remove != nullptr);
    QVERIFY(remove->property("compactIconButton").toBool());
    QCOMPARE(remove->toolButtonStyle(), Qt::ToolButtonIconOnly);

    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
    QVERIFY(remove->isVisible());
    auto *tabs = widget.findChild<QTabWidget *>(QStringLiteral("productivityTabs"));
    QVERIFY(tabs != nullptr);
    tabs->setCurrentIndex(1);
    QVERIFY2(remove->isVisible() && !remove->isEnabled(),
             "历史页没有可删除的片段，删除按钮必须置灰（但不隐藏，避免标题行跳动）");

    // 在历史页点"新建片段"要切回片段页再弹编辑框，否则用户看不到新片段落在哪里。
    newSnippet->click();
    QCOMPARE(tabs->currentIndex(), 0);
    QVERIFY(remove->isEnabled());
    auto *dialog = widget.findChild<QDialog *>(QStringLiteral("productivitySnippetDialog"));
    QVERIFY(dialog != nullptr && dialog->isVisible());
    dialog->reject();
}

void ProductivityTest::snippetSearchFiltersRowsAndSuspendsReordering()
{
    SnippetRecord disk;
    disk.id = QStringLiteral("snippet-disk");
    disk.kind = SnippetNodeKind::Snippet;
    disk.name = QStringLiteral("Disk usage");
    disk.command = QStringLiteral("df -h");
    disk.sortOrder = 0;
    QVERIFY(m_model->saveSnippet(disk));
    SnippetRecord nginx;
    nginx.id = QStringLiteral("snippet-nginx");
    nginx.kind = SnippetNodeKind::Snippet;
    nginx.name = QStringLiteral("重启");
    nginx.command = QStringLiteral("systemctl restart nginx");
    nginx.description = QStringLiteral("Web 服务");
    nginx.sortOrder = 1;
    QVERIFY(m_model->saveSnippet(nginx));

    ProductivityWidget widget(m_model.get());
    auto *search = widget.findChild<QLineEdit *>(QStringLiteral("productivitySnippetSearch"));
    auto *list = widget.findChild<QListWidget *>(QStringLiteral("productivitySnippetList"));
    QVERIFY(search != nullptr);
    QVERIFY(list != nullptr);
    QCOMPARE(list->dragDropMode(), QAbstractItemView::InternalMove);

    // 命令里的词、不同大小写都要命中；不匹配的行隐藏而不是删除。
    search->setText(QStringLiteral("NGINX"));
    QCOMPARE(list->count(), 2);
    QVERIFY(findSnippetItem(list, disk.id)->isHidden());
    QVERIFY(!findSnippetItem(list, nginx.id)->isHidden());
    QCOMPARE(list->dragDropMode(), QAbstractItemView::NoDragDrop);

    // 说明字段同样参与匹配。
    search->setText(QStringLiteral("web"));
    QVERIFY(findSnippetItem(list, disk.id)->isHidden());
    QVERIFY(!findSnippetItem(list, nginx.id)->isHidden());

    // 模型变化触发整表重建后，筛选必须继续生效。
    disk.command = QStringLiteral("df -h /");
    QVERIFY(m_model->saveSnippet(disk));
    QVERIFY(findSnippetItem(list, disk.id)->isHidden());

    search->clear();
    QVERIFY(!findSnippetItem(list, disk.id)->isHidden());
    QVERIFY(!findSnippetItem(list, nginx.id)->isHidden());
    QCOMPARE(list->dragDropMode(), QAbstractItemView::InternalMove);
}

void ProductivityTest::terminalPaneShowsAndExpiresTransientStatus()
{
    TerminalPane pane;
    auto *statusLabel = pane.findChild<QLabel *>(
        QStringLiteral("terminalSessionTransientStatus"));
    QVERIFY2(statusLabel != nullptr,
             "会话状态栏必须存在一个专门承载瞬时提示的 QLabel，紧邻服务器名之后");
    QVERIFY(statusLabel->text().isEmpty());

    pane.showTransientStatus(QStringLiteral("Reading home directory…"));
    QVERIFY2(statusLabel->text().contains(QStringLiteral("Reading home directory…")),
             "调用后必须立即显示传入的消息文字");

    // 到期后必须自动清空，不能永久占用状态栏空间——这条提示本质上是转瞬即逝的操作反馈。
    QTRY_VERIFY_WITH_TIMEOUT(statusLabel->text().isEmpty(), 9000);
}

void ProductivityTest::terminalPaneShowsIconForTransferResultAndClearsOnPlainStatus()
{
    TerminalPane pane;
    auto *statusLabel = pane.findChild<QLabel *>(
        QStringLiteral("terminalSessionTransientStatus"));
    auto *statusIcon = pane.findChild<QLabel *>(
        QStringLiteral("terminalSessionTransientStatusIcon"));
    QVERIFY(statusLabel != nullptr);
    QVERIFY2(statusIcon != nullptr,
             "会话状态栏必须存在一个紧贴瞬时提示前面的传输结果图标 QLabel");
    // pane 没有 show()，isVisible() 会因为顶层还没显示而恒为 false；必须用 isHidden()
    // 判断"是否被显式隐藏"，这与 setVisible(false/true) 的调用状态直接对应。
    QVERIFY2(statusIcon->isHidden() || statusIcon->pixmap().isNull(),
             "尚未展示任何传输结果之前不应该已经有图标");

    pane.showTransientTransferResult(true, QStringLiteral("Upload a.txt → /root"));
    QVERIFY2(statusLabel->text().contains(QStringLiteral("Upload a.txt → /root")),
             "传输结果文字必须原样展示");
    QVERIFY2(!statusIcon->isHidden() && !statusIcon->pixmap().isNull(),
             "传输成功后必须在文字前渲染一个非空图标");
    const QPixmap successPixmap = statusIcon->pixmap();

    pane.showTransientTransferResult(false, QStringLiteral("上传 b.txt 失败：连接被拒绝"));
    QVERIFY2(!statusIcon->isHidden() && !statusIcon->pixmap().isNull(),
             "传输失败同样必须展示图标，不能悄悄退化成纯文字");
    // 颜色不同，位图数据不应完全相同——否则用户无法用肉眼区分这次是成功还是失败。
    QVERIFY2(statusIcon->pixmap().toImage() != successPixmap.toImage(),
             "成功和失败必须使用不同的图标，而不是同一张图换个位置贴上去");

    // 普通提示（如目录读取进度）没有成功/失败语义，绝不能沿用上一次传输结果遗留的图标。
    pane.showTransientStatus(QStringLiteral("Reading home directory…"));
    QVERIFY2(statusIcon->pixmap().isNull() && statusIcon->isHidden(),
             "切换回普通提示后必须清空上一次传输结果的图标");
}

void ProductivityTest::terminalPaneDoesNotExposeInferredTargetBadge()
{
    TerminalPane pane;
    // 直接断言控件不存在而不是仅检查 hidden：隐藏控件未来可能被旧状态流重新设为可见，
    // 从对象树移除才能永久满足“终端会话不显示该提示”的界面契约。
    QVERIFY2(pane.findChild<QLabel *>(QStringLiteral("terminalSessionNestedBadge")) == nullptr,
             "终端状态栏不应创建“推测嵌套目标”徽章");
}

void ProductivityTest::snippetListIsFlatWithDescriptionFallbackAndShortcutHints()
{
    // 分组功能已移除：即使数据库里还残留旧的 Folder 节点（例如从旧版本升级而来），界面
    // 也只应该展示根层级的可执行片段，不能尝试渲染分组本身。
    SnippetRecord legacyFolder;
    legacyFolder.id = QStringLiteral("legacy-folder");
    legacyFolder.kind = SnippetNodeKind::Folder;
    legacyFolder.name = QStringLiteral("旧版分组");
    QVERIFY(m_model->saveSnippet(legacyFolder));

    SnippetRecord withDescription;
    withDescription.id = QStringLiteral("snippet-with-desc");
    withDescription.kind = SnippetNodeKind::Snippet;
    withDescription.name = QStringLiteral("查看磁盘占用");
    withDescription.command = QStringLiteral("df -h | sort -k5 -h -r");
    withDescription.description = QStringLiteral("按占用从高到低查看各分区使用率");
    withDescription.sortOrder = 0;
    QVERIFY(m_model->saveSnippet(withDescription));

    // 没有说明时必须退化显示命令本身，否则第二行会是空的，用户看不出这条片段做什么。
    SnippetRecord withoutDescription;
    withoutDescription.id = QStringLiteral("snippet-without-desc");
    withoutDescription.kind = SnippetNodeKind::Snippet;
    withoutDescription.name = QStringLiteral("重启 nginx");
    withoutDescription.command = QStringLiteral("systemctl restart nginx");
    withoutDescription.sortOrder = 1;
    QVERIFY(m_model->saveSnippet(withoutDescription));

    ProductivityWidget widget(m_model.get());
    QListWidget *list = widget.findChild<QListWidget *>(
        QStringLiteral("productivitySnippetList"));
    QVERIFY(list != nullptr);

    // 只有两个可执行片段会出现；分组节点被过滤掉。
    QCOMPARE(list->count(), 2);

    QListWidgetItem *withDescItem = findSnippetItem(list, withDescription.id);
    QVERIFY(withDescItem != nullptr);
    QWidget *withDescRow = list->itemWidget(withDescItem);
    QVERIFY(withDescRow != nullptr);
    bool foundDescriptionText = false;
    for (QLabel *label : withDescRow->findChildren<QLabel *>()) {
        if (label->text() == withDescription.description) {
            foundDescriptionText = true;
        }
    }
    QVERIFY2(foundDescriptionText, "有说明时第二行必须展示说明文字");

    QListWidgetItem *withoutDescItem = findSnippetItem(list, withoutDescription.id);
    QVERIFY(withoutDescItem != nullptr);
    QWidget *withoutDescRow = list->itemWidget(withoutDescItem);
    QVERIFY(withoutDescRow != nullptr);
    bool foundCommandFallback = false;
    for (QLabel *label : withoutDescRow->findChildren<QLabel *>()) {
        if (label->text() == withoutDescription.command) {
            foundCommandFallback = true;
        }
    }
    QVERIFY2(foundCommandFallback, "没有说明时第二行必须退化显示命令本身");

    // 每行都要有执行/复制按钮；前两行（第 1、2 个片段）的执行按钮要提示 Ctrl+1、Ctrl+2。
    for (QWidget *row : {withDescRow, withoutDescRow}) {
        QVERIFY(row->findChild<QToolButton *>(QStringLiteral("productivitySnippetExecute"))
                    != nullptr);
        QVERIFY(row->findChild<QToolButton *>(QStringLiteral("productivitySnippetCopy"))
                    != nullptr);
    }
    auto *firstExecute = withDescRow->findChild<QToolButton *>(
        QStringLiteral("productivitySnippetExecute"));
    auto *secondExecute = withoutDescRow->findChild<QToolButton *>(
        QStringLiteral("productivitySnippetExecute"));
    QVERIFY2(firstExecute->toolTip().contains(QStringLiteral("Ctrl+1")),
             "第一个片段的执行按钮必须提示 Ctrl+1");
    QVERIFY2(secondExecute->toolTip().contains(QStringLiteral("Ctrl+2")),
             "第二个片段的执行按钮必须提示 Ctrl+2");
}

void ProductivityTest::snippetEditorDialogSavesWithoutGroupingField()
{
    ProductivityWidget widget(m_model.get());

    auto *newSnippetButton = widget.findChild<QPushButton *>(
        QStringLiteral("productivityNewSnippet"));
    QVERIFY(newSnippetButton != nullptr);
    newSnippetButton->click();

    auto *dialog = widget.findChild<QDialog *>(QStringLiteral("productivitySnippetDialog"));
    QVERIFY2(dialog != nullptr, "新建片段必须弹出对话框，而不是就地展开表单");
    QVERIFY2(dialog->isVisible(), "点击新建后对话框必须立即可见");

    // 分组字段必须彻底消失：对话框里不应再有任何用于选择父分组的下拉框。
    QVERIFY(dialog->findChild<QComboBox *>() == nullptr);

    auto *nameEdit = dialog->findChild<QLineEdit *>(
        QStringLiteral("productivitySnippetNameEdit"));
    auto *commandEdit = dialog->findChild<QTextEdit *>(
        QStringLiteral("productivityCommandEdit"));
    auto *descriptionEdit = dialog->findChild<QLineEdit *>(
        QStringLiteral("productivitySnippetDescriptionEdit"));
    auto *saveButton = dialog->findChild<QPushButton *>(
        QStringLiteral("productivitySnippetSave"));
    QVERIFY(nameEdit != nullptr);
    QVERIFY(commandEdit != nullptr);
    QVERIFY(descriptionEdit != nullptr);
    QVERIFY(saveButton != nullptr);

    nameEdit->setText(QStringLiteral("查看磁盘"));
    commandEdit->setPlainText(QStringLiteral("df -hl"));
    descriptionEdit->setText(QStringLiteral("查看磁盘占用"));
    saveButton->click();

    QVERIFY2(!dialog->isVisible(), "保存成功后对话框必须关闭");

    bool ok = false;
    const QList<SnippetRecord> rootSnippets = m_model->snippetsInGroup(QString(), &ok);
    QVERIFY(ok);
    QCOMPARE(rootSnippets.size(), 1);
    QCOMPARE(rootSnippets.constFirst().name, QStringLiteral("查看磁盘"));
    QCOMPARE(rootSnippets.constFirst().command, QStringLiteral("df -hl"));
    QVERIFY(rootSnippets.constFirst().parentId.isEmpty());
}

void ProductivityTest::snippetShortcutExecutesFirstSnippetOnCurrentTerminal()
{
    SnippetRecord snippet;
    snippet.id = QStringLiteral("snippet-shortcut");
    snippet.kind = SnippetNodeKind::Snippet;
    snippet.name = QStringLiteral("查看内存");
    snippet.command = QStringLiteral("free -h");
    QVERIFY(m_model->saveSnippet(snippet));

    ProductivityWidget widget(m_model.get());
    widget.resize(360, 480);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));

    auto *list = widget.findChild<QListWidget *>(QStringLiteral("productivitySnippetList"));
    QVERIFY(list != nullptr);
    list->setFocus();
    QVERIFY(QTest::qWaitForWindowActive(&widget));

    QSignalSpy executionSpy(&widget, &ProductivityWidget::commandExecutionRequested);
    QTest::keyClick(&widget, Qt::Key_1, Qt::ControlModifier);
    QCOMPARE(executionSpy.count(), 1);
    QCOMPARE(executionSpy.constFirst().at(0).toString(), QStringLiteral("free -h"));
    QVERIFY(executionSpy.constFirst().at(1).toStringList().isEmpty());
}

void ProductivityTest::snippetRowCopyButtonWritesCommandToClipboard()
{
    SnippetRecord snippet;
    snippet.id = QStringLiteral("snippet-copy");
    snippet.kind = SnippetNodeKind::Snippet;
    snippet.name = QStringLiteral("查看进程");
    snippet.command = QStringLiteral("ps aux | grep sshd");
    QVERIFY(m_model->saveSnippet(snippet));

    ProductivityWidget widget(m_model.get());
    auto *list = widget.findChild<QListWidget *>(QStringLiteral("productivitySnippetList"));
    QVERIFY(list != nullptr);
    QListWidgetItem *item = findSnippetItem(list, snippet.id);
    QVERIFY(item != nullptr);
    auto *copyButton = list->itemWidget(item)->findChild<QToolButton *>(
        QStringLiteral("productivitySnippetCopy"));
    QVERIFY(copyButton != nullptr);

    QSignalSpy executionSpy(&widget, &ProductivityWidget::commandExecutionRequested);
    copyButton->click();
    QCOMPARE(executionSpy.count(), 0);
    QCOMPARE(QApplication::clipboard()->text(), snippet.command);
}

void ProductivityTest::reorderingSnippetListPersistsNewSortOrder()
{
    SnippetRecord first;
    first.id = QStringLiteral("snippet-first");
    first.kind = SnippetNodeKind::Snippet;
    first.name = QStringLiteral("第一条");
    first.command = QStringLiteral("echo first");
    first.sortOrder = 0;
    QVERIFY(m_model->saveSnippet(first));

    SnippetRecord second;
    second.id = QStringLiteral("snippet-second");
    second.kind = SnippetNodeKind::Snippet;
    second.name = QStringLiteral("第二条");
    second.command = QStringLiteral("echo second");
    second.sortOrder = 1;
    QVERIFY(m_model->saveSnippet(second));

    ProductivityWidget widget(m_model.get());
    auto *list = widget.findChild<QListWidget *>(QStringLiteral("productivitySnippetList"));
    QVERIFY(list != nullptr);
    QCOMPARE(list->dragDropMode(), QAbstractItemView::InternalMove);
    QCOMPARE(list->count(), 2);
    QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), first.id);
    QCOMPARE(list->item(1)->data(Qt::UserRole).toString(), second.id);

    // 模拟一次拖拽把第二条挪到最前面：QListWidget 内部拖拽最终也是先取出再插入。
    QListWidgetItem *movedItem = list->takeItem(1);
    QVERIFY(movedItem != nullptr);
    list->insertItem(0, movedItem);

    QVERIFY(QMetaObject::invokeMethod(&widget, "persistSnippetOrder", Qt::DirectConnection));

    const std::optional<SnippetRecord> updatedFirst = m_model->snippet(first.id);
    const std::optional<SnippetRecord> updatedSecond = m_model->snippet(second.id);
    QVERIFY(updatedFirst.has_value());
    QVERIFY(updatedSecond.has_value());
    QCOMPARE(updatedSecond->sortOrder, 0);
    QCOMPARE(updatedFirst->sortOrder, 1);

    // 保存触发了 reloadSnippets，列表应该按新的 sortOrder 重新排列。
    QCOMPARE(list->count(), 2);
    QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), second.id);
    QCOMPARE(list->item(1)->data(Qt::UserRole).toString(), first.id);
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

namespace {

/**
 * @brief 把任意源文案翻成「!!<原文>」的测试替身翻译器，只为触发 QEvent::LanguageChange。
 */
class MarkerTranslator final : public QTranslator {
public:
    QString translate(const char *context,
                      const char *sourceText,
                      const char *disambiguation = nullptr,
                      int n = -1) const override
    {
        Q_UNUSED(context)
        Q_UNUSED(disambiguation)
        Q_UNUSED(n)
        return QStringLiteral("!!") + QString::fromUtf8(sourceText);
    }

    bool isEmpty() const override { return false; }
};

} // namespace

void ProductivityTest::snippetRowsAndTabsRetranslateOnLanguageChange()
{
    SnippetRecord snippet;
    snippet.id = QStringLiteral("snippet-mem");
    snippet.kind = SnippetNodeKind::Snippet;
    snippet.name = QStringLiteral("内存占用");
    snippet.command = QStringLiteral("free -m");
    snippet.sortOrder = 0;
    QVERIFY(m_model->saveSnippet(snippet));

    ProductivityWidget widget(m_model.get());

    auto *tabs = widget.findChild<QTabWidget *>(QStringLiteral("productivityTabs"));
    QVERIFY(tabs != nullptr);
    QCOMPARE(tabs->tabText(0), QStringLiteral("Command snippets"));

    auto *list = widget.findChild<QListWidget *>(QStringLiteral("productivitySnippetList"));
    QVERIFY(list != nullptr);
    // 语言切换会整表重建，行控件指针随之失效，因此每次都按 id 重新定位当前行。
    // 行内按钮已是纯图标，语言只体现在 Tooltip 上。
    const auto buttonText = [&](const QString &objectName) {
        QListWidgetItem *item = findSnippetItem(list, snippet.id);
        return list->itemWidget(item)->findChild<QToolButton *>(objectName)->toolTip();
    };
    const auto executeText = [&]() {
        return buttonText(QStringLiteral("productivitySnippetExecute"));
    };
    const auto copyText = [&]() {
        return buttonText(QStringLiteral("productivitySnippetCopy"));
    };
    QCOMPARE(copyText(), QStringLiteral("Copy command"));
    auto *search = widget.findChild<QLineEdit *>(QStringLiteral("productivitySnippetSearch"));
    QVERIFY(search != nullptr);

    MarkerTranslator translator;
    QVERIFY(QCoreApplication::installTranslator(&translator));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LanguageChange);

    QCOMPARE(tabs->tabText(0), QStringLiteral("!!Command snippets"));
    QCOMPARE(copyText(), QStringLiteral("!!Copy command"));
    QCOMPARE(executeText(), QStringLiteral("!!Run (%1)").arg(QStringLiteral("Ctrl+1")));
    QVERIFY(search->placeholderText().startsWith(QStringLiteral("!!")));

    QVERIFY(QCoreApplication::removeTranslator(&translator));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LanguageChange);
    QCOMPARE(copyText(), QStringLiteral("Copy command"));
}

QTEST_MAIN(ProductivityTest)

#include "ProductivityTest.moc"
