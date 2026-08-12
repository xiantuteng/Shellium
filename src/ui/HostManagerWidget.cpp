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
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QPushButton>
#include <QSet>
#include <QSaveFile>
#include <QScrollBar>
#include <QSignalBlocker>
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

class HostChainDialog;

/** @brief 树节点保存数据库主键的角色，避开 Qt 内置显示和图标角色。 */
constexpr int HostIdRole = Qt::UserRole;

/** @brief 树节点保存 HostNodeKind 数值的角色，用于阻止分组被误连接。 */
constexpr int HostKindRole = Qt::UserRole + 1;

/** @brief 主机节点保存”账号@地址:端口”副标题的角色；分组节点不设置该角色。 */
constexpr int HostDetailRole = Qt::UserRole + 2;

/** @brief 主机节点保存解析继承后的标签列表，委托用第一枚标签绘制紧凑状态徽章。 */
constexpr int HostTagsRole = Qt::UserRole + 3;

/**
 * @brief “未分组”分区标题伪节点在 HostKindRole 里存放的哨兵值。
 *
 * 故意取 HostNodeKind 枚举范围之外的负数，而不是新增 HostNodeKind 取值：这个节点只是树的
 * 展示结构，不对应任何数据库记录，混进持久化枚举会让 saveHost/resolveHost 等代码被迫处理
 * 一个不存在的”分组/主机”之外的第三种 kind。
 */
constexpr int kSectionHeaderKindValue = -1;

/**
 * @brief 按数据库稳定主键查找当前主机树中的真实记录节点。
 * @param tree 待搜索的主机树；为空时直接返回空指针。
 * @param recordId hosts 表中的稳定主键；空字符串不代表任何真实节点。
 * @return 找到的主机或分组节点；记录不在当前树中时返回 nullptr。
 *
 * 刷新会销毁全部 QTreeWidgetItem，旧节点指针不能跨刷新使用。这里递归比较 HostIdRole，
 * 既能覆盖任意深度的分组，也会自然跳过没有数据库 id 的“未分组”展示标题。没有使用名称
 * 作为匹配条件，是因为名称正是编辑时最常变化的字段，而且不同分组允许存在同名节点。
 */
[[nodiscard]] QTreeWidgetItem *findTreeItemByRecordId(QTreeWidget *tree,
                                                       const QString &recordId)
{
    if (tree == nullptr || recordId.isEmpty()) {
        return nullptr;
    }

    QList<QTreeWidgetItem *> pendingItems;
    pendingItems.reserve(tree->topLevelItemCount());
    for (int index = 0; index < tree->topLevelItemCount(); ++index) {
        pendingItems.append(tree->topLevelItem(index));
    }

    while (!pendingItems.isEmpty()) {
        QTreeWidgetItem *item = pendingItems.takeLast();
        if (item->data(0, HostIdRole).toString() == recordId) {
            return item;
        }
        // 采用显式栈而非固定层级循环，保证将来增加更深的分组嵌套后仍能恢复选择。
        for (int childIndex = 0; childIndex < item->childCount(); ++childIndex) {
            pendingItems.append(item->child(childIndex));
        }
    }
    return nullptr;
}

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

/** @brief 主机名称使用略放大的常规字重，避免地址列表在大量粗体下显得拥挤。 */
[[nodiscard]] QFont hostNameFont(const QFont &base)
{
    return scaledFont(base, 0.5, false);
}

/** @brief 分组名称保留略放大的粗体，用字重与文件夹图标共同标识组织层级。 */
[[nodiscard]] QFont folderNameFont(const QFont &base)
{
    return scaledFont(base, 0.5, true);
}

/** @brief 连接副标题使用系统等宽字体，便于快速比较相近的 IP、账号和端口。 */
[[nodiscard]] QFont connectionDetailFont(const QFont &base)
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (base.pointSizeF() > 0.0) {
        font.setPointSizeF(std::max(8.0, base.pointSizeF() - 1.0));
    } else if (base.pixelSize() > 0) {
        font.setPixelSize(std::max(10, base.pixelSize() - 1));
    }
    font.setBold(false);
    return font;
}

/** @brief 分组展开箭头的边长（逻辑像素）。 */
constexpr int kToggleArrowSize = 8;

/** @brief 展开箭头相对分组行左缘的位置，与参考设计中的树形层级入口保持一致。 */
constexpr int kToggleArrowLeft = 8;

/** @brief 分组文件夹图标相对行左缘的位置，箭头与图标之间保留明确间隔。 */
constexpr int kFolderIconLeft = 29;

/** @brief 分组名称相对行左缘的位置，使所有层级的文字基线形成稳定纵列。 */
constexpr int kFolderTextLeft = 52;

/** @brief 主机图标圆角容器相对主机行左缘的位置。 */
constexpr int kHostIconBoxLeft = 10;

/** @brief 主机名称与连接副标题相对行左缘的位置。 */
constexpr int kHostTextLeft = 46;

/** @brief 主机图标容器尺寸；26px 在 50px 行高中既清晰又保留足够呼吸感。 */
constexpr int kHostIconBoxSize = 26;

/** @brief 分组、主机与“未分组”分区各自的固定行高。 */
constexpr int kFolderRowHeight = 34;
constexpr int kHostRowHeight = 50;
constexpr int kSectionHeaderRowHeight = 42;

/**
 * @brief 分组展开按钮的横向点击宽度（逻辑像素）。
 *
 * 箭头本身只有 8 像素，若直接用绘制矩形做命中区，用户必须精确点击三角形。24 像素让
 * 左侧入口达到易点击尺寸，同时不会覆盖后面的文件夹图标。
 */
constexpr int kToggleHitAreaWidth = 24;

/** @brief 判断索引指向的是否为分组节点。 */
[[nodiscard]] bool isFolderIndex(const QModelIndex &index)
{
    return static_cast<HostNodeKind>(index.data(HostKindRole).toInt()) == HostNodeKind::Folder;
}

/**
 * @brief 把主机节点画成“大号名称 + 小号连接信息”两行的委托。
 *
 * 单行只显示名称时，用户必须逐个悬停才知道某条记录连的是哪个账号和端口，同名不同环境的
 * 主机因此极易连错。分组只有一行名称，左侧按“箭头—文件夹—名称”排列；主机则使用带
 * 圆角容器的服务器图标。全部几何由同一个委托控制，避免平台样式改变图标间距后破坏设计。
 */
class HostItemDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    /** @brief 点中分组左侧展开按钮区域时触发；由外部接上真正的展开/收起。 */
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
        // 参考设计把层级开关放在每个分组最左侧。固定位置比跟随名称更稳定，也让不同长度
        // 的分组名共享一条箭头纵列；纵向以整行为准居中，避免受字体基线影响。
        return QRect(opt.rect.left() + kToggleArrowLeft,
                     opt.rect.center().y() - (kToggleArrowSize / 2),
                     kToggleArrowSize,
                     kToggleArrowSize);
    }

    /**
     * @brief 计算分组左侧展开按钮的完整鼠标命中区域。
     * @param option 当前树节点的样式与几何信息。
     * @param index 当前树节点索引。
     * @return 覆盖行左侧完整高度、且至少包含箭头的矩形；不可展开节点返回空矩形。
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
        QRect hitRect(opt.rect.left(),
                      opt.rect.top(),
                      std::min(kToggleHitAreaWidth, opt.rect.width()),
                      opt.rect.height());
        // 合并实际箭头矩形可防止未来微调箭头位置后出现“图形有一部分点不中”；最后裁回
        // 当前行，避免树很窄时热区越界并抢占滚动条或相邻节点的事件。
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

        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
        QStyleOptionViewItem background = option;
        initStyleOption(&background, index);
        background.text.clear();
        background.icon = {};
        background.features &= ~QStyleOptionViewItem::HasDecoration;
        // Qt 会分别绘制“缩进分支区”和“实际节点区”。若让系统样式继续处理 hover/selected，
        // 子主机左侧缩进区会出现一块与右侧圆角卡片相连的矩形尾巴。两种交互态都清除后，
        // 再由委托只在 option.rect（实际节点区域）内绘制完整圆角矩形。
        background.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver);
        const QWidget *widget = option.widget;
        QStyle *style = widget != nullptr ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);
        paintInteractionBackground(painter, option.rect, selected, hovered, widget);
        paintGuideLine(painter, option, index);

        // 主机图标先放进独立圆角容器：未选中时是白色/深色表面卡片，选中时改为品牌绿色。
        // 这样图标不会漂浮在两行文字旁边，当前连接目标也能通过颜色块被快速定位。
        const QRect iconBox(option.rect.left() + kHostIconBoxLeft,
                            option.rect.center().y() - (kHostIconBoxSize / 2),
                            kHostIconBoxSize,
                            kHostIconBoxSize);
        QColor configuredSurface;
        if (widget != nullptr) {
            configuredSurface = QColor(widget->property("hostIconSurfaceColor").toString());
        }
        QColor iconBoxFill = selected
            ? ShelliumUi::accentColor()
            : (configuredSurface.isValid()
                   ? configuredSurface
                   : background.palette.color(QPalette::Base));
        QColor iconBoxBorder = background.palette.color(QPalette::Text);
        iconBoxBorder.setAlphaF(selected ? 0.0F : 0.12F);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(selected ? Qt::NoPen : QPen(iconBoxBorder, 1));
        painter->setBrush(iconBoxFill);
        painter->drawRoundedRect(iconBox.adjusted(0, 0, -1, -1), 8, 8);

        const QRect hostSymbolRect = iconBox.adjusted(6, 6, -6, -6);
        if (selected && widget != nullptr) {
            UiIcons::symbol(widget,
                            UiIcons::Symbol::HostItem,
                            QColor(QStringLiteral("#0c2f24")))
                .paint(painter, hostSymbolRect, Qt::AlignCenter);
        } else {
            qvariant_cast<QIcon>(index.data(Qt::DecorationRole))
                .paint(painter, hostSymbolRect, Qt::AlignCenter);
        }
        painter->restore();

        const QFont nameFont = hostNameFont(option.font);
        const QFont detailFont = connectionDetailFont(option.font);
        const QFontMetrics nameMetrics(nameFont);
        const QFontMetrics detailMetrics(detailFont);
        // 选中态不再反相成高亮文字色：底纹很浅，白色高亮文字反而对比不足，正常文字色在
        // 浅色底纹上已经足够醒目，也让选中前后文字颜色保持一致、不"跳色"。
        const QColor baseColor = background.palette.color(QPalette::Text);

        const int textLeft = option.rect.left() + kHostTextLeft;
        const QRect fullTextRect(textLeft,
                                 option.rect.top(),
                                 std::max(0, option.rect.right() - textLeft - 7),
                                 option.rect.height());
        const int textBlockHeight = nameMetrics.height() + detailMetrics.height() + 1;
        const int textBlockTop = fullTextRect.center().y() - (textBlockHeight / 2);
        const QStringList tags = index.data(HostTagsRole).toStringList();
        const QString visibleTag = tags.isEmpty() ? QString{} : tags.constFirst();
        const QFont tagFont = scaledFont(option.font, -2.0, true);
        const QFontMetrics tagMetrics(tagFont);
        const int tagWidth = visibleTag.isEmpty()
            ? 0
            : std::min(72, tagMetrics.horizontalAdvance(visibleTag) + 12);
        const int nameAvailableWidth = std::max(
            0, fullTextRect.width() - (tagWidth > 0 ? tagWidth + 7 : 0));
        const QString name = index.data(Qt::DisplayRole).toString();
        const QString elidedName = nameMetrics.elidedText(name, Qt::ElideRight, nameAvailableWidth);
        const int paintedNameWidth = nameMetrics.horizontalAdvance(elidedName);

        painter->save();
        const QRect nameRect(fullTextRect.left(),
                             textBlockTop,
                             nameAvailableWidth,
                             nameMetrics.height());
        painter->setFont(nameFont);
        painter->setPen(baseColor);
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, elidedName);

        if (tagWidth > 0) {
            // 标签紧跟名称而不是固定贴右，阅读顺序与“名称 测试”一致；当名称过长时先截断
            // 名称，仍保留标签，因为环境标签通常比名称末尾更能避免误连生产服务器。
            const int tagLeft = std::min(nameRect.right() + 7,
                                         nameRect.left() + paintedNameWidth + 7);
            const QRect tagRect(tagLeft,
                                nameRect.center().y() - 9,
                                tagWidth,
                                18);
            QColor tagFill = ShelliumUi::accentColor();
            tagFill.setAlphaF(0.16F);
            QColor tagText = ShelliumUi::accentColor().darker(135);
            painter->setPen(Qt::NoPen);
            painter->setBrush(tagFill);
            painter->drawRoundedRect(tagRect, 7, 7);
            painter->setFont(tagFont);
            painter->setPen(tagText);
            painter->drawText(tagRect,
                              Qt::AlignCenter,
                              tagMetrics.elidedText(visibleTag,
                                                    Qt::ElideRight,
                                                    tagRect.width() - 8));
        }

        QColor detailColor = baseColor;
        detailColor.setAlphaF(0.58F);
        const QRect detailRect(fullTextRect.left(),
                               nameRect.bottom() + 1,
                               fullTextRect.width(),
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
            hint.setHeight(kSectionHeaderRowHeight);
            return hint;
        }
        if (index.data(HostDetailRole).toString().isEmpty()) {
            hint.setHeight(kFolderRowHeight);
            return hint;
        }
        hint.setHeight(kHostRowHeight);
        return hint;
    }

    bool editorEvent(QEvent *event,
                     QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        // 箭头已不在 Qt 的分支装饰列里，QTreeView 自带的“点击分支即展开”对它无效，必须由
        // 委托自己做命中判断。热区使用左侧完整按钮区域而不是 8×8 的三角形；与复选框一致
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
    /** @brief 画分组：左侧箭头、文件夹和名称依次排列；空分组不追加冗余数量文字。 */
    void paintFolder(QPainter *painter,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) const
    {
        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
        QStyleOptionViewItem background = option;
        initStyleOption(&background, index);
        const QString name = background.text;
        background.text.clear();
        background.icon = {};
        background.features &= ~QStyleOptionViewItem::HasDecoration;
        // 分组也使用同一套自绘交互背景，避免顶层和子层节点在悬停时出现两种圆角规则。
        background.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver);
        const QWidget *widget = option.widget;
        QStyle *style = widget != nullptr ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);
        paintInteractionBackground(painter, option.rect, selected, hovered, widget);
        paintGuideLine(painter, option, index);

        const bool emptyFolder = index.model() == nullptr || index.model()->rowCount(index) == 0;
        const QRect iconRect(option.rect.left() + kFolderIconLeft,
                             option.rect.center().y() - 9,
                             18,
                             18);
        qvariant_cast<QIcon>(index.data(Qt::DecorationRole))
            .paint(painter,
                   iconRect,
                   Qt::AlignCenter,
                   emptyFolder ? QIcon::Disabled : QIcon::Normal);

        const QRect arrowRect = toggleArrowRect(option, index);
        QColor baseColor = background.palette.color(QPalette::Text);
        if (emptyFolder) {
            baseColor.setAlphaF(0.58F);
        }

        painter->save();
        // 分组保留粗体作为组织层级标题；具体主机名称使用常规字重，避免连续地址列表形成
        // 大片视觉重块。两者字号一致，因此层级变化不会造成文字基线跳动。
        const QFont nameFont = folderNameFont(background.font);
        painter->setFont(nameFont);
        painter->setPen(baseColor);
        const QFontMetrics metrics(nameFont);
        const QRect textRect(option.rect.left() + kFolderTextLeft,
                             option.rect.top(),
                             std::max(0,
                                      option.rect.width() - kFolderTextLeft - 8),
                             option.rect.height());
        painter->drawText(textRect,
                          Qt::AlignLeft | Qt::AlignVCenter,
                          metrics.elidedText(name, Qt::ElideRight, textRect.width()));
        if (!arrowRect.isNull()) {
            drawToggleArrow(painter, arrowRect, (option.state & QStyle::State_Open) != 0, baseColor);
        }
        painter->restore();
    }

    /**
     * @brief 只在实际节点区域绘制完整圆角的悬停或选中背景。
     * @param rect 委托收到的实际节点矩形，不包含父级缩进区。
     * @param selected 是否处于选中状态；优先于悬停状态。
     * @param hovered 鼠标是否位于当前节点。
     * @param widget 提供主题动态属性的主机树；允许为空并使用安全回退色。
     *
     * 不能把背景交给 QTreeView 的通用 item 规则：子节点的分支缩进区会被平台样式连带填充，
     * 形成用户指出的左侧“小尾巴”。选中态继续使用半透明品牌色，悬停态读取 MainWindow
     * 写入的主题色，两者都只绘制一个边界明确的圆角矩形。
     */
    static void paintInteractionBackground(QPainter *painter,
                                           const QRect &rect,
                                           const bool selected,
                                           const bool hovered,
                                           const QWidget *widget)
    {
        if (!selected && !hovered) {
            return;
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        QColor fill;
        QColor border;
        if (selected) {
            fill = ShelliumUi::accentColor();
            fill.setAlphaF(0.13F);
            border = ShelliumUi::accentColor();
            border.setAlphaF(0.42F);
        } else {
            if (widget != nullptr) {
                fill = QColor(widget->property("hostHoverBackgroundColor").toString());
            }
            if (!fill.isValid()) {
                fill = QColor(QStringLiteral("#e1e7eb"));
            }
        }

        const QRect fillRect = rect.adjusted(0, 2, -1, -2);
        painter->setPen(selected ? QPen(border, 1) : Qt::NoPen);
        painter->setBrush(fill);
        painter->drawRoundedRect(fillRect, 9, 9);
        painter->restore();
    }

    /**
     * @brief 在子节点缩进区绘制中性的父子层级连线。
     * @param painter 当前节点的绘制画笔，不得为空。
     * @param option 当前节点几何与调色板信息。
     * @param index 当前节点索引；顶层节点不会绘制连线。
     *
     * 连线表达分组归属，必须保留；但它始终使用同一低透明度中性色，不随 hover/selected
     * 变色，也不参与背景填充。因此右侧圆角卡片仍然完整独立，左侧不会再出现矩形背景尾巴
     * 或绿色选中强调条。
     */
    static void paintGuideLine(QPainter *painter,
                               const QStyleOptionViewItem &option,
                               const QModelIndex &index)
    {
        Q_ASSERT(painter != nullptr);
        if (!index.parent().isValid()) {
            return;
        }

        QColor lineColor = option.palette.color(QPalette::Text);
        lineColor.setAlphaF(0.14F);
        painter->save();
        painter->setPen(QPen(lineColor, 1));
        const int lineX = option.rect.left() - 15;
        painter->drawLine(lineX, option.rect.top(), lineX, option.rect.bottom());
        painter->drawLine(lineX,
                          option.rect.center().y(),
                          option.rect.left() - 3,
                          option.rect.center().y());
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
    /**
     * @brief 禁止 Qt 在子节点缩进区绘制平台分支背景或装饰。
     * @param painter Qt 原本用于绘制分支区域的画笔；此处不使用。
     * @param rect 当前分支缩进区域；保持透明，不做填充。
     * @param index 对应节点索引；层级线改由 HostItemDelegate 稳定绘制。
     *
     * 即使委托清除了 MouseOver/Selected，QTreeView 仍可能通过 drawBranches 单独给缩进区
     * 绘制悬停或选中底色，形成圆角卡片左边的矩形尾巴。主机树已经自绘展开箭头和中性层级
     * 连线，关闭平台层不会损失交互元素，只会避免系统样式重复绘制或填充缩进区。
     */
    void drawBranches(QPainter *painter,
                      const QRect &rect,
                      const QModelIndex &index) const override
    {
        Q_UNUSED(painter)
        Q_UNUSED(rect)
        Q_UNUSED(index)
    }

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
 * 可见字段时不会被静默清空。密码不写入 HostRecord，而是随保存动作自动交给 CredentialVault：
 * 开启保险箱密码时使用主密码加密，关闭时使用本机随机密钥透明加密。
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
                              ApplicationDatabase *database,
                              CredentialVault *credentialVault,
                              QWidget *parent = nullptr)
        : QDialog(parent)
        , m_initial(initial)
        , m_inheritedParent(inheritedParent)
        , m_database(database)
        , m_credentialVault(credentialVault)
        , m_nameEdit(new QLineEdit(initial.name, this))
    {
        Q_ASSERT(m_database != nullptr);
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
            m_passwordEdit->setPlaceholderText(tr("输入后随主机自动保存"));
            m_identityEdit = new QLineEdit(initial.identityFile, this);
            m_identityEdit->setObjectName(QStringLiteral("hostIdentityEdit"));
            m_identityEdit->setPlaceholderText(
                tr("留空则继承父分组或使用 Agent/默认密钥"));
            m_proxyJumpsEdit = new QLineEdit(
                initial.proxyJumps.join(QLatin1Char(',')), this);
            m_proxyJumpsEdit->setObjectName(QStringLiteral("hostProxyJumpsEdit"));
            m_proxyJumpsEdit->setPlaceholderText(tr("逗号分隔；留空则继承父分组"));
            m_hostChain = initial.hostChain;
            m_hostChainButton = new QPushButton(this);
            m_hostChainButton->setObjectName(QStringLiteral("hostChainButton"));
            m_hostChainButton->setMinimumWidth(260);
            UiIcons::decorateButton(
                m_hostChainButton,
                UiIcons::symbol(
                    this,
                    UiIcons::Symbol::ArrowUp,
                    this->palette().color(QPalette::WindowText)),
                tr("编辑当前主机在最终连接前依次经过的跳板机"));
            refreshHostChainSummary();
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

            // 认证区严格按用户填写路径排列：用户名、密码、私钥和受管主机链保持在主路径中；
            // 自由文本 ProxyJump 收进末尾的高级入口，避免与主机链形成两个并列的首选方案。
            // 保存凭据和旧版算法开关仍保持直接可见，因为它们会明显改变安全行为。
            auto *authCard = new SettingsCard(tr("认证与跳板"), this);
            authCard->addRow(tr("用户名"), m_usernameEdit);
            // 查看入口使用 QLineEdit 的 TrailingPosition 内嵌动作，而不是额外占据一列的文字
            // 按钮。Qt 会自动为文本保留右侧图标空间，长密码不会画到眼睛图标下面；动作仍是
            // 独立 QAction，便于禁用、提供无障碍名称和在主题切换时替换图标。
            m_passwordVisibilityAction = m_passwordEdit->addAction(
                UiIcons::symbol(
                    this,
                    UiIcons::Symbol::Eye,
                    this->palette().color(QPalette::WindowText)),
                QLineEdit::TrailingPosition);
            m_passwordVisibilityAction->setObjectName(
                QStringLiteral("hostPasswordVisibilityAction"));
            m_passwordVisibilityAction->setCheckable(true);
            m_passwordVisibilityAction->setText(tr("查看密码"));
            // 未启用主密码时仍保持眼睛图标可点击。直接禁用虽然阻止了明文显示，却也切断了
            // 用户理解原因和找到设置入口的路径；实际授权仍在 toggled 回调第一层严格拦截。
            m_passwordVisibilityAction->setEnabled(true);
            m_passwordVisibilityAction->setToolTip(
                m_credentialVault->passwordProtectionEnabled()
                    ? tr("查看已保存密码；保险箱锁定时会先要求输入主密码")
                    : tr("点击查看如何启用主密码保护"));
            authCard->addRow(
                tr("密码"),
                m_passwordEdit,
                tr("密码会自动保存；启用主密码保护后可解锁查看明文。"));
            authCard->addRow(tr("私钥文件"), identityContainer);
            authCard->addRow(tr("主机链"),
                             m_hostChainButton,
                             tr("选择主机管理中的已有主机作为跳板，也可以在链编辑器中新增主机。"));
            authCard->addRow(tr("旧版兼容"),
                             m_legacyHmacToggle,
                             tr("仅为该主机允许 hmac-sha1（不继承）。"));

            m_advancedSettingsButton = new QToolButton(authCard);
            m_advancedSettingsButton->setObjectName(
                QStringLiteral("hostAdvancedSettingsButton"));
            m_advancedSettingsButton->setProperty("advancedDisclosure", true);
            m_advancedSettingsButton->setCheckable(true);
            m_advancedSettingsButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            m_advancedSettingsButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            m_advancedSettingsButton->setAccessibleName(tr("展开 ProxyJump 高级设置"));
            // 已有主机若保存过手工 ProxyJump，打开编辑器时必须直接展示真实生效值，不能因
            // “弱化入口”而把旧路由悄悄藏起来；全新配置和单纯继承父分组时则默认折叠。
            m_advancedSettingsButton->setChecked(!initial.proxyJumps.isEmpty());
            authCard->addFullWidthWidget(m_advancedSettingsButton);
            authCard->addRow(
                tr("ProxyJump"),
                m_proxyJumpsEdit,
                tr("兼容 OpenSSH 的高级手工跳板配置；通常请优先使用上方主机链。"));
            m_proxyJumpRow = m_proxyJumpsEdit->parentWidget();
            m_proxyJumpRow->setObjectName(QStringLiteral("hostProxyJumpRow"));
            refreshProxyJumpDisclosure();

            connect(browseButton, &QPushButton::clicked, this, [this]() {
                const QString path = QFileDialog::getOpenFileName(
                    this, tr("选择 SSH 私钥文件"));
                if (!path.isEmpty()) {
                    m_identityEdit->setText(path);
                }
            });
            connect(m_hostChainButton,
                    &QPushButton::clicked,
                    this,
                    &HostRecordDialog::editHostChain);
            connect(m_proxyJumpsEdit,
                    &QLineEdit::textEdited,
                    this,
                    [this](const QString &manualProxyJump) {
                        // 两套路线同时存在会让“实际经过哪些主机”变得不可判断。只有用户亲自
                        // 输入非空高级配置时才清除受管链；构造阶段 setText 不会触发该分支。
                        if (!manualProxyJump.trimmed().isEmpty() && !m_hostChain.isEmpty()) {
                            m_hostChain.clear();
                            refreshHostChainSummary();
                        }
                    });
            connect(m_proxyJumpsEdit,
                    &QLineEdit::textChanged,
                    this,
                    [this]() { refreshProxyJumpDisclosure(); });
            connect(m_advancedSettingsButton,
                    &QToolButton::toggled,
                    this,
                    [this]() { refreshProxyJumpDisclosure(); });
            connect(m_passwordVisibilityAction,
                    &QAction::toggled,
                    this,
                    [this](const bool visible) {
                        if (!m_credentialVault->passwordProtectionEnabled()) {
                            // setChecked(false) 会再次发出 toggled。用信号阻断器复位动作，确保
                            // 一次点击只弹出一条提示，且密码框始终保持 Password 回显模式。
                            const QSignalBlocker blocker(m_passwordVisibilityAction);
                            m_passwordVisibilityAction->setChecked(false);
                            m_passwordEdit->setEchoMode(QLineEdit::Password);
                            if (visible) {
                                QMessageBox::information(
                                    this,
                                    tr("需要主密码保护"),
                                    tr("查看主机密码需要先启用主密码保护。请前往“设置 → "
                                       "连接与安全 → 凭据保险箱”，开启“使用主密码保护凭据”。"));
                            }
                            return;
                        }
                        // “已启用”只决定按钮是否可用，真正显示明文前仍必须确认当前会话持有
                        // 有效主密码密钥。即使这条历史凭据还采用透明本机密钥，也不能绕过
                        // 用户刚启用的保险箱授权边界直接显示。
                        if (visible && !m_credentialVault->isUnlocked()
                            && !VaultDialog::ensureUnlocked(m_credentialVault, this)) {
                            m_passwordVisibilityAction->setChecked(false);
                            return;
                        }
                        if (visible && !m_passwordLoaded && !loadSavedPassword(true)) {
                            m_passwordVisibilityAction->setChecked(false);
                            return;
                        }
                        m_passwordEdit->setEchoMode(
                            visible ? QLineEdit::Normal : QLineEdit::Password);
                        m_passwordVisibilityAction->setText(
                            visible ? tr("隐藏密码") : tr("查看密码"));
                        m_passwordVisibilityAction->setToolTip(
                            visible ? tr("隐藏密码")
                                    : tr("查看已保存密码；保险箱锁定时会先要求输入主密码"));
                        m_passwordVisibilityAction->setIcon(
                            UiIcons::symbol(
                                this,
                                visible ? UiIcons::Symbol::EyeHidden : UiIcons::Symbol::Eye,
                                this->palette().color(QPalette::WindowText)));
                    });
            connect(m_passwordEdit,
                    &QLineEdit::textEdited,
                    this,
                    [this]() {
                        // 用户输入（包括清空）后，输入框就是本次编辑的权威值；查看按钮不得
                        // 再从旧凭据加载并覆盖刚输入的内容。textEdited 不会被程序 setText
                        // 触发，因此下方自动载入已保存密码仍能准确维护同一状态位。
                        m_passwordLoaded = true;
                        m_passwordLoadedFromStorage = false;
                    });

            // 已保存密码先读入输入框并保持 Password 回显，因此编辑页能直接显示与真实长度
            // 对应的星号。透明本机凭据无需交互即可读取；主密码保险库若因闲置已锁定，则延迟
            // 到用户点击“查看”时再弹解锁框，避免仅打开配置窗口就打断操作。
            loadSavedPassword(false);

            connect(m_credentialVault, &CredentialVault::vaultReset, this, [this]() {
                // 当前编辑器持有打开窗口时的 HostRecord 副本。重置事务已经在数据库中清空
                // credential_id，若继续保存旧副本会把失效引用写回，因此必须同步清理直接凭据
                // 与继承凭据。用户本次亲自输入的新密码仍保留，稍后可按无主密码模式重新保存。
                m_initial.credentialId.clear();
                m_inheritedParent.credentialId.clear();
                if (m_passwordLoadedFromStorage) {
                    m_passwordEdit->clear();
                    m_passwordLoaded = false;
                    m_passwordLoadedFromStorage = false;
                }
                m_passwordEdit->setEchoMode(QLineEdit::Password);
                m_passwordEdit->setPlaceholderText(tr("保险箱已重置，请重新输入凭据"));
                if (m_passwordVisibilityAction != nullptr) {
                    const QSignalBlocker blocker(m_passwordVisibilityAction);
                    m_passwordVisibilityAction->setChecked(false);
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
            result.hostChain = m_hostChain;
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
     * @brief 链编辑器是否已经现场保存过独立的新主机。
     * @return true 表示即使当前目标随后取消，主机侧栏也应刷新以展示新建记录。
     */
    [[nodiscard]] bool relatedHostsCreated() const { return m_relatedHostsCreated; }

private:
    /**
     * @brief 把当前记录引用的已保存密码读入掩码输入框。
     * @param allowUnlock true 时允许弹出主密码解锁窗口；false 只尝试无交互读取。
     * @return 没有已保存凭据或成功载入时返回 true；需要解锁但不允许交互、解锁取消或读取
     *         失败时返回 false。失败不会清空既有 credentialId。
     */
    bool loadSavedPassword(const bool allowUnlock)
    {
        if (m_passwordEdit == nullptr || m_passwordLoaded) {
            return true;
        }
        HostRecord credentialSource = m_initial;
        if (credentialSource.credentialId.isEmpty()) {
            credentialSource.credentialId = m_inheritedParent.credentialId;
        }
        if (credentialSource.credentialId.isEmpty()) {
            return true;
        }

        QString credentialError;
        if (m_credentialVault->credentialRequiresUnlock(
                credentialSource.credentialId, &credentialError)
            && !m_credentialVault->isUnlocked()) {
            if (!allowUnlock
                || !VaultDialog::ensureUnlocked(m_credentialVault, this)) {
                m_passwordEdit->setPlaceholderText(tr("已保存；解锁保险箱后可查看"));
                return false;
            }
        }
        ConnectionTarget target = credentialSource.toConnectionTarget();
        if (!m_credentialVault->applyHostCredential(
                credentialSource, target, &credentialError)) {
            if (allowUnlock) {
                QMessageBox::warning(
                    this,
                    tr("无法读取已保存密码"),
                    credentialError.isEmpty() ? tr("凭据不存在或已经损坏。")
                                              : credentialError);
            }
            return false;
        }
        m_passwordEdit->setText(target.password);
        target.password.fill(QChar::Null);
        target.password.clear();
        m_passwordLoaded = true;
        m_passwordLoadedFromStorage = true;
        return true;
    }

    /** @brief 打开 Termius 风格的独立主机链编辑器，并把确认后的有序 id 写回当前草稿。 */
    void editHostChain();

    /**
     * @brief 同步 ProxyJump 高级入口的摘要、箭头和明细行可见性。
     *
     * 折叠状态下仍显示“已配置”或“继承”摘要，是为了让用户无需展开即可判断连接是否还
     * 存在手工跳板路径；展开状态只控制展示，不改变字段内容或父分组继承语义。
     */
    void refreshProxyJumpDisclosure()
    {
        if (m_advancedSettingsButton == nullptr || m_proxyJumpsEdit == nullptr
            || m_proxyJumpRow == nullptr) {
            return;
        }

        const bool expanded = m_advancedSettingsButton->isChecked();
        const bool hasExplicitProxyJump = !m_proxyJumpsEdit->text().trimmed().isEmpty();
        // 非空受管链拥有路由优先级，此时父分组即使配置了 ProxyJump 也不会参与连接，摘要
        // 不能错误宣称“正在继承”；只有直连草稿才提示潜在生效的继承路径。
        const bool inheritsProxyJump = !hasExplicitProxyJump && m_hostChain.isEmpty()
            && !m_inheritedParent.proxyJumps.isEmpty();
        QString summary = tr("高级设置");
        if (hasExplicitProxyJump) {
            summary = tr("高级设置 · ProxyJump 已配置");
        } else if (inheritsProxyJump) {
            summary = tr("高级设置 · 继承 ProxyJump");
        }

        m_advancedSettingsButton->setText(summary);
        m_advancedSettingsButton->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        m_advancedSettingsButton->setAccessibleName(
            expanded ? tr("收起 ProxyJump 高级设置") : tr("展开 ProxyJump 高级设置"));
        m_advancedSettingsButton->setToolTip(
            tr("仅在兼容 OpenSSH 配置或需要手工指定跳板地址时使用"));
        m_proxyJumpRow->setVisible(expanded);
    }

    /** @brief 根据当前链路数量刷新主机配置页入口摘要，避免用户必须再次打开才能确认状态。 */
    void refreshHostChainSummary()
    {
        if (m_hostChainButton == nullptr) {
            return;
        }
        if (m_hostChain.isEmpty()) {
            m_hostChainButton->setText(tr("主机链  ·  直连  ›"));
        } else if (m_hostChain.size() == 1) {
            const std::optional<HostRecord> jump = m_database->findHost(m_hostChain.constFirst());
            m_hostChainButton->setText(
                tr("主机链  ·  经 %1  ›")
                    .arg(jump.has_value() ? jump->name : tr("失效主机")));
        } else {
            m_hostChainButton->setText(tr("主机链  ·  %1 个跳板  ›").arg(m_hostChain.size()));
        }
        // 主机链会覆盖父分组 ProxyJump；链路增删后同步摘要，避免继续显示已经不生效的继承。
        refreshProxyJumpDisclosure();
    }

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

        QList<HostRecord> routeRecords;
        QSet<QString> routeIds;
        for (const QString &jumpId : effective.hostChain) {
            QString routeError;
            const QList<HostRecord> expanded = m_database->resolveConnectionChain(
                jumpId, &routeError);
            if (expanded.isEmpty()) {
                QMessageBox::warning(
                    this,
                    tr("无法测试主机链"),
                    routeError.isEmpty() ? tr("跳板机不存在或链路无效：%1").arg(jumpId)
                                         : routeError);
                return;
            }
            for (const HostRecord &hop : expanded) {
                if (routeIds.contains(hop.id) || hop.id == effective.id) {
                    QMessageBox::warning(
                        this, tr("无法测试主机链"), tr("主机链包含重复节点或环路：%1").arg(hop.name));
                    return;
                }
                routeIds.insert(hop.id);
                routeRecords.append(hop);
            }
        }
        effective.hostChain.clear();
        routeRecords.append(effective);

        QList<ConnectionTarget> connectionPlan;
        connectionPlan.reserve(routeRecords.size());
        bool vaultReady = false;
        for (qsizetype index = 0; index < routeRecords.size(); ++index) {
            const HostRecord &hop = routeRecords.at(index);
            ConnectionTarget hopTarget = hop.toConnectionTarget();
            if (routeRecords.size() > 1) {
                hopTarget.proxyJumps.clear();
            }
            if (index + 1 == routeRecords.size()) {
                hopTarget.password = sessionPassword();
            }
            if (hopTarget.password.isEmpty() && !hop.credentialId.isEmpty()) {
                QString credentialError;
                const bool requiresUnlock = m_credentialVault->credentialRequiresUnlock(
                    hop.credentialId, &credentialError);
                if (!credentialError.isEmpty()) {
                    QMessageBox::warning(this, tr("无法测试连接"), credentialError);
                    return;
                }
                if (requiresUnlock && !vaultReady) {
                    if (!VaultDialog::ensureUnlocked(m_credentialVault, this)) {
                        return;
                    }
                    vaultReady = true;
                }
                if (!m_credentialVault->applyHostCredential(hop, hopTarget, &credentialError)) {
                    QMessageBox::warning(
                        this,
                        tr("无法测试连接"),
                        credentialError.isEmpty()
                            ? tr("无法读取“%1”的已保存凭据。").arg(hop.name)
                            : credentialError);
                    return;
                }
            }
            connectionPlan.append(hopTarget);
        }

        m_testInProgress = true;
        m_testConnectionButton->setEnabled(false);
        m_testConnectionButton->setText(tr("测试中…"));

        // 测试连接复用生产 SSH 状态机，才能覆盖 ProxyJump、私钥、密码和旧版 HMAC 等完整
        // 配置。第二个参数关闭 Shell 集成安装，避免一次只读测试在远端写入临时脚本。
        const QString testSpecifier = connectionPlan.constLast().connectionSpecifier(true);
        auto *session = new SshSessionStack(connectionPlan, false, this);
        // 工作线程已经复制了测试快照，立即擦除这里的临时密码副本，避免信号闭包再额外持有
        // 一份明文直到会话销毁；输入框本身仍由用户控制，保存/再次测试行为不会受影响。
        for (ConnectionTarget &temporaryTarget : connectionPlan) {
            temporaryTarget.password.fill(QChar::Null);
            temporaryTarget.password.clear();
        }
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
    /** @brief 主机链选择和现场新增主机所使用的应用数据库，不拥有生命周期。 */
    ApplicationDatabase *m_database = nullptr;
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
    /** @brief 折叠手工 ProxyJump 的次要入口；摘要始终提示显式配置或父分组继承状态。 */
    QToolButton *m_advancedSettingsButton = nullptr;
    /** @brief SettingsCard 为 ProxyJump 创建的完整标签/输入行，用于整体隐藏无障碍与布局内容。 */
    QWidget *m_proxyJumpRow = nullptr;
    /** @brief 当前对话框中的受管链草稿；只有外层主机保存后才持久化到 owner_host_id。 */
    QStringList m_hostChain;
    QPushButton *m_hostChainButton = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    /** @brief 密码框右侧的眼睛图标动作，仅在启用保险箱密码时允许切换明文。 */
    QAction *m_passwordVisibilityAction = nullptr;
    /** @brief 输入框是否已有来自凭据或用户编辑的权威值，避免查看时用旧凭据覆盖新输入。 */
    bool m_passwordLoaded = false;
    /** @brief 当前输入值是否由旧持久凭据解密得到；保险箱重置时仅清除此类旧明文。 */
    bool m_passwordLoadedFromStorage = false;
    ToggleSwitch *m_legacyHmacToggle = nullptr;
    SettingsCard *m_authenticationCard = nullptr;
    QPushButton *m_testConnectionButton = nullptr;
    /** @brief 当前测试使用的后台 SSH 栈；结束后由 Qt 事件循环安全销毁。 */
    SshSessionStack *m_testSession = nullptr;
    /** @brief 防止底层重复完成信号产生多个结果弹窗或并行测试。 */
    bool m_testInProgress = false;
    /** @brief 记录链编辑器产生的独立数据库变更，供外层在取消目标编辑时仍刷新主机树。 */
    bool m_relatedHostsCreated = false;
};

/**
 * @brief 绘制主机链与跳板机选择器中的两行主机卡片。
 *
 * 主机链行额外绘制六点拖拽手柄，并把名称/连接地址相对整张卡片居中；选择器行不显示手柄，
 * 内容左对齐。委托直接绘制选中与悬停底纹，避免 macOS、Windows 原生列表样式在圆角容器内
 * 产生蓝色矩形高亮，从而让同一交互在深浅主题和各平台保持一致。
 */
class HostChainRowDelegate final : public QStyledItemDelegate {
public:
    /**
     * @brief 创建一类主机卡片委托。
     * @param showDragHandle true 为可排序链路行绘制六点手柄，false 为普通候选项。
     * @param centerContent true 让两行文字相对整卡居中，false 使用标准左对齐。
     * @param parent Qt 父对象。
     */
    HostChainRowDelegate(const bool showDragHandle,
                         const bool centerContent,
                         QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
        , m_showDragHandle(showDragHandle)
        , m_centerContent(centerContent)
    {
    }

    /** @brief 返回参考设计规定的紧凑双行高度；宽度仍由列表视口决定。 */
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const override
    {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        hint.setHeight(m_showDragHandle ? 68 : 62);
        return hint;
    }

protected:
    /**
     * @brief 绘制交互底纹、拖拽手柄、主机名称和等宽连接说明。
     * @param painter 当前列表视口的绘制器，不可为空。
     * @param option 当前行的矩形、状态与调色板。
     * @param index 当前模型索引；DisplayRole 使用“名称\n连接信息”格式。
     */
    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        Q_ASSERT(painter != nullptr);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
        const QColor textColor = option.palette.color(QPalette::Text);
        QColor interactionFill = selected ? ShelliumUi::accentColor() : textColor;
        interactionFill.setAlphaF(selected ? 0.14F : (hovered ? 0.055F : 0.0F));
        if (interactionFill.alpha() > 0) {
            // 填满列表内部整行而不额外画圆角：外层 QListWidget 已负责裁剪圆角，连续候选项
            // 因而共享一个整洁容器，选中态也不会在边缘留下系统高亮色缝隙。
            painter->fillRect(option.rect, interactionFill);
        }

        if (index.row() + 1 < index.model()->rowCount(index.parent())) {
            QColor separator = textColor;
            separator.setAlphaF(0.10F);
            painter->setPen(QPen(separator, 1));
            painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
        }

        if (m_showDragHandle) {
            // 六个圆点只属于可拖动跳板行，并固定在整卡左侧；文字区域不为它参与居中计算，
            // 因此名称和地址始终相对整张卡片居中，不会被手柄推向右侧。
            QColor gripColor = textColor;
            gripColor.setAlphaF(0.34F);
            painter->setPen(Qt::NoPen);
            painter->setBrush(gripColor);
            const int centerX = option.rect.left() + 22;
            const int centerY = option.rect.center().y();
            for (const int columnOffset : {-3, 3}) {
                for (const int rowOffset : {-7, 0, 7}) {
                    painter->drawEllipse(
                        QPointF(centerX + columnOffset, centerY + rowOffset), 1.4, 1.4);
                }
            }
        }

        const QString displayText = index.data(Qt::DisplayRole).toString();
        const qsizetype lineBreak = displayText.indexOf(QLatin1Char('\n'));
        const QString title = lineBreak < 0 ? displayText : displayText.left(lineBreak);
        const QString detail = lineBreak < 0 ? QString{} : displayText.mid(lineBreak + 1);
        const QFont titleFont = scaledFont(option.font, 0.0, true);
        const QFont detailFont = connectionDetailFont(option.font);
        const QFontMetrics titleMetrics(titleFont);
        const QFontMetrics detailMetrics(detailFont);
        const int blockHeight = titleMetrics.height() + detailMetrics.height() + 1;
        const int blockTop = option.rect.center().y() - (blockHeight / 2);
        const int horizontalInset = m_centerContent ? 42 : 16;
        const QRect textBounds = option.rect.adjusted(horizontalInset, 0, -horizontalInset, 0);
        const Qt::Alignment horizontalAlignment = m_centerContent ? Qt::AlignHCenter
                                                                  : Qt::AlignLeft;

        painter->setFont(titleFont);
        painter->setPen(textColor);
        painter->drawText(
            QRect(textBounds.left(), blockTop, textBounds.width(), titleMetrics.height()),
            horizontalAlignment | Qt::AlignVCenter,
            titleMetrics.elidedText(title, Qt::ElideRight, textBounds.width()));

        QColor detailColor = textColor;
        detailColor.setAlphaF(0.58F);
        painter->setFont(detailFont);
        painter->setPen(detailColor);
        painter->drawText(
            QRect(textBounds.left(),
                  blockTop + titleMetrics.height() + 1,
                  textBounds.width(),
                  detailMetrics.height()),
            horizontalAlignment | Qt::AlignVCenter,
            detailMetrics.elidedText(detail, Qt::ElideRight, textBounds.width()));
        painter->restore();
    }

private:
    /** @brief 是否绘制六点拖拽手柄；候选列表必须为 false，避免暗示它可排序。 */
    bool m_showDragHandle = false;
    /** @brief 是否将文字相对整张卡片居中；选择器候选项采用左对齐。 */
    bool m_centerContent = false;
};

/**
 * @brief 从主机管理中搜索并选择一个已有跳板机，或转入新建主机流程。
 *
 * 选择器只展示 Host 节点，不把组织分组伪装成可连接目标；当前目标和已经位于链中的主机
 * 由调用方排除，避免用户直到保存阶段才收到“自身引用/重复主机”的迟到错误。
 */
class HostPickerDialog final : public QDialog {
public:
    HostPickerDialog(const QList<HostRecord> &candidates,
                     const QSet<QString> &excludedIds,
                     QWidget *parent = nullptr)
        : QDialog(parent)
        , m_searchEdit(new QLineEdit(this))
        , m_list(new QListWidget(this))
    {
        setObjectName(QStringLiteral("HostPickerDialog"));
        setWindowTitle(tr("选择跳板机"));
        setMinimumSize(460, 470);
        ShelliumUi::applyStandaloneDialogStyle(this);

        auto *body = new QWidget(this);
        body->setObjectName(QStringLiteral("HostPickerBody"));
        auto *subtitle = new QLabel(tr("选择已有主机"), body);
        subtitle->setObjectName(QStringLiteral("HostPickerSubtitle"));

        m_searchEdit->setObjectName(QStringLiteral("hostChainSearchEdit"));
        m_searchEdit->setPlaceholderText(tr("搜索名称、地址或账号"));
        // 搜索图标作为 QLineEdit 的 leading action 存在，不占用独立布局，也不会遮挡输入文本；
        // 使用语义图标而不是 Unicode 放大镜，保证高分屏和不同字体环境下轮廓一致。
        m_searchEdit->addAction(
            UiIcons::symbol(
                this,
                UiIcons::Symbol::Search,
                this->palette().color(QPalette::PlaceholderText)),
            QLineEdit::LeadingPosition);
        m_list->setObjectName(QStringLiteral("hostChainCandidateList"));
        m_list->setSelectionMode(QAbstractItemView::SingleSelection);
        m_list->setItemDelegate(new HostChainRowDelegate(false, false, m_list));
        m_list->setMouseTracking(true);
        m_list->setUniformItemSizes(true);
        m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        for (const HostRecord &record : candidates) {
            if (record.kind != HostNodeKind::Host || excludedIds.contains(record.id)) {
                continue;
            }
            const ConnectionTarget target = record.toConnectionTarget();
            auto *item = new QListWidgetItem(
                QStringLiteral("%1\n%2").arg(record.name, target.connectionSpecifier(true)),
                m_list);
            item->setData(Qt::UserRole, record.id);
            item->setData(Qt::UserRole + 1,
                          QStringLiteral("%1 %2 %3")
                              .arg(record.name, record.host, record.username).toLower());
            item->setSizeHint(QSize(0, 62));
        }

        auto *newButton = new QPushButton(tr("新建主机"), this);
        newButton->setObjectName(QStringLiteral("hostChainCreateHostButton"));
        auto *selectButton = new QPushButton(tr("选择"), this);
        selectButton->setObjectName(QStringLiteral("hostChainSelectHostButton"));
        selectButton->setProperty("primaryAction", true);
        selectButton->setEnabled(false);
        selectButton->setDefault(true);
        UiIcons::decorateButton(
            newButton,
            UiIcons::symbol(
                this,
                UiIcons::Symbol::Plus,
                this->palette().color(QPalette::WindowText)),
            tr("新建一台可复用的跳板主机"));
        UiIcons::decorateButton(
            selectButton,
            UiIcons::symbol(
                this,
                UiIcons::Symbol::Check,
                QColor(QStringLiteral("#0c2f24"))),
            tr("把当前选中的主机加入链路"));

        auto *bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(24, 18, 24, 20);
        bodyLayout->setSpacing(12);
        bodyLayout->addWidget(subtitle);
        bodyLayout->addWidget(m_searchEdit);
        bodyLayout->addWidget(m_list, 1);

        auto *footer = new QWidget(this);
        footer->setObjectName(QStringLiteral("HostPickerFooter"));
        auto *footerLayout = new QHBoxLayout(footer);
        footerLayout->setContentsMargins(24, 13, 24, 13);
        footerLayout->setSpacing(10);
        footerLayout->addWidget(newButton);
        footerLayout->addStretch(1);
        footerLayout->addWidget(selectButton);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(body, 1);
        layout->addWidget(footer);

        connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
            const QString needle = text.trimmed().toLower();
            for (int row = 0; row < m_list->count(); ++row) {
                QListWidgetItem *item = m_list->item(row);
                item->setHidden(!needle.isEmpty()
                                && !item->data(Qt::UserRole + 1).toString().contains(needle));
            }
            // 过滤后若原选中项被隐藏，立即清空当前项并禁用“选择”。保留一个看不见的选中
            // id 会让用户在结果为空时仍能提交旧目标，属于危险且难以察觉的状态错位。
            if (m_list->currentItem() != nullptr && m_list->currentItem()->isHidden()) {
                m_list->setCurrentItem(nullptr);
            }
        });
        connect(m_list, &QListWidget::currentItemChanged, this,
                [selectButton](QListWidgetItem *current) {
                    selectButton->setEnabled(current != nullptr && !current->isHidden());
                });
        connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
            if (m_list->currentItem() != nullptr) {
                accept();
            }
        });
        connect(selectButton, &QPushButton::clicked, this, &QDialog::accept);
        connect(newButton, &QPushButton::clicked, this, [this]() {
            m_createNewRequested = true;
            accept();
        });
    }

    /** @brief 返回当前选中的已有主机 id；选择“新建主机”或无选择时为空。 */
    [[nodiscard]] QString selectedHostId() const
    {
        return m_list->currentItem() == nullptr
            ? QString() : m_list->currentItem()->data(Qt::UserRole).toString();
    }

    /** @brief 用户是否选择了“新建主机”分支。 */
    [[nodiscard]] bool createNewRequested() const { return m_createNewRequested; }

private:
    QLineEdit *m_searchEdit = nullptr;
    QListWidget *m_list = nullptr;
    bool m_createNewRequested = false;
};

