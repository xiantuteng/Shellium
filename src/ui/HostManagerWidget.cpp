#include "ui/HostManagerWidget.h"

#include "core/ConnectionConfigCodec.h"
#include "i18n/Retranslate.h"
#include "security/Vault.h"
#include "ssh/SshSessionStack.h"
#include "storage/ApplicationDatabase.h"
#include "ui/SettingsWidgets.h"
#include "ui/HostDistributionIcon.h"
#include "ui/UiIcons.h"
#include "ui/VaultDialog.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFile>
#include <QFileIconProvider>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
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
#include <tuple>

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
 * @brief 主机节点保存首枚标签的用户自选颜色。
 *
 * 颜色不写进 HostRecord::tags：同一个标签在所有主机上必须是同一种颜色，否则“生产”在
 * 一台机器上是红色、在另一台是绿色，徽章就失去了统一预警的意义。因此颜色按标签名保存
 * 在全局设置里，这里只缓存解析结果供委托直接取用。无效颜色表示沿用强调色。
 */
constexpr int HostTagColorRole = Qt::UserRole + 4;

/**
 * @brief 主机目录展开状态在 settings 表中的稳定键名。
 *
 * 键名带版本后缀，是因为该值包含结构化 JSON；未来若更换结构，可以新增 v2 并保留明确的
 * 迁移边界，而不必猜测旧值究竟采用哪种格式。
 */
const QString kHostTreeExpansionSettingKey =
    QStringLiteral("ui/host_tree_folder_expansion_v1");

/** @brief 当前主机目录展开状态 JSON 结构版本。 */
constexpr int kHostTreeExpansionStateVersion = 1;

/**
 * @brief 标签配色在 settings 表中的稳定键名。
 *
 * 值是 {"version":1,"colors":{"生产":"#ef5b5b"}} 形式的 JSON。配色按标签名而不是按主机
 * 保存：标签的作用是让同一类环境在整棵树里呈现同一种视觉信号，若允许每台主机各自着色，
 * 徽章颜色就只能靠逐条记忆，反而比不着色更容易误连。
 */
const QString kHostTagColorSettingKey = QStringLiteral("ui/host_tag_colors_v1");

/** @brief 当前标签配色 JSON 结构版本。 */
constexpr int kHostTagColorStateVersion = 1;

/**
 * @brief 标签可选配色。
 *
 * 刻意只提供有限的一组，而不是打开系统取色器：标签颜色要在浅色和深色主题、以及很小的
 * 徽章面积上同时保持可读，任意取色很容易产生与背景对比不足的值。这里的顺序即界面顺序，
 * 第一项与全局强调色同族，作为未指定配色时的自然延续。
 */
[[nodiscard]] QList<QColor> tagColorChoices()
{
    return {ShelliumUi::accentColor(),
            QColor(QStringLiteral("#4c8dff")),
            QColor(QStringLiteral("#f2b134")),
            QColor(QStringLiteral("#ef5b5b")),
            QColor(QStringLiteral("#a06bff")),
            QColor(QStringLiteral("#25c6c0")),
            QColor(QStringLiteral("#8a94a6"))};
}

/**
 * @brief 读取全部标签配色。
 * @param database 应用数据库；为空或尚无记录时返回空表。
 * @return 标签名到 "#rrggbb" 的映射；无法解析的历史值会被忽略而不是让整表失效。
 */
[[nodiscard]] QHash<QString, QString> loadTagColors(const ApplicationDatabase *database)
{
    QHash<QString, QString> colors;
    if (database == nullptr) {
        return colors;
    }
    const std::optional<QString> stored = database->setting(kHostTagColorSettingKey);
    if (!stored.has_value()) {
        return colors;
    }
    const QJsonObject root =
        QJsonDocument::fromJson(stored->toUtf8()).object();
    const QJsonObject entries = root.value(QStringLiteral("colors")).toObject();
    for (auto entry = entries.constBegin(); entry != entries.constEnd(); ++entry) {
        const QColor color(entry.value().toString());
        if (color.isValid()) {
            colors.insert(entry.key(), color.name());
        }
    }
    return colors;
}

/**
 * @brief 覆盖写入全部标签配色。
 * @param database 应用数据库；为空时直接返回 false。
 * @param colors 标签名到颜色的完整映射。
 * @return 写入是否成功；失败只影响配色，调用方不应因此放弃标签本身的保存。
 */
bool saveTagColors(ApplicationDatabase *database, const QHash<QString, QString> &colors)
{
    if (database == nullptr) {
        return false;
    }
    QJsonObject entries;
    for (auto entry = colors.constBegin(); entry != colors.constEnd(); ++entry) {
        entries.insert(entry.key(), entry.value());
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), kHostTagColorStateVersion);
    root.insert(QStringLiteral("colors"), entries);
    return database->setSetting(
        kHostTagColorSettingKey,
        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

/**
 * @brief 解析某个标签应当使用的颜色。
 * @param colors loadTagColors() 的结果。
 * @param tag 标签名。
 * @return 用户显式指定的颜色；未指定时回落到强调色，保证徽章永远可见。
 */
[[nodiscard]] QColor resolveTagColor(const QHash<QString, QString> &colors, const QString &tag)
{
    const QColor stored(colors.value(tag));
    return stored.isValid() ? stored : ShelliumUi::accentColor();
}

/**
 * @brief 按标签实际配色生成药丸按钮的样式表，供名称框入口和标签编辑器的历史胶囊共用。
 * @param color 标签颜色。
 * @param highlighted 是否处于"当前正被编辑"的高亮态；只调深底色透明度，不引入描边，
 *        避免边框改变按钮几何尺寸导致流式布局在切换态之间跳动。
 * @return 可直接传给 QToolButton::setStyleSheet() 的样式表。
 */
[[nodiscard]] QString tagPillStyleSheet(const QColor &color, bool highlighted = false)
{
    QColor fill = color;
    fill.setAlphaF(highlighted ? 0.32F : 0.16F);
    const QColor text = color.darker(135);
    return QStringLiteral(
        "QToolButton { border: none; background: rgba(%1,%2,%3,%4); color: %5;"
        " border-radius: 8px; min-height: 24px; max-height: 24px; padding: 0 7px;"
        " font-size: 12px; font-weight: 600; }")
        .arg(fill.red())
        .arg(fill.green())
        .arg(fill.blue())
        .arg(fill.alphaF())
        .arg(text.name());
}

/**
 * @brief 汇总当前库中出现过的全部标签，供配置窗口给出历史候选。
 * @param database 应用数据库；为空时返回空列表。
 * @return 去重并按名称排序的标签名。
 *
 * 已经没有任何主机在用、但仍保留着配色的标签也会列出：用户往往是先删掉最后一台机器，
 * 再新建替代机器并期望沿用同一个标签，若此时历史候选消失，就只能靠回忆重新拼写。
 */
[[nodiscard]] QStringList collectKnownTags(const ApplicationDatabase *database)
{
    if (database == nullptr) {
        return {};
    }
    QSet<QString> unique;
    bool ok = false;
    // 空关键字会命中全部记录；标签同时挂在主机和分组上，两者都要计入历史候选。
    for (const HostRecord &record : database->searchHosts(QString{}, &ok)) {
        for (const QString &tag : record.tags) {
            if (!tag.trimmed().isEmpty()) {
                unique.insert(tag.trimmed());
            }
        }
    }
    const QHash<QString, QString> colors = loadTagColors(database);
    for (auto entry = colors.constBegin(); entry != colors.constEnd(); ++entry) {
        unique.insert(entry.key());
    }
    QStringList tags(unique.constBegin(), unique.constEnd());
    std::sort(tags.begin(), tags.end());
    return tags;
}

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

/** @brief 主机名称与连接副标题相对行左缘的位置，为放大的图标保留稳定间隔。 */
constexpr int kHostTextLeft = 50;

/** @brief 主机图标容器尺寸；30px 在 50px 行高中更醒目，仍保留上下各 10px 呼吸空间。 */
constexpr int kHostIconBoxSize = 30;

/**
 * @brief 图标内容相对圆角容器的内边距。
 *
 * 从原来的 6px 收紧为 5px，让通用服务器和发行版 Logo 的有效图形从约 14px 增至 20px；
 * 仅放大容器而不收紧内边距会让用户几乎看不出图标本身发生变化。
 */
constexpr int kHostIconContentInset = 5;

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

        const QRect hostSymbolRect = iconBox.adjusted(kHostIconContentInset,
                                                      kHostIconContentInset,
                                                      -kHostIconContentInset,
                                                      -kHostIconContentInset);
        // 选中态也必须保留发行版图标。此前高亮时会统一替换成服务器轮廓，导致用户刚想
        // 确认“这台是什么系统”时最关键的视觉线索反而消失；图标自身带有不透明圆底，能
        // 在绿色选中底纹上保持辨识度，无需再强制换色。
        qvariant_cast<QIcon>(index.data(Qt::DecorationRole))
            .paint(painter, hostSymbolRect, Qt::AlignCenter);
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
            // 颜色由用户在标签编辑器里选定；未选择时 HostTagColorRole 为空，回落到强调色，
            // 保证旧数据不会突然画成透明或黑色徽章。
            const QColor storedTagColor =
                qvariant_cast<QColor>(index.data(HostTagColorRole));
            const QColor tagBase =
                storedTagColor.isValid() ? storedTagColor : ShelliumUi::accentColor();
            QColor tagFill = tagBase;
            tagFill.setAlphaF(0.16F);
            QColor tagText = tagBase.darker(135);
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
        // QAbstractItemView 会在基类 dragMoveEvent() 中根据本次鼠标坐标更新
        // dropIndicatorPosition()。必须先让 Qt 完成这一步，再判断目标父节点；如果在基类
        // 之前读取，该值仍属于上一次悬停位置。典型后果是：刚开始在同一分组的两台主机
        // 之间拖动时，旧的 OnItem 状态会把目标主机误当成父节点并拒绝，只有先经过某个
        // 分组、让旧状态变为合法后，同组排序才突然可用。
        QTreeWidget::dragMoveEvent(event);
        if (!event->isAccepted()) {
            // 基类已经因为 MIME 类型、拖放动作或项目 flags 拒绝时，沿用 Qt 的决定；这里
            // 只收紧主机树的业务约束，不把框架判定为非法的拖放重新放行。
            return;
        }

        QTreeWidgetItem *draggedItem = currentItem();
        QTreeWidgetItem *candidateParent = nullptr;
        if (draggedItem == nullptr
            || !computeLegalDrop(event->position().toPoint(), draggedItem, &candidateParent)) {
            event->ignore();
        }
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
 * @brief 极简流式布局：子控件从左到右排列，超出可用宽度后自动换到下一行。
 *
 * 历史标签数量不固定，用普通的 QHBoxLayout 只会把多余标签挤出可视区域或产生横向滚动条；
 * Qt 没有内置的流式布局，这里按官方示例精简实现一份，仅服务于标签胶囊的排列场景。
 */
class FlowLayout final : public QLayout {
public:
    explicit FlowLayout(QWidget *parent, int hSpacing = 8, int vSpacing = 8)
        : QLayout(parent)
        , m_hSpace(hSpacing)
        , m_vSpace(vSpacing)
    {
    }

    ~FlowLayout() override
    {
        QLayoutItem *item = nullptr;
        while ((item = takeAt(0)) != nullptr) {
            delete item;
        }
    }

    void addItem(QLayoutItem *item) override { m_items.append(item); }
    [[nodiscard]] int count() const override { return m_items.size(); }
    [[nodiscard]] QLayoutItem *itemAt(int index) const override { return m_items.value(index); }
    QLayoutItem *takeAt(int index) override
    {
        return (index >= 0 && index < m_items.size()) ? m_items.takeAt(index) : nullptr;
    }
    [[nodiscard]] Qt::Orientations expandingDirections() const override { return {}; }
    [[nodiscard]] bool hasHeightForWidth() const override { return true; }
    [[nodiscard]] int heightForWidth(int width) const override
    {
        return doLayout(QRect(0, 0, width, 0), true);
    }
    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);
        std::ignore = doLayout(rect, false);
    }
    [[nodiscard]] QSize sizeHint() const override { return minimumSize(); }
    [[nodiscard]] QSize minimumSize() const override
    {
        QSize size;
        for (const QLayoutItem *item : m_items) {
            size = size.expandedTo(item->minimumSize());
        }
        const QMargins margins = contentsMargins();
        size += QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
        return size;
    }

