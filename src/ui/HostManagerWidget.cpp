#include "ui/HostManagerWidget.h"

#include "core/ConnectionConfigCodec.h"
#include "security/Vault.h"
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

/** @brief 主机节点保存“账号@地址:端口”副标题的角色；分组节点不设置该角色。 */
constexpr int HostDetailRole = Qt::UserRole + 2;

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

/** @brief 节点名称行的字体；分组与主机共用，避免同一棵树里出现两种标题字重。 */
[[nodiscard]] QFont itemNameFont(const QFont &base)
{
    return scaledFont(base, 1.0, true);
}

/** @brief 分组展开箭头的边长（像素）。 */
constexpr int kToggleArrowSize = 8;

/** @brief 展开箭头与行右端之间的留白。 */
constexpr int kToggleArrowMargin = 6;

/** @brief 分组名称右端与展开箭头之间至少要留出的空隙。 */
constexpr int kToggleArrowSpacing = 6;

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

    /** @brief 点中分组展开箭头时触发；由外部接上真正的展开/收起。 */
    std::function<void(const QModelIndex &)> onToggleRequested;

    /**
     * @brief 分组展开箭头的绘制与点击热区。
     * @return 非分组节点、或没有子节点的空分组返回空矩形（不画也点不中）。
     */
    [[nodiscard]] QRect toggleRect(const QStyleOptionViewItem &option,
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

protected:
    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (isFolderIndex(index)) {
            paintFolder(painter, option, index);
            return;
        }

        const QString detail = index.data(HostDetailRole).toString();
        if (detail.isEmpty()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        // 背景、选中态与焦点框仍由样式绘制，本委托只替换文字部分，避免自绘出一套与其余
        // 列表不一致的选中色。
        QStyleOptionViewItem background = option;
        initStyleOption(&background, index);
        background.text.clear();
        const QWidget *widget = option.widget;
        QStyle *style = widget != nullptr ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);

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
        const bool selected = (background.state & QStyle::State_Selected) != 0;
        const QColor baseColor = background.palette.color(
            selected ? QPalette::HighlightedText : QPalette::Text);

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
        // 委托自己做命中判断。与复选框一致地取释放事件，按下后移开鼠标就不会误触发。
        if (event != nullptr && event->type() == QEvent::MouseButtonRelease && onToggleRequested) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton
                && toggleRect(option, index).contains(mouseEvent->position().toPoint())) {
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

    /** @brief 扣掉右端箭头及其左右留白后，留给分组名称的宽度。 */
    [[nodiscard]] static int availableNameWidth(const QRect &textRect)
    {
        return std::max(
            0, textRect.width() - kToggleArrowMargin - kToggleArrowSize - kToggleArrowSpacing);
    }

    /** @brief 画分组：一行名称，紧跟其后的是展开/收起箭头。 */
    void paintFolder(QPainter *painter,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) const
    {
        QStyleOptionViewItem background = option;
        initStyleOption(&background, index);
        const QString name = background.text;
        background.text.clear();
        const QWidget *widget = option.widget;
        QStyle *style = widget != nullptr ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);

        const QRect iconRect =
            style->subElementRect(QStyle::SE_ItemViewItemDecoration, &background, widget);
        if (!iconRect.isEmpty()) {
            qvariant_cast<QIcon>(index.data(Qt::DecorationRole))
                .paint(painter, iconRect, Qt::AlignCenter);
        }

        const QRect textRect = itemTextRect(background);
        const QRect arrowRect = toggleRect(option, index);
        // 名称区始终扣掉箭头宽度：否则长分组名省略后正好铺满文字区，箭头会压在名字上。
        const int nameWidth = arrowRect.isNull() ? textRect.width() : availableNameWidth(textRect);
        const QColor baseColor = background.palette.color(
            (background.state & QStyle::State_Selected) != 0 ? QPalette::HighlightedText
                                                             : QPalette::Text);

        painter->save();
        // 分组名与主机名同字号同字重：两者都是这棵树里的“标题”，字体一旦不同，分组会被
        // 读成比主机更次一级的说明文字。
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
 * 主机和分组共用 HostRecord，分组中的端口、用户名、私钥与 ProxyJump 会被子节点继承。
 * 因此不能只让分组编辑名称；本对话框显式提供全部可继承字段，并用“继承父分组端口”复选框
 * 保留 std::optional 端口的三态语义。密码只作为新建后立即连接的会话值返回，绝不写入记录。
 */
class HostRecordDialog final : public QDialog {
public:
    /**
     * @brief 创建记录编辑器。
     * @param initial 待编辑快照；新建时调用方应先填好 kind、id 和 parentId。
     * @param parent Qt 父对象。
     */
    explicit HostRecordDialog(const HostRecord &initial, QWidget *parent = nullptr)
        : QDialog(parent)
        , m_initial(initial)
        , m_nameEdit(new QLineEdit(initial.name, this))
        , m_hostEdit(new QLineEdit(initial.host, this))
        , m_portSpin(new QSpinBox(this))
        , m_inheritPortToggle(new ToggleSwitch(this))
        , m_usernameEdit(new QLineEdit(initial.username, this))
        , m_identityEdit(new QLineEdit(initial.identityFile, this))
        , m_proxyJumpsEdit(new QLineEdit(initial.proxyJumps.join(QLatin1Char(',')), this))
        , m_tagsEdit(new QLineEdit(initial.tags.join(QLatin1Char(',')), this))
        , m_passwordEdit(new QLineEdit(this))
        , m_savePasswordToggle(new ToggleSwitch(this))
        , m_legacyHmacToggle(new ToggleSwitch(this))
    {
        // 字段版式与设置窗口、连接对话框统一：分组卡片 + “左说明右控件”的行 + 滑动开关。
        const bool isHost = initial.kind == HostNodeKind::Host;
        setObjectName(QStringLiteral("ConnectionDialog"));
        setWindowTitle(isHost ? tr("主机配置") : tr("分组配置"));
        setMinimumWidth(580);

        m_portSpin->setRange(1, 65535);
        m_portSpin->setValue(initial.port.value_or(quint16{22}));
        m_inheritPortToggle->setChecked(!initial.port.has_value());
        m_portSpin->setEnabled(initial.port.has_value());
        m_usernameEdit->setPlaceholderText(tr("留空则继承父分组"));
        m_identityEdit->setPlaceholderText(tr("留空则继承父分组或使用 Agent/默认密钥"));
        m_proxyJumpsEdit->setPlaceholderText(tr("逗号分隔；留空则继承父分组"));
        m_tagsEdit->setPlaceholderText(tr("逗号分隔；会与祖先分组标签合并"));
        m_passwordEdit->setEchoMode(QLineEdit::Password);
        m_passwordEdit->setPlaceholderText(tr("仅用于本次立即连接；保存由保险库负责"));
        m_savePasswordToggle->setChecked(!initial.credentialId.isEmpty());
        m_legacyHmacToggle->setChecked(initial.allowLegacyHmacSha1);
        for (QLineEdit *edit : {m_nameEdit, m_hostEdit, m_usernameEdit, m_proxyJumpsEdit,
                                m_tagsEdit, m_passwordEdit}) {
            edit->setMinimumWidth(260);
        }

        auto *identityContainer = new QWidget(this);
        auto *identityLayout = new QHBoxLayout(identityContainer);
        identityLayout->setContentsMargins(0, 0, 0, 0);
        identityLayout->setSpacing(8);
        auto *browseButton = new QPushButton(tr("浏览…"), identityContainer);
        UiIcons::decorateButton(
            browseButton,
            UiIcons::symbol(this, UiIcons::Symbol::Upload, this->palette().color(QPalette::WindowText)),
            tr("选择 SSH 私钥文件"));
        m_identityEdit->setMinimumWidth(200);
        identityLayout->addWidget(m_identityEdit, 1);
        identityLayout->addWidget(browseButton);

        auto *basicsCard = new SettingsCard(isHost ? tr("主机") : tr("分组"), this);
        basicsCard->addRow(tr("名称"), m_nameEdit);
        if (isHost) {
            basicsCard->addRow(tr("主机地址"), m_hostEdit);
        }
        basicsCard->addRow(tr("继承父分组端口"),
                           m_inheritPortToggle,
                           tr("关闭后才会使用下方端口，开启表示沿用父分组取值。"));
        QWidget *portRow = basicsCard->addRow(tr("端口"), new SettingsStepper(m_portSpin, basicsCard));
        portRow->setEnabled(initial.port.has_value());
        basicsCard->addRow(tr("用户名"), m_usernameEdit);
        basicsCard->addRow(tr("标签"), m_tagsEdit);

        auto *authCard = new SettingsCard(tr("认证与跳板"), this);
        authCard->addRow(tr("私钥文件"), identityContainer);
        authCard->addRow(tr("ProxyJump"), m_proxyJumpsEdit);
        if (isHost) {
            authCard->addRow(tr("会话密码/口令"), m_passwordEdit);
            authCard->addRow(tr("加密保存到本地保险库"), m_savePasswordToggle);
            authCard->addRow(tr("旧版兼容"),
                             m_legacyHmacToggle,
                             tr("仅为该主机允许 hmac-sha1（不继承）。"));
        } else {
            // 分组不展示主机地址、密码与旧版兼容这几行，但这些控件已经以对话框为父对象
            // 创建好了。父子关系只决定归属、不决定位置：没有被任何布局接管的控件会停在
            // (0,0) 并按 sizeHint 绘制，于是分组配置窗口左上角会浮出一个空白地址框、一段
            // 密码占位文字和一个开关，盖住卡片顶部；它们还能被 Tab 选中并接收键入。
            // 显式隐藏即可让它们退出绘制与焦点链，record() 里读取它们的取值仍然有效。
            for (QWidget *hostOnlyField : {static_cast<QWidget *>(m_hostEdit),
                                           static_cast<QWidget *>(m_passwordEdit),
                                           static_cast<QWidget *>(m_savePasswordToggle),
                                           static_cast<QWidget *>(m_legacyHmacToggle)}) {
                hostOnlyField->setVisible(false);
            }
        }

        auto *footer = new QWidget(this);
        footer->setObjectName(QStringLiteral("SettingsFooter"));
        auto *footerLayout = new QHBoxLayout(footer);
        footerLayout->setContentsMargins(20, 12, 20, 12);
        footerLayout->setSpacing(10);
        auto *cancelButton = new QPushButton(tr("取消"), footer);
        auto *saveButton = new QPushButton(tr("保存"), footer);
        saveButton->setProperty("primaryAction", true);
        saveButton->setDefault(true);
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
        bodyLayout->addWidget(authCard);
        bodyLayout->addStretch(1);
        layout->addWidget(body, 1);
        layout->addWidget(footer);

        connect(m_inheritPortToggle, &ToggleSwitch::toggled,
                portRow, [portRow](const bool inherit) { portRow->setEnabled(!inherit); });
        connect(browseButton, &QPushButton::clicked, this, [this]() {
            const QString path = QFileDialog::getOpenFileName(this, tr("选择 SSH 私钥文件"));
            if (!path.isEmpty()) {
                m_identityEdit->setText(path);
            }
        });
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

    /** @brief 返回用户确认后的记录；id、kind、parentId、排序和创建时间保持不变。 */
    [[nodiscard]] HostRecord record() const
    {
        HostRecord result = m_initial;
        result.name = m_nameEdit->text().trimmed();
        result.host = m_hostEdit->text().trimmed();
        result.port = m_inheritPortToggle->isChecked()
            ? std::optional<quint16>{}
            : std::optional<quint16>{static_cast<quint16>(m_portSpin->value())};
        result.username = m_usernameEdit->text().trimmed();
        result.identityFile = m_identityEdit->text().trimmed();
        result.proxyJumps = splitList(m_proxyJumpsEdit->text());
        result.tags = splitList(m_tagsEdit->text());
        result.allowLegacyHmacSha1 = result.kind == HostNodeKind::Host
            && m_legacyHmacToggle->isChecked();
        return result;
    }

    /** @brief 返回只用于紧随保存之后首次连接的明文，不属于 HostRecord 持久化数据。 */
    [[nodiscard]] QString sessionPassword() const
    {
        return m_passwordEdit->text();
    }

    /**
     * @brief 用户是否要求把本次输入加密保存到保险库。
     * @return 只有主机记录且复选框选中时返回 true；分组永远不直接保存密码。
     */
    [[nodiscard]] bool shouldSavePassword() const
    {
        return m_initial.kind == HostNodeKind::Host && m_savePasswordToggle->isChecked();
    }

    /**
     * @brief 用户是否明确取消了一条既有凭据引用。
     * @return 原记录有关联凭据且复选框被取消时返回 true。
     */
    [[nodiscard]] bool shouldDetachCredential() const
    {
        return !m_initial.credentialId.isEmpty() && !m_savePasswordToggle->isChecked();
    }

private:
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
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    ToggleSwitch *m_inheritPortToggle = nullptr;
    QLineEdit *m_usernameEdit = nullptr;
    QLineEdit *m_identityEdit = nullptr;
    QLineEdit *m_proxyJumpsEdit = nullptr;
    QLineEdit *m_tagsEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    ToggleSwitch *m_savePasswordToggle = nullptr;
    ToggleSwitch *m_legacyHmacToggle = nullptr;
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
        if (item != nullptr) {
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
        if (!appendChildren(nullptr, {}, &error)) {
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
    HostRecordDialog dialog(record, this);
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

    HostRecordDialog dialog(record, this);
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

    HostRecordDialog dialog(*existing, this);
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
    item->setIcon(
        0,
        record.kind == HostNodeKind::Folder
            ? UiIcons::symbol(m_tree, UiIcons::Symbol::Folder, m_tree->palette().color(QPalette::WindowText))
            : UiIcons::symbol(m_tree, UiIcons::Symbol::Hosts, m_tree->palette().color(QPalette::WindowText)));
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
