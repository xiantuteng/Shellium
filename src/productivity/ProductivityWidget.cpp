#include "productivity/ProductivityWidget.h"

#include "productivity/ProductivityModel.h"
#include "i18n/Retranslate.h"
#include "ui/FocusWithin.h"
#include "ui/UiIcons.h"

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QDate>
#include <QDialog>
#include <QEvent>
#include <QFontDatabase>
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
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QStackedWidget>
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
    FocusWithin::track(bar);
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

/** @brief 行内操作按钮容器的 objectName；悬停或选中时显示，其余时间隐藏。 */
const QString kRowActionsName = QStringLiteral("productivityRowActions");
/** @brief 操作按钮出现时让位的尾部信息（历史行的执行时间）。 */
const QString kRowTrailingName = QStringLiteral("productivityRowTrailing");
/** @brief 标记"属于效率工具列表的行控件"，事件过滤器据此识别鼠标进出。 */
const char *const kListRowProperty = "productivityListRow";

/** @brief 历史分段：今天、昨天、更早。 */
constexpr int kSectionToday = 0;
constexpr int kSectionYesterday = 1;
constexpr int kSectionEarlier = 2;

/** @brief 选中的视图按钮底色是固定的品牌绿，图标与主按钮一样固定用深色保证对比度。 */
const QColor kOnAccentColor(QStringLiteral("#0c2f24"));

/**
 * @brief 单行显示、超宽时以省略号结尾的标签。
 *
 * text() 始终保留完整原文：搜索、测试和无障碍读取到的都是真实命令，只有绘制时才截断。
 * 多行命令在预览里折成一行，避免把卡片撑高。
 */
class ElidedLabel final : public QLabel {
public:
    using QLabel::QLabel;

    [[nodiscard]] QSize sizeHint() const override
    {
        const QMargins margins = contentsMargins();
        return {0, fontMetrics().height() + margins.top() + margins.bottom()};
    }

    [[nodiscard]] QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        // QFrame 负责画样式表里的底色、圆角与描边；文字由这里自己画，QLabel 原本的
        // 绘制不再调用，否则完整文本会和省略后的文本叠在一起。
        QFrame::paintEvent(event);
        QPainter painter(this);
        const QRect area = contentsRect();
        const QString elided = fontMetrics().elidedText(
            text().simplified(), Qt::ElideRight, area.width());
        style()->drawItemText(&painter, area, Qt::AlignLeft | Qt::AlignVCenter, palette(),
                              isEnabled(), elided, foregroundRole());
    }
};

/** @brief 构造一个单行省略标签，横向不参与布局宽度竞争。 */
ElidedLabel *createElidedLabel(const QString &text, const QString &objectName, QWidget *parent)
{
    auto *label = new ElidedLabel(text, parent);
    label->setObjectName(objectName);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    label->setToolTip(text);
    return label;
}

/** @brief 等宽字体：命令预览必须一眼和名称、说明区分开。 */
QFont monospaceFont(const QFont &base, const qreal pointSizeDelta)
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSizeF(std::max(7.0, base.pointSizeF() + pointSizeDelta));
    return font;
}

/**
 * @brief 行首的圆角图标块：只是装饰，不接收鼠标，点击穿透到列表以便选中与拖拽。
 *
 * 用 QToolButton 而不是 QLabel 承载图标，是为了用上随调色板重绘的图标引擎：深浅主题
 * 切换后无需重建整张列表。
 */
QToolButton *createRowTile(QWidget *parent, const UiIcons::Symbol symbol, const int size,
                           const int iconSize)
{
    auto *tile = new QToolButton(parent);
    tile->setProperty("productivityRowTile", true);
    tile->setAttribute(Qt::WA_TransparentForMouseEvents);
    tile->setFocusPolicy(Qt::NoFocus);
    tile->setToolButtonStyle(Qt::ToolButtonIconOnly);
    tile->setFixedSize(size, size);
    tile->setIconSize(QSize(iconSize, iconSize));
    tile->setIcon(UiIcons::symbol(tile, symbol, tile->palette().color(QPalette::WindowText)));
    return tile;
}