private:
    [[nodiscard]] int doLayout(const QRect &rect, bool testOnly) const
    {
        const QMargins margins = contentsMargins();
        const QRect area = rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
        int x = area.x();
        int y = area.y();
        int lineHeight = 0;
        for (QLayoutItem *item : m_items) {
            int nextX = x + item->sizeHint().width() + m_hSpace;
            if (nextX - m_hSpace > area.right() && lineHeight > 0) {
                x = area.x();
                y += lineHeight + m_vSpace;
                nextX = x + item->sizeHint().width() + m_hSpace;
                lineHeight = 0;
            }
            if (!testOnly) {
                item->setGeometry(QRect(QPoint(x, y), item->sizeHint()));
            }
            x = nextX;
            lineHeight = std::max(lineHeight, item->sizeHint().height());
        }
        return y + lineHeight - rect.y() + margins.bottom();
    }

    QList<QLayoutItem *> m_items;
    int m_hSpace;
    int m_vSpace;
};

/**
 * @brief 标签配色选择用的小圆点按钮。
 *
 * 用自绘按钮而不是带背景色样式表的 QPushButton：样式表里的背景色会被全局主题的按钮规则
 * 覆盖，而且无法表达“选中”所需的外圈描边。按钮本身可选中，交由 QButtonGroup 保证互斥。
 */
class TagColorSwatch final : public QAbstractButton {
    Q_DECLARE_TR_FUNCTIONS(TagColorSwatch)
public:
    /**
     * @brief 创建一个展示指定颜色的可选中圆点。
     * @param color 该分段代表的颜色。
     * @param parent Qt 父控件。
     */
    TagColorSwatch(const QColor &color, QWidget *parent = nullptr)
        : QAbstractButton(parent)
        , m_color(color)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        // 选中态要在圆点外面再画一圈描边，固定尺寸必须比圆点本身留出至少一圈的余量，
        // 否则描边会顶到控件边界，抗锯齿处理后看起来像被裁掉了一截、圆环不闭合。
        setFixedSize(32, 32);
        setToolTip(color.name());
        setAccessibleName(tr("Tag color %1").arg(color.name()));
    }

    /** @brief 返回该圆点代表的颜色。 */
    [[nodiscard]] QColor color() const { return m_color; }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event)
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPointF center = QRectF(rect()).center();
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_color);
        painter.drawEllipse(center, 9.0, 9.0);
        if (isChecked()) {
            // 选中态用同色外圈而不是勾号：圆点很小，勾号会盖住正在挑选的颜色本身。
            QPen ring(m_color);
            ring.setWidthF(2.0);
            painter.setPen(ring);
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(center, 13.0, 13.0);
        }
    }

private:
    QColor m_color;
};

/**
 * @brief 主机标签编辑器：新增标签、选择配色并复用历史标签。
 *
 * 一台主机只允许挂一个标签，因此提交新标签会替换掉本机原有的那一个，而不是追加。
 * 标签本身随 HostRecord 保存，配色则按标签名保存在全局设置里，因此本窗口确认时会立即写回
 * 配色，而标签交由调用方在保存主机时一并持久化。这样即使用户随后取消主机配置，已经
 * 挑好的配色也不会丢；反之若把配色也推迟到主机保存，同一标签在不同主机上就会出现分歧。
 */
class HostTagDialog final : public QDialog {
    Q_DECLARE_TR_FUNCTIONS(HostTagDialog)
public:
    /**
     * @brief 创建标签编辑器。
     * @param tags 当前记录已有的标签。
     * @param database 应用数据库，用于读取历史标签与读写配色，不得为空。
     * @param parent Qt 父窗口。
     */
    HostTagDialog(const QStringList &tags, ApplicationDatabase *database, QWidget *parent = nullptr)
        : QDialog(parent)
        , m_database(database)
        , m_colors(loadTagColors(database))
    {
        Q_ASSERT(m_database != nullptr);
        // 一台主机只保留一个标签；旧数据若残留多个标签（改动前允许追加），只取第一个。
        for (const QString &tag : tags) {
            const QString normalized = tag.trimmed();
            if (!normalized.isEmpty()) {
                m_tags = {normalized};
                break;
            }
        }

        // 复用主机配置窗口的对象名，是为了直接套用全局样式表里的卡片、输入框与页脚规则；
        // 内部控件仍使用各自独立的对象名，测试与样式定位不会互相干扰。
        setObjectName(QStringLiteral("ConnectionDialog"));
        setWindowTitle(tr("Tag"));
        setMinimumWidth(420);
        resize(440, 480);

        m_nameEdit = new QLineEdit(this);
        m_nameEdit->setObjectName(QStringLiteral("hostTagNameEdit"));
        m_nameEdit->setPlaceholderText(tr("Enter a tag name, e.g. production"));
        m_nameEdit->setMinimumWidth(200);

        auto *colorRow = new QWidget(this);
        colorRow->setObjectName(QStringLiteral("hostTagColorChooser"));
        // 圆点是自绘的固定尺寸控件：不锁定行高，外层布局在窗口偏矮时会把整行压扁，圆点被
        // 裁掉下半部分，看上去像一排半圆而不是颜色。
        colorRow->setFixedHeight(32);
        auto *colorLayout = new QHBoxLayout(colorRow);
        colorLayout->setContentsMargins(0, 0, 0, 0);
        colorLayout->setSpacing(6);
        m_colorGroup = new QButtonGroup(this);
        m_colorGroup->setExclusive(true);
        const QList<QColor> choices = tagColorChoices();
        for (const QColor &choice : choices) {
            auto *swatch = new TagColorSwatch(choice, colorRow);
            m_colorGroup->addButton(swatch);
            colorLayout->addWidget(swatch);
            m_swatches.append(swatch);
        }
        // 末尾留一个可伸缩空位，否则布局会把多余宽度均摊成圆点之间的空隙，让"一组配色"
        // 散成横跨整行的孤立圆点。
        colorLayout->addStretch(1);
        m_swatches.constFirst()->setChecked(true);

        m_historyFlowWidget = new QWidget(this);
        m_historyFlowWidget->setObjectName(QStringLiteral("hostTagHistoryFlow"));
        new FlowLayout(m_historyFlowWidget);

        // 卡片不设标题：窗口标题已经是"标签"，名称、颜色和历史标签又是同一件事——挑一个
        // 名称和颜色——没有必要在正文里再重复一次这个词。
        auto *editorCard = new SettingsCard(QString(), this);
        editorCard->addWideRow(tr("Name"), m_nameEdit);
        editorCard->addWideRow(tr("Color"), colorRow);
        editorCard->addFullWidthWidget(m_historyFlowWidget);
        editorCard->addNote(tr("Click a tag to load its name and color above for editing or direct confirmation."));

        auto *footer = new QWidget(this);
        footer->setObjectName(QStringLiteral("SettingsFooter"));
        auto *footerLayout = new QHBoxLayout(footer);
        footerLayout->setContentsMargins(20, 12, 20, 12);
        footerLayout->setSpacing(10);
        auto *cancelButton = new QPushButton(tr("Cancel"), footer);
        cancelButton->setObjectName(QStringLiteral("hostTagCancelButton"));
        auto *acceptButton = new QPushButton(tr("OK"), footer);
        acceptButton->setObjectName(QStringLiteral("hostTagAcceptButton"));
        acceptButton->setProperty("primaryAction", true);
        acceptButton->setDefault(true);
        UiIcons::decorateButton(
            cancelButton,
            UiIcons::symbol(
                this, UiIcons::Symbol::Cancel, this->palette().color(QPalette::WindowText)),
            tr("Discard this tab change"));
        UiIcons::decorateButton(
            acceptButton,
            UiIcons::symbol(this, UiIcons::Symbol::Check, QColor(QStringLiteral("#0c2f24"))),
            tr("Confirm tag and color"));
        auto *deleteButton = new QPushButton(tr("Delete tag"), footer);
        deleteButton->setObjectName(QStringLiteral("hostTagDeleteButton"));
        // 只有本机已经挂着标签时，"删除"才有意义；新增场景下留白反而更清楚。
        deleteButton->setEnabled(!m_tags.isEmpty());
        UiIcons::decorateButton(
            deleteButton,
            UiIcons::symbol(this, UiIcons::Symbol::Trash, this->palette().color(QPalette::WindowText)),
            tr("Delete this tag"));
        footerLayout->addWidget(deleteButton);
        footerLayout->addStretch(1);
        footerLayout->addWidget(cancelButton);
        footerLayout->addWidget(acceptButton);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto *body = new QWidget(this);
        auto *bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(22, 20, 22, 18);
        bodyLayout->setSpacing(16);
        bodyLayout->addWidget(editorCard);
        bodyLayout->addStretch(1);
        layout->addWidget(body, 1);
        layout->addWidget(footer);

        // 回车等价于点击"确定"：保存动作统一只读名称框和颜色选择器的当前内容，不需要
        // 单独的中间提交步骤。
        connect(m_nameEdit, &QLineEdit::returnPressed, this, [this]() { accept(); });
        connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
        connect(acceptButton, &QPushButton::clicked, this, &QDialog::accept);
        connect(deleteButton, &QPushButton::clicked, this, [this]() {
            // 复用 accept() 里"名称为空即删除标签"的路径，删除和确认走同一条持久化逻辑。
            m_nameEdit->clear();
            accept();
        });

        m_history = collectKnownTags(m_database);
        if (!m_tags.isEmpty()) {
            loadTagIntoEditor(m_tags.constFirst());
        }
        refreshLists();
    }

