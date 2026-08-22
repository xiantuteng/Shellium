#include "productivity/ProductivityWidget.h"

#include "productivity/ProductivityModel.h"
#include "ui/UiIcons.h"

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QShortcut>
#include <QTabWidget>
#include <QTextEdit>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace {

/** @brief QListWidgetItem::UserRole 中保存的稳定 Snippet id。 */
constexpr int kIdRole = Qt::UserRole;

/** @brief 目标列表项 UserRole 中保存的持久化主机 id。 */
constexpr int kTargetIdRole = Qt::UserRole;

/** @brief 最多提供数字快捷键的片段数量：Ctrl+1..Ctrl+9、Ctrl+0。 */
constexpr int kMaxShortcutSnippets = 10;

/**
 * @brief 将效率侧栏的次要命令配置为紧凑图标按钮。
 * @param button 待配置按钮；不得为空。
 * @param accessibleName 动作完整中文名称，同时用于 Tooltip 和无障碍树。
 *
 * 树管理工具栏动作使用图标，完整语义通过 Tooltip 与无障碍名称保留；保存、执行等
 * 内容级主流程继续使用文字按钮。
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
 * @brief 创建一条可复选的目标列表项，供历史筛选和广播使用。
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

/**
 * @brief 把标签文字调淡，用于列表行的次要说明和快捷键提示。
 */
