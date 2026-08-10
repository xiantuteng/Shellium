#include "core/HostRecord.h"
#include "forwarding/PortForwardManagerWidget.h"
#include "forwarding/PortForwardRule.h"
#include "storage/ApplicationDatabase.h"

#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QSet>
#include <QTest>

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

    /** @brief 没有保存任何主机时，列表必须为空并提示去主机管理创建，而不是留白。 */
    void emptyStateShowsGuidanceWhenNoHostsSaved();

private:
    /** @brief 递归查找控件树中第一个显示指定文本的 QLabel。 */
    static QLabel *findLabelWithText(const QWidget *root, const QString &text);

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

void PortForwardManagerWidgetTest::emptyStateShowsGuidanceWhenNoHostsSaved()
{
    PortForwardManagerWidget widget(m_database.get());
    auto *list = widget.findChild<QListWidget *>(QStringLiteral("PortForwardRuleList"));
    QVERIFY(list != nullptr);
    QCOMPARE(list->count(), 0);
    QVERIFY2(findLabelWithText(&widget, QStringLiteral("请先在主机管理中保存一台主机"))
                 != nullptr,
             "没有保存主机时必须提示去主机管理创建，而不是留一片空白列表");
}

QTEST_MAIN(PortForwardManagerWidgetTest)

#include "PortForwardManagerWidgetTest.moc"
