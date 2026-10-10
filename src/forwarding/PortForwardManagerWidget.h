#pragma once

#include "platform/NativeSocket.h"

#include "forwarding/PortForwardRule.h"

#include <QHash>
#include <QPointer>
#include <QWidget>

class ApplicationDatabase;
class PortForwardRuntime;
class QComboBox;
class QLabel;
class QListWidget;
class SshSessionStack;

/**
 * @brief 端口转发规则 CRUD、会话绑定、一键启停与实时统计侧栏。
 *
 * 规则以持久化 hostId 归属主机，不以易变化的地址字符串归属。一个主机打开多个标签时，
 * 界面只展示一份规则；启动时选择第一条仍存活的根会话，断开后若还有同主机会话且规则
 * 配置为自动启动，则自动迁移并重启。
 */
class PortForwardManagerWidget final : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief 创建端口转发管理侧栏并加载保存主机。
     * @param database 已初始化数据库，生命周期必须覆盖本控件。
     * @param parent Qt 父对象。
     */
    explicit PortForwardManagerWidget(ApplicationDatabase *database,
                                      QWidget *parent = nullptr);
    ~PortForwardManagerWidget() override;

    /**
     * @brief 登记一条已经认证成功的根 SSH 会话，并启动该主机的 autoStart 规则。
     * @param hostId 持久化主机 id；临时连接不得传入。
     * @param sessionStack 与该根连接对应的 SSH 栈；不转移所有权。
     */
    void registerSession(const QString &hostId, SshSessionStack *sessionStack);

    /**
     * @brief 注销一条即将断开的根会话并停止依赖它的运行时。
     * @param hostId 持久化主机 id。
     * @param sessionStack 原先登记的准确对象，防止误删同主机另一个标签的会话。
     */
    void unregisterSession(const QString &hostId, SshSessionStack *sessionStack);

    /** @brief 主机树外部修改后重新读取主机选择器与当前规则。 */
    void refresh();

    /**
     * @brief 把主机选择器切到指定主机并重新加载其规则。
     * @param hostId 持久化主机 id；临时连接或已删除的 id 不在选择器中。
     * @return 选择器中存在该主机时返回 true；否则保持原选择不变并返回 false。
     *
     * 供主窗口在激活本页或切换终端标签时跟随当前终端，也供“新增端口转发”入口先定位主机。
     * 找不到时不清空选择，避免本地 Shell 标签把用户手动挑选的主机冲掉。
     */
    bool selectHost(const QString &hostId);

    /**
     * @brief 为指定主机打开“新建端口转发”对话框。
     * @param hostId 持久化主机 id，通常来自主机树右键菜单。
     *
     * 先切换选择器再复用 editRule()，保证新规则归属、保存后的数量刷新与工具栏“新建”完全一致。
     */
    void createRuleForHost(const QString &hostId);

signals:
    /** @brief 非致命启动、停止、数据库错误，供主窗口顶部提示区展示。 */
    void statusMessage(const QString &message);

private slots:
    /**
     * @brief 接收单条运行时的最新统计，刷新对应卡片并上报启动失败。
     * @param ruleId 产生统计的持久化规则 id。
     * @param statistics 包含状态、连接数、流量和最近错误的完整快照。
     *
     * 独立槽函数让异步运行时只有一个状态落库入口，卡片展示与顶部提示不会因新增连接方式
     * 而走不同分支；直接保存 lastError 也保证列表重建后仍能显示具体失败原因。
     */
    void applyRuntimeStatistics(const QString &ruleId,
                                const PortForwardStatistics &statistics);

private:
    /** @brief 重新加载主机下拉框并尽量保留当前选择。 */
    void reloadHosts();

    /** @brief 读取当前主机规则并重建单列卡片列表；运行时统计按 ruleId 合并显示。 */
    void reloadRules();

    /** @brief 弹出新建或编辑规则对话框；existing 为空表示新建。 */
    void editRule(const std::optional<PortForwardRule> &existing = std::nullopt);

    /** @brief 删除当前规则；运行中的规则会先停止。 */
    void removeSelectedRule();

    /** @brief 启动当前选中规则；供双击和单条规则右键菜单使用。 */
    void startSelectedRule();

    /** @brief 停止当前选中规则并保留统计快照；供单条规则右键菜单使用。 */
    void stopSelectedRule();

    /**
     * @brief 经用户确认后启动当前下拉主机的全部转发规则。
     *
     * 已处于启动中或运行中的规则保持不变；没有可用根 SSH 会话时不弹出无意义确认框，
     * 而是先提示用户连接对应主机。
     */
    void startAllRules();

    /**
     * @brief 经用户确认后停止当前下拉主机的全部活动转发规则。
     *
     * 停止动作会关闭规则的监听器和活动连接，但不会删除规则及其累计统计快照。
     */
    void stopAllRules();

    /** @brief 返回当前卡片列表选择的规则 id；没有选择时返回空。 */
    [[nodiscard]] QString selectedRuleId() const;

    /** @brief 返回 hostId 的第一条可运行根会话，并顺便清除已经析构的弱指针。 */
    [[nodiscard]] SshSessionStack *availableSession(const QString &hostId);

    /** @brief 使用指定会话创建/替换规则运行时，并把统计更新接回卡片列表。 */
    bool startRule(const PortForwardRule &rule,
                   SshSessionStack *sessionStack,
                   QString *errorMessage = nullptr);

    /** @brief 对一台刚连接的主机启动全部 autoStart 规则。 */
    void startAutomaticRules(const QString &hostId, SshSessionStack *sessionStack);

    /**
     * @brief 把一条远程转发入站 fd 唯一交给对应规则运行时。
     * @param sessionStack 产生连接的根会话，用于阻止同 hostId 多标签之间误投递。
     * @param ruleId 服务端监听所属的持久化规则 id。
     * @param tunnelFd 待接管 fd；若没有匹配运行时，本方法负责立即关闭。
     */
    void dispatchRemoteConnection(SshSessionStack *sessionStack,
                                  const QString &ruleId,
                                  NativeSocketHandle tunnelFd);

    /** @brief 把运行状态枚举翻译成稳定中文。 */
    [[nodiscard]] static QString stateText(PortForwardState state);

    /**
     * @brief 生成规则卡片使用的本机与远程端点说明。
     * @param rule 待展示的完整转发规则。
     * @return 两行地址映射；远程转发会反转箭头以准确表达连接方向。
     */
    [[nodiscard]] static QString endpointMappingText(const PortForwardRule &rule);

    /** @brief 把流量字节格式化成紧凑 IEC 单位。 */
    [[nodiscard]] static QString formatBytes(quint64 bytes);

    ApplicationDatabase *m_database = nullptr;
    QComboBox *m_hostCombo = nullptr;
    QListWidget *m_ruleList = nullptr;
    QLabel *m_summaryLabel = nullptr;

    /** @brief 每台主机当前打开的根会话弱引用列表。 */
    QHash<QString, QList<QPointer<SshSessionStack>>> m_sessions;

    /** @brief 按规则 id 保存运行时；停止后仍保留对象，以继续展示最后统计。 */
    QHash<QString, QPointer<PortForwardRuntime>> m_runtimes;

    /** @brief 最近一次收到的统计，即使运行时随后销毁也可展示失败原因。 */
    QHash<QString, PortForwardStatistics> m_statistics;
};