void applyMutedPalette(QWidget *widget, const qreal alpha)
{
    QColor color = widget->palette().color(QPalette::WindowText);
    color.setAlphaF(alpha);
    QPalette palette = widget->palette();
    palette.setColor(QPalette::WindowText, color);
    widget->setPalette(palette);
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
    auto *newSnippetButton = new QToolButton(snippetPage);
    newSnippetButton->setObjectName(QStringLiteral("productivityNewSnippet"));
    newSnippetButton->setText(tr("新建片段"));
    auto *removeButton = new QToolButton(snippetPage);
    removeButton->setObjectName(QStringLiteral("productivityRemoveSnippet"));
    removeButton->setText(tr("删除"));
    UiIcons::decorateButton(
        newSnippetButton,
        UiIcons::symbol(this, UiIcons::Symbol::Plus, this->palette().color(QPalette::WindowText)),
        tr("新建可执行命令片段"));
    UiIcons::decorateButton(
        removeButton,
        UiIcons::symbol(this, UiIcons::Symbol::Trash, this->palette().color(QPalette::WindowText)),
        tr("删除当前选中的片段"));
    configureProductivityIconButton(newSnippetButton, tr("新建可执行命令片段"));
    configureProductivityIconButton(removeButton, tr("删除当前选中的片段"));
    snippetToolbar->addWidget(newSnippetButton);
    snippetToolbar->addStretch();
    snippetToolbar->addWidget(removeButton);
    snippetPageLayout->addLayout(snippetToolbar);

    m_snippetList = new QListWidget(snippetPage);
    m_snippetList->setObjectName(QStringLiteral("productivitySnippetList"));
    m_snippetList->setSelectionMode(QAbstractItemView::SingleSelection);
    // 分组功能已移除，列表始终是根层级的扁平序列；InternalMove 让用户可以直接拖拽调整
    // sortOrder，不再需要单独的"上移/下移"按钮。
    m_snippetList->setDragDropMode(QAbstractItemView::InternalMove);
    m_snippetList->setDefaultDropAction(Qt::MoveAction);
    m_snippetList->setContextMenuPolicy(Qt::CustomContextMenu);
    snippetPageLayout->addWidget(m_snippetList, 1);
    m_tabs->addTab(snippetPage, tr("命令片段"));

    connect(newSnippetButton, &QToolButton::clicked,
            this, [this]() { openSnippetEditor(QString()); });
    connect(removeButton, &QToolButton::clicked,
            this, &ProductivityWidget::removeSelectedRecord);
    connect(m_snippetList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) {
                if (item != nullptr) {
                    openSnippetEditor(item->data(kIdRole).toString());
                }
            });
    // 拖拽结束后 QListWidget 的内部模型会发出 rowsMoved；据此把新的可见顺序回写为
    // sortOrder，而不是尝试在拖拽过程中追踪每一次中间态。
    connect(m_snippetList->model(), &QAbstractItemModel::rowsMoved,
            this, &ProductivityWidget::persistSnippetOrder);
    connect(m_snippetList,
            &QWidget::customContextMenuRequested,
            this,
            [this](const QPoint &position) {
                QListWidgetItem *item = m_snippetList->itemAt(position);
                if (item != nullptr) {
                    m_snippetList->setCurrentItem(item);
                }
                QMenu menu(this);
                UiIcons::modernizePopup(&menu);
                QAction *newSnippetAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Plus, this->palette().color(QPalette::WindowText)),
                    tr("新建片段"));
                connect(newSnippetAction, &QAction::triggered,
                        this, [this]() { openSnippetEditor(QString()); });
                if (item != nullptr) {
                    menu.addSeparator();
                    const QString id = item->data(kIdRole).toString();
                    QAction *executeAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Play, this->palette().color(QPalette::WindowText)),
                        tr("执行"));
                    QAction *copyAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Copy, this->palette().color(QPalette::WindowText)),
                        tr("复制"));
                    QAction *editAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Edit, this->palette().color(QPalette::WindowText)),
                        tr("编辑"));
                    QAction *deleteAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Trash, this->palette().color(QPalette::WindowText)),
                        tr("删除"));
                    connect(executeAction, &QAction::triggered,
                            this, [this, id]() { executeSnippetForId(id); });
                    connect(copyAction, &QAction::triggered,
                            this, [this, id]() { copySnippetForId(id); });
                    connect(editAction, &QAction::triggered,
                            this, [this, id]() { openSnippetEditor(id); });
                    connect(deleteAction,
                            &QAction::triggered,
                            this,
                            &ProductivityWidget::removeSelectedRecord);
                }
                menu.exec(m_snippetList->viewport()->mapToGlobal(position));
            });

    // Ctrl+1..Ctrl+9、Ctrl+0 分别执行列表中第 1..10 个片段；超出范围的按键在触发时安全
    // 忽略。快捷键只创建一次，绑定的是行索引而非片段 id，与行内按钮提示的数字保持一致。
    m_snippetShortcuts.reserve(kMaxShortcutSnippets);
    for (int index = 0; index < kMaxShortcutSnippets; ++index) {
        const int digit = (index == kMaxShortcutSnippets - 1) ? 0 : index + 1;
        auto *shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(digit)), this);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated,
                this, [this, index]() { executeSnippetAtRow(index); });
        m_snippetShortcuts.append(shortcut);
    }

    buildEditorDialog();

    // --------------------------------------------------------------- 历史标签
    auto *historyPage = new QWidget(m_tabs);
    historyPage->setObjectName(QStringLiteral("productivityHistoryPage"));
    auto *historyLayout = new QVBoxLayout(historyPage);
    historyLayout->setContentsMargins(0, 8, 0, 0);
    historyLayout->setSpacing(8);
    m_historyHostCombo = new QComboBox(historyPage);
    UiIcons::modernizePopup(m_historyHostCombo);
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

