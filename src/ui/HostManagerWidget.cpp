#include "ui/HostManagerWidget.h"

#include "core/ConnectionConfigCodec.h"
#include "security/Vault.h"
#include "ssh/SshSessionStack.h"
#include "storage/ApplicationDatabase.h"
#include "ui/SettingsWidgets.h"
#include "ui/UiIcons.h"
#include "ui/VaultDialog.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFile>
#include <QFileIconProvider>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QPushButton>
#include <QSet>
#include <QSaveFile>
#include <QSpinBox>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {

/** @brief 树节点保存数据库主键的角色，避开 Qt 内置显示和图标角色。 */
constexpr int HostIdRole = Qt::UserRole;

/** @brief 树节点保存 HostNodeKind 数值的角色，用于阻止分组被误连接。 */
constexpr int HostKindRole = Qt::UserRole + 1;

/** @brief 主机节点保存”账号@地址:端口”副标题的角色；分组节点不设置该角色。 */
constexpr int HostDetailRole = Qt::UserRole + 2;

/**
 * @brief “未分组”分区标题伪节点在 HostKindRole 里存放的哨兵值。
 *
 * 故意取 HostNodeKind 枚举范围之外的负数，而不是新增 HostNodeKind 取值：这个节点只是树的
 * 展示结构，不对应任何数据库记录，混进持久化枚举会让 saveHost/resolveHost 等代码被迫处理
 * 一个不存在的”分组/主机”之外的第三种 kind。
 */
constexpr int kSectionHeaderKindValue = -1;

/**
 * @brief 创建一个不对应任何数据库记录的分区标题伪节点（如"未分组"）。
 * @param text 分区标题文字。
 * @return 不可选中、不可编辑、不接受拖放的 QTreeWidgetItem；调用方负责把它加到树里。
 */
[[nodiscard]] QTreeWidgetItem *createSectionHeaderItem(const QString &text)
{
    auto *item = new QTreeWidgetItem;
    item->setText(0, text);
    item->setData(0, HostKindRole, kSectionHeaderKindValue);
    item->setFlags(item->flags()
                   & ~(Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled
                       | Qt::ItemIsEditable));
    return item;
}

/**
 * @brief 返回与“本地文件”列表一致的操作系统文件夹图标。
 * @param context 图标主题缺失时提供自绘回退和调色板颜色的界面控件，不得为空。
 * @return QFileSystemModel 同源的系统文件夹图标；平台未提供时退回 Shellium 线性文件夹。
 *
 * 本地文件树由 QFileSystemModel 驱动，它内部同样通过 QFileIconProvider 获取文件夹图标。
 * 主机分组直接使用这一来源，才能在 macOS 上呈现相同的蓝色系统文件夹，并在 Windows、
 * Linux 上自然跟随各自桌面主题，而不是复制一份只在当前平台看起来相近的图形。
 */
[[nodiscard]] QIcon localFileFolderIcon(const QWidget *context)
{
    Q_ASSERT(context != nullptr);
    static QFileIconProvider iconProvider;
    const QIcon platformFolder = iconProvider.icon(QFileIconProvider::Folder);
    return platformFolder.isNull()
        ? UiIcons::symbol(
              context,
              UiIcons::Symbol::Folder,
              context->palette().color(QPalette::WindowText))
        : platformFolder;
}

/** @brief 在保持像素/点两种字号语义的前提下缩放字体，避免 pointSizeF 为 -1 时算出 0 号字。 */
QFont scaledFont(QFont font, const double deltaPoints, const bool bold)
{
    font.setBold(bold);
    if (font.pointSizeF() > 0.0) {
        font.setPointSizeF(std::max(7.0, font.pointSizeF() + deltaPoints));
    } else if (font.pixelSize() > 0) {
        font.setPixelSize(std::max(9, font.pixelSize() + qRound(deltaPoints * 4.0 / 3.0)));
    }
    return font;
}

/** @brief 节点名称行的字体；分组与主机统一放大一号，但保持常规字重以减少视觉压迫。 */
[[nodiscard]] QFont itemNameFont(const QFont &base)
{
    return scaledFont(base, 1.0, false);
}

/** @brief 分组展开箭头的边长（像素）。 */
constexpr int kToggleArrowSize = 8;

/** @brief 展开箭头与行右端之间的留白。 */
constexpr int kToggleArrowMargin = 6;

/** @brief 分组名称右端与展开箭头之间至少要留出的空隙。 */
constexpr int kToggleArrowSpacing = 6;

/**
 * @brief 分组展开按钮的最小横向点击宽度（像素）。
 *
 * 箭头本身只有 8 像素，若直接用绘制矩形做命中区，用户必须精确点击三角形。36 像素与
 * 当前分组行高度接近，可形成完整、易点击的右侧按钮区域，同时不会占用大段名称区域。
 */
constexpr int kToggleHitAreaWidth = 36;

/** @brief 判断索引指向的是否为分组节点。 */
[[nodiscard]] bool isFolderIndex(const QModelIndex &index)
{
    return static_cast<HostNodeKind>(index.data(HostKindRole).toInt()) == HostNodeKind::Folder;
}

/**
 * @brief 把主机节点画成“大号名称 + 小号连接信息”两行的委托。
 *
 * 单行只显示名称时，用户必须逐个悬停才知道某条记录连的是哪个账号和端口，同名不同环境的
 * 主机因此极易连错。分组只有一行名称，但展开箭头由本委托画在行的右端并纵向居中，而不是
 * Qt 默认的左侧分支装饰位——那条装饰列会在树的左边留下一条只为箭头存在的空白。
 */
class HostItemDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    /** @brief 点中分组右侧展开按钮区域时触发；由外部接上真正的展开/收起。 */
    std::function<void(const QModelIndex &)> onToggleRequested;

    /**
     * @brief 计算分组展开箭头的实际绘制矩形。
     * @return 非分组节点、或没有子节点的空分组返回空矩形（不画也点不中）。
     */
    [[nodiscard]] QRect toggleArrowRect(const QStyleOptionViewItem &option,
                                        const QModelIndex &index) const
    {
        if (!isFolderIndex(index) || index.model() == nullptr
            || index.model()->rowCount(index) == 0) {
            return {};
        }

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QRect textRect = itemTextRect(opt);
        // 箭头贴着行的右端，而不是跟着名字长度浮动：分组名长短不一时，一列对齐的箭头才
        // 读得出"这些都是可展开的分组"。纵向以整行为准居中，与名称基线无关。
        return QRect(textRect.right() - kToggleArrowMargin - kToggleArrowSize + 1,
                     opt.rect.center().y() - (kToggleArrowSize / 2),
                     kToggleArrowSize,
                     kToggleArrowSize);
    }

    /**
     * @brief 计算分组右侧展开按钮的完整鼠标命中区域。
     * @param option 当前树节点的样式与几何信息。
     * @param index 当前树节点索引。
     * @return 覆盖行右侧完整高度、且至少包含箭头的矩形；不可展开节点返回空矩形。
     *
     * 点击热区与绘制矩形必须分开：视觉上保留轻量小箭头，交互上则让箭头周围的整块区域
     * 都可点击。使用整行高度还能避免用户在箭头上方或下方几像素处点击却没有响应。
     */
    [[nodiscard]] QRect toggleHitRect(const QStyleOptionViewItem &option,
                                      const QModelIndex &index) const
    {
        const QRect arrowRect = toggleArrowRect(option, index);
        if (arrowRect.isNull()) {
            return {};
        }

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const int hitLeft = std::max(opt.rect.left(),
                                     opt.rect.right() - kToggleHitAreaWidth + 1);
        QRect hitRect(hitLeft,
                      opt.rect.top(),
                      opt.rect.right() - hitLeft + 1,
                      opt.rect.height());
        // 不同平台样式可能给文字区配置不同的右侧内边距。合并箭头矩形可保证无论主题如何
        // 调整，视觉箭头永远完全落在热区内；最后裁回当前行，避免抢占相邻节点的事件。
        return hitRect.united(arrowRect).intersected(opt.rect);
    }