/** @brief 行内"执行"按钮：品牌绿实心圆，与其余透明图标按钮区分出主次。 */
void configureRowRunButton(QToolButton *button, const QString &toolTip)
{
    configureRowIconButton(button, UiIcons::Symbol::Play, toolTip);
    button->setProperty("productivityRunAction", true);
    button->setIcon(UiIcons::symbol(button, UiIcons::Symbol::Play, kOnAccentColor));
}

/** @brief 历史记录落在哪个分段。 */
int historySectionFor(const QDateTime &executedAt)
{
    const QDate day = executedAt.toLocalTime().date();
    const QDate today = QDate::currentDate();
    if (day >= today) {
        return kSectionToday;
    }
    return day == today.addDays(-1) ? kSectionYesterday : kSectionEarlier;
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
    m_pages = new QStackedWidget(this);
    m_pages->setObjectName(QStringLiteral("productivityPages"));
    rootLayout->addWidget(m_pages);

    // ------------------------------------------------------------ 标题右侧视图切换器
    // 两枚图标按钮取代原来占一整行的"片段 / 历史"标签栏：纵向空间全部留给列表，切换入口
    // 紧挨着页面标题，读起来就是"效率工具 · 当前看的是哪一类"。
    m_viewSwitcher = new QFrame(this);
    m_viewSwitcher->setObjectName(QStringLiteral("ProductivityViewSwitcher"));
    auto *switcherLayout = new QHBoxLayout(m_viewSwitcher);
    switcherLayout->setContentsMargins(3, 3, 3, 3);
    switcherLayout->setSpacing(2);
    auto *viewGroup = new QButtonGroup(m_viewSwitcher);
    viewGroup->setExclusive(true);
    const auto createViewButton = [this, switcherLayout, viewGroup](const QString &objectName) {
        auto *button = new QToolButton(m_viewSwitcher);
        button->setObjectName(objectName);
        button->setProperty("productivityViewButton", true);
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setFixedSize(30, 30);
        button->setIconSize(QSize(18, 18));
        button->setCursor(Qt::PointingHandCursor);
        viewGroup->addButton(button);
        switcherLayout->addWidget(button);
        return button;
    };
    m_snippetsViewButton = createViewButton(QStringLiteral("productivitySnippetsView"));
    m_historyViewButton = createViewButton(QStringLiteral("productivityHistoryView"));
    connect(m_snippetsViewButton, &QToolButton::clicked,
            this, [this]() { setCurrentView(View::Snippets); });
    connect(m_historyViewButton, &QToolButton::clicked,
            this, [this]() { setCurrentView(View::History); });

    // ------------------------------------------------------------ 标题行动作条
    // 与"主机"侧栏一致：绿色文字主按钮，整体由 MainWindow 摆进共享标题行右侧。删除不再
    // 占标题行的位置：它只作用于某一条片段，放在该行的"更多"菜单和右键菜单里更不易误删。
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
        UiIcons::symbol(this, UiIcons::Symbol::Plus, kOnAccentColor),
        tr("Create an executable command snippet"));
    auto *headerLayout = new QHBoxLayout(m_headerActions);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(6);
    headerLayout->addWidget(newSnippetButton);

    // -------------------------------------------------------------- 片段页
    auto *snippetPage = new QWidget(m_pages);
    snippetPage->setObjectName(QStringLiteral("productivitySnippetPage"));
    auto *snippetPageLayout = new QVBoxLayout(snippetPage);
    snippetPageLayout->setContentsMargins(0, 2, 0, 0);
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
    m_snippetList->setSpacing(3);
    m_snippetList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_snippetList->setMouseTracking(true);
    snippetPageLayout->addWidget(m_snippetList, 1);
    m_pages->addWidget(snippetPage);

    connect(newSnippetButton, &QPushButton::clicked, this, [this]() {
        setCurrentView(View::Snippets);
        openSnippetEditor(QString());
    });
    connect(m_snippetSearchEdit, &QLineEdit::textChanged,
            this, &ProductivityWidget::applySnippetFilter);
    connect(m_snippetList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) {
                if (item != nullptr) {
                    openSnippetEditor(item->data(kIdRole).toString());
                }
            });
    connect(m_snippetList, &QListWidget::itemSelectionChanged,
            this, [this]() { updateRowActionVisibility(m_snippetList); });
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
                showSnippetMenu(item != nullptr ? item->data(kIdRole).toString() : QString(),
                                m_snippetList->viewport()->mapToGlobal(position));
            });

    // 侧栏不再提供 Ctrl+1..Ctrl+0 数字快捷键：按序号执行片段统一由终端里的 Cmd+K 快速面板
    // （Cmd+1..9）负责，两套编号并存只会让同一个片段在两处对应不同的按键。

    buildEditorDialog();

    // --------------------------------------------------------------- 历史页
    auto *historyPage = new QWidget(m_pages);
    historyPage->setObjectName(QStringLiteral("productivityHistoryPage"));
    auto *historyLayout = new QVBoxLayout(historyPage);
    historyLayout->setContentsMargins(0, 2, 0, 0);
    historyLayout->setSpacing(10);
    m_historyHostCombo = new QComboBox(historyPage);
    UiIcons::modernizePopup(m_historyHostCombo);
    m_historyHostCombo->setObjectName(QStringLiteral("productivityHistoryHost"));
    // 主机名通常不长，下拉框按内容收窄、搜索框占余下宽度，两者并排成一行筛选栏，比上下
    // 堆叠少占一行；主机名过长时由下拉框自身省略，不挤掉搜索框。
    m_historyHostCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_historyHostCombo->setMaximumWidth(190);
    m_historyPrefixEdit = new QLineEdit(historyPage);
    m_historyPrefixEdit->setObjectName(QStringLiteral("productivityHistoryPrefix"));
    m_historyPrefixEdit->setPlaceholderText(tr("Filter by command prefix"));
    auto *historyFilterRow = new QHBoxLayout;
    historyFilterRow->setContentsMargins(0, 0, 0, 0);
    historyFilterRow->setSpacing(8);
    historyFilterRow->addWidget(m_historyHostCombo);
    QFrame *historySearchBar = createSearchBar(m_historyPrefixEdit, historyPage);
    historySearchBar->setMinimumWidth(110);
    historyFilterRow->addWidget(historySearchBar, 1);
    historyLayout->addLayout(historyFilterRow);
    m_historyList = new QListWidget(historyPage);
    m_historyList->setObjectName(QStringLiteral("productivityHistoryList"));
    m_historyList->setSpacing(1);
    m_historyList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_historyList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_historyList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_historyList->setMouseTracking(true);
    historyLayout->addWidget(m_historyList, 1);
    m_pages->addWidget(historyPage);

    i18n::onRetranslate(this, [this, newSnippetButton] {
        m_snippetSearchEdit->setPlaceholderText(tr("Search name / command…"));
        m_historyPrefixEdit->setPlaceholderText(tr("Filter by command prefix"));
        newSnippetButton->setText(tr("New snippet"));
        newSnippetButton->setToolTip(tr("Create an executable command snippet"));
        for (const auto &[button, text] :
             {std::pair{m_snippetsViewButton, tr("Command snippets")},
              std::pair{m_historyViewButton, tr("Command history")}}) {
            button->setToolTip(text);
            button->setAccessibleName(text);
        }
        // 行内按钮的提示和历史分段标题在建行时一次性写死，语言切换时不会自己更新；整表
        // 重建是让每一行都回到当前语言最省心的做法（右键菜单按需构造，无需在此处理）。
        reloadSnippets();
        reloadHistory();
    });

    connect(m_historyHostCombo, &QComboBox::currentIndexChanged,
            this, &ProductivityWidget::reloadHistory);
    connect(m_historyPrefixEdit, &QLineEdit::textChanged,
            this, &ProductivityWidget::reloadHistory);
    connect(m_historyList, &QListWidget::itemSelectionChanged,
            this, [this]() { updateRowActionVisibility(m_historyList); });
    connect(m_historyList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) {
                if (item != nullptr) {
                    executeHistoryCommand(item->data(Qt::UserRole).toString());
                }
            });
    connect(m_historyList,
            &QWidget::customContextMenuRequested,
            this,
            [this](const QPoint &position) {
                QListWidgetItem *item = m_historyList->itemAt(position);
                const QString command =
                    item != nullptr ? item->data(Qt::UserRole).toString() : QString();
                if (command.isEmpty()) {
                    return;
                }
                m_historyList->setCurrentItem(item);
                QMenu menu(this);
                UiIcons::modernizePopup(&menu);
                const QColor iconColor = this->palette().color(QPalette::WindowText);
                QAction *executeAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Play, iconColor), tr("Run"));
                QAction *copyAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Copy, iconColor), tr("Copy command"));
                QAction *saveAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Bookmark, iconColor),
                    tr("Save as snippet"));
                connect(executeAction, &QAction::triggered,
                        this, [this, command]() { executeHistoryCommand(command); });
                connect(copyAction, &QAction::triggered, this, [command]() {
                    if (QApplication::clipboard() != nullptr) {
                        QApplication::clipboard()->setText(command);
                    }
                });
                connect(saveAction, &QAction::triggered,
                        this, [this, command]() { saveHistoryCommandAsSnippet(command); });
                menu.exec(m_historyList->viewport()->mapToGlobal(position));
            });

    updateViewSwitcher();
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