void ProductivityWidget::buildEditorDialog()
{
    m_editorDialog = new QDialog(this);
    m_editorDialog->setObjectName(QStringLiteral("productivitySnippetDialog"));
    m_editorDialog->setModal(true);
    m_editorDialog->setMinimumWidth(340);

    auto *layout = new QFormLayout(m_editorDialog);
    layout->setObjectName(QStringLiteral("productivitySnippetEditorLayout"));
    layout->setRowWrapPolicy(QFormLayout::WrapAllRows);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->setLabelAlignment(Qt::AlignLeft);
    layout->setVerticalSpacing(8);

    m_editorNameEdit = new QLineEdit(m_editorDialog);
    m_editorNameEdit->setObjectName(QStringLiteral("productivitySnippetNameEdit"));
    m_editorCommandEdit = new QTextEdit(m_editorDialog);
    m_editorCommandEdit->setObjectName(QStringLiteral("productivityCommandEdit"));
    m_editorCommandEdit->setAcceptRichText(false);
    // 弹窗不再受侧栏宽度约束，给命令输入更充裕的高度，方便查看较长的管道命令。
    m_editorCommandEdit->setMinimumHeight(140);
    m_editorDescriptionEdit = new QLineEdit(m_editorDialog);
    m_editorDescriptionEdit->setObjectName(QStringLiteral("productivitySnippetDescriptionEdit"));

    layout->addRow(tr("名称"), m_editorNameEdit);
    layout->addRow(tr("命令"), m_editorCommandEdit);
    layout->addRow(tr("说明"), m_editorDescriptionEdit);

    auto *cancelButton = new QPushButton(tr("取消"), m_editorDialog);
    cancelButton->setObjectName(QStringLiteral("productivityCancelEditor"));
    m_editorSaveButton = new QPushButton(tr("保存"), m_editorDialog);
    m_editorSaveButton->setObjectName(QStringLiteral("productivitySnippetSave"));
    m_editorSaveButton->setProperty("primaryAction", true);
    UiIcons::decorateButton(
        m_editorSaveButton,
        UiIcons::symbol(this, UiIcons::Symbol::Download, this->palette().color(QPalette::WindowText)),
        tr("保存当前片段修改"));
    auto *editorActions = new QHBoxLayout;
    editorActions->setContentsMargins(0, 0, 0, 0);
    editorActions->setSpacing(8);
    editorActions->addStretch(1);
    editorActions->addWidget(cancelButton);
    editorActions->addWidget(m_editorSaveButton);
    layout->addRow(editorActions);

    connect(cancelButton, &QPushButton::clicked, m_editorDialog, &QDialog::reject);
    connect(m_editorSaveButton, &QPushButton::clicked,
            this, &ProductivityWidget::saveEditorRecord);
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
    m_snippetList->clear();
    if (m_model == nullptr) {
        return;
    }

    // 分组功能已移除：始终只读取根层级，历史数据中残留的分组或嵌套片段不会在界面上
    // 出现。这是刻意的简化，而不是缺陷——用户已经明确要求去掉分组。
    bool ok = false;
    const QList<SnippetRecord> records = m_model->snippetsInGroup(QString(), &ok);
    if (!ok) {
        return;
    }
    int shortcutIndex = 0;
    for (const SnippetRecord &record : records) {
        if (record.kind != SnippetNodeKind::Snippet) {
            continue;
        }
        auto *item = new QListWidgetItem;
        item->setData(kIdRole, record.id);
        item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);
        m_snippetList->addItem(item);
        QWidget *row = createSnippetRowWidget(record, shortcutIndex);
        item->setSizeHint(row->sizeHint());
        m_snippetList->setItemWidget(item, row);
        ++shortcutIndex;
    }
}