    /** @brief 返回确认后的标签列表；一台主机最多一个标签，用于主机树徽章。 */
    [[nodiscard]] QStringList tags() const { return m_tags; }

    /** @brief 返回确认后的全部标签配色，供调用方判断是否需要刷新界面。 */
    [[nodiscard]] QHash<QString, QString> tagColors() const { return m_colors; }

protected:
    /**
     * @brief 确认时把名称框和颜色选择器的当前内容写成本机标签，再落盘配色。
     *
     * 保存永远只认editor 里此刻的文本和颜色，不管它是刚输入的新名称、还是点击历史胶囊
     * 载入的旧名称——两者走的是同一条路径，用户不需要区分"新增"和"复用"两种操作。
     */
    void accept() override
    {
        const QString name = m_nameEdit->text().trimmed();
        if (name.contains(QLatin1Char(','))) {
            // 标签在数据库里以逗号连接存入同一列，含逗号的名称会被拆成两个假标签。
            QMessageBox::warning(this, tr("Invalid tag"), tr("A tag name cannot contain a comma."));
            m_nameEdit->setFocus();
            return;
        }
        if (name.isEmpty()) {
            // 名称框留空表示"这台主机不挂标签"，而不是拒绝确认。
            m_tags.clear();
        } else {
            m_tags = {name};
            m_colors.insert(name, selectedColor().name());
            if (!m_history.contains(name)) {
                m_history.append(name);
                std::sort(m_history.begin(), m_history.end());
            }
        }
        if (!saveTagColors(m_database, m_colors)) {
            // 配色失败不阻断标签本身：标签是筛选与识别的主体，颜色只是附加信号。
            qWarning() << "无法保存标签配色";
        }
        QDialog::accept();
    }

private:
    /** @brief 返回当前选中的配色，未选中时回落到第一种。 */
    [[nodiscard]] QColor selectedColor() const
    {
        for (const TagColorSwatch *swatch : m_swatches) {
            if (swatch->isChecked()) {
                return swatch->color();
            }
        }
        return m_swatches.constFirst()->color();
    }

    /** @brief 把已有标签的名称与配色载入编辑区，便于改色或确认后重新加入。 */
    void loadTagIntoEditor(const QString &tag)
    {
        m_nameEdit->setText(tag);
        const QColor color = resolveTagColor(m_colors, tag);
        for (TagColorSwatch *swatch : m_swatches) {
            if (swatch->color().rgb() == color.rgb()) {
                swatch->setChecked(true);
                return;
            }
        }
    }

    /**
     * @brief 把某个历史标签渲染成一枚"#标签"胶囊按钮，样式与名称框内的标签入口共用同一
     * 套按实际颜色渲染的公式。
     *
     * 点击只把该标签的名称和颜色载入上方编辑区，不直接改动本机标签——是否真的采用，由
     * 用户接下来点"确定"（或按回车）决定，与直接输入新名称走的是同一条确认路径。
     */
    [[nodiscard]] QToolButton *buildTagChip(const QString &tag)
    {
        const bool highlighted = m_nameEdit->text().trimmed() == tag;
        auto *chip = new QToolButton(m_historyFlowWidget);
        chip->setObjectName(QStringLiteral("hostTagChip"));
        chip->setText(QStringLiteral("#%1").arg(tag));
        chip->setStyleSheet(tagPillStyleSheet(resolveTagColor(m_colors, tag), highlighted));
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip(tr("Load name and color"));
        connect(chip, &QToolButton::clicked, this, [this, tag]() {
            loadTagIntoEditor(tag);
            refreshLists();
        });
        return chip;
    }

    /** @brief 按当前历史标签重建胶囊流式布局，并高亮与名称框当前内容一致的那一个。 */
    void refreshLists()
    {
        QLayout *flow = m_historyFlowWidget->layout();
        Q_ASSERT(flow != nullptr);
        QLayoutItem *item = nullptr;
        while ((item = flow->takeAt(0)) != nullptr) {
            delete item->widget();
            delete item;
        }
        for (const QString &tag : m_history) {
            flow->addWidget(buildTagChip(tag));
        }
    }

    ApplicationDatabase *m_database = nullptr;
    QStringList m_tags;
    QStringList m_history;
    QHash<QString, QString> m_colors;
    QLineEdit *m_nameEdit = nullptr;
    QButtonGroup *m_colorGroup = nullptr;
    QList<TagColorSwatch *> m_swatches;
    QWidget *m_historyFlowWidget = nullptr;
};

/**
 * @brief 贴在 QLineEdit 内部右端的文字按钮。
 *
 * QLineEdit::addAction() 只能放图标，而“#标签”这样的入口必须带文字才自解释。这里把按钮
 * 作为输入框的子控件叠在其上，并同步扩大输入框右侧文本边距，保证很长的名称不会画到按钮
 * 底下。输入框宽度本身不变，入口只占用其内部空间。
 */
class InlineFieldButton final : public QToolButton {
public:
    /**
     * @brief 在给定输入框内部右端创建按钮。
     * @param text 按钮文字。
     * @param field 宿主输入框，不得为空；按钮成为它的子控件。
     */
    InlineFieldButton(const QString &text, QLineEdit *field)
        : QToolButton(field)
        , m_field(field)
    {
        Q_ASSERT(m_field != nullptr);
        setText(text);
        setCursor(Qt::ArrowCursor);
        setFocusPolicy(Qt::NoFocus);
        // 外观交给全局样式表按 objectName 匹配（与侧栏 #标签 入口共用同一套药丸样式），这里
        // 不再自带边框/底色，否则会盖住全局规则，让两处入口的样式无法保持一致。
        m_field->installEventFilter(this);
        applyGeometry();
    }

    /** @brief 更新按钮文字并重新预留输入框边距。 */
    void updateText(const QString &text)
    {
        setText(text);
        applyGeometry();
    }

    /**
     * @brief 按标签实际配色渲染入口，与主机树上的标签徽章使用同一套底色/文字公式。
     * @param color 标签在标签编辑器里选定的颜色。
     *
     * 全局样式表里的灰色药丸只适合"未选标签"的占位态；一旦选定了标签，入口必须能让用户
     * 一眼看出这台主机具体挂的是哪种颜色的标签，而不是所有标签看起来都一样。
     */
    void setTagColor(const QColor &color) { setStyleSheet(tagPillStyleSheet(color)); }

    /** @brief 清除按标签配色的局部样式，回落到全局灰色药丸占位态。 */
    void clearTagColor() { setStyleSheet(QString()); }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_field && event->type() == QEvent::Resize) {
            applyGeometry();
        }
        return QToolButton::eventFilter(watched, event);
    }

private:
    /** @brief 把按钮吸附到输入框右端，并同步文本右边距。 */
    void applyGeometry()
    {
        const int width = sizeHint().width();
        const int height = std::min(sizeHint().height(), std::max(1, m_field->height() - 8));
        setGeometry(std::max(0, m_field->width() - width - kFieldInset),
                    (m_field->height() - height) / 2,
                    width,
                    height);
        const QMargins margins = m_field->textMargins();
        m_field->setTextMargins(
            margins.left(), margins.top(), width + kFieldInset, margins.bottom());
    }

    /** @brief 按钮与输入框内边框之间的间距，保证不压住圆角描边。 */
    static constexpr int kFieldInset = 6;

    QLineEdit *m_field = nullptr;
};

/**
 * @brief 编辑一条持久化主机或分组记录的内部对话框。
 *
 * 分组编辑器只暴露名称，避免把组织层级与 SSH 连接参数混在一起；主机编辑器则按“基础信息、
 * 认证与跳板”组织字段。未展示的旧分组配置和主机标签仍保留在 HostRecord 中，编辑名称或其他
 * 可见字段时不会被静默清空。密码不写入 HostRecord，而是随保存动作自动交给 CredentialVault：
 * 开启保险箱密码时使用主密码加密，关闭时使用本机随机密钥透明加密。
 */
class HostRecordDialog final : public QDialog {
    Q_DECLARE_TR_FUNCTIONS(HostRecordDialog)
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
        , m_tags(initial.tags)
        , m_tagColors(loadTagColors(database))
    {
        Q_ASSERT(m_database != nullptr);
        Q_ASSERT(m_credentialVault != nullptr);

        // 字段版式与设置窗口、连接对话框统一：分组卡片 + “左说明右控件”的行 + 滑动开关。
        // 这里先只创建两种记录都需要的名称输入框；主机专属控件放到 isHost 分支中创建，
        // 从对象树层面保证分组窗口除名称外不再残留可聚焦或可被辅助技术发现的字段。
        const bool isHost = initial.kind == HostNodeKind::Host;
        setObjectName(QStringLiteral("ConnectionDialog"));
        setWindowTitle(isHost ? tr("Host configuration") : tr("Group configuration"));
        // 主机窗口的名称行多出分组下拉框，而名称输入框本身必须保持原有宽度，因此窗口下限
        // 相应加宽；分组窗口字段未变，沿用原值。
        setMinimumWidth(isHost ? 728 : 580);
        m_nameEdit->setObjectName(QStringLiteral("hostRecordNameEdit"));
        m_nameEdit->setMinimumWidth(260);

        auto *basicsCard = new SettingsCard(isHost ? tr("Hosts") : tr("Group"), this);
        // SettingsCard 的固定对象名由全局样式表用于绘制白色圆角区域，不能为了测试定位而
        // 覆盖；一旦改名，QFrame#SettingsCard 选择器就会失效，卡片会退化成透明矩形。
        if (!isHost) {
            // 分组窗口保持"只有名称"的极简形态：分组既不需要标签入口，也不能在这里改父级，
            // 否则移动分组会连带整棵子树，与侧栏里的拖放形成两套语义不同的入口。
            basicsCard->addRow(tr("Name"), m_nameEdit);
        }
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
            m_usernameEdit->setPlaceholderText(tr("Leave empty to inherit from the parent group"));
            m_passwordEdit = new QLineEdit(this);
            m_passwordEdit->setObjectName(QStringLiteral("hostPasswordEdit"));
            m_passwordEdit->setEchoMode(QLineEdit::Password);
            m_passwordEdit->setPlaceholderText(tr("Saved automatically with the host after you type it"));
            m_identityEdit = new QLineEdit(initial.identityFile, this);
            m_identityEdit->setObjectName(QStringLiteral("hostIdentityEdit"));
            m_identityEdit->setPlaceholderText(
                tr("Leave empty to inherit from the parent group or use the agent / default key"));
            m_proxyJumpsEdit = new QLineEdit(
                initial.proxyJumps.join(QLatin1Char(',')), this);
            m_proxyJumpsEdit->setObjectName(QStringLiteral("hostProxyJumpsEdit"));
            m_proxyJumpsEdit->setPlaceholderText(tr("Comma-separated; leave empty to inherit from the parent group"));
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
                tr("Edit the jump hosts the current host passes through before the final connection"));
            refreshHostChainSummary();
            m_legacyHmacToggle = new ToggleSwitch(this);
            m_legacyHmacToggle->setObjectName(QStringLiteral("hostLegacyHmacToggle"));
            m_legacyHmacToggle->setChecked(initial.allowLegacyHmacSha1);

