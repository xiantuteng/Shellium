#include "forwarding/PortForwardManagerWidget.h"
#include "platform/NativeSocket.h"

#include "forwarding/PortForwardRuntime.h"
#include "ssh/SshSessionStack.h"
#include "storage/ApplicationDatabase.h"
#include "ui/UiIcons.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QFileIconProvider>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <utility>


namespace {

/** @brief 表格首列存放持久化规则 id 的自定义数据角色。 */
constexpr int kRuleIdRole = Qt::UserRole + 31;

/** @brief 主机树节点保存持久化 hostId 的数据角色；分组节点保持为空，确保无法被选中。 */
constexpr int kHostIdRole = Qt::UserRole + 32;

/** @brief 主机树节点保存 HostNodeKind 的角色，供键盘激活时区分分组与可选主机。 */
constexpr int kHostKindRole = Qt::UserRole + 33;

/** @brief 主机树节点保存端口转发规则数量；零值不追加任何显示文本。 */
constexpr int kForwardingCountRole = Qt::UserRole + 34;

/**
 * @brief 返回与主机管理选中卡片一致的品牌绿色。
 *
 * 端口转发控件的独立测试目标没有链接 SettingsWidgets.cpp，不能为了一个纯展示色引入整个
 * 设置组件依赖。这里使用应用主题的稳定品牌色，并仍通过透明度区分背景与描边。
 */
QColor forwardingAccentColor()
{
    return QColor(QStringLiteral("#2bd981"));
}

/**
 * @brief 返回与主机管理及本地文件树同源的系统文件夹图标。
 * @param context 平台图标不可用时提供主题色的上下文控件。
 * @return 系统文件夹图标；不可用时退回 Shellium 文件夹图标。
 *
 * 使用 QFileIconProvider 能让端口转发下拉树与主机管理的分组图标完全一致，而不是继续
 * 使用一枚颜色和线宽不同的自绘图标，造成两个页面像来自不同产品。
 */
QIcon forwardingFolderIcon(const QWidget *context)
{
    Q_ASSERT(context != nullptr);
    static QFileIconProvider iconProvider;
    const QIcon platformFolder = iconProvider.icon(QFileIconProvider::Folder);
    return platformFolder.isNull()
        ? UiIcons::symbol(context,
                          UiIcons::Symbol::Folder,
                          context->palette().color(QPalette::WindowText),
                          QSize(18, 18))
        : platformFolder;
}

/** @brief 端口转发主机树的分组行固定高度，与主机管理保持一致。 */
constexpr int kForwardingFolderRowHeight = 34;

/** @brief 端口转发主机树的主机行固定高度；单行内容无需沿用主机管理的双行 50px。 */
constexpr int kForwardingHostRowHeight = 38;

/**
 * @brief 以主机管理树的视觉规则绘制端口转发主机选择树。
 *
 * 分组使用“箭头—系统文件夹—粗体名称”，主机使用圆角图标容器、常规字重名称和低对比
 * 数量文字。选中与悬停背景仅绘制在实际节点区域，层级线保持中性，避免原生分支样式形成
 * 黑色双边框、整行直角选中色以及过密的树状线。
 */
class PortForwardHostDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    /** @brief 点击分组左侧展开热区时由宿主切换展开状态。 */
    std::function<void(const QModelIndex &)> onToggleRequested;

protected:
    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (static_cast<HostNodeKind>(index.data(kHostKindRole).toInt())
            == HostNodeKind::Folder) {
            paintFolder(painter, option, index);
            return;
        }
        paintHost(painter, option, index);
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        hint.setHeight(static_cast<HostNodeKind>(index.data(kHostKindRole).toInt())
                               == HostNodeKind::Folder
                           ? kForwardingFolderRowHeight
                           : kForwardingHostRowHeight);
        return hint;
    }

    bool editorEvent(QEvent *event,
                     QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        if (event != nullptr && event->type() == QEvent::MouseButtonRelease
            && onToggleRequested
            && static_cast<HostNodeKind>(index.data(kHostKindRole).toInt())
                   == HostNodeKind::Folder) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            const QRect toggleArea(option.rect.left(), option.rect.top(), 26, option.rect.height());
            if (mouseEvent->button() == Qt::LeftButton
                && toggleArea.contains(mouseEvent->position().toPoint())) {
                onToggleRequested(index);
                return true;
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

private:
    /** @brief 清除平台 item 文本和交互态，只保留透明基础背景。 */
    static void paintBase(QPainter *painter,
                          const QStyleOptionViewItem &option,
                          const QModelIndex &index)
    {
        QStyleOptionViewItem background = option;
        background.text.clear();
        background.icon = {};
        background.features &= ~QStyleOptionViewItem::HasDecoration;
        background.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver);
        const QWidget *widget = option.widget;
        QStyle *style = widget != nullptr ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);

        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        if (!selected && !hovered) {
            return;
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        QColor fill;
        QColor border;
        if (selected) {
            fill = forwardingAccentColor();
            fill.setAlphaF(0.13F);
            border = forwardingAccentColor();
            border.setAlphaF(0.42F);
        } else if (widget != nullptr) {
            fill = QColor(widget->property("hostHoverBackgroundColor").toString());
        }
        if (!fill.isValid()) {
            fill = QColor(QStringLiteral("#e1e7eb"));
        }
        painter->setPen(selected ? QPen(border, 1) : Qt::NoPen);
        painter->setBrush(fill);
        painter->drawRoundedRect(option.rect.adjusted(0, 2, -1, -2), 9, 9);
        painter->restore();
    }

    /** @brief 为所有非顶层节点绘制与主机管理相同的低透明度父子引导线。 */
    static void paintGuideLine(QPainter *painter,
                               const QStyleOptionViewItem &option,
                               const QModelIndex &index)
    {
        if (!index.parent().isValid()) {
            return;
        }
        QColor lineColor = option.palette.color(QPalette::Text);
        lineColor.setAlphaF(0.14F);
        const int lineX = option.rect.left() - 15;
        painter->save();
        painter->setPen(QPen(lineColor, 1));
        painter->drawLine(lineX, option.rect.top(), lineX, option.rect.bottom());
        painter->drawLine(lineX, option.rect.center().y(), option.rect.left() - 3,
                          option.rect.center().y());
        painter->restore();
    }

    /** @brief 绘制分组的箭头、系统文件夹图标与粗体名称。 */
    static void paintFolder(QPainter *painter,
                            const QStyleOptionViewItem &option,
                            const QModelIndex &index)
    {
        paintBase(painter, option, index);
        paintGuideLine(painter, option, index);
        const QColor textColor = option.palette.color(QPalette::Text);
        const bool hasChildren = index.model() != nullptr && index.model()->rowCount(index) > 0;

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        if (hasChildren) {
            QColor arrowColor = textColor;
            arrowColor.setAlphaF(0.55F);
            painter->setPen(Qt::NoPen);
            painter->setBrush(arrowColor);
            const QPointF center(option.rect.left() + 12, option.rect.center().y());
            QPolygonF arrow;
            if (option.state.testFlag(QStyle::State_Open)) {
                arrow << QPointF(center.x() - 4, center.y() - 2)
                      << QPointF(center.x() + 4, center.y() - 2)
                      << QPointF(center.x(), center.y() + 3);
            } else {
                arrow << QPointF(center.x() - 2, center.y() - 4)
                      << QPointF(center.x() - 2, center.y() + 4)
                      << QPointF(center.x() + 3, center.y());
            }
            painter->drawPolygon(arrow);
        }

        qvariant_cast<QIcon>(index.data(Qt::DecorationRole))
            .paint(painter,
                   QRect(option.rect.left() + 29, option.rect.center().y() - 9, 18, 18),
                   Qt::AlignCenter);
        QFont font = option.font;
        font.setBold(true);
        painter->setFont(font);
        painter->setPen(textColor);
        const QRect textRect(option.rect.left() + 52,
                             option.rect.top(),
                             std::max(0, option.rect.width() - 60),
                             option.rect.height());
        painter->drawText(textRect,
                          Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(font).elidedText(index.data(Qt::DisplayRole).toString(),
                                                        Qt::ElideRight,
                                                        textRect.width()));
        painter->restore();
    }

    /** @brief 绘制主机图标容器、名称，以及仅在非零时出现的灰色数量文字。 */
    static void paintHost(QPainter *painter,
                          const QStyleOptionViewItem &option,
                          const QModelIndex &index)
    {
        paintBase(painter, option, index);
        paintGuideLine(painter, option, index);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const QWidget *widget = option.widget;
        const QRect iconBox(option.rect.left() + 6,
                            option.rect.center().y() - 13,
                            26,
                            26);
        QColor iconFill = selected ? forwardingAccentColor() : QColor();
        if (!selected && widget != nullptr) {
            iconFill = QColor(widget->property("hostIconSurfaceColor").toString());
        }
        if (!iconFill.isValid()) {
            iconFill = option.palette.color(QPalette::Base);
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        QColor iconBorder = option.palette.color(QPalette::Text);
        iconBorder.setAlphaF(selected ? 0.0F : 0.12F);
        painter->setPen(selected ? Qt::NoPen : QPen(iconBorder, 1));
        painter->setBrush(iconFill);
        painter->drawRoundedRect(iconBox.adjusted(0, 0, -1, -1), 8, 8);
        UiIcons::symbol(widget,
                        UiIcons::Symbol::HostItem,
                        selected ? QColor(QStringLiteral("#0c2f24"))
                                 : option.palette.color(QPalette::Text),
                        QSize(14, 14))
            .paint(painter, iconBox.adjusted(6, 6, -6, -6), Qt::AlignCenter);

        QFont nameFont = option.font;
        nameFont.setBold(false);
        const QFontMetrics nameMetrics(nameFont);
        const int count = index.data(kForwardingCountRole).toInt();
        const QString countText = count > 0 ? QStringLiteral("(%1)").arg(count) : QString();
        // 数量只是名称后的次要文字，保持同字号并降低透明度即可；不强行缩小可以避免像素
        // 字体或系统字体没有有效 pointSizeF 时触发无效字号，同时更接近主机管理的标签节奏。
        const QFont countFont = nameFont;
        const QFontMetrics countMetrics(countFont);
        const int countWidth = countText.isEmpty()
            ? 0
            : countMetrics.horizontalAdvance(countText) + 8;
        const int textLeft = option.rect.left() + 42;
        const QRect nameRect(textLeft,
                             option.rect.top(),
                             std::max(0, option.rect.right() - textLeft - countWidth - 8),
                             option.rect.height());
        painter->setFont(nameFont);
        painter->setPen(option.palette.color(QPalette::Text));
        painter->drawText(nameRect,
                          Qt::AlignLeft | Qt::AlignVCenter,
                          nameMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                                 Qt::ElideRight,
                                                 nameRect.width()));

        if (!countText.isEmpty()) {
            QColor countColor = option.palette.color(QPalette::Text);
            countColor.setAlphaF(0.52F);
            painter->setFont(countFont);
            painter->setPen(countColor);
            painter->drawText(QRect(nameRect.right() + 6,
                                    option.rect.top(),
                                    countWidth,
                                    option.rect.height()),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              countText);
        }
        painter->restore();
    }
};