QWidget *ProductivityWidget::createSnippetRowWidget(const SnippetRecord &record,
                                                     const int shortcutIndex)
{
    auto *row = new QWidget(m_snippetList);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(8, 6, 8, 6);
    rowLayout->setSpacing(6);

    // 文字区域设为鼠标事件透明，点击会穿透到下方的列表视口，这样用户仍可以直接在名称/
    // 说明区域按住拖拽排序，而不必找一个额外的拖拽手柄；右侧按钮区域保持正常可点击。
    auto *textArea = new QWidget(row);
    textArea->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *textLayout = new QVBoxLayout(textArea);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(1);

    auto *nameLabel = new QLabel(record.name, textArea);
    nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    QFont nameFont = nameLabel->font();
    nameFont.setBold(true);
    nameLabel->setFont(nameFont);

    const QString preview = record.description.isEmpty() ? record.command : record.description;
    auto *previewLabel = new QLabel(preview, textArea);
    previewLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    QFont previewFont = previewLabel->font();
    previewFont.setPointSizeF(std::max(7.0, previewFont.pointSizeF() - 1.5));
    previewLabel->setFont(previewFont);
    applyMutedPalette(previewLabel, 0.62);

    textLayout->addWidget(nameLabel);
    textLayout->addWidget(previewLabel);
    rowLayout->addWidget(textArea, 1);

    // 执行按钮采用与 AI 助手命令卡片一致的绿色主按钮样式（primaryAction），快捷键提示直接
    // 拼进按钮文字，不再用单独的灰色标签摆在旁边。
    auto *executeButton = new QPushButton(tr("执行"), row);
    executeButton->setObjectName(QStringLiteral("productivitySnippetExecute"));
    executeButton->setProperty("compactAction", true);
    executeButton->setProperty("primaryAction", true);
    if (shortcutIndex >= 0 && shortcutIndex < kMaxShortcutSnippets) {
        const int digit = (shortcutIndex == kMaxShortcutSnippets - 1) ? 0 : shortcutIndex + 1;
        const QString hint = QStringLiteral("Ctrl+%1").arg(digit);
        executeButton->setText(tr("执行  %1").arg(hint));
        executeButton->setToolTip(tr("快捷键 %1").arg(hint));
    }
    rowLayout->addWidget(executeButton);

    auto *copyButton = new QPushButton(tr("复制"), row);
    copyButton->setObjectName(QStringLiteral("productivitySnippetCopy"));
    copyButton->setProperty("compactAction", true);
    rowLayout->addWidget(copyButton);

    const QString id = record.id;
    connect(executeButton, &QPushButton::clicked,
            this, [this, id]() { executeSnippetForId(id); });
    connect(copyButton, &QPushButton::clicked,
            this, [this, id]() { copySnippetForId(id); });

    return row;
}

void ProductivityWidget::openSnippetEditor(const QString &id)
{
    if (m_editorDialog == nullptr) {
        return;
    }
    const bool isNew = id.isEmpty();
    if (isNew) {
        m_editingId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_editorNameEdit->clear();
        m_editorCommandEdit->clear();
        m_editorDescriptionEdit->clear();
        m_editorDialog->setWindowTitle(tr("新建片段"));
    } else {
        if (m_model == nullptr) {
            emit statusMessage(tr("效率工具模型尚未就绪"));
            return;
        }
        const std::optional<SnippetRecord> record = m_model->snippet(id);
        if (!record.has_value()) {
            emit statusMessage(tr("选中的 Snippet 已不存在，请刷新列表"));
            return;
        }
        m_editingId = record->id;
        m_editorNameEdit->setText(record->name);
        m_editorCommandEdit->setPlainText(record->command);
        m_editorDescriptionEdit->setText(record->description);
        m_editorDialog->setWindowTitle(tr("编辑片段"));
    }
    m_editorNameEdit->setFocus();
    // open() 是非阻塞的模态弹窗：既保证用户在关闭前无法操作侧栏其余部分，又不会像
    // exec() 那样阻塞事件循环，测试可以在弹窗仍打开时继续与其中的控件交互。
    m_editorDialog->open();
}

void ProductivityWidget::saveEditorRecord()
{
    if (m_model == nullptr) {
        emit statusMessage(tr("效率工具模型尚未就绪"));
        return;
    }
    const QString name = m_editorNameEdit->text().trimmed();
    const QString command = m_editorCommandEdit->toPlainText();
    if (name.isEmpty() || command.trimmed().isEmpty()) {
        emit statusMessage(tr("名称和命令不能为空"));
        return;
    }

    const std::optional<SnippetRecord> existing = m_model->snippet(m_editingId);
    SnippetRecord record;
    record.id = m_editingId;
    record.parentId = QString(); // 分组功能已移除，片段始终挂在根层级。
    record.kind = SnippetNodeKind::Snippet;
    record.name = name;
    record.command = command;
    record.description = m_editorDescriptionEdit->text();
    record.sortOrder = existing.has_value() ? existing->sortOrder : m_snippetList->count();

    if (m_model->saveSnippet(record)) {
        emit statusMessage(tr("Snippet 已保存"));
        m_editorDialog->accept();
    }
}

