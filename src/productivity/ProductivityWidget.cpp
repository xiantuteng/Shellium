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
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QSet>
#include <QSplitter>
#include <QTabWidget>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUuid>
#include <QVBoxLayout>

namespace {

/** @brief QTreeWidgetItem::UserRole 中保存的稳定节点 id。 */
constexpr int kIdRole = Qt::UserRole;

/** @brief QTreeWidgetItem::UserRole+1 中保存的 SnippetNodeKind 整数值。 */
constexpr int kKindRole = Qt::UserRole + 1;

/** @brief 目标列表项 UserRole 中保存的持久化主机 id。 */
constexpr int kTargetIdRole = Qt::UserRole;

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
    rootLayout->setContentsMargins(4, 4, 4, 4);
    m_tabs = new QTabWidget(this);
    rootLayout->addWidget(m_tabs);

    // -------------------------------------------------------------- Snippets 标签
    auto *snippetPage = new QWidget(m_tabs);
    auto *snippetPageLayout = new QVBoxLayout(snippetPage);
    snippetPageLayout->setContentsMargins(2, 2, 2, 2);

    auto *snippetToolbar = new QHBoxLayout;
    auto *newGroupButton = new QPushButton(tr("新建分组"), snippetPage);
    auto *newSnippetButton = new QPushButton(tr("新建片段"), snippetPage);
    auto *removeButton = new QPushButton(tr("删除"), snippetPage);
    UiIcons::decorateButton(
        newGroupButton,
        UiIcons::themed(this, QStringLiteral("folder-new"), QStyle::SP_FileDialogNewFolder),
        tr("新建 Snippet 分组"));
    UiIcons::decorateButton(
        newSnippetButton,
        UiIcons::themed(this, QStringLiteral("list-add"), QStyle::SP_FileIcon),
        tr("新建可执行命令片段"));
    UiIcons::decorateButton(
        removeButton,
        UiIcons::themed(this, QStringLiteral("edit-delete"), QStyle::SP_TrashIcon),
        tr("删除当前选中的分组或片段"));
    snippetToolbar->addWidget(newGroupButton);
    snippetToolbar->addWidget(newSnippetButton);
    snippetToolbar->addWidget(removeButton);
    snippetToolbar->addStretch();
    snippetPageLayout->addLayout(snippetToolbar);

    auto *snippetSplitter = new QSplitter(Qt::Vertical, snippetPage);
    m_snippetTree = new QTreeWidget(snippetSplitter);
    m_snippetTree->setObjectName(QStringLiteral("productivitySnippetTree"));
    m_snippetTree->setHeaderLabels({tr("名称"), tr("说明")});
    m_snippetTree->header()->setStretchLastSection(true);
    m_snippetTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_snippetTree->setContextMenuPolicy(Qt::CustomContextMenu);

    auto *editorBox = new QGroupBox(tr("片段编辑"), snippetSplitter);
    auto *editorLayout = new QFormLayout(editorBox);
    m_nameEdit = new QLineEdit(editorBox);
    m_parentGroupCombo = new QComboBox(editorBox);
    m_commandEdit = new QLineEdit(editorBox);
    m_descriptionEdit = new QTextEdit(editorBox);
    m_descriptionEdit->setMaximumHeight(72);
    m_saveButton = new QPushButton(tr("保存"), editorBox);
    UiIcons::decorateButton(
        m_saveButton,
        UiIcons::themed(this, QStringLiteral("document-save"), QStyle::SP_DialogSaveButton),
        tr("保存当前片段或分组修改"));
    editorLayout->addRow(tr("名称"), m_nameEdit);
    editorLayout->addRow(tr("所属分组"), m_parentGroupCombo);
    editorLayout->addRow(tr("命令"), m_commandEdit);
    editorLayout->addRow(tr("说明"), m_descriptionEdit);
    editorLayout->addRow(QString(), m_saveButton);
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
    UiIcons::decorateButton(
        executeButton,
        UiIcons::themed(this, QStringLiteral("media-playback-start"), QStyle::SP_MediaPlay),
        tr("在明确选择的目标终端执行片段"));
    executeButton->setObjectName(QStringLiteral("productivityExecuteSnippet"));
    executionLayout->addWidget(m_executionTargets);
    executionLayout->addWidget(executeButton);
    snippetPageLayout->addWidget(executionBox);
    m_tabs->addTab(snippetPage, tr("Snippets"));

