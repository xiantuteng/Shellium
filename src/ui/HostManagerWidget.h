#pragma once

#include "core/HostRecord.h"

#include <QWidget>

class ApplicationDatabase;
class CredentialVault;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QTreeWidget;
class QTreeWidgetItem;

/**
 * @brief Shellium 左侧的持久化主机管理面板。
 *
 * 本控件把 ApplicationDatabase 已有的主机、分组、搜索和最近连接能力组成一个完整的
 * 用户界面。控件只负责编辑与选择，不直接持有终端或凭据明文；真正的连接由 MainWindow
 * 收到 hostConnectionRequested 信号后发起，从而避免主机树与某个标签页耦合。
 */
class HostManagerWidget final : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief 创建主机管理面板并立即从数据库加载树和最近连接。
     * @param database 已初始化的应用数据库；生命周期必须覆盖本控件。
     * @param parent Qt 父对象。
     */
    explicit HostManagerWidget(ApplicationDatabase *database,
                               CredentialVault *credentialVault,
                               QWidget *parent = nullptr);

    /** @brief 重新读取主机树和最近连接，供连接历史变化后主动刷新。 */
    void refresh();

signals:
    /** @brief 主机或分组列表已重新加载，依赖 hostId 的其他侧栏应同步刷新。 */
    void hostsChanged();

    /**
     * @brief 用户要求连接一条已保存的主机记录。
     * @param hostId 数据库 hosts 表中的稳定主键；接收方应再次 resolveHost，不能信任旧快照。
     * @param openInNewTab true 表示应在新标签页中连接（双击触发），而不是复用当前标签页。
     */
    void hostConnectionRequested(const QString &hostId, const ConnectionTarget &sessionTarget,
                                 bool openInNewTab);

private:
    /** @brief 从数据库递归加载某个分组的直接子节点。 */
    bool appendChildren(QTreeWidgetItem *parentItem,
                        const QString &parentId,
                        QString *errorMessage);

    /** @brief 根据搜索框内容展示完整树、名称地址搜索结果或精确标签结果。 */
    void applyFilter();

    /** @brief 弹出名称输入框并在当前分组下新建子分组。 */
    void addGroup();

    /** @brief 弹出连接配置框并在当前分组下保存一台新主机。 */
    void addHost();

    /** @brief 编辑当前选中的分组名称或主机连接参数。 */
    void editSelected();

    /** @brief 经明确风险确认后删除当前节点；删除分组会级联删除其全部子节点。 */
    void removeSelected();

    /** @brief 从 OpenSSH config 或 Shellium JSON 导入主机，并以单个事务写入数据库。 */
    void importHosts();

    /** @brief 把全部主机和分组导出为不含凭据的 Shellium JSON。 */
    void exportHosts();

    /** @brief 处理树或最近列表双击，只允许可连接的 Host 节点发出请求；双击总是新开标签页。 */
    void activateTreeItem(QTreeWidgetItem *item);
    void activateRecentItem(QListWidgetItem *item);

    /**
     * @brief 解析持久化主机并发出连接请求。
     * @param hostId 数据库稳定主键。
     * @param sessionTarget 可选的本次会话覆盖值；新建主机时用于携带不落库的临时密码。
     * @param openInNewTab true 表示应在新标签页中连接。
     */
    void requestConnection(const QString &hostId,
                           const ConnectionTarget &sessionTarget = ConnectionTarget{},
                           bool openInNewTab = false);

    /** @brief 返回当前选中节点；没有选择时返回空记录。 */
    [[nodiscard]] std::optional<HostRecord> selectedRecord(QString *errorMessage = nullptr) const;

    /** @brief 返回新节点应归属的父分组；选中主机时使用其所在分组。 */
    [[nodiscard]] QString selectedParentGroupId() const;

    /** @brief 把一条主机/分组记录转换为树节点，并保存稳定 id 与类型到 UserRole。 */
    [[nodiscard]] QTreeWidgetItem *createTreeItem(const HostRecord &record) const;

    /** @brief 按父节点在前的顺序递归收集完整主机树，供安全导出使用。 */
    bool collectRecords(const QString &parentId,
                        QList<HostRecord> *records,
                        QString *errorMessage) const;

    /** @brief 统一展示数据库错误，避免失败被误当作空列表。 */
    void showDatabaseError(const QString &operation, const QString &details) const;

    ApplicationDatabase *m_database = nullptr;
    CredentialVault *m_credentialVault = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QTreeWidget *m_tree = nullptr;
    QListWidget *m_recentList = nullptr;
};