protected:
    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (index.data(HostKindRole).toInt() == kSectionHeaderKindValue) {
            paintSectionHeader(painter, option, index);
            return;
        }
        if (isFolderIndex(index)) {
            paintFolder(painter, option, index);
            return;
        }

        const QString detail = index.data(HostDetailRole).toString();
        if (detail.isEmpty()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        // 选中态改为委托自绘的"淡底纹 + 描边 + 左侧强调条"，不再借用样式的高对比选中色；
        // 焦点框等其余装饰仍交给样式，只是先摘掉 State_Selected，避免样式再叠一层旧背景。
        const bool selected = (option.state & QStyle::State_Selected) != 0;
        QStyleOptionViewItem background = option;
        initStyleOption(&background, index);
        background.text.clear();
        background.state &= ~QStyle::State_Selected;
        const QWidget *widget = option.widget;
        QStyle *style = widget != nullptr ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);
        if (selected) {
            paintSelectionBackground(painter, option.rect);
        }
        paintGuideLine(painter, option, index);

        const QRect contentRect =
            style->subElementRect(QStyle::SE_ItemViewItemText, &background, widget);
        const QRect iconRect =
            style->subElementRect(QStyle::SE_ItemViewItemDecoration, &background, widget);
        if (!iconRect.isEmpty()) {
            qvariant_cast<QIcon>(index.data(Qt::DecorationRole))
                .paint(painter, iconRect, Qt::AlignCenter);
        }

        const QFont nameFont = itemNameFont(option.font);
        const QFont detailFont = scaledFont(option.font, -1.5, false);
        const QFontMetrics nameMetrics(nameFont);
        const QFontMetrics detailMetrics(detailFont);
        // 选中态不再反相成高亮文字色：底纹很浅，白色高亮文字反而对比不足，正常文字色在
        // 浅色底纹上已经足够醒目，也让选中前后文字颜色保持一致、不"跳色"。
        const QColor baseColor = background.palette.color(QPalette::Text);

        painter->save();
        const QRect nameRect(contentRect.left(),
                             contentRect.top(),
                             contentRect.width(),
                             nameMetrics.height());
        painter->setFont(nameFont);
        painter->setPen(baseColor);
        painter->drawText(nameRect,
                          Qt::AlignLeft | Qt::AlignVCenter,
                          nameMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                                 Qt::ElideRight,
                                                 nameRect.width()));

        QColor detailColor = baseColor;
        detailColor.setAlphaF(0.62F);
        const QRect detailRect(contentRect.left(),
                               nameRect.bottom() + 1,
                               contentRect.width(),
                               detailMetrics.height());
        painter->setFont(detailFont);
        painter->setPen(detailColor);
        painter->drawText(detailRect,
                          Qt::AlignLeft | Qt::AlignVCenter,
                          detailMetrics.elidedText(detail, Qt::ElideMiddle, detailRect.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        if (index.data(HostKindRole).toInt() == kSectionHeaderKindValue) {
            // 分区标题只是一行小字加一条分隔线，不需要按名称字体放大行高。
            hint.setHeight(std::max(hint.height(), QFontMetrics(option.font).height() + 14));
            return hint;
        }
        if (index.data(HostDetailRole).toString().isEmpty()) {
            // 分组行按放大后的名称字体留高，否则 Qt 用基准字体算出的行高会切掉字的上下缘。
            hint.setHeight(
                std::max(hint.height(), QFontMetrics(itemNameFont(option.font)).height() + 8));
            return hint;
        }
        const int textHeight = QFontMetrics(itemNameFont(option.font)).height()
            + QFontMetrics(scaledFont(option.font, -1.5, false)).height() + 1;
        hint.setHeight(std::max(hint.height(), textHeight + 8));
        return hint;
    }

    bool editorEvent(QEvent *event,
                     QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        // 箭头已不在 Qt 的分支装饰列里，QTreeView 自带的“点击分支即展开”对它无效，必须由
        // 委托自己做命中判断。热区使用右侧完整按钮区域而不是 8×8 的三角形；与复选框一致
        // 地取释放事件，按下后移开鼠标就不会误触发。
        if (event != nullptr && event->type() == QEvent::MouseButtonRelease && onToggleRequested) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton
                && toggleHitRect(option, index).contains(mouseEvent->position().toPoint())) {
                onToggleRequested(index);
                return true;
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

private:
    /** @brief 样式为文字保留的区域；分组与主机共用同一份计算。 */
    [[nodiscard]] static QRect itemTextRect(const QStyleOptionViewItem &option)
    {
        QStyle *style =
            option.widget != nullptr ? option.widget->style() : QApplication::style();
        return style->subElementRect(QStyle::SE_ItemViewItemText, &option, option.widget);
    }

    /**
     * @brief 扣掉右侧按钮热区及安全间距后，计算分组名称可用宽度。
     * @param textRect 样式为节点文字提供的完整矩形。
     * @param hitRect 展开按钮的鼠标命中矩形。
     * @return 名称可绘制宽度；空间不足时返回 0。
     *
     * 名称必须避开完整热区而不只是小箭头，否则长名称会画进可点击区域，用户点击文字末尾
     * 时就会意外展开或收起分组。
     */
    [[nodiscard]] static int availableNameWidth(const QRect &textRect, const QRect &hitRect)
    {
        if (hitRect.isNull()) {
            return textRect.width();
        }
        const int nameRight = std::min(textRect.right(), hitRect.left() - kToggleArrowSpacing);
        return std::max(0, nameRight - textRect.left() + 1);
    }

    /** @brief 画分组：一行名称，紧跟其后的是展开/收起箭头；没有子节点的分组不画箭头。 */
    void paintFolder(QPainter *painter,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) const
    {
        const bool selected = (option.state & QStyle::State_Selected) != 0;
        QStyleOptionViewItem background = option;
        initStyleOption(&background, index);
        const QString name = background.text;
        background.text.clear();
        background.state &= ~QStyle::State_Selected;
        const QWidget *widget = option.widget;
        QStyle *style = widget != nullptr ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);
        if (selected) {
            paintSelectionBackground(painter, option.rect);
        }
        paintGuideLine(painter, option, index);

        const QRect iconRect =
            style->subElementRect(QStyle::SE_ItemViewItemDecoration, &background, widget);
        if (!iconRect.isEmpty()) {
            qvariant_cast<QIcon>(index.data(Qt::DecorationRole))
                .paint(painter, iconRect, Qt::AlignCenter);
        }

        const QRect textRect = itemTextRect(background);
        const QRect arrowRect = toggleArrowRect(option, index);
        const QRect hitRect = toggleHitRect(option, index);
        // 名称区始终扣掉完整按钮热区：否则长分组名会画进可点击区域，也会造成“点名称却
        // 展开”的交互歧义。没有子节点的分组，toggleHitRect 本就返回空矩形，名称占满全宽。
        const int nameWidth = availableNameWidth(textRect, hitRect);
        const QColor baseColor = background.palette.color(QPalette::Text);

        painter->save();
        // 分组名与主机名保持同字号、同一常规字重；层级已经由图标、缩进和展开箭头表达，
        // 不再叠加粗体，避免主机较多时整棵树出现过重的黑色文字块。
        const QFont nameFont = itemNameFont(background.font);
        painter->setFont(nameFont);
        painter->setPen(baseColor);
        const QFontMetrics metrics(nameFont);
        painter->drawText(QRect(textRect.left(), textRect.top(), nameWidth, textRect.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          metrics.elidedText(name, Qt::ElideRight, nameWidth));
        if (!arrowRect.isNull()) {
            drawToggleArrow(painter, arrowRect, (option.state & QStyle::State_Open) != 0, baseColor);
        }
        painter->restore();
    }

    /**
     * @brief 画选中态的柔和背景：把强调色以低透明度叠在已经画好的行背景上。
     * @param rect 整行矩形（列宽即树宽，因为分组展开列已关闭）。
     *
     * 之前按“深色/浅色主题各一套固定色号”实现，用的是控件的 QPalette 明暗来判断——但
     * Shellium 的深浅主题是应用自己的样式表选择，并不会同步改写 QPalette，本机系统外观
     * 和应用内主题不一致时（例如系统深色、应用选了浅色）就会判断反，选中行画出一块几乎
     * 看不清文字的深底色。改成把强调色叠加在“已经画好的正确背景”之上，就不需要再猜深浅：
     * 无论背景本身是什么颜色，叠加同一个低透明度的绿都只会让它变浅，不会反转成看不清的
     * 深色块。
     */
    static void paintSelectionBackground(QPainter *painter, const QRect &rect)
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        QColor fill = ShelliumUi::accentColor();
        fill.setAlphaF(0.16F);
        QColor border = ShelliumUi::accentColor();
        border.setAlphaF(0.45F);

        const QRect fillRect = rect.adjusted(1, 1, -1, -1);
        painter->setPen(QPen(border, 1));
        painter->setBrush(fill);
        painter->drawRoundedRect(fillRect, 8, 8);
        painter->restore();
    }

    /**
     * @brief 为非顶层节点画一条竖向引导线，纵向贯穿整行，再接一小段横向连接线指向图标。
     *
     * 树关闭了 Qt 默认的分支装饰列（setRootIsDecorated(false)），缩进因此纯靠留白表达，
     * 子节点较多时很难一眼看出"这几行同属一个分组"。引导线画在缩进留白区内，不占用任何
     * 图标/文字空间，也不需要重新打开分支装饰列（那会在树左侧多出一条只为箭头存在的列）。
     */
    static void paintGuideLine(QPainter *painter,
                               const QStyleOptionViewItem &option,
                               const QModelIndex &index)
    {
        if (!index.parent().isValid()) {
            return;
        }
        QColor lineColor = option.palette.color(QPalette::Text);
        lineColor.setAlphaF(0.14F);
        painter->save();
        painter->setPen(QPen(lineColor, 1));
        const int x = option.rect.left() - 9;
        painter->drawLine(x, option.rect.top(), x, option.rect.bottom());
        const int midY = option.rect.center().y();
        painter->drawLine(x, midY, option.rect.left() - 2, midY);
        painter->restore();
    }

    /** @brief 画"未分组"一类分区标题：灰色小字 + 右侧一条延伸到行尾的分隔线。 */
    void paintSectionHeader(QPainter *painter,
                            const QStyleOptionViewItem &option,
                            const QModelIndex &index) const
    {
        const QString text = index.data(Qt::DisplayRole).toString();
        const QFont font = scaledFont(option.font, -1.0, true);
        const QFontMetrics metrics(font);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        QColor labelColor = option.palette.color(QPalette::Text);
        labelColor.setAlphaF(0.5F);
        painter->setFont(font);
        painter->setPen(labelColor);
        const QRect textRect(option.rect.left() + 2,
                             option.rect.top(),
                             metrics.horizontalAdvance(text) + 4,
                             option.rect.height());
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text);

        QColor lineColor = labelColor;
        lineColor.setAlphaF(0.22F);
        painter->setPen(QPen(lineColor, 1));
        const int lineY = option.rect.center().y();
        const int lineLeft = textRect.right() + 8;
        if (lineLeft < option.rect.right() - 2) {
            painter->drawLine(lineLeft, lineY, option.rect.right() - 2, lineY);
        }
        painter->restore();
    }

    /** @brief 展开时画朝下的实心三角，收起时画朝右的，与常见文件树的方向约定一致。 */
    static void drawToggleArrow(QPainter *painter,
                                const QRect &rect,
                                const bool expanded,
                                const QColor &color)
    {
        QColor arrowColor = color;
        arrowColor.setAlphaF(0.55F);
        const QPointF center(rect.center());
        const double half = rect.width() / 2.0;
        QPolygonF triangle;
        if (expanded) {
            triangle << QPointF(center.x() - half, center.y() - half * 0.6)
                     << QPointF(center.x() + half, center.y() - half * 0.6)
                     << QPointF(center.x(), center.y() + half * 0.7);
        } else {
            triangle << QPointF(center.x() - half * 0.6, center.y() - half)
                     << QPointF(center.x() + half * 0.7, center.y())
                     << QPointF(center.x() - half * 0.6, center.y() + half);
        }
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(arrowColor);
        painter->drawPolygon(triangle);
    }
};

