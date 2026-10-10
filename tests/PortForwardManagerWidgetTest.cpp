#include "core/HostRecord.h"
#include "forwarding/PortForwardManagerWidget.h"
#include "forwarding/PortForwardRule.h"
#include "storage/ApplicationDatabase.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QStyle>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <memory>

/**
 * @brief 端口转发侧栏从四列表格改成卡片式列表后的界面回归。
 *
 * 表格→QListWidget 迁移改写了选中态读取、双击和右键菜单的底层 API；这里没有沿用旧行为
 * 的测试可以复用（迁移前完全没有这个控件的界面测试），因此只覆盖迁移动了手的部分：
 * 每条规则渲染成正确内容的卡片、id 角色能正确挂到列表项上、按主机切换正确重新加载。
 * 涉及模态对话框的按钮点击流程（编辑/启动确认等）不在本文件覆盖范围内。
 */
class PortForwardManagerWidgetTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    /** @brief 每条规则必须渲染成带稳定 id、名称、类型徽章和地址映射的卡片。 */
    void ruleListRendersEachRuleAsACardWithStableIdAndReadableContent();

    /** @brief 切换主机下拉框必须只显示该主机的规则，不残留上一台主机的卡片。 */
    void switchingHostReloadsRuleListForThatHostOnly();

    /** @brief 主机下拉弹层必须保留分组层级、禁止选择分组，并在主机名后显示规则数量。 */
    void hostSelectorUsesNonSelectableTreeGroupsAndDisplaysForwardingCounts();

    /** @brief 没有保存任何主机时，列表必须为空并提示去主机管理创建，而不是留白。 */
    void emptyStateShowsGuidanceWhenNoHostsSaved();

    /** @brief 运行时失败后卡片必须直接展示具体原因，不能只把错误藏在悬停提示里。 */
    void failedRuleCardDisplaysConcreteReason();

    /** @brief 工具栏只保留新建与靠右的紧凑批量启停按钮，不再常驻编辑和删除。 */
    void toolbarKeepsOnlyAddAndRightAlignedCompactRuntimeActions();

    /** @brief 异步状态刷新重建卡片列表后，必须按稳定规则 id 恢复用户选择。 */
    void runtimeRefreshPreservesSelectedRule();

    /** @brief selectHost 切到已保存主机；未知 id（本地 Shell、临时连接）不得冲掉当前选择。 */
    void selectHostSwitchesOnlyToKnownHosts();

    /** @brief 从主机右键发起的新建必须先切到那台主机，再弹出“新建端口转发”对话框。 */
    void createRuleForHostOpensNewRuleDialogForThatHost();

private:
    /** @brief 递归查找控件树中第一个显示指定文本的 QLabel。 */
    static QLabel *findLabelWithText(const QWidget *root, const QString &text);

    /**
     * @brief 按稳定 hostId 在树中递归查找节点。
     * @param tree 端口转发主机弹层中的树控件；不得为空。
     * @param hostId 目标主机 id；分组没有该数据，因此不会被误命中。
     * @return 找到的主机节点；不存在返回 nullptr。
     */
    static QTreeWidgetItem *findHostTreeItem(QTreeWidget *tree, const QString &hostId);

    std::unique_ptr<ApplicationDatabase> m_database;
};

void PortForwardManagerWidgetTest::init()
{
    m_database = std::make_unique<ApplicationDatabase>(ApplicationDatabase::inMemoryPath());
    QString error;
    QVERIFY2(m_database->initialize(&error), qPrintable(error));
}

void PortForwardManagerWidgetTest::cleanup()
{
    m_database.reset();
}

QLabel *PortForwardManagerWidgetTest::findLabelWithText(const QWidget *root, const QString &text)
{
    const QList<QLabel *> labels = root->findChildren<QLabel *>();
    for (QLabel *label : labels) {
        if (label->text() == text) {
            return label;
        }
    }
    return nullptr;
}

QTreeWidgetItem *PortForwardManagerWidgetTest::findHostTreeItem(QTreeWidget *tree,
                                                                const QString &hostId)
{
    Q_ASSERT(tree != nullptr);
    constexpr int kHostIdRole = Qt::UserRole + 32;
    QTreeWidgetItemIterator iterator(tree);
    while (*iterator != nullptr) {
        if ((*iterator)->data(0, kHostIdRole).toString() == hostId) {
            return *iterator;
        }
        ++iterator;
    }
    return nullptr;
}