/**
 * @brief 关闭平台原生分支绘制的端口转发主机树。
 *
 * 箭头和层级线已经由 PortForwardHostDelegate 按主机管理规范绘制；若继续让 QTreeWidget
 * 绘制平台分支，macOS 会叠加黑色竖线、原生箭头和缩进区选中底色，正是上一版显得突兀
 * 的主要原因。这里只禁用分支绘制，不改变滚动、选择或键盘导航行为。
 */
class PortForwardHostTree final : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;

protected:
    /** @brief 保持分支区透明；实际箭头和引导线统一由项目委托绘制。 */
    void drawBranches(QPainter *painter,
                      const QRect &rect,
                      const QModelIndex &index) const override
    {
        Q_UNUSED(painter)
        Q_UNUSED(rect)
        Q_UNUSED(index)
    }
};

/**
 * @brief 端口转发专用的树形主机下拉框。
 *
 * QComboBox 原生弹出视图虽然可以替换为 QTreeView，但其 currentIndex 仍是相对于单个
 * rootModelIndex 的行号，选择任意深度子节点后很难稳定恢复到完整模型索引。这里保留一个
 * 只含主机的扁平 QComboBox 模型作为真实选择状态，再用独立 Qt::Popup 树呈现组织层级：
 * 分组只负责展开，主机点击后映射回稳定 hostId。这样既兼容现有 currentData()/信号调用，
 * 又从数据结构上保证分组永远不会成为端口转发归属对象。
 */
class HostTreeComboBox final : public QComboBox {
public:
    /**
     * @brief 创建下拉框与现代化树形弹层。
     * @param parent Qt 父控件。
     */
    explicit HostTreeComboBox(QWidget *parent = nullptr)
        : QComboBox(parent)
        , m_popup(new QFrame(this, Qt::Popup | Qt::FramelessWindowHint))
        , m_tree(new PortForwardHostTree(m_popup))
    {
        setObjectName(QStringLiteral("PortForwardHostCombo"));
        setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        setMinimumContentsLength(14);
        setAccessibleName(tr("端口转发主机"));

        // 树形列表承载大量文字，不能使用 WA_TranslucentBackground：macOS 会把下方主界面
        // 一并合成进顶层窗口，重叠文字不仅降低可读性，还会让抗锯齿边缘看起来发虚。这里让
        // 弹层与滚动视口都进行不透明绘制，圆角和边框交给 QSS；Qt::Popup 仍负责外部点击关闭。
        m_popup->setObjectName(QStringLiteral("PortForwardHostPopup"));
        m_popup->setAttribute(Qt::WA_TranslucentBackground, false);
        m_popup->setAttribute(Qt::WA_StyledBackground, true);
        m_popup->setAutoFillBackground(true);
        // 外框完全交给应用 QSS 绘制。StyledPanel 在 macOS 上会额外叠一层原生黑框，形成
        // 截图中的双边界，与主机管理的轻量表面卡片明显不一致。
        m_popup->setFrameShape(QFrame::NoFrame);
        m_popup->setFrameShadow(QFrame::Plain);
        auto *popupLayout = new QVBoxLayout(m_popup);
        // 内容与外框之间保留 5px 安全区，滚动时文字和选中背景不会覆盖边框。
        popupLayout->setContentsMargins(5, 5, 5, 5);
        popupLayout->setSpacing(0);

        m_tree->setObjectName(QStringLiteral("PortForwardHostTree"));
        m_tree->setAttribute(Qt::WA_StyledBackground, true);
        m_tree->setAutoFillBackground(true);
        m_tree->viewport()->setAttribute(Qt::WA_StyledBackground, true);
        m_tree->viewport()->setAutoFillBackground(true);
        m_tree->setHeaderHidden(true);
        m_tree->setColumnCount(1);
        // 与主机管理一致：平台分支装饰关闭，由委托统一画箭头、圆角选中背景和轻量连线。
        // 这样不会出现原生黑框、直角选中条和重复箭头，也不会为徽章第二列牺牲名称宽度。
        m_tree->setRootIsDecorated(false);
        m_tree->setItemsExpandable(true);
        m_tree->setExpandsOnDoubleClick(true);
        m_tree->setIndentation(32);
        m_tree->setIconSize(QSize(18, 18));
        m_tree->setUniformRowHeights(true);
        m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
        m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_tree->setProperty("hostIconSurfaceColor", QStringLiteral("#ffffff"));
        m_tree->setProperty("hostHoverBackgroundColor", QStringLiteral("#e1e7eb"));
        auto *treeDelegate = new PortForwardHostDelegate(m_tree);
        treeDelegate->onToggleRequested = [this](const QModelIndex &index) {
            m_tree->setExpanded(index, !m_tree->isExpanded(index));
        };
        m_tree->setItemDelegate(treeDelegate);
        m_tree->installEventFilter(this);
        popupLayout->addWidget(m_tree);

        connect(m_tree, &QTreeWidget::itemClicked,
                this, [this](QTreeWidgetItem *item, const int) { selectTreeItem(item); });
        connect(m_tree, &QTreeWidget::itemActivated,
                this, [this](QTreeWidgetItem *item, const int) { selectTreeItem(item); });
    }

