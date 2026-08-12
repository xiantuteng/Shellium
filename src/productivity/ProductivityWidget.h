#pragma once

#include "core/ConnectionTarget.h"
#include "core/HostRecord.h"

#include <QWidget>

class ProductivityModel;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTabWidget;
class QTextEdit;
class QTreeWidget;
class QTreeWidgetItem;

/**
 * @brief Snippets、命令历史、自动补全入口和广播输入的独立效率工具侧栏。
 *
 * 控件不持有 TerminalPane，也不直接向网络写数据。所有执行行为都通过携带稳定主机 id 的
 * 信号交给上层路由，因此同一控件既能嵌入 MainWindow，也能在测试或未来浮动窗口中复用。
 * 数据读写统一经过 ProductivityModel，界面不会直接拼 SQL。
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

    /** @brief 切换业务模型并重新加载 Snippet 和历史。 */
    void setModel(ProductivityModel *model);

    /**
     * @brief 更新当前可执行目标。
     * @param targets 通常对应已打开且可交互的终端；id 应使用持久化主机主键。
     *
     * 选中多个目标后执行 Snippet 即形成多主机执行入口；广播模式同样只会向选中的 id
     * 发信号。空选择由上层解释为“当前终端”，不会在本控件内猜测当前标签页。
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

    /**
     * @brief 用户开启或关闭实时广播模式。
     * @param enabled true 表示把源终端后续原始键盘字节复制给目标终端。
     * @param targetIds 广播目标稳定主机 id；为空时上层应拒绝开启并提示用户选择目标。
     */
    void broadcastModeChanged(bool enabled, const QStringList &targetIds);

    /**
     * @brief 请求一次性向多个终端发送原始字节。
     * @param bytes 已编码为 UTF-8 的输入；发送命令时末尾包含回车字节。
     * @param targetIds 广播目标稳定主机 id。
     */
    void broadcastInputRequested(const QByteArray &bytes, const QStringList &targetIds);

    /** @brief 可展示在主窗口顶部非阻塞提示区的非致命结果或错误信息。 */
    void statusMessage(const QString &message);

private:
    /** @brief 创建三个标签页及其表单，构造函数只调用一次。 */
    void buildUi();

    /** @brief 重新构造 Snippet 树和父分组选择框。 */
    void reloadSnippets();

    /** @brief 递归加载某个分组；visited 防止旧损坏数据造成无限递归。 */
    bool appendSnippetChildren(QTreeWidgetItem *parentItem,
                               const QString &parentId,
                               QSet<QString> *visited);

    /** @brief 把当前树节点内容填入编辑表单。 */
    void editTreeItem(QTreeWidgetItem *item);

    /** @brief 开始创建一条未保存的新节点。 */
    void beginNewRecord(SnippetNodeKind kind);

    /** @brief 校验表单并保存当前新建或编辑记录。 */
    void saveEditorRecord();

    /** @brief 明确确认后删除当前树节点；分组会连同后代删除。 */
    void removeSelectedRecord();

    /** @brief 执行当前选中的可执行 Snippet；分组不会发出执行信号。 */
    void executeSelectedSnippet();

    /** @brief 根据当前主机和过滤前缀刷新历史列表。 */
    void reloadHistory();

    /** @brief 从带复选框的目标列表提取稳定 id，保持界面顺序并去重。 */
    [[nodiscard]] static QStringList checkedTargetIds(const QListWidget *list);

    /** @brief 将目标同时填充到执行列表、历史筛选和广播列表。 */
    void populateTargetSelectors();

    ProductivityModel *m_model = nullptr; ///< 非拥有指针；业务模型通常由 MainWindow 持有。
    QList<ConnectionTarget> m_targets;     ///< 最近一次由上层提供的可执行目标快照。

    QTabWidget *m_tabs = nullptr;
    QTreeWidget *m_snippetTree = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QComboBox *m_parentGroupCombo = nullptr;
    /**
     * @brief 命令正文编辑器。
     *
     * 命令可能包含较长的管道和参数，使用多行编辑器可以在窄侧栏内自然换行，避免用户
     * 只能依靠水平滚动检查内容。
     */
    QTextEdit *m_commandEdit = nullptr;

    /** @brief 片段的一行简短说明；列表预览会优先展示该字段。 */
    QLineEdit *m_descriptionEdit = nullptr;
    QPushButton *m_saveButton = nullptr;
    QListWidget *m_executionTargets = nullptr;

    QComboBox *m_historyHostCombo = nullptr;
    QLineEdit *m_historyPrefixEdit = nullptr;
    QListWidget *m_historyList = nullptr;

    QListWidget *m_broadcastTargets = nullptr;
    QCheckBox *m_broadcastEnabled = nullptr;
    QLineEdit *m_broadcastInput = nullptr;

    QString m_editingId; ///< 当前编辑记录 id；新记录在 beginNewRecord 时立即生成稳定 UUID。
    SnippetNodeKind m_editingKind = SnippetNodeKind::Snippet;
};