/**
 * @brief 支持"主机与分组"拖拽排序/移动的树控件。
 *
 * 选用 QAbstractItemView::InternalMove 让 Qt 完成节点的实际挪动，本类只负责在挪动前
 * 校验合法性（主机不能收留子节点；分组不能被拖进自己的子孙里形成环），以及在挪动完成
 * 后通过 onDropped 回调通知外部把新的 parentId/sortOrder 写回数据库。之所以不用信号，
 * 是因为这个类只在 HostManagerWidget.cpp 内部使用，避免为了一个回调再引入 moc。
 */
class HostTreeWidget final : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;

    /** @brief 拖放合法且 Qt 内部移动完成后触发；oldParentItem 为空表示原先在根层级。 */
    std::function<void(QTreeWidgetItem *movedItem, QTreeWidgetItem *oldParentItem)> onDropped;

protected:
    void dragMoveEvent(QDragMoveEvent *event) override
    {
        QTreeWidgetItem *draggedItem = currentItem();
        QTreeWidgetItem *candidateParent = nullptr;
        if (draggedItem == nullptr
            || !computeLegalDrop(event->position().toPoint(), draggedItem, &candidateParent)) {
            event->ignore();
            return;
        }
        QTreeWidget::dragMoveEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        QTreeWidgetItem *draggedItem = currentItem();
        QTreeWidgetItem *candidateParent = nullptr;
        if (draggedItem == nullptr
            || !computeLegalDrop(event->position().toPoint(), draggedItem, &candidateParent)) {
            event->ignore();
            return;
        }

        QTreeWidgetItem *oldParentItem = draggedItem->parent();
        QTreeWidget::dropEvent(event);
        if (onDropped) {
            onDropped(draggedItem, oldParentItem);
        }
    }

