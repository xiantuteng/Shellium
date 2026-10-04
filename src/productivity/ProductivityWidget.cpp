#include "productivity/ProductivityWidget.h"

#include "productivity/ProductivityModel.h"
#include "i18n/Retranslate.h"
#include "ui/UiIcons.h"

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace {

/** @brief QListWidgetItem::UserRole 中保存的稳定 Snippet id。 */
constexpr int kIdRole = Qt::UserRole;

/** @brief 片段行中参与搜索的名称、命令和说明，统一小写后存放，筛选时无需再查库。 */
constexpr int kSearchTextRole = Qt::UserRole + 1;

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
 * @brief 列表行内的"执行/复制"按钮：比标题行按钮更小，平时透明，只在悬停时出底色。
 *
 * 主机树的行内没有任何彩色按钮；此前每行一枚绿色"执行"加一枚描边"复制"，在长列表里
 * 形成一整列高亮块，与主机侧栏的安静行风格明显不一致。
 */
void configureRowIconButton(QToolButton *button,
                            const UiIcons::Symbol symbol,
                            const QString &toolTip)
{
    Q_ASSERT(button != nullptr);
    button->setProperty("compactIconButton", true);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(false);
    button->setFixedSize(28, 28);
    button->setIconSize(QSize(15, 15));
    button->setIcon(UiIcons::symbol(button, symbol, button->palette().color(QPalette::WindowText)));
    button->setToolTip(toolTip);
    button->setAccessibleName(toolTip);
}

/**
 * @brief 用与"主机"侧栏相同的外框包住一个搜索输入框。
 * @param edit 实际承载输入的行编辑器；调用方保留其指针与 objectName，测试接口不变。
 * @param parent 外框的父控件。
 *
 * 外观（圆角描边、无内框、左侧放大镜）由 MainWindow 样式表按 ProductivitySearchBar /
 * ProductivitySearchEdit 两个名字与 HostSearchBar 共用同一组规则，这里只负责结构。
 */
QFrame *createSearchBar(QLineEdit *edit, QWidget *parent)
{
    auto *bar = new QFrame(parent);
    bar->setObjectName(QStringLiteral("ProductivitySearchBar"));
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(5, 0, 7, 0);
    layout->setSpacing(2);
    edit->setParent(bar);
    edit->setProperty("productivitySearchEdit", true);
    edit->setClearButtonEnabled(true);
    edit->addAction(
        UiIcons::symbol(edit, UiIcons::Symbol::Search, edit->palette().color(QPalette::WindowText)),
        QLineEdit::LeadingPosition);
    layout->addWidget(edit, 1);
    return bar;
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
    rootLayout->setSpacing(0);
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("productivityTabs"));
    m_tabs->setDocumentMode(true);
    // documentMode 下 macOS 会给标签栏画一条浅灰底座，在深色侧栏里很突兀；关掉底座并让
    // 两个标签均分宽度，使其与下方通栏搜索框同宽对齐。
    m_tabs->tabBar()->setDrawBase(false);
    m_tabs->tabBar()->setExpanding(true);
    rootLayout->addWidget(m_tabs);

    // ------------------------------------------------------------ 标题行动作条
    // 与"主机"侧栏一致：绿色文字主按钮 + 竖分隔线 + 次要图标按钮，整体由 MainWindow 摆进
    // 共享标题行。页面内部因此不再需要一条单独的工具栏，纵向空间全部留给列表。
    m_headerActions = new QWidget(this);
    m_headerActions->setObjectName(QStringLiteral("productivityHeaderActions"));
    auto *newSnippetButton = new QPushButton(tr("New snippet"), m_headerActions);
    newSnippetButton->setObjectName(QStringLiteral("productivityNewSnippet"));
    newSnippetButton->setProperty("primaryAction", true);
    newSnippetButton->setProperty("hostPrimaryAction", true);
    newSnippetButton->setMinimumWidth(90);
    newSnippetButton->setFixedHeight(30);
    // 主操作底色在深浅主题下都是固定的高亮绿，图标同主机页一样固定用深色保证对比度。
    UiIcons::decorateButton(
        newSnippetButton,
        UiIcons::symbol(this, UiIcons::Symbol::Plus, QColor(QStringLiteral("#0c2f24"))),
        tr("Create an executable command snippet"));
    auto *removeButton = new QToolButton(m_headerActions);
    removeButton->setObjectName(QStringLiteral("productivityRemoveSnippet"));
    removeButton->setText(tr("Delete"));
    UiIcons::decorateButton(
        removeButton,
        UiIcons::symbol(this, UiIcons::Symbol::Trash, this->palette().color(QPalette::WindowText)),
        tr("Delete the selected snippet"));
    configureProductivityIconButton(removeButton, tr("Delete the selected snippet"));
    auto *actionDivider = new QFrame(m_headerActions);
    actionDivider->setObjectName(QStringLiteral("HostHeaderActionDivider"));
    actionDivider->setFrameShape(QFrame::VLine);
    actionDivider->setFixedSize(1, 20);
    auto *headerLayout = new QHBoxLayout(m_headerActions);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(6);
    headerLayout->addWidget(newSnippetButton);
    headerLayout->addWidget(actionDivider, 0, Qt::AlignVCenter);
    headerLayout->addWidget(removeButton);

    // -------------------------------------------------------------- Snippets 标签
    auto *snippetPage = new QWidget(m_tabs);
    auto *snippetPageLayout = new QVBoxLayout(snippetPage);
    snippetPageLayout->setContentsMargins(0, 10, 0, 0);
    snippetPageLayout->setSpacing(10);

    m_snippetSearchEdit = new QLineEdit(snippetPage);
    m_snippetSearchEdit->setObjectName(QStringLiteral("productivitySnippetSearch"));
    m_snippetSearchEdit->setPlaceholderText(tr("Search name / command…"));
    snippetPageLayout->addWidget(createSearchBar(m_snippetSearchEdit, snippetPage));

    m_snippetList = new QListWidget(snippetPage);
    m_snippetList->setObjectName(QStringLiteral("productivitySnippetList"));
    m_snippetList->setSelectionMode(QAbstractItemView::SingleSelection);
    // 分组功能已移除，列表始终是根层级的扁平序列；InternalMove 让用户可以直接拖拽调整
    // sortOrder，不再需要单独的"上移/下移"按钮。
    m_snippetList->setDragDropMode(QAbstractItemView::InternalMove);
    m_snippetList->setDefaultDropAction(Qt::MoveAction);
    m_snippetList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_snippetList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_snippetList->setSpacing(1);
    m_snippetList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    snippetPageLayout->addWidget(m_snippetList, 1);
    const int snippetTabIndex = m_tabs->addTab(snippetPage, tr("Command snippets"));
    i18n::onRetranslate(this, [this, snippetTabIndex, newSnippetButton, removeButton] {
        m_tabs->setTabText(snippetTabIndex, tr("Command snippets"));
        m_snippetSearchEdit->setPlaceholderText(tr("Search name / command…"));
        newSnippetButton->setText(tr("New snippet"));
        newSnippetButton->setToolTip(tr("Create an executable command snippet"));
        removeButton->setText(tr("Delete"));
        removeButton->setToolTip(tr("Delete the selected snippet"));
        removeButton->setAccessibleName(tr("Delete the selected snippet"));
        // 行内“执行/复制”按钮的提示在 createSnippetRowWidget 里一次性写死，语言切换时
        // 不会自己更新；整表重建是让每一行都回到当前语言最省心的做法（右键菜单是按需
        // 构造的，下次弹出即为新语言，无需在此处理）。
        reloadSnippets();
    });

    // 删除只对片段页有意义；切到历史页时置灰，免得用户以为它能删历史记录。只置灰不隐藏，
    // 否则标题行宽度变化会让"新建片段"按钮随标签切换左右跳动。
    connect(m_tabs, &QTabWidget::currentChanged, this,
            [snippetTabIndex, removeButton](const int index) {
                removeButton->setEnabled(index == snippetTabIndex);
            });
    connect(newSnippetButton, &QPushButton::clicked, this, [this, snippetTabIndex]() {
        m_tabs->setCurrentIndex(snippetTabIndex);
        openSnippetEditor(QString());
    });
    connect(removeButton, &QToolButton::clicked,
            this, &ProductivityWidget::removeSelectedRecord);
    connect(m_snippetSearchEdit, &QLineEdit::textChanged,
            this, &ProductivityWidget::applySnippetFilter);
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
                    tr("New snippet"));
                connect(newSnippetAction, &QAction::triggered,
                        this, [this]() { openSnippetEditor(QString()); });
                if (item != nullptr) {
                    menu.addSeparator();
                    const QString id = item->data(kIdRole).toString();
                    QAction *executeAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Play, this->palette().color(QPalette::WindowText)),
                        tr("Run"));
                    QAction *copyAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Copy, this->palette().color(QPalette::WindowText)),
                        tr("Copy"));
                    QAction *editAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Edit, this->palette().color(QPalette::WindowText)),
                        tr("Edit"));
                    QAction *deleteAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Trash, this->palette().color(QPalette::WindowText)),
                        tr("Delete"));
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

    // 侧栏不再提供 Ctrl+1..Ctrl+0 数字快捷键：按序号执行片段统一由终端里的 Cmd+K 快速面板
    // （Cmd+1..9）负责，两套编号并存只会让同一个片段在两处对应不同的按键。

    buildEditorDialog();

    // --------------------------------------------------------------- 历史标签
    auto *historyPage = new QWidget(m_tabs);
    historyPage->setObjectName(QStringLiteral("productivityHistoryPage"));
    auto *historyLayout = new QVBoxLayout(historyPage);
    historyLayout->setContentsMargins(0, 10, 0, 0);
    historyLayout->setSpacing(10);
    m_historyHostCombo = new QComboBox(historyPage);
    UiIcons::modernizePopup(m_historyHostCombo);
    m_historyHostCombo->setObjectName(QStringLiteral("productivityHistoryHost"));
    m_historyPrefixEdit = new QLineEdit(historyPage);
    m_historyPrefixEdit->setObjectName(QStringLiteral("productivityHistoryPrefix"));
    m_historyPrefixEdit->setPlaceholderText(tr("Filter by command prefix"));
    // 主机与前缀筛选在窄侧栏内上下堆叠；并排布局会让两个输入框都只剩百余像素，主机名
    // 和长前缀同时被截断。前缀输入框套用与主机页相同的搜索框外观。
    historyLayout->addWidget(m_historyHostCombo);
    historyLayout->addWidget(createSearchBar(m_historyPrefixEdit, historyPage));
    m_historyList = new QListWidget(historyPage);
    m_historyList->setObjectName(QStringLiteral("productivityHistoryList"));
    m_historyList->setSpacing(1);
    m_historyList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_historyList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    historyLayout->addWidget(m_historyList, 1);
    auto *historyExecuteButton = new QPushButton(tr("Run on the selected host"), historyPage);
    historyExecuteButton->setObjectName(QStringLiteral("productivityHistoryExecute"));
    // 侧栏里唯一的绿色主按钮留给标题行的"新建片段"，与主机页只有"新增主机"高亮一致；
    // 历史重跑是次要动作，使用普通描边按钮。
    UiIcons::decorateButton(
        historyExecuteButton,
        UiIcons::symbol(this, UiIcons::Symbol::Play, this->palette().color(QPalette::WindowText)),
        tr("Re-run the command on the host selected in the history filter"));
    historyLayout->addWidget(historyExecuteButton);
    const int historyTabIndex = m_tabs->addTab(historyPage, tr("Command history"));
    i18n::onRetranslate(this, [this, historyTabIndex, historyExecuteButton] {
        m_tabs->setTabText(historyTabIndex, tr("Command history"));
        m_historyPrefixEdit->setPlaceholderText(tr("Filter by command prefix"));
        historyExecuteButton->setText(tr("Run on the selected host"));
        historyExecuteButton->setToolTip(
            tr("Re-run the command on the host selected in the history filter"));
    });

    connect(m_historyHostCombo, &QComboBox::currentIndexChanged,
            this, &ProductivityWidget::reloadHistory);
    connect(m_historyPrefixEdit, &QLineEdit::textChanged,
            this, &ProductivityWidget::reloadHistory);
    connect(historyExecuteButton, &QPushButton::clicked, this, [this]() {
        const QListWidgetItem *item = m_historyList->currentItem();
        if (item == nullptr) {
            emit statusMessage(tr("Select a command history entry first"));
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
                    tr("Run"));
                QAction *copyAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Copy, this->palette().color(QPalette::WindowText)),
                    tr("Copy command"));
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

    layout->addRow(tr("Name"), m_editorNameEdit);
    layout->addRow(tr("Command"), m_editorCommandEdit);
    layout->addRow(tr("Description"), m_editorDescriptionEdit);

    auto *cancelButton = new QPushButton(tr("Cancel"), m_editorDialog);
    cancelButton->setObjectName(QStringLiteral("productivityCancelEditor"));
    m_editorSaveButton = new QPushButton(tr("Save"), m_editorDialog);
    m_editorSaveButton->setObjectName(QStringLiteral("productivitySnippetSave"));
    m_editorSaveButton->setProperty("primaryAction", true);
    UiIcons::decorateButton(
        m_editorSaveButton,
        UiIcons::symbol(this, UiIcons::Symbol::Download, this->palette().color(QPalette::WindowText)),
        tr("Save changes to the current snippet"));
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

