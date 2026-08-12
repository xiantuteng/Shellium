#include "productivity/ProductivityWidget.h"

#include "productivity/ProductivityModel.h"
#include "ui/UiIcons.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QSplitter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTextEdit>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace {

/** @brief QTreeWidgetItem::UserRole 中保存的稳定节点 id。 */
constexpr int kIdRole = Qt::UserRole;

/** @brief QTreeWidgetItem::UserRole+1 中保存的 SnippetNodeKind 整数值。 */
constexpr int kKindRole = Qt::UserRole + 1;

/**
 * @brief QTreeWidgetItem::UserRole+2 中保存的第二行预览文案。
 *
 * 只有 Snippet 节点会设置：有 description 就显示 description，为空则退化显示 command，
 * 不引入新数据源。分组节点不设置该角色，委托据此判断要不要多画一行。
 */
constexpr int kPreviewRole = Qt::UserRole + 2;

/** @brief 目标列表项 UserRole 中保存的持久化主机 id。 */
constexpr int kTargetIdRole = Qt::UserRole;

/** @brief 按点/像素两种字号语义缩放字体，避免 pointSizeF 为 -1 时算出 0 号字。 */
QFont scaledFont(QFont font, const double deltaPoints)
{
    if (font.pointSizeF() > 0.0) {
        font.setPointSizeF(std::max(7.0, font.pointSizeF() + deltaPoints));
    } else if (font.pixelSize() > 0) {
        font.setPixelSize(std::max(9, font.pixelSize() + qRound(deltaPoints * 4.0 / 3.0)));
    }
    return font;
}

/**
 * @brief 把片段行画成"名称 + 命令/说明预览"两行，分组行仍是单行。
 *
 * 320px 侧栏放不下"名称/说明"两列表头，两列本身也会把说明挤到几乎看不清的宽度。改成
 * 单列后第二行复用 kPreviewRole 里已经算好的文案，委托只负责排版，不重新读数据库。
 */
class SnippetItemDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void paint(QPainter *painter,
              const QStyleOptionViewItem &option,
              const QModelIndex &index) const override
    {
        const QString preview = index.data(kPreviewRole).toString();
        if (preview.isEmpty()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        // 背景与选中态仍交给样式（generic QTreeView 已有的浅色选中底纹），本委托只接管
        // 文字排版，不重新实现一套选中视觉。
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

        const QFont nameFont = option.font;
        const QFont previewFont = scaledFont(option.font, -1.5);
        const QFontMetrics nameMetrics(nameFont);
        const QFontMetrics previewMetrics(previewFont);
        // 选中态下 generic QTreeView 的 QSS 只换背景、不换字色（@TEXT@ 保持不变），这里跟着
        // 用同一条 Text 角色，选中前后文字颜色才不会“跳色”。
        const QColor baseColor = background.palette.color(QPalette::Text);

        painter->save();
        const QRect nameRect(contentRect.left(), contentRect.top(), contentRect.width(),
                             nameMetrics.height());
        painter->setFont(nameFont);
        painter->setPen(baseColor);
        painter->drawText(nameRect,
                          Qt::AlignLeft | Qt::AlignVCenter,
                          nameMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                                 Qt::ElideRight,
                                                 nameRect.width()));

        QColor previewColor = baseColor;
        previewColor.setAlphaF(0.62F);
        const QRect previewRect(contentRect.left(),
                                nameRect.bottom() + 1,
                                contentRect.width(),
                                previewMetrics.height());
        painter->setFont(previewFont);
        painter->setPen(previewColor);
        painter->drawText(previewRect,
                          Qt::AlignLeft | Qt::AlignVCenter,
                          previewMetrics.elidedText(preview, Qt::ElideMiddle, previewRect.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        if (index.data(kPreviewRole).toString().isEmpty()) {
            return hint;
        }
        const int textHeight = QFontMetrics(option.font).height()
            + QFontMetrics(scaledFont(option.font, -1.5)).height() + 1;
        hint.setHeight(std::max(hint.height(), textHeight + 8));
        return hint;
    }
};

/**
 * @brief 将效率侧栏的次要命令配置为紧凑图标按钮。
 * @param button 待配置按钮；不得为空。
 * @param accessibleName 动作完整中文名称，同时用于 Tooltip 和无障碍树。
 *
 * Snippet 页面需要把主要空间留给树与编辑器。树管理工具栏中的三个动作都使用图标，
 * 完整语义通过 Tooltip 与无障碍名称保留；保存、执行等内容级主流程继续使用文字按钮。
 */
void configureProductivityIconButton(QToolButton *button, const QString &accessibleName)
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
 * @brief 创建一条可复选的执行目标列表项。
 *
 * 列表项不保存 ConnectionTarget 全量对象，尤其不能复制 password；稳定 id 足以让上层在
 * 真正发送前重新定位会话，也避免侧栏快照意外延长凭据驻留时间。
 */
QListWidgetItem *createTargetItem(const ConnectionTarget &target, QListWidget *list)
{
    auto *item = new QListWidgetItem(
        target.displayName.isEmpty() ? target.connectionSpecifier(true) : target.displayName,
        list);
    item->setData(kTargetIdRole, target.id);
    item->setToolTip(target.connectionSpecifier(true));
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
    return item;
}

} // namespace

