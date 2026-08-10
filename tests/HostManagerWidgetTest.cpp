#include "core/HostRecord.h"
#include "security/Vault.h"
#include "storage/ApplicationDatabase.h"
#include "ui/HostManagerWidget.h"
#include "ui/UiIcons.h"

#include <QAbstractButton>
#include <QApplication>
#include <QColor>
#include <QFileIconProvider>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <algorithm>
#include <memory>

/**
 * @brief 主机侧栏的展示与动作布局回归。
 *
 * 全部用例跑在内存数据库上，绝不触碰用户真实数据文件。
 */
class HostManagerWidgetTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    /** @brief 主机副标题必须补齐从分组继承来的账号与端口，否则看不出实际连去哪里。 */
    void hostSubtitleShowsInheritedAccountHostAndPort();

    /** @brief 分组没有连接信息，不得被画成两行。 */
    void groupHasNoSubtitle();

    /** @brief 侧栏标题行只暴露新建主机、新建分组与刷新三个动作。 */
    void headerExposesOnlyThreePrimaryActions();

    /** @brief 分组配置窗口只能保留名称字段，且页脚按钮必须带语义图标。 */
    void groupDialogHasNoStrayWidgets();

    /** @brief 主机配置字段顺序、移除项、左下角测试按钮和按钮图标必须符合新版交互。 */
    void hostDialogUsesRequestedFieldOrderAndIcons();

    /** @brief 名称为空时应根据账号、地址和端口自动命名，且不得覆盖人工名称。 */
    void emptyHostNameFollowsConnectionFieldsUntilManuallyEdited();

    /** @brief 顶层节点顶格、分组下的主机右缩进，二者不能再被分支装饰列推乱。 */
    void topLevelNodesShareLeftEdgeAndChildHostsIndent();

    /** @brief 展开箭头必须右对齐，且整个右侧图标区域都能展开、收起。 */
    void groupToggleIsRightAlignedVerticallyCenteredAndClickable();

    /** @brief 分组名与主机名必须使用同一套常规字重字体，不得以粗体争抢视觉注意力。 */
    void groupNameUsesSameFontAsHostName();

    /** @brief 同一个自绘图标实例必须在主题切换后改用新前景色，显式品牌色则保持不变。 */
    void semanticIconsFollowLightAndDarkPalettes();

    /** @brief 无父分组的主机必须排在全部顶层分组之后，并由一个不可选中的"未分组"标题隔开。 */
    void ungroupedHostsFollowFoldersBehindSectionHeader();

    /** @brief 没有任何无父分组主机时，树里不应出现"未分组"标题这个多余的空壳分区。 */
    void sectionHeaderAbsentWhenNoUngroupedHosts();

private:
    [[nodiscard]] QTreeWidgetItem *findItem(const HostManagerWidget &widget,
                                            const QString &name) const;

    std::unique_ptr<ApplicationDatabase> m_database;
    std::unique_ptr<CredentialVault> m_vault;
};

void HostManagerWidgetTest::init()
{
    // 内存数据库保证每个用例都从空库开始，也不会触碰用户真实数据文件。
    m_database = std::make_unique<ApplicationDatabase>(ApplicationDatabase::inMemoryPath());
    QString error;
    QVERIFY2(m_database->initialize(&error), qPrintable(error));
    m_vault = std::make_unique<CredentialVault>(*m_database);
}

void HostManagerWidgetTest::cleanup()
{
    m_vault.reset();
    m_database.reset();
}

QTreeWidgetItem *HostManagerWidgetTest::findItem(const HostManagerWidget &widget,
                                                 const QString &name) const
{
    auto *tree = widget.findChild<QTreeWidget *>();
    if (tree == nullptr) {
        return nullptr;
    }
    const QList<QTreeWidgetItem *> matches =
        tree->findItems(name, Qt::MatchExactly | Qt::MatchRecursive);
    return matches.isEmpty() ? nullptr : matches.constFirst();
}

