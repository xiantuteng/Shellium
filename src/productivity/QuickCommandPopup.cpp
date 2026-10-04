#include "productivity/QuickCommandPopup.h"

#include "ui/FocusWithin.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kCommandRole = Qt::UserRole;      ///< 行对应的完整命令。
constexpr int kDetailRole = Qt::UserRole + 1;   ///< 次要说明文字。
constexpr int kDigitRole = Qt::UserRole + 2;    ///< 数字角标 1..9；0 表示该行没有数字键。
constexpr int kIndexRole = Qt::UserRole + 3;    ///< 行在 showEntries() 入参中的下标。
constexpr int kShortcutRole = Qt::UserRole + 4; ///< 行尾右对齐显示的快捷键文字。
constexpr int kCurrentRole = Qt::UserRole + 5;  ///< 行是否为当前对象（如活动标签）。

constexpr int kRowHeight = 36;
constexpr int kSearchHeight = 36; ///< 搜索栏固定高度，与参考设计一致，避免随字体撑高。
constexpr int kMaxVisibleRows = 9;
constexpr int kCornerRadius = 12;

// 面板与标题栏一样始终使用深色：它是浮在终端之上的临时 HUD，不随工作区深浅主题切换，
// 也就不会在浅色主题下与深色终端背景形成刺眼的白块。
const QColor kPanelColor(QStringLiteral("#21283b"));
const QColor kPanelBorderColor(QStringLiteral("#30384f"));
const QColor kRowSelectedColor(QStringLiteral("#39415d"));
const QColor kBadgeColor(QStringLiteral("#30384f"));
const QColor kTextColor(QStringLiteral("#f0f3fa"));
const QColor kMutedColor(QStringLiteral("#929bb3"));
const QColor kCurrentColor(QStringLiteral("#2bd981")); ///< 当前对象标题，与应用强调色一致。

/**
 * @brief 绘制"数字角标 + 标题 + 次要说明"的行。
 *
 * 用委托而不是每行一个 QWidget：历史可能有几十行，筛选时只改 item 数据即可重绘，
 * 不必反复创建和销毁子控件。
 */
class QuickCommandDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        Q_UNUSED(index)
        return QSize(option.rect.width(), kRowHeight);
    }

    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRectF rowRect = QRectF(option.rect).adjusted(0.0, 2.0, 0.0, -2.0);
        if (option.state.testFlag(QStyle::State_Selected)) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(kRowSelectedColor);
            painter->drawRoundedRect(rowRect, 9.0, 9.0);
        }

        const int digit = index.data(kDigitRole).toInt();
        QFont badgeFont = option.font;
        badgeFont.setBold(true);
        // 角标直接显示完整按键（"⌘1" / "Ctrl+1"）：只写数字会让人以为单按数字就能执行。
        // 宽度按最宽的 "…8" 统一计算，所有行的标题列因此严格对齐，第 10 行以后没有角标
        // 也照样留出同宽空位。
        const QFontMetrics badgeMetrics(badgeFont);
        const qreal badgeWidth = std::max(
            20.0, qreal(badgeMetrics.horizontalAdvance(QuickCommandPopup::digitShortcutText(8)))
                      + 12.0);
        const QRectF badgeRect(rowRect.left() + 10.0, rowRect.center().y() - 10.0,
                               badgeWidth, 20.0);
        if (digit > 0) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(kBadgeColor);
            painter->drawRoundedRect(badgeRect, 6.0, 6.0);
            painter->setFont(badgeFont);
            painter->setPen(kTextColor);
            painter->drawText(badgeRect, Qt::AlignCenter,
                              QuickCommandPopup::digitShortcutText(digit));
        }

        const QString title = index.data(Qt::DisplayRole).toString();
        const QString detail = index.data(kDetailRole).toString();
        const QString shortcut = index.data(kShortcutRole).toString();
        const qreal textLeft = badgeRect.right() + 14.0;
        QRectF textRect(textLeft, rowRect.top(), rowRect.right() - textLeft - 12.0,
                        rowRect.height());
        painter->setFont(option.font);
        // 必须用画笔实际字体的度量：样式表可能改了列表字体，option.font 量出的宽度会偏窄，
        // 短标题也被误省略。多留几像素抵消子像素取整。
        const QFontMetrics titleMetrics = painter->fontMetrics();
        if (!shortcut.isEmpty()) {
            // 快捷键贴右对齐并先占位，标题只在剩余宽度内省略：旧命令面板用制表符拼接名称和
            // 快捷键，名称长短不一时两列错位，长名称还会被快捷键盖住。
            const int shortcutWidth = titleMetrics.horizontalAdvance(shortcut) + 4;
            painter->setPen(kMutedColor);
            painter->drawText(QRectF(textRect.right() - shortcutWidth, textRect.top(),
                                     shortcutWidth, textRect.height()),
                              Qt::AlignVCenter | Qt::AlignRight, shortcut);
            textRect.setRight(textRect.right() - shortcutWidth - 16.0);
        }
        const int titleWidth = detail.isEmpty()
            ? int(textRect.width())
            : std::min(titleMetrics.horizontalAdvance(title) + 4, int(textRect.width() * 0.45));
        // 当前标签只靠初始选中不够：用户上下移动后选中框离开，仍要能一眼认出"我在哪"。
        painter->setPen(index.data(kCurrentRole).toBool() ? kCurrentColor : kTextColor);
        painter->drawText(QRectF(textRect.left(), textRect.top(), titleWidth, textRect.height()),
                          Qt::AlignVCenter | Qt::AlignLeft,
                          titleMetrics.elidedText(title, Qt::ElideRight, titleWidth));
        if (!detail.isEmpty()) {
            const qreal detailLeft = textRect.left() + titleWidth + 14.0;
            const QRectF detailRect(detailLeft, textRect.top(),
                                    textRect.right() - detailLeft, textRect.height());
            painter->setPen(kMutedColor);
            // 命令预览只显示首行：多行片段会把行高撑乱，完整内容在执行时才需要。
            const QString firstLine = detail.section(QLatin1Char('\n'), 0, 0).simplified();
            painter->drawText(detailRect, Qt::AlignVCenter | Qt::AlignLeft,
                              titleMetrics.elidedText(firstLine, Qt::ElideRight,
                                                      int(detailRect.width())));
        }
        painter->restore();
    }
};