void PortForwardManagerWidgetTest::ruleListRendersEachRuleAsACardWithStableIdAndReadableContent()
{
    HostRecord host;
    host.id = QStringLiteral("host-1");
    host.kind = HostNodeKind::Host;
    host.name = QStringLiteral("东西湖城运");
    host.host = QStringLiteral("10.70.165.50");
    QString error;
    QVERIFY2(m_database->saveHost(host, &error), qPrintable(error));

    PortForwardRule localRule;
    localRule.id = QStringLiteral("rule-local");
    localRule.hostId = host.id;
    localRule.name = QStringLiteral("本地管理后台");
    localRule.type = PortForwardType::Local;
    localRule.bindHost = QStringLiteral("127.0.0.1");
    localRule.bindPort = 8080;
    localRule.targetHost = QStringLiteral("10.70.165.50");
    localRule.targetPort = 80;
    QVERIFY2(m_database->savePortForward(localRule, &error), qPrintable(error));

    PortForwardRule dynamicRule;
    dynamicRule.id = QStringLiteral("rule-dynamic");
    dynamicRule.hostId = host.id;
    dynamicRule.name = QStringLiteral("SOCKS5 代理");
    dynamicRule.type = PortForwardType::Dynamic;
    dynamicRule.bindHost = QStringLiteral("127.0.0.1");
    dynamicRule.bindPort = 1080;
    QVERIFY2(m_database->savePortForward(dynamicRule, &error), qPrintable(error));

    PortForwardManagerWidget widget(m_database.get());
    auto *list = widget.findChild<QListWidget *>(QStringLiteral("PortForwardRuleList"));
    QVERIFY(list != nullptr);
    QCOMPARE(list->count(), 2);

    constexpr int kRuleIdRole = Qt::UserRole + 31;
    QSet<QString> seenIds;
    for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem *item = list->item(row);
        seenIds.insert(item->data(kRuleIdRole).toString());

        QWidget *rowWidget = list->itemWidget(item);
        QVERIFY2(rowWidget != nullptr, "每一行都必须挂一个自定义卡片控件，不能是裸文字项");
        QVERIFY2(rowWidget->testAttribute(Qt::WA_TransparentForMouseEvents),
                 "卡片内容必须透传鼠标事件，否则点击文字区域无法稳定选中列表项");
    }
    QCOMPARE(seenIds, QSet<QString>({localRule.id, dynamicRule.id}));

    // 卡片必须能读到名称和类型徽章文字，这是表格改单列列表后仍要保留的核心信息。
    QVERIFY2(findLabelWithText(list, localRule.name) != nullptr,
             "本地转发卡片必须显示规则名称");
    QVERIFY2(findLabelWithText(list, QStringLiteral("-L")) != nullptr,
             "本地转发的类型徽章必须显示 -L");
    QVERIFY2(findLabelWithText(list, dynamicRule.name) != nullptr,
             "动态转发卡片必须显示规则名称");
    QVERIFY2(findLabelWithText(list, QStringLiteral("-D")) != nullptr,
             "动态转发的类型徽章必须显示 -D");
}