    /**
     * @brief 同步完整主机树、主机规则数量并尽量恢复原选择。
     * @param records 数据库中的全部分组与主机记录。
     * @param forwardingCounts hostId 到端口转发规则数的聚合结果。
     * @param preferredHostId 刷新前选中的主机 id；不存在时自动选择第一台主机。
     *
     * 扁平模型和弹层树在同一次信号阻断区间内重建，外部不会观察到“文字已更新但树还是
     * 旧数据”的中间状态。全部主机加入完成后只触发一次最终选择变更。
     */
    void setHostRecords(const QList<HostRecord> &records,
                        const QHash<QString, int> &forwardingCounts,
                        const QString &preferredHostId)
    {
        const QSignalBlocker blocker(this);
        clear();
        m_tree->clear();
        m_recordCount = records.size();

        QHash<QString, QList<HostRecord>> childrenByParent;
        for (const HostRecord &record : records) {
            childrenByParent[record.parentId].append(record);
            if (record.kind == HostNodeKind::Host) {
                const int count = forwardingCounts.value(record.id, 0);
                addItem(UiIcons::symbol(
                            this,
                            UiIcons::Symbol::HostItem,
                            palette().color(QPalette::WindowText),
                            QSize(16, 16)),
                        count > 0 ? tr("%1  (%2)").arg(record.name).arg(count) : record.name,
                        record.id);
            }
        }

        // 使用递归按 parentId 构造任意深度的分组树。数据库已阻止环和悬空父引用，因此
        // 这里无需猜测异常节点的归属；若未来导入层放宽约束，未挂到根下的节点也不会被误选。
        const auto appendChildren = [&](const auto &self,
                                        QTreeWidgetItem *parentItem,
                                        const QString &parentId) -> void {
            const QList<HostRecord> children = childrenByParent.value(parentId);
            for (const HostRecord &record : children) {
                const int count = forwardingCounts.value(record.id, 0);
                auto *item = new QTreeWidgetItem;
                item->setText(0, record.name);
                item->setData(0, kHostKindRole, static_cast<int>(record.kind));
                if (record.kind == HostNodeKind::Host) {
                    item->setData(0, kHostIdRole, record.id);
                    item->setData(0, kForwardingCountRole, count);
                    item->setIcon(0, UiIcons::symbol(
                                         this,
                                         UiIcons::Symbol::HostItem,
                                         palette().color(QPalette::WindowText),
                                         QSize(16, 16)));
                    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                } else {
                    item->setIcon(0, forwardingFolderIcon(this));
                    // 分组只承担组织和展开功能，主动移除 ItemIsSelectable 可以同时阻止
                    // 鼠标和键盘把它写入当前 hostId，符合数据库端“规则不能绑定分组”的约束。
                    item->setFlags(Qt::ItemIsEnabled);
                }

                if (parentItem == nullptr) {
                    m_tree->addTopLevelItem(item);
                } else {
                    parentItem->addChild(item);
                }
                if (record.kind == HostNodeKind::Folder) {
                    self(self, item, record.id);
                }

            }
        };
        appendChildren(appendChildren, nullptr, QString());
        m_tree->expandAll();

        const int restoredIndex = findData(preferredHostId);
        setCurrentIndex(restoredIndex >= 0 ? restoredIndex : (count() > 0 ? 0 : -1));
        synchronizeTreeSelection();
    }

protected:
    /** @brief 在字段下方显示自定义树弹层，并把当前主机滚动到可见区域。 */
    void showPopup() override
    {
        if (count() <= 0) {
            return;
        }
        synchronizeTreeSelection();

        // 主题可能在控件构造后切换。每次显示前把当前下拉框调色板同步给独立顶层弹窗及其
        // viewport，确保 autoFillBackground 使用的是最新实色，而不是启动阶段的系统颜色。
        const QPalette popupPalette = palette();
        m_popup->setPalette(popupPalette);
        m_tree->setPalette(popupPalette);
        m_tree->viewport()->setPalette(popupPalette);
        // MainWindow 会像主机管理一样把应用主题的表面色写入动态属性；这里不再从操作系统
        // 调色板反推，否则“系统深色 + 应用浅色”等组合会覆盖正确属性并造成两页风格割裂。

        const QPoint belowField = mapToGlobal(QPoint(0, height() + 4));
        QScreen *screen = QGuiApplication::screenAt(belowField);
        const QRect available = screen == nullptr
            ? QRect(belowField, QSize(360, 360))
            : screen->availableGeometry();
        const int popupWidth = std::min(std::max(width(), 300), available.width());
        const int estimatedRowsHeight = std::clamp(m_recordCount * 32 + 14, 112, 360);
        const int popupHeight = std::min(estimatedRowsHeight, available.height());
        int popupX = belowField.x();
        if (popupX + popupWidth > available.right() + 1) {
            popupX = available.right() + 1 - popupWidth;
        }
        int popupY = belowField.y();
        if (popupY + popupHeight > available.bottom() + 1) {
            popupY = mapToGlobal(QPoint(0, -popupHeight - 4)).y();
        }
        m_popup->setGeometry(popupX, popupY, popupWidth, popupHeight);
        m_popup->show();
        m_popup->raise();
        m_tree->setFocus(Qt::PopupFocusReason);
    }

    /** @brief 关闭自定义弹层；Qt::Popup 的外部点击也会进入同一隐藏状态。 */
    void hidePopup() override
    {
        m_popup->hide();
    }

    /**
     * @brief 处理弹层内的 Escape、Enter 与空格键，补齐原生下拉框的键盘可访问性。
     * @param watched 事件来源；只有主机树会进入特殊处理。
     * @param event 待处理 Qt 事件。
     * @return 已消费事件返回 true，其余交回 QComboBox 默认逻辑。
     */
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_tree && event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Escape) {
                hidePopup();
                setFocus(Qt::PopupFocusReason);
                return true;
            }
            if (keyEvent->key() == Qt::Key_Return
                || keyEvent->key() == Qt::Key_Enter
                || keyEvent->key() == Qt::Key_Space) {
                QTreeWidgetItem *item = m_tree->currentItem();
                if (item != nullptr && !item->data(0, kHostIdRole).toString().isEmpty()) {
                    selectTreeItem(item);
                } else if (item != nullptr) {
                    item->setExpanded(!item->isExpanded());
                }
                return true;
            }
        }
        return QComboBox::eventFilter(watched, event);
    }

private:
    /**
     * @brief 将可选树节点映射为扁平模型中的 currentIndex；分组点击不改变选择。
     * @param item 用户点击或键盘激活的树节点；可为空。
     */
    void selectTreeItem(QTreeWidgetItem *item)
    {
        if (item == nullptr) {
            return;
        }
        const QString hostId = item->data(0, kHostIdRole).toString();
        if (hostId.isEmpty()) {
            return;
        }
        const int hostIndex = findData(hostId);
        if (hostIndex < 0) {
            return;
        }
        setCurrentIndex(hostIndex);
        hidePopup();
        setFocus(Qt::PopupFocusReason);
    }

    /** @brief 根据当前扁平模型 hostId 恢复树选择并展开全部祖先节点。 */
    void synchronizeTreeSelection()
    {
        const QString selectedHostId = currentData().toString();
        QTreeWidgetItemIterator iterator(m_tree);
        while (*iterator != nullptr) {
            QTreeWidgetItem *item = *iterator;
            if (item->data(0, kHostIdRole).toString() == selectedHostId) {
                for (QTreeWidgetItem *ancestor = item->parent(); ancestor != nullptr;
                     ancestor = ancestor->parent()) {
                    ancestor->setExpanded(true);
                }
                m_tree->setCurrentItem(item);
                m_tree->scrollToItem(item, QAbstractItemView::PositionAtCenter);
                return;
            }
            ++iterator;
        }
        m_tree->setCurrentItem(nullptr);
    }

    QFrame *m_popup = nullptr;
    QTreeWidget *m_tree = nullptr;
    int m_recordCount = 0;
};

/**
 * @brief 新建/编辑单条转发规则的模态表单。
 *
 * 表单只产生经过基础校验的值对象，不直接写数据库或启动网络；这样取消不会留下半保存
 * 规则，数据库外键和运行时安全确认仍由上层统一处理。
 */