/**
 * @brief 把 Cmd+1..9（其他平台 Ctrl+1..9，含小键盘）映射为 1..9；其他按键返回 0。
 *
 * 只认带 Command 的数字：裸数字要留给搜索框，否则用户无法搜索 "ping 10.0.0.1" 这类含数字
 * 的命令。Qt 在 macOS 把 Command 报告为 ControlModifier，物理 Control 是 MetaModifier，
 * 因此这里统一判断 ControlModifier 即可。
 */
[[nodiscard]] int digitForKey(const QKeyEvent *event)
{
    const Qt::KeyboardModifiers modifiers = event->modifiers() & ~Qt::KeypadModifier;
    if (modifiers != Qt::ControlModifier) {
        return 0;
    }
    if (event->key() >= Qt::Key_1 && event->key() <= Qt::Key_9) {
        return event->key() - Qt::Key_0;
    }
    return 0;
}

} // namespace

QuickCommandPopup::QuickCommandPopup(QWidget *parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint)
{
    setObjectName(QStringLiteral("QuickCommandPopup"));
    setAttribute(Qt::WA_TranslucentBackground, true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 8);
    layout->setSpacing(4);

    auto *searchFrame = new QWidget(this);
    searchFrame->setObjectName(QStringLiteral("QuickCommandSearchFrame"));
    searchFrame->setAttribute(Qt::WA_StyledBackground, true);
    FocusWithin::track(searchFrame);
    auto *searchLayout = new QHBoxLayout(searchFrame);
    searchFrame->setFixedHeight(kSearchHeight);
    searchLayout->setContentsMargins(8, 0, 12, 0);
    searchLayout->setSpacing(8);
    m_chipLabel = new QLabel(searchFrame);
    m_chipLabel->setObjectName(QStringLiteral("QuickCommandChip"));
    m_searchEdit = new QLineEdit(searchFrame);
    m_searchEdit->setObjectName(QStringLiteral("QuickCommandSearch"));
    m_searchEdit->setFrame(false);
    m_searchEdit->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_shortcutLabel = new QLabel(searchFrame);
    m_shortcutLabel->setObjectName(QStringLiteral("QuickCommandShortcut"));
    m_chipLabel->setFixedHeight(kSearchHeight - 12);
    searchLayout->addWidget(m_chipLabel, 0, Qt::AlignVCenter);
    searchLayout->addWidget(m_searchEdit, 1);
    searchLayout->addWidget(m_shortcutLabel);
    layout->addWidget(searchFrame);

    m_sectionLabel = new QLabel(this);
    m_sectionLabel->setObjectName(QStringLiteral("QuickCommandSection"));
    m_sectionLabel->setContentsMargins(8, 4, 0, 0);
    layout->addWidget(m_sectionLabel);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("QuickCommandList"));
    m_list->setItemDelegate(new QuickCommandDelegate(m_list));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setUniformItemSizes(true);
    layout->addWidget(m_list, 1);

    m_emptyLabel = new QLabel(this);
    m_emptyLabel->setObjectName(QStringLiteral("QuickCommandEmpty"));
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setMinimumHeight(kRowHeight * 2);
    layout->addWidget(m_emptyLabel);

    m_footerLabel = new QLabel(this);
    m_footerLabel->setObjectName(QStringLiteral("QuickCommandFooter"));
    m_footerLabel->setContentsMargins(8, 2, 0, 0);
    layout->addWidget(m_footerLabel);

    // 样式写在控件自身：面板是顶层 Popup，不能依赖主窗口子树的样式；但 qApp 级样式表仍会
    // 生效，因此这里用 objectName 选择器提高优先级，保证颜色不被全局 QLineEdit 规则覆盖。
    setStyleSheet(QStringLiteral(R"QSS(
        QWidget#QuickCommandSearchFrame {
            background: #293147;
            /* 常驻 1px 透明描边占位，聚焦时只换颜色，避免框内内容跳动。 */
            border: 1px solid transparent;
            border-radius: 9px;
        }
        QWidget#QuickCommandSearchFrame[focusWithin="true"] {
            border-color: #2bd981;
        }
        QLabel#QuickCommandChip {
            background: #39415d;
            color: #f0f3fa;
            border-radius: 12px;
            padding: 0 10px;
        }
        QLineEdit#QuickCommandSearch {
            background: transparent;
            border: 0;
            color: #f0f3fa;
            padding: 0;
            selection-background-color: #2f6256;
        }
        QLabel#QuickCommandShortcut,
        QLabel#QuickCommandSection,
        QLabel#QuickCommandEmpty,
        QLabel#QuickCommandFooter {
            background: transparent;
            color: #929bb3;
        }
        QListWidget#QuickCommandList {
            background: transparent;
            border: 0;
            outline: 0;
        }
    )QSS"));

    m_searchEdit->installEventFilter(this);
    m_list->installEventFilter(this);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this]() { applyFilter(); });
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        m_list->setCurrentItem(item);
        chooseCurrent();
    });
}