void PortForwardManagerWidgetTest::switchingHostReloadsRuleListForThatHostOnly()
{
    HostRecord first;
    first.id = QStringLiteral("host-a");
    first.kind = HostNodeKind::Host;
    first.name = QStringLiteral("应用一");
    first.host = QStringLiteral("10.0.0.1");
    QString error;
    QVERIFY2(m_database->saveHost(first, &error), qPrintable(error));

    HostRecord second;
    second.id = QStringLiteral("host-b");
    second.kind = HostNodeKind::Host;
    second.name = QStringLiteral("应用二");
    second.host = QStringLiteral("10.0.0.2");
    QVERIFY2(m_database->saveHost(second, &error), qPrintable(error));

    PortForwardRule ruleA;
    ruleA.id = QStringLiteral("rule-a");
    ruleA.hostId = first.id;
    ruleA.name = QStringLiteral("规则 A");
    ruleA.bindPort = 3307;
    ruleA.targetHost = QStringLiteral("127.0.0.1");
    ruleA.targetPort = 3306;
    QVERIFY2(m_database->savePortForward(ruleA, &error), qPrintable(error));

    PortForwardRule ruleB;
    ruleB.id = QStringLiteral("rule-b");
    ruleB.hostId = second.id;
    ruleB.name = QStringLiteral("规则 B");
    ruleB.bindPort = 9000;
    ruleB.targetHost = QStringLiteral("127.0.0.1");
    ruleB.targetPort = 9000;
    QVERIFY2(m_database->savePortForward(ruleB, &error), qPrintable(error));

    PortForwardManagerWidget widget(m_database.get());
    auto *hostCombo = widget.findChild<QComboBox *>();
    auto *list = widget.findChild<QListWidget *>(QStringLiteral("PortForwardRuleList"));
    QVERIFY(hostCombo != nullptr);
    QVERIFY(list != nullptr);
    QCOMPARE(hostCombo->count(), 2);

    const int indexOfA = hostCombo->findData(first.id);
    QVERIFY(indexOfA >= 0);
    hostCombo->setCurrentIndex(indexOfA);
    QCOMPARE(list->count(), 1);
    QVERIFY2(findLabelWithText(list, ruleA.name) != nullptr, "切到主机 A 应看到规则 A");
    QVERIFY2(findLabelWithText(list, ruleB.name) == nullptr,
             "切到主机 A 不应残留主机 B 的规则卡片");

    const int indexOfB = hostCombo->findData(second.id);
    QVERIFY(indexOfB >= 0);
    hostCombo->setCurrentIndex(indexOfB);
    QCOMPARE(list->count(), 1);
    QVERIFY2(findLabelWithText(list, ruleB.name) != nullptr, "切到主机 B 应看到规则 B");
    QVERIFY2(findLabelWithText(list, ruleA.name) == nullptr,
             "切到主机 B 不应残留主机 A 的规则卡片");
}