/**
 * @brief Termius 风格的受管主机链编辑器。
 *
 * 列表内容是从本机到最终目标之前依次经过的主机。最终目标以固定卡片显示在底部，不进入
 * 可拖动列表，因此用户不可能误把目标拖成跳板或从链路中删除。现有主机只保存稳定 id；
 * 新建主机则通过完整 HostRecordDialog 保存为可复用的普通主机，再自动加入当前草稿。
 */
class HostChainDialog final : public QDialog {
public:
    HostChainDialog(const HostRecord &target,
                    const QStringList &initialChain,
                    ApplicationDatabase *database,
                    CredentialVault *credentialVault,
                    QWidget *parent = nullptr)
        : QDialog(parent)
        , m_target(target)
        , m_database(database)
        , m_credentialVault(credentialVault)
        , m_list(new QListWidget(this))
    {
        Q_ASSERT(m_database != nullptr);
        Q_ASSERT(m_credentialVault != nullptr);
        setObjectName(QStringLiteral("HostChainDialog"));
        setWindowTitle(tr("编辑主机链"));
        setMinimumSize(560, 560);
        ShelliumUi::applyStandaloneDialogStyle(this);

        auto *body = new QWidget(this);
        body->setObjectName(QStringLiteral("HostChainBody"));
        const QString targetDetail = target.toConnectionTarget().connectionSpecifier(true);
        auto *explanation = new QLabel(
            tr("添加的主机将按下列顺序建立连接，最后进入 "
               "<span style=\"font-family: monospace; font-weight: 600;\">%1</span>。")
                .arg(targetDetail.toHtmlEscaped()),
            body);
        explanation->setObjectName(QStringLiteral("HostChainIntroduction"));
        explanation->setTextFormat(Qt::RichText);
        explanation->setWordWrap(true);

        auto *addButton = new QPushButton(tr("添加主机"), body);
        addButton->setObjectName(QStringLiteral("hostChainAddButton"));
        addButton->setProperty("primaryAction", true);
        addButton->setMinimumHeight(42);
        UiIcons::decorateButton(
            addButton,
            UiIcons::symbol(
                this,
                UiIcons::Symbol::Plus,
                QColor(QStringLiteral("#0c2f24"))),
            tr("从主机管理中选择或新建一台跳板主机"));

        m_list->setObjectName(QStringLiteral("hostChainList"));
        m_list->setDragDropMode(QAbstractItemView::InternalMove);
        m_list->setDefaultDropAction(Qt::MoveAction);
        m_list->setSelectionMode(QAbstractItemView::SingleSelection);
        m_list->setItemDelegate(new HostChainRowDelegate(true, true, m_list));
        m_list->setMouseTracking(true);
        m_list->setUniformItemSizes(true);
        m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_list->setDragDropOverwriteMode(false);
        m_list->setDropIndicatorShown(true);
        for (const QString &hostId : initialChain) {
            appendHost(hostId);
        }
        refreshListHeight();

        auto *arrow = new QLabel(QStringLiteral("↓"), body);
        arrow->setObjectName(QStringLiteral("hostChainRouteArrow"));
        arrow->setAlignment(Qt::AlignCenter);
        QFont arrowFont = arrow->font();
        arrowFont = scaledFont(arrowFont, 4.0, false);
        arrow->setFont(arrowFont);

        auto *targetCard = new QFrame(body);
        targetCard->setObjectName(QStringLiteral("hostChainTargetCard"));
        targetCard->setMinimumHeight(82);
        targetCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto *targetLabel = new QLabel(tr("目标主机"), targetCard);
        targetLabel->setObjectName(QStringLiteral("hostChainTargetLabel"));
        targetLabel->setAlignment(Qt::AlignCenter);
        auto *targetDetailLabel = new QLabel(targetDetail, targetCard);
        targetDetailLabel->setObjectName(QStringLiteral("hostChainTargetDetail"));
        targetDetailLabel->setAlignment(Qt::AlignCenter);
        targetDetailLabel->setFont(connectionDetailFont(targetDetailLabel->font()));
        auto *targetLayout = new QVBoxLayout(targetCard);
        targetLayout->setContentsMargins(16, 10, 16, 10);
        targetLayout->setSpacing(2);
        targetLayout->addStretch(1);
        targetLayout->addWidget(targetLabel);
        targetLayout->addWidget(targetDetailLabel);
        targetLayout->addStretch(1);

        auto *removeButton = new QPushButton(tr("从链路移除"), this);
        removeButton->setObjectName(QStringLiteral("hostChainRemoveButton"));
        removeButton->setProperty("hostChainDanger", true);
        removeButton->setEnabled(false);
        auto *cancelButton = new QPushButton(tr("取消"), this);
        cancelButton->setObjectName(QStringLiteral("hostChainCancelButton"));
        auto *saveButton = new QPushButton(tr("保存"), this);
        saveButton->setObjectName(QStringLiteral("hostChainSaveButton"));
        saveButton->setProperty("primaryAction", true);
        saveButton->setDefault(true);
        UiIcons::decorateButton(
            cancelButton,
            UiIcons::symbol(
                this,
                UiIcons::Symbol::Cancel,
                this->palette().color(QPalette::WindowText)),
            tr("放弃当前主机链修改"));
        UiIcons::decorateButton(
            saveButton,
            UiIcons::symbol(
                this,
                UiIcons::Symbol::Check,
                QColor(QStringLiteral("#0c2f24"))),
            tr("保存当前主机链顺序"));

        auto *bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(28, 22, 28, 22);
        bodyLayout->setSpacing(14);
        bodyLayout->addWidget(explanation);
        bodyLayout->addWidget(addButton);
        bodyLayout->addWidget(m_list);
        bodyLayout->addWidget(arrow);
        bodyLayout->addWidget(targetCard);

        auto *footer = new QWidget(this);
        footer->setObjectName(QStringLiteral("HostChainFooter"));
        auto *footerLayout = new QHBoxLayout(footer);
        footerLayout->setContentsMargins(28, 13, 28, 13);
        footerLayout->setSpacing(10);
        footerLayout->addWidget(removeButton);
        footerLayout->addStretch(1);
        footerLayout->addWidget(cancelButton);
        footerLayout->addWidget(saveButton);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(body, 1);
        layout->addWidget(footer);

        connect(m_list, &QListWidget::currentItemChanged, this,
                [removeButton](QListWidgetItem *current) {
                    removeButton->setEnabled(current != nullptr);
                });
        connect(removeButton, &QPushButton::clicked, this, [this]() {
            delete m_list->takeItem(m_list->currentRow());
            refreshListHeight();
        });
        connect(addButton, &QPushButton::clicked, this, &HostChainDialog::addHost);
        connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
        connect(saveButton, &QPushButton::clicked, this, &QDialog::accept);
    }