void HostManagerWidgetTest::hostSubtitleShowsInheritedAccountHostAndPort()
{
    HostRecord group;
    group.id = QStringLiteral("group-1");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("武汉");
    group.username = QStringLiteral("root");
    group.port = 2222;
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    // 主机自身刻意不设账号和端口：空值代表"继承分组"，而不是"没有账号"。
    HostRecord host;
    host.id = QStringLiteral("host-1");
    host.parentId = group.id;
    host.kind = HostNodeKind::Host;
    host.name = QStringLiteral("市民热线预发布");
    host.host = QStringLiteral("10.194.135.11");
    QVERIFY2(m_database->saveHost(host, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    QTreeWidgetItem *item = findItem(widget, host.name);
    QVERIFY2(item != nullptr, "主机节点未出现在树中");

    const QString subtitle = item->data(0, Qt::UserRole + 2).toString();
    QCOMPARE(subtitle, QStringLiteral("root@10.194.135.11:2222"));
    // 悬停提示与副标题必须一致，避免两处各自拼一份地址后逐渐讲出不同的故事。
    QCOMPARE(item->toolTip(0), subtitle);
}

void HostManagerWidgetTest::groupHasNoSubtitle()
{
    HostRecord group;
    group.id = QStringLiteral("group-2");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("江夏镇");
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    QTreeWidgetItem *item = findItem(widget, group.name);
    QVERIFY2(item != nullptr, "分组节点未出现在树中");
    QVERIFY(item->data(0, Qt::UserRole + 2).toString().isEmpty());

    // 主机分组应与 QFileSystemModel 驱动的“本地文件”列表使用同源系统文件夹图标。直接
    // 比较最终位图而不是图标 cacheKey：不同 QIcon 实例可能绘制完全相同，但缓存键不同。
    QFileIconProvider iconProvider;
    QIcon expectedIcon = iconProvider.icon(QFileIconProvider::Folder);
    if (expectedIcon.isNull()) {
        expectedIcon = UiIcons::symbol(
            &widget,
            UiIcons::Symbol::Folder,
            widget.palette().color(QPalette::WindowText));
    }
    const QSize comparisonSize(20, 20);
    QCOMPARE(item->icon(0).pixmap(comparisonSize).toImage(),
             expectedIcon.pixmap(comparisonSize).toImage());
}

void HostManagerWidgetTest::headerExposesOnlyThreePrimaryActions()
{
    HostManagerWidget widget(m_database.get(), m_vault.get());
    QWidget *header = widget.headerActionsWidget();
    QVERIFY(header != nullptr);

    QStringList labels;
    const QList<QAbstractButton *> buttons = header->findChildren<QAbstractButton *>();
    for (const QAbstractButton *button : buttons) {
        labels.append(button->text());
    }
    std::sort(labels.begin(), labels.end());
    QCOMPARE(labels,
             (QStringList{QStringLiteral("刷新"),
                          QStringLiteral("新建主机"),
                          QStringLiteral("新建分组")}));

    // 导入/导出已移入树的右键菜单，不应再占用侧栏顶部的常驻位置。
    QVERIFY(!labels.contains(QStringLiteral("导入")));
    QVERIFY(!labels.contains(QStringLiteral("导出")));

    // "最近连接"整块已移除，面板里不应再残留任何列表控件。
    QVERIFY(widget.findChild<QListWidget *>() == nullptr);
}

namespace {

/** @brief 递归收集某个布局（含嵌套子布局）直接管理的全部控件。 */
void collectLayoutWidgets(QLayout *layout, QSet<QWidget *> *managed)
{
    if (layout == nullptr) {
        return;
    }
    for (int index = 0; index < layout->count(); ++index) {
        QLayoutItem *item = layout->itemAt(index);
        if (QWidget *widget = item->widget()) {
            managed->insert(widget);
        }
        collectLayoutWidgets(item->layout(), managed);
    }
}

} // namespace

void HostManagerWidgetTest::groupDialogHasNoStrayWidgets()
{
    HostManagerWidget widget(m_database.get(), m_vault.get());

    // 编辑对话框是 HostManagerWidget.cpp 内部类型，只能经"新建分组"动作弹出；它是模态的，
    // 所以在事件循环里取当前模态窗口来检查，再关掉它让 addGroup() 返回。
    QAbstractButton *addGroupButton = nullptr;
    for (QAbstractButton *button : widget.headerActionsWidget()->findChildren<QAbstractButton *>()) {
        if (button->text() == QStringLiteral("新建分组")) {
            addGroupButton = button;
        }
    }
    QVERIFY2(addGroupButton != nullptr, "侧栏没有新建分组按钮");

    QString failure;
    QTimer::singleShot(0, [&failure]() {
        QWidget *dialog = QApplication::activeModalWidget();
        if (dialog == nullptr) {
            failure = QStringLiteral("分组配置对话框未能弹出");
            return;
        }
        QSet<QWidget *> managed;
        collectLayoutWidgets(dialog->layout(), &managed);
        // QFocusFrame（macOS 焦点环）和 QSizeGrip 由 Qt 自行创建并围绕目标控件浮动定位，
        // 本就不该进入布局；把它们计入会让本用例随焦点时序随机失败。
        static const QSet<QString> qtFloatingHelpers = {
            QStringLiteral("QFocusFrame"), QStringLiteral("QSizeGrip")};
        for (QWidget *child : dialog->findChildren<QWidget *>(Qt::FindDirectChildrenOnly)) {
            const QString className = QString::fromLatin1(child->metaObject()->className());
            // 显式隐藏的控件不绘制、也不参与 Tab 顺序，是可接受的"备用控件"形态。
            if (child->isHidden() || managed.contains(child)
                || qtFloatingHelpers.contains(className)) {
                continue;
            }
            failure = QStringLiteral("控件 %1 可见但不在任何布局中，会浮在分组配置窗口左上角")
                          .arg(className);
            break;
        }

        // “只保留分组名称”不仅意味着其他字段不可见，也意味着它们不应继续以隐藏输入框
        // 潜伏在对象树中；否则辅助技术和后续维护仍可能误把它们当成可用配置项。
        const QList<QLineEdit *> lineEdits = dialog->findChildren<QLineEdit *>();
        if (failure.isEmpty()
            && (lineEdits.size() != 1
                || lineEdits.constFirst()->objectName()
                    != QStringLiteral("hostRecordNameEdit"))) {
            failure = QStringLiteral("分组配置窗口没有严格只保留名称输入框");
        }

        // 圆角和卡片底色由 QFrame#SettingsCard 样式选择器提供；业务代码若为了定位控件
        // 覆盖 objectName，字段虽然还在，承载它的圆角区域却会在视觉上完全消失。
        const QList<QWidget *> cards = dialog->findChildren<QWidget *>(
            QStringLiteral("SettingsCard"));
        if (failure.isEmpty() && cards.size() != 1) {
            failure = QStringLiteral("分组名称外层的圆角卡片区域未保留");
        }

        const QList<QPushButton *> buttons = dialog->findChildren<QPushButton *>();
        if (failure.isEmpty() && buttons.size() != 2) {
            failure = QStringLiteral("分组配置窗口应只保留取消和保存两个按钮");
        }
        for (const QPushButton *button : buttons) {
            if (failure.isEmpty() && button->icon().isNull()) {
                failure = QStringLiteral("分组配置按钮“%1”缺少图标").arg(button->text());
            }
        }
        dialog->close();
    });

    // 分组窗口只创建名称和页脚动作，任何主机地址、认证或跳板控件都不应存在。
    addGroupButton->click();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
}

void HostManagerWidgetTest::hostDialogUsesRequestedFieldOrderAndIcons()
{
    HostManagerWidget widget(m_database.get(), m_vault.get());

    QAbstractButton *addHostButton = nullptr;
    for (QAbstractButton *button : widget.headerActionsWidget()->findChildren<QAbstractButton *>()) {
        if (button->text() == QStringLiteral("新建主机")) {
            addHostButton = button;
            break;
        }
    }
    QVERIFY2(addHostButton != nullptr, "侧栏没有新建主机按钮");

    QString failure;
    QTimer::singleShot(0, [&failure]() {
        QWidget *dialog = QApplication::activeModalWidget();
        if (dialog == nullptr) {
            failure = QStringLiteral("主机配置对话框未能弹出");
            return;
        }

        QStringList labelTexts;
        for (const QLabel *label : dialog->findChildren<QLabel *>()) {
            labelTexts.append(label->text());
        }
        // 已移除项必须连标签都不存在；只把控件藏起来仍会让可访问性树和布局保留旧语义。
        for (const QString &removedLabel : {QStringLiteral("继承父分组端口"),
                                            QStringLiteral("标签"),
                                            QStringLiteral("会话密码/口令")}) {
            if (labelTexts.contains(removedLabel)) {
                failure = QStringLiteral("主机配置仍包含已移除字段“%1”").arg(removedLabel);
                break;
            }
        }

        const QList<QWidget *> cards = dialog->findChildren<QWidget *>(
            QStringLiteral("SettingsCard"));
        if (failure.isEmpty() && cards.size() != 2) {
            failure = QStringLiteral("主机基础信息和认证信息的圆角卡片区域未完整保留");
        }

        const auto *username = dialog->findChild<QLineEdit *>(
            QStringLiteral("hostUsernameEdit"));
        const auto *password = dialog->findChild<QLineEdit *>(
            QStringLiteral("hostPasswordEdit"));
        const auto *identity = dialog->findChild<QLineEdit *>(
            QStringLiteral("hostIdentityEdit"));
        const auto *proxyJumps = dialog->findChild<QLineEdit *>(
            QStringLiteral("hostProxyJumpsEdit"));
        if (failure.isEmpty()
            && (username == nullptr || password == nullptr || identity == nullptr
                || proxyJumps == nullptr)) {
            failure = QStringLiteral("主机配置缺少认证与跳板字段");
        }
        if (failure.isEmpty()) {
            const int usernameTop = username->mapTo(dialog, QPoint{}).y();
            const int passwordTop = password->mapTo(dialog, QPoint{}).y();
            const int identityTop = identity->mapTo(dialog, QPoint{}).y();
            const int proxyTop = proxyJumps->mapTo(dialog, QPoint{}).y();
            if (!(usernameTop < passwordTop && passwordTop < identityTop
                  && identityTop < proxyTop)) {
                failure = QStringLiteral(
                    "认证字段未按用户名、密码、私钥文件、ProxyJump 的顺序排列");
            }
        }

        const auto *testButton = dialog->findChild<QPushButton *>(
            QStringLiteral("hostTestConnectionButton"));
        const auto *cancelButton = dialog->findChild<QPushButton *>(
            QStringLiteral("hostRecordCancelButton"));
        const auto *saveButton = dialog->findChild<QPushButton *>(
            QStringLiteral("hostRecordSaveButton"));
        if (failure.isEmpty()
            && (testButton == nullptr || cancelButton == nullptr || saveButton == nullptr)) {
            failure = QStringLiteral("主机配置页脚缺少测试连接、取消或保存按钮");
        }
        if (failure.isEmpty()
            && testButton->mapTo(dialog, QPoint{}).x()
                >= cancelButton->mapTo(dialog, QPoint{}).x()) {
            failure = QStringLiteral("测试连接按钮没有位于页脚左侧");
        }

        // 这里检查所有可见文字按钮，覆盖浏览、测试连接、取消与保存；步进器的加减号本身
        // 就是约定俗成的图形符号，不属于需要额外前置图标的文字按钮。
        for (const QPushButton *button : dialog->findChildren<QPushButton *>()) {
            if (failure.isEmpty() && button->isVisible() && button->icon().isNull()) {
                failure = QStringLiteral("主机配置按钮“%1”缺少图标").arg(button->text());
            }
        }
        dialog->close();
    });

    addHostButton->click();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
}

void HostManagerWidgetTest::emptyHostNameFollowsConnectionFieldsUntilManuallyEdited()
{
    HostManagerWidget widget(m_database.get(), m_vault.get());

    QAbstractButton *addHostButton = nullptr;
    for (QAbstractButton *button : widget.headerActionsWidget()->findChildren<QAbstractButton *>()) {
        if (button->text() == QStringLiteral("新建主机")) {
            addHostButton = button;
            break;
        }
    }
    QVERIFY2(addHostButton != nullptr, "侧栏没有新建主机按钮");

    QString failure;
    QTimer::singleShot(0, [&failure]() {
        QWidget *dialog = QApplication::activeModalWidget();
        if (dialog == nullptr) {
            failure = QStringLiteral("主机配置对话框未能弹出");
            return;
        }

        auto *nameEdit = dialog->findChild<QLineEdit *>(
            QStringLiteral("hostRecordNameEdit"));
        auto *hostEdit = dialog->findChild<QLineEdit *>(
            QStringLiteral("hostAddressEdit"));
        auto *usernameEdit = dialog->findChild<QLineEdit *>(
            QStringLiteral("hostUsernameEdit"));
        auto *portSpin = dialog->findChild<QSpinBox *>(
            QStringLiteral("hostPortSpin"));
        if (nameEdit == nullptr || hostEdit == nullptr
            || usernameEdit == nullptr || portSpin == nullptr) {
            failure = QStringLiteral("主机配置缺少自动命名所需的输入控件");
            dialog->close();
            return;
        }

        // 用户通常先填地址、后填认证信息。地址输入期间名称必须持续同步完整文本，不能在
        // 第一个字符触发 setText() 后便把自动生成内容误当成人工名称而停止更新。
        QTest::keyClicks(hostEdit, "server.example.com");
        if (nameEdit->text() != QStringLiteral("server.example.com:22")) {
            failure = QStringLiteral("未按主机地址和默认端口自动生成名称");
        }

        if (failure.isEmpty()) {
            QTest::keyClicks(usernameEdit, "deploy");
            if (nameEdit->text() != QStringLiteral("deploy@server.example.com:22")) {
                failure = QStringLiteral("用户名变化后自动名称未同步");
            }
        }

        if (failure.isEmpty()) {
            portSpin->setValue(2222);
            if (nameEdit->text() != QStringLiteral("deploy@server.example.com:2222")) {
                failure = QStringLiteral("端口变化后自动名称未同步");
            }
        }

        if (failure.isEmpty()) {
            // 通过真实键盘事件产生 textEdited，验证业务名称一经人工指定便不会被后续连接参数
            // 覆盖；直接调用 setText() 不代表用户操作，无法覆盖这条交互边界。
            nameEdit->selectAll();
            QTest::keyClicks(nameEdit, "Production API");
            hostEdit->setText(QStringLiteral("replacement.example.com"));
            portSpin->setValue(2200);
            if (nameEdit->text() != QStringLiteral("Production API")) {
                failure = QStringLiteral("修改连接参数覆盖了用户手动填写的名称");
            }
        }

        dialog->close();
    });

    addHostButton->click();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
}

namespace {

/** @brief 建一个带子主机的分组，外加一台顶层主机，凑出"分组/主机"两种层级关系。 */
bool seedGroupWithChildAndTopLevelHost(ApplicationDatabase *database, QString *error)
{
    HostRecord group;
    group.id = QStringLiteral("group-3");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("上饶E呼即办");
    if (!database->saveHost(group, error)) {
        return false;
    }

    HostRecord child;
    child.id = QStringLiteral("host-3");
    child.parentId = group.id;
    child.kind = HostNodeKind::Host;
    child.name = QStringLiteral("中间件");
    child.host = QStringLiteral("115.159.35.31");
    if (!database->saveHost(child, error)) {
        return false;
    }

    HostRecord topLevel;
    topLevel.id = QStringLiteral("host-4");
    topLevel.kind = HostNodeKind::Host;
    topLevel.name = QStringLiteral("10.38.217.99");
    topLevel.host = QStringLiteral("10.38.217.99");
    return database->saveHost(topLevel, error);
}

} // namespace

void HostManagerWidgetTest::topLevelNodesShareLeftEdgeAndChildHostsIndent()
{
    QString error;
    QVERIFY2(seedGroupWithChildAndTopLevelHost(m_database.get(), &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    widget.resize(320, 400);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));

    auto *tree = widget.findChild<QTreeWidget *>();
    QVERIFY(tree != nullptr);
    QTreeWidgetItem *groupItem = findItem(widget, QStringLiteral("上饶E呼即办"));
    QTreeWidgetItem *childItem = findItem(widget, QStringLiteral("中间件"));
    QTreeWidgetItem *topLevelItem = findItem(widget, QStringLiteral("10.38.217.99"));
    QVERIFY(groupItem != nullptr);
    QVERIFY(childItem != nullptr);
    QVERIFY(topLevelItem != nullptr);
    QVERIFY(groupItem->isExpanded());

    // 顶层分组与顶层主机必须顶格并对齐：只为箭头存在的分支装饰列一旦还在，这里就会差出
    // 一整列空白。
    const QRect groupRect = tree->visualItemRect(groupItem);
    const QRect topLevelRect = tree->visualItemRect(topLevelItem);
    QCOMPARE(topLevelRect.left(), groupRect.left());
    QCOMPARE(groupRect.left(), 0);

    // 分组下的主机必须右缩进一级，否则看不出它属于这个分组。
    const QRect childRect = tree->visualItemRect(childItem);
    QCOMPARE(childRect.left() - groupRect.left(), tree->indentation());
    QVERIFY2(tree->indentation() > 0, "层级缩进被压成 0，分组与主机会顶成一列");
}

void HostManagerWidgetTest::groupToggleIsRightAlignedVerticallyCenteredAndClickable()
{
    QString error;
    QVERIFY2(seedGroupWithChildAndTopLevelHost(m_database.get(), &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    widget.resize(320, 400);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));

    auto *tree = widget.findChild<QTreeWidget *>();
    QVERIFY(tree != nullptr);
    QTreeWidgetItem *groupItem = findItem(widget, QStringLiteral("上饶E呼即办"));
    QVERIFY(groupItem != nullptr);
    QVERIFY(groupItem->isExpanded());
    const QRect groupRect = tree->visualItemRect(groupItem);

    // 先在最后一行下方的空白处点一下：窗口刚显示时的第一次点击会被平台层用于激活窗口，
    // 落在树上会让"这一下到底算不算命中"变得不稳定。空白处没有任何节点，点它不改变状态。
    QTest::mouseClick(tree->viewport(),
                      Qt::LeftButton,
                      {},
                      QPoint(groupRect.center().x(), tree->viewport()->height() - 4));

    // 点一下再点回来，只用于探测某个点是不是热区，不改变探测前后的展开状态。
    const auto togglesAt = [&](const QPoint &point) {
        const bool before = groupItem->isExpanded();
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, point);
        const bool changed = groupItem->isExpanded() != before;
        groupItem->setExpanded(before);
        return changed;
    };

    // 行首现在只有分组图标和名称，点它不该再展开收起。
    QVERIFY2(!togglesAt(QPoint(groupRect.left() + 2, groupRect.center().y())), "行首被当成展开按钮");

    // 横向：热区必须覆盖行末完整图标区域，而不是只有 8 像素宽的箭头图形。逐像素探测既
    // 验证它右对齐，也防止后续重构又把可点击范围缩回三角形本身。
    int leftmostHit = -1;
    int rightmostHit = -1;
    for (int x = groupRect.left(); x < groupRect.right(); ++x) {
        if (togglesAt(QPoint(x, groupRect.center().y()))) {
            leftmostHit = leftmostHit < 0 ? x : leftmostHit;
            rightmostHit = x;
        }
    }
    QVERIFY2(leftmostHit >= 0, "分组行上找不到能展开收起的箭头");
    const int nameWidth = tree->fontMetrics().horizontalAdvance(QStringLiteral("上饶E呼即办"));
    QVERIFY2(leftmostHit > groupRect.left() + nameWidth,
             qPrintable(QStringLiteral("箭头左缘 x=%1 紧贴着分组名称（名称宽 %2）")
                            .arg(leftmostHit)
                            .arg(nameWidth)));
    QVERIFY2(groupRect.right() - rightmostHit <= 12,
             qPrintable(QStringLiteral("箭头右缘距行末 %1 像素，没有右对齐")
                            .arg(groupRect.right() - rightmostHit)));
    QVERIFY2(rightmostHit - leftmostHit + 1 >= 28,
             qPrintable(QStringLiteral("右侧展开按钮热区只有 %1 像素宽，仍然过小")
                            .arg(rightmostHit - leftmostHit + 1)));

    // 纵向：右侧图标区域应覆盖当前分组行的完整高度。循环没有探测 QRect::bottom() 这一
    // 个边界像素，因此上下各允许 1 像素取整误差。
    const int probeX = (leftmostHit + rightmostHit) / 2;
    int topHit = -1;
    int bottomHit = -1;
    for (int y = groupRect.top(); y < groupRect.bottom(); ++y) {
        if (togglesAt(QPoint(probeX, y))) {
            topHit = topHit < 0 ? y : topHit;
            bottomHit = y;
        }
    }
    QVERIFY(topHit >= 0);
    QVERIFY2(topHit <= groupRect.top() + 1,
             qPrintable(QStringLiteral("右侧展开按钮热区顶部留出了 %1 像素死区")
                            .arg(topHit - groupRect.top())));
    QVERIFY2(bottomHit >= groupRect.bottom() - 2,
             qPrintable(QStringLiteral("右侧展开按钮热区底部留出了 %1 像素死区")
                            .arg(groupRect.bottom() - bottomHit)));

    // 收起后再点同一个位置必须能重新展开，避免做成“单向收起”。
    const QPoint arrowCenter(probeX, (topHit + bottomHit) / 2);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, arrowCenter);
    QVERIFY2(!groupItem->isExpanded(), "点击箭头没有收起分组");
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, arrowCenter);
    QVERIFY2(groupItem->isExpanded(), "再次点击箭头没有重新展开分组");
}