void PortForwardManagerWidgetTest::hostSelectorUsesNonSelectableTreeGroupsAndDisplaysForwardingCounts()
{
    HostRecord productionGroup;
    productionGroup.id = QStringLiteral("group-production");
    productionGroup.kind = HostNodeKind::Folder;
    productionGroup.name = QStringLiteral("生产环境");
    QString error;
    QVERIFY2(m_database->saveHost(productionGroup, &error), qPrintable(error));

    HostRecord databaseGroup;
    databaseGroup.id = QStringLiteral("group-database");
    databaseGroup.parentId = productionGroup.id;
    databaseGroup.kind = HostNodeKind::Folder;
    databaseGroup.name = QStringLiteral("数据库");
    QVERIFY2(m_database->saveHost(databaseGroup, &error), qPrintable(error));

    HostRecord nestedHost;
    nestedHost.id = QStringLiteral("host-nested");
    nestedHost.parentId = databaseGroup.id;
    nestedHost.kind = HostNodeKind::Host;
    nestedHost.name = QStringLiteral("核心 MySQL");
    nestedHost.host = QStringLiteral("10.0.0.10");
    QVERIFY2(m_database->saveHost(nestedHost, &error), qPrintable(error));

    HostRecord rootHost;
    rootHost.id = QStringLiteral("host-root");
    rootHost.kind = HostNodeKind::Host;
    rootHost.name = QStringLiteral("独立跳板机");
    rootHost.host = QStringLiteral("10.0.0.20");
    QVERIFY2(m_database->saveHost(rootHost, &error), qPrintable(error));

    for (int index = 0; index < 2; ++index) {
        PortForwardRule rule;
        rule.id = QStringLiteral("nested-rule-%1").arg(index);
        rule.hostId = nestedHost.id;
        rule.name = QStringLiteral("数据库规则 %1").arg(index + 1);
        rule.type = PortForwardType::Local;
        rule.bindHost = QStringLiteral("127.0.0.1");
        rule.bindPort = static_cast<quint16>(3307 + index);
        rule.targetHost = QStringLiteral("127.0.0.1");
        rule.targetPort = 3306;
        QVERIFY2(m_database->savePortForward(rule, &error), qPrintable(error));
    }

    PortForwardManagerWidget widget(m_database.get());
    auto *hostCombo = widget.findChild<QComboBox *>(QStringLiteral("PortForwardHostCombo"));
    auto *hostTree = widget.findChild<QTreeWidget *>(QStringLiteral("PortForwardHostTree"));
    QVERIFY2(hostCombo != nullptr, "端口转发页必须保留 QComboBox 兼容接口");
    QVERIFY2(hostTree != nullptr, "主机下拉框必须提供树形弹层，不能继续使用扁平列表");
    QCOMPARE(hostCombo->count(), 2);

    auto *hostPopup = widget.findChild<QFrame *>(QStringLiteral("PortForwardHostPopup"));
    QVERIFY2(hostPopup != nullptr, "树形列表必须由独立弹层承载，以便绘制完整外边框");
    QVERIFY2(!hostPopup->testAttribute(Qt::WA_TranslucentBackground),
             "主机弹层不得透明，否则会与主界面文字重叠并造成视觉模糊");
    QVERIFY2(hostPopup->autoFillBackground(),
             "主机弹层必须主动填充实色背景，不能依赖平台透明窗口的默认行为");
    QVERIFY2(hostTree->autoFillBackground() && hostTree->viewport()->autoFillBackground(),
             "树与滚动视口都必须填充背景，避免只有边框不透明而内容区继续透底");

    // 两层 parent 关系必须原样反映在弹层中；这比只检查文本更能防止后续重构再次拍平主机。
    QTreeWidgetItem *nestedItem = findHostTreeItem(hostTree, nestedHost.id);
    QVERIFY(nestedItem != nullptr);
    QVERIFY(nestedItem->parent() != nullptr);
    QCOMPARE(nestedItem->parent()->text(0), databaseGroup.name);
    QVERIFY(nestedItem->parent()->parent() != nullptr);
    QCOMPARE(nestedItem->parent()->parent()->text(0), productionGroup.name);
    QCOMPARE(nestedItem->text(0), nestedHost.name);
    constexpr int kForwardingCountRole = Qt::UserRole + 34;
    QCOMPARE(nestedItem->data(0, kForwardingCountRole).toInt(), 2);

    QTreeWidgetItem *rootItem = findHostTreeItem(hostTree, rootHost.id);
    QVERIFY(rootItem != nullptr);
    QVERIFY(rootItem->parent() == nullptr);
    QCOMPARE(rootItem->text(0), rootHost.name);
    QCOMPARE(rootItem->data(0, kForwardingCountRole).toInt(), 0);

    constexpr int kHostIdRole = Qt::UserRole + 32;
    QTreeWidgetItem *topGroupItem = nestedItem->parent()->parent();
    QVERIFY2(!(topGroupItem->flags() & Qt::ItemIsSelectable),
             "分组节点必须移除 ItemIsSelectable，鼠标与键盘都不能把规则绑定到分组");
    QVERIFY2(topGroupItem->data(0, kHostIdRole).toString().isEmpty(),
             "分组不得携带可被业务层误读为主机的 hostId");

    const int nestedComboIndex = hostCombo->findData(nestedHost.id);
    QVERIFY(nestedComboIndex >= 0);
    QCOMPARE(hostCombo->itemText(nestedComboIndex), QStringLiteral("核心 MySQL  (2)"));
    const int rootComboIndex = hostCombo->findData(rootHost.id);
    QVERIFY(rootComboIndex >= 0);
    QCOMPARE(hostCombo->itemText(rootComboIndex), rootHost.name);

    // 与主机管理一样关闭平台分支装饰，并使用单列委托统一绘制箭头、层级线和圆角选中态。
    QVERIFY(!hostTree->rootIsDecorated());
    QCOMPARE(hostTree->columnCount(), 1);
    QCOMPARE(hostTree->indentation(), 32);
    QCOMPARE(QString::fromLatin1(hostTree->itemDelegate()->metaObject()->className()),
             QStringLiteral("QStyledItemDelegate"));
}

void PortForwardManagerWidgetTest::emptyStateShowsGuidanceWhenNoHostsSaved()
{
    PortForwardManagerWidget widget(m_database.get());
    auto *list = widget.findChild<QListWidget *>(QStringLiteral("PortForwardRuleList"));
    QVERIFY(list != nullptr);
    QCOMPARE(list->count(), 0);
    QVERIFY2(findLabelWithText(&widget, QStringLiteral("Save a host in Host Manager first"))
                 != nullptr,
             "没有保存主机时必须提示去主机管理创建，而不是留一片空白列表");
}