            for (QLineEdit *edit : {m_hostEdit, m_usernameEdit, m_passwordEdit,
                                    m_proxyJumpsEdit}) {
                edit->setMinimumWidth(260);
            }

            basicsCard->addRow(tr("Host address"), m_hostEdit);
            basicsCard->addRow(
                tr("Port"), new SettingsStepper(m_portSpin, basicsCard));
            // 名称排在地址与端口之后，是因为它默认由这两项自动生成：先填连接参数、再确认
            // 或改写生成出的名称，符合用户真实的填写顺序，也让自动名称不再"凭空先出现"。
            basicsCard->addRow(tr("Name"), buildNameRow());

            auto *identityContainer = new QWidget(this);
            auto *identityLayout = new QHBoxLayout(identityContainer);
            identityLayout->setContentsMargins(0, 0, 0, 0);
            identityLayout->setSpacing(8);
            auto *browseButton = new QPushButton(tr("Browse…"), identityContainer);
            browseButton->setObjectName(QStringLiteral("hostIdentityBrowseButton"));
            UiIcons::decorateButton(
                browseButton,
                UiIcons::symbol(
                    this,
                    UiIcons::Symbol::Upload,
                    this->palette().color(QPalette::WindowText)),
                tr("Select SSH private key file"));
            m_identityEdit->setMinimumWidth(200);
            identityLayout->addWidget(m_identityEdit, 1);
            identityLayout->addWidget(browseButton);

            // 认证区严格按用户填写路径排列：用户名、密码、私钥和受管主机链保持在主路径中；
            // 自由文本 ProxyJump 收进末尾的高级入口，避免与主机链形成两个并列的首选方案。
            // 保存凭据和旧版算法开关仍保持直接可见，因为它们会明显改变安全行为。
            auto *authCard = new SettingsCard(tr("Authentication and jump hosts"), this);
            authCard->addRow(tr("Username"), m_usernameEdit);
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
            m_passwordVisibilityAction->setText(tr("View password"));
            // 未启用主密码时仍保持眼睛图标可点击。直接禁用虽然阻止了明文显示，却也切断了
            // 用户理解原因和找到设置入口的路径；实际授权仍在 toggled 回调第一层严格拦截。
            m_passwordVisibilityAction->setEnabled(true);
            m_passwordVisibilityAction->setToolTip(
                m_credentialVault->passwordProtectionEnabled()
                    ? tr("View the saved password; when the vault is locked you will be asked for the master password first")
                    : tr("Click to see how to enable master password protection"));
            authCard->addRow(
                tr("Password"),
                m_passwordEdit,
                tr("The password is saved automatically; enable master password protection to unlock and view the plaintext."));
            authCard->addRow(tr("Private key file"), identityContainer);
            authCard->addRow(tr("Host chain"),
                             m_hostChainButton,
                             tr("Choose an existing host in Host Manager as a jump host, or add a host in the chain editor."));
            authCard->addRow(tr("Legacy compatibility"),
                             m_legacyHmacToggle,
                             tr("Allow hmac-sha1 for this host only (not inherited)."));

            m_advancedSettingsButton = new QToolButton(authCard);
            m_advancedSettingsButton->setObjectName(
                QStringLiteral("hostAdvancedSettingsButton"));
            m_advancedSettingsButton->setProperty("advancedDisclosure", true);
            m_advancedSettingsButton->setCheckable(true);
            m_advancedSettingsButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            m_advancedSettingsButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            m_advancedSettingsButton->setAccessibleName(tr("Expand ProxyJump advanced settings"));
            // 已有主机若保存过手工 ProxyJump，打开编辑器时必须直接展示真实生效值，不能因
            // “弱化入口”而把旧路由悄悄藏起来；全新配置和单纯继承父分组时则默认折叠。
            m_advancedSettingsButton->setChecked(!initial.proxyJumps.isEmpty());
            authCard->addFullWidthWidget(m_advancedSettingsButton);
            authCard->addRow(
                tr("ProxyJump"),
                m_proxyJumpsEdit,
                tr("OpenSSH-compatible advanced manual jump host configuration; normally prefer the host chain above."));
            m_proxyJumpRow = m_proxyJumpsEdit->parentWidget();
            m_proxyJumpRow->setObjectName(QStringLiteral("hostProxyJumpRow"));
            refreshProxyJumpDisclosure();