class PortForwardRuleDialog final : public QDialog {
public:
    explicit PortForwardRuleDialog(const QString &hostId,
                                   const std::optional<PortForwardRule> &existing,
                                   QWidget *parent = nullptr)
        : QDialog(parent)
        , m_hostId(hostId)
        , m_id(existing.has_value()
                   ? existing->id
                   : QUuid::createUuid().toString(QUuid::WithoutBraces))
    {
        setWindowTitle(existing.has_value() ? tr("编辑端口转发") : tr("新建端口转发"));
        setMinimumWidth(480);
        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        m_nameEdit = new QLineEdit(this);
        m_typeCombo = new QComboBox(this);
        UiIcons::modernizePopup(m_typeCombo);
        m_typeCombo->addItem(tr("本地转发 (-L)"), static_cast<int>(PortForwardType::Local));
        m_typeCombo->addItem(tr("远程转发 (-R)"), static_cast<int>(PortForwardType::Remote));
        m_typeCombo->addItem(tr("动态 SOCKS5 (-D)"), static_cast<int>(PortForwardType::Dynamic));
        m_bindHostEdit = new QLineEdit(QStringLiteral("127.0.0.1"), this);
        m_bindPortSpin = new QSpinBox(this);
        m_bindPortSpin->setRange(1, 65535);
        m_bindPortSpin->setValue(8080);
        m_targetHostEdit = new QLineEdit(QStringLiteral("127.0.0.1"), this);
        m_targetPortSpin = new QSpinBox(this);
        m_targetPortSpin->setRange(1, 65535);
        m_targetPortSpin->setValue(80);
        m_autoStartCheck = new QCheckBox(tr("根 SSH 连接成功后自动启动"), this);
        // 地址与端口在语义上是同一个端点。组合为一行不仅贴近常见 host:port 写法，也让
        // 用户能够横向对照监听端点和目标端点，避免在六行表单中串错端口。
        auto *bindEndpoint = new QWidget(this);
        auto *bindEndpointLayout = new QHBoxLayout(bindEndpoint);
        bindEndpointLayout->setContentsMargins(0, 0, 0, 0);
        bindEndpointLayout->setSpacing(8);
        bindEndpointLayout->addWidget(m_bindHostEdit, 1);
        bindEndpointLayout->addWidget(new QLabel(QStringLiteral(":"), bindEndpoint));
        m_bindPortSpin->setFixedWidth(112);
        bindEndpointLayout->addWidget(m_bindPortSpin);

        auto *targetEndpoint = new QWidget(this);
        auto *targetEndpointLayout = new QHBoxLayout(targetEndpoint);
        targetEndpointLayout->setContentsMargins(0, 0, 0, 0);
        targetEndpointLayout->setSpacing(8);
        targetEndpointLayout->addWidget(m_targetHostEdit, 1);
        targetEndpointLayout->addWidget(new QLabel(QStringLiteral(":"), targetEndpoint));
        m_targetPortSpin->setFixedWidth(112);
        targetEndpointLayout->addWidget(m_targetPortSpin);

        form->addRow(tr("名称："), m_nameEdit);
        form->addRow(tr("类型："), m_typeCombo);
        form->addRow(tr("监听："), bindEndpoint);
        form->addRow(tr("目标："), targetEndpoint);
        form->addRow(QString(), m_autoStartCheck);
        layout->addLayout(form);

        auto *securityNotice = new QLabel(
            tr("安全提示：把监听地址设为 0.0.0.0 或 :: 会向局域网/公网暴露端口；"
               "是否可达仍受系统防火墙和 SSH 服务端 GatewayPorts 限制。"),
            this);
        securityNotice->setObjectName(QStringLiteral("PortForwardSecurityNotice"));
        securityNotice->setWordWrap(true);
        layout->addWidget(securityNotice);

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
            QString error;
            if (!rule().isValid(&error)) {
                QMessageBox::warning(this, tr("规则不完整"), error);
                return;
            }
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_typeCombo, &QComboBox::currentIndexChanged, this, [this](const int) {
            const bool dynamic = selectedType() == PortForwardType::Dynamic;
            m_targetHostEdit->setEnabled(!dynamic);
            m_targetPortSpin->setEnabled(!dynamic);
        });

        if (existing.has_value()) {
            m_nameEdit->setText(existing->name);
            m_typeCombo->setCurrentIndex(
                m_typeCombo->findData(static_cast<int>(existing->type)));
            m_bindHostEdit->setText(existing->bindHost);
            m_bindPortSpin->setValue(existing->bindPort);
            m_targetHostEdit->setText(existing->targetHost);
            m_targetPortSpin->setValue(existing->targetPort == 0 ? 1 : existing->targetPort);
            m_autoStartCheck->setChecked(existing->autoStart);
        }
        m_typeCombo->currentIndexChanged(m_typeCombo->currentIndex());
    }

    /** @brief 返回当前表单的完整规则；调用者只在对话框 Accepted 后持久化。 */
    [[nodiscard]] PortForwardRule rule() const
    {
        PortForwardRule value;
        value.id = m_id;
        value.hostId = m_hostId;
        value.name = m_nameEdit->text().trimmed();
        value.type = selectedType();
        value.bindHost = m_bindHostEdit->text().trimmed();
        value.bindPort = static_cast<quint16>(m_bindPortSpin->value());
        value.targetHost = value.type == PortForwardType::Dynamic
            ? QString()
            : m_targetHostEdit->text().trimmed();
        value.targetPort = value.type == PortForwardType::Dynamic
            ? 0
            : static_cast<quint16>(m_targetPortSpin->value());
        value.autoStart = m_autoStartCheck->isChecked();
        return value;
    }

private:
    /** @brief 从下拉框稳定整数值还原转发类型。 */
    [[nodiscard]] PortForwardType selectedType() const
    {
        return static_cast<PortForwardType>(m_typeCombo->currentData().toInt());
    }

    QString m_hostId;
    QString m_id;
    QLineEdit *m_nameEdit = nullptr;
    QComboBox *m_typeCombo = nullptr;
    QLineEdit *m_bindHostEdit = nullptr;
    QSpinBox *m_bindPortSpin = nullptr;
    QLineEdit *m_targetHostEdit = nullptr;
    QSpinBox *m_targetPortSpin = nullptr;
    QCheckBox *m_autoStartCheck = nullptr;
};

/** @brief 转发类型徽章使用的紧凑文本。 */
QString typeText(const PortForwardType type)
{
    switch (type) {
    case PortForwardType::Remote:
        return QStringLiteral("-R");
    case PortForwardType::Dynamic:
        return QStringLiteral("-D");
    case PortForwardType::Local:
    default:
        return QStringLiteral("-L");
    }
}

/**
 * @brief 将主机与端口格式化为不会混淆 IPv6 冒号的端点文本。
 * @param host IP、主机名或特殊监听地址。
 * @param port TCP 端口。
 * @return IPv4/主机名形如 host:port，IPv6 形如 [host]:port。
 */
QString formatEndpoint(const QString &host, const quint16 port)
{
    const QString normalizedHost = host.trimmed();
    // IPv6 地址本身包含冒号，若继续直接拼接端口会无法判断哪一段是端口；方括号形式与
    // URL/OpenSSH 的常见表示一致，同时不会改变实际持久化的监听或目标地址。
    return normalizedHost.contains(QLatin1Char(':'))
        ? QStringLiteral("[%1]:%2").arg(normalizedHost).arg(port)
        : QStringLiteral("%1:%2").arg(normalizedHost).arg(port);
}

/**
 * @brief 转发类型徽章的背景/文字配色，用半透明背景色叠加在列表项实际背景上。
 *
 * 用 rgba() 而不是"深色/浅色主题各一套固定色号"，是因为侧栏的 QPalette 不保证和应用
 * 自己选择的深浅主题同步（系统外观与应用内主题不一致时会读到错的一套）——半透明背景色
 * 由 Qt 直接跟当前实际显示的底色做 alpha 合成，不需要猜测,同一份颜色在深浅主题下都读得清。
 */
QString typeChipStyleSheet(const PortForwardType type)
{
    QString background;
    QString foreground;
    switch (type) {
    case PortForwardType::Remote:
        background = QStringLiteral("rgba(59,120,201,40)");
        foreground = QStringLiteral("#3b78c9");
        break;
    case PortForwardType::Dynamic:
        background = QStringLiteral("rgba(132,82,201,40)");
        foreground = QStringLiteral("#8452c9");
        break;
    case PortForwardType::Local:
    default:
        background = QStringLiteral("rgba(43,217,129,40)");
        foreground = QStringLiteral("#159a60");
        break;
    }
    return QStringLiteral("background:%1; color:%2; border-radius:6px; padding:1px 7px; "
                          "font-weight:700; font-size:10px;")
        .arg(background, foreground);
}

/**
 * @brief 状态点颜色；固定单一色号，不按深浅主题分支。
 *
 * 圆点是纯色实心图形，不存在"和背景对比不足"的问题，没必要为了区区一个点引入同一类
 * 深浅误判风险（参见主机树选中态曾经踩过的坑）。
 */
QColor statusDotColor(const PortForwardState state)
{
    switch (state) {
    case PortForwardState::Running:
        return QColor(QStringLiteral("#2bd981"));
    case PortForwardState::Failed:
        return QColor(QStringLiteral("#e5484d"));
    case PortForwardState::Starting:
    case PortForwardState::Stopping:
        return QColor(QStringLiteral("#e2a94f"));
    case PortForwardState::Stopped:
    default:
        return QColor(QStringLiteral("#8a94a6"));
    }
}