void ProductivityWidget::removeSelectedRecord()
{
    QListWidgetItem *item = m_snippetList->currentItem();
    if (item == nullptr || m_model == nullptr) {
        emit statusMessage(tr("请先选择要删除的 Snippet"));
        return;
    }
    if (QMessageBox::warning(this,
                             tr("确认删除"),
                             tr("确定删除选中的 Snippet 吗？"),
                             QMessageBox::Yes | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    int removed = 0;
    if (m_model->removeSnippet(item->data(kIdRole).toString(), &removed)) {
        m_editingId.clear();
        emit statusMessage(tr("已删除 Snippet"));
    }
}

void ProductivityWidget::executeSnippetAtRow(const int row)
{
    if (row < 0 || row >= m_snippetList->count()) {
        emit statusMessage(tr("请先选择要执行的 Snippet"));
        return;
    }
    executeSnippetForId(m_snippetList->item(row)->data(kIdRole).toString());
}

void ProductivityWidget::executeSnippetForId(const QString &id)
{
    if (m_model == nullptr) {
        emit statusMessage(tr("效率工具模型尚未就绪"));
        return;
    }
    const std::optional<SnippetRecord> record = m_model->snippet(id);
    if (!record.has_value() || record->command.trimmed().isEmpty()) {
        emit statusMessage(tr("Snippet 已不存在或命令为空"));
        return;
    }
    // 执行目标不再由界面勾选：空 targetIds 由上层解释为当前活动终端会话。
    emit commandExecutionRequested(record->command, QStringList{});
}

void ProductivityWidget::copySnippetForId(const QString &id)
{
    if (m_model == nullptr) {
        return;
    }
    const std::optional<SnippetRecord> record = m_model->snippet(id);
    if (!record.has_value()) {
        emit statusMessage(tr("Snippet 已不存在"));
        return;
    }
    if (QApplication::clipboard() != nullptr) {
        QApplication::clipboard()->setText(record->command);
    }
    emit statusMessage(tr("命令已复制到剪贴板"));
}

void ProductivityWidget::persistSnippetOrder()
{
    if (m_model == nullptr) {
        return;
    }
    // 先把当前可见顺序整体读出来，再逐条保存：saveSnippet 成功后会同步发出
    // snippetsChanged 并触发 reloadSnippets，边遍历边重建列表会让循环踩在已经失效的
    // QListWidgetItem 指针上。
    QStringList orderedIds;
    orderedIds.reserve(m_snippetList->count());
    for (int row = 0; row < m_snippetList->count(); ++row) {
        orderedIds.append(m_snippetList->item(row)->data(kIdRole).toString());
    }

    bool changed = false;
    for (int row = 0; row < orderedIds.size(); ++row) {
        std::optional<SnippetRecord> record = m_model->snippet(orderedIds.at(row));
        if (!record.has_value() || record->sortOrder == row) {
            continue;
        }
        record->sortOrder = row;
        changed = true;
        if (!m_model->saveSnippet(*record)) {
            break;
        }
    }
    if (changed) {
        reloadSnippets();
    }
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
    const QStringList previousBroadcastIds = checkedTargetIds(m_broadcastTargets);

    m_broadcastTargets->clear();
    m_historyHostCombo->clear();
    m_historyHostCombo->addItem(tr("本地 / 当前终端"), QString());

    QSet<QString> seen;
    for (const ConnectionTarget &target : std::as_const(m_targets)) {
        if (!target.isValid() || target.id.isEmpty() || seen.contains(target.id)) {
            continue;
        }
        seen.insert(target.id);
        QListWidgetItem *broadcastItem = createTargetItem(target, m_broadcastTargets);
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