            connect(browseButton, &QPushButton::clicked, this, [this]() {
                const QString path = QFileDialog::getOpenFileName(
                    this, tr("Select SSH private key file"));
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
                                    tr("Master password protection required"),
                                    tr("Viewing host passwords requires master password protection. Go to 'Settings → Connections & Security → Credential Vault' and turn on 'Protect credentials with a master password'."));
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
                            visible ? tr("Hide password") : tr("View password"));
                        m_passwordVisibilityAction->setToolTip(
                            visible ? tr("Hide password")
                                    : tr("View the saved password; when the vault is locked you will be asked for the master password first"));
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
                m_passwordEdit->setPlaceholderText(tr("The vault has been reset; please re-enter your credentials"));
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
        auto *cancelButton = new QPushButton(tr("Cancel"), footer);
        cancelButton->setObjectName(QStringLiteral("hostRecordCancelButton"));
        auto *saveButton = new QPushButton(tr("Save"), footer);
        saveButton->setObjectName(QStringLiteral("hostRecordSaveButton"));
        saveButton->setProperty("primaryAction", true);
        saveButton->setDefault(true);
        UiIcons::decorateButton(
            cancelButton,
            UiIcons::symbol(
                this, UiIcons::Symbol::Cancel, this->palette().color(QPalette::WindowText)),
            tr("Discard this configuration change"));
        UiIcons::decorateButton(
            saveButton,
            // 保存是绿色主操作，固定深色图标才能同时兼顾浅色与深色主题下的对比度。
            UiIcons::symbol(
                this, UiIcons::Symbol::Check, QColor(QStringLiteral("#0c2f24"))),
            tr("Save the current configuration"));
        if (isHost) {
            m_testConnectionButton = new QPushButton(tr("Test connection"), footer);
            m_testConnectionButton->setObjectName(
                QStringLiteral("hostTestConnectionButton"));
            UiIcons::decorateButton(
                m_testConnectionButton,
                UiIcons::symbol(
                    this,
                    UiIcons::Symbol::Connect,
                    this->palette().color(QPalette::WindowText)),
                tr("Test the SSH connection using the current unsaved configuration"));
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
                QMessageBox::warning(this, tr("The configuration is incomplete"), tr("Enter a name."));
                m_nameEdit->setFocus();
                return;
            }
            if (isHost && m_hostEdit->text().trimmed().isEmpty()) {
                QMessageBox::warning(this, tr("The configuration is incomplete"), tr("Enter a host name or IP address."));
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
            // 端口不再提供继承开关，保存时始终记录输入框中的明确值。
            result.tags = m_tags;
            // 分组下拉框的数据项就是目标 parentId；空字符串代表根层级的"未分组"。
            result.parentId = m_groupCombo->currentData().toString();
            result.host = m_hostEdit->text().trimmed();
            result.port = static_cast<quint16>(m_portSpin->value());
            result.username = m_usernameEdit->text().trimmed();
            result.identityFile = m_identityEdit->text().trimmed();
            result.proxyJumps = splitList(m_proxyJumpsEdit->text());
            result.hostChain = m_hostChain;
            result.allowLegacyHmacSha1 = m_legacyHmacToggle->isChecked();
            if (result.host != m_initial.host || result.port != m_initial.port
                || result.username != m_initial.username) {
                // 地址、端口或账号一改，这条记录指向的很可能已经是另一台服务器，缓存的
                // 发行版随即失效。清空而不是保留，既让侧栏立刻回到通用图标，也让下一次
                // 连接重新触发“首次登录探测”，避免图标长期停留在旧系统上误导用户。
                result.distributionId.clear();
                result.distributionVersionId.clear();
            }
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
                m_passwordEdit->setPlaceholderText(tr("Saved; view it after unlocking the vault"));
                return false;
            }
        }
        ConnectionTarget target = credentialSource.toConnectionTarget();
        if (!m_credentialVault->applyHostCredential(
                credentialSource, target, &credentialError)) {
            if (allowUnlock) {
                QMessageBox::warning(
                    this,
                    tr("Could not read the saved password"),
                    credentialError.isEmpty() ? tr("The credential does not exist or is corrupt.")
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

    /**
     * @brief 组装"分组下拉框 + 名称输入框（内嵌 #标签 入口）"的名称行。
     * @return 可直接交给 SettingsCard::addRow() 的容器控件。
     *
     * 三个控件放在同一行，是因为它们回答的是同一个问题——这台主机在列表里叫什么、归属
     * 哪里、属于哪类环境。名称输入框保持原有最小宽度，分组框只在其左侧额外占位，标签入口
     * 则叠在名称框内部，不再挤占名称本身的可读宽度。
     */
    [[nodiscard]] QWidget *buildNameRow()
    {
        m_groupCombo = new QComboBox(this);
        UiIcons::modernizePopup(m_groupCombo);
        m_groupCombo->setObjectName(QStringLiteral("hostRecordGroupCombo"));
        m_groupCombo->setMinimumWidth(150);
        m_groupCombo->setMaximumWidth(200);
        m_groupCombo->setToolTip(tr("Choose which group this host belongs to in Host Manager"));
        m_groupCombo->setAccessibleName(tr("Group"));
        populateGroupChoices();

        m_tagButton = new InlineFieldButton(tr("#tag"), m_nameEdit);
        m_tagButton->setObjectName(QStringLiteral("hostRecordTagButton"));
        m_tagButton->setToolTip(tr("Add tags to this host and choose a tag color"));
        m_tagButton->setAccessibleName(tr("Edit tag"));
        refreshTagButton();

        auto *nameRow = new QWidget(this);
        auto *nameLayout = new QHBoxLayout(nameRow);
        nameLayout->setContentsMargins(0, 0, 0, 0);
        nameLayout->setSpacing(8);
        nameLayout->addWidget(m_groupCombo);
        nameLayout->addWidget(m_nameEdit);

        connect(m_tagButton, &QToolButton::clicked, this, &HostRecordDialog::editTags);
        connect(m_groupCombo,
                &QComboBox::currentIndexChanged,
                this,
                [this]() { applySelectedGroup(); });
        return nameRow;
    }

    /**
     * @brief 用库中全部分组填充下拉框，并选中当前记录的父分组。
     *
     * 条目顺序必须与主界面侧边栏一致，因此不按字母排序，而是复用侧边栏同款的
     * listHostsInGroup(parentId) 逐层遍历（按 sort_order、name 排列子节点），只是深度优先
     * 展开时把子孙分组紧跟在父分组之后。条目文本采用"父分组 / 子分组"的完整路径，因为同名
     * 子分组在不同父级下完全合法，只显示末级名称会让用户无法区分。递归带深度/环兜底：数据
     * 损坏产生的父子环不应让窗口卡死。
     */
    void populateGroupChoices()
    {
        const QSignalBlocker blocker(m_groupCombo);
        m_groupCombo->clear();
        m_groupCombo->addItem(tr("Ungrouped"), QString{});

        QList<QPair<QString, QString>> entries;
        QSet<QString> visited;
        collectGroupChoices(QString{}, QString{}, visited, entries);
        for (const auto &entry : entries) {
            m_groupCombo->addItem(entry.first, entry.second);
        }

        const int currentIndex = m_groupCombo->findData(m_initial.parentId);
        // 父分组可能已被删除；此时回落到"未分组"，保存后记录也就真正回到根层级。
        m_groupCombo->setCurrentIndex(currentIndex >= 0 ? currentIndex : 0);
    }

    /**
     * @brief 深度优先收集某一父分组下的所有子分组，顺序与侧边栏树一致。
     *
     * @param parentId 要展开的父分组 id；空串代表根层级。
     * @param parentPath 到 parentId 为止的显示路径（不含 parentId 自身），用于拼出完整路径。
     * @param visited 已访问过的分组 id，防止数据损坏形成的父子环导致无限递归。
     */
    void collectGroupChoices(const QString &parentId,
                              const QString &parentPath,
                              QSet<QString> &visited,
                              QList<QPair<QString, QString>> &entries)
    {
        bool ok = false;
        for (const HostRecord &record : m_database->listHostsInGroup(parentId, &ok)) {
            if (record.kind != HostNodeKind::Folder || visited.contains(record.id)) {
                continue;
            }
            visited.insert(record.id);
            const QString path = parentPath.isEmpty()
                ? record.name
                : parentPath + QStringLiteral(" / ") + record.name;
            entries.append({path, record.id});
            collectGroupChoices(record.id, path, visited, entries);
        }
    }

    /**
     * @brief 切换分组后重新解析继承值。
     *
     * 用户名、私钥、凭据和 ProxyJump 都按父分组继承，换组等于换掉这台主机的默认值来源。
     * 若不同步，"测试连接"和自动名称会继续使用旧分组的账号，得到与保存后完全不同的结果。
     */
    void applySelectedGroup()
    {
        const QString parentId = m_groupCombo->currentData().toString();
        if (parentId.isEmpty()) {
            m_inheritedParent = HostRecord{};
        } else {
            m_inheritedParent = m_database->resolveHost(parentId).value_or(HostRecord{});
        }
        refreshAutomaticName();
        refreshProxyJumpDisclosure();
    }

    /** @brief 打开标签编辑器，并把确认后的标签写回当前草稿。 */
    void editTags()
    {
        HostTagDialog dialog(m_tags, m_database, this);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        m_tags = dialog.tags();
        m_tagColors = dialog.tagColors();
        refreshTagButton();
    }

    /**
     * @brief 让名称框内的入口显示当前标签及其配色，而不是永远只写"#标签"。
     *
     * 标签一旦保存就只在主机树的小徽章里出现；如果配置窗口不回显文字与颜色，用户每次都得
     * 点开弹窗才能确认这台机器到底带没带"生产"标记、以及它是不是真的还是那个颜色。
     */
    void refreshTagButton()
    {
        if (m_tagButton == nullptr) {
            return;
        }
        if (m_tags.isEmpty()) {
            m_tagButton->updateText(tr("#tag"));
            m_tagButton->clearTagColor();
        } else {
            m_tagButton->updateText(QStringLiteral("#%1").arg(m_tags.constFirst()));
            m_tagButton->setTagColor(resolveTagColor(m_tagColors, m_tags.constFirst()));
        }
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
        QString summary = tr("Advanced settings");
        if (hasExplicitProxyJump) {
            summary = tr("Advanced settings · ProxyJump configured");
        } else if (inheritsProxyJump) {
            summary = tr("Advanced settings · inherits ProxyJump");
        }

        m_advancedSettingsButton->setText(summary);
        m_advancedSettingsButton->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        m_advancedSettingsButton->setAccessibleName(
            expanded ? tr("Collapse ProxyJump advanced settings") : tr("Expand ProxyJump advanced settings"));
        m_advancedSettingsButton->setToolTip(
            tr("Use only for OpenSSH-compatible configuration or when a jump host address must be specified manually"));
        m_proxyJumpRow->setVisible(expanded);
    }

    /** @brief 根据当前链路数量刷新主机配置页入口摘要，避免用户必须再次打开才能确认状态。 */
    void refreshHostChainSummary()
    {
        if (m_hostChainButton == nullptr) {
            return;
        }
        if (m_hostChain.isEmpty()) {
            m_hostChainButton->setText(tr("Host chain  ·  direct  ›"));
        } else if (m_hostChain.size() == 1) {
            const std::optional<HostRecord> jump = m_database->findHost(m_hostChain.constFirst());
            m_hostChainButton->setText(
                tr("Host chain  ·  via %1  ›")
                    .arg(jump.has_value() ? jump->name : tr("Invalid host")));
        } else {
            m_hostChainButton->setText(tr("Host chain  ·  %1 jump hosts  ›").arg(m_hostChain.size()));
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
                    this, tr("The connection details are incomplete"), tr("Enter a host name or IP address."));
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
                    tr("Could not test the host chain"),
                    routeError.isEmpty() ? tr("The jump host does not exist or the chain is invalid: %1").arg(jumpId)
                                         : routeError);
                return;
            }
            for (const HostRecord &hop : expanded) {
                if (routeIds.contains(hop.id) || hop.id == effective.id) {
                    QMessageBox::warning(
                        this, tr("Could not test the host chain"), tr("The host chain contains duplicate nodes or a loop: %1").arg(hop.name));
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
                    QMessageBox::warning(this, tr("Could not test the connection"), credentialError);
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
                        tr("Could not test the connection"),
                        credentialError.isEmpty()
                            ? tr("Could not read the saved credentials for '%1'.").arg(hop.name)
                            : credentialError);
                    return;
                }
            }
            connectionPlan.append(hopTarget);
        }

        m_testInProgress = true;
        m_testConnectionButton->setEnabled(false);
        m_testConnectionButton->setText(tr("Testing…"));

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
                        tr("Successfully connected to %1.").arg(testSpecifier));
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
                        tr("The server only supports legacy hmac-sha1. If you have accepted the risk, turn on 'Legacy compatibility' and retry.\n\n%1")
                            .arg(message));
                });
        connect(session,
                &SshSessionStack::layerCredentialRequired,
                this,
                [this](const ConnectionTarget &, const bool retry) {
                    finishConnectionTest(
                        false,
                        retry ? tr("SSH authentication failed. Check the password or private key passphrase and retry.")
                              : tr("The server requires a password or private key passphrase. Fill in 'Password' and retry."));
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
                        tr("Confirm the server host key"),
                        tr("Connecting to %1 for the first time.\n\nSHA-256 fingerprint:\n%2\n\nVerify the fingerprint through a trusted channel. Trust it and continue testing?")
                            .arg(verificationTarget.connectionSpecifier(true), fingerprint),
                        QMessageBox::Yes | QMessageBox::No,
                        QMessageBox::No);
                    if (m_testSession == session) {
                        session->confirmUnknownHost(answer == QMessageBox::Yes);
                    }
                });
        connect(session,
                &SshSessionStack::hostKeyChangeVerificationRequired,
                this,
                [this, session](const ConnectionTarget &verificationTarget,
                                const QString &fingerprint) {
                    // 测试连接也必须使用与正式终端完全相同的变更核验流程；否则测试按钮会
                    // 永久报错，或者更危险地在用户不知情时绕过旧指纹保护。
                    const QMessageBox::StandardButton answer = QMessageBox::warning(
                        this,
                        tr("The server host key has changed"),
                        tr("The host key returned by %1 does not match the local record.\n\nNew SHA-256 fingerprint:\n%2\n\nThis may be a server reinstall or key rotation, or a man-in-the-middle attack. Verify the new fingerprint through a trusted channel first.\n\nConfirm the key has changed, replace the old record in known_hosts, and continue testing?")
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
        m_testConnectionButton->setText(tr("Test connection"));
        if (m_testSession != nullptr) {
            m_testSession->requestStop();
        }

        if (succeeded) {
            QMessageBox::information(this, tr("Connection test succeeded"), message);
        } else {
            QMessageBox::warning(
                this,
                tr("Connection test failed"),
                message.isEmpty() ? tr("The SSH connection could not be established. Check the current configuration.") : message);
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
    /** @brief 当前草稿的标签；仅在主机窗口中由标签编辑器修改，分组窗口保持原值。 */
    QStringList m_tags;
    /** @brief 标签名到配色的映射，用于按标签实际颜色渲染名称框内的入口。 */
    QHash<QString, QString> m_tagColors;
    /** @brief 名称框内部右端的标签入口，回显当前首枚标签及其配色。 */
    InlineFieldButton *m_tagButton = nullptr;
    /** @brief 名称框左侧的所属分组下拉框；数据项保存目标 parentId。 */
    QComboBox *m_groupCombo = nullptr;
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
    Q_DECLARE_TR_FUNCTIONS(HostPickerDialog)
public:
    HostPickerDialog(const QList<HostRecord> &candidates,
                     const QSet<QString> &excludedIds,
                     QWidget *parent = nullptr)
        : QDialog(parent)
        , m_searchEdit(new QLineEdit(this))
        , m_list(new QListWidget(this))
    {
        setObjectName(QStringLiteral("HostPickerDialog"));
        setWindowTitle(tr("Select jump host"));
        setMinimumSize(460, 470);
        ShelliumUi::applyStandaloneDialogStyle(this);

        auto *body = new QWidget(this);
        body->setObjectName(QStringLiteral("HostPickerBody"));
        auto *subtitle = new QLabel(tr("Select an existing host"), body);
        subtitle->setObjectName(QStringLiteral("HostPickerSubtitle"));

        m_searchEdit->setObjectName(QStringLiteral("hostChainSearchEdit"));
        m_searchEdit->setPlaceholderText(tr("Search name, address, or account"));
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

        auto *newButton = new QPushButton(tr("New host"), this);
        newButton->setObjectName(QStringLiteral("hostChainCreateHostButton"));
        auto *selectButton = new QPushButton(tr("Select"), this);
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
            tr("Create a reusable jump host"));
        UiIcons::decorateButton(
            selectButton,
            UiIcons::symbol(
                this,
                UiIcons::Symbol::Check,
                QColor(QStringLiteral("#0c2f24"))),
            tr("Add the selected host to the chain"));

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
    Q_DECLARE_TR_FUNCTIONS(HostChainDialog)
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
        setWindowTitle(tr("Edit host chain"));
        setMinimumSize(560, 560);
        ShelliumUi::applyStandaloneDialogStyle(this);

        auto *body = new QWidget(this);
        body->setObjectName(QStringLiteral("HostChainBody"));
        const QString targetDetail = target.toConnectionTarget().connectionSpecifier(true);
        auto *explanation = new QLabel(
            tr("The hosts you add connect in the following order, finally reaching <span style=\"font-family: monospace; font-weight: 600;\">%1</span>.")
                .arg(targetDetail.toHtmlEscaped()),
            body);
        explanation->setObjectName(QStringLiteral("HostChainIntroduction"));
        explanation->setTextFormat(Qt::RichText);
        explanation->setWordWrap(true);

        auto *addButton = new QPushButton(tr("Add host"), body);
        addButton->setObjectName(QStringLiteral("hostChainAddButton"));
        addButton->setProperty("primaryAction", true);
        addButton->setMinimumHeight(42);
        UiIcons::decorateButton(
            addButton,
            UiIcons::symbol(
                this,
                UiIcons::Symbol::Plus,
                QColor(QStringLiteral("#0c2f24"))),
            tr("Choose or create a jump host in Host Manager"));

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
        auto *targetLabel = new QLabel(tr("Target host"), targetCard);
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

        auto *removeButton = new QPushButton(tr("Remove from chain"), this);
        removeButton->setObjectName(QStringLiteral("hostChainRemoveButton"));
        removeButton->setProperty("hostChainDanger", true);
        removeButton->setEnabled(false);
        auto *cancelButton = new QPushButton(tr("Cancel"), this);
        cancelButton->setObjectName(QStringLiteral("hostChainCancelButton"));
        auto *saveButton = new QPushButton(tr("Save"), this);
        saveButton->setObjectName(QStringLiteral("hostChainSaveButton"));
        saveButton->setProperty("primaryAction", true);
        saveButton->setDefault(true);
        UiIcons::decorateButton(
            cancelButton,
            UiIcons::symbol(
                this,
                UiIcons::Symbol::Cancel,
                this->palette().color(QPalette::WindowText)),
            tr("Discard changes to the current host chain"));
        UiIcons::decorateButton(
            saveButton,
            UiIcons::symbol(
                this,
                UiIcons::Symbol::Check,
                QColor(QStringLiteral("#0c2f24"))),
            tr("Save the current host chain order"));

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
        const QString title = record.has_value() ? record->name : tr("Invalid host reference");
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
            QMessageBox::critical(this, tr("Failed to read host"), error);
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
                created, target, tr("Login credentials for %1").arg(created.name), &error);
        } else {
            saved = m_database->saveHost(created, &error);
        }
        if (!saved) {
            QMessageBox::critical(this, tr("Failed to create jump host"), error);
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
    m_searchEdit->setPlaceholderText(tr("Search name / address…"));
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
    tagFilterButton->setText(tr("#tag"));
    tagFilterButton->setToolTip(tr("Filter hosts by exact tag"));
    tagFilterButton->setAccessibleName(tr("Filter hosts by exact tag"));
    tagFilterButton->setCursor(Qt::PointingHandCursor);
    searchBarLayout->addWidget(tagFilterButton, 0, Qt::AlignVCenter);
    m_searchBar = searchBar;

    // 建议面板用 Qt::Tool + WA_ShowWithoutActivating：既不抢主窗口的键盘焦点（用户在弹出面板
    // 悬浮期间仍能继续在搜索框里敲字过滤标签），又不像子控件那样被主机树裁剪，可以画到列表
    // 上方。是否收起由下面安装的全局事件过滤器（点击面板外）和 Esc 按键统一处理。
    m_tagSuggestionsPopup = new QFrame(this);
    m_tagSuggestionsPopup->setObjectName(QStringLiteral("HostTagSuggestionsPopup"));
    m_tagSuggestionsPopup->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint
                                          | Qt::NoDropShadowWindowHint);
    m_tagSuggestionsPopup->setAttribute(Qt::WA_ShowWithoutActivating);
    auto *popupLayout = new QVBoxLayout(m_tagSuggestionsPopup);
    popupLayout->setContentsMargins(10, 10, 10, 10);
    m_tagSuggestionsFlow = new QWidget(m_tagSuggestionsPopup);
    new FlowLayout(m_tagSuggestionsFlow, 8, 8);
    popupLayout->addWidget(m_tagSuggestionsFlow);
    m_tagSuggestionsPopup->hide();
    qApp->installEventFilter(this);

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

    // 监听 QTreeWidget 的最终状态信号，而不是只监听上方自绘箭头。这样鼠标双击、键盘左右键、
    // 辅助功能以及将来新增的“全部展开/折叠”入口都会自动保存，且每一次有效状态变化只写一次。
    connect(m_tree,
            &QTreeWidget::itemExpanded,
            this,
            [this](QTreeWidgetItem *item) { persistFolderExpansionState(item, true); });
    connect(m_tree,
            &QTreeWidget::itemCollapsed,
            this,
            [this](QTreeWidgetItem *item) { persistFolderExpansionState(item, false); });

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
    addGroupButton->setText(tr("New group"));
    auto *addHostButton = new QPushButton(tr("New host"), m_headerActions);
    addHostButton->setObjectName(QStringLiteral("HostAddButton"));
    addHostButton->setProperty("primaryAction", true);
    addHostButton->setProperty("hostPrimaryAction", true);
    // 30px 总高度与两侧 28px 图标按钮形成紧凑动作组，同时为 QSS 的内容区、内边距和边框
    // 留足空间，避免固定高度小于样式最小尺寸时圆角底部被裁剪。
    addHostButton->setMinimumWidth(90);
    addHostButton->setFixedHeight(30);
    auto *refreshButton = new QToolButton(m_headerActions);
    refreshButton->setText(tr("Refresh"));
    UiIcons::decorateButton(
        addGroupButton,
        UiIcons::symbol(this, UiIcons::Symbol::FolderAdd, this->palette().color(QPalette::WindowText)),
        tr("Create a new host group at the current level"));
    UiIcons::decorateButton(
        addHostButton,
        // 主操作背景在浅色和深色主题下都固定为高亮绿色，图标也必须固定使用深色；若让它
        // 跟随深色主题的浅色 WindowText，绿色按钮上的“+”反而会降低对比度。
        UiIcons::symbol(this,
                        UiIcons::Symbol::Plus,
                        QColor(QStringLiteral("#0c2f24"))),
        tr("Create a new SSH host in the current group"));
    UiIcons::decorateButton(
        refreshButton,
        UiIcons::symbol(this, UiIcons::Symbol::Refresh, this->palette().color(QPalette::WindowText)),
        tr("Reload hosts and groups"));
    configureSidebarIconButton(addGroupButton, tr("Create a new host group at the current level"));
    configureSidebarIconButton(refreshButton, tr("Reload hosts and groups"));

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

    connect(m_searchEdit, &QLineEdit::textChanged, this, [this]() {
        applyFilter();
        updateTagSuggestions();
    });
    connect(tagFilterButton, &QToolButton::clicked, this, [this]() {
        // “#标签”是搜索语法的可发现入口：若当前不是标签查询就写入前缀，随后把焦点和光标
        // 交还输入框，让用户继续输入标签名；已有标签查询则只聚焦，不破坏用户内容。
        if (!m_searchEdit->text().trimmed().startsWith(QLatin1Char('#'))) {
            m_searchEdit->setText(QStringLiteral("#"));
        }
        m_searchEdit->setFocus(Qt::ShortcutFocusReason);
        m_searchEdit->setCursorPosition(m_searchEdit->text().size());
        // 文本未变化时上面的 setText() 不会触发 textChanged，这里显式补一次，保证已处于
        // "#xxx" 查询状态时再次点击入口也能唤出建议面板，而不是必须先清空重打。
        updateTagSuggestions();
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

    // 必须先加载持久化快照再首次刷新；否则首屏会按默认值构建，随后再恢复时产生可见跳变。
    loadFolderExpansionState();

    // 常驻侧栏：语言切换时就地重设标题行文案并重建主机树（"未分组"分区标题等在 refresh()
    // 里产生）。闭包首次执行等价于原先构造末尾的初始化 + 首屏刷新。
    i18n::onRetranslate(this, [this, addHostButton, addGroupButton, refreshButton, tagFilterButton] {
        m_searchEdit->setPlaceholderText(tr("Search name / address…"));
        addHostButton->setText(tr("New host"));
        addGroupButton->setText(tr("New group"));
        refreshButton->setText(tr("Refresh"));
        tagFilterButton->setText(tr("#tag"));
        tagFilterButton->setToolTip(tr("Filter hosts by exact tag"));
        addHostButton->setToolTip(tr("Create a new SSH host in the current group"));
        addGroupButton->setToolTip(tr("Create a new host group at the current level"));
        refreshButton->setToolTip(tr("Reload hosts and groups"));
        refresh();
    });
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
            tr("Connect"));
        connect(connectAction, &QAction::triggered,
                this, [this, item]() { activateTreeItem(item); });
        menu->addSeparator();
    }

    // “新建主机”是主机管理中的首要创建动作，必须排在“新建分组”之前；这里的顺序与
    // 标题动作区保持一致，避免同一页面的两个入口形成相反的操作习惯。
    QAction *addHostAction = menu->addAction(
        UiIcons::symbol(this, UiIcons::Symbol::Plus, this->palette().color(QPalette::WindowText)),
        tr("New host"));
    QAction *addGroupAction = menu->addAction(
        UiIcons::symbol(this,
                        UiIcons::Symbol::FolderAdd,
                        this->palette().color(QPalette::WindowText)),
        tr("New group"));
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
                tr("Copy"));
            connect(copyAction,
                    &QAction::triggered,
                    this,
                    &HostManagerWidget::copySelectedHost);
        }
        QAction *editAction = menu->addAction(
            UiIcons::symbol(this,
                            UiIcons::Symbol::Edit,
                            this->palette().color(QPalette::WindowText)),
            tr("Edit"));
        QAction *deleteAction = menu->addAction(
            UiIcons::symbol(this,
                            UiIcons::Symbol::Trash,
                            this->palette().color(QPalette::WindowText)),
            tr("Delete"));
        connect(editAction, &QAction::triggered, this, &HostManagerWidget::editSelected);
        connect(deleteAction, &QAction::triggered, this, &HostManagerWidget::removeSelected);
    }

    menu->addSeparator();
    QAction *importAction = menu->addAction(
        UiIcons::symbol(this,
                        UiIcons::Symbol::Upload,
                        this->palette().color(QPalette::WindowText)),
        tr("Import Hosts…"));
    QAction *exportAction = menu->addAction(
        UiIcons::symbol(this,
                        UiIcons::Symbol::Download,
                        this->palette().color(QPalette::WindowText)),
        tr("Export Hosts…"));
    connect(importAction, &QAction::triggered, this, &HostManagerWidget::importHosts);
    connect(exportAction, &QAction::triggered, this, &HostManagerWidget::exportHosts);
    menu->addSeparator();
    QAction *refreshAction = menu->addAction(
        UiIcons::symbol(this,
                        UiIcons::Symbol::Refresh,
                        this->palette().color(QPalette::WindowText)),
        tr("Refresh"));
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
        // 只恢复选择，不再强制展开祖先目录。强制展开会覆盖用户刚保存的折叠偏好；即使当前
        // 记录位于折叠目录内，也应由用户下次主动展开，而不是让普通刷新悄悄改变目录状态。
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
        m_tree->addTopLevelItem(createSectionHeaderItem(tr("Ungrouped")));
        for (const HostRecord &record : ungroupedHosts) {
            m_tree->addTopLevelItem(createTreeItem(record));
        }
    }
    return true;
}