ProductivityWidget::ProductivityWidget(ProductivityModel *model, QWidget *parent)
    : QWidget(parent)
    , m_model(model)
{
    buildUi();
    setModel(model);
}

void ProductivityWidget::buildUi()
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("productivityTabs"));
    m_tabs->setDocumentMode(true);
    rootLayout->addWidget(m_tabs);

    // -------------------------------------------------------------- Snippets 标签
    auto *snippetPage = new QWidget(m_tabs);
    auto *snippetPageLayout = new QVBoxLayout(snippetPage);
    snippetPageLayout->setContentsMargins(0, 8, 0, 0);
    snippetPageLayout->setSpacing(8);

    auto *snippetToolbar = new QHBoxLayout;
    snippetToolbar->setSpacing(5);
    auto *newGroupButton = new QToolButton(snippetPage);
    newGroupButton->setObjectName(QStringLiteral("productivityNewGroup"));
    newGroupButton->setText(tr("新建分组"));
    auto *newSnippetButton = new QToolButton(snippetPage);
    newSnippetButton->setObjectName(QStringLiteral("productivityNewSnippet"));
    newSnippetButton->setText(tr("新建片段"));
    auto *removeButton = new QToolButton(snippetPage);
    removeButton->setObjectName(QStringLiteral("productivityRemoveSnippet"));
    removeButton->setText(tr("删除"));
    UiIcons::decorateButton(
        newGroupButton,
        UiIcons::symbol(this, UiIcons::Symbol::FolderAdd, this->palette().color(QPalette::WindowText)),
        tr("新建 Snippet 分组"));
    UiIcons::decorateButton(
        newSnippetButton,
        UiIcons::symbol(this, UiIcons::Symbol::Plus, this->palette().color(QPalette::WindowText)),
        tr("新建可执行命令片段"));
    UiIcons::decorateButton(
        removeButton,
        UiIcons::symbol(this, UiIcons::Symbol::Trash, this->palette().color(QPalette::WindowText)),
        tr("删除当前选中的分组或片段"));
    configureProductivityIconButton(newGroupButton, tr("新建 Snippet 分组"));
    configureProductivityIconButton(newSnippetButton, tr("新建可执行命令片段"));
    configureProductivityIconButton(removeButton, tr("删除当前选中的分组或片段"));
    snippetToolbar->addWidget(newGroupButton);
    snippetToolbar->addWidget(newSnippetButton);
    snippetToolbar->addStretch();
    snippetToolbar->addWidget(removeButton);
    snippetPageLayout->addLayout(snippetToolbar);

    auto *snippetSplitter = new QSplitter(Qt::Vertical, snippetPage);
    m_snippetTree = new QTreeWidget(snippetSplitter);
    m_snippetTree->setObjectName(QStringLiteral("productivitySnippetTree"));
    // 320px 侧栏放不下"名称/说明"两列表头；说明改由 SnippetItemDelegate 画成名称下方的
    // 第二行小字，单列就够，也不需要再显示表头。
    m_snippetTree->setHeaderHidden(true);
    m_snippetTree->setColumnCount(1);
    m_snippetTree->setItemDelegate(new SnippetItemDelegate(m_snippetTree));
    m_snippetTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_snippetTree->setContextMenuPolicy(Qt::CustomContextMenu);

    auto *editorBox = new QGroupBox(tr("片段编辑"), snippetSplitter);
    editorBox->setObjectName(QStringLiteral("productivitySnippetEditor"));
    auto *editorLayout = new QFormLayout(editorBox);
    editorLayout->setObjectName(QStringLiteral("productivitySnippetEditorLayout"));
    // 侧栏实际内容宽度只有约 284px。强制所有标签位于字段上方，可以给输入框保留完整
    // 宽度，也保证侧栏拖到最小宽度时不会出现标签挤压命令内容的情况。
    editorLayout->setRowWrapPolicy(QFormLayout::WrapAllRows);
    editorLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    editorLayout->setLabelAlignment(Qt::AlignLeft);
    editorLayout->setHorizontalSpacing(0);
    editorLayout->setVerticalSpacing(6);
    m_nameEdit = new QLineEdit(editorBox);
    m_parentGroupCombo = new QComboBox(editorBox);
    m_commandEdit = new QTextEdit(editorBox);
    m_commandEdit->setObjectName(QStringLiteral("productivityCommandEdit"));
    m_commandEdit->setAcceptRichText(false);
    m_commandEdit->setMaximumHeight(72);
    m_descriptionEdit = new QLineEdit(editorBox);
    m_saveButton = new QPushButton(tr("保存"), editorBox);
    m_saveButton->setProperty("primaryAction", true);
    auto *cancelEditorButton = new QPushButton(tr("取消"), editorBox);
    cancelEditorButton->setObjectName(QStringLiteral("productivityCancelEditor"));
    UiIcons::decorateButton(
        m_saveButton,
        UiIcons::symbol(this, UiIcons::Symbol::Download, this->palette().color(QPalette::WindowText)),
        tr("保存当前片段或分组修改"));
    editorLayout->addRow(tr("名称"), m_nameEdit);
    editorLayout->addRow(tr("所属分组"), m_parentGroupCombo);
    editorLayout->addRow(tr("命令"), m_commandEdit);
    editorLayout->addRow(tr("说明"), m_descriptionEdit);
    auto *editorActions = new QHBoxLayout;
    editorActions->setContentsMargins(0, 0, 0, 0);
    editorActions->setSpacing(8);
    editorActions->addStretch(1);
    editorActions->addWidget(cancelEditorButton);
    editorActions->addWidget(m_saveButton);
    editorLayout->addRow(editorActions);
    snippetSplitter->addWidget(m_snippetTree);
    snippetSplitter->addWidget(editorBox);
    snippetSplitter->setStretchFactor(0, 2);
    snippetSplitter->setStretchFactor(1, 1);
    snippetPageLayout->addWidget(snippetSplitter, 1);

    auto *executionBox = new QGroupBox(tr("执行目标（可多选；不选表示当前终端）"), snippetPage);
    auto *executionLayout = new QVBoxLayout(executionBox);
    m_executionTargets = new QListWidget(executionBox);
    m_executionTargets->setObjectName(QStringLiteral("productivityExecutionTargets"));
    m_executionTargets->setMaximumHeight(105);
    auto *executeButton = new QPushButton(tr("执行选中片段"), executionBox);
    executeButton->setProperty("primaryAction", true);
    UiIcons::decorateButton(
        executeButton,
        UiIcons::symbol(this, UiIcons::Symbol::Play, this->palette().color(QPalette::WindowText)),
        tr("在明确选择的目标终端执行片段"));
    executeButton->setObjectName(QStringLiteral("productivityExecuteSnippet"));
    executionLayout->addWidget(m_executionTargets);
    executionLayout->addWidget(executeButton);
    snippetPageLayout->addWidget(executionBox);
    m_tabs->addTab(snippetPage, tr("命令片段"));

    connect(newGroupButton, &QToolButton::clicked, this,
            [this]() { beginNewRecord(SnippetNodeKind::Folder); });
    connect(newSnippetButton, &QToolButton::clicked, this,
            [this]() { beginNewRecord(SnippetNodeKind::Snippet); });
    connect(removeButton, &QToolButton::clicked,
            this, &ProductivityWidget::removeSelectedRecord);
    connect(m_saveButton, &QPushButton::clicked,
            this, &ProductivityWidget::saveEditorRecord);
    connect(cancelEditorButton, &QPushButton::clicked, this, [this]() {
        // “取消”不改变数据库，只丢弃当前表单中的临时输入。若树中已有选择，则恢复该记录
        // 的持久化快照；空树场景下清理字段，避免看起来像已经保存了一条空记录。
        if (QTreeWidgetItem *selected = m_snippetTree->currentItem(); selected != nullptr) {
            editTreeItem(selected);
            return;
        }
        m_editingId.clear();
        m_nameEdit->clear();
        m_parentGroupCombo->setCurrentIndex(0);
        m_commandEdit->clear();
        m_descriptionEdit->clear();
        m_saveButton->setText(tr("保存"));
    });
    connect(executeButton, &QPushButton::clicked,
            this, &ProductivityWidget::executeSelectedSnippet);
    connect(m_snippetTree, &QTreeWidget::itemSelectionChanged, this, [this]() {
        editTreeItem(m_snippetTree->currentItem());
    });
    connect(m_snippetTree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *, int) { executeSelectedSnippet(); });
    connect(m_snippetTree,
            &QWidget::customContextMenuRequested,
            this,
            [this](const QPoint &position) {
                QTreeWidgetItem *item = m_snippetTree->itemAt(position);
                if (item != nullptr) {
                    m_snippetTree->setCurrentItem(item);
                }
                QMenu menu(this);
                UiIcons::modernizePopup(&menu);
                QAction *newGroupAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::FolderAdd, this->palette().color(QPalette::WindowText)),
                    tr("新建分组"));
                QAction *newSnippetAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Plus, this->palette().color(QPalette::WindowText)),
                    tr("新建片段"));
                connect(newGroupAction, &QAction::triggered,
                        this, [this]() { beginNewRecord(SnippetNodeKind::Folder); });
                connect(newSnippetAction, &QAction::triggered,
                        this, [this]() { beginNewRecord(SnippetNodeKind::Snippet); });
                if (item != nullptr) {
                    menu.addSeparator();
                    const bool isSnippet = static_cast<SnippetNodeKind>(
                                               item->data(0, kKindRole).toInt())
                        == SnippetNodeKind::Snippet;
                    if (isSnippet) {
                        QAction *executeAction = menu.addAction(
                            UiIcons::symbol(this, UiIcons::Symbol::Play, this->palette().color(QPalette::WindowText)),
                            tr("执行"));
                        connect(executeAction,
                                &QAction::triggered,
                                this,
                                &ProductivityWidget::executeSelectedSnippet);
                    }
                    QAction *editAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Edit, this->palette().color(QPalette::WindowText)),
                        tr("编辑"));
                    QAction *deleteAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Trash, this->palette().color(QPalette::WindowText)),
                        tr("删除"));
                    connect(editAction, &QAction::triggered, this, [this, item]() {
                        editTreeItem(item);
                        m_nameEdit->setFocus();
                    });
                    connect(deleteAction,
                            &QAction::triggered,
                            this,
                            &ProductivityWidget::removeSelectedRecord);
                }
                menu.exec(m_snippetTree->viewport()->mapToGlobal(position));
            });

    // --------------------------------------------------------------- 历史标签
    auto *historyPage = new QWidget(m_tabs);
    historyPage->setObjectName(QStringLiteral("productivityHistoryPage"));
    auto *historyLayout = new QVBoxLayout(historyPage);
    historyLayout->setContentsMargins(0, 8, 0, 0);
    historyLayout->setSpacing(8);
    m_historyHostCombo = new QComboBox(historyPage);
    m_historyHostCombo->setObjectName(QStringLiteral("productivityHistoryHost"));
    m_historyPrefixEdit = new QLineEdit(historyPage);
    m_historyPrefixEdit->setObjectName(QStringLiteral("productivityHistoryPrefix"));
    m_historyPrefixEdit->setPlaceholderText(tr("按命令前缀筛选"));
    // 主机与前缀筛选在窄侧栏内上下堆叠；并排布局会让两个输入框都只剩百余像素，主机名
    // 和长前缀同时被截断。
    historyLayout->addWidget(m_historyHostCombo);
    historyLayout->addWidget(m_historyPrefixEdit);
    m_historyList = new QListWidget(historyPage);
    m_historyList->setContextMenuPolicy(Qt::CustomContextMenu);
    historyLayout->addWidget(m_historyList, 1);
    auto *historyExecuteButton = new QPushButton(tr("在所选主机执行"), historyPage);
    historyExecuteButton->setObjectName(QStringLiteral("productivityHistoryExecute"));
    historyExecuteButton->setProperty("primaryAction", true);
    UiIcons::decorateButton(
        historyExecuteButton,
        UiIcons::symbol(this, UiIcons::Symbol::Play, this->palette().color(QPalette::WindowText)),
        tr("在历史筛选器选中的主机上重新执行命令"));
    historyLayout->addWidget(historyExecuteButton);
    m_tabs->addTab(historyPage, tr("命令历史"));

    connect(m_historyHostCombo, &QComboBox::currentIndexChanged,
            this, &ProductivityWidget::reloadHistory);
    connect(m_historyPrefixEdit, &QLineEdit::textChanged,
            this, &ProductivityWidget::reloadHistory);
    connect(historyExecuteButton, &QPushButton::clicked, this, [this]() {
        const QListWidgetItem *item = m_historyList->currentItem();
        if (item == nullptr) {
            emit statusMessage(tr("请先选择一条命令历史"));
            return;
        }
        const QString hostId = m_historyHostCombo->currentData().toString();
        emit commandExecutionRequested(item->data(Qt::UserRole).toString(),
                                       hostId.isEmpty() ? QStringList{} : QStringList{hostId});
    });
    connect(m_historyList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *) {
                const QListWidgetItem *item = m_historyList->currentItem();
                if (item == nullptr) {
                    return;
                }
                const QString hostId = m_historyHostCombo->currentData().toString();
                emit commandExecutionRequested(item->data(Qt::UserRole).toString(),
                                               hostId.isEmpty() ? QStringList{}
                                                                : QStringList{hostId});
            });
    connect(m_historyList,
            &QWidget::customContextMenuRequested,
            this,
            [this](const QPoint &position) {
                QListWidgetItem *item = m_historyList->itemAt(position);
                if (item == nullptr) {
                    return;
                }
                m_historyList->setCurrentItem(item);
                const QString command = item->data(Qt::UserRole).toString();
                QMenu menu(this);
                UiIcons::modernizePopup(&menu);
                QAction *executeAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Play, this->palette().color(QPalette::WindowText)),
                    tr("执行"));
                QAction *copyAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Copy, this->palette().color(QPalette::WindowText)),
                    tr("复制命令"));
                connect(executeAction, &QAction::triggered, this, [this, command]() {
                    const QString hostId = m_historyHostCombo->currentData().toString();
                    emit commandExecutionRequested(
                        command, hostId.isEmpty() ? QStringList{} : QStringList{hostId});
                });
                connect(copyAction, &QAction::triggered, this, [command]() {
                    if (QApplication::clipboard() != nullptr) {
                        QApplication::clipboard()->setText(command);
                    }
                });
                menu.exec(m_historyList->viewport()->mapToGlobal(position));
            });

    // --------------------------------------------------------------- 广播标签
    auto *broadcastPage = new QWidget(m_tabs);
    broadcastPage->setObjectName(QStringLiteral("productivityBroadcastPage"));
    auto *broadcastLayout = new QVBoxLayout(broadcastPage);
    broadcastLayout->setContentsMargins(0, 8, 0, 0);
    broadcastLayout->setSpacing(8);
    broadcastLayout->addWidget(new QLabel(
        tr("选择目标后可开启实时键盘广播；源终端由主窗口绑定为当前标签页。"), broadcastPage));
    m_broadcastTargets = new QListWidget(broadcastPage);
    m_broadcastTargets->setObjectName(QStringLiteral("productivityBroadcastTargets"));
    broadcastLayout->addWidget(m_broadcastTargets, 1);
    m_broadcastEnabled = new QCheckBox(tr("开启实时原始输入广播"), broadcastPage);
    m_broadcastEnabled->setObjectName(QStringLiteral("productivityBroadcastEnabled"));
    broadcastLayout->addWidget(m_broadcastEnabled);
    m_broadcastInput = new QLineEdit(broadcastPage);
    m_broadcastInput->setObjectName(QStringLiteral("productivityBroadcastInput"));
    m_broadcastInput->setPlaceholderText(tr("一次性向选中目标发送命令"));
    auto *broadcastSendButton = new QPushButton(tr("广播发送"), broadcastPage);
    broadcastSendButton->setProperty("primaryAction", true);
    UiIcons::decorateButton(
        broadcastSendButton,
        UiIcons::symbol(this, UiIcons::Symbol::Send, this->palette().color(QPalette::WindowText)),
        tr("向明确勾选的多个终端发送一次命令"));
    broadcastSendButton->setObjectName(QStringLiteral("productivityBroadcastSend"));
    // 一次性命令输入与发送按钮同样采用单列通栏布局，防止按钮文字挤占命令输入空间。
    broadcastLayout->addWidget(m_broadcastInput);
    broadcastLayout->addWidget(broadcastSendButton);
    m_tabs->addTab(broadcastPage, tr("广播输入"));

    connect(m_broadcastEnabled, &QCheckBox::toggled, this, [this](const bool enabled) {
        const QStringList ids = checkedTargetIds(m_broadcastTargets);
        if (enabled && ids.isEmpty()) {
            // 不自动选择全部目标：广播命令具有破坏性，必须由用户明确勾选接收方。
            m_broadcastEnabled->setChecked(false);
            emit statusMessage(tr("开启广播前请明确勾选至少一个目标终端"));
            return;
        }
        emit broadcastModeChanged(enabled, ids);
    });
    connect(m_broadcastTargets, &QListWidget::itemChanged, this,
            [this](QListWidgetItem *) {
                if (m_broadcastEnabled->isChecked()) {
                    emit broadcastModeChanged(true, checkedTargetIds(m_broadcastTargets));
                }
            });
    connect(broadcastSendButton, &QPushButton::clicked, this, [this]() {
        const QString command = m_broadcastInput->text();
        const QStringList ids = checkedTargetIds(m_broadcastTargets);
        if (command.trimmed().isEmpty() || ids.isEmpty()) {
            emit statusMessage(tr("广播发送需要非空命令和至少一个明确目标"));
            return;
        }
        emit broadcastInputRequested(command.toUtf8() + QByteArrayLiteral("\r"), ids);
        m_broadcastInput->clear();
    });
    connect(m_broadcastInput, &QLineEdit::returnPressed,
            broadcastSendButton, &QPushButton::click);
}

