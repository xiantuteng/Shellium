#pragma once

#include <QList>
#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;

/**
 * @brief 终端会话里按 Cmd+K / Cmd+J 弹出的快速命令面板，也承载 Cmd+Shift+P 命令面板与
 *        标题栏的标签切换列表。
 *
 * 面板只负责展示与选择：上层把片段或历史整理成 Entry 列表交给 showEntries()，用户选中后
 * 通过 commandChosen 交回命令文本，由上层决定在哪个终端执行。面板不持有 TerminalPane，
 * 也不读数据库，因此同一个控件既服务片段又服务历史，测试也无需真实会话。
 *
 * 按键约定：Cmd+1..9（其他平台 Ctrl+1..9）直接执行当前可见列表中的第 1..9 项；裸数字照常
 * 输入搜索框；上下方向键移动选中项，回车执行选中项；Esc 或点击面板外关闭。
 *
 * 使用 Qt::Popup 顶层窗口：点击外部自动关闭、键盘被面板独占，终端不会在面板打开期间
 * 收到误按的数字。
 */
class QuickCommandPopup final : public QWidget {
    Q_OBJECT

public:
    /** @brief 面板展示的数据种类，决定标签、占位文字和空列表提示。 */
    enum class Mode {
        Snippets, ///< 命令片段（Cmd+K）。
        History,  ///< 当前主机的命令历史（Cmd+J）。
        Actions,  ///< 应用动作的命令面板（Cmd+Shift+P）；条目没有命令文本，只发 entryChosen。
        Tabs,     ///< 全部会话标签；条目没有命令文本，只发 entryChosen，初始选中当前标签。
    };

    /** @brief 列表中的一行。 */
    struct Entry {
        QString title;   ///< 主标题；片段为名称，历史为命令本身。
        QString detail;  ///< 次要说明；片段为命令预览，历史留空。
        QString command; ///< 选中后交给终端执行的完整命令；Actions 模式为空。
        QString shortcut = {}; ///< 行尾右对齐显示的快捷键，可为空。
        bool current = false;  ///< 当前对象（如活动标签）：标题高亮，未搜索时初始选中该行。
    };

    /** @brief Cmd+数字可直接执行的最大行数（1..9）。 */
    static constexpr int kMaxDigitEntries = 9;

    /**
     * @brief 创建面板；创建后保持隐藏，直到 showEntries() 被调用。
     * @param parent 通常是主窗口，只用于生命周期管理与定位。
     */
    explicit QuickCommandPopup(QWidget *parent = nullptr);

    /**
     * @brief 填充列表并在 anchor 上方居中弹出。
     * @param mode 决定界面文案。
     * @param entries 按展示顺序排列的条目；第 1 项对应数字键 1。
     * @param shortcutHint 搜索框右侧显示的快捷键提示，例如 "⌘K"；可为空。
     * @param anchor 用于定位的控件，通常是当前终端；为空时以父窗口定位。
     */
    void showEntries(Mode mode,
                     const QList<Entry> &entries,
                     const QString &shortcutHint,
                     QWidget *anchor);

    /**
     * @brief 第 digit 行执行快捷键的显示文字，角标与底部提示共用。
     * @param digit 1..9；越界返回空字符串。
     * @return macOS 为 "⌘1"，其他平台为 "Ctrl+1"。
     */
    [[nodiscard]] static QString digitShortcutText(int digit);

    /** @brief 当前展示模式，主要供测试与上层判断。 */
    [[nodiscard]] Mode mode() const { return m_mode; }

    /**
     * @brief 第 position 个可见条目的命令；position 从 0 开始。
     * @return 越界时返回空字符串。
     */
    [[nodiscard]] QString visibleCommandAt(int position) const;

    /** @brief 第 position 个可见条目的标题；越界返回空字符串。 */
    [[nodiscard]] QString visibleTitleAt(int position) const;

    /** @brief 当前可见条目数量。 */
    [[nodiscard]] int visibleCount() const;

    /** @brief 当前选中的可见位置（从 0 开始）；没有选中项时返回 -1。 */
    [[nodiscard]] int currentVisiblePosition() const;

signals:
    /**
     * @brief 用户选中一条命令；面板此时已经隐藏。
     * @param command 不带结尾换行的命令文本。
     */
    void commandChosen(const QString &command);

    /**
     * @brief 用户选中第 entryIndex 个条目（showEntries() 入参下标）；面板此时已经隐藏。
     *
     * 每次选中都会发出；命令非空时随后再发 commandChosen。Actions 模式只靠本信号定位动作。
     */
    void entryChosen(int entryIndex);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    /** @brief 按搜索框内容隐藏不匹配的行，并刷新每行的数字角标。 */
    void applyFilter();

    /** @brief 选中第 position 个可见条目并发出 commandChosen；越界时忽略。 */
    void chooseVisible(int position);

    /** @brief 执行当前选中行。 */
    void chooseCurrent();

    /** @brief 隐藏面板并为 item 发出 entryChosen / commandChosen；无效行忽略。 */
    void chooseItem(const QListWidgetItem *item);

    /** @brief 选中项在可见行中上下移动 delta 步。 */
    void moveSelection(int delta);

    /**
     * @brief 统一处理面板内的按键。
     * @return true 表示按键已被面板消费。
     */
    bool handleKey(QKeyEvent *event);

    Mode m_mode = Mode::Snippets;
    QList<Entry> m_entries;
    QLabel *m_chipLabel = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QLabel *m_shortcutLabel = nullptr;
    QLabel *m_sectionLabel = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QLabel *m_footerLabel = nullptr;
};