void HostManagerWidget::loadFolderExpansionState()
{
    m_folderExpansionState.clear();

    QString error;
    const std::optional<QString> storedValue =
        m_database->setting(kHostTreeExpansionSettingKey, &error);
    if (!error.isEmpty()) {
        // 展开状态只是界面偏好，读取失败不能阻断主机管理器启动。数据库层已给出可诊断文本，
        // 这里记录日志并回退到全部默认展开；后续用户操作仍会尝试重新写入以实现自动修复。
        qWarning().noquote() << tr("Failed to read the host folder expansion state: %1").arg(error);
        return;
    }
    if (!storedValue.has_value()) {
        // 全新安装或升级前从未保存过状态时保持历史行为：所有目录默认展开。
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(storedValue->toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        // 损坏的纯界面设置不值得阻塞启动；忽略整个值比部分猜测更可预测，下次真实操作会用
        // 当前支持的结构覆盖它。日志保留偏移和原因，方便排查手工改库或异常写入。
        qWarning().noquote()
            << tr("The host folder expansion state is corrupt; using the default expansion: %1 (offset %2)")
                   .arg(parseError.errorString())
                   .arg(parseError.offset);
        return;
    }

    const QJsonObject rootObject = document.object();
    const QJsonValue versionValue = rootObject.value(QStringLiteral("version"));
    const QJsonValue foldersValue = rootObject.value(QStringLiteral("folders"));
    if (!versionValue.isDouble()
        || versionValue.toInt() != kHostTreeExpansionStateVersion
        || !foldersValue.isObject()) {
        // 版本或结构不受支持时保留默认展开。显式校验可避免未来格式升级后把任意 JSON 字段
        // 错当成目录 id，进而产生难以解释的随机折叠状态。
        qWarning().noquote()
            << tr("The host folder expansion state version or structure is unsupported; using the default expansion");
        return;
    }

    const QJsonObject folderStates = foldersValue.toObject();
    for (auto iterator = folderStates.constBegin(); iterator != folderStates.constEnd(); ++iterator) {
        // 单个无效条目不影响其他目录的合法状态。稳定 id 不能为空，值也必须是真正的 JSON
        // 布尔值；拒绝 0/1 和字符串可防止宽松转换把损坏数据解释成用户偏好。
        if (!iterator.key().isEmpty() && iterator.value().isBool()) {
            m_folderExpansionState.insert(iterator.key(), iterator.value().toBool());
        }
    }
}

void HostManagerWidget::restoreFolderExpansionState()
{
    // setExpanded() 会正常发出 itemExpanded/itemCollapsed。恢复属于程序重建而不是用户操作，
    // 必须阻断信号，否则启动时就会为每个目录同步写库，甚至覆盖尚未应用完的其他目录状态。
    const QSignalBlocker expansionSignalBlocker(m_tree);

    QList<QTreeWidgetItem *> pendingItems;
    pendingItems.reserve(m_tree->topLevelItemCount());
    for (int index = 0; index < m_tree->topLevelItemCount(); ++index) {
        pendingItems.append(m_tree->topLevelItem(index));
    }

    while (!pendingItems.isEmpty()) {
        QTreeWidgetItem *item = pendingItems.takeLast();
        const auto kind = static_cast<HostNodeKind>(item->data(0, HostKindRole).toInt());
        if (kind == HostNodeKind::Folder) {
            const QString folderId = item->data(0, HostIdRole).toString();
            if (!folderId.isEmpty()) {
                // 未出现过的新目录使用 true，并把默认值纳入内存快照。此后用户操作任意目录时，
                // 会把当前树中所有已知目录一起保存，确保展开与折叠状态形成完整、一致的快照。
                const bool expanded = m_folderExpansionState.value(folderId, true);
                m_folderExpansionState.insert(folderId, expanded);
                item->setExpanded(expanded);
            }
        }

        // 使用显式栈覆盖任意深度的嵌套目录，避免实现只对顶层分组有效。
        for (int childIndex = 0; childIndex < item->childCount(); ++childIndex) {
            pendingItems.append(item->child(childIndex));
        }
    }
}

void HostManagerWidget::persistFolderExpansionState(QTreeWidgetItem *item, const bool expanded)
{
    if (item == nullptr
        || static_cast<HostNodeKind>(item->data(0, HostKindRole).toInt())
            != HostNodeKind::Folder) {
        // 搜索结果或“未分组”标题等展示节点不是持久化目录，不能污染设置快照。
        return;
    }

    const QString folderId = item->data(0, HostIdRole).toString();
    if (folderId.isEmpty()) {
        // 真实目录理论上始终有数据库 id；防御性忽略异常节点，避免把空键写入后让多个目录
        // 共享同一状态。主机数据本身不在这里修改，因此无需冒险尝试自动修复。
        return;
    }
    m_folderExpansionState.insert(folderId, expanded);

    QJsonObject folderStates;
    for (auto iterator = m_folderExpansionState.constBegin();
         iterator != m_folderExpansionState.constEnd();
         ++iterator) {
        folderStates.insert(iterator.key(), iterator.value());
    }

    QJsonObject rootObject;
    rootObject.insert(QStringLiteral("version"), kHostTreeExpansionStateVersion);
    rootObject.insert(QStringLiteral("folders"), folderStates);
    const QString serializedState = QString::fromUtf8(
        QJsonDocument(rootObject).toJson(QJsonDocument::Compact));

    QString error;
    if (!m_database->setSetting(kHostTreeExpansionSettingKey, serializedState, &error)) {
        // 状态已留在内存中，因此当前会话交互仍正确；明确提示落库失败，让用户知道下次启动
        // 可能无法恢复，而不是把持久化故障伪装成保存成功。
        showDatabaseError(tr("Save host folder expansion state"), error);
    }
}

void HostManagerWidget::applyFilter()
{
    // clear() 和后续状态恢复都属于模型重建，不应被 itemCollapsed/itemExpanded 监听器当成用户
    // 操作。这里覆盖整个重建过程；restoreFolderExpansionState() 内的阻断器允许安全嵌套。
    const QSignalBlocker treeRebuildSignalBlocker(m_tree);
    m_tree->clear();
    // 配色可能刚在标签编辑器里被改过。放在这里而不是 refresh() 里，是因为搜索过滤也会重建
    // 全部节点却不经过 refresh()，两条路径必须拿到同一份最新快照。
    m_tagColors = loadTagColors(m_database);
    const QString query = m_searchEdit->text().trimmed();
    QString error;

    // 搜索结果是按命中排序的扁平列表，节点的 QTreeWidgetItem 父子关系与数据库里真实的
    // parentId 无关；如果这时还允许拖拽，drop 完成后会把搜索排序误当成真实排序写库。
    m_tree->setDragEnabled(query.isEmpty());
    m_tree->setAcceptDrops(query.isEmpty());

    if (query.isEmpty()) {
        if (!appendRootLevel(&error)) {
            showDatabaseError(tr("Load host tree"), error);
            return;
        }
        restoreFolderExpansionState();
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
        showDatabaseError(tr("Search hosts"), error);
        return;
    }

    // 搜索结果使用扁平视图，避免为了展示一个命中项而加载并展开整条祖先链；节点 id 仍是
    // 稳定数据库主键，双击连接和编辑不会受显示结构变化影响。
    for (const HostRecord &record : results) {
        m_tree->addTopLevelItem(createTreeItem(record));
    }
}

QToolButton *HostManagerWidget::buildTagSuggestionChip(const QString &tag)
{
    auto *chip = new QToolButton(m_tagSuggestionsFlow);
    chip->setObjectName(QStringLiteral("hostTagChip"));
    chip->setText(QStringLiteral("#%1").arg(tag));
    chip->setStyleSheet(tagPillStyleSheet(resolveTagColor(m_tagColors, tag)));
    chip->setCursor(Qt::PointingHandCursor);
    chip->setToolTip(tr("Filter hosts by this exact tag"));
    // 点击直接把完整"#标签名"写回搜索框：既复用 applyFilter() 已有的精确匹配路径，也让
    // 面板本身不必重复一份筛选逻辑。写入会经 textChanged 关闭面板（前缀不再是待选状态）。
    connect(chip, &QToolButton::clicked, this, [this, tag]() {
        m_searchEdit->setText(QStringLiteral("#%1").arg(tag));
        m_searchEdit->setFocus(Qt::ShortcutFocusReason);
        m_searchEdit->setCursorPosition(m_searchEdit->text().size());
        hideTagSuggestions();
    });
    return chip;
}

void HostManagerWidget::updateTagSuggestions()
{
    const QString query = m_searchEdit->text().trimmed();
    if (!query.startsWith(QLatin1Char('#'))) {
        hideTagSuggestions();
        return;
    }

    const QString prefix = query.mid(1);
    QStringList matches;
    for (const QString &tag : collectKnownTags(m_database)) {
        if (prefix.isEmpty() || tag.contains(prefix, Qt::CaseInsensitive)) {
            matches.append(tag);
        }
    }

    QLayout *flow = m_tagSuggestionsFlow->layout();
    Q_ASSERT(flow != nullptr);
    QLayoutItem *item = nullptr;
    while ((item = flow->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    if (matches.isEmpty()) {
        // 没有任何标签可选（库中尚无标签，或输入的前缀未命中任何历史标签）时不展示空面板，
        // 一个只有内边距的空框只会让用户误以为程序卡住了。
        hideTagSuggestions();
        return;
    }
    for (const QString &tag : matches) {
        flow->addWidget(buildTagSuggestionChip(tag));
    }

    m_tagSuggestionsPopup->adjustSize();
    const int width = std::max(m_searchBar->width(), m_tagSuggestionsPopup->sizeHint().width());
    m_tagSuggestionsPopup->setFixedWidth(width);
    m_tagSuggestionsPopup->adjustSize();
    const QPoint anchor = m_searchBar->mapToGlobal(QPoint(0, m_searchBar->height() + 4));
    m_tagSuggestionsPopup->move(anchor);
    m_tagSuggestionsPopup->show();
    m_tagSuggestionsPopup->raise();
}

void HostManagerWidget::hideTagSuggestions()
{
    if (m_tagSuggestionsPopup != nullptr) {
        m_tagSuggestionsPopup->hide();
    }
}

bool HostManagerWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (m_tagSuggestionsPopup != nullptr && m_tagSuggestionsPopup->isVisible()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            auto *clicked = QApplication::widgetAt(mouseEvent->globalPosition().toPoint());
            const bool insidePopup = clicked != nullptr
                && (clicked == m_tagSuggestionsPopup || m_tagSuggestionsPopup->isAncestorOf(clicked));
            const bool insideSearchBar = clicked != nullptr
                && (clicked == m_searchBar || m_searchBar->isAncestorOf(clicked));
            if (!insidePopup && !insideSearchBar) {
                hideTagSuggestions();
            }
        } else if (watched == m_searchEdit && event->type() == QEvent::KeyPress) {
            if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
                hideTagSuggestions();
            }
        }
    }
    return QWidget::eventFilter(watched, event);
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
        showDatabaseError(tr("New group"), error);
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
                tr("Read parent group configuration"),
                parentError.isEmpty() ? tr("The parent group does not exist") : parentError);
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
            record, target, tr("Login credentials for %1").arg(record.name), &error);
        if (!hostSaved) {
            showDatabaseError(tr("Save encrypted credentials"), error);
            return;
        }
    }
    if (!hostSaved && !m_database->saveHost(record, &error)) {
        showDatabaseError(tr("Save host"), error);
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
            showDatabaseError(tr("Read host"), error);
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
        showDatabaseError(tr("Read current group"), error);
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
        showDatabaseError(tr("Duplicate host"), error);
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
            showDatabaseError(tr("Read node"), error);
        }
        return;
    }

    HostRecord inheritedParent;
    if (existing->kind == HostNodeKind::Host && !existing->parentId.isEmpty()) {
        const std::optional<HostRecord> resolvedParent =
            m_database->resolveHost(existing->parentId, &error);
        if (!resolvedParent.has_value()) {
            showDatabaseError(
                tr("Read parent group configuration"),
                error.isEmpty() ? tr("The parent group does not exist") : error);
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
            updated, editedTarget, tr("Login credentials for %1").arg(updated.name), &error);
        if (!hostSaved) {
            showDatabaseError(tr("Update encrypted credentials"), error);
            return;
        }
    }

    if (!hostSaved && !m_database->saveHost(updated, &error)) {
        showDatabaseError(tr("Update node"), error);
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
            showDatabaseError(tr("Read node"), error);
        }
        return;
    }

    const QString warning = record->kind == HostNodeKind::Folder
        ? tr("Deleting group '%1' also deletes all of its subgroups and hosts, and cannot be undone. Continue?")
              .arg(record->name)
        : tr("Delete host '%1'? This action cannot be undone.").arg(record->name);
    if (QMessageBox::warning(this,
                             tr("Confirm delete"),
                             warning,
                             QMessageBox::Yes | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    int removedCount = 0;
    if (!m_database->removeHost(record->id, &removedCount, &error)) {
        showDatabaseError(tr("Delete node"), error);
        return;
    }
    refresh();
}

void HostManagerWidget::importHosts()
{
    const QString defaultConfig = QDir::home().filePath(QStringLiteral(".ssh/config"));
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Import host configuration"),
        QFileInfo::exists(defaultConfig) ? defaultConfig : QDir::homePath(),
        tr("Supported configuration (*.json *.conf *.config);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this,
                              tr("Could not read the import file"),
                              tr("Could not open %1: %2").arg(path, file.errorString()));
        return;
    }
    const QByteArray content = file.readAll();
    QList<HostRecord> importedRecords;
    QString error;
    const bool parsed = content.trimmed().startsWith('{')
        ? ConnectionConfigCodec::importJson(content, &importedRecords, &error)
        : ConnectionConfigCodec::importOpenSsh(content, &importedRecords, &error);
    if (!parsed) {
        QMessageBox::critical(this, tr("Import failed"), error);
        return;
    }
    if (importedRecords.isEmpty()) {
        QMessageBox::information(this, tr("No hosts to import"), tr("The file has no usable records."));
        return;
    }

    const QString confirmation =
        tr("%1 host/group records will be imported. Every record is assigned a new local primary key, and credentials are not imported from the configuration file.")
            .arg(importedRecords.size());
    if (QMessageBox::question(this,
                              tr("Confirm import"),
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
        showDatabaseError(tr("Import hosts"), error);
        return;
    }
    refresh();
    QMessageBox::information(this,
                             tr("Import complete"),
                             tr("Successfully imported %1 records.").arg(importedRecords.size()));
}

void HostManagerWidget::exportHosts()
{
    QList<HostRecord> records;
    QString error;
    if (!collectRecords({}, &records, &error)) {
        showDatabaseError(tr("Read export data"), error);
        return;
    }
    if (records.isEmpty()) {
        QMessageBox::information(this, tr("No hosts to export"), tr("The host tree is currently empty."));
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this,
        tr("Export Shellium hosts"),
        QDir::home().filePath(QStringLiteral("shellium-hosts.json")),
        tr("Shellium JSON (*.json)"));
    if (path.isEmpty()) {
        return;
    }

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        QMessageBox::critical(this,
                              tr("Could not write the export file"),
                              tr("Could not open %1: %2").arg(path, output.errorString()));
        return;
    }
    const QByteArray json = ConnectionConfigCodec::exportJson(records, &error);
    if (json.isEmpty()) {
        QMessageBox::critical(this,
                              tr("Export failed"),
                              error.isEmpty() ? tr("Could not encode the host configuration") : error);
        output.cancelWriting();
        return;
    }
    if (output.write(json) != json.size() || !output.commit()) {
        QMessageBox::critical(this,
                              tr("Export failed"),
                              tr("Failed to write %1: %2").arg(path, output.errorString()));
        return;
    }
    QMessageBox::information(this,
                             tr("Export complete"),
                             tr("%1 host/group records exported. The file contains no credentials.")
                                 .arg(records.size()));
}