void ProductivityWidget::setModel(ProductivityModel *model)
{
    if (m_model != nullptr) {
        disconnect(m_model, nullptr, this, nullptr);
    }
    m_model = model;
    if (m_model != nullptr) {
        connect(m_model, &ProductivityModel::snippetsChanged,
                this, &ProductivityWidget::reloadSnippets);
        connect(m_model, &ProductivityModel::commandHistoryChanged,
                this, [this](const QString &hostId) {
                    if (m_historyHostCombo->currentData().toString() == hostId) {
                        reloadHistory();
                    }
                });
        connect(m_model, &ProductivityModel::errorOccurred,
                this, &ProductivityWidget::statusMessage);
    }
    refresh();
}

void ProductivityWidget::setExecutionTargets(const QList<ConnectionTarget> &targets)
{
    m_targets = targets;
    populateTargetSelectors();
}

void ProductivityWidget::refresh()
{
    reloadSnippets();
    reloadHistory();
}

void ProductivityWidget::reloadSnippets()
{
    m_snippetTree->clear();
    m_parentGroupCombo->clear();
    m_parentGroupCombo->addItem(tr("根分组"), QString());
    if (m_model == nullptr) {
        return;
    }

    QSet<QString> visited;
    if (!appendSnippetChildren(nullptr, QString(), &visited)) {
        return;
    }
    m_snippetTree->expandAll();
}