void PortForwardManagerWidgetTest::failedRuleCardDisplaysConcreteReason()
{
    HostRecord host;
    host.id = QStringLiteral("failed-host");
    host.kind = HostNodeKind::Host;
    host.name = QStringLiteral("故障测试主机");
    host.host = QStringLiteral("10.0.0.8");
    QString databaseError;
    QVERIFY2(m_database->saveHost(host, &databaseError), qPrintable(databaseError));

    PortForwardRule rule;
    rule.id = QStringLiteral("failed-rule");
    rule.hostId = host.id;
    rule.name = QStringLiteral("占用端口测试");
    rule.type = PortForwardType::Local;
    rule.bindHost = QStringLiteral("127.0.0.1");
    rule.bindPort = 8080;
    rule.targetHost = QStringLiteral("10.0.0.9");
    rule.targetPort = 80;
    QVERIFY2(m_database->savePortForward(rule, &databaseError), qPrintable(databaseError));

    PortForwardManagerWidget widget(m_database.get());
    PortForwardStatistics failedStatistics;
    failedStatistics.state = PortForwardState::Failed;
    // 换行模拟系统/SSH 返回的多段错误；界面应合并为空格，避免把卡片布局拆成任意多行。
    failedStatistics.lastError =
        QStringLiteral("Failed to listen on 127.0.0.1:8080: Address already in use\nchange the port");
    QVERIFY2(QMetaObject::invokeMethod(&widget,
                                       "applyRuntimeStatistics",
                                       Qt::DirectConnection,
                                       Q_ARG(QString, rule.id),
                                       Q_ARG(PortForwardStatistics, failedStatistics)),
             "测试无法投递运行时统计，私有槽签名可能已与生产连接不一致");

    auto *list = widget.findChild<QListWidget *>(QStringLiteral("PortForwardRuleList"));
    QVERIFY(list != nullptr);
    auto *errorLabel = list->findChild<QLabel *>(QStringLiteral("PortForwardErrorLabel"));
    QVERIFY2(errorLabel != nullptr, "失败卡片必须创建直接可见的原因标签");
    QCOMPARE(errorLabel->text(),
             QStringLiteral("Reason: Failed to listen on 127.0.0.1:8080: Address already in use change the port"));
    QVERIFY2(errorLabel->wordWrap(), "较长失败原因必须允许换行，不能在窄侧栏中被直接裁掉");
    // 设计稿只在确有活动连接时追加数量；失败态的零连接没有决策价值，显示“· 0”反而会
    // 干扰用户优先阅读失败原因。因此这里验证简化后的状态文本，同时继续要求原因可见。
    QVERIFY2(findLabelWithText(list, QStringLiteral("Failed")) != nullptr,
             "展示失败原因时不能丢失原有状态");
}

void PortForwardManagerWidgetTest::toolbarKeepsOnlyAddAndRightAlignedCompactRuntimeActions()
{
    PortForwardManagerWidget widget(m_database.get());
    widget.resize(260, 600);

    auto *addButton = widget.findChild<QPushButton *>(QStringLiteral("AddPortForwardButton"));
    auto *startButton = widget.findChild<QPushButton *>(
        QStringLiteral("StartAllPortForwardsButton"));
    auto *stopButton = widget.findChild<QPushButton *>(
        QStringLiteral("StopAllPortForwardsButton"));
    auto *ruleList = widget.findChild<QListWidget *>(QStringLiteral("PortForwardRuleList"));
    QVERIFY(addButton != nullptr);
    QVERIFY2(widget.findChild<QWidget *>(QStringLiteral("EditPortForwardButton")) == nullptr,
             "新建按钮右侧不应继续常驻编辑按钮");
    QVERIFY2(widget.findChild<QWidget *>(QStringLiteral("DeletePortForwardButton")) == nullptr,
             "新建按钮右侧不应继续常驻删除按钮");
    QVERIFY(startButton != nullptr);
    QVERIFY(stopButton != nullptr);
    QVERIFY(ruleList != nullptr);

    // 四个可见入口必须处于同一布局，且伸缩项位于新建和启停之间，由此保证启停靠右。
    auto *managementLayout = widget.findChild<QHBoxLayout *>(
        QStringLiteral("PortForwardManagementLayout"));
    QVERIFY(managementLayout != nullptr);
    QVERIFY(managementLayout->indexOf(addButton) >= 0);
    QVERIFY(managementLayout->indexOf(startButton) > managementLayout->indexOf(addButton));
    QVERIFY(managementLayout->indexOf(stopButton) > managementLayout->indexOf(startButton));
    QCOMPARE(startButton->width(), 66);
    QCOMPARE(stopButton->width(), 66);
}