namespace {

/**
 * @brief 统计某块区域里"有墨"的最右一列，用来量出一段文字实际画了多宽。
 * @param background 该行的底色，与之明显不同的像素即视为文字笔画。
 * @return 相对于 area.left() 的墨迹宽度；整块空白返回 0。
 */
int inkWidth(const QImage &image, const QRect &area, const QColor &background)
{
    int rightmost = -1;
    for (int x = area.left(); x <= area.right() && x < image.width(); ++x) {
        for (int y = area.top(); y <= area.bottom() && y < image.height(); ++y) {
            const QColor pixel = image.pixelColor(x, y);
            // 阈值取 24：反锯齿边缘也算墨，但挡得住选中态之类的淡背景过渡。
            if (std::abs(pixel.red() - background.red()) > 24
                || std::abs(pixel.green() - background.green()) > 24
                || std::abs(pixel.blue() - background.blue()) > 24) {
                rightmost = x;
                break;
            }
        }
    }
    return rightmost < 0 ? 0 : rightmost - area.left() + 1;
}

} // namespace

void HostManagerWidgetTest::semanticIconsFollowLightAndDarkPalettes()
{
    QWidget context;
    const QColor lightThemeIcon(QStringLiteral("#1b2230"));
    const QColor darkThemeIcon(QStringLiteral("#f0f3fa"));
    const QColor fixedBrandIcon(QStringLiteral("#0c2f24"));

    const auto setWindowText = [&context](const QColor &color) {
        QPalette palette = context.palette();
        // 正常和禁用模式分别读取对应 ColorGroup；测试正常模式时也同步其它组，避免平台默认
        // 调色板在控件尚未显示时把 Active 自动解析为 Inactive 而造成无关差异。
        palette.setColor(QPalette::Active, QPalette::WindowText, color);
        palette.setColor(QPalette::Inactive, QPalette::WindowText, color);
        palette.setColor(QPalette::Disabled, QPalette::WindowText, color);
        context.setPalette(palette);
    };
    const auto strongestPixel = [](const QIcon &icon) {
        const QImage image = icon.pixmap(QSize(20, 20), QIcon::Normal).toImage();
        QColor result;
        int greatestAlpha = -1;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.alpha() > greatestAlpha) {
                    greatestAlpha = pixel.alpha();
                    result = pixel;
                }
            }
        }
        return result;
    };
    const auto totalOpacity = [](const QIcon &icon) {
        const QImage image = icon.pixmap(QSize(64, 64), QIcon::Normal).toImage();
        qint64 opacity = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                opacity += image.pixelColor(x, y).alpha();
            }
        }
        return opacity;
    };

    setWindowText(lightThemeIcon);
    const QIcon adaptive = UiIcons::symbol(
        &context, UiIcons::Symbol::Refresh, context.palette().color(QPalette::WindowText));
    const QIcon fixed = UiIcons::symbol(&context, UiIcons::Symbol::Plus, fixedBrandIcon);
    QCOMPARE(strongestPixel(adaptive).rgb(), lightThemeIcon.rgb());
    QCOMPARE(strongestPixel(fixed).rgb(), fixedBrandIcon.rgb());

    // 主机树图标与导航栏服务器图标保持同一轮廓，但重复密集出现时必须使用更细笔画。
    // 以相同尺寸和颜色比较总不透明度，可直接约束实际渲染重量，而不依赖实现中的线宽数字。
    const QIcon navigationHosts = UiIcons::symbol(
        &context, UiIcons::Symbol::Hosts, context.palette().color(QPalette::WindowText));
    const QIcon treeHost = UiIcons::symbol(
        &context, UiIcons::Symbol::HostItem, context.palette().color(QPalette::WindowText));
    QVERIFY2(totalOpacity(treeHost) < totalOpacity(navigationHosts),
             "主机树服务器图标没有比导航栏图标使用更轻的线条");

    // 关键回归：不重新创建 QIcon，只切换所属控件的调色板。同一个实例必须立即绘制成深色
    // 主题前景色，否则应用设置页实时切换主题后仍会保留旧位图并消失在新背景上。
    setWindowText(darkThemeIcon);
    QCOMPARE(strongestPixel(adaptive).rgb(), darkThemeIcon.rgb());
    // 主按钮的绿色背景不随主题改变，它的深色“+”属于显式品牌色，不能被全局前景色覆盖。
    QCOMPARE(strongestPixel(fixed).rgb(), fixedBrandIcon.rgb());
}