bool ProductivityWidget::appendSnippetChildren(QTreeWidgetItem *parentItem,
                                               const QString &parentId,
                                               QSet<QString> *visited)
{
    bool ok = false;
    const QList<SnippetRecord> records = m_model->snippetsInGroup(parentId, &ok);
    if (!ok) {
        return false;
    }
    for (const SnippetRecord &record : records) {
        if (visited->contains(record.id)) {
            emit statusMessage(tr("检测到 Snippet 树循环引用，已停止展开节点：%1").arg(record.name));
            continue;
        }
        visited->insert(record.id);
        auto *item = new QTreeWidgetItem;
        item->setText(0, record.name);
        item->setIcon(
            0,
            record.kind == SnippetNodeKind::Folder
                ? UiIcons::symbol(m_snippetTree, UiIcons::Symbol::Folder, m_snippetTree->palette().color(QPalette::WindowText))
                : UiIcons::symbol(m_snippetTree, UiIcons::Symbol::File, m_snippetTree->palette().color(QPalette::WindowText)));
        if (record.kind == SnippetNodeKind::Snippet) {
            item->setData(0,
                         kPreviewRole,
                         record.description.isEmpty() ? record.command : record.description);
        }
        item->setData(0, kIdRole, record.id);
        item->setData(0, kKindRole, static_cast<int>(record.kind));
        if (parentItem == nullptr) {
            m_snippetTree->addTopLevelItem(item);
        } else {
            parentItem->addChild(item);
        }
        if (record.kind == SnippetNodeKind::Folder) {
            m_parentGroupCombo->addItem(record.name, record.id);
            if (!appendSnippetChildren(item, record.id, visited)) {
                return false;
            }
        }
        visited->remove(record.id);
    }
    return true;
}