void QuickCommandPopup::showEntries(const Mode mode,
                                    const QList<Entry> &entries,
                                    const QString &shortcutHint,
                                    QWidget *anchor)
{
    m_mode = mode;
    m_entries = entries;
    if (mode == Mode::Actions) {
        m_chipLabel->setText(tr("Commands"));
        m_searchEdit->setPlaceholderText(tr("Search commands"));
        m_sectionLabel->setText(tr("Available commands"));
        m_emptyLabel->setText(tr("No commands are available right now."));
    } else if (mode == Mode::Tabs) {
        m_chipLabel->setText(tr("Tabs"));
        m_searchEdit->setPlaceholderText(tr("Search tabs"));
        m_sectionLabel->setText(tr("Open tabs"));
        m_emptyLabel->setText(tr("No open tabs."));
    } else if (mode == Mode::Snippets) {
        m_chipLabel->setText(tr("Snippets"));
        m_searchEdit->setPlaceholderText(tr("Search snippets"));
        m_sectionLabel->setText(tr("Snippets"));
        m_emptyLabel->setText(tr("No snippets yet. Add them in the Snippets sidebar."));
    } else {
        m_chipLabel->setText(tr("History"));
        m_searchEdit->setPlaceholderText(tr("Search command history"));
        m_sectionLabel->setText(tr("Recent commands"));
        m_emptyLabel->setText(tr("No command history for this session yet."));
    }
    // 与角标同源："⌘1–9"（macOS）或 "Ctrl+1–9"。
    const QString digitKeys = digitShortcutText(1) + QStringLiteral("–9");
    m_footerLabel->setText(mode == Mode::Tabs
                               ? tr("%1 switch · Enter switch to selected · Esc close").arg(digitKeys)
                               : tr("%1 run · Enter run selected · Esc close").arg(digitKeys));
    m_shortcutLabel->setText(shortcutHint);
    m_shortcutLabel->setVisible(!shortcutHint.isEmpty());

    m_list->clear();
    for (const Entry &entry : entries) {
        auto *item = new QListWidgetItem(entry.title, m_list);
        item->setData(kCommandRole, entry.command);
        item->setData(kDetailRole, entry.detail);
        item->setData(kShortcutRole, entry.shortcut);
        item->setData(kIndexRole, int(m_list->count() - 1));
        item->setData(kCurrentRole, entry.current);
        item->setToolTip(entry.command.isEmpty() ? entry.title : entry.command);
    }
    {
        // 清空搜索框会触发 applyFilter；这里随后再显式调用一次，保证文本本来就为空时也刷新。
        const QSignalBlocker blocker(m_searchEdit);
        m_searchEdit->clear();
    }
    applyFilter();

    // 以终端本身而不是整个窗口为参照居中：侧栏展开时窗口中心落在终端左侧，面板会明显偏左。
    QWidget *reference = anchor != nullptr ? anchor : parentWidget();
    QRect area = reference != nullptr
        ? QRect(reference->mapToGlobal(QPoint(0, 0)), reference->size())
        : QRect();
    if (area.isEmpty()) {
        const QScreen *screen = QApplication::primaryScreen();
        area = screen != nullptr ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    }
    // 宽度取窗口可用宽度的 70%，上下限同比例收窄（原 420..760）：面板只需容纳一行名称与
    // 命令预览，过宽会让视线在角标和命令之间来回横跳。
    const int width = std::clamp(int((area.width() - 120) * 0.7), 294, 532);
    const int rows = std::clamp(int(entries.size()), 2, kMaxVisibleRows);
    m_list->setFixedHeight(rows * kRowHeight + 4);
    adjustSize();
    resize(width, sizeHint().height());
    move(area.left() + (area.width() - width) / 2,
         area.top() + std::max(40, area.height() / 5));
    show();
    raise();
    activateWindow();
    m_searchEdit->setFocus(Qt::PopupFocusReason);
}