void HostManagerWidgetTest::groupNameUsesSameFontAsHostName()
{
    // 两个节点取同一串名字：字体一致时，画出来的墨迹宽度必然相等；任一类型擅自改变字号
    // 或字重，同样的文本都会出现明显不同的渲染结果。
    const QString sharedName = QStringLiteral("上饶E呼即办");

    HostRecord group;
    group.id = QStringLiteral("group-5");
    group.kind = HostNodeKind::Folder;
    group.name = sharedName;
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostRecord host;
    host.id = QStringLiteral("host-5");
    host.kind = HostNodeKind::Host;
    host.name = sharedName;
    host.host = QStringLiteral("10.38.217.99");
    QVERIFY2(m_database->saveHost(host, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    widget.resize(320, 400);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));

    auto *tree = widget.findChild<QTreeWidget *>();
    QVERIFY(tree != nullptr);
    // 选中态会换掉底色和字色，量墨迹前先清掉，让两行都落在同一种背景上。
    tree->clearSelection();
    tree->setCurrentItem(nullptr);

    QTreeWidgetItem *groupItem = nullptr;
    QTreeWidgetItem *hostItem = nullptr;
    for (int row = 0; row < tree->topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = tree->topLevelItem(row);
        const auto kind = static_cast<HostNodeKind>(item->data(0, Qt::UserRole + 1).toInt());
        (kind == HostNodeKind::Folder ? groupItem : hostItem) = item;
    }
    QVERIFY(groupItem != nullptr);
    QVERIFY(hostItem != nullptr);
    // 模型层明确写入 Normal，既表达产品约束，也阻止平台主题把某一种节点默认成粗体。
    QCOMPARE(groupItem->font(0).weight(), QFont::Normal);
    QCOMPARE(hostItem->font(0).weight(), QFont::Normal);
    QVERIFY(!groupItem->font(0).bold());
    QVERIFY(!hostItem->font(0).bold());

    const QImage rendered = tree->viewport()->grab().toImage();
    const QRect groupRect = tree->visualItemRect(groupItem);
    const QRect hostRect = tree->visualItemRect(hostItem);
    QVERIFY(!groupRect.isEmpty());
    QVERIFY(!hostRect.isEmpty());

    // visualItemRect 给的是逻辑坐标，grab() 出来的图在 Retina 上是设备像素，取样前要换算。
    const int scale = std::max(1, qRound(rendered.devicePixelRatio()));
    const auto toDevice = [scale](const QRect &rect) {
        return QRect(rect.left() * scale, rect.top() * scale, rect.width() * scale,
                     rect.height() * scale);
    };

    // 空分组没有展开箭头，但行右侧会画一个子节点数量徽章；主机名在上、连接信息在下，
    // 只量上半行。两种节点行末都留一段右侧装饰边距不参与取样，量的才是纯文字渲染宽度。
    QVERIFY2(groupItem->childCount() == 0, "分组带了子节点，行末的展开箭头会混进墨迹里");
    constexpr int kRightDecorationMargin = 40;
    const QRect groupArea = toDevice(QRect(groupRect.left(),
                                           groupRect.top(),
                                           std::max(0, groupRect.width() - kRightDecorationMargin),
                                           groupRect.height()));
    const QRect hostArea = toDevice(
        QRect(hostRect.left(), hostRect.top(), hostRect.width(), hostRect.height() / 2));
    const QColor background = rendered.pixelColor(groupArea.right() - 1, groupArea.top() + 1);

    const int groupInk = inkWidth(rendered, groupArea, background);
    const int hostInk = inkWidth(rendered, hostArea, background);
    QVERIFY2(groupInk > 0 && hostInk > 0, "没量到任何文字墨迹，行的取样区域不对");
    // 容差按逻辑像素折算：字体相同时只剩反锯齿的零头，换字号或字重会差出明显像素。
    QVERIFY2(std::abs(groupInk - hostInk) <= 2 * scale,
             qPrintable(QStringLiteral("分组名墨迹宽 %1，主机名墨迹宽 %2（设备像素），两者字体不一致")
                            .arg(groupInk)
                            .arg(hostInk)));
}