/**
 * @brief 把网络或 SSH 层错误整理为适合卡片直接展示的单行安全文本。
 * @param message 运行时返回的原始错误，可能包含换行或不可见控制字符。
 * @param maximumLength 允许保留的最大 UTF-16 字符数；小于 2 时返回空字符串。
 * @return 移除控制字符、合并空白并按上限截断后的文本；无有效内容时返回空字符串。
 *
 * 错误内容可能间接来自远端 SSH 服务或系统网络栈，不能原样把控制字符送进界面。卡片空间
 * 也有限，设置上限可避免异常长错误把一条规则撑满整个侧栏；完整诊断仍通过较长 Tooltip
 * 提供，且两条路径使用相同清理规则。
 */
QString displayableForwardingError(const QString &message, const qsizetype maximumLength)
{
    if (maximumLength < 2) {
        return {};
    }

    QString sanitized;
    sanitized.reserve(std::min(message.size(), maximumLength));
    for (const QChar character : message) {
        if (character.isSpace()) {
            sanitized.append(QLatin1Char(' '));
            continue;
        }
        const QChar::Category category = character.category();
        if (category == QChar::Other_Control || category == QChar::Other_Format) {
            // C0/C1、ESC 和双向文本控制符没有可读内容，还可能改变后续文字方向或视觉顺序。
            continue;
        }
        sanitized.append(character);
    }
    sanitized = sanitized.simplified();
    if (sanitized.size() > maximumLength) {
        sanitized.truncate(maximumLength - 1);
        sanitized.append(QChar(0x2026));
    }
    return sanitized;
}

/**
 * @brief 端口转发卡片式列表的单行内容：名称+类型徽章一行，地址映射+状态一行。
 *
 * 原来的四列表格在默认 320px 侧栏里要么互相挤占，要么被裁到看不清；改成两行卡片后，
 * 地址映射可以占满整行宽度，不再需要横向滚动或极窄的列。行内容由调用方（reloadRules）
 * 预先算好文本传入，本类只负责排版和配色，不重新访问数据库或规则对象。
 */
class PortForwardRuleRow final : public QWidget {
public:
    PortForwardRuleRow(const QString &name,
                       const PortForwardType type,
                       const QString &endpointText,
                       const PortForwardState state,
                       const QString &stateLabel,
                       const int activeConnections,
                       const QString &lastError,
                       QWidget *parent = nullptr)
        : QWidget(parent)
    {
        // setItemWidget() 的内容控件会覆盖 QListWidget viewport。让卡片及其子标签对鼠标
        // 透明，点击名称、地址或状态时事件才能稳定落到列表项上并建立选中态；否则只有
        // 卡片边缘的少量空白可选中，看起来就像选中记录会随机取消或失效。
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 5, 6, 5);
        layout->setSpacing(2);

        auto *topRow = new QHBoxLayout;
        auto *nameLabel = new QLabel(name, this);
        QFont nameFont = nameLabel->font();
        nameFont.setBold(true);
        nameLabel->setFont(nameFont);
        nameLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto *typeLabel = new QLabel(typeText(type), this);
        typeLabel->setStyleSheet(typeChipStyleSheet(type));
        topRow->addWidget(nameLabel, 1);
        topRow->addWidget(typeLabel, 0, Qt::AlignRight | Qt::AlignVCenter);
        layout->addLayout(topRow);

        auto *endpointLabel = new QLabel(endpointText, this);
        QFont monoFont(QStringLiteral("Menlo"));
        monoFont.setStyleHint(QFont::Monospace);
        monoFont.setPointSizeF(std::max(7.0, this->font().pointSizeF() - 1.0));
        endpointLabel->setFont(monoFont);
        endpointLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));
        layout->addWidget(endpointLabel);

        auto *statusRow = new QHBoxLayout;
        statusRow->setSpacing(5);
        auto *dotLabel = new QLabel(QStringLiteral("●"), this);
        dotLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 9px;").arg(statusDotColor(state).name()));
        // 连接数为零时不展示“· 0”，让停止、失败等静态状态保持简洁；只有存在活动连接时
        // 才追加数量，和状态本身形成清楚的主次关系。
        auto *statusLabel = new QLabel(
            activeConnections > 0
                ? tr("%1 · %2").arg(stateLabel).arg(activeConnections)
                : stateLabel,
            this);
        statusLabel->setStyleSheet(QStringLiteral("color: palette(mid); font-size: 11px;"));
        statusRow->addWidget(dotLabel, 0, Qt::AlignVCenter);
        statusRow->addWidget(statusLabel, 0, Qt::AlignVCenter);
        statusRow->addStretch(1);
        layout->addLayout(statusRow);

        // 启动失败不能只藏在 Tooltip：触屏用户没有稳定的悬停入口，截图和远程协助场景也
        // 无法知道红点为何出现。运行中某一条客户端连接失败时监听仍然有效，因此使用“最近
        // 错误”而不是“失败原因”，避免把整条规则误报为不可用。
        QString visibleError = displayableForwardingError(lastError, 220);
        if (state == PortForwardState::Failed && visibleError.isEmpty()) {
            visibleError = tr("运行时未返回具体错误信息");
        }
        if (!visibleError.isEmpty()) {
            auto *errorLabel = new QLabel(this);
            errorLabel->setObjectName(QStringLiteral("PortForwardErrorLabel"));
            errorLabel->setTextFormat(Qt::PlainText);
            errorLabel->setWordWrap(true);
            errorLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            errorLabel->setStyleSheet(
                QStringLiteral("color:#e5484d; font-size:10px; padding-top:1px;"));
            errorLabel->setText(
                state == PortForwardState::Failed
                    ? tr("失败原因：%1").arg(visibleError)
                    : tr("最近错误：%1").arg(visibleError));
            errorLabel->setToolTip(displayableForwardingError(lastError, 1000));
            layout->addWidget(errorLabel);
        }
    }
};

} // namespace