QWidget *ProductivityWidget::headerViewSwitcherWidget() const
{
    return m_viewSwitcher;
}

void ProductivityWidget::setCurrentView(const View view)
{
    m_pages->setCurrentIndex(static_cast<int>(view));
    updateViewSwitcher();
}

ProductivityWidget::View ProductivityWidget::currentView() const
{
    return static_cast<View>(m_pages->currentIndex());
}

void ProductivityWidget::updateViewSwitcher()
{
    const bool snippets = currentView() == View::Snippets;
    const QColor idleColor = palette().color(QPalette::WindowText);
    for (const auto &[button, symbol, active] :
         {std::tuple{m_snippetsViewButton, UiIcons::Symbol::SnippetStack, snippets},
          std::tuple{m_historyViewButton, UiIcons::Symbol::HistoryRewind, !snippets}}) {
        button->setChecked(active);
        // 选中项的绿底是固定品牌色，图标用固定深色；未选中项跟随主题文字色。
        button->setIcon(UiIcons::symbol(button, symbol, active ? kOnAccentColor : idleColor));
    }
}

void ProductivityWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && m_snippetsViewButton != nullptr) {
        updateViewSwitcher();
    }
}

bool ProductivityWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::LayoutRequest) {
        // 列表项高度是建行时按当时的 sizeHint 记下的。主窗口样式表往往在行建好之后才生效，
        // 给命令预览补上内边距、换掉字号；不重新同步，行会被压回旧高度、文字被截掉上下
        // 一截。行内布局失效时会收到 LayoutRequest，借此把最新高度写回对应列表项。
        auto *row = qobject_cast<QWidget *>(watched);
        if (row != nullptr && row->property(kListRowProperty).toBool()) {
            for (QListWidget *list : {m_snippetList, m_historyList}) {
                for (int index = 0; index < list->count(); ++index) {
                    QListWidgetItem *item = list->item(index);
                    if (list->itemWidget(item) == row) {
                        if (item->sizeHint() != row->sizeHint()) {
                            item->setSizeHint(row->sizeHint());
                        }
                        return QWidget::eventFilter(watched, event);
                    }
                }
            }
        }
    }
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        auto *row = qobject_cast<QWidget *>(watched);
        if (row != nullptr && row->property(kListRowProperty).toBool()) {
            if (event->type() == QEvent::Enter) {
                m_hoveredRow = row;
            } else if (m_hoveredRow == row) {
                m_hoveredRow.clear();
            }
            auto *list = qobject_cast<QListWidget *>(row->parentWidget() != nullptr
                                                         ? row->parentWidget()->parentWidget()
                                                         : nullptr);
            if (list != nullptr) {
                updateRowActionVisibility(list);
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ProductivityWidget::updateRowActionVisibility(QListWidget *list)
{
    for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem *item = list->item(row);
        QWidget *rowWidget = list->itemWidget(item);
        if (rowWidget == nullptr) {
            continue;
        }
        const bool active = item->isSelected() || rowWidget == m_hoveredRow;
        if (auto *actions = rowWidget->findChild<QWidget *>(kRowActionsName)) {
            actions->setVisible(active);
        }
        if (auto *trailing = rowWidget->findChild<QWidget *>(kRowTrailingName)) {
            trailing->setVisible(!active);
        }
    }
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
    m_hoveredRow.clear();
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
    row->setProperty(kListRowProperty, true);
    row->installEventFilter(this);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(10, 9, 8, 9);
    rowLayout->setSpacing(10);

    rowLayout->addWidget(createRowTile(row, UiIcons::Symbol::Terminal, 32, 16), 0, Qt::AlignTop);

    // 文字区域设为鼠标事件透明，点击会穿透到下方的列表视口，这样用户仍可以直接在名称/
    // 说明区域按住拖拽排序，而不必找一个额外的拖拽手柄；右侧按钮区域保持正常可点击。
    auto *textArea = new QWidget(row);
    textArea->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *textLayout = new QVBoxLayout(textArea);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(5);

    auto *nameLabel = createElidedLabel(record.name, QStringLiteral("ProductivityRowTitle"), textArea);
    QFont nameFont = nameLabel->font();
    nameFont.setWeight(QFont::DemiBold);
    nameLabel->setFont(nameFont);
    textLayout->addWidget(nameLabel);

    // 命令始终以等宽小字预览：名称说明"做什么"，预览让人确认"实际会敲什么"。
    auto *commandLabel = createElidedLabel(
        record.command, QStringLiteral("ProductivityCommandPreview"), textArea);
    commandLabel->setFont(monospaceFont(commandLabel->font(), -1.5));
    textLayout->addWidget(commandLabel);

    if (!record.description.trimmed().isEmpty()) {
        auto *descriptionLabel = createElidedLabel(
            record.description, QStringLiteral("ProductivityRowCaption"), textArea);
        QFont captionFont = descriptionLabel->font();
        captionFont.setPointSizeF(std::max(7.0, captionFont.pointSizeF() - 1.5));
        descriptionLabel->setFont(captionFont);
        textLayout->addWidget(descriptionLabel);
    }
    rowLayout->addWidget(textArea, 1);

    // 操作按钮只在悬停或选中时出现（updateRowActionVisibility），长列表平时只有文字。
    auto *actions = new QWidget(row);
    actions->setObjectName(kRowActionsName);
    auto *actionsLayout = new QHBoxLayout(actions);
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->setSpacing(4);

    auto *executeButton = new QToolButton(actions);
    executeButton->setObjectName(QStringLiteral("productivitySnippetExecute"));
    configureRowRunButton(executeButton, tr("Run"));
    actionsLayout->addWidget(executeButton);

    auto *copyButton = new QToolButton(actions);
    copyButton->setObjectName(QStringLiteral("productivitySnippetCopy"));
    configureRowIconButton(copyButton, UiIcons::Symbol::Copy, tr("Copy command"));
    actionsLayout->addWidget(copyButton);

    auto *moreButton = new QToolButton(actions);
    moreButton->setObjectName(QStringLiteral("productivitySnippetMore"));
    configureRowIconButton(moreButton, UiIcons::Symbol::More, tr("More actions"));
    actionsLayout->addWidget(moreButton);
    actions->setVisible(false);
    rowLayout->addWidget(actions, 0, Qt::AlignVCenter);

    const QString id = record.id;
    connect(executeButton, &QToolButton::clicked,
            this, [this, id]() { executeSnippetForId(id); });
    connect(copyButton, &QToolButton::clicked,
            this, [this, id]() { copySnippetForId(id); });
    connect(moreButton, &QToolButton::clicked, this, [this, id, moreButton]() {
        for (int index = 0; index < m_snippetList->count(); ++index) {
            if (m_snippetList->item(index)->data(kIdRole).toString() == id) {
                m_snippetList->setCurrentRow(index);
                break;
            }
        }
        showSnippetMenu(id, moreButton->mapToGlobal(QPoint(0, moreButton->height())));
    });

    return row;
}

void ProductivityWidget::showSnippetMenu(const QString &id, const QPoint &globalPosition)
{
    QMenu menu(this);
    UiIcons::modernizePopup(&menu);
    const QColor iconColor = this->palette().color(QPalette::WindowText);
    QAction *newSnippetAction = menu.addAction(
        UiIcons::symbol(this, UiIcons::Symbol::Plus, iconColor), tr("New snippet"));
    connect(newSnippetAction, &QAction::triggered,
            this, [this]() { openSnippetEditor(QString()); });
    if (!id.isEmpty()) {
        menu.addSeparator();
        QAction *executeAction = menu.addAction(
            UiIcons::symbol(this, UiIcons::Symbol::Play, iconColor), tr("Run"));
        QAction *copyAction = menu.addAction(
            UiIcons::symbol(this, UiIcons::Symbol::Copy, iconColor), tr("Copy"));
        QAction *editAction = menu.addAction(
            UiIcons::symbol(this, UiIcons::Symbol::Edit, iconColor), tr("Edit"));
        menu.addSeparator();
        QAction *deleteAction = menu.addAction(
            UiIcons::symbol(this, UiIcons::Symbol::Trash, iconColor), tr("Delete"));
        connect(executeAction, &QAction::triggered,
                this, [this, id]() { executeSnippetForId(id); });
        connect(copyAction, &QAction::triggered,
                this, [this, id]() { copySnippetForId(id); });
        connect(editAction, &QAction::triggered,
                this, [this, id]() { openSnippetEditor(id); });
        connect(deleteAction, &QAction::triggered,
                this, &ProductivityWidget::removeSelectedRecord);
    }
    menu.exec(globalPosition);
}

QWidget *ProductivityWidget::createHistoryRowWidget(const CommandHistoryEntry &entry,
                                                    const int sectionDay)
{
    auto *row = new QWidget(m_historyList);
    row->setProperty(kListRowProperty, true);
    row->installEventFilter(this);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(8, 6, 6, 6);
    rowLayout->setSpacing(10);

    rowLayout->addWidget(createRowTile(row, UiIcons::Symbol::Terminal, 24, 13), 0, Qt::AlignVCenter);

    auto *commandLabel = createElidedLabel(
        entry.command, QStringLiteral("ProductivityHistoryCommand"), row);
    commandLabel->setFont(monospaceFont(commandLabel->font(), -0.5));
    rowLayout->addWidget(commandLabel, 1);

    // 今天和昨天已经由分段标题表达，时间只需精确到分钟；更早的记录改显示日期。
    const QDateTime local = entry.executedAt.toLocalTime();
    const QString timeText = sectionDay == kSectionEarlier
        ? QLocale().toString(local.date(), QLocale::ShortFormat)
        : QLocale().toString(local.time(), QLocale::ShortFormat);
    auto *timeLabel = new QLabel(timeText, row);
    timeLabel->setObjectName(kRowTrailingName);
    timeLabel->setProperty("productivityCaption", true);
    timeLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    QFont timeFont = timeLabel->font();
    timeFont.setPointSizeF(std::max(7.0, timeFont.pointSizeF() - 1.5));
    timeLabel->setFont(timeFont);
    rowLayout->addWidget(timeLabel, 0, Qt::AlignVCenter);

    auto *actions = new QWidget(row);
    actions->setObjectName(kRowActionsName);
    auto *actionsLayout = new QHBoxLayout(actions);
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->setSpacing(4);
    auto *runButton = new QToolButton(actions);
    runButton->setObjectName(QStringLiteral("productivityHistoryRun"));
    configureRowRunButton(runButton, tr("Re-run the command on the host selected in the history filter"));
    auto *copyButton = new QToolButton(actions);
    copyButton->setObjectName(QStringLiteral("productivityHistoryCopy"));
    configureRowIconButton(copyButton, UiIcons::Symbol::Copy, tr("Copy command"));
    auto *saveButton = new QToolButton(actions);
    saveButton->setObjectName(QStringLiteral("productivityHistorySaveSnippet"));
    configureRowIconButton(saveButton, UiIcons::Symbol::Bookmark, tr("Save as snippet"));
    for (QToolButton *button : {runButton, copyButton, saveButton}) {
        actionsLayout->addWidget(button);
    }
    actions->setVisible(false);
    rowLayout->addWidget(actions, 0, Qt::AlignVCenter);

    const QString command = entry.command;
    connect(runButton, &QToolButton::clicked,
            this, [this, command]() { executeHistoryCommand(command); });
    connect(copyButton, &QToolButton::clicked, this, [this, command]() {
        if (QApplication::clipboard() != nullptr) {
            QApplication::clipboard()->setText(command);
        }
        emit statusMessage(tr("Command copied to clipboard"));
    });
    connect(saveButton, &QToolButton::clicked,
            this, [this, command]() { saveHistoryCommandAsSnippet(command); });
    return row;
}

void ProductivityWidget::executeHistoryCommand(const QString &command)
{
    if (command.isEmpty()) {
        return;
    }
    const QString hostId = m_historyHostCombo->currentData().toString();
    emit commandExecutionRequested(command,
                                   hostId.isEmpty() ? QStringList{} : QStringList{hostId});
}

void ProductivityWidget::saveHistoryCommandAsSnippet(const QString &command)
{
    // 先切回片段页再弹编辑框：保存后用户要能立刻看到新片段落在列表里。
    setCurrentView(View::Snippets);
    openSnippetEditor(QString());
    m_editorCommandEdit->setPlainText(command);
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
    m_hoveredRow.clear();
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
    int currentSection = -1;
    for (const CommandHistoryEntry &entry : history) {
        if (!entry.command.startsWith(prefix, Qt::CaseInsensitive) || shown.contains(entry.command)) {
            continue;
        }
        shown.insert(entry.command);

        // 历史按最近优先排列，分段只会依次推进，遇到新分段时插一行不可选的标题。
        const int section = historySectionFor(entry.executedAt);
        if (section != currentSection) {
            currentSection = section;
            const QString title = section == kSectionToday ? tr("Today")
                : section == kSectionYesterday             ? tr("Yesterday")
                                                           : tr("Earlier");
            auto *header = new QListWidgetItem(m_historyList);
            header->setFlags(Qt::NoItemFlags);
            auto *headerLabel = new QLabel(title, m_historyList);
            headerLabel->setProperty("sectionCaption", true);
            headerLabel->setContentsMargins(6, currentSection == kSectionToday ? 0 : 6, 0, 0);
            header->setSizeHint(headerLabel->sizeHint());
            m_historyList->setItemWidget(header, headerLabel);
        }

        auto *item = new QListWidgetItem(m_historyList);
        item->setData(Qt::UserRole, entry.command);
        // Qt 6.8 已移除旧的 Qt::DefaultLocaleShortDate 枚举；显式使用系统区域设置可继续
        // 尊重用户日期格式，同时避免把本地化字符串写死在业务模型中。
        item->setToolTip(QLocale().toString(entry.executedAt.toLocalTime(), QLocale::ShortFormat));
        QWidget *row = createHistoryRowWidget(entry, section);
        item->setSizeHint(row->sizeHint());
        m_historyList->setItemWidget(item, row);
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
