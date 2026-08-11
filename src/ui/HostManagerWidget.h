#pragma once

#include "core/HostRecord.h"

#include <QWidget>

class ApplicationDatabase;
class CredentialVault;
class QLineEdit;
class QMenu;
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

    /** @brief 重新读取主机树，供连接历史变化后主动刷新。 */
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

    /** @brief 根据搜索框内容展示完整树、名称地址搜索结果或精确标签结果。 */
    void applyFilter();

    /** @brief 弹出名称输入框并在当前分组下新建子分组。 */
    void addGroup();

    /** @brief 弹出连接配置框并在当前分组下保存一台新主机。 */
    void addHost();

    /**
     * @brief 在原主机所属分组中复制当前选中的主机。
     *
     * 复制会保留连接参数、凭据引用、标签和主机链等业务配置，只重新生成稳定主键，并按
     * 当前分组已有名称依次追加“-1”“-2”等可用序号。分组节点和“未分组”展示标题不
     * 属于可复制主机，会被安全忽略。
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
    QTreeWidget *m_tree = nullptr;
    QWidget *m_headerActions = nullptr;
    /** @brief 最近一次完整统计得到的主机数量；搜索过滤不会改变该值。 */
    int m_hostCount = 0;
};