QWidget *ProductivityWidget::headerActionsWidget() const
{
    return m_headerActions;
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
    populateHistoryHostSelector();
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
    for (const SnippetRecord &record : records) {
        if (record.kind != SnippetNodeKind::Snippet) {
            continue;
        }
        auto *item = new QListWidgetItem;
        item->setData(kIdRole, record.id);
        item->setData(kSearchTextRole,
                      (record.name + QLatin1Char('\n') + record.command + QLatin1Char('\n')
                       + record.description).toCaseFolded());
        item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);
        m_snippetList->addItem(item);
        QWidget *row = createSnippetRowWidget(record);
        item->setSizeHint(row->sizeHint());
        m_snippetList->setItemWidget(item, row);
    }
    applySnippetFilter();
}

void ProductivityWidget::applySnippetFilter()
{
    const QString needle = m_snippetSearchEdit->text().trimmed().toCaseFolded();
    for (int row = 0; row < m_snippetList->count(); ++row) {
        QListWidgetItem *item = m_snippetList->item(row);
        item->setHidden(!needle.isEmpty()
                        && !item->data(kSearchTextRole).toString().contains(needle));
    }
    // 与主机页搜索相同：筛选结果只是局部视图，此时拖拽得到的顺序不能代表完整列表，
    // 写回 sortOrder 会打乱被隐藏的片段，因此筛选期间直接关闭拖放。
    m_snippetList->setDragDropMode(needle.isEmpty() ? QAbstractItemView::InternalMove
                                                    : QAbstractItemView::NoDragDrop);
}