bool HostManagerWidget::collectRecords(const QString &parentId,
                                       QList<HostRecord> *records,
                                       QString *errorMessage) const
{
    if (records == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = tr("The export record container is empty");
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
        showDatabaseError(tr("Resolve host chain"),
                          error.isEmpty() ? tr("The host record or chain does not exist") : error);
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
    const QIcon distributionIcon = HostDistributionIcon::iconFor(record.distributionId);
    item->setIcon(
        0,
        record.kind == HostNodeKind::Folder
            ? localFileFolderIcon(m_tree)
            // 尚未完成首次登录、或远端不提供 os-release 时才使用原有通用服务器图标；只要
            // 已有可识别发行版，侧栏左侧就稳定显示其对应图标，刷新与重启不会退化。
            : (distributionIcon.isNull()
                   ? UiIcons::symbol(
                         m_tree,
                         UiIcons::Symbol::HostItem,
                         m_tree->palette().color(QPalette::WindowText))
                   : distributionIcon));
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
                ? tr(" · via %1").arg(firstJump.has_value() ? firstJump->name : tr("Invalid jump host"))
                : tr(" · via %1 and %2 jump hosts")
                      .arg(firstJump.has_value() ? firstJump->name : tr("Invalid jump host"))
                      .arg(record.hostChain.size());
        }
        item->setData(0, HostDetailRole, detail);
        // resolveHost() 会把父分组标签合并进主机；展示这一结果才能让“分组标记为测试环境”
        // 的主机在树中也出现相同徽章，与 #标签 搜索的实际命中语义保持一致。
        item->setData(0, HostTagsRole, displayRecord.tags);
        item->setData(0,
                      HostTagColorRole,
                      displayRecord.tags.isEmpty()
                          ? QVariant{}
                          : QVariant::fromValue(resolveTagColor(
                                m_tagColors, displayRecord.tags.constFirst())));
        const QString distributionText = HostDistributionIcon::displayName(
            record.distributionId, record.distributionVersionId);
        // 版本号不会挤占两行主机卡片的有限空间，但悬停时必须可见，方便用户确认图标背后
        // 的精确发行版本，而不是只凭颜色猜测系统家族。
        item->setToolTip(0, distributionText.isEmpty()
                                ? detail
                                : tr("%1\nSystem: %2").arg(detail, distributionText));
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
        showDatabaseError(tr("Save drag-and-drop order"), error);
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
                *errorMessage = tr("Node %1 not found").arg(childId);
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
                          tr("Host data operation failed"),
                          tr("%1 failed: %2").arg(operation, details));
}