PortForwardManagerWidget::PortForwardManagerWidget(ApplicationDatabase *database,
                                                     QWidget *parent)
    : QWidget(parent)
    , m_database(database)
    , m_hostCombo(new HostTreeComboBox(this))
    , m_ruleList(new QListWidget(this))
    , m_summaryLabel(new QLabel(tr("尚无运行中的端口转发"), this))
{
    Q_ASSERT(m_database != nullptr);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *hostSection = new QVBoxLayout;
    hostSection->setSpacing(6);
    auto *hostLabel = new QLabel(tr("主机"), this);
    hostLabel->setObjectName(QStringLiteral("PortForwardHostLabel"));
    hostSection->addWidget(hostLabel);
    hostSection->addWidget(m_hostCombo);
    layout->addLayout(hostSection);
    layout->addSpacing(12);

    // 原来的四列表格在默认 320px 侧栏里放不下，改成卡片式单列列表：每条规则用
    // PortForwardRuleRow 画成两行，天然不需要横向滚动，侧栏加宽后也只是卡片变宽。
    m_ruleList->setObjectName(QStringLiteral("PortForwardRuleList"));
    m_ruleList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_ruleList->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ruleList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_ruleList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *managementButtons = new QHBoxLayout;
    managementButtons->setObjectName(QStringLiteral("PortForwardManagementLayout"));
    managementButtons->setSpacing(6);
    auto *addButton = new QPushButton(tr("新建"), this);
    addButton->setObjectName(QStringLiteral("AddPortForwardButton"));
    addButton->setProperty("primaryAction", true);
    auto *startButton = new QPushButton(tr("启动"), this);
    startButton->setObjectName(QStringLiteral("StartAllPortForwardsButton"));
    startButton->setProperty("forwardingRuntimeAction", true);
    startButton->setFixedWidth(66);
    auto *stopButton = new QPushButton(tr("停止"), this);
    stopButton->setObjectName(QStringLiteral("StopAllPortForwardsButton"));
    stopButton->setProperty("forwardingRuntimeAction", true);
    stopButton->setFixedWidth(66);
    UiIcons::decorateButton(
        addButton,
        UiIcons::symbol(this, UiIcons::Symbol::Plus, this->palette().color(QPalette::WindowText)),
        tr("新建端口转发规则"));
    UiIcons::decorateButton(
        startButton,
        UiIcons::symbol(this, UiIcons::Symbol::Play, this->palette().color(QPalette::WindowText)),
        tr("启动当前主机的全部转发规则"));
    UiIcons::decorateButton(
        stopButton,
        UiIcons::symbol(this, UiIcons::Symbol::Stop, this->palette().color(QPalette::WindowText)),
        tr("停止当前主机的全部转发规则"));
    managementButtons->addWidget(addButton);
    managementButtons->addStretch(1);
    managementButtons->addWidget(startButton);
    managementButtons->addWidget(stopButton);
    layout->addLayout(managementButtons);

    // 编辑与删除不再占用常驻空间，仍可从规则右键菜单进入。批量启停收窄后位于同一行
    // 最右侧，既保留高频入口，又把更多纵向空间交给规则列表。
    layout->addSpacing(10);
    layout->addWidget(m_ruleList, 1);

    m_summaryLabel->setObjectName(QStringLiteral("PortForwardSummary"));
    m_summaryLabel->setWordWrap(true);
    layout->addWidget(m_summaryLabel);

    connect(m_hostCombo, &QComboBox::currentIndexChanged,
            this, [this](const int) { reloadRules(); });
    connect(addButton, &QPushButton::clicked,
            this, [this]() { editRule(); });
    connect(startButton, &QPushButton::clicked,
            this, &PortForwardManagerWidget::startAllRules);
    connect(stopButton, &QPushButton::clicked,
            this, &PortForwardManagerWidget::stopAllRules);
    connect(m_ruleList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *) { startSelectedRule(); });
    connect(m_ruleList,
            &QWidget::customContextMenuRequested,
            this,
            [this, addButton](const QPoint &position) {
                QListWidgetItem *item = m_ruleList->itemAt(position);
                if (item != nullptr) {
                    m_ruleList->setCurrentItem(item);
                }

                QMenu menu(this);
                UiIcons::modernizePopup(&menu);
                QAction *addAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Plus, this->palette().color(QPalette::WindowText)),
                    tr("新建规则"));
                connect(addAction, &QAction::triggered, addButton, &QPushButton::click);
                if (item != nullptr) {
                    menu.addSeparator();
                    QAction *startAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Play, this->palette().color(QPalette::WindowText)),
                        tr("启动"));
                    QAction *stopAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Stop, this->palette().color(QPalette::WindowText)),
                        tr("停止"));
                    QAction *editAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Edit, this->palette().color(QPalette::WindowText)),
                        tr("编辑"));
                    QAction *deleteAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Trash, this->palette().color(QPalette::WindowText)),
                        tr("删除"));
                    // 常驻编辑/删除按钮已移除，右键菜单直接调用单条规则方法；启动/停止也
                    // 必须保持单条语义，不能借用顶部针对当前主机全部规则的批量按钮。
                    connect(startAction, &QAction::triggered,
                            this, &PortForwardManagerWidget::startSelectedRule);
                    connect(stopAction, &QAction::triggered,
                            this, &PortForwardManagerWidget::stopSelectedRule);
                    connect(editAction, &QAction::triggered, this, [this]() {
                        QString error;
                        const std::optional<PortForwardRule> rule =
                            m_database->findPortForward(selectedRuleId(), &error);
                        if (!rule.has_value()) {
                            emit statusMessage(error.isEmpty()
                                                   ? tr("请选择要编辑的规则")
                                                   : error);
                            return;
                        }
                        editRule(rule);
                    });
                    connect(deleteAction, &QAction::triggered,
                            this, &PortForwardManagerWidget::removeSelectedRule);
                }
                menu.exec(m_ruleList->viewport()->mapToGlobal(position));
            });

    reloadHosts();
}

PortForwardManagerWidget::~PortForwardManagerWidget()
{
    // 运行时网络线程持有非拥有的 SshSessionStack 指针。侧栏可能先于中央标签析构，因此
    // 必须在自身析构中同步清理，不能把它们留给栈 QObject 的子对象清理阶段。
    const QList<QPointer<PortForwardRuntime>> runtimes = m_runtimes.values();
    m_runtimes.clear();
    for (PortForwardRuntime *runtime : runtimes) {
        if (runtime != nullptr) {
            delete runtime;
        }
    }
}

void PortForwardManagerWidget::registerSession(const QString &hostId,
                                                SshSessionStack *sessionStack)
{
    if (hostId.isEmpty() || sessionStack == nullptr) {
        return;
    }
    QList<QPointer<SshSessionStack>> &sessions = m_sessions[hostId];
    const bool alreadyRegistered = std::any_of(
        sessions.cbegin(), sessions.cend(),
        [sessionStack](const QPointer<SshSessionStack> &candidate) {
            return candidate == sessionStack;
        });
    if (!alreadyRegistered) {
        sessions.append(sessionStack);
        // QObject::UniqueConnection 对 lambda 不生效，因此只在首次登记准确对象时创建这两条
        // 连接。这样 shellConnected 重复通知也不会让一个 fd 被分发多次。
        connect(sessionStack, &QObject::destroyed, this,
                [this, hostId, sessionStack]() { unregisterSession(hostId, sessionStack); });
        connect(sessionStack,
                &SshSessionStack::remoteForwardConnection,
                this,
                [this, sessionStack](const QString &ruleId,
                                     const NativeSocketHandle tunnelFd) {
                    dispatchRemoteConnection(sessionStack, ruleId, tunnelFd);
                },
                Qt::QueuedConnection);
    }
    startAutomaticRules(hostId, sessionStack);
    reloadRules();
}

void PortForwardManagerWidget::unregisterSession(const QString &hostId,
                                                  SshSessionStack *sessionStack)
{
    QList<QPointer<SshSessionStack>> &sessions = m_sessions[hostId];
    sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                  [sessionStack](const QPointer<SshSessionStack> &candidate) {
                                      return candidate.isNull() || candidate == sessionStack;
                                  }),
                   sessions.end());
    if (sessions.isEmpty()) {
        m_sessions.remove(hostId);
    }

    for (auto iterator = m_runtimes.begin(); iterator != m_runtimes.end();) {
        PortForwardRuntime *runtime = iterator.value();
        if (runtime == nullptr) {
            iterator = m_runtimes.erase(iterator);
            continue;
        }
        if (runtime->rule().hostId == hostId
            && runtime->parent() == sessionStack) {
            iterator = m_runtimes.erase(iterator);
            // forwardingSessionAboutToClose 在 SSH 栈收到 requestStop 后、实际析构前同步
            // 发出；此处同步析构网络线程，保证其不再调用即将失效的栈对象。
            delete runtime;
        } else {
            ++iterator;
        }
    }

    SshSessionStack *replacement = availableSession(hostId);
    if (replacement != nullptr) {
        // 即使 QObject::destroyed 到达时旧 Runtime 已作为栈子对象被清理，也要重新扫描全部
        // autoStart 规则；startRule 会幂等跳过仍在运行的规则。
        startAutomaticRules(hostId, replacement);
    }
    reloadRules();
}

void PortForwardManagerWidget::dispatchRemoteConnection(SshSessionStack *sessionStack,
                                                         const QString &ruleId,
                                                         const NativeSocketHandle tunnelFd)
{
    if (!isNativeSocketValid(tunnelFd)) {
        return;
    }
    PortForwardRuntime *runtime = m_runtimes.value(ruleId);
    if (runtime == nullptr || runtime->parent() != sessionStack
        || runtime->rule().type != PortForwardType::Remote) {
        // 规则可能在服务端接受 channel 与 GUI 收到排队信号之间被停止、删除或迁移。管理器
        // 是信号的唯一接收者，因此找不到准确所有者时必须在此关闭，避免长期泄漏 fd。
        closeNativeSocket(tunnelFd);
        return;
    }
    runtime->acceptRemoteConnection(tunnelFd);
}

void PortForwardManagerWidget::refresh()
{
    reloadHosts();
}

void PortForwardManagerWidget::reloadHosts()
{
    const QString previousHostId = m_hostCombo->currentData().toString();
    bool hostsLoaded = false;
    QString error;
    const QList<HostRecord> records = m_database->searchHosts({}, &hostsLoaded, &error);
    if (!hostsLoaded) {
        emit statusMessage(tr("读取端口转发主机失败：%1").arg(error));
        reloadRules();
        return;
    }

    bool countsLoaded = false;
    const QHash<QString, int> forwardingCounts =
        m_database->portForwardCounts(&countsLoaded, &error);
    if (!countsLoaded) {
        emit statusMessage(tr("读取端口转发数量失败：%1").arg(error));
        reloadRules();
        return;
    }

    // m_hostCombo 在构造时固定为 HostTreeComboBox。保留基类成员类型可减少头文件对具体 UI
    // 实现的暴露，但这里必须调用专用树模型同步入口；qobject_cast 不适用于无 Q_OBJECT 的
    // 私有实现类，因此使用受构造不变量保护的 static_cast。
    auto *hostTreeCombo = static_cast<HostTreeComboBox *>(m_hostCombo);
    hostTreeCombo->setHostRecords(records, forwardingCounts, previousHostId);
    reloadRules();
}