void ProductivityWidget::editTreeItem(QTreeWidgetItem *item)
{
    if (item == nullptr || m_model == nullptr) {
        return;
    }
    const QString id = item->data(0, kIdRole).toString();
    const std::optional<SnippetRecord> record = m_model->snippet(id);
    if (!record.has_value()) {
        emit statusMessage(tr("选中的 Snippet 已不存在，请刷新列表"));
        return;
    }
    m_editingId = record->id;
    m_editingKind = record->kind;
    m_nameEdit->setText(record->name);
    const int parentIndex = m_parentGroupCombo->findData(record->parentId);
    m_parentGroupCombo->setCurrentIndex(parentIndex >= 0 ? parentIndex : 0);
    m_commandEdit->setPlainText(record->command);
    m_commandEdit->setEnabled(record->kind == SnippetNodeKind::Snippet);
    m_descriptionEdit->setText(record->description);
    m_saveButton->setText(tr("保存修改"));
}

void ProductivityWidget::beginNewRecord(const SnippetNodeKind kind)
{
    m_editingId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_editingKind = kind;
    m_nameEdit->clear();
    m_commandEdit->clear();
    m_commandEdit->setEnabled(kind == SnippetNodeKind::Snippet);
    m_descriptionEdit->clear();

    // 新节点默认放在当前分组；若选中的是片段，则放到该片段的父分组。这样连续录入同组
    // 命令无需反复选择父级，同时不会把新节点错误挂到叶子下面。
    QString parentId;
    if (QTreeWidgetItem *selected = m_snippetTree->currentItem(); selected != nullptr) {
        const SnippetNodeKind selectedKind = static_cast<SnippetNodeKind>(
            selected->data(0, kKindRole).toInt());
        if (selectedKind == SnippetNodeKind::Folder) {
            parentId = selected->data(0, kIdRole).toString();
        } else if (selected->parent() != nullptr) {
            parentId = selected->parent()->data(0, kIdRole).toString();
        }
    }
    const int parentIndex = m_parentGroupCombo->findData(parentId);
    m_parentGroupCombo->setCurrentIndex(parentIndex >= 0 ? parentIndex : 0);
    m_saveButton->setText(kind == SnippetNodeKind::Folder ? tr("创建分组") : tr("创建片段"));
    m_nameEdit->setFocus();
}