private:
    /** @brief node 是否等于 ancestor，或者是 ancestor 的任意层级子孙。 */
    [[nodiscard]] static bool isSameOrDescendant(QTreeWidgetItem *node, QTreeWidgetItem *ancestor)
    {
        for (QTreeWidgetItem *cursor = node; cursor != nullptr; cursor = cursor->parent()) {
            if (cursor == ancestor) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 判断把 draggedItem 放到 pos 处是否合法，并给出目标父节点。
     * @param outNewParent 合法时写入目标父节点；根层级为空指针。
     * @return 目标父节点必须是分组（或根层级），且不能是被拖动节点自身或其子孙。
     */
    [[nodiscard]] bool computeLegalDrop(const QPoint &pos,
                                        QTreeWidgetItem *draggedItem,
                                        QTreeWidgetItem **outNewParent) const
    {
        QTreeWidgetItem *targetItem = itemAt(pos);
        QTreeWidgetItem *candidateParent = nullptr;
        if (targetItem != nullptr) {
            candidateParent = dropIndicatorPosition() == QAbstractItemView::OnItem
                ? targetItem
                : targetItem->parent();
        }

        if (candidateParent != nullptr) {
            const auto kind = static_cast<HostNodeKind>(candidateParent->data(0, HostKindRole).toInt());
            if (kind != HostNodeKind::Folder) {
                // 主机不是容器，不能收留任何子节点。
                return false;
            }
        }

        if (isSameOrDescendant(candidateParent, draggedItem)) {
            // 目标是被拖动节点自己，或者是它的子孙：会形成环，拒绝。
            return false;
        }

        if (outNewParent != nullptr) {
            *outNewParent = candidateParent;
        }
        return true;
    }
};

/**
 * @brief 把侧栏次要动作配置成紧凑图标按钮。
 * @param button 待配置的工具按钮；不得为空。
 * @param accessibleName 无障碍名称和悬停提示中使用的完整动作说明。
 *
 * 320 像素侧栏若同时展示七个“图标 + 文字”按钮会被迫压缩成难以辨认的碎片。保留一个
 * 明确的主操作文字，其余动作使用带完整 Tooltip/AccessibleName 的图标，是现代桌面工具
 * 常用的信息密度平衡；功能并未隐藏到不可发现的菜单中。
 */
void configureSidebarIconButton(QToolButton *button, const QString &accessibleName)
{
    Q_ASSERT(button != nullptr);
    button->setProperty("compactIconButton", true);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(false);
    button->setFixedSize(34, 34);
    button->setIconSize(QSize(17, 17));
    button->setToolTip(accessibleName);
    button->setAccessibleName(accessibleName);
}

/**
 * @brief 编辑一条持久化主机或分组记录的内部对话框。
 *
 * 分组编辑器只暴露名称，避免把组织层级与 SSH 连接参数混在一起；主机编辑器则按“基础信息、
 * 认证与跳板”组织字段。未展示的旧分组配置和主机标签仍保留在 HostRecord 中，编辑名称或其他
 * 可见字段时不会被静默清空。密码只作为测试连接或保存后立即连接的会话值返回，绝不写入记录。
 */
class HostRecordDialog final : public QDialog {
public:
    /**
     * @brief 创建记录编辑器。
     * @param initial 待编辑快照；新建时调用方应先填好 kind、id 和 parentId。
     * @param inheritedParent 当前父分组解析后的配置；仅用于测试尚未保存主机时补齐继承值。
     * @param credentialVault 凭据保险库；测试已有主机时可读取已加密保存的密码，不得为空。
     * @param parent Qt 父对象。
     */
    explicit HostRecordDialog(const HostRecord &initial,
                              const HostRecord &inheritedParent,
                              CredentialVault *credentialVault,
                              QWidget *parent = nullptr)
        : QDialog(parent)
        , m_initial(initial)
        , m_inheritedParent(inheritedParent)
        , m_credentialVault(credentialVault)
        , m_nameEdit(new QLineEdit(initial.name, this))
    {
        Q_ASSERT(m_credentialVault != nullptr);

        // 字段版式与设置窗口、连接对话框统一：分组卡片 + “左说明右控件”的行 + 滑动开关。
        // 这里先只创建两种记录都需要的名称输入框；主机专属控件放到 isHost 分支中创建，
        // 从对象树层面保证分组窗口除名称外不再残留可聚焦或可被辅助技术发现的字段。
        const bool isHost = initial.kind == HostNodeKind::Host;
        setObjectName(QStringLiteral("ConnectionDialog"));
        setWindowTitle(isHost ? tr("主机配置") : tr("分组配置"));
        setMinimumWidth(580);
        m_nameEdit->setObjectName(QStringLiteral("hostRecordNameEdit"));
        m_nameEdit->setMinimumWidth(260);

        auto *basicsCard = new SettingsCard(isHost ? tr("主机") : tr("分组"), this);
        // SettingsCard 的固定对象名由全局样式表用于绘制白色圆角区域，不能为了测试定位而
        // 覆盖；一旦改名，QFrame#SettingsCard 选择器就会失效，卡片会退化成透明矩形。
        basicsCard->addRow(tr("名称"), m_nameEdit);
        if (isHost) {
            // 主机专属字段只在主机窗口中实例化。端口始终以明确值保存，因此不再提供容易让
            // 用户混淆“当前值”和“父分组值”的继承开关；已有继承端口在打开时显示其有效值。
            m_hostEdit = new QLineEdit(initial.host, this);
            m_hostEdit->setObjectName(QStringLiteral("hostAddressEdit"));
            m_portSpin = new QSpinBox(this);
            m_portSpin->setObjectName(QStringLiteral("hostPortSpin"));
            m_portSpin->setRange(1, 65535);
            m_portSpin->setValue(initial.port.value_or(
                inheritedParent.port.value_or(quint16{22})));

            m_usernameEdit = new QLineEdit(initial.username, this);
            m_usernameEdit->setObjectName(QStringLiteral("hostUsernameEdit"));
            m_usernameEdit->setPlaceholderText(tr("留空则继承父分组"));
            m_passwordEdit = new QLineEdit(this);
            m_passwordEdit->setObjectName(QStringLiteral("hostPasswordEdit"));
            m_passwordEdit->setEchoMode(QLineEdit::Password);
            m_passwordEdit->setPlaceholderText(tr("仅用于本次连接；保存由保险库负责"));
            m_identityEdit = new QLineEdit(initial.identityFile, this);
            m_identityEdit->setObjectName(QStringLiteral("hostIdentityEdit"));
            m_identityEdit->setPlaceholderText(
                tr("留空则继承父分组或使用 Agent/默认密钥"));
            m_proxyJumpsEdit = new QLineEdit(
                initial.proxyJumps.join(QLatin1Char(',')), this);
            m_proxyJumpsEdit->setObjectName(QStringLiteral("hostProxyJumpsEdit"));
            m_proxyJumpsEdit->setPlaceholderText(tr("逗号分隔；留空则继承父分组"));
            m_savePasswordToggle = new ToggleSwitch(this);
            m_savePasswordToggle->setObjectName(QStringLiteral("hostSavePasswordToggle"));
            m_savePasswordToggle->setChecked(
                !initial.credentialId.isEmpty() || !inheritedParent.credentialId.isEmpty());
            m_legacyHmacToggle = new ToggleSwitch(this);
            m_legacyHmacToggle->setObjectName(QStringLiteral("hostLegacyHmacToggle"));
            m_legacyHmacToggle->setChecked(initial.allowLegacyHmacSha1);

            for (QLineEdit *edit : {m_hostEdit, m_usernameEdit, m_passwordEdit,
                                    m_proxyJumpsEdit}) {
                edit->setMinimumWidth(260);
            }

            basicsCard->addRow(tr("主机地址"), m_hostEdit);
            basicsCard->addRow(
                tr("端口"), new SettingsStepper(m_portSpin, basicsCard));

            auto *identityContainer = new QWidget(this);
            auto *identityLayout = new QHBoxLayout(identityContainer);
            identityLayout->setContentsMargins(0, 0, 0, 0);
            identityLayout->setSpacing(8);
            auto *browseButton = new QPushButton(tr("浏览…"), identityContainer);
            browseButton->setObjectName(QStringLiteral("hostIdentityBrowseButton"));
            UiIcons::decorateButton(
                browseButton,
                UiIcons::symbol(
                    this,
                    UiIcons::Symbol::Upload,
                    this->palette().color(QPalette::WindowText)),
                tr("选择 SSH 私钥文件"));
            m_identityEdit->setMinimumWidth(200);
            identityLayout->addWidget(m_identityEdit, 1);
            identityLayout->addWidget(browseButton);

            // 认证区严格按用户填写路径排列：用户名、密码、私钥、ProxyJump，最后才是保存与
            // 兼容性开关。账号和凭据相邻能减少来回扫描，也符合 SSH 的认证心智顺序。
            auto *authCard = new SettingsCard(tr("认证与跳板"), this);
            authCard->addRow(tr("用户名"), m_usernameEdit);
            authCard->addRow(tr("密码"), m_passwordEdit);
            authCard->addRow(tr("私钥文件"), identityContainer);
            authCard->addRow(tr("ProxyJump"), m_proxyJumpsEdit);
            authCard->addRow(tr("加密保存到本地保险库"), m_savePasswordToggle);
            authCard->addRow(tr("旧版兼容"),
                             m_legacyHmacToggle,
                             tr("仅为该主机允许 hmac-sha1（不继承）。"));

            connect(browseButton, &QPushButton::clicked, this, [this]() {
                const QString path = QFileDialog::getOpenFileName(
                    this, tr("选择 SSH 私钥文件"));
                if (!path.isEmpty()) {
                    m_identityEdit->setText(path);
                }
            });

            // 只有打开窗口时名称为空，或用户后来主动清空名称，才启用自动命名。名称框一旦
            // 收到非空人工输入便立即退出自动模式，后续修改账号、地址或端口都不能覆盖用户
            // 赋予主机的业务名称。使用 textEdited 判断人工操作，是为了避免 setText() 写入的
            // 自动名称反过来被误判成用户定制名称，导致只输入第一个地址字符后便停止同步。
            m_automaticNameEnabled = initial.name.trimmed().isEmpty();
            connect(m_nameEdit,
                    &QLineEdit::textEdited,
                    this,
                    [this](const QString &editedName) {
                        m_automaticNameEnabled = editedName.trimmed().isEmpty();
                    });
            connect(m_hostEdit,
                    &QLineEdit::textChanged,
                    this,
                    [this]() { refreshAutomaticName(); });
            connect(m_usernameEdit,
                    &QLineEdit::textChanged,
                    this,
                    [this]() { refreshAutomaticName(); });
            connect(m_portSpin,
                    &QSpinBox::valueChanged,
                    this,
                    [this]() { refreshAutomaticName(); });

            // 编辑异常旧数据时可能出现“地址已有值但名称为空”。连接建立在控件初始值全部
            // 写入之后，因此在这里主动同步一次，让该场景无需再敲击地址框也能得到可保存名称。
            refreshAutomaticName();

            // 认证卡只属于主机窗口，先暂存到成员中供下方统一加入正文布局；分组窗口不会
            // 创建空白认证卡，因此界面视觉和无障碍对象树都真正只剩“名称”。
            m_authenticationCard = authCard;
        }

        auto *footer = new QWidget(this);
        footer->setObjectName(QStringLiteral("SettingsFooter"));
        auto *footerLayout = new QHBoxLayout(footer);
        footerLayout->setContentsMargins(20, 12, 20, 12);
        footerLayout->setSpacing(10);
        auto *cancelButton = new QPushButton(tr("取消"), footer);
        cancelButton->setObjectName(QStringLiteral("hostRecordCancelButton"));
        auto *saveButton = new QPushButton(tr("保存"), footer);
        saveButton->setObjectName(QStringLiteral("hostRecordSaveButton"));
        saveButton->setProperty("primaryAction", true);
        saveButton->setDefault(true);
        UiIcons::decorateButton(
            cancelButton,
            UiIcons::symbol(
                this, UiIcons::Symbol::Cancel, this->palette().color(QPalette::WindowText)),
            tr("取消本次配置修改"));
        UiIcons::decorateButton(
            saveButton,
            // 保存是绿色主操作，固定深色图标才能同时兼顾浅色与深色主题下的对比度。
            UiIcons::symbol(
                this, UiIcons::Symbol::Check, QColor(QStringLiteral("#0c2f24"))),
            tr("保存当前配置"));
        if (isHost) {
            m_testConnectionButton = new QPushButton(tr("测试连接"), footer);
            m_testConnectionButton->setObjectName(
                QStringLiteral("hostTestConnectionButton"));
            UiIcons::decorateButton(
                m_testConnectionButton,
                UiIcons::symbol(
                    this,
                    UiIcons::Symbol::Connect,
                    this->palette().color(QPalette::WindowText)),
                tr("使用当前未保存的配置测试 SSH 连接"));
            // 测试属于独立的辅助动作，固定在页脚左下角；右侧仍保留取消和保存的标准顺序。
            footerLayout->addWidget(m_testConnectionButton);
        }
        footerLayout->addStretch(1);
        footerLayout->addWidget(cancelButton);
        footerLayout->addWidget(saveButton);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto *body = new QWidget(this);
        auto *bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(22, 20, 22, 18);
        bodyLayout->setSpacing(16);
        bodyLayout->addWidget(basicsCard);
        if (m_authenticationCard != nullptr) {
            bodyLayout->addWidget(m_authenticationCard);
        }
        bodyLayout->addStretch(1);
        layout->addWidget(body, 1);
        layout->addWidget(footer);

        if (m_testConnectionButton != nullptr) {
            connect(m_testConnectionButton,
                    &QPushButton::clicked,
                    this,
                    &HostRecordDialog::startConnectionTest);
        }
        connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
        connect(saveButton, &QPushButton::clicked, this, [this, isHost]() {
            if (m_nameEdit->text().trimmed().isEmpty()) {
                QMessageBox::warning(this, tr("配置不完整"), tr("请输入名称。"));
                m_nameEdit->setFocus();
                return;
            }
            if (isHost && m_hostEdit->text().trimmed().isEmpty()) {
                QMessageBox::warning(this, tr("配置不完整"), tr("请输入主机名或 IP 地址。"));
                m_hostEdit->setFocus();
                return;
            }
            QDialog::accept();
        });
    }

    /**
     * @brief 销毁配置窗口，并安全移交仍在退出中的连接测试线程。
     *
     * DNS 解析等系统调用可能短暂阻塞，窗口关闭时不能强行析构仍在运行的 QThread。这里先
     * 请求停止，再把尚未退出的会话临时交给 QApplication；线程结束后既有 deleteLater
     * 连接会完成回收，避免崩溃，也不会让关闭窗口被网络超时长时间卡住。
     */
    ~HostRecordDialog() override
    {
        if (m_testSession == nullptr || !m_testSession->isRunning()) {
            return;
        }
        SshSessionStack *finishingSession = m_testSession;
        m_testSession = nullptr;
        finishingSession->requestStop();
        QObject::disconnect(finishingSession, nullptr, this, nullptr);
        finishingSession->setParent(QApplication::instance());
    }

    /** @brief 返回用户确认后的记录；id、kind、parentId、排序和创建时间保持不变。 */
    [[nodiscard]] HostRecord record() const
    {
        HostRecord result = m_initial;
        result.name = m_nameEdit->text().trimmed();
        if (result.kind == HostNodeKind::Host) {
            // 标签已从界面移除，但不能因此把旧标签清空；只更新用户仍能看见并控制的字段。
            // 端口不再提供继承开关，保存时始终记录输入框中的明确值。
            result.host = m_hostEdit->text().trimmed();
            result.port = static_cast<quint16>(m_portSpin->value());
            result.username = m_usernameEdit->text().trimmed();
            result.identityFile = m_identityEdit->text().trimmed();
            result.proxyJumps = splitList(m_proxyJumpsEdit->text());
            result.allowLegacyHmacSha1 = m_legacyHmacToggle->isChecked();
        }
        return result;
    }

    /** @brief 返回只用于紧随保存之后首次连接的明文，不属于 HostRecord 持久化数据。 */
    [[nodiscard]] QString sessionPassword() const
    {
        return m_passwordEdit == nullptr ? QString{} : m_passwordEdit->text();
    }

    /**
     * @brief 用户是否要求把本次输入加密保存到保险库。
     * @return 只有主机记录且复选框选中时返回 true；分组永远不直接保存密码。
     */
    [[nodiscard]] bool shouldSavePassword() const
    {
        return m_savePasswordToggle != nullptr && m_savePasswordToggle->isChecked();
    }

    /**
     * @brief 用户是否明确取消了一条既有凭据引用。
     * @return 原记录有关联凭据且复选框被取消时返回 true。
     */
    [[nodiscard]] bool shouldDetachCredential() const
    {
        return m_savePasswordToggle != nullptr
            && !m_initial.credentialId.isEmpty()
            && !m_savePasswordToggle->isChecked();
    }

private:
    /**
     * @brief 用当前有效账号、主机地址和端口刷新自动生成的主机名称。
     *
     * 名称自动模式关闭时，本方法不做任何修改，保证人工名称拥有最高优先级。地址为空时清空
     * 尚未完成的自动名称；地址有效时生成“用户名@主机地址:端口”。用户名输入框为空代表继承，
     * 因而优先使用父分组解析后的有效用户名；若两处都没有用户名，则沿用 ConnectionTarget 的
     * 规范格式退化为“主机地址:端口”，避免产生以 `@` 开头的无效、难读名称。
     */
    void refreshAutomaticName()
    {
        if (!m_automaticNameEnabled || m_hostEdit == nullptr
            || m_usernameEdit == nullptr || m_portSpin == nullptr) {
            return;
        }

        const QString hostAddress = m_hostEdit->text().trimmed();
        if (hostAddress.isEmpty()) {
            m_nameEdit->clear();
            return;
        }

        ConnectionTarget target;
        target.host = hostAddress;
        target.username = m_usernameEdit->text().trimmed();
        if (target.username.isEmpty()) {
            target.username = m_inheritedParent.username.trimmed();
        }
        target.port = static_cast<quint16>(m_portSpin->value());
        // 默认 22 端口也必须显示：自动名称承担区分同址不同 SSH 服务的职责，不能省略默认值。
        m_nameEdit->setText(target.connectionSpecifier(true));
    }

    /**
     * @brief 使用当前表单值异步执行一次真实 SSH 握手和认证。
     *
     * 测试过程不保存 HostRecord，也不安装 Shell 集成脚本；连接成功后立即主动断开。若当前
     * 主机引用了保险库凭据且密码框为空，会先经用户解锁授权后读取一次性明文用于本次测试。
     */
    void startConnectionTest()
    {
        if (m_testInProgress || m_hostEdit == nullptr
            || m_hostEdit->text().trimmed().isEmpty()) {
            if (m_hostEdit != nullptr && m_hostEdit->text().trimmed().isEmpty()) {
                QMessageBox::warning(
                    this, tr("连接信息不完整"), tr("请输入主机名或 IP 地址。"));
                m_hostEdit->setFocus();
            }
            return;
        }

        HostRecord effective = record();
        // 尚未保存的主机无法交给数据库 resolveHost()。在这里按同一套“空值继承父分组”
        // 规则补齐测试快照，保证测试结果与保存后从主机树发起的真实连接一致。
        if (effective.username.isEmpty()) {
            effective.username = m_inheritedParent.username;
        }
        if (effective.identityFile.isEmpty()) {
            effective.identityFile = m_inheritedParent.identityFile;
        }
        if (effective.proxyJumps.isEmpty()) {
            effective.proxyJumps = m_inheritedParent.proxyJumps;
        }
        if (effective.credentialId.isEmpty()) {
            effective.credentialId = m_inheritedParent.credentialId;
        }

        ConnectionTarget target = effective.toConnectionTarget();
        target.password = sessionPassword();
        if (target.password.isEmpty() && !effective.credentialId.isEmpty()
            && shouldSavePassword()) {
            if (!VaultDialog::ensureUnlocked(m_credentialVault, this)) {
                return;
            }
            QString credentialError;
            if (!m_credentialVault->applyHostCredential(
                    effective, target, &credentialError)) {
                QMessageBox::warning(
                    this,
                    tr("无法测试连接"),
                    credentialError.isEmpty() ? tr("无法读取已保存的主机凭据。")
                                              : credentialError);
                return;
            }
        }

        m_testInProgress = true;
        m_testConnectionButton->setEnabled(false);
        m_testConnectionButton->setText(tr("测试中…"));

        // 测试连接复用生产 SSH 状态机，才能覆盖 ProxyJump、私钥、密码和旧版 HMAC 等完整
        // 配置。第二个参数关闭 Shell 集成安装，避免一次只读测试在远端写入临时脚本。
        const QString testSpecifier = target.connectionSpecifier(true);
        auto *session = new SshSessionStack(target, false, this);
        // 工作线程已经复制了测试快照，立即擦除这里的临时密码副本，避免信号闭包再额外持有
        // 一份明文直到会话销毁；输入框本身仍由用户控制，保存/再次测试行为不会受影响。
        target.password.fill(QChar::Null);
        target.password.clear();
        m_testSession = session;
        connect(session,
                &SshSessionStack::shellConnected,
                this,
                [this, testSpecifier]() {
                    finishConnectionTest(
                        true,
                        tr("已成功连接 %1。").arg(testSpecifier));
                });
        connect(session,
                &SshSessionStack::connectionError,
                this,
                [this](const QString &message) {
                    finishConnectionTest(false, message);
                });
        connect(session,
                &SshSessionStack::legacyHmacRequired,
                this,
                [this](const QString &message) {
                    finishConnectionTest(
                        false,
                        tr("服务器只支持旧版 hmac-sha1。若已确认风险，请开启“旧版兼容”后重试。\n\n%1")
                            .arg(message));
                });
        connect(session,
                &SshSessionStack::layerCredentialRequired,
                this,
                [this](const ConnectionTarget &, const bool retry) {
                    finishConnectionTest(
                        false,
                        retry ? tr("SSH 认证失败，请检查密码或私钥口令后重试。")
                              : tr("服务器需要密码或私钥口令，请填写“密码”后重试。"));
                });
        connect(session,
                &SshSessionStack::hostKeyVerificationRequired,
                this,
                [this, session](const ConnectionTarget &verificationTarget,
                                const QString &fingerprint) {
                    // 测试连接与正式连接遵守同一 known_hosts 安全边界：未知指纹必须经用户
                    // 人工核验，不能为了“测试方便”自动信任并写入本机配置。
                    const QMessageBox::StandardButton answer = QMessageBox::question(
                        this,
                        tr("确认服务器主机密钥"),
                        tr("首次连接 %1。\n\nSHA-256 指纹：\n%2\n\n"
                           "请通过可信渠道核对指纹。确认信任并继续测试吗？")
                            .arg(verificationTarget.connectionSpecifier(true), fingerprint),
                        QMessageBox::Yes | QMessageBox::No,
                        QMessageBox::No);
                    if (m_testSession == session) {
                        session->confirmUnknownHost(answer == QMessageBox::Yes);
                    }
                });
        connect(session, &QThread::finished, this, [this, session]() {
            if (m_testSession == session) {
                m_testSession = nullptr;
            }
        });
        // 回收连接以 session 自身为接收者，不依赖对话框继续存活；用户在测试中关闭窗口时，
        // 对话框析构会把仍在退出的线程移交给 QApplication，该连接随后仍能正常执行。
        connect(session, &QThread::finished, session, &QObject::deleteLater);
        session->start();
    }

    /**
     * @brief 结束当前测试、恢复按钮状态并向用户展示结论。
     * @param succeeded true 表示 SSH Shell 已完成认证并成功打开；false 表示测试失败。
     * @param message 成功摘要或可诊断的失败原因。
     */
    void finishConnectionTest(const bool succeeded, const QString &message)
    {
        // 同一次失败可能伴随多个底层信号；用状态门只展示一个结论，避免连续弹出错误框。
        if (!m_testInProgress) {
            return;
        }
        m_testInProgress = false;
        m_testConnectionButton->setEnabled(true);
        m_testConnectionButton->setText(tr("测试连接"));
        if (m_testSession != nullptr) {
            m_testSession->requestStop();
        }

        if (succeeded) {
            QMessageBox::information(this, tr("测试连接成功"), message);
        } else {
            QMessageBox::warning(
                this,
                tr("测试连接失败"),
                message.isEmpty() ? tr("SSH 连接未能建立，请检查当前配置。") : message);
        }
    }

    /**
     * @brief 把逗号列表规范化为去空白、去空项、去重复的字符串列表。
     * @param text 用户输入文本。
     * @return 保留首次出现顺序的规范化列表。
     */
    [[nodiscard]] static QStringList splitList(const QString &text)
    {
        QStringList result;
        for (const QString &part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString normalized = part.trimmed();
            if (!normalized.isEmpty() && !result.contains(normalized)) {
                result.append(normalized);
            }
        }
        return result;
    }

    HostRecord m_initial;
    /** @brief 父分组的已解析配置，仅用于补齐尚未持久化的测试连接快照。 */
    HostRecord m_inheritedParent;
    /** @brief 应用共享凭据保险库，不拥有其生命周期。 */
    CredentialVault *m_credentialVault = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QLineEdit *m_usernameEdit = nullptr;
    /** @brief 是否允许连接字段继续驱动名称；用户输入非空名称后永久关闭至再次清空。 */
    bool m_automaticNameEnabled = false;
    QLineEdit *m_identityEdit = nullptr;
    QLineEdit *m_proxyJumpsEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    ToggleSwitch *m_savePasswordToggle = nullptr;
    ToggleSwitch *m_legacyHmacToggle = nullptr;
    SettingsCard *m_authenticationCard = nullptr;
    QPushButton *m_testConnectionButton = nullptr;
    /** @brief 当前测试使用的后台 SSH 栈；结束后由 Qt 事件循环安全销毁。 */
    SshSessionStack *m_testSession = nullptr;
    /** @brief 防止底层重复完成信号产生多个结果弹窗或并行测试。 */
    bool m_testInProgress = false;
};

} // namespace

