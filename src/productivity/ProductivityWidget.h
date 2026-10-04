#pragma once

#include "core/ConnectionTarget.h"
#include "core/HostRecord.h"

#include <QWidget>

class ProductivityModel;
class QComboBox;
class QDialog;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QShortcut;
class QTabWidget;
class QTextEdit;
class QToolButton;

/**
 * @brief Snippets、命令历史和自动补全入口的独立效率工具侧栏。
 *
 * 控件不持有 TerminalPane，也不直接向网络写数据。所有执行行为都通过携带稳定主机 id 的
 * 信号交给上层路由，因此同一控件既能嵌入 MainWindow，也能在测试或未来浮动窗口中复用。
 * 数据读写统一经过 ProductivityModel，界面不会直接拼 SQL。
 *
 * Snippet 分组功能已移除：所有片段都是根层级的扁平列表，新增/编辑改用弹出对话框，
 * 列表本身支持拖拽排序；片段始终在当前活动终端执行，不再提供多目标勾选框。
 *
 * 原先的"广播输入"标签页已移除：向多个终端同步输入统一由工作空间标题栏的广播开关负责，
 * 侧栏不再维护第二套目标勾选与实时广播状态。整体样式对齐"主机"侧栏：主操作放进共享
 * 标题行、搜索框与主机搜索框同款、列表行使用同一套悬停/选中圆角卡片。
 */
class ProductivityWidget final : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief 创建效率工具控件。
     * @param model 可复用业务模型，生命周期必须覆盖本控件；允许为空，之后用 setModel 注入。
     * @param parent Qt 父对象。
     */
    explicit ProductivityWidget(ProductivityModel *model = nullptr,
                                QWidget *parent = nullptr);

    /**
     * @brief 返回需要挂到侧栏共享标题行右侧的动作条（新建片段 + 删除）。
     *
     * 与 HostManagerWidget::headerActionsWidget() 同一模式：控件由本类创建并持有逻辑，
     * MainWindow 只负责摆放，并在切到其他侧栏页面时隐藏。
     */
    [[nodiscard]] QWidget *headerActionsWidget() const;

    /** @brief 切换业务模型并重新加载 Snippet 和历史。 */
    void setModel(ProductivityModel *model);

    /**
     * @brief 更新当前可执行目标。
     * @param targets 通常对应已打开且可交互的终端；id 应使用持久化主机主键。
     *
     * Snippet 始终在当前活动终端执行，不再使用该列表；只有历史筛选的主机下拉框从这里填充。
     */
    void setExecutionTargets(const QList<ConnectionTarget> &targets);

    /** @brief 从模型重新加载全部可见数据；数据库切换或外部写入后调用。 */
    void refresh();

signals:
    /**
     * @brief 请求在一个或多个终端执行完整命令。
     * @param command 不带结尾换行的命令文本。
     * @param targetIds 选中的稳定主机 id；空列表表示由上层使用当前终端。
     */
    void commandExecutionRequested(const QString &command, const QStringList &targetIds);

    /** @brief 可展示在主窗口顶部非阻塞提示区的非致命结果或错误信息。 */
    void statusMessage(const QString &message);

private:
    /** @brief 创建标题行动作条、两个标签页及其表单，构造函数只调用一次。 */
    void buildUi();

    /** @brief 创建新增/编辑片段弹窗，仅调用一次；随后每次复用并重置内容。 */
    void buildEditorDialog();

    /** @brief 重新从模型加载扁平片段列表。 */
    void reloadSnippets();

    /** @brief 按搜索框内容隐藏不匹配的片段行；筛选期间关闭拖拽，避免把局部顺序写回。 */
    void applySnippetFilter();

    /** @brief 为单个片段构造带"执行/复制"图标按钮的列表行控件。 */
    QWidget *createSnippetRowWidget(const SnippetRecord &record, int shortcutIndex);

    /** @brief 打开编辑弹窗并预填指定片段；id 为空表示新建。 */
    void openSnippetEditor(const QString &id);

    /** @brief 校验弹窗内容并保存，成功后关闭弹窗。 */
    void saveEditorRecord();

    /** @brief 明确确认后删除当前选中片段。 */
    void removeSelectedRecord();

    /** @brief 执行列表中指定行的 Snippet；供数字快捷键使用，内部转发到 executeSnippetForId。 */
    void executeSnippetAtRow(int row);

    /** @brief 按稳定 id 执行 Snippet；供行内按钮和快捷键共用，不受列表重排影响。 */
    void executeSnippetForId(const QString &id);

    /** @brief 把指定 id 片段的命令复制到剪贴板。 */
    void copySnippetForId(const QString &id);

    /** @brief 根据当前主机和过滤前缀刷新历史列表。 */
    void reloadHistory();

    /** @brief 用最新目标快照重建历史筛选的主机下拉框，尽量保留原选中项。 */
    void populateHistoryHostSelector();

private slots:
    /**
     * @brief 拖拽排序结束后，把当前显示顺序回写为 sortOrder 并刷新列表。
     *
     * 声明为 slot（而非普通私有方法）是为了让测试可以通过 QMetaObject::invokeMethod
     * 直接触发，不必真的驱动一次原生拖拽手势。
     */
    void persistSnippetOrder();

private:
    ProductivityModel *m_model = nullptr; ///< 非拥有指针；业务模型通常由 MainWindow 持有。
    QList<ConnectionTarget> m_targets;     ///< 最近一次由上层提供的可执行目标快照。

    QWidget *m_headerActions = nullptr; ///< 挂到侧栏标题行的动作条；见 headerActionsWidget()。
    QTabWidget *m_tabs = nullptr;
    QLineEdit *m_snippetSearchEdit = nullptr;
    QListWidget *m_snippetList = nullptr;

    QDialog *m_editorDialog = nullptr;
    QLineEdit *m_editorNameEdit = nullptr;
    /**
     * @brief 命令正文编辑器。
     *
     * 命令可能包含较长的管道和参数，使用多行编辑器可以自然换行，避免用户只能依靠水平
     * 滚动检查内容。
     */
    QTextEdit *m_editorCommandEdit = nullptr;

    /** @brief 片段的一行简短说明；列表预览会优先展示该字段。 */
    QLineEdit *m_editorDescriptionEdit = nullptr;
    QPushButton *m_editorSaveButton = nullptr;

    /** @brief Ctrl+1..Ctrl+9,Ctrl+0 对应列表前 10 个片段的执行快捷键，构造时一次性创建。 */
    QList<QShortcut *> m_snippetShortcuts;

    QComboBox *m_historyHostCombo = nullptr;
    QLineEdit *m_historyPrefixEdit = nullptr;
    QListWidget *m_historyList = nullptr;

    QString m_editingId; ///< 当前弹窗正在编辑的片段 id；空表示新建。
};