    connect(newGroupButton, &QPushButton::clicked, this,
            [this]() { beginNewRecord(SnippetNodeKind::Folder); });
    connect(newSnippetButton, &QPushButton::clicked, this,
            [this]() { beginNewRecord(SnippetNodeKind::Snippet); });
    connect(removeButton, &QPushButton::clicked,
            this, &ProductivityWidget::removeSelectedRecord);
    connect(m_saveButton, &QPushButton::clicked,
            this, &ProductivityWidget::saveEditorRecord);
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
                QAction *newGroupAction = menu.addAction(
                    UiIcons::themed(this, QStringLiteral("folder-new"),
                                    QStyle::SP_FileDialogNewFolder),
                    tr("新建分组"));
                QAction *newSnippetAction = menu.addAction(
                    UiIcons::themed(this, QStringLiteral("list-add"), QStyle::SP_FileIcon),
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
                            UiIcons::themed(this,
                                            QStringLiteral("media-playback-start"),
                                            QStyle::SP_MediaPlay),
                            tr("执行"));
                        connect(executeAction,
                                &QAction::triggered,
                                this,
                                &ProductivityWidget::executeSelectedSnippet);
                    }
                    QAction *editAction = menu.addAction(
                        UiIcons::themed(this, QStringLiteral("document-edit"),
                                        QStyle::SP_FileDialogDetailedView),
                        tr("编辑"));
                    QAction *deleteAction = menu.addAction(
                        UiIcons::themed(this, QStringLiteral("edit-delete"), QStyle::SP_TrashIcon),
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
    auto *historyLayout = new QVBoxLayout(historyPage);
    auto *historyFilterLayout = new QHBoxLayout;
    m_historyHostCombo = new QComboBox(historyPage);
    m_historyPrefixEdit = new QLineEdit(historyPage);
    m_historyPrefixEdit->setPlaceholderText(tr("按命令前缀筛选"));
    historyFilterLayout->addWidget(new QLabel(tr("主机"), historyPage));
    historyFilterLayout->addWidget(m_historyHostCombo, 1);
    historyFilterLayout->addWidget(m_historyPrefixEdit, 2);
    historyLayout->addLayout(historyFilterLayout);
    m_historyList = new QListWidget(historyPage);
    m_historyList->setContextMenuPolicy(Qt::CustomContextMenu);
    historyLayout->addWidget(m_historyList, 1);
    auto *historyExecuteButton = new QPushButton(tr("在所选主机执行"), historyPage);
    UiIcons::decorateButton(
        historyExecuteButton,
        UiIcons::themed(this, QStringLiteral("media-playback-start"), QStyle::SP_MediaPlay),
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
                QAction *executeAction = menu.addAction(
                    UiIcons::themed(this, QStringLiteral("media-playback-start"),
                                    QStyle::SP_MediaPlay),
                    tr("执行"));
                QAction *copyAction = menu.addAction(
                    UiIcons::themed(this, QStringLiteral("edit-copy"), QStyle::SP_FileIcon),
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
    auto *broadcastLayout = new QVBoxLayout(broadcastPage);
    broadcastLayout->addWidget(new QLabel(
        tr("选择目标后可开启实时键盘广播；源终端由主窗口绑定为当前标签页。"), broadcastPage));
    m_broadcastTargets = new QListWidget(broadcastPage);
    m_broadcastTargets->setObjectName(QStringLiteral("productivityBroadcastTargets"));
    broadcastLayout->addWidget(m_broadcastTargets, 1);
    m_broadcastEnabled = new QCheckBox(tr("开启实时原始输入广播"), broadcastPage);
    m_broadcastEnabled->setObjectName(QStringLiteral("productivityBroadcastEnabled"));
    broadcastLayout->addWidget(m_broadcastEnabled);
    auto *broadcastInputLayout = new QHBoxLayout;
    m_broadcastInput = new QLineEdit(broadcastPage);
    m_broadcastInput->setObjectName(QStringLiteral("productivityBroadcastInput"));
    m_broadcastInput->setPlaceholderText(tr("一次性向选中目标发送命令"));
    auto *broadcastSendButton = new QPushButton(tr("广播发送"), broadcastPage);
    UiIcons::decorateButton(
        broadcastSendButton,
        UiIcons::themed(this, QStringLiteral("mail-send"), QStyle::SP_ArrowForward),
        tr("向明确勾选的多个终端发送一次命令"));
    broadcastSendButton->setObjectName(QStringLiteral("productivityBroadcastSend"));
    broadcastInputLayout->addWidget(m_broadcastInput, 1);
    broadcastInputLayout->addWidget(broadcastSendButton);
    broadcastLayout->addLayout(broadcastInputLayout);
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
                ? UiIcons::themed(m_snippetTree, QStringLiteral("folder"), QStyle::SP_DirIcon)
                : UiIcons::themed(m_snippetTree, QStringLiteral("text-x-script"),
                                  QStyle::SP_FileIcon));
        item->setText(1, record.description);
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
    m_commandEdit->setText(record->command);
    m_commandEdit->setEnabled(record->kind == SnippetNodeKind::Snippet);
    m_descriptionEdit->setPlainText(record->description);
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
    record.command = m_editingKind == SnippetNodeKind::Snippet ? m_commandEdit->text() : QString();
    record.description = m_descriptionEdit->toPlainText();
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