HostManagerWidget::HostManagerWidget(ApplicationDatabase *database,
                                     CredentialVault *credentialVault,
                                     QWidget *parent)
    : QWidget(parent)
    , m_database(database)
    , m_credentialVault(credentialVault)
    , m_searchEdit(new QLineEdit(this))
    , m_tree(new HostTreeWidget(this))
    , m_headerActions(new QWidget(this))
{
    Q_ASSERT(m_database != nullptr);
    Q_ASSERT(m_credentialVault != nullptr);

    m_searchEdit->setPlaceholderText(tr("搜索名称/地址；输入 #标签 精确筛选"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->addAction(
        UiIcons::symbol(this, UiIcons::Symbol::Search, this->palette().color(QPalette::WindowText)),
        QLineEdit::LeadingPosition);

    // 为主机树设置稳定的对象名，供应用级样式表只增强主机选中态。不能直接加深所有
    // QTreeView 的选中色，否则设置页、效率工具等无关列表也会被意外改变视觉层级。
    m_tree->setObjectName(QStringLiteral("HostTree"));
    m_tree->setHeaderHidden(true);
    // 展开箭头改由 HostItemDelegate 画在分组行右端，Qt 自带的左侧分支装饰列必须关掉：
    // 那一列只为箭头存在，留着会在顶层节点左边压出一条空白。层级缩进则要保留——分组和它
    // 下面的主机全部顶格时，看不出谁属于谁。
    m_tree->setRootIsDecorated(false);
    m_tree->setIndentation(16);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    auto *itemDelegate = new HostItemDelegate(m_tree);
    itemDelegate->onToggleRequested = [this](const QModelIndex &index) {
        m_tree->setExpanded(index, !m_tree->isExpanded(index));
    };
    m_tree->setItemDelegate(itemDelegate);

    // 拖拽排序/跨分组移动：Qt 负责实际挪动 QTreeWidgetItem，HostTreeWidget 只负责合法性
    // 校验，真正的持久化在下面的 onDropped 回调里完成。搜索结果是扁平视图，与真实父子
    // 关系无关，applyFilter() 会在搜索时临时关闭拖放，避免把搜索命中的顺序误写回数据库。
    m_tree->setDragEnabled(true);
    m_tree->setAcceptDrops(true);
    m_tree->setDropIndicatorShown(true);
    m_tree->setDragDropMode(QAbstractItemView::InternalMove);
    static_cast<HostTreeWidget *>(m_tree)->onDropped =
        [this](QTreeWidgetItem *movedItem, QTreeWidgetItem *oldParentItem) {
            persistTreeReorder(movedItem, oldParentItem);
        };

    // 侧栏标题行只保留三个最高频动作；编辑/删除/导入/导出都能在树的右键菜单里找到，
    // 而它们都需要先选中一个节点，本来就更适合右键这条“先选中再操作”的路径。
    auto *addGroupButton = new QToolButton(m_headerActions);
    addGroupButton->setText(tr("新建分组"));
    auto *addHostButton = new QPushButton(tr("新建主机"), m_headerActions);
    addHostButton->setProperty("primaryAction", true);
    auto *refreshButton = new QToolButton(m_headerActions);
    refreshButton->setText(tr("刷新"));
    UiIcons::decorateButton(
        addGroupButton,
        UiIcons::symbol(this, UiIcons::Symbol::FolderAdd, this->palette().color(QPalette::WindowText)),
        tr("在当前层级新建主机分组"));
    UiIcons::decorateButton(
        addHostButton,
        // 主操作背景在浅色和深色主题下都固定为高亮绿色，图标也必须固定使用深色；若让它
        // 跟随深色主题的浅色 WindowText，绿色按钮上的“+”反而会降低对比度。
        UiIcons::symbol(this,
                        UiIcons::Symbol::Plus,
                        QColor(QStringLiteral("#0c2f24"))),
        tr("在当前分组中新建 SSH 主机"));
    UiIcons::decorateButton(
        refreshButton,
        UiIcons::symbol(this, UiIcons::Symbol::Refresh, this->palette().color(QPalette::WindowText)),
        tr("重新读取主机与分组"));
    configureSidebarIconButton(addGroupButton, tr("在当前层级新建主机分组"));
    configureSidebarIconButton(refreshButton, tr("重新读取主机与分组"));

    auto *headerLayout = new QHBoxLayout(m_headerActions);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(6);
    headerLayout->addWidget(addHostButton);
    headerLayout->addWidget(addGroupButton);
    headerLayout->addWidget(refreshButton);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(m_searchEdit);
    // 不再放"主机与分组"小标题：侧栏顶部已经写着"主机"，树里除了主机和分组也没有别的东西，
    // 这一行只是在挤占本就紧张的纵向空间。
    layout->addWidget(m_tree, 1);

    connect(m_searchEdit, &QLineEdit::textChanged, this, [this]() { applyFilter(); });
    connect(addGroupButton, &QToolButton::clicked, this, &HostManagerWidget::addGroup);
    connect(addHostButton, &QPushButton::clicked, this, &HostManagerWidget::addHost);
    connect(refreshButton, &QToolButton::clicked, this, &HostManagerWidget::refresh);
    connect(m_tree, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem *item, const int) { activateTreeItem(item); });
    connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        QTreeWidgetItem *item = m_tree->itemAt(position);
        if (item != nullptr) {
            m_tree->setCurrentItem(item);
        }

        QMenu menu(this);
                UiIcons::modernizePopup(&menu);
        const bool isHost = item != nullptr
            && static_cast<HostNodeKind>(item->data(0, HostKindRole).toInt())
                == HostNodeKind::Host;
        if (isHost) {
            QAction *connectAction = menu.addAction(
                UiIcons::symbol(this, UiIcons::Symbol::Connect, this->palette().color(QPalette::WindowText)),
                tr("连接"));
            connect(connectAction, &QAction::triggered,
                    this, [this, item]() { activateTreeItem(item); });
            menu.addSeparator();
        }
        QAction *addGroupAction = menu.addAction(
            UiIcons::symbol(this, UiIcons::Symbol::FolderAdd, this->palette().color(QPalette::WindowText)),
            tr("新建分组"));
        QAction *addHostAction = menu.addAction(
            UiIcons::symbol(this, UiIcons::Symbol::Plus, this->palette().color(QPalette::WindowText)),
            tr("新建主机"));
        connect(addGroupAction, &QAction::triggered, this, &HostManagerWidget::addGroup);
        connect(addHostAction, &QAction::triggered, this, &HostManagerWidget::addHost);
        // “未分组”分区标题不对应任何数据库记录，右键它时不提供编辑/删除——那两个动作
        // 只会因为没有选中项而静默无效，不如直接不显示。
        const bool isRecordItem =
            item != nullptr && item->data(0, HostKindRole).toInt() != kSectionHeaderKindValue;
        if (isRecordItem) {
            menu.addSeparator();
            QAction *editAction = menu.addAction(
                UiIcons::symbol(this, UiIcons::Symbol::Edit, this->palette().color(QPalette::WindowText)),
                tr("编辑"));
            QAction *deleteAction = menu.addAction(
                UiIcons::symbol(this, UiIcons::Symbol::Trash, this->palette().color(QPalette::WindowText)),
                tr("删除"));
            connect(editAction, &QAction::triggered, this, &HostManagerWidget::editSelected);
            connect(deleteAction, &QAction::triggered, this, &HostManagerWidget::removeSelected);
        }
        menu.addSeparator();
        QAction *importAction = menu.addAction(
            UiIcons::symbol(this, UiIcons::Symbol::Upload, this->palette().color(QPalette::WindowText)),
            tr("导入主机…"));
        QAction *exportAction = menu.addAction(
            UiIcons::symbol(this, UiIcons::Symbol::Download, this->palette().color(QPalette::WindowText)),
            tr("导出主机…"));
        connect(importAction, &QAction::triggered, this, &HostManagerWidget::importHosts);
        connect(exportAction, &QAction::triggered, this, &HostManagerWidget::exportHosts);
        menu.addSeparator();
        QAction *refreshAction = menu.addAction(
            UiIcons::symbol(this, UiIcons::Symbol::Refresh, this->palette().color(QPalette::WindowText)),
            tr("刷新"));
        connect(refreshAction, &QAction::triggered, this, &HostManagerWidget::refresh);
        menu.exec(m_tree->viewport()->mapToGlobal(position));
    });

    refresh();
}