    /** @brief 返回用户当前排列的跳板机 id，顺序即实际连接顺序。 */
    [[nodiscard]] QStringList chain() const
    {
        QStringList result;
        for (int row = 0; row < m_list->count(); ++row) {
            result.append(m_list->item(row)->data(Qt::UserRole).toString());
        }
        return result;
    }

    /** @brief 是否通过“新建主机”保存过独立记录。 */
    [[nodiscard]] bool databaseChanged() const { return m_databaseChanged; }

private:
    /**
     * @brief 按跳板数量调整列表高度，最多同时展示三行。
     *
     * 空链路仍保留一行高度，维持“这里是可添加跳板的列表”这一视觉位置；超过三跳时改用
     * 列表自身滚动，避免主机链异常长时把目标主机卡片和保存按钮推出窗口可视区域。
     */
    void refreshListHeight()
    {
        constexpr int rowHeight = 68;
        constexpr int frameAllowance = 2;
        const int visibleRows = std::clamp(m_list->count(), 1, 3);
        m_list->setFixedHeight((visibleRows * rowHeight) + frameAllowance);
        m_list->setVerticalScrollBarPolicy(
            m_list->count() > visibleRows ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    }

    /** @brief 把一台已存在主机转换为可拖动链路卡片；缺失引用用醒目占位保留以便用户移除。 */
    void appendHost(const QString &hostId)
    {
        const std::optional<HostRecord> record = m_database->resolveHost(hostId);
        const QString title = record.has_value() ? record->name : tr("失效主机引用");
        const QString detail = record.has_value()
            ? record->toConnectionTarget().connectionSpecifier(true) : hostId;
        auto *item = new QListWidgetItem(
            QStringLiteral("%1\n%2").arg(title, detail), m_list);
        item->setData(Qt::UserRole, hostId);
        item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        item->setSizeHint(QSize(0, 68));
        refreshListHeight();
    }

    /** @brief 打开已有主机选择器；选择新建时保存完整主机并自动加入当前链路。 */
    void addHost()
    {
        bool ok = false;
        QString error;
        const QList<HostRecord> rawCandidates = m_database->searchHosts({}, &ok, &error);
        if (!ok) {
            QMessageBox::critical(this, tr("读取主机失败"), error);
            return;
        }

        QSet<QString> excluded{m_target.id};
        for (const QString &hostId : chain()) {
            excluded.insert(hostId);
        }
        QList<HostRecord> candidates;
        for (const HostRecord &raw : rawCandidates) {
            if (raw.kind != HostNodeKind::Host) {
                continue;
            }
            // 选择器展示的账号和端口必须是实际连接值，而不是“空代表继承”的原始字段；
            // 否则用户可能因为看到错误摘要而选中同名的另一台服务器。
            candidates.append(m_database->resolveHost(raw.id).value_or(raw));
        }
        HostPickerDialog picker(candidates, excluded, this);
        if (picker.exec() != QDialog::Accepted) {
            return;
        }
        if (!picker.createNewRequested()) {
            const QString selectedId = picker.selectedHostId();
            if (!selectedId.isEmpty()) {
                appendHost(selectedId);
            }
            return;
        }

        HostRecord created;
        created.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        // 现场新建默认放在目标所在分组，用户返回主机管理时能在当前上下文立即找到；这只是
        // 组织位置，不会让跳板机继承目标主机本身的任何连接字段。
        created.parentId = m_target.parentId;
        created.kind = HostNodeKind::Host;
        created.port = quint16{22};
        HostRecord inheritedParent;
        if (!created.parentId.isEmpty()) {
            inheritedParent = m_database->resolveHost(created.parentId).value_or(HostRecord{});
        }
        HostRecordDialog dialog(created, inheritedParent, m_database, m_credentialVault, this);
        const int dialogResult = dialog.exec();
        m_databaseChanged = m_databaseChanged || dialog.relatedHostsCreated();
        if (dialogResult != QDialog::Accepted) {
            return;
        }
        created = dialog.record();
        ConnectionTarget target = created.toConnectionTarget();
        target.password = dialog.sessionPassword();
        bool saved = false;
        if (!dialog.sessionPassword().isEmpty()) {
            if (m_credentialVault->passwordProtectionEnabled()
                && !VaultDialog::ensureUnlocked(m_credentialVault, this)) {
                return;
            }
            saved = m_credentialVault->saveConnectionPassword(
                created, target, tr("%1 的登录凭据").arg(created.name), &error);
        } else {
            saved = m_database->saveHost(created, &error);
        }
        if (!saved) {
            QMessageBox::critical(this, tr("新建跳板机失败"), error);
            return;
        }
        m_databaseChanged = true;
        appendHost(created.id);
    }

    HostRecord m_target;
    ApplicationDatabase *m_database = nullptr;
    CredentialVault *m_credentialVault = nullptr;
    QListWidget *m_list = nullptr;
    bool m_databaseChanged = false;
};

void HostRecordDialog::editHostChain()
{
    HostRecord target = record();
    HostChainDialog dialog(target, m_hostChain, m_database, m_credentialVault, this);
    if (dialog.exec() == QDialog::Accepted) {
        m_hostChain = dialog.chain();
        // 受管主机链与自由文本 ProxyJump 都表达路由，同时启用会执行两次跳转。用户确认
        // 非空受管链时清空旧文本；若后来清空链路，仍可重新填写高级 ProxyJump。
        if (!m_hostChain.isEmpty() && m_proxyJumpsEdit != nullptr) {
            m_proxyJumpsEdit->clear();
        }
        refreshHostChainSummary();
    }
    m_relatedHostsCreated = m_relatedHostsCreated || dialog.databaseChanged();
}

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

