#pragma once

#include "core/HostRecord.h"

#include <QHash>
#include <QWidget>

class ApplicationDatabase;
class CredentialVault;
class QFrame;
class QLineEdit;
class QMenu;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

/**
 * @brief Shellium 左侧的持久化主机管理面板。
 *
 * 本控件把 ApplicationDatabase 已有的主机、分组与搜索能力组成一个完整的
 * 用户界面。控件只负责编辑与选择，不直接持有终端或凭据明文；真正的连接由 MainWindow
 * 收到 hostConnectionRequested 信号后发起，从而避免主机树与某个标签页耦合。
 */
class HostManagerWidget final : public QWidget {
    Q_OBJECT

    // 界面回归测试需要直接验证拖放持久化的私有边界：测试只操作内存数据库，不会把测试
    // 入口暴露成正式 API。使用友元比为了测试扩大 reindexChildren() 的生产可见性更稳妥。
    friend class HostManagerWidgetTest;

public:
    /**
     * @brief 创建主机管理面板并立即从数据库加载主机树。
     * @param database 已初始化的应用数据库；生命周期必须覆盖本控件。
     * @param parent Qt 父对象。
     */
    explicit HostManagerWidget(ApplicationDatabase *database,
                               CredentialVault *credentialVault,
                               QWidget *parent = nullptr);

    /**
     * @brief 重新读取主机树，并按数据库稳定主键恢复刷新前选中的主机或分组。
     *
     * 主机树刷新会销毁旧 QTreeWidgetItem，因此不能依赖节点指针保留界面状态。本方法会在
     * 重建前记录当前记录 id 和垂直滚动位置，重建后恢复高亮、当前项，并按持久化偏好恢复
     * 各目录的展开/折叠状态；若记录已被删除或不再满足当前搜索条件，则保持无选中项，避免
     * 错误选中另一条记录。
     */
    void refresh();

    /**
     * @brief 返回数据库中的可连接主机总数，不包含仅用于组织的分组。
     * @return 最近一次 refresh() 成功统计出的主机数量；数据库读取失败时保留上次有效值。
     */
    [[nodiscard]] int hostCount() const;

    /**
     * @brief 返回承载新建主机/新建分组/刷新的动作条。
     *
     * 侧栏标题由 MainWindow 统一渲染，主机页的主要动作需要与标题同排右对齐。把动作条作为
     * 独立控件交出去，标题栏就不必知道主机页内部有哪些动作。
     */
    [[nodiscard]] QWidget *headerActionsWidget() const;

    /**
     * @brief 递归收集某个分组下的全部可连接主机（含任意深度子分组）。
     * @param groupId 分组稳定 id；传空字符串表示根层级。
     * @param hosts 输出参数，按数据库原始顺序追加，只包含 Host 类型记录。
     * @param errorMessage 失败时写入数据库错误详情。
     * @return 成功返回 true；数据库读取失败返回 false 且不修改 hosts。
     */
    bool collectHostsInGroup(const QString &groupId,
                             QList<HostRecord> *hosts,
                             QString *errorMessage) const;

signals:
    /** @brief 主机或分组列表已重新加载，依赖 hostId 的其他侧栏应同步刷新。 */
    void hostsChanged();

    /**
     * @brief 主机总数发生变化，侧栏标题可据此同步“主机 N 台”摘要。
     * @param count 数据库中 Host 类型记录的总数，不包含分组与展示伪节点。
     */
    void hostCountChanged(int count);

    /**
     * @brief 用户要求连接一条已保存的主机记录。
     * @param hostId 数据库 hosts 表中的稳定主键；接收方应再次 resolveHost，不能信任旧快照。
     * @param connectionPlan 已解析但尚未读取保险库凭据的外层到最终目标计划；新建主机后的
     *        最后一层可能携带仅供本次连接使用的临时密码。
     * @param openInNewTab true 表示应在新标签页中连接（双击触发），而不是复用当前标签页。
     */
    void hostConnectionRequested(const QString &hostId,
                                 const QList<ConnectionTarget> &connectionPlan,
                                 bool openInNewTab);

    /**
     * @brief 用户要求把某个分组下的全部主机一次性连接到同一个新工作空间。
     * @param groupId 数据库中该分组的稳定主键。
     * @param groupName 分组名称，接收方据此命名新建的工作空间标签。
     */
    void hostGroupConnectionRequested(const QString &groupId, const QString &groupName);

private:
    /** @brief 从数据库递归加载某个分组的直接子节点。 */
    bool appendChildren(QTreeWidgetItem *parentItem,
                        const QString &parentId,
                        QString *errorMessage);