QWidget *HostManagerWidget::headerActionsWidget() const
{
    return m_headerActions;
}

void HostManagerWidget::refresh()
{
    applyFilter();
    // 该信号放在整棵树刷新之后，订阅方不会读到一半旧、一半新的主机集。
    emit hostsChanged();
}

bool HostManagerWidget::appendChildren(QTreeWidgetItem *parentItem,
                                       const QString &parentId,
                                       QString *errorMessage)
{
    bool ok = false;
    const QList<HostRecord> children =
        m_database->listHostsInGroup(parentId, &ok, errorMessage);
    if (!ok) {
        return false;
    }

    for (const HostRecord &record : children) {
        QTreeWidgetItem *item = createTreeItem(record);
        if (parentItem == nullptr) {
            m_tree->addTopLevelItem(item);
        } else {
            parentItem->addChild(item);
        }
        if (record.kind == HostNodeKind::Folder
            && !appendChildren(item, record.id, errorMessage)) {
            return false;
        }
    }
    return true;
}

bool HostManagerWidget::appendRootLevel(QString *errorMessage)
{
    bool ok = false;
    const QList<HostRecord> rootRecords = m_database->listHostsInGroup({}, &ok, errorMessage);
    if (!ok) {
        return false;
    }

    QList<HostRecord> folders;
    QList<HostRecord> ungroupedHosts;
    for (const HostRecord &record : rootRecords) {
        (record.kind == HostNodeKind::Folder ? folders : ungroupedHosts).append(record);
    }

    for (const HostRecord &record : folders) {
        QTreeWidgetItem *item = createTreeItem(record);
        m_tree->addTopLevelItem(item);
        if (!appendChildren(item, record.id, errorMessage)) {
            return false;
        }
    }

    if (!ungroupedHosts.isEmpty()) {
        m_tree->addTopLevelItem(createSectionHeaderItem(tr("未分组")));
        for (const HostRecord &record : ungroupedHosts) {
            m_tree->addTopLevelItem(createTreeItem(record));
        }
    }
    return true;
}