QWidget *ProductivityWidget::createSnippetRowWidget(const SnippetRecord &record)
{
    auto *row = new QWidget(m_snippetList);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(10, 7, 6, 7);
    rowLayout->setSpacing(4);

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
    nameFont.setWeight(QFont::DemiBold);
    nameLabel->setFont(nameFont);

    const QString preview = record.description.isEmpty() ? record.command : record.description;
    auto *previewLabel = new QLabel(preview, textArea);
    previewLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    QFont previewFont = previewLabel->font();
    previewFont.setPointSizeF(std::max(7.0, previewFont.pointSizeF() - 1.5));
    previewLabel->setFont(previewFont);
    applyMutedPalette(previewLabel, 0.62);

    // 长命令不能把行撑宽：横向忽略文字的理想宽度，超出部分直接裁掉，右侧按钮始终可见。
    for (QLabel *label : {nameLabel, previewLabel}) {
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    textLayout->addWidget(nameLabel);
    textLayout->addWidget(previewLabel);
    rowLayout->addWidget(textArea, 1);

    // 执行/复制收成两枚透明图标按钮，与主机树行内只有文字、交互靠悬停卡片表达的风格
    // 保持一致。
    const QString runToolTip = tr("Run");

    auto *executeButton = new QToolButton(row);
    executeButton->setObjectName(QStringLiteral("productivitySnippetExecute"));
    configureRowIconButton(executeButton, UiIcons::Symbol::Play, runToolTip);
    rowLayout->addWidget(executeButton, 0, Qt::AlignVCenter);

    auto *copyButton = new QToolButton(row);
    copyButton->setObjectName(QStringLiteral("productivitySnippetCopy"));
    configureRowIconButton(copyButton, UiIcons::Symbol::Copy, tr("Copy command"));
    rowLayout->addWidget(copyButton, 0, Qt::AlignVCenter);

    const QString id = record.id;
    connect(executeButton, &QToolButton::clicked,
            this, [this, id]() { executeSnippetForId(id); });
    connect(copyButton, &QToolButton::clicked,
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
        m_editorDialog->setWindowTitle(tr("New snippet"));
    } else {
        if (m_model == nullptr) {
            emit statusMessage(tr("The productivity model is not ready yet"));
            return;
        }
        const std::optional<SnippetRecord> record = m_model->snippet(id);
        if (!record.has_value()) {
            emit statusMessage(tr("The selected snippet no longer exists; refresh the list"));
            return;
        }
        m_editingId = record->id;
        m_editorNameEdit->setText(record->name);
        m_editorCommandEdit->setPlainText(record->command);
        m_editorDescriptionEdit->setText(record->description);
        m_editorDialog->setWindowTitle(tr("Edit snippet"));
    }
    m_editorNameEdit->setFocus();
    // open() 是非阻塞的模态弹窗：既保证用户在关闭前无法操作侧栏其余部分，又不会像
    // exec() 那样阻塞事件循环，测试可以在弹窗仍打开时继续与其中的控件交互。
    m_editorDialog->open();
}

void ProductivityWidget::saveEditorRecord()
{
    if (m_model == nullptr) {
        emit statusMessage(tr("The productivity model is not ready yet"));
        return;
    }
    const QString name = m_editorNameEdit->text().trimmed();
    const QString command = m_editorCommandEdit->toPlainText();
    if (name.isEmpty() || command.trimmed().isEmpty()) {
        emit statusMessage(tr("The name and command cannot be empty"));
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
        emit statusMessage(tr("Snippet saved"));
        m_editorDialog->accept();
    }
}

void ProductivityWidget::removeSelectedRecord()
{
    QListWidgetItem *item = m_snippetList->currentItem();
    if (item == nullptr || m_model == nullptr) {
        emit statusMessage(tr("Select a snippet to delete first"));
        return;
    }
    if (QMessageBox::warning(this,
                             tr("Confirm delete"),
                             tr("Delete the selected snippet?"),
                             QMessageBox::Yes | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    int removed = 0;
    if (m_model->removeSnippet(item->data(kIdRole).toString(), &removed)) {
        m_editingId.clear();
        emit statusMessage(tr("Snippet deleted"));
    }
}

void ProductivityWidget::executeSnippetForId(const QString &id)
{
    if (m_model == nullptr) {
        emit statusMessage(tr("The productivity model is not ready yet"));
        return;
    }
    const std::optional<SnippetRecord> record = m_model->snippet(id);
    if (!record.has_value() || record->command.trimmed().isEmpty()) {
        emit statusMessage(tr("The snippet no longer exists or its command is empty"));
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
        emit statusMessage(tr("The snippet no longer exists"));
        return;
    }
    if (QApplication::clipboard() != nullptr) {
        QApplication::clipboard()->setText(record->command);
    }
    emit statusMessage(tr("Command copied to clipboard"));
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

void ProductivityWidget::populateHistoryHostSelector()
{
    const QString previousHistoryId = m_historyHostCombo->currentData().toString();

    // 重建期间屏蔽 currentIndexChanged，末尾统一 reloadHistory 一次，避免每加一项都查库。
    {
        const QSignalBlocker blocker(m_historyHostCombo);
        m_historyHostCombo->clear();
        m_historyHostCombo->addItem(tr("Local / current terminal"), QString());

        QSet<QString> seen;
        for (const ConnectionTarget &target : std::as_const(m_targets)) {
            if (!target.isValid() || target.id.isEmpty() || seen.contains(target.id)) {
                continue;
            }
            seen.insert(target.id);
            m_historyHostCombo->addItem(
                target.displayName.isEmpty() ? target.connectionSpecifier(true) : target.displayName,
                target.id);
        }
        const int historyIndex = m_historyHostCombo->findData(previousHistoryId);
        m_historyHostCombo->setCurrentIndex(historyIndex >= 0 ? historyIndex : 0);
    }
    reloadHistory();
}