void HostManagerWidgetTest::ungroupedHostsFollowFoldersBehindSectionHeader()
{
    HostRecord group;
    group.id = QStringLiteral("group-6");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("武汉");
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostRecord groupedHost;
    groupedHost.id = QStringLiteral("host-6a");
    groupedHost.parentId = group.id;
    groupedHost.kind = HostNodeKind::Host;
    groupedHost.name = QStringLiteral("预发布");
    groupedHost.host = QStringLiteral("10.194.135.11");
    QVERIFY2(m_database->saveHost(groupedHost, &error), qPrintable(error));

    // 无父分组，代表"未分组"主机。
    HostRecord ungroupedHost;
    ungroupedHost.id = QStringLiteral("host-6b");
    ungroupedHost.kind = HostNodeKind::Host;
    ungroupedHost.name = QStringLiteral("10.38.217.99");
    ungroupedHost.host = QStringLiteral("10.38.217.99");
    QVERIFY2(m_database->saveHost(ungroupedHost, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    auto *tree = widget.findChild<QTreeWidget *>();
    QVERIFY(tree != nullptr);

    // 顶层只应有三项：分组、"未分组"分区标题、无父主机；顺序必须是分组在前、主机在后，
    // 中间用标题隔开，否则两类节点混排会让用户分不清哪些是组织结构、哪些是可连接目标。
    QCOMPARE(tree->topLevelItemCount(), 3);

    QTreeWidgetItem *folderItem = tree->topLevelItem(0);
    QCOMPARE(folderItem->text(0), QStringLiteral("武汉"));
    QCOMPARE(static_cast<HostNodeKind>(folderItem->data(0, Qt::UserRole + 1).toInt()),
             HostNodeKind::Folder);

    QTreeWidgetItem *headerItem = tree->topLevelItem(1);
    QCOMPARE(headerItem->text(0), QStringLiteral("未分组"));
    // 标题的 kind 既不是 Folder 也不是 Host：它不对应任何数据库记录，不能被误当成两者
    // 之一参与拖拽、编辑或删除。
    const int headerKind = headerItem->data(0, Qt::UserRole + 1).toInt();
    QVERIFY(headerKind != static_cast<int>(HostNodeKind::Folder));
    QVERIFY(headerKind != static_cast<int>(HostNodeKind::Host));
    QVERIFY2(!(headerItem->flags() & Qt::ItemIsSelectable), "分区标题不能被选中");
    QVERIFY2(!(headerItem->flags() & Qt::ItemIsDragEnabled), "分区标题不能被拖动");
    QVERIFY2(!(headerItem->flags() & Qt::ItemIsDropEnabled), "分区标题不能接收拖放");

    QTreeWidgetItem *ungroupedItem = tree->topLevelItem(2);
    QCOMPARE(ungroupedItem->text(0), QStringLiteral("10.38.217.99"));
    QCOMPARE(static_cast<HostNodeKind>(ungroupedItem->data(0, Qt::UserRole + 1).toInt()),
             HostNodeKind::Host);
    QVERIFY2(ungroupedItem->flags() & Qt::ItemIsSelectable, "无父主机本身仍必须可选中");

    // 分组下的主机仍是子节点，不受"未分组"分区影响。
    QCOMPARE(folderItem->childCount(), 1);
    QCOMPARE(folderItem->child(0)->text(0), QStringLiteral("预发布"));
}

void HostManagerWidgetTest::sectionHeaderAbsentWhenNoUngroupedHosts()
{
    HostRecord group;
    group.id = QStringLiteral("group-7");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("东西湖城运");
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostRecord groupedHost;
    groupedHost.id = QStringLiteral("host-7");
    groupedHost.parentId = group.id;
    groupedHost.kind = HostNodeKind::Host;
    groupedHost.name = QStringLiteral("10.70.165.50");
    groupedHost.host = QStringLiteral("10.70.165.50");
    QVERIFY2(m_database->saveHost(groupedHost, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    auto *tree = widget.findChild<QTreeWidget *>();
    QVERIFY(tree != nullptr);

    // 没有无父主机时不该凭空多出一个"未分组"空壳分区。
    QCOMPARE(tree->topLevelItemCount(), 1);
    QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("东西湖城运"));
}

QTEST_MAIN(HostManagerWidgetTest)

#include "HostManagerWidgetTest.moc"