void ProductivityWidget::saveEditorRecord()
{
    if (m_model == nullptr) {
        emit statusMessage(tr("效率工具模型尚未就绪"));
        return;
    }
    if (m_editingId.isEmpty()) {
        beginNewRecord(SnippetNodeKind::Snippet);
    }

    SnippetRecord record;
    record.id = m_editingId;
    record.parentId = m_parentGroupCombo->currentData().toString();
    record.kind = m_editingKind;
    record.name = m_nameEdit->text();
    record.command = m_editingKind == SnippetNodeKind::Snippet
        ? m_commandEdit->toPlainText()
        : QString();
    record.description = m_descriptionEdit->text();
    if (m_model->saveSnippet(record)) {
        emit statusMessage(record.kind == SnippetNodeKind::Folder
                               ? tr("Snippet 分组已保存")
                               : tr("Snippet 已保存"));
    }
}

void ProductivityWidget::removeSelectedRecord()
{
    QTreeWidgetItem *item = m_snippetTree->currentItem();
    if (item == nullptr || m_model == nullptr) {
        emit statusMessage(tr("请先选择要删除的 Snippet 或分组"));
        return;
    }
    const SnippetNodeKind kind = static_cast<SnippetNodeKind>(item->data(0, kKindRole).toInt());
    const QString warning = kind == SnippetNodeKind::Folder
        ? tr("删除分组会同时删除其中全部子分组和片段，确定继续吗？")
        : tr("确定删除选中的 Snippet 吗？");
    if (QMessageBox::warning(this,
                             tr("确认删除"),
                             warning,
                             QMessageBox::Yes | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    int removed = 0;
    if (m_model->removeSnippet(item->data(0, kIdRole).toString(), &removed)) {
        m_editingId.clear();
        emit statusMessage(tr("已删除 %1 个 Snippet 树节点").arg(removed));
    }
}

void ProductivityWidget::executeSelectedSnippet()
{
    QTreeWidgetItem *item = m_snippetTree->currentItem();
    if (item == nullptr || m_model == nullptr) {
        emit statusMessage(tr("请先选择要执行的 Snippet"));
        return;
    }
    if (static_cast<SnippetNodeKind>(item->data(0, kKindRole).toInt())
        != SnippetNodeKind::Snippet) {
        emit statusMessage(tr("分组不可执行，请选择其中的具体 Snippet"));
        return;
    }
    const std::optional<SnippetRecord> record = m_model->snippet(
        item->data(0, kIdRole).toString());
    if (!record.has_value() || record->command.trimmed().isEmpty()) {
        emit statusMessage(tr("Snippet 已不存在或命令为空"));
        return;
    }
    emit commandExecutionRequested(record->command, checkedTargetIds(m_executionTargets));
}

void ProductivityWidget::reloadHistory()
{
    m_historyList->clear();
    if (m_model == nullptr || m_historyHostCombo->count() == 0) {
        return;
    }
    const QString hostId = m_historyHostCombo->currentData().toString();
    bool ok = false;
    const QList<CommandHistoryEntry> history = m_model->recentHistory(hostId, 500, &ok);
    if (!ok) {
        return;
    }
    const QString prefix = m_historyPrefixEdit->text();
    QSet<QString> shown;
    for (const CommandHistoryEntry &entry : history) {
        if (!entry.command.startsWith(prefix, Qt::CaseInsensitive) || shown.contains(entry.command)) {
            continue;
        }
        shown.insert(entry.command);
        auto *item = new QListWidgetItem(entry.command, m_historyList);
        item->setData(Qt::UserRole, entry.command);
        // Qt 6.8 已移除旧的 Qt::DefaultLocaleShortDate 枚举；显式使用系统区域设置可继续
        // 尊重用户日期格式，同时避免把本地化字符串写死在业务模型中。
        item->setToolTip(QLocale().toString(entry.executedAt.toLocalTime(), QLocale::ShortFormat));
    }
}

QStringList ProductivityWidget::checkedTargetIds(const QListWidget *list)
{
    QStringList ids;
    QSet<QString> seen;
    for (int index = 0; index < list->count(); ++index) {
        const QListWidgetItem *item = list->item(index);
        const QString id = item->data(kTargetIdRole).toString();
        if (item->checkState() == Qt::Checked && !id.isEmpty() && !seen.contains(id)) {
            seen.insert(id);
            ids.append(id);
        }
    }
    return ids;
}

void ProductivityWidget::populateTargetSelectors()
{
    const QString previousHistoryId = m_historyHostCombo->currentData().toString();
    const QStringList previousExecutionIds = checkedTargetIds(m_executionTargets);
    const QStringList previousBroadcastIds = checkedTargetIds(m_broadcastTargets);

    m_executionTargets->clear();
    m_broadcastTargets->clear();
    m_historyHostCombo->clear();
    m_historyHostCombo->addItem(tr("本地 / 当前终端"), QString());

    QSet<QString> seen;
    for (const ConnectionTarget &target : std::as_const(m_targets)) {
        if (!target.isValid() || target.id.isEmpty() || seen.contains(target.id)) {
            continue;
        }
        seen.insert(target.id);
        QListWidgetItem *executionItem = createTargetItem(target, m_executionTargets);
        QListWidgetItem *broadcastItem = createTargetItem(target, m_broadcastTargets);
        executionItem->setCheckState(previousExecutionIds.contains(target.id)
                                         ? Qt::Checked : Qt::Unchecked);
        broadcastItem->setCheckState(previousBroadcastIds.contains(target.id)
                                         ? Qt::Checked : Qt::Unchecked);
        m_historyHostCombo->addItem(
            target.displayName.isEmpty() ? target.connectionSpecifier(true) : target.displayName,
            target.id);
    }
    const int historyIndex = m_historyHostCombo->findData(previousHistoryId);
    m_historyHostCombo->setCurrentIndex(historyIndex >= 0 ? historyIndex : 0);

    // 目标列表更新可能使已启用广播的目标消失。立即同步新集合，不能继续向已关闭标签页
    // 持有的旧指针发送输入；上层收到空集合后应安全关闭广播。
    if (m_broadcastEnabled->isChecked()) {
        const QStringList ids = checkedTargetIds(m_broadcastTargets);
        if (ids.isEmpty()) {
            m_broadcastEnabled->setChecked(false);
        } else {
            emit broadcastModeChanged(true, ids);
        }
    }
    reloadHistory();
}