void PortForwardManagerWidgetTest::runtimeRefreshPreservesSelectedRule()
{
    HostRecord host;
    host.id = QStringLiteral("selection-host");
    host.kind = HostNodeKind::Host;
    host.name = QStringLiteral("选择测试主机");
    host.host = QStringLiteral("10.0.0.12");
    QString error;
    QVERIFY2(m_database->saveHost(host, &error), qPrintable(error));

    for (int index = 0; index < 2; ++index) {
        PortForwardRule rule;
        rule.id = QStringLiteral("selection-rule-%1").arg(index);
        rule.hostId = host.id;
        rule.name = QStringLiteral("选择规则 %1").arg(index + 1);
        rule.bindPort = static_cast<quint16>(8100 + index);
        rule.targetHost = QStringLiteral("127.0.0.1");
        rule.targetPort = 80;
        QVERIFY2(m_database->savePortForward(rule, &error), qPrintable(error));
    }

    PortForwardManagerWidget widget(m_database.get());
    auto *list = widget.findChild<QListWidget *>(QStringLiteral("PortForwardRuleList"));
    QVERIFY(list != nullptr);
    QCOMPARE(list->count(), 2);
    constexpr int kRuleIdRole = Qt::UserRole + 31;
    QListWidgetItem *selectedItem = list->item(1);
    const QString selectedRuleId = selectedItem->data(kRuleIdRole).toString();
    list->setCurrentItem(selectedItem, QItemSelectionModel::ClearAndSelect);
    QVERIFY(selectedItem->isSelected());

    PortForwardStatistics statistics;
    statistics.state = PortForwardState::Starting;
    QVERIFY2(QMetaObject::invokeMethod(&widget,
                                       "applyRuntimeStatistics",
                                       Qt::DirectConnection,
                                       Q_ARG(QString, selectedRuleId),
                                       Q_ARG(PortForwardStatistics, statistics)),
             "测试无法触发模拟异步状态刷新");

    QVERIFY2(list->currentItem() != nullptr, "状态刷新后当前选择不得变为空");
    QCOMPARE(list->currentItem()->data(kRuleIdRole).toString(), selectedRuleId);
    QVERIFY2(list->currentItem()->isSelected(), "恢复 currentItem 时也必须恢复实际选中标记");
}

void PortForwardManagerWidgetTest::selectHostSwitchesOnlyToKnownHosts()
{
    QString error;
    for (const QString &id : {QStringLiteral("select-a"), QStringLiteral("select-b")}) {
        HostRecord host;
        host.id = id;
        host.kind = HostNodeKind::Host;
        host.name = id;
        host.host = QStringLiteral("10.0.0.9");
        QVERIFY2(m_database->saveHost(host, &error), qPrintable(error));
    }

    PortForwardManagerWidget widget(m_database.get());
    auto *hostCombo = widget.findChild<QComboBox *>();
    QVERIFY(hostCombo != nullptr);
    hostCombo->setCurrentIndex(hostCombo->findData(QStringLiteral("select-a")));

    QVERIFY(widget.selectHost(QStringLiteral("select-b")));
    QCOMPARE(hostCombo->currentData().toString(), QStringLiteral("select-b"));

    QVERIFY(!widget.selectHost(QStringLiteral("temporary-connection-uuid")));
    QVERIFY(!widget.selectHost(QString()));
    QCOMPARE(hostCombo->currentData().toString(), QStringLiteral("select-b"));
}

void PortForwardManagerWidgetTest::createRuleForHostOpensNewRuleDialogForThatHost()
{
    QString error;
    for (const QString &id : {QStringLiteral("create-a"), QStringLiteral("create-b")}) {
        HostRecord host;
        host.id = id;
        host.kind = HostNodeKind::Host;
        host.name = id;
        host.host = QStringLiteral("10.0.0.10");
        QVERIFY2(m_database->saveHost(host, &error), qPrintable(error));
    }

    PortForwardManagerWidget widget(m_database.get());
    auto *hostCombo = widget.findChild<QComboBox *>();
    QVERIFY(hostCombo != nullptr);
    hostCombo->setCurrentIndex(hostCombo->findData(QStringLiteral("create-a")));

    // 对话框是模态 exec()；在事件循环里记下标题与当时的主机选择后取消，避免测试阻塞。
    QString dialogTitle;
    QString hostWhileDialogOpen;
    QTimer::singleShot(0, &widget, [&]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        dialogTitle = dialog->windowTitle();
        hostWhileDialogOpen = hostCombo->currentData().toString();
        dialog->reject();
    });
    widget.createRuleForHost(QStringLiteral("create-b"));

    QCOMPARE(dialogTitle, QStringLiteral("New port forwarding"));
    QCOMPARE(hostWhileDialogOpen, QStringLiteral("create-b"));
    QCOMPARE(hostCombo->currentData().toString(), QStringLiteral("create-b"));
}

QTEST_MAIN(PortForwardManagerWidgetTest)

#include "PortForwardManagerWidgetTest.moc"