    m_searchEdit->setObjectName(QStringLiteral("HostSearchEdit"));
    m_searchEdit->setPlaceholderText(tr("搜索名称 / 地址…"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->addAction(
        UiIcons::symbol(this, UiIcons::Symbol::Search, this->palette().color(QPalette::WindowText)),
        QLineEdit::LeadingPosition);

    // QLineEdit 自带动作只支持图标，无法呈现参考设计右侧的“#标签”文字胶囊，因此使用一个
    // QFrame 组合输入框和工具按钮。输入框仍是原来的 m_searchEdit，搜索、清除与测试接口都
    // 不受影响；包装层只负责边框、圆角和右侧快捷入口。
    auto *searchBar = new QFrame(this);
    searchBar->setObjectName(QStringLiteral("HostSearchBar"));
    auto *searchBarLayout = new QHBoxLayout(searchBar);
    searchBarLayout->setContentsMargins(5, 0, 7, 0);
    searchBarLayout->setSpacing(2);
    searchBarLayout->addWidget(m_searchEdit, 1);
    auto *tagFilterButton = new QToolButton(searchBar);
    tagFilterButton->setObjectName(QStringLiteral("HostTagFilterButton"));
    tagFilterButton->setText(tr("#标签"));
    tagFilterButton->setToolTip(tr("按标签精确筛选主机"));
    tagFilterButton->setAccessibleName(tr("按标签精确筛选主机"));
    tagFilterButton->setCursor(Qt::PointingHandCursor);
    searchBarLayout->addWidget(tagFilterButton, 0, Qt::AlignVCenter);

    // 为主机树设置稳定的对象名，供应用级样式表只增强主机选中态。不能直接加深所有
    // QTreeView 的选中色，否则设置页、效率工具等无关列表也会被意外改变视觉层级。
    m_tree->setObjectName(QStringLiteral("HostTree"));
    // QTreeView 会在委托绘制前按平台样式给“当前/选中”行铺底，而且可能连缩进分支区一起
    // 填充。这里在组件自身层级强制透明，确保无论控件是否挂进 MainWindow、运行在哪个平台，
    // 背景都只由 HostItemDelegate 在右侧实际节点矩形内绘制。局部规则只处理背景，不覆盖
    // 应用级 QSS 的字体、行高、颜色或圆角参数。
    m_tree->setStyleSheet(QStringLiteral(R"QSS(
        QTreeView#HostTree {
            selection-background-color: transparent;
        }
        QTreeView#HostTree::item:hover,
        QTreeView#HostTree::item:selected,
        QTreeView#HostTree::item:selected:active,
        QTreeView#HostTree::item:selected:!active,
        QTreeView#HostTree::item:selected:hover {
            background: transparent;
        }
    )QSS"));
    // 某些原生样式绘制缩进区时直接读取 QPalette::Highlight 而不经过 item QSS。同步把该
    // 调色板角色设为透明，作为跨平台后备；右侧选中卡片由委托使用品牌色绘制，不依赖它。
    QPalette hostTreePalette = m_tree->palette();
    hostTreePalette.setColor(QPalette::Active, QPalette::Highlight, Qt::transparent);
    hostTreePalette.setColor(QPalette::Inactive, QPalette::Highlight, Qt::transparent);
    m_tree->setPalette(hostTreePalette);
    // HostManagerWidget 可在不含 MainWindow 的测试和独立容器中使用，因此先提供浅色表面回退；
    // MainWindow::applyModernAppearance() 会按实际应用主题覆盖该属性。不能直接读取
    // QPalette::Base：用户可在深色 macOS 上选择应用浅色主题，两套明暗状态并不保证一致。
    m_tree->setProperty("hostIconSurfaceColor", QStringLiteral("#ffffff"));
    m_tree->setProperty("hostHoverBackgroundColor", QStringLiteral("#e1e7eb"));
    m_tree->setHeaderHidden(true);
    // 展开箭头改由 HostItemDelegate 画在分组行左侧，Qt 自带的分支装饰列必须关掉，否则
    // 会出现两套箭头和不可控的平台间距。32px 层级缩进配合委托绘制的中性连线表达父子
    // 关系；连线不随悬停/选中变色，右侧圆角卡片也不会再带矩形背景“小尾巴”。
    m_tree->setRootIsDecorated(false);
    m_tree->setIndentation(32);
    m_tree->setIconSize(QSize(18, 18));
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
    addHostButton->setObjectName(QStringLiteral("HostAddButton"));
    addHostButton->setProperty("primaryAction", true);
    addHostButton->setProperty("hostPrimaryAction", true);
    // 30px 总高度与两侧 28px 图标按钮形成紧凑动作组，同时为 QSS 的内容区、内边距和边框
    // 留足空间，避免固定高度小于样式最小尺寸时圆角底部被裁剪。
    addHostButton->setMinimumWidth(90);
    addHostButton->setFixedHeight(30);
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
    auto *actionDivider = new QFrame(m_headerActions);
    actionDivider->setObjectName(QStringLiteral("HostHeaderActionDivider"));
    actionDivider->setFrameShape(QFrame::VLine);
    actionDivider->setFixedSize(1, 20);
    headerLayout->addWidget(actionDivider, 0, Qt::AlignVCenter);
    headerLayout->addWidget(addGroupButton);
    headerLayout->addWidget(refreshButton);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    layout->addWidget(searchBar);
    // 不再放"主机与分组"小标题：侧栏顶部已经写着"主机"，树里除了主机和分组也没有别的东西，
    // 这一行只是在挤占本就紧张的纵向空间。
    layout->addWidget(m_tree, 1);

    connect(m_searchEdit, &QLineEdit::textChanged, this, [this]() { applyFilter(); });
    connect(tagFilterButton, &QToolButton::clicked, this, [this]() {
        // “#标签”是搜索语法的可发现入口：若当前不是标签查询就写入前缀，随后把焦点和光标
        // 交还输入框，让用户继续输入标签名；已有标签查询则只聚焦，不破坏用户内容。
        if (!m_searchEdit->text().trimmed().startsWith(QLatin1Char('#'))) {
            m_searchEdit->setText(QStringLiteral("#"));
        }
        m_searchEdit->setFocus(Qt::ShortcutFocusReason);
        m_searchEdit->setCursorPosition(m_searchEdit->text().size());
    });
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
        populateContextMenu(&menu, item);
        menu.exec(m_tree->viewport()->mapToGlobal(position));
    });

    refresh();
}