QString QuickCommandPopup::digitShortcutText(const int digit)
{
    if (digit < 1 || digit > kMaxDigitEntries) {
        return {};
    }
    // NativeText 在 macOS 给出 "⌘1"，其他平台给出 "Ctrl+1"，与系统菜单的快捷键写法一致。
    return QKeySequence(Qt::CTRL | Qt::Key(Qt::Key_0 + digit))
        .toString(QKeySequence::NativeText);
}

QString QuickCommandPopup::visibleCommandAt(const int position) const
{
    int visible = 0;
    for (int row = 0; row < m_list->count(); ++row) {
        const QListWidgetItem *item = m_list->item(row);
        if (item->isHidden()) {
            continue;
        }
        if (visible == position) {
            return item->data(kCommandRole).toString();
        }
        ++visible;
    }
    return {};
}

QString QuickCommandPopup::visibleTitleAt(const int position) const
{
    int visible = 0;
    for (int row = 0; row < m_list->count(); ++row) {
        const QListWidgetItem *item = m_list->item(row);
        if (item->isHidden()) {
            continue;
        }
        if (visible == position) {
            return item->text();
        }
        ++visible;
    }
    return {};
}

int QuickCommandPopup::currentVisiblePosition() const
{
    const QListWidgetItem *current = m_list->currentItem();
    int visible = 0;
    for (int row = 0; row < m_list->count(); ++row) {
        const QListWidgetItem *item = m_list->item(row);
        if (item->isHidden()) {
            continue;
        }
        if (item == current) {
            return visible;
        }
        ++visible;
    }
    return -1;
}

int QuickCommandPopup::visibleCount() const
{
    int visible = 0;
    for (int row = 0; row < m_list->count(); ++row) {
        if (!m_list->item(row)->isHidden()) {
            ++visible;
        }
    }
    return visible;
}