void PortForwardManagerWidget::reloadRules()
{
    const QString previouslySelectedRuleId = selectedRuleId();
    // QListWidget::clear() 只删除 QListWidgetItem 本身，不会删除 setItemWidget() 挂上去的
    // 卡片控件——那是单独 reparent 出去的子控件，clear() 之后仍留在列表里泄漏，且残留内容
    // 还会被 findChildren 之类的遍历重新找到。必须先逐个摘下并手动删除，再清空条目。
    for (int row = 0; row < m_ruleList->count(); ++row) {
        QListWidgetItem *item = m_ruleList->item(row);
        QWidget *oldRowWidget = m_ruleList->itemWidget(item);
        m_ruleList->removeItemWidget(item);
        delete oldRowWidget;
    }
    m_ruleList->clear();
    const QString hostId = m_hostCombo->currentData().toString();
    if (hostId.isEmpty()) {
        m_summaryLabel->setText(tr("请先在主机管理中保存一台主机"));
        return;
    }
    bool ok = false;
    QString error;
    const QList<PortForwardRule> rules = m_database->listPortForwards(hostId, &ok, &error);
    if (!ok) {
        emit statusMessage(tr("读取端口转发规则失败：%1").arg(error));
        return;
    }

    quint64 activeConnections = 0;
    quint64 totalTraffic = 0;
    QListWidgetItem *restoredSelection = nullptr;
    for (const PortForwardRule &rule : rules) {
        const PortForwardStatistics statistics = m_statistics.value(rule.id);

        auto *item = new QListWidgetItem(m_ruleList);
        item->setData(kRuleIdRole, rule.id);
        auto *row = new PortForwardRuleRow(rule.name,
                                           rule.type,
                                           endpointMappingText(rule),
                                           statistics.state,
                                           stateText(statistics.state),
                                           statistics.activeConnections,
                                           statistics.lastError,
                                           m_ruleList);
        item->setSizeHint(row->sizeHint());
        m_ruleList->setItemWidget(item, row);
        if (!previouslySelectedRuleId.isEmpty() && rule.id == previouslySelectedRuleId) {
            restoredSelection = item;
        }

        const quint64 traffic = statistics.bytesToTarget + statistics.bytesFromTarget;
        activeConnections += statistics.activeConnections;
        totalTraffic += traffic;
    }

    // 运行时统计、主机连接状态或数据库保存都会重建卡片列表。按稳定 ruleId 恢复选择，
    // 避免用户刚选中的记录因异步状态刷新突然取消；规则已被删除或切换主机时则自然不恢复。
    if (restoredSelection != nullptr) {
        m_ruleList->setCurrentItem(restoredSelection,
                                   QItemSelectionModel::ClearAndSelect);
        m_ruleList->scrollToItem(restoredSelection, QAbstractItemView::EnsureVisible);
    }
    m_summaryLabel->setText(
        tr("%1 条规则，%2 条活动连接，累计流量 %3；%4")
            .arg(rules.size())
            .arg(activeConnections)
            .arg(formatBytes(totalTraffic))
            .arg(availableSession(hostId) == nullptr
                     ? tr("主机未连接 · SSH 会话不可用")
                     : tr("主机已连接 · SSH 会话可用")));
}