QWidget *HostManagerWidget::headerActionsWidget() const
{
    return m_headerActions;
}

int HostManagerWidget::hostCount() const
{
    return m_hostCount;
}

void HostManagerWidget::populateContextMenu(QMenu *menu, QTreeWidgetItem *item)
{
    Q_ASSERT(menu != nullptr);
    UiIcons::modernizePopup(menu);

    const bool isHost = item != nullptr
        && static_cast<HostNodeKind>(item->data(0, HostKindRole).toInt())
            == HostNodeKind::Host;
    if (isHost) {
        QAction *connectAction = menu->addAction(
            UiIcons::symbol(this,
                            UiIcons::Symbol::Connect,
                            this->palette().color(QPalette::WindowText)),
            tr("连接"));
        connect(connectAction, &QAction::triggered,
                this, [this, item]() { activateTreeItem(item); });
        menu->addSeparator();
    }

    // “新建主机”是主机管理中的首要创建动作，必须排在“新建分组”之前；这里的顺序与
    // 标题动作区保持一致，避免同一页面的两个入口形成相反的操作习惯。
    QAction *addHostAction = menu->addAction(
        UiIcons::symbol(this, UiIcons::Symbol::Plus, this->palette().color(QPalette::WindowText)),
        tr("新建主机"));
    QAction *addGroupAction = menu->addAction(
        UiIcons::symbol(this,
                        UiIcons::Symbol::FolderAdd,
                        this->palette().color(QPalette::WindowText)),
        tr("新建分组"));
    connect(addHostAction, &QAction::triggered, this, &HostManagerWidget::addHost);
    connect(addGroupAction, &QAction::triggered, this, &HostManagerWidget::addGroup);

    // “未分组”分区标题不对应任何数据库记录，右键它时不提供编辑/删除——那两个动作
    // 只会因为没有选中项而静默无效，不如直接不显示。复制进一步限定为真实主机，避免
    // 用户误以为可以通过复制分组来递归克隆整棵子树。
    const bool isRecordItem =
        item != nullptr && item->data(0, HostKindRole).toInt() != kSectionHeaderKindValue;
    if (isRecordItem) {
        menu->addSeparator();
        if (isHost) {
            QAction *copyAction = menu->addAction(
                UiIcons::symbol(this,
                                UiIcons::Symbol::Copy,
                                this->palette().color(QPalette::WindowText)),
                tr("复制"));
            connect(copyAction,
                    &QAction::triggered,
                    this,
                    &HostManagerWidget::copySelectedHost);
        }
        QAction *editAction = menu->addAction(
            UiIcons::symbol(this,
                            UiIcons::Symbol::Edit,
                            this->palette().color(QPalette::WindowText)),
            tr("编辑"));
        QAction *deleteAction = menu->addAction(
            UiIcons::symbol(this,
                            UiIcons::Symbol::Trash,
                            this->palette().color(QPalette::WindowText)),
            tr("删除"));
        connect(editAction, &QAction::triggered, this, &HostManagerWidget::editSelected);
        connect(deleteAction, &QAction::triggered, this, &HostManagerWidget::removeSelected);
    }

    menu->addSeparator();
    QAction *importAction = menu->addAction(
        UiIcons::symbol(this,
                        UiIcons::Symbol::Upload,
                        this->palette().color(QPalette::WindowText)),
        tr("导入主机…"));
    QAction *exportAction = menu->addAction(
        UiIcons::symbol(this,
                        UiIcons::Symbol::Download,
                        this->palette().color(QPalette::WindowText)),
        tr("导出主机…"));
    connect(importAction, &QAction::triggered, this, &HostManagerWidget::importHosts);
    connect(exportAction, &QAction::triggered, this, &HostManagerWidget::exportHosts);
    menu->addSeparator();
    QAction *refreshAction = menu->addAction(
        UiIcons::symbol(this,
                        UiIcons::Symbol::Refresh,
                        this->palette().color(QPalette::WindowText)),
        tr("刷新"));
    connect(refreshAction, &QAction::triggered, this, &HostManagerWidget::refresh);
}