bool QuickCommandPopup::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == m_searchEdit || watched == m_list) && event->type() == QEvent::KeyPress) {
        if (handleKey(static_cast<QKeyEvent *>(event))) {
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QuickCommandPopup::keyPressEvent(QKeyEvent *event)
{
    if (!handleKey(event)) {
        QWidget::keyPressEvent(event);
    }
}

bool QuickCommandPopup::handleKey(QKeyEvent *event)
{
    if (const int digit = digitForKey(event); digit > 0) {
        chooseVisible(digit - 1);
        return true;
    }
    switch (event->key()) {
    case Qt::Key_Escape:
        hide();
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        chooseCurrent();
        return true;
    case Qt::Key_Up:
        moveSelection(-1);
        return true;
    case Qt::Key_Down:
        moveSelection(1);
        return true;
    default:
        return false;
    }
}

void QuickCommandPopup::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(kPanelBorderColor, 1.0));
    painter.setBrush(kPanelColor);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                            kCornerRadius, kCornerRadius);
}

void QuickCommandPopup::applyFilter()
{
    const QString keyword = m_searchEdit->text().trimmed();
    int visible = 0;
    QListWidgetItem *firstVisible = nullptr;
    QListWidgetItem *currentEntry = nullptr;
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem *item = m_list->item(row);
        const bool matches = keyword.isEmpty()
            || item->text().contains(keyword, Qt::CaseInsensitive)
            || item->data(kCommandRole).toString().contains(keyword, Qt::CaseInsensitive)
            || item->data(kDetailRole).toString().contains(keyword, Qt::CaseInsensitive)
            || item->data(kShortcutRole).toString().contains(keyword, Qt::CaseInsensitive);
        item->setHidden(!matches);
        // 角标跟随可见顺序重新编号：筛选后按 1 执行的就是眼前第一行，而不是原列表第一项。
        item->setData(kDigitRole, matches && visible < kMaxDigitEntries ? visible + 1 : 0);
        if (matches) {
            if (firstVisible == nullptr) {
                firstVisible = item;
            }
            if (currentEntry == nullptr && item->data(kCurrentRole).toBool()) {
                currentEntry = item;
            }
            ++visible;
        }
    }
    // 未输入关键字时停在当前对象上（标签列表打开即指向活动标签）；一旦开始搜索，回车应
    // 落在最匹配的第一行，而不是恰好也匹配的当前标签。
    QListWidgetItem *selected = keyword.isEmpty() && currentEntry != nullptr ? currentEntry
                                                                             : firstVisible;
    m_list->setCurrentItem(selected);
    if (selected != nullptr) {
        m_list->scrollToItem(selected);
    }
    m_list->setVisible(!m_entries.isEmpty());
    m_emptyLabel->setVisible(m_entries.isEmpty());
}

void QuickCommandPopup::chooseVisible(const int position)
{
    int visible = 0;
    for (int row = 0; row < m_list->count(); ++row) {
        const QListWidgetItem *item = m_list->item(row);
        if (item->isHidden()) {
            continue;
        }
        if (visible == position) {
            chooseItem(item);
            return;
        }
        ++visible;
    }
}

void QuickCommandPopup::chooseCurrent()
{
    chooseItem(m_list->currentItem());
}

void QuickCommandPopup::chooseItem(const QListWidgetItem *item)
{
    if (item == nullptr || item->isHidden()) {
        return;
    }
    const int entryIndex = item->data(kIndexRole).toInt();
    const QString command = item->data(kCommandRole).toString();
    if (m_mode != Mode::Actions && m_mode != Mode::Tabs && command.isEmpty()) {
        return;
    }
    // 先隐藏再发信号：被触发的动作可能弹出模态对话框，Popup 必须先交出键盘与焦点。
    hide();
    emit entryChosen(entryIndex);
    if (!command.isEmpty()) {
        emit commandChosen(command);
    }
}

void QuickCommandPopup::moveSelection(const int delta)
{
    const int count = m_list->count();
    if (count == 0) {
        return;
    }
    int row = m_list->currentRow();
    for (int step = 0; step < count; ++step) {
        row += delta;
        if (row < 0 || row >= count) {
            return;
        }
        if (!m_list->item(row)->isHidden()) {
            m_list->setCurrentRow(row);
            return;
        }
    }
}