    /**
     * @brief 加载根层级：先铺全部顶层分组（及其子树），再铺一个"未分组"分区标题，最后
     * 铺没有父分组的主机。
     *
     * appendChildren() 按数据库原始 sortOrder 把顶层分组和顶层主机混在一起追加，在分组
     * 较少、主机较多时会让分组散落在主机中间，不利于识别"这是组织结构还是可连接目标"。
     */
    bool appendRootLevel(QString *errorMessage);

    /**
     * @brief 从应用设置表加载各主机目录上次保存的展开/折叠状态。
     *
     * 状态以目录的数据库稳定 id 为键，因此目录改名、排序或移动后仍能命中原记录。设置不存在、
     * 版本不兼容或内容损坏时采用“目录默认展开”的兼容行为，避免纯界面偏好阻断主机列表启动。
     */
    void loadFolderExpansionState();

    /**
     * @brief 把已加载的目录展开状态应用到刚重建完成的完整主机树。
     *
     * 恢复期间会阻断树的展开/折叠信号，防止程序恢复动作被误判成用户操作并立即反向写库。
     * 尚无历史记录的新目录默认展开，以延续 Shellium 原有行为并确保新建内容立即可见。
     */
    void restoreFolderExpansionState();

    /**
     * @brief 记录并立即持久化一次目录展开状态变化。
     * @param item 发生展开或折叠的树节点；非目录或缺少稳定 id 的展示节点会被安全忽略。
     * @param expanded true 表示展开，false 表示折叠。
     *
     * Qt 的 itemExpanded/itemCollapsed 同时覆盖鼠标、键盘和辅助功能入口，因此统一监听这两个
     * 信号比只在自绘箭头点击回调中保存更完整。写入失败会向用户显示数据库错误，不静默丢状态。
     */
    void persistFolderExpansionState(QTreeWidgetItem *item, bool expanded);

    /** @brief 根据搜索框内容展示完整树、名称地址搜索结果或精确标签结果。 */
    void applyFilter();

    /**
     * @brief 按搜索框当前的"#前缀"重建标签建议悬浮面板，并据此显示或隐藏它。
     *
     * 面板列出库中全部已知标签（含仅剩配色、已无主机在用的历史标签），随"#"后的输入
     * 实时按前缀过滤，点击某个胶囊等价于手动敲完整个"#标签名"。
     */
    void updateTagSuggestions();

    /** @brief 隐藏标签建议悬浮面板；面板本身不销毁，供下次显示复用。 */
    void hideTagSuggestions();

    /** @brief 把一个已知标签渲染成建议面板里可点击的胶囊按钮。 */
    [[nodiscard]] QToolButton *buildTagSuggestionChip(const QString &tag);

    /** @brief 拦截全局鼠标点击与搜索框的 Esc 键，用于在点击面板外或取消时收起建议面板。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

    /** @brief 弹出名称输入框并在当前分组下新建子分组。 */
    void addGroup();

    /** @brief 弹出连接配置框并在当前分组下保存一台新主机。 */
    void addHost();

    /**
     * @brief 在原主机所属分组中复制当前选中的主机。
     *
     * 复制会保留连接参数、凭据内容、标签和主机链等业务配置，但为副本生成新的主机 id 和
     * 独立 credentialId。凭据只复制密文及公开元数据，不读取明文；这样后续修改副本密码
     * 不会覆盖原主机。名称按当前分组已有记录依次追加“-1”“-2”等可用序号。
     */
    void copySelectedHost();

    /** @brief 编辑当前选中的分组名称或主机连接参数。 */
    void editSelected();

    /** @brief 经明确风险确认后删除当前节点；删除分组会级联删除其全部子节点。 */
    void removeSelected();

    /** @brief 从 OpenSSH config 或 Shellium JSON 导入主机，并以单个事务写入数据库。 */
    void importHosts();

    /** @brief 把全部主机和分组导出为不含凭据的 Shellium JSON。 */
    void exportHosts();

    /** @brief 处理树双击，只允许可连接的 Host 节点发出请求；双击总是新开标签页。 */
    void activateTreeItem(QTreeWidgetItem *item);