void PortForwardManagerWidget::editRule(const std::optional<PortForwardRule> &existing)
{
    const QString hostId = m_hostCombo->currentData().toString();
    if (hostId.isEmpty()) {
        QMessageBox::information(this, tr("没有保存主机"), tr("请先在主机管理中创建主机。"));
        return;
    }
    if (existing.has_value() && m_runtimes.contains(existing->id)
        && m_runtimes.value(existing->id) != nullptr
        && m_runtimes.value(existing->id)->statistics().state == PortForwardState::Running) {
        QMessageBox::information(this,
                                 tr("规则正在运行"),
                                 tr("请先停止规则，再修改监听或目标参数。"));
        return;
    }
    PortForwardRuleDialog dialog(hostId, existing, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const PortForwardRule rule = dialog.rule();
    const bool publiclyExposed = rule.bindHost == QStringLiteral("0.0.0.0")
        || rule.bindHost == QStringLiteral("::");
    if (publiclyExposed
        && QMessageBox::warning(this,
                                tr("确认暴露监听端口"),
                                tr("规则会监听所有网络接口，其他设备可能访问该端口。"
                                   "确认已配置防火墙并继续保存吗？"),
                                QMessageBox::Yes | QMessageBox::Cancel,
                                QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    QString error;
    if (!m_database->savePortForward(rule, &error)) {
        QMessageBox::critical(this, tr("保存规则失败"), error);
        return;
    }
    // 新增规则后主机名称后的数量也必须立即变化；reloadHosts 会保留当前 hostId，并在一次
    // 聚合查询后同步树和规则卡片，避免数量要等到外部刷新才更新。
    reloadHosts();
}

void PortForwardManagerWidget::removeSelectedRule()
{
    const QString ruleId = selectedRuleId();
    if (ruleId.isEmpty()) {
        emit statusMessage(tr("请选择要删除的端口转发规则"));
        return;
    }
    if (QMessageBox::warning(this,
                             tr("确认删除规则"),
                             tr("删除后无法撤销；正在运行的监听会立即停止。"),
                             QMessageBox::Yes | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    if (PortForwardRuntime *runtime = m_runtimes.take(ruleId); runtime != nullptr) {
        delete runtime;
    }
    int removed = 0;
    QString error;
    if (!m_database->removePortForward(ruleId, &removed, &error)) {
        QMessageBox::critical(this, tr("删除规则失败"), error);
        return;
    }
    m_statistics.remove(ruleId);
    // 删除规则同时影响主机树数量徽标，因此统一重载主机树并保留当前主机选择。
    reloadHosts();
}

void PortForwardManagerWidget::startSelectedRule()
{
    const QString ruleId = selectedRuleId();
    QString error;
    const std::optional<PortForwardRule> rule = m_database->findPortForward(ruleId, &error);
    if (!rule.has_value()) {
        emit statusMessage(error.isEmpty() ? tr("请选择要启动的规则") : error);
        return;
    }
    SshSessionStack *sessionStack = availableSession(rule->hostId);
    if (sessionStack == nullptr) {
        QMessageBox::information(this,
                                 tr("主机尚未连接"),
                                 tr("请先从主机树连接“%1”，再启动这条规则。")
                                     .arg(m_hostCombo->currentText()));
        return;
    }
    if (!startRule(*rule, sessionStack, &error)) {
        QMessageBox::critical(this, tr("启动转发失败"), error);
    }
}

void PortForwardManagerWidget::stopSelectedRule()
{
    const QString ruleId = selectedRuleId();
    PortForwardRuntime *runtime = m_runtimes.value(ruleId);
    if (runtime == nullptr) {
        emit statusMessage(tr("所选规则当前未运行"));
        return;
    }
    runtime->stop();
    reloadRules();
}

void PortForwardManagerWidget::startAllRules()
{
    const QString hostId = m_hostCombo->currentData().toString();
    if (hostId.isEmpty()) {
        QMessageBox::information(this,
                                 tr("没有保存主机"),
                                 tr("请先在主机管理中创建并选择一台主机。"));
        return;
    }

    bool ok = false;
    QString error;
    const QList<PortForwardRule> rules = m_database->listPortForwards(hostId, &ok, &error);
    if (!ok) {
        QMessageBox::critical(this, tr("读取转发规则失败"), error);
        return;
    }
    if (rules.isEmpty()) {
        QMessageBox::information(this,
                                 tr("没有转发规则"),
                                 tr("当前主机还没有可启动的端口转发规则。"));
        return;
    }

    SshSessionStack *sessionStack = availableSession(hostId);
    if (sessionStack == nullptr) {
        QMessageBox::information(this,
                                 tr("主机尚未连接"),
                                 tr("请先从主机树连接“%1”，再启动全部转发规则。")
                                     .arg(m_hostCombo->currentText()));
        return;
    }

    if (QMessageBox::question(
            this,
            tr("确认启动全部转发"),
            tr("将启动主机“%1”下的全部 %2 条转发规则；已在运行的规则会保持不变。是否继续？")
                .arg(m_hostCombo->currentText())
                .arg(rules.size()),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    int failedCount = 0;
    QStringList failedRules;
    for (const PortForwardRule &rule : rules) {
        QString startError;
        if (!startRule(rule, sessionStack, &startError)) {
            ++failedCount;
            failedRules.append(tr("%1：%2").arg(rule.name, startError));
        }
    }
    reloadRules();

    // startRule 只负责同步前置校验，真正的监听与 SSH 请求异步执行；异步失败仍会由
    // statisticsChanged 更新行状态。这里仅汇总在提交阶段就能确定的失败，避免误报成功。
    if (failedCount > 0) {
        QMessageBox::warning(
            this,
            tr("部分规则未能启动"),
            tr("%1 条规则未能提交启动：\n%2")
                .arg(failedCount)
                .arg(failedRules.join(QLatin1Char('\n'))));
        return;
    }
    emit statusMessage(tr("已提交当前主机的 %1 条端口转发启动任务").arg(rules.size()));
}

void PortForwardManagerWidget::stopAllRules()
{
    const QString hostId = m_hostCombo->currentData().toString();
    if (hostId.isEmpty()) {
        QMessageBox::information(this,
                                 tr("没有保存主机"),
                                 tr("请先在主机管理中创建并选择一台主机。"));
        return;
    }

    bool ok = false;
    QString error;
    const QList<PortForwardRule> rules = m_database->listPortForwards(hostId, &ok, &error);
    if (!ok) {
        QMessageBox::critical(this, tr("读取转发规则失败"), error);
        return;
    }

    QList<PortForwardRuntime *> activeRuntimes;
    for (const PortForwardRule &rule : rules) {
        PortForwardRuntime *runtime = m_runtimes.value(rule.id);
        if (runtime == nullptr) {
            continue;
        }
        const PortForwardState state = runtime->statistics().state;
        // 新建运行时的异步 Starting 通知可能尚未回到 GUI，此时 m_statistics 中还没有条目；
        // 把这种对象也视为活动运行时，确保用户紧接着点击“停止全部”不会漏掉排队中的启动。
        if (!m_statistics.contains(rule.id) || state != PortForwardState::Stopped) {
            activeRuntimes.append(runtime);
        }
    }
    if (activeRuntimes.isEmpty()) {
        emit statusMessage(tr("当前主机没有正在运行的端口转发规则"));
        return;
    }

    if (QMessageBox::question(
            this,
            tr("确认停止全部转发"),
            tr("将停止主机“%1”下的 %2 条活动转发规则，并关闭相关连接。是否继续？")
                .arg(m_hostCombo->currentText())
                .arg(activeRuntimes.size()),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    // 每个 runtime 在自己的网络线程异步收尾；逐一提交 stop 不会阻塞 GUI，也不会改变或
    // 删除数据库中的规则。统计信号返回后，表格会分别进入“停止中”和“已停止”状态。
    for (PortForwardRuntime *runtime : std::as_const(activeRuntimes)) {
        runtime->stop();
    }
    reloadRules();
    emit statusMessage(tr("已提交当前主机的 %1 条端口转发停止任务")
                           .arg(activeRuntimes.size()));
}

QString PortForwardManagerWidget::selectedRuleId() const
{
    QListWidgetItem *item = m_ruleList->currentItem();
    return item == nullptr ? QString() : item->data(kRuleIdRole).toString();
}

SshSessionStack *PortForwardManagerWidget::availableSession(const QString &hostId)
{
    QList<QPointer<SshSessionStack>> &sessions = m_sessions[hostId];
    sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                  [](const QPointer<SshSessionStack> &session) {
                                      return session.isNull() || !session->isRunning();
                                  }),
                   sessions.end());
    if (sessions.isEmpty()) {
        m_sessions.remove(hostId);
        return nullptr;
    }
    return sessions.constFirst().data();
}

bool PortForwardManagerWidget::startRule(const PortForwardRule &rule,
                                         SshSessionStack *sessionStack,
                                         QString *errorMessage)
{
    QString validationError;
    if (!rule.isValid(&validationError)) {
        // 数据库通常已保证字段合法，但导入旧版本数据或未来迁移异常时仍可能到达这里；在
        // 创建网络线程前再次校验，可以让单条和批量启动都得到可定位错误而不是触发断言。
        if (errorMessage != nullptr) {
            *errorMessage = validationError;
        }
        return false;
    }
    if (sessionStack == nullptr || !sessionStack->isRunning()) {
        // Runtime 构造函数要求非空会话，且异步启动若接到已断开的会话只会稍后失败。这里
        // 同步拒绝可让批量汇总准确指出前置条件，并关闭会话断开与点击启动之间的竞态窗口。
        if (errorMessage != nullptr) {
            *errorMessage = tr("主机 SSH 会话不可用");
        }
        return false;
    }

    if (PortForwardRuntime *existing = m_runtimes.value(rule.id); existing != nullptr) {
        const PortForwardState state = existing->statistics().state;
        if (state == PortForwardState::Running || state == PortForwardState::Starting) {
            return true;
        }
        m_runtimes.remove(rule.id);
        delete existing;
    }
    auto *runtime = new PortForwardRuntime(rule, sessionStack, sessionStack);
    m_runtimes.insert(rule.id, runtime);
    connect(runtime,
            &PortForwardRuntime::statisticsChanged,
            this,
            &PortForwardManagerWidget::applyRuntimeStatistics);
    connect(runtime,
            &PortForwardRuntime::statusMessage,
            this,
            &PortForwardManagerWidget::statusMessage);
    // Runtime 的本地监听和远程请求都在专属网络线程异步执行；字段与会话前置条件已在
    // 本方法完成，后续 bind/服务端拒绝通过 statisticsChanged/statusMessage 明确上报。
    runtime->start();
    emit statusMessage(tr("正在启动端口转发：%1").arg(rule.name));
    return true;
}

void PortForwardManagerWidget::applyRuntimeStatistics(
    const QString &ruleId,
    const PortForwardStatistics &statistics)
{
    if (ruleId.isEmpty()) {
        // 缺少稳定 id 的统计无法安全归属到任何卡片；忽略比污染另一条规则的状态更可靠。
        return;
    }
    m_statistics.insert(ruleId, statistics);
    reloadRules();
    if (statistics.state == PortForwardState::Failed) {
        const QString reason = displayableForwardingError(statistics.lastError, 1000);
        emit statusMessage(
            tr("端口转发失败：%1")
                .arg(reason.isEmpty() ? tr("运行时未返回具体错误信息") : reason));
    }
}

void PortForwardManagerWidget::startAutomaticRules(const QString &hostId,
                                                    SshSessionStack *sessionStack)
{
    bool ok = false;
    QString error;
    const QList<PortForwardRule> rules = m_database->listPortForwards(hostId, &ok, &error);
    if (!ok) {
        emit statusMessage(tr("读取自动转发规则失败：%1").arg(error));
        return;
    }
    for (const PortForwardRule &rule : rules) {
        if (rule.autoStart) {
            QString startError;
            if (!startRule(rule, sessionStack, &startError)) {
                emit statusMessage(tr("自动启动“%1”失败：%2").arg(rule.name, startError));
            }
        }
    }
}

QString PortForwardManagerWidget::stateText(const PortForwardState state)
{
    switch (state) {
    case PortForwardState::Starting:
        return tr("启动中");
    case PortForwardState::Running:
        return tr("运行中");
    case PortForwardState::Stopping:
        return tr("停止中");
    case PortForwardState::Failed:
        return tr("失败");
    case PortForwardState::Stopped:
    default:
        return tr("已停止");
    }
}

QString PortForwardManagerWidget::endpointMappingText(const PortForwardRule &rule)
{
    const QString bindEndpoint = formatEndpoint(rule.bindHost, rule.bindPort);
    switch (rule.type) {
    case PortForwardType::Remote:
        // -R 在远程 SSH 服务器监听 bind，在本机连接 target；反向箭头用于避免把两端角色
        // 错画成本地转发，同时第二列始终保持“本机在前、远程在后”的阅读顺序。
        return tr("本机 %1\n← 远程 %2")
            .arg(formatEndpoint(rule.targetHost, rule.targetPort), bindEndpoint);
    case PortForwardType::Dynamic:
        // SOCKS5 的远程目标由每个客户端请求动态给出，不能展示一个并不存在的固定端口。
        return tr("本机 %1\n→ 远程（按 SOCKS 请求）").arg(bindEndpoint);
    case PortForwardType::Local:
    default:
        return tr("本机 %1\n→ 远程 %2")
            .arg(bindEndpoint, formatEndpoint(rule.targetHost, rule.targetPort));
    }
}

QString PortForwardManagerWidget::formatBytes(const quint64 bytes)
{
    static const QStringList units{
        QStringLiteral("B"), QStringLiteral("KiB"), QStringLiteral("MiB"),
        QStringLiteral("GiB"), QStringLiteral("TiB")};
    double value = static_cast<double>(bytes);
    qsizetype unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? tr("%1 %2").arg(bytes).arg(units.at(unit))
                     : tr("%1 %2").arg(value, 0, 'f', 1).arg(units.at(unit));
}