void HostManagerWidget::applyFilter()
{
    m_tree->clear();
    const QString query = m_searchEdit->text().trimmed();
    QString error;

    // 搜索结果是按命中排序的扁平列表，节点的 QTreeWidgetItem 父子关系与数据库里真实的
    // parentId 无关；如果这时还允许拖拽，drop 完成后会把搜索排序误当成真实排序写库。
    m_tree->setDragEnabled(query.isEmpty());
    m_tree->setAcceptDrops(query.isEmpty());

    if (query.isEmpty()) {
        if (!appendRootLevel(&error)) {
            showDatabaseError(tr("加载主机树"), error);
            return;
        }
        m_tree->expandAll();
        return;
    }

    bool ok = false;
    QList<HostRecord> results;
    if (query.startsWith(QLatin1Char('#'))) {
        results = m_database->hostsWithTag(query.mid(1).trimmed(), &ok, &error);
    } else {
        results = m_database->searchHosts(query, &ok, &error);
    }
    if (!ok) {
        showDatabaseError(tr("搜索主机"), error);
        return;
    }

    // 搜索结果使用扁平视图，避免为了展示一个命中项而加载并展开整条祖先链；节点 id 仍是
    // 稳定数据库主键，双击连接和编辑不会受显示结构变化影响。
    for (const HostRecord &record : results) {
        m_tree->addTopLevelItem(createTreeItem(record));
    }
}