void HostManagerWidget::refresh()
{
    // QTreeWidget 的选择模型保存的是 QModelIndex，而 applyFilter() 会 clear() 并创建全新节点。
    // 因此必须在重建前提取数据库稳定 id，不能保存 QTreeWidgetItem* 或 QModelIndex。当前控件
    // 是单选模式，优先读取 selectedItems() 能准确对应用户看到的绿色高亮；currentItem() 仅作
    // 键盘当前项的兼容后备，避免平台样式导致 current 与 selected 短暂不同步时丢失选择。
    const QList<QTreeWidgetItem *> selectedItems = m_tree->selectedItems();
    QTreeWidgetItem *selectedItem = selectedItems.isEmpty()
        ? m_tree->currentItem()
        : selectedItems.constFirst();
    const QString selectedRecordId = selectedItem == nullptr
        ? QString{}
        : selectedItem->data(0, HostIdRole).toString();
    const int previousVerticalScrollValue = m_tree->verticalScrollBar()->value();

    applyFilter();

    QTreeWidgetItem *restoredItem = findTreeItemByRecordId(m_tree, selectedRecordId);
    if (restoredItem != nullptr) {
        // 完整树当前默认 expandAll()，这里仍显式展开祖先：若以后改为记忆分组折叠状态，或
        // 从扁平搜索切回层级树，恢复的选中节点也不会藏在折叠分组里而让用户误以为已丢失。
        for (QTreeWidgetItem *ancestor = restoredItem->parent();
             ancestor != nullptr;
             ancestor = ancestor->parent()) {
            ancestor->setExpanded(true);
        }
        m_tree->setCurrentItem(restoredItem);
        restoredItem->setSelected(true);
    }

    // 重建模型后滚动条范围可能变化，先强制完成一次布局，再让 QScrollBar 自动把旧值限制在
    // 新范围内。这样普通刷新和仅修改名称等不改变列表长度的保存操作会停留在原浏览位置；
    // 若记录被删除导致列表变短，也不会产生越界位置。
    m_tree->doItemsLayout();
    m_tree->verticalScrollBar()->setValue(previousVerticalScrollValue);

    // 标题中的数量始终代表数据库全部可连接主机，而不是当前搜索结果。复用导出路径的递归
    // 遍历可以正确覆盖任意深度的嵌套分组，也避免直接访问数据库内部连接或复制 SQL 规则。
    QList<HostRecord> records;
    QString countError;
    if (collectRecords({}, &records, &countError)) {
        const int refreshedCount = static_cast<int>(std::count_if(
            records.cbegin(), records.cend(), [](const HostRecord &record) {
                return record.kind == HostNodeKind::Host;
            }));
        if (m_hostCount != refreshedCount) {
            m_hostCount = refreshedCount;
            emit hostCountChanged(m_hostCount);
        }
    }
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
    HostRecordDialog dialog(record, HostRecord{}, m_database, m_credentialVault, this);
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

    HostRecordDialog dialog(record, inheritedParent, m_database, m_credentialVault, this);
    if (dialog.exec() != QDialog::Accepted) {
        if (dialog.relatedHostsCreated()) {
            refresh();
        }
        return;
    }

    record = dialog.record();
    QString error;
    ConnectionTarget target = record.toConnectionTarget();
    target.password = dialog.sessionPassword();
    bool hostSaved = false;
    if (!dialog.sessionPassword().isEmpty()) {
        if (m_credentialVault->passwordProtectionEnabled()
            && !VaultDialog::ensureUnlocked(m_credentialVault, this)) {
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

    // 用户在“新建主机”对话框中输入的密码已由 CredentialVault 加密落库；立即连接继续
    // 复用输入框中的进程内副本，避免刚保存后再做一次解密。从主机树再次连接时再按凭据
    // 元数据选择主密码保险箱或本机透明密钥。
    // saveConnectionPassword 可能为记录补上新 credentialId，因此持久字段重新从 record
    // 生成；明文仍只取对话框的本次会话副本，不会进入数据库。
    // 新建主机是一次独立的连接意图，必须新建标签承载。如果复用当前标签，已连接主机
    // 的会话会被新主机替换，且用户无法同时保留两个远程会话。
    target = record.toConnectionTarget();
    target.password = dialog.sessionPassword();
    requestConnection(record.id, target, true);
}

void HostManagerWidget::copySelectedHost()
{
    QString error;
    const std::optional<HostRecord> source = selectedRecord(&error);
    if (!source.has_value()) {
        if (!error.isEmpty()) {
            showDatabaseError(tr("读取主机"), error);
        }
        return;
    }
    if (source->kind != HostNodeKind::Host) {
        // 该方法当前只由主机节点的右键动作调用，但仍在业务入口守住类型边界。这样未来若
        // 增加快捷键或自动化入口，也不会意外把分组当作普通记录复制成空连接目标。
        return;
    }

    bool siblingsLoaded = false;
    const QList<HostRecord> siblings =
        m_database->listHostsInGroup(source->parentId, &siblingsLoaded, &error);
    if (!siblingsLoaded) {
        showDatabaseError(tr("读取当前分组"), error);
        return;
    }

    QSet<QString> occupiedNames;
    occupiedNames.reserve(siblings.size());
    for (const HostRecord &sibling : siblings) {
        occupiedNames.insert(sibling.name);
    }

    // 从 1 开始寻找当前分组中第一个尚未占用的副本序号。每次都查询持久化记录而不是保存
    // 进程内计数，应用重启后仍能延续“-2”“-3”，删除某个副本后也能自然复用空缺序号。
    quint64 copyIndex = 1;
    QString copiedName;
    do {
        copiedName = QStringLiteral("%1-%2").arg(source->name).arg(copyIndex);
        ++copyIndex;
    } while (occupiedNames.contains(copiedName));

    HostRecord copied = *source;
    copied.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    copied.name = copiedName;
    // parentId 原样保留正是“当前目录”的持久化语义；连接参数、标签、旧式 ProxyJump 和
    // 受管主机链来自整条记录副本。凭据引用是唯一例外：它会在下方克隆为独立密文，避免
    // 修改副本密码时覆盖原主机。
    // 时间戳属于记录本身而不是主机配置，必须清空后交给数据库生成当前时间，不能让新记录
    // 冒充与原记录同一时刻创建、更新的数据。
    copied.createdAt = QDateTime{};
    copied.updatedAt = QDateTime{};

    const bool copiedSaved = source->credentialId.isEmpty()
        ? m_database->saveHost(copied, &error)
        : m_credentialVault->cloneCredentialForHost(
              copied, source->credentialId, &error);
    if (!copiedSaved) {
        showDatabaseError(tr("复制主机"), error);
        return;
    }
    refresh();
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

    HostRecordDialog dialog(*existing, inheritedParent, m_database, m_credentialVault, this);
    if (dialog.exec() != QDialog::Accepted) {
        if (dialog.relatedHostsCreated()) {
            refresh();
        }
        return;
    }
    HostRecord updated = dialog.record();
    ConnectionTarget editedTarget = updated.toConnectionTarget();
    editedTarget.password = dialog.sessionPassword();
    bool hostSaved = false;
    if (!dialog.sessionPassword().isEmpty()) {
        if (m_credentialVault->passwordProtectionEnabled()
            && !VaultDialog::ensureUnlocked(m_credentialVault, this)) {
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
    QString error;
    const QList<HostRecord> records = m_database->resolveConnectionChain(hostId, &error);
    if (records.isEmpty()) {
        showDatabaseError(tr("解析主机链"),
                          error.isEmpty() ? tr("主机记录或链路不存在") : error);
        return;
    }

    QList<ConnectionTarget> connectionPlan;
    connectionPlan.reserve(records.size());
    for (const HostRecord &record : records) {
        ConnectionTarget target = record.toConnectionTarget();
        // 受管链由 Shellium 的自持会话逐层建立，不能同时再把手动 ProxyJump 交给 libssh，
        // 否则同一跳会执行两次。单层计划仍保留原高级 ProxyJump 兼容行为。
        if (records.size() > 1) {
            target.proxyJumps.clear();
        }
        connectionPlan.append(target);
    }
    if (sessionTarget.isValid()) {
        // 新建主机后的首次连接可能带有未落库密码，只覆盖最终目标；跳板层继续由主窗口按
        // 各自 credentialId 从保险库加载，不能把目标密码错误复用于整条链。
        connectionPlan.last() = sessionTarget;
        if (connectionPlan.size() > 1) {
            connectionPlan.last().proxyJumps.clear();
        }
    }
    emit hostConnectionRequested(hostId, connectionPlan, openInNewTab);
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
    // 字重同时写入模型，保证编辑态、辅助技术和不经过自绘委托的后备路径与最终视觉一致：
    // 分组是组织标题所以加粗，具体主机使用常规字重，避免主机列表整体过重。
    QFont itemNameFont = m_tree->font();
    itemNameFont.setWeight(record.kind == HostNodeKind::Folder ? QFont::Bold : QFont::Normal);
    item->setFont(0, itemNameFont);
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
        const HostRecord displayRecord = resolved.has_value() ? *resolved : record;
        QString detail = displayRecord.toConnectionTarget().connectionSpecifier(true);
        if (!record.hostChain.isEmpty()) {
            const std::optional<HostRecord> firstJump =
                m_database->findHost(record.hostChain.constFirst());
            detail += record.hostChain.size() == 1
                ? tr(" · 经 %1").arg(firstJump.has_value() ? firstJump->name : tr("失效跳板"))
                : tr(" · 经 %1 等 %2 个跳板")
                      .arg(firstJump.has_value() ? firstJump->name : tr("失效跳板"))
                      .arg(record.hostChain.size());
        }
        item->setData(0, HostDetailRole, detail);
        // resolveHost() 会把父分组标签合并进主机；展示这一结果才能让“分组标记为测试环境”
        // 的主机在树中也出现相同徽章，与 #标签 搜索的实际命中语义保持一致。
        item->setData(0, HostTagsRole, displayRecord.tags);
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

    // 根层级可能夹有“未分组”分区标题。该标题只是为了把顶层分组和无父主机在视觉上
    // 隔开，既没有数据库 id，也不应占用 sortOrder；因此排序号必须由真实记录单独累计。
    // 如果直接使用树节点下标，保存过程会在标题处查询空 id 并误报“找不到节点”，同时
    // 标题之后的主机排序号还会留下一个无意义的空洞。
    int persistedSortOrder = 0;
    for (int treeIndex = 0; treeIndex < childCount; ++treeIndex) {
        QTreeWidgetItem *child = parentItem != nullptr
            ? parentItem->child(treeIndex)
            : m_tree->topLevelItem(treeIndex);
        if (child->data(0, HostKindRole).toInt() == kSectionHeaderKindValue) {
            // 展示伪节点绝不能进入 findHost()/saveHost()。显式判断哨兵类型而不是仅判断 id
            // 是否为空，可以继续把真实节点意外丢失 id 当作数据错误暴露出来，避免静默漏存。
            continue;
        }
        const QString childId = child->data(0, HostIdRole).toString();
        std::optional<HostRecord> record = m_database->findHost(childId, errorMessage);
        if (!record.has_value()) {
            if (errorMessage != nullptr && errorMessage->isEmpty()) {
                *errorMessage = tr("找不到节点 %1").arg(childId);
            }
            return false;
        }
        record->parentId = parentId;
        record->sortOrder = persistedSortOrder;
        if (!m_database->saveHost(*record, errorMessage)) {
            return false;
        }
        ++persistedSortOrder;
    }
    return true;
}

void HostManagerWidget::showDatabaseError(const QString &operation, const QString &details) const
{
    QMessageBox::critical(const_cast<HostManagerWidget *>(this),
                          tr("主机数据操作失败"),
                          tr("%1失败：%2").arg(operation, details));
}
