#pragma once

#include "core/HostRecord.h"

#include <QObject>
#include <QStringList>

class ApplicationDatabase;

/**
 * @brief Snippet、命令历史与历史补全的可复用业务模型。
 *
 * 该模型刻意不依赖 MainWindow、终端控件或具体侧栏布局：界面可以订阅变更信号，终端也
 * 可以单独调用 completionCandidates。所有持久化仍由 ApplicationDatabase 完成，本类只
 * 负责业务校验、去重和错误传播，避免多处界面复制同一套规则。
 *
 * database 为非拥有指针，应用数据库的生命周期必须覆盖模型。模型只应在创建数据库连接
 * 的线程使用；Qt SQL 连接不能跨线程共享。
 */
class ProductivityModel final : public QObject {
    Q_OBJECT

public:
    /**
     * @brief 创建效率工具模型。
     * @param database 已初始化数据库；允许暂时为空，之后可通过 setDatabase 注入。
     * @param parent Qt 父对象。
     */
    explicit ProductivityModel(ApplicationDatabase *database = nullptr,
                               QObject *parent = nullptr);

    /**
     * @brief 切换模型使用的数据库。
     * @param database 已初始化数据库，生命周期必须覆盖本模型；可传 nullptr 进入未就绪状态。
     */
    void setDatabase(ApplicationDatabase *database);

    /** @brief 返回当前数据库指针，主要供组合控件确认模型是否可用。 */
    [[nodiscard]] ApplicationDatabase *database() const { return m_database; }

    /**
     * @brief 保存 Snippet 或分组并广播 snippetsChanged。
     * @return 保存成功返回 true；失败会同时发出 errorOccurred。
     */
    bool saveSnippet(const SnippetRecord &record);

    /**
     * @brief 删除 Snippet 或整棵分组子树。
     * @param id 待删除节点 id。
     * @param removedCount 可选出参，接收目标和后代的总删除数。
     * @return 删除事务成功返回 true；失败会发出 errorOccurred。
     */
    bool removeSnippet(const QString &id, int *removedCount = nullptr);

    /**
     * @brief 读取某分组的直接子节点。
     * @param parentId 空表示根层级。
     * @param ok 可选出参，区分空分组和查询错误。
     */
    [[nodiscard]] QList<SnippetRecord> snippetsInGroup(const QString &parentId,
                                                       bool *ok = nullptr) const;

    /**
     * @brief 按 id 读取单个节点。
     * @return 节点不存在或查询失败时返回 std::nullopt；查询失败会发出 errorOccurred。
     */
    [[nodiscard]] std::optional<SnippetRecord> snippet(const QString &id) const;

    /**
     * @brief 记录一条用户实际提交的命令。
     * @param hostId 持久化主机 id；空表示本地 Shell。
     * @param command 原始命令文本。
     * @return 已落库返回 true；空命令或以空白开头的私密命令会安全忽略并返回 true。
     *
     * 以空白开头时不持久化，是为了兼容常见 Shell 的 HISTCONTROL=ignorespace 习惯：用户
     * 经常用它临时执行包含令牌的命令，效率工具不能反而把秘密写入自己的数据库。
     */
    bool recordCommand(const QString &hostId, const QString &command);

    /**
     * @brief 读取某主机最近命令。
     * @param hostId 空表示本地 Shell。
     * @param limit 最大返回数量。
     * @param ok 可选出参，区分空历史和查询错误。
     */
    [[nodiscard]] QList<CommandHistoryEntry> recentHistory(const QString &hostId,
                                                           int limit,
                                                           bool *ok = nullptr) const;

    /**
     * @brief 从某主机历史生成按最近使用排序的前缀补全候选。
     * @param hostId 空表示本地 Shell。
     * @param prefix 当前已输入的命令前缀；匹配不区分大小写。
     * @param limit 最大候选数，重复命令只保留最近一条。
     * @return 查询失败时返回空列表并发出 errorOccurred。
     */
    [[nodiscard]] QStringList completionCandidates(const QString &hostId,
                                                   const QString &prefix,
                                                   int limit = 100) const;

signals:
    /** @brief Snippet 树保存或删除成功，界面应重新读取当前树。 */
    void snippetsChanged();

    /** @brief 某主机新增命令历史，终端可刷新该主机的自动补全模型。 */
    void commandHistoryChanged(const QString &hostId);

    /** @brief 数据库或业务校验失败；message 可直接展示给用户。 */
    void errorOccurred(const QString &message) const;

private:
    /** @brief 保证数据库已注入；未注入时统一发出错误，避免每个接口静默返回空数据。 */
    bool ensureDatabase() const;

    ApplicationDatabase *m_database = nullptr; ///< 非拥有指针；见类说明中的生命周期约束。
};

