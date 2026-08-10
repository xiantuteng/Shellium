#include "core/HostRecord.h"
#include "security/Vault.h"
#include "storage/ApplicationDatabase.h"
#include "ui/HostManagerWidget.h"
#include "ui/UiIcons.h"

#include <QAbstractButton>
#include <QApplication>
#include <QColor>
#include <QImage>
#include <QLayout>
#include <QListWidget>
#include <QSet>
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

    /** @brief 分组配置窗口不得让主机专属控件浮在左上角盖住卡片顶部。 */
    void groupDialogHasNoStrayWidgets();

    /** @brief 顶层节点顶格、分组下的主机右缩进，二者不能再被分支装饰列推乱。 */
    void topLevelNodesShareLeftEdgeAndChildHostsIndent();

    /** @brief 展开箭头必须右对齐贴在行末并纵向居中，且点得动、点得回。 */
    void groupToggleIsRightAlignedVerticallyCenteredAndClickable();

    /** @brief 分组名与主机名必须用同一套字体：同一串文字画出来的墨迹宽度要相等。 */
    void groupNameUsesSameFontAsHostName();

    /** @brief 同一个自绘图标实例必须在主题切换后改用新前景色，显式品牌色则保持不变。 */
    void semanticIconsFollowLightAndDarkPalettes();

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
        dialog->close();
    });

    // 分组不展示主机地址、会话密码与旧版兼容开关，但这些控件仍以对话框为父对象被创建。
    // 父子关系只决定归属、不决定位置：没有被任何布局接管的控件会停在 (0,0) 按 sizeHint
    // 绘制，于是窗口顶部浮出一个空白输入框、一段密码占位文字和一个开关，盖住卡片标题。
    addGroupButton->click();
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

    // 横向：热区必须右对齐贴在行末，而不是跟在名字后面。
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

    // 纵向：热区中心必须落在行的中线上，允许 1 像素的取整误差。
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
    QVERIFY2(std::abs(((topHit + bottomHit) / 2) - groupRect.center().y()) <= 1,
             qPrintable(QStringLiteral("箭头纵向中心 %1 偏离行中线 %2")
                            .arg((topHit + bottomHit) / 2)
                            .arg(groupRect.center().y())));

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

    setWindowText(lightThemeIcon);
    const QIcon adaptive = UiIcons::symbol(
        &context, UiIcons::Symbol::Refresh, context.palette().color(QPalette::WindowText));
    const QIcon fixed = UiIcons::symbol(&context, UiIcons::Symbol::Plus, fixedBrandIcon);
    QCOMPARE(strongestPixel(adaptive).rgb(), lightThemeIcon.rgb());
    QCOMPARE(strongestPixel(fixed).rgb(), fixedBrandIcon.rgb());

    // 关键回归：不重新创建 QIcon，只切换所属控件的调色板。同一个实例必须立即绘制成深色
    // 主题前景色，否则应用设置页实时切换主题后仍会保留旧位图并消失在新背景上。
    setWindowText(darkThemeIcon);
    QCOMPARE(strongestPixel(adaptive).rgb(), darkThemeIcon.rgb());
    // 主按钮的绿色背景不随主题改变，它的深色“+”属于显式品牌色，不能被全局前景色覆盖。
    QCOMPARE(strongestPixel(fixed).rgb(), fixedBrandIcon.rgb());
}

void HostManagerWidgetTest::groupNameUsesSameFontAsHostName()
{
    // 两个节点取同一串名字：字体一致时，画出来的墨迹宽度必然相等；分组一旦退回基准字体
    // （不加粗、小一号），同样的字就会明显窄一截。
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

    // 空分组没有展开箭头，整行右侧只剩名称；主机名在上、连接信息在下，只量上半行。
    QVERIFY2(groupItem->childCount() == 0, "分组带了子节点，行末的箭头会混进墨迹里");
    const QRect groupArea = toDevice(groupRect);
    const QRect hostArea = toDevice(
        QRect(hostRect.left(), hostRect.top(), hostRect.width(), hostRect.height() / 2));
    const QColor background = rendered.pixelColor(groupArea.right() - 1, groupArea.top() + 1);

    const int groupInk = inkWidth(rendered, groupArea, background);
    const int hostInk = inkWidth(rendered, hostArea, background);
    QVERIFY2(groupInk > 0 && hostInk > 0, "没量到任何文字墨迹，行的取样区域不对");
    // 容差按逻辑像素折算：字体相同时只剩反锯齿的零头，换一号字/去掉加粗会差出十几个像素。
    QVERIFY2(std::abs(groupInk - hostInk) <= 2 * scale,
             qPrintable(QStringLiteral("分组名墨迹宽 %1，主机名墨迹宽 %2（设备像素），两者字体不一致")
                            .arg(groupInk)
                            .arg(hostInk)));
}

QTEST_MAIN(HostManagerWidgetTest)

#include "HostManagerWidgetTest.moc"