    /**
     * @brief 解析持久化主机并发出连接请求。
     * @param hostId 数据库稳定主键。
     * @param sessionTarget 可选的最终目标会话覆盖值；新建主机时用于携带不落库的临时密码。
     * @param openInNewTab true 表示应在新标签页中连接。
     */
    void requestConnection(const QString &hostId,
                           const ConnectionTarget &sessionTarget = ConnectionTarget{},
                           bool openInNewTab = false);

    /** @brief 返回当前选中节点；没有选择时返回空记录。 */
    [[nodiscard]] std::optional<HostRecord> selectedRecord(QString *errorMessage = nullptr) const;

    /** @brief 返回新节点应归属的父分组；选中主机时使用其所在分组。 */
    [[nodiscard]] QString selectedParentGroupId() const;

    /**
     * @brief 根据右键命中的树节点填充主机管理上下文菜单。
     * @param menu 接收动作的菜单对象；调用方负责生命周期和最终显示。
     * @param item 右键命中的节点；为空时表示点击树的空白区域。
     *
     * 将菜单构造独立出来，既避免弹出逻辑与动作顺序混杂，也让界面回归测试无需真的打开
     * 原生弹出窗口即可核对菜单内容。动作仍统一调用面板方法，不复制业务实现。
     */
    void populateContextMenu(QMenu *menu, QTreeWidgetItem *item);

    /** @brief 把一条主机/分组记录转换为树节点，并保存稳定 id 与类型到 UserRole。 */
    [[nodiscard]] QTreeWidgetItem *createTreeItem(const HostRecord &record) const;

    /** @brief 按父节点在前的顺序递归收集完整主机树，供安全导出使用。 */
    bool collectRecords(const QString &parentId,
                        QList<HostRecord> *records,
                        QString *errorMessage) const;

    /** @brief 统一展示数据库错误，避免失败被误当作空列表。 */
    void showDatabaseError(const QString &operation, const QString &details) const;

    /**
     * @brief 拖放完成后持久化受影响节点的 parentId 与 sortOrder。
     * @param movedItem 被拖动的节点，此时已经挂在 Qt 内部完成移动后的新位置。
     * @param oldParentItem 拖动前的父节点；根层级为空指针。
     */
    void persistTreeReorder(QTreeWidgetItem *movedItem, QTreeWidgetItem *oldParentItem);

    /**
     * @brief 把某个父节点（或根层级，parentItem 为空）下的直接子节点顺序写回数据库。
     *
     * 根层级允许包含不对应数据库记录的“未分组”展示标题；该伪节点会被跳过，并且不会
     * 占用 sortOrder。真实节点缺少 id 时仍按错误处理，防止损坏数据被静默忽略。
     * @return 全部子节点写入成功返回 true；中途失败时终止并通过 errorMessage 返回原因。
     */
    bool reindexChildren(QTreeWidgetItem *parentItem, QString *errorMessage);

    ApplicationDatabase *m_database = nullptr;
    CredentialVault *m_credentialVault = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QFrame *m_searchBar = nullptr;
    QTreeWidget *m_tree = nullptr;
    QWidget *m_headerActions = nullptr;
    /** @brief 标签建议悬浮面板；懒加载，构造时创建一次，之后按内容重建胶囊并显示/隐藏。 */
    QFrame *m_tagSuggestionsPopup = nullptr;
    /** @brief 承载建议面板内标签胶囊的流式布局容器。 */
    QWidget *m_tagSuggestionsFlow = nullptr;
    /**
     * @brief 目录稳定 id 到展开状态的内存快照。
     *
     * 该快照既承接启动加载，也会在每次真实操作后整体写回 settings 表。没有出现在快照中的
     * 新目录按展开处理，避免应用升级后已有用户配置让后来创建的目录意外默认折叠。
     */
    QHash<QString, bool> m_folderExpansionState;
    /**
     * @brief 标签名到 "#rrggbb" 的配色快照，每次重建主机树前刷新一次。
     *
     * 缓存的原因是配色存放在 settings 表：若在 createTreeItem() 里按节点读取，一棵有上百
     * 台主机的树每次刷新都会产生上百次相同的 SQL 查询。
     */
    QHash<QString, QString> m_tagColors;
    /** @brief 最近一次完整统计得到的主机数量；搜索过滤不会改变该值。 */
    int m_hostCount = 0;
};