void HostManagerWidget::addGroup()
{
    HostRecord record;
    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.parentId = selectedParentGroupId();
    record.kind = HostNodeKind::Folder;
    // 分组窗口只编辑名称，不需要父分组的连接参数；仍传入共享保险库以保持构造契约统一。
    HostRecordDialog dialog(record, HostRecord{}, m_credentialVault, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    record = dialog.record();

    QString error;
    if (!m_database->saveHost(record, &error)) {
        showDatabaseError(tr("新建分组"), error);
        return;
    }
    refresh();
}

void HostManagerWidget::addHost()
{
    HostRecord record;
    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.parentId = selectedParentGroupId();
    record.kind = HostNodeKind::Host;
    record.port = quint16{22};

    HostRecord inheritedParent;
    if (!record.parentId.isEmpty()) {
        QString parentError;
        const std::optional<HostRecord> resolvedParent =
            m_database->resolveHost(record.parentId, &parentError);
        if (!resolvedParent.has_value()) {
            showDatabaseError(
                tr("读取父分组配置"),
                parentError.isEmpty() ? tr("父分组不存在") : parentError);
            return;
        }
        inheritedParent = *resolvedParent;
    }

    HostRecordDialog dialog(record, inheritedParent, m_credentialVault, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    record = dialog.record();
    QString error;
    ConnectionTarget target = record.toConnectionTarget();
    target.password = dialog.sessionPassword();
    bool hostSaved = false;
    if (dialog.shouldSavePassword() && !dialog.sessionPassword().isEmpty()) {
        if (!VaultDialog::ensureUnlocked(m_credentialVault, this)) {
            return;
        }
        // 凭据密文与 host.credentialId 必须处在同一事务中；先 saveSecret 再 saveHost 会在
        // 第二步失败时留下永远无人引用的密文。服务层的组合接口专门维护该原子性边界。
        hostSaved = m_credentialVault->saveConnectionPassword(
            record, target, tr("%1 的登录凭据").arg(record.name), &error);
        if (!hostSaved) {
            showDatabaseError(tr("保存加密凭据"), error);
            return;
        }
    }
    if (!hostSaved && !m_database->saveHost(record, &error)) {
        showDatabaseError(tr("保存主机"), error);
        return;
    }
    refresh();

    // 用户在“新建主机”对话框中可能输入了本次会话密码。密码不会落库，但立即连接可以
    // 复用这份进程内数据；从主机树再次连接时则由 Vault 或终端内联提示提供凭据。
    // saveConnectionPassword 可能为记录补上新 credentialId，因此持久字段重新从 record
    // 生成；明文仍只取对话框的本次会话副本，不会进入数据库。
    target = record.toConnectionTarget();
    target.password = dialog.sessionPassword();
    emit hostConnectionRequested(record.id, target, false);
}

void HostManagerWidget::editSelected()
{
    QString error;
    const std::optional<HostRecord> existing = selectedRecord(&error);
    if (!existing.has_value()) {
        if (!error.isEmpty()) {
            showDatabaseError(tr("读取节点"), error);
        }
        return;
    }

    HostRecord inheritedParent;
    if (existing->kind == HostNodeKind::Host && !existing->parentId.isEmpty()) {
        const std::optional<HostRecord> resolvedParent =
            m_database->resolveHost(existing->parentId, &error);
        if (!resolvedParent.has_value()) {
            showDatabaseError(
                tr("读取父分组配置"),
                error.isEmpty() ? tr("父分组不存在") : error);
            return;
        }
        inheritedParent = *resolvedParent;
    }

    HostRecordDialog dialog(*existing, inheritedParent, m_credentialVault, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    HostRecord updated = dialog.record();
    if (dialog.shouldDetachCredential()) {
        // 只解除当前主机的引用，不删除 credentials 行：一个凭据可能被多台主机共享，未经
        // 反向引用检查就删除会让其他连接静默失效。孤立凭据由后续凭据管理页显式清理。
        updated.credentialId.clear();
    }
    ConnectionTarget editedTarget = updated.toConnectionTarget();
    editedTarget.password = dialog.sessionPassword();
    bool hostSaved = false;
    if (dialog.shouldSavePassword() && !dialog.sessionPassword().isEmpty()) {
        if (!VaultDialog::ensureUnlocked(m_credentialVault, this)) {
            return;
        }
        hostSaved = m_credentialVault->saveConnectionPassword(
            updated, editedTarget, tr("%1 的登录凭据").arg(updated.name), &error);
        if (!hostSaved) {
            showDatabaseError(tr("更新加密凭据"), error);
            return;
        }
    }

    if (!hostSaved && !m_database->saveHost(updated, &error)) {
        showDatabaseError(tr("更新节点"), error);
        return;
    }
    refresh();
}

void HostManagerWidget::removeSelected()
{
    QString error;
    const std::optional<HostRecord> record = selectedRecord(&error);
    if (!record.has_value()) {
        if (!error.isEmpty()) {
            showDatabaseError(tr("读取节点"), error);
        }
        return;
    }

    const QString warning = record->kind == HostNodeKind::Folder
        ? tr("删除分组“%1”会同时删除其全部子分组和主机，且无法撤销。确定继续吗？")
              .arg(record->name)
        : tr("确定删除主机“%1”吗？此操作无法撤销。").arg(record->name);
    if (QMessageBox::warning(this,
                             tr("确认删除"),
                             warning,
                             QMessageBox::Yes | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    int removedCount = 0;
    if (!m_database->removeHost(record->id, &removedCount, &error)) {
        showDatabaseError(tr("删除节点"), error);
        return;
    }
    refresh();
}

void HostManagerWidget::importHosts()
{
    const QString defaultConfig = QDir::home().filePath(QStringLiteral(".ssh/config"));
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("导入主机配置"),
        QFileInfo::exists(defaultConfig) ? defaultConfig : QDir::homePath(),
        tr("支持的配置 (*.json *.conf *.config);;所有文件 (*)"));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this,
                              tr("无法读取导入文件"),
                              tr("无法打开 %1：%2").arg(path, file.errorString()));
        return;
    }
    const QByteArray content = file.readAll();
    QList<HostRecord> importedRecords;
    QString error;
    const bool parsed = content.trimmed().startsWith('{')
        ? ConnectionConfigCodec::importJson(content, &importedRecords, &error)
        : ConnectionConfigCodec::importOpenSsh(content, &importedRecords, &error);
    if (!parsed) {
        QMessageBox::critical(this, tr("导入失败"), error);
        return;
    }
    if (importedRecords.isEmpty()) {
        QMessageBox::information(this, tr("没有可导入主机"), tr("文件中没有可用记录。"));
        return;
    }

    const QString confirmation =
        tr("将导入 %1 条主机/分组记录。所有记录会分配新的本地主键，凭据不会从配置文件导入。")
            .arg(importedRecords.size());
    if (QMessageBox::question(this,
                              tr("确认导入"),
                              confirmation,
                              QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    // 编解码服务在单个事务内统一重映射 id、清除凭据引用并保持父子关系。把这段安全边界
    // 收口在一处，可以避免其他导入入口漏掉外部 id 冲突或半批写入回滚。
    if (!ConnectionConfigCodec::persistImported(*m_database,
                                                importedRecords,
                                                selectedParentGroupId(),
                                                &error)) {
        showDatabaseError(tr("导入主机"), error);
        return;
    }
    refresh();
    QMessageBox::information(this,
                             tr("导入完成"),
                             tr("已成功导入 %1 条记录。").arg(importedRecords.size()));
}

void HostManagerWidget::exportHosts()
{
    QList<HostRecord> records;
    QString error;
    if (!collectRecords({}, &records, &error)) {
        showDatabaseError(tr("读取导出数据"), error);
        return;
    }
    if (records.isEmpty()) {
        QMessageBox::information(this, tr("没有可导出主机"), tr("主机树当前为空。"));
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this,
        tr("导出 Shellium 主机"),
        QDir::home().filePath(QStringLiteral("shellium-hosts.json")),
        tr("Shellium JSON (*.json)"));
    if (path.isEmpty()) {
        return;
    }

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        QMessageBox::critical(this,
                              tr("无法写入导出文件"),
                              tr("无法打开 %1：%2").arg(path, output.errorString()));
        return;
    }
    const QByteArray json = ConnectionConfigCodec::exportJson(records, &error);
    if (json.isEmpty()) {
        QMessageBox::critical(this,
                              tr("导出失败"),
                              error.isEmpty() ? tr("无法编码主机配置") : error);
        output.cancelWriting();
        return;
    }
    if (output.write(json) != json.size() || !output.commit()) {
        QMessageBox::critical(this,
                              tr("导出失败"),
                              tr("写入 %1 失败：%2").arg(path, output.errorString()));
        return;
    }
    QMessageBox::information(this,
                             tr("导出完成"),
                             tr("已导出 %1 条主机/分组记录。文件不包含任何凭据。")
                                 .arg(records.size()));
}

bool HostManagerWidget::collectRecords(const QString &parentId,
                                       QList<HostRecord> *records,
                                       QString *errorMessage) const
{
    if (records == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = tr("导出记录容器为空");
        }
        return false;
    }
    bool ok = false;
    const QList<HostRecord> children =
        m_database->listHostsInGroup(parentId, &ok, errorMessage);
    if (!ok) {
        return false;
    }
    for (const HostRecord &record : children) {
        records->append(record);
        if (record.kind == HostNodeKind::Folder
            && !collectRecords(record.id, records, errorMessage)) {
            return false;
        }
    }
    return true;
}

void HostManagerWidget::activateTreeItem(QTreeWidgetItem *item)
{
    if (item == nullptr
        || static_cast<HostNodeKind>(item->data(0, HostKindRole).toInt())
            != HostNodeKind::Host) {
        return;
    }
    // 双击总是开新标签页，与单击选中/右键"连接"复用当前标签页的行为区分开。
    requestConnection(item->data(0, HostIdRole).toString(), ConnectionTarget{}, true);
}

void HostManagerWidget::requestConnection(const QString &hostId,
                                          const ConnectionTarget &sessionTarget,
                                          const bool openInNewTab)
{
    if (sessionTarget.isValid()) {
        // 新建主机后立即连接时，使用对话框产生的会话快照以保留未落库密码；其他字段仍与
        // 刚保存的记录一致。后续从树中连接会重新 resolveHost，获取最新的分组继承结果。
        emit hostConnectionRequested(hostId, sessionTarget, openInNewTab);
        return;
    }

    QString error;
    const std::optional<HostRecord> record = m_database->resolveHost(hostId, &error);
    if (!record.has_value()) {
        showDatabaseError(tr("解析主机配置"),
                          error.isEmpty() ? tr("主机记录不存在") : error);
        return;
    }
    ConnectionTarget target = record->toConnectionTarget();
    if (!record->credentialId.isEmpty()) {
        if (!VaultDialog::ensureUnlocked(m_credentialVault, this)) {
            return;
        }
        if (!m_credentialVault->applyHostCredential(*record, target, &error)) {
            showDatabaseError(tr("读取主机凭据"), error);
            return;
        }
    }
    emit hostConnectionRequested(hostId, target, openInNewTab);
}

std::optional<HostRecord> HostManagerWidget::selectedRecord(QString *errorMessage) const
{
    const QList<QTreeWidgetItem *> selected = m_tree->selectedItems();
    if (selected.isEmpty()) {
        return std::nullopt;
    }
    return m_database->findHost(selected.constFirst()->data(0, HostIdRole).toString(), errorMessage);
}

QString HostManagerWidget::selectedParentGroupId() const
{
    QString error;
    const std::optional<HostRecord> record = selectedRecord(&error);
    if (!record.has_value()) {
        return {};
    }
    return record->kind == HostNodeKind::Folder ? record->id : record->parentId;
}

QTreeWidgetItem *HostManagerWidget::createTreeItem(const HostRecord &record) const
{
    auto *item = new QTreeWidgetItem;
    item->setText(0, record.name);
    // 在模型层也明确记录常规字重，防止平台主题或后续样式表把树的默认字体改成粗体；委托
    // 绘制时仍会在此基础上放大字号，但不会改变分组和主机共同的常规字重约定。
    QFont regularFont = m_tree->font();
    regularFont.setWeight(QFont::Normal);
    item->setFont(0, regularFont);
    item->setIcon(
        0,
        record.kind == HostNodeKind::Folder
            ? localFileFolderIcon(m_tree)
            : UiIcons::symbol(
                  m_tree,
                  UiIcons::Symbol::HostItem,
                  m_tree->palette().color(QPalette::WindowText)));
    item->setData(0, HostIdRole, record.id);
    item->setData(0, HostKindRole, static_cast<int>(record.kind));
    if (record.kind == HostNodeKind::Host) {
        // 副标题必须展示"实际会连上哪里"，因此用 resolveHost 补齐从父分组继承来的账号和
        // 端口；记录自身的字段为空只代表"继承"，直接显示会漏掉最关键的信息。
        const std::optional<HostRecord> resolved = m_database->resolveHost(record.id);
        const QString detail =
            (resolved.has_value() ? *resolved : record).toConnectionTarget().connectionSpecifier(true);
        item->setData(0, HostDetailRole, detail);
        item->setToolTip(0, detail);
    }
    // 两种节点都能被拖动；只有分组能接收拖放（成为其他节点的父分组），主机不是容器。
    Qt::ItemFlags flags = item->flags() | Qt::ItemIsDragEnabled;
    if (record.kind == HostNodeKind::Folder) {
        flags |= Qt::ItemIsDropEnabled;
    } else {
        flags &= ~Qt::ItemIsDropEnabled;
    }
    item->setFlags(flags);
    return item;
}

void HostManagerWidget::persistTreeReorder(QTreeWidgetItem *movedItem, QTreeWidgetItem *oldParentItem)
{
    if (movedItem == nullptr) {
        return;
    }

    QString error;
    QTreeWidgetItem *newParentItem = movedItem->parent();
    bool ok = reindexChildren(newParentItem, &error);
    if (ok && oldParentItem != newParentItem) {
        // 被拖走的旧父节点剩余的兄弟也要重新编号，避免 sortOrder 出现意义不明的空洞。
        ok = reindexChildren(oldParentItem, &error);
    }
    if (!ok) {
        showDatabaseError(tr("保存拖放顺序"), error);
    }
    // 无论成功与否都刷新：成功时让内存树与刚写入的数据库状态严格对齐；失败时把树还原成
    // 数据库里真实的样子，避免用户以为拖放生效了，重启后才发现顺序没变。
    refresh();
}

bool HostManagerWidget::reindexChildren(QTreeWidgetItem *parentItem, QString *errorMessage)
{
    const QString parentId = parentItem != nullptr
        ? parentItem->data(0, HostIdRole).toString()
        : QString();
    const int childCount = parentItem != nullptr
        ? parentItem->childCount()
        : m_tree->topLevelItemCount();

    for (int i = 0; i < childCount; ++i) {
        QTreeWidgetItem *child = parentItem != nullptr
            ? parentItem->child(i)
            : m_tree->topLevelItem(i);
        const QString childId = child->data(0, HostIdRole).toString();
        std::optional<HostRecord> record = m_database->findHost(childId, errorMessage);
        if (!record.has_value()) {
            if (errorMessage != nullptr && errorMessage->isEmpty()) {
                *errorMessage = tr("找不到节点 %1").arg(childId);
            }
            return false;
        }
        record->parentId = parentId;
        record->sortOrder = i;
        if (!m_database->saveHost(*record, errorMessage)) {
            return false;
        }
    }
    return true;
}

void HostManagerWidget::showDatabaseError(const QString &operation, const QString &details) const
{
    QMessageBox::critical(const_cast<HostManagerWidget *>(this),
                          tr("主机数据操作失败"),
                          tr("%1失败：%2").arg(operation, details));
}
