#include "core/HostRecord.h"
#include "storage/ApplicationDatabase.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

/**
 * @brief 验证数据库持久化层的读写、事务与迁移行为。
 *
 * 全部用例都跑在内存数据库或 QTemporaryDir 里的临时文件上，绝不触碰用户真实数据文件。
 */
class ApplicationDatabaseTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    /** @brief 主机记录的每个字段都必须原样往返，未设置的端口不能被写成默认值。 */
    void hostRoundTripPreservesEveryField();

    /** @brief 更新分组不得连带删除其下的主机。 */
    void savingGroupAgainKeepsChildren();

    /** @brief 根层级与分组内的列表必须相互隔离，并按 sortOrder 排序。 */
    void listsRootAndGroupSeparately();

    /** @brief 搜索应同时命中名称与地址，且用户输入的通配符必须按字面处理。 */
    void searchMatchesNameAndAddressWithoutWildcardInjection();

    /** @brief 标签筛选必须精确匹配，不能让 prod 命中 production。 */
    void tagFilterMatchesWholeTagOnly();

    /** @brief 未设置字段沿父分组链继承，已设置字段永远优先。 */
    void resolveHostInheritsFromAncestors();

    /** @brief 受管主机链必须按顺序往返，并递归展开为可直接建立的连接计划。 */
    void managedHostChainRoundTripsAndResolvesInOrder();

    /** @brief 主机链必须拒绝自身引用、重复节点、分组节点、缺失引用和跨主机环路。 */
    void managedHostChainRejectsInvalidGraphs();

    /** @brief 正被连接链引用的跳板机不得被删除，避免目标静默退化为危险的本机直连。 */
    void referencedJumpHostCannotBeDeleted();

    /** @brief 删除分组级联删除子节点；删除不存在的 id 不算失败。 */
    void removeHostCascadesAndReportsMissing();

    /** @brief 查询失败与“没有这条数据”必须能被区分。 */
    void distinguishesMissingRowFromError();

    /** @brief 凭据只接受密文与元数据，缺少密文或算法标识必须被拒绝。 */
    void credentialRejectsPlaintextAndRoundTripsMetadata();

    /** @brief 保险库公开配置必须完整往返，并把“未初始化”与“配置损坏”严格区分。 */
    void vaultConfigurationDistinguishesUninitializedFromCorrupt();

    /** @brief 连接历史按时间倒序返回，补录断开时间会算出时长。 */
    void connectionHistoryOrdersByRecencyAndComputesDuration();

    /** @brief 命令历史按主机隔离，本地命令不与远程命令混在一起。 */
    void commandHistoryIsScopedPerHost();

    /** @brief 设置项的类型化读取要区分“未设置”和“值已损坏”。 */
    void typedSettingsDistinguishUnsetFromCorrupt();

    /** @brief 工作区布局必须校验 JSON、稳定往返并按更新时间排序。 */
    void workspaceRoundTripValidatesJsonAndOrdersByUpdateTime();

    /** @brief 事务中任一步失败，之前的写入必须全部回滚。 */
    void failedTransactionRollsBackEarlierWrites();

    /** @brief 从 schema 版本 1 升级后，旧数据必须完好且新列可用。 */
    void migrationFromVersionOnePreservesData();

    /** @brief 版本 6 迁移必须把历史共享凭据拆成独立密文引用，且不改变密文内容。 */
    void migrationToVersionSixSplitsSharedCredentialReferences();

private:
    /** @brief 构造一台可连接主机的样例记录。 */
    static HostRecord makeHost(const QString &id, const QString &name, const QString &address);

    std::unique_ptr<ApplicationDatabase> m_database;
};

HostRecord ApplicationDatabaseTest::makeHost(const QString &id,
                                             const QString &name,
                                             const QString &address)
{
    HostRecord record;
    record.id = id;
    record.kind = HostNodeKind::Host;
    record.name = name;
    record.host = address;
    return record;
}

void ApplicationDatabaseTest::init()
{
    // 内存数据库保证每个用例都从空库开始，也不会在磁盘留下任何残留。
    m_database = std::make_unique<ApplicationDatabase>(ApplicationDatabase::inMemoryPath());
    QString error;
    QVERIFY2(m_database->initialize(&error), qPrintable(error));
}

void ApplicationDatabaseTest::cleanup()
{
    m_database.reset();
}

void ApplicationDatabaseTest::hostRoundTripPreservesEveryField()
{
    HostRecord group;
    group.id = QStringLiteral("group-1");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("生产环境");
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostRecord host = makeHost(QStringLiteral("host-1"), QStringLiteral("数据库"),
                               QStringLiteral("db.internal"));
    host.parentId = group.id;
    host.port = 2222;
    host.username = QStringLiteral("deploy");
    host.credentialId = QStringLiteral("cred-1");
    host.identityFile = QStringLiteral("/home/deploy/.ssh/id_ed25519");
    host.tags = QStringList{QStringLiteral("db"), QStringLiteral("prod")};
    host.proxyJumps = QStringList{QStringLiteral("bastion@edge:2200")};
    host.hostChain = QStringList{QStringLiteral("jump-managed")};
    host.allowLegacyHmacSha1 = true;
    host.sortOrder = 7;
    host.createdAt = QDateTime::fromString(QStringLiteral("2026-01-02T03:04:05.678Z"),
                                           Qt::ISODateWithMs);
    host.updatedAt = QDateTime::fromString(QStringLiteral("2026-02-03T04:05:06.789Z"),
                                           Qt::ISODateWithMs);
    // 受管链使用真实外键，必须先保存被引用的跳板机。
    HostRecord managedJump = makeHost(QStringLiteral("jump-managed"),
                                      QStringLiteral("受管跳板机"),
                                      QStringLiteral("edge.example"));
    QVERIFY2(m_database->saveHost(managedJump, &error), qPrintable(error));
    QVERIFY2(m_database->saveHost(host, &error), qPrintable(error));

    const std::optional<HostRecord> loaded = m_database->findHost(host.id, &error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(loaded->parentId, host.parentId);
    QCOMPARE(loaded->kind, HostNodeKind::Host);
    QCOMPARE(loaded->name, host.name);
    QCOMPARE(loaded->host, host.host);
    QCOMPARE(loaded->port, host.port);
    QCOMPARE(loaded->username, host.username);
    QCOMPARE(loaded->credentialId, host.credentialId);
    QCOMPARE(loaded->identityFile, host.identityFile);
    QCOMPARE(loaded->tags, host.tags);
    QCOMPARE(loaded->proxyJumps, host.proxyJumps);
    QCOMPARE(loaded->hostChain, host.hostChain);
    QCOMPARE(loaded->allowLegacyHmacSha1, true);
    QCOMPARE(loaded->sortOrder, 7);
    QCOMPARE(loaded->createdAt, host.createdAt);
    QCOMPARE(loaded->updatedAt, host.updatedAt);

    // 未设置端口必须保持“未设置”，否则子主机再也无法跟随分组端口变化。
    HostRecord inheriting = makeHost(QStringLiteral("host-2"), QStringLiteral("缓存"),
                                     QStringLiteral("cache.internal"));
    inheriting.parentId = group.id;
    QVERIFY2(m_database->saveHost(inheriting, &error), qPrintable(error));
    const std::optional<HostRecord> reloaded = m_database->findHost(inheriting.id, &error);
    QVERIFY2(reloaded.has_value(), qPrintable(error));
    QVERIFY(!reloaded->port.has_value());

    // 更新同一 id 应该是覆盖而不是插入第二行。
    HostRecord renamed = *loaded;
    renamed.name = QStringLiteral("主数据库");
    QVERIFY2(m_database->saveHost(renamed, &error), qPrintable(error));
    bool ok = false;
    const QList<HostRecord> children = m_database->listHostsInGroup(group.id, &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(children.size(), 2);
}

void ApplicationDatabaseTest::savingGroupAgainKeepsChildren()
{
    QString error;
    HostRecord group;
    group.id = QStringLiteral("group-1");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("测试环境");
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostRecord child = makeHost(QStringLiteral("host-1"), QStringLiteral("网关"),
                                QStringLiteral("gw.internal"));
    child.parentId = group.id;
    QVERIFY2(m_database->saveHost(child, &error), qPrintable(error));

    group.name = QStringLiteral("预发布环境");
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    const std::optional<HostRecord> survivor = m_database->findHost(child.id, &error);
    QVERIFY2(survivor.has_value(), "更新分组后子主机被级联删除了");
    QCOMPARE(survivor->name, QStringLiteral("网关"));
}

void ApplicationDatabaseTest::listsRootAndGroupSeparately()
{
    QString error;
    HostRecord group;
    group.id = QStringLiteral("group-1");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("分组");
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostRecord rootHost = makeHost(QStringLiteral("root-host"), QStringLiteral("独立主机"),
                                   QStringLiteral("solo.example"));
    QVERIFY2(m_database->saveHost(rootHost, &error), qPrintable(error));

    HostRecord second = makeHost(QStringLiteral("child-b"), QStringLiteral("B"),
                                 QStringLiteral("b.example"));
    second.parentId = group.id;
    second.sortOrder = 2;
    QVERIFY2(m_database->saveHost(second, &error), qPrintable(error));

    HostRecord first = makeHost(QStringLiteral("child-a"), QStringLiteral("A"),
                                QStringLiteral("a.example"));
    first.parentId = group.id;
    first.sortOrder = 1;
    QVERIFY2(m_database->saveHost(first, &error), qPrintable(error));

    bool ok = false;
    const QList<HostRecord> roots = m_database->listHostsInGroup(QString(), &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(roots.size(), 2); // 分组本身与独立主机都在根层级。

    const QList<HostRecord> children = m_database->listHostsInGroup(group.id, &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(children.size(), 2);
    QCOMPARE(children.at(0).id, QStringLiteral("child-a"));
    QCOMPARE(children.at(1).id, QStringLiteral("child-b"));

    const QList<HostRecord> empty = m_database->listHostsInGroup(QStringLiteral("不存在"), &ok, &error);
    QVERIFY2(ok, "空分组不应被当成查询失败");
    QVERIFY(empty.isEmpty());
}

void ApplicationDatabaseTest::searchMatchesNameAndAddressWithoutWildcardInjection()
{
    QString error;
    QVERIFY2(m_database->saveHost(makeHost(QStringLiteral("h1"), QStringLiteral("生产数据库"),
                                           QStringLiteral("db.prod.internal")), &error),
             qPrintable(error));
    QVERIFY2(m_database->saveHost(makeHost(QStringLiteral("h2"), QStringLiteral("日志收集"),
                                           QStringLiteral("logs.internal")), &error),
             qPrintable(error));
    QVERIFY2(m_database->saveHost(makeHost(QStringLiteral("h3"), QStringLiteral("db_1"),
                                           QStringLiteral("literal.example")), &error),
             qPrintable(error));

    bool ok = false;
    const QList<HostRecord> byName = m_database->searchHosts(QStringLiteral("日志"), &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(byName.size(), 1);
    QCOMPARE(byName.at(0).id, QStringLiteral("h2"));

    const QList<HostRecord> byAddress = m_database->searchHosts(QStringLiteral("prod.internal"),
                                                                &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(byAddress.size(), 1);
    QCOMPARE(byAddress.at(0).id, QStringLiteral("h1"));

    // 下划线若未转义会被当成任意单字符，"db_1" 也会命中别的主机。
    const QList<HostRecord> literal = m_database->searchHosts(QStringLiteral("db_1"), &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(literal.size(), 1);
    QCOMPARE(literal.at(0).id, QStringLiteral("h3"));

    // 百分号同理：不转义就会把整张表都搜出来。
    const QList<HostRecord> percent = m_database->searchHosts(QStringLiteral("%"), &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QVERIFY2(percent.isEmpty(), "用户输入的 % 被当成了通配符");
}

void ApplicationDatabaseTest::tagFilterMatchesWholeTagOnly()
{
    QString error;
    HostRecord prod = makeHost(QStringLiteral("h1"), QStringLiteral("生产"), QStringLiteral("p.example"));
    prod.tags = QStringList{QStringLiteral("db"), QStringLiteral("prod")};
    QVERIFY2(m_database->saveHost(prod, &error), qPrintable(error));

    HostRecord production = makeHost(QStringLiteral("h2"), QStringLiteral("生产候选"),
                                     QStringLiteral("q.example"));
    production.tags = QStringList{QStringLiteral("production")};
    QVERIFY2(m_database->saveHost(production, &error), qPrintable(error));

    HostRecord untagged = makeHost(QStringLiteral("h3"), QStringLiteral("无标签"),
                                   QStringLiteral("r.example"));
    QVERIFY2(m_database->saveHost(untagged, &error), qPrintable(error));

    bool ok = false;
    const QList<HostRecord> tagged = m_database->hostsWithTag(QStringLiteral("prod"), &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(tagged.size(), 1);
    QCOMPARE(tagged.at(0).id, QStringLiteral("h1"));
}

void ApplicationDatabaseTest::resolveHostInheritsFromAncestors()
{
    QString error;
    HostRecord root;
    root.id = QStringLiteral("root");
    root.kind = HostNodeKind::Folder;
    root.name = QStringLiteral("公司");
    root.port = 2200;
    root.username = QStringLiteral("ops");
    root.proxyJumps = QStringList{QStringLiteral("bastion@edge")};
    root.tags = QStringList{QStringLiteral("corp")};
    QVERIFY2(m_database->saveHost(root, &error), qPrintable(error));

    HostRecord team;
    team.id = QStringLiteral("team");
    team.parentId = root.id;
    team.kind = HostNodeKind::Folder;
    team.name = QStringLiteral("平台组");
    team.identityFile = QStringLiteral("/keys/platform");
    team.tags = QStringList{QStringLiteral("platform")};
    QVERIFY2(m_database->saveHost(team, &error), qPrintable(error));

    HostRecord leaf = makeHost(QStringLiteral("leaf"), QStringLiteral("构建机"),
                               QStringLiteral("build.internal"));
    leaf.parentId = team.id;
    leaf.username = QStringLiteral("builder"); // 显式设置必须压过祖先的 ops。
    QVERIFY2(m_database->saveHost(leaf, &error), qPrintable(error));

    const std::optional<HostRecord> resolved = m_database->resolveHost(leaf.id, &error);
    QVERIFY2(resolved.has_value(), qPrintable(error));
    QCOMPARE(resolved->username, QStringLiteral("builder"));
    QCOMPARE(resolved->port, std::optional<quint16>(2200));
    QCOMPARE(resolved->identityFile, QStringLiteral("/keys/platform"));
    QCOMPARE(resolved->proxyJumps, QStringList{QStringLiteral("bastion@edge")});
    QVERIFY(resolved->tags.contains(QStringLiteral("platform")));
    QVERIFY(resolved->tags.contains(QStringLiteral("corp")));

    // 未解析的原始记录必须保持“未设置”，继承只发生在 resolveHost 的返回值上。
    const std::optional<HostRecord> raw = m_database->findHost(leaf.id, &error);
    QVERIFY2(raw.has_value(), qPrintable(error));
    QVERIFY(!raw->port.has_value());

    const ConnectionTarget target = resolved->toConnectionTarget();
    QCOMPARE(target.port, quint16{2200});
    QCOMPARE(target.username, QStringLiteral("builder"));
    QVERIFY(target.password.isEmpty());
}

void ApplicationDatabaseTest::managedHostChainRoundTripsAndResolvesInOrder()
{
    QString error;
    HostRecord edge = makeHost(QStringLiteral("edge"), QStringLiteral("公网入口"),
                               QStringLiteral("edge.example"));
    edge.username = QStringLiteral("edge-user");
    QVERIFY2(m_database->saveHost(edge, &error), qPrintable(error));

    HostRecord bastion = makeHost(QStringLiteral("bastion"), QStringLiteral("业务跳板机"),
                                  QStringLiteral("10.0.0.10"));
    bastion.username = QStringLiteral("ops");
    // 跳板机自己也需要经过公网入口；目标引用它时解析器应展开为同一条可见的实际路径。
    bastion.hostChain = {edge.id};
    QVERIFY2(m_database->saveHost(bastion, &error), qPrintable(error));

    HostRecord target = makeHost(QStringLiteral("target"), QStringLiteral("订单数据库"),
                                 QStringLiteral("10.20.3.15"));
    target.username = QStringLiteral("dba");
    target.hostChain = {bastion.id};
    QVERIFY2(m_database->saveHost(target, &error), qPrintable(error));

    const std::optional<HostRecord> reloaded = m_database->findHost(target.id, &error);
    QVERIFY2(reloaded.has_value(), qPrintable(error));
    QCOMPARE(reloaded->hostChain, QStringList{bastion.id});

    const QList<HostRecord> plan = m_database->resolveConnectionChain(target.id, &error);
    QVERIFY2(!plan.isEmpty(), qPrintable(error));
    QCOMPARE(plan.size(), 3);
    QCOMPARE(plan.at(0).id, edge.id);
    QCOMPARE(plan.at(1).id, bastion.id);
    QCOMPARE(plan.at(2).id, target.id);
    // 返回计划已经被扁平化，调用方不应再次递归展开结构字段。
    QVERIFY(plan.at(0).hostChain.isEmpty());
    QVERIFY(plan.at(1).hostChain.isEmpty());
    QVERIFY(plan.at(2).hostChain.isEmpty());
}

void ApplicationDatabaseTest::managedHostChainRejectsInvalidGraphs()
{
    QString error;
    HostRecord jump = makeHost(QStringLiteral("jump"), QStringLiteral("跳板机"),
                               QStringLiteral("jump.example"));
    QVERIFY2(m_database->saveHost(jump, &error), qPrintable(error));

    HostRecord target = makeHost(QStringLiteral("target"), QStringLiteral("目标"),
                                 QStringLiteral("target.internal"));
    target.hostChain = {target.id};
    QVERIFY(!m_database->saveHost(target, &error));
    QVERIFY(error.contains(QStringLiteral("自身")));

    target.hostChain = {jump.id, jump.id};
    QVERIFY(!m_database->saveHost(target, &error));
    QVERIFY(error.contains(QStringLiteral("重复")));

    HostRecord folder;
    folder.id = QStringLiteral("folder");
    folder.kind = HostNodeKind::Folder;
    folder.name = QStringLiteral("分组");
    QVERIFY2(m_database->saveHost(folder, &error), qPrintable(error));
    target.hostChain = {folder.id};
    QVERIFY(!m_database->saveHost(target, &error));
    QVERIFY(error.contains(QStringLiteral("分组")));

    target.hostChain = {QStringLiteral("missing")};
    QVERIFY(!m_database->saveHost(target, &error));
    QVERIFY(error.contains(QStringLiteral("不存在")));

    // 先保存 A→B，再尝试把 B 改成 B→A，第二次保存必须检测递归环路并完整回滚。
    HostRecord first = makeHost(QStringLiteral("first"), QStringLiteral("A"),
                                QStringLiteral("a.example"));
    HostRecord second = makeHost(QStringLiteral("second"), QStringLiteral("B"),
                                 QStringLiteral("b.example"));
    QVERIFY2(m_database->saveHost(second, &error), qPrintable(error));
    first.hostChain = {second.id};
    QVERIFY2(m_database->saveHost(first, &error), qPrintable(error));
    second.hostChain = {first.id};
    QVERIFY(!m_database->saveHost(second, &error));
    QVERIFY(error.contains(QStringLiteral("环路")));
    QCOMPARE(m_database->findHost(second.id, &error)->hostChain, QStringList{});
}

void ApplicationDatabaseTest::referencedJumpHostCannotBeDeleted()
{
    QString error;
    HostRecord jump = makeHost(QStringLiteral("jump"), QStringLiteral("生产跳板机"),
                               QStringLiteral("jump.example"));
    QVERIFY2(m_database->saveHost(jump, &error), qPrintable(error));

    HostRecord target = makeHost(QStringLiteral("target"), QStringLiteral("内网主机"),
                                 QStringLiteral("10.0.0.8"));
    target.hostChain = {jump.id};
    QVERIFY2(m_database->saveHost(target, &error), qPrintable(error));

    int removed = -1;
    QVERIFY(!m_database->removeHost(jump.id, &removed, &error));
    QCOMPARE(removed, 0);
    QVERIFY2(m_database->findHost(jump.id, &error).has_value(),
             "被引用的跳板机删除失败后必须仍然存在");
}

void ApplicationDatabaseTest::removeHostCascadesAndReportsMissing()
{
    QString error;
    HostRecord group;
    group.id = QStringLiteral("group-1");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("待删除");
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostRecord child = makeHost(QStringLiteral("host-1"), QStringLiteral("子主机"),
                                QStringLiteral("c.example"));
    child.parentId = group.id;
    QVERIFY2(m_database->saveHost(child, &error), qPrintable(error));

    int removed = -1;
    QVERIFY2(m_database->removeHost(group.id, &removed, &error), qPrintable(error));
    QCOMPARE(removed, 1);
    QVERIFY(!m_database->findHost(child.id, &error).has_value());
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // 删除不存在的记录不是错误，但必须报告没有删掉任何行。
    QVERIFY2(m_database->removeHost(QStringLiteral("不存在"), &removed, &error), qPrintable(error));
    QCOMPARE(removed, 0);
}

void ApplicationDatabaseTest::distinguishesMissingRowFromError()
{
    QString error = QStringLiteral("哨兵");
    const std::optional<HostRecord> missing = m_database->findHost(QStringLiteral("无此主机"), &error);
    QVERIFY(!missing.has_value());
    QCOMPARE(error, QStringLiteral("哨兵")); // 没有数据不应该写入错误信息。

    error.clear();
    QVERIFY(!m_database->setting(QStringLiteral("从未设置的键"), &error).has_value());
    QVERIFY(error.isEmpty());

    // 而真正的查询失败必须给出可读原因：删表后再查即模拟磁盘/结构损坏。
    QSqlQuery drop(m_database->connection());
    QVERIFY2(drop.exec(QStringLiteral("DROP TABLE hosts")), qPrintable(drop.lastError().text()));
    QVERIFY(!m_database->findHost(QStringLiteral("任意"), &error).has_value());
    QVERIFY2(!error.isEmpty(), "查询失败时没有给出错误信息");
}

void ApplicationDatabaseTest::credentialRejectsPlaintextAndRoundTripsMetadata()
{
    QString error;
    CredentialRecord plaintext;
    plaintext.id = QStringLiteral("cred-bad");
    plaintext.name = QStringLiteral("危险凭据");
    // 缺少 cipher 与密文，说明调用方绕开了 Vault，必须拒绝而不是写进磁盘。
    QVERIFY(!m_database->saveCredential(plaintext, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!m_database->findCredential(plaintext.id, &error).has_value());

    // 有 payload 但没有算法标识，是“直接把口令原文塞进 payload”最典型的样子。
    // 数据库自身的 NOT NULL 约束拦不住它，只能靠这层显式校验。
    CredentialRecord missingCipher = plaintext;
    missingCipher.id = QStringLiteral("cred-bad-2");
    missingCipher.encryptedPayload = QByteArrayLiteral("hunter2");
    error.clear();
    QVERIFY2(!m_database->saveCredential(missingCipher, &error),
             "缺少加密算法标识的凭据被写入了数据库");
    QVERIFY(!error.isEmpty());
    QVERIFY(!m_database->findCredential(missingCipher.id, &error).has_value());

    CredentialRecord record;
    record.id = QStringLiteral("cred-1");
    record.name = QStringLiteral("生产 root");
    record.type = CredentialType::PrivateKey;
    // 密文含 0x00 字节，用来验证走的是 BLOB 而不是会被截断的字符串通道。
    record.encryptedPayload = QByteArray(16, '\x5a') + QByteArrayLiteral("ciphertext");
    record.encryptedPayload[1] = '\0';
    record.cipher = QStringLiteral("xchacha20-poly1305-v1");
    record.nonce = QByteArray(24, '\x10');
    record.nonce[1] = '\0';
    record.kdfAlgorithm = QStringLiteral("argon2id13");
    record.kdfSalt = QByteArray(16, '\x20');
    record.kdfSalt[1] = '\0';
    record.kdfIterations = 4;
    record.kdfMemoryKib = 262144;
    record.kdfParallelism = 1;
    QVERIFY2(m_database->saveCredential(record, &error), qPrintable(error));

    const std::optional<CredentialRecord> loaded = m_database->findCredential(record.id, &error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(loaded->name, record.name);
    QCOMPARE(loaded->type, CredentialType::PrivateKey);
    QCOMPARE(loaded->encryptedPayload, record.encryptedPayload);
    QCOMPARE(loaded->cipher, record.cipher);
    QCOMPARE(loaded->nonce, record.nonce);
    QCOMPARE(loaded->kdfAlgorithm, record.kdfAlgorithm);
    QCOMPARE(loaded->kdfSalt, record.kdfSalt);
    QCOMPARE(loaded->kdfIterations, 4LL);
    QCOMPARE(loaded->kdfMemoryKib, 262144LL);
    QCOMPARE(loaded->kdfParallelism, 1LL);

    bool ok = false;
    QCOMPARE(m_database->listCredentials(&ok, &error).size(), 1);
    QVERIFY2(ok, qPrintable(error));

    int removed = -1;
    QVERIFY2(m_database->removeCredential(record.id, &removed, &error), qPrintable(error));
    QCOMPARE(removed, 1);
    QVERIFY(m_database->listCredentials(&ok, &error).isEmpty());
}

void ApplicationDatabaseTest::vaultConfigurationDistinguishesUninitializedFromCorrupt()
{
    QString error = QStringLiteral("哨兵");
    const std::optional<VaultConfiguration> missing = m_database->vaultConfiguration(&error);
    QVERIFY(!missing.has_value());
    // 没有配置是正常的首次启动状态，不能覆盖调用方已有错误缓冲。
    QCOMPARE(error, QStringLiteral("哨兵"));

    VaultConfiguration incomplete;
    incomplete.kdfAlgorithm = QStringLiteral("argon2id13");
    error.clear();
    QVERIFY(!m_database->saveVaultConfiguration(incomplete, &error));
    QVERIFY2(!error.isEmpty(), "缺少盐值和校验块的配置被写入数据库");

    VaultConfiguration configuration;
    configuration.formatVersion = 1;
    configuration.kdfAlgorithm = QStringLiteral("argon2id13");
    configuration.salt = QByteArray(16, '\x31');
    configuration.verifier = QByteArray(40, '\x42');
    const QDateTime created = QDateTime::fromString(QStringLiteral("2026-08-08T01:02:03.004Z"),
                                                    Qt::ISODateWithMs);
    configuration.createdAt = created;
    configuration.updatedAt = created;
    error.clear();
    QVERIFY2(m_database->saveVaultConfiguration(configuration, &error), qPrintable(error));

    const std::optional<VaultConfiguration> loaded = m_database->vaultConfiguration(&error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(loaded->formatVersion, 1);
    QCOMPARE(loaded->kdfAlgorithm, configuration.kdfAlgorithm);
    QCOMPARE(loaded->salt, configuration.salt);
    QCOMPARE(loaded->verifier, configuration.verifier);
    QCOMPARE(loaded->createdAt, created);

    // 模拟磁盘截断产生的空校验块：必须报告损坏，不能退回“未初始化”创建流程。
    QSqlQuery corrupt(m_database->connection());
    QVERIFY2(corrupt.exec(QStringLiteral("UPDATE vault_metadata SET verifier = X'' WHERE id = 1")),
             qPrintable(corrupt.lastError().text()));
    error.clear();
    QVERIFY(!m_database->vaultConfiguration(&error).has_value());
    QVERIFY2(!error.isEmpty(), "损坏的保险库配置没有返回错误原因");
}

void ApplicationDatabaseTest::connectionHistoryOrdersByRecencyAndComputesDuration()
{
    QString error;
    const QDateTime base = QDateTime::fromString(QStringLiteral("2026-03-01T10:00:00.000Z"),
                                                 Qt::ISODateWithMs);

    ConnectionHistoryEntry older;
    older.hostId = QStringLiteral("host-1");
    older.connectedAt = base;
    const std::optional<qint64> olderId = m_database->appendConnectionHistory(older, &error);
    QVERIFY2(olderId.has_value(), qPrintable(error));

    ConnectionHistoryEntry newer;
    newer.hostId = QStringLiteral("host-1");
    newer.connectedAt = base.addSecs(60);
    const std::optional<qint64> newerId = m_database->appendConnectionHistory(newer, &error);
    QVERIFY2(newerId.has_value(), qPrintable(error));

    ConnectionHistoryEntry failed;
    failed.hostId = QStringLiteral("host-2");
    failed.connectedAt = base.addSecs(120);
    failed.succeeded = false;
    failed.error = QStringLiteral("认证失败");
    QVERIFY2(m_database->appendConnectionHistory(failed, &error).has_value(), qPrintable(error));

    bool ok = false;
    const QList<ConnectionHistoryEntry> recent = m_database->recentConnections(2, QString(), &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(recent.size(), 2);
    QCOMPARE(recent.at(0).hostId, QStringLiteral("host-2"));
    QCOMPARE(recent.at(0).succeeded, false);
    QCOMPARE(recent.at(0).error, QStringLiteral("认证失败"));
    QCOMPARE(recent.at(1).id, *newerId);

    const QList<ConnectionHistoryEntry> perHost =
        m_database->recentConnections(10, QStringLiteral("host-1"), &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(perHost.size(), 2);

    bool updated = false;
    QVERIFY2(m_database->finishConnectionHistory(*olderId, base.addSecs(90), &updated, &error),
             qPrintable(error));
    QVERIFY(updated);

    const QList<ConnectionHistoryEntry> afterFinish =
        m_database->recentConnections(10, QStringLiteral("host-1"), &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    const ConnectionHistoryEntry finished = afterFinish.at(1);
    QCOMPARE(finished.id, *olderId);
    QVERIFY(finished.disconnectedAt.has_value());
    QCOMPARE(*finished.disconnectedAt, base.addSecs(90));
    QVERIFY(finished.durationMs.has_value());
    // julianday 是浮点运算，允许几毫秒误差，但绝不能偏离 90 秒这个量级。
    QVERIFY2(qAbs(*finished.durationMs - 90000) <= 5,
             qPrintable(QStringLiteral("时长计算错误：%1").arg(*finished.durationMs)));

    // 尚未结束的会话必须保持“无时长”，不能被填成 0。
    QVERIFY(!afterFinish.at(0).durationMs.has_value());

    bool missingUpdated = true;
    QVERIFY2(m_database->finishConnectionHistory(999999, base, &missingUpdated, &error),
             qPrintable(error));
    QVERIFY(!missingUpdated);

    QVERIFY(m_database->recentConnections(0, QString(), &ok, &error).isEmpty());
    QVERIFY(ok);
}

void ApplicationDatabaseTest::commandHistoryIsScopedPerHost()
{
    QString error;
    const QDateTime base = QDateTime::fromString(QStringLiteral("2026-03-01T10:00:00.000Z"),
                                                 Qt::ISODateWithMs);

    CommandHistoryEntry remoteFirst;
    remoteFirst.hostId = QStringLiteral("host-1");
    remoteFirst.command = QStringLiteral("uptime");
    remoteFirst.executedAt = base;
    remoteFirst.exitCode = 0;
    QVERIFY2(m_database->appendCommandHistory(remoteFirst, &error).has_value(), qPrintable(error));

    CommandHistoryEntry remoteSecond;
    remoteSecond.hostId = QStringLiteral("host-1");
    remoteSecond.command = QStringLiteral("systemctl restart nginx");
    remoteSecond.executedAt = base.addSecs(10);
    QVERIFY2(m_database->appendCommandHistory(remoteSecond, &error).has_value(), qPrintable(error));

    CommandHistoryEntry otherHost;
    otherHost.hostId = QStringLiteral("host-2");
    otherHost.command = QStringLiteral("df -h");
    otherHost.executedAt = base.addSecs(20);
    QVERIFY2(m_database->appendCommandHistory(otherHost, &error).has_value(), qPrintable(error));

    CommandHistoryEntry local;
    local.command = QStringLiteral("ls");
    local.executedAt = base.addSecs(30);
    QVERIFY2(m_database->appendCommandHistory(local, &error).has_value(), qPrintable(error));

    bool ok = false;
    const QList<CommandHistoryEntry> forHost =
        m_database->recentCommands(QStringLiteral("host-1"), 10, &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(forHost.size(), 2);
    QCOMPARE(forHost.at(0).command, QStringLiteral("systemctl restart nginx"));
    QVERIFY(!forHost.at(0).exitCode.has_value());
    QCOMPARE(forHost.at(1).command, QStringLiteral("uptime"));
    QCOMPARE(*forHost.at(1).exitCode, 0);
    QCOMPARE(forHost.at(1).executedAt, base);

    const QList<CommandHistoryEntry> localCommands = m_database->recentCommands(QString(), 10, &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(localCommands.size(), 1);
    QCOMPARE(localCommands.at(0).command, QStringLiteral("ls"));

    QVERIFY(!m_database->appendCommandHistory(CommandHistoryEntry{}, &error).has_value());
    QVERIFY(!error.isEmpty());
}

void ApplicationDatabaseTest::typedSettingsDistinguishUnsetFromCorrupt()
{
    QString error;
    QVERIFY2(m_database->setSetting(QStringLiteral("ui/theme"), QStringLiteral("dark"), &error),
             qPrintable(error));
    QCOMPARE(*m_database->setting(QStringLiteral("ui/theme"), &error), QStringLiteral("dark"));

    QVERIFY2(m_database->setSetting(QStringLiteral("ui/theme"), QStringLiteral("light"), &error),
             qPrintable(error));
    QCOMPARE(*m_database->setting(QStringLiteral("ui/theme"), &error), QStringLiteral("light"));

    QVERIFY2(m_database->setBoolSetting(QStringLiteral("terminal/bell"), true, &error), qPrintable(error));
    QCOMPARE(*m_database->boolSetting(QStringLiteral("terminal/bell"), &error), true);
    QVERIFY2(m_database->setBoolSetting(QStringLiteral("terminal/bell"), false, &error), qPrintable(error));
    QCOMPARE(*m_database->boolSetting(QStringLiteral("terminal/bell"), &error), false);

    QVERIFY2(m_database->setIntSetting(QStringLiteral("terminal/scrollback"), 50000, &error),
             qPrintable(error));
    QCOMPARE(*m_database->intSetting(QStringLiteral("terminal/scrollback"), &error), 50000);

    error.clear();
    QVERIFY(!m_database->boolSetting(QStringLiteral("从未设置"), &error).has_value());
    QVERIFY2(error.isEmpty(), "未设置的键不应报错");

    // 值被外部改坏时必须报错，而不是静默当成 false 关掉用户开启过的功能。
    QVERIFY2(m_database->setSetting(QStringLiteral("terminal/bell"), QStringLiteral("maybe"), &error),
             qPrintable(error));
    error.clear();
    QVERIFY(!m_database->boolSetting(QStringLiteral("terminal/bell"), &error).has_value());
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY(!m_database->intSetting(QStringLiteral("ui/theme"), &error).has_value());
    QVERIFY(!error.isEmpty());

    QVERIFY2(m_database->removeSetting(QStringLiteral("ui/theme"), &error), qPrintable(error));
    QVERIFY(!m_database->setting(QStringLiteral("ui/theme"), &error).has_value());
}

void ApplicationDatabaseTest::workspaceRoundTripValidatesJsonAndOrdersByUpdateTime()
{
    QString error;
    WorkspaceRecord invalid;
    invalid.id = QStringLiteral("invalid");
    invalid.name = QStringLiteral("损坏布局");
    invalid.layoutJson = QStringLiteral("{not-json");
    QVERIFY(!m_database->saveWorkspace(invalid, &error));
    QVERIFY2(!error.isEmpty(), "非法工作区 JSON 被持久化了");

    WorkspaceRecord older;
    older.id = QStringLiteral("workspace-old");
    older.name = QStringLiteral("日常运维");
    older.layoutJson = QStringLiteral(
        R"({"version":1,"tabs":[{"title":"数据库","hostId":"host-db","filesVisible":true}]})");
    older.updatedAt = QDateTime::fromString(QStringLiteral("2026-08-01T01:02:03.000Z"),
                                            Qt::ISODateWithMs);
    error.clear();
    QVERIFY2(m_database->saveWorkspace(older, &error), qPrintable(error));

    WorkspaceRecord newer;
    newer.id = QStringLiteral("workspace-new");
    newer.name = QStringLiteral("故障排查");
    newer.layoutJson = QStringLiteral(
        R"({"version":1,"tabs":[{"title":"本地","hostId":"","filesVisible":false}]})");
    newer.updatedAt = older.updatedAt.addDays(1);
    QVERIFY2(m_database->saveWorkspace(newer, &error), qPrintable(error));

    const std::optional<WorkspaceRecord> loaded =
        m_database->findWorkspace(older.id, &error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(loaded->name, older.name);
    // 数据库会把 JSON 规范成紧凑格式，但结构化内容必须仍可解析且版本保持不变。
    const QJsonDocument layout = QJsonDocument::fromJson(loaded->layoutJson.toUtf8());
    QVERIFY(layout.isObject());
    QCOMPARE(layout.object().value(QStringLiteral("version")).toInt(), 1);

    bool ok = false;
    const QList<WorkspaceRecord> records = m_database->listWorkspaces(&ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(records.size(), 2);
    QCOMPARE(records.constFirst().id, newer.id);

    int removed = -1;
    QVERIFY2(m_database->removeWorkspace(older.id, &removed, &error), qPrintable(error));
    QCOMPARE(removed, 1);
    QVERIFY(!m_database->findWorkspace(older.id, &error).has_value());
}

void ApplicationDatabaseTest::failedTransactionRollsBackEarlierWrites()
{
    QString error;
    const bool committed = m_database->runInTransaction([&](QString *innerError) {
        if (!m_database->saveHost(makeHost(QStringLiteral("h1"), QStringLiteral("先写入的主机"),
                                           QStringLiteral("a.example")), innerError)) {
            return false;
        }
        if (!m_database->setSetting(QStringLiteral("批量导入"), QStringLiteral("进行中"), innerError)) {
            return false;
        }
        // 第二台主机缺少名称，模拟导入到一半发现数据非法。
        HostRecord broken;
        broken.id = QStringLiteral("h2");
        if (!m_database->saveHost(broken, innerError)) {
            return false;
        }
        return true;
    }, &error);

    QVERIFY(!committed);
    QVERIFY2(!error.isEmpty(), "事务失败必须给出可读原因");

    // 关键点：失败前已经成功的写入也必须消失，否则会留下半截导入结果。
    QString lookupError;
    QVERIFY2(!m_database->findHost(QStringLiteral("h1"), &lookupError).has_value(),
             "事务回滚后主机仍然存在");
    QVERIFY(lookupError.isEmpty());
    QVERIFY2(!m_database->setting(QStringLiteral("批量导入"), &lookupError).has_value(),
             "事务回滚后设置项仍然存在");

    // 同样的组合在全部成功时必须真正提交。
    QVERIFY2(m_database->runInTransaction([&](QString *innerError) {
                 return m_database->saveHost(makeHost(QStringLiteral("h3"), QStringLiteral("正常主机"),
                                                      QStringLiteral("c.example")), innerError)
                     && m_database->setSetting(QStringLiteral("批量导入"), QStringLiteral("完成"),
                                               innerError);
             }, &error),
             qPrintable(error));
    QVERIFY(m_database->findHost(QStringLiteral("h3"), &error).has_value());
    QCOMPARE(*m_database->setting(QStringLiteral("批量导入"), &error), QStringLiteral("完成"));
}

void ApplicationDatabaseTest::migrationFromVersionOnePreservesData()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString path = temporaryDirectory.filePath(QStringLiteral("legacy.db"));

    // 手工构造一个 schema 版本 1 的旧库：只有版本 1 的列，且已经存有用户数据。
    const QString legacyConnection = QStringLiteral("legacy-fixture");
    {
        // legacy 与 query 必须先于 removeDatabase 析构，否则 Qt 会警告连接仍在使用中。
        QSqlDatabase legacy = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), legacyConnection);
        legacy.setDatabaseName(path);
        QVERIFY2(legacy.open(), qPrintable(legacy.lastError().text()));
        QSqlQuery query(legacy);
        const QStringList statements = {
            QStringLiteral("CREATE TABLE schema_version (version INTEGER NOT NULL)"),
            QStringLiteral("INSERT INTO schema_version(version) VALUES (1)"),
            QStringLiteral("CREATE TABLE hosts (id TEXT PRIMARY KEY, "
                           "parent_id TEXT REFERENCES hosts(id) ON DELETE CASCADE, "
                           "kind INTEGER NOT NULL, name TEXT NOT NULL, host TEXT, port INTEGER, "
                           "username TEXT, credential_id TEXT, identity_file TEXT, tags TEXT, "
                           "proxy_jump TEXT, legacy_hmac INTEGER NOT NULL DEFAULT 0, "
                           "sort_order INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL, "
                           "updated_at TEXT NOT NULL)"),
            QStringLiteral("INSERT INTO hosts VALUES ('old-host', NULL, 1, '旧主机', 'legacy.example', "
                           "2022, 'root', NULL, NULL, 'legacy', NULL, 0, 3, "
                           "'2025-01-01T00:00:00.000Z', '2025-01-01T00:00:00.000Z')"),
            QStringLiteral("CREATE TABLE credentials (id TEXT PRIMARY KEY, name TEXT NOT NULL, "
                           "type INTEGER NOT NULL, encrypted_payload BLOB NOT NULL, "
                           "created_at TEXT NOT NULL, updated_at TEXT NOT NULL)"),
            QStringLiteral("INSERT INTO credentials VALUES ('old-cred', '旧凭据', 0, X'0102FF', "
                           "'2025-01-01T00:00:00.000Z', '2025-01-01T00:00:00.000Z')"),
            QStringLiteral("CREATE TABLE connection_history (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                           "host_id TEXT, connected_at TEXT NOT NULL, disconnected_at TEXT, "
                           "result INTEGER, error TEXT)"),
            QStringLiteral("INSERT INTO connection_history(host_id, connected_at, result) "
                           "VALUES ('old-host', '2025-01-01T00:00:00.000Z', 1)"),
            QStringLiteral("CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)"),
            QStringLiteral("INSERT INTO settings VALUES ('ui/theme', 'dark')")
        };
        for (const QString &statement : statements) {
            QVERIFY2(query.exec(statement), qPrintable(query.lastError().text()));
        }
        legacy.close();
    }
    QSqlDatabase::removeDatabase(legacyConnection);

    ApplicationDatabase upgraded(path);
    QString error;
    QVERIFY2(upgraded.initialize(&error), qPrintable(error));
    QCOMPARE(*upgraded.schemaVersion(&error), ApplicationDatabase::expectedSchemaVersion());

    // 旧数据必须一字不差地留在原地。
    const std::optional<HostRecord> host = upgraded.findHost(QStringLiteral("old-host"), &error);
    QVERIFY2(host.has_value(), "迁移后旧主机丢失");
    QCOMPARE(host->name, QStringLiteral("旧主机"));
    QCOMPARE(host->host, QStringLiteral("legacy.example"));
    QCOMPARE(host->port, std::optional<quint16>(2022));
    QCOMPARE(host->username, QStringLiteral("root"));
    QCOMPARE(host->tags, QStringList{QStringLiteral("legacy")});
    QCOMPARE(host->sortOrder, 3);

    const std::optional<CredentialRecord> credential =
        upgraded.findCredential(QStringLiteral("old-cred"), &error);
    QVERIFY2(credential.has_value(), "迁移后旧凭据丢失");
    QCOMPARE(credential->name, QStringLiteral("旧凭据"));
    QCOMPARE(credential->encryptedPayload, QByteArrayLiteral("\x01\x02\xff"));
    QVERIFY(credential->cipher.isEmpty()); // 版本 1 没有该列，升级后为空是预期结果。

    bool ok = false;
    QCOMPARE(upgraded.recentConnections(10, QString(), &ok, &error).size(), 1);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(*upgraded.setting(QStringLiteral("ui/theme"), &error), QStringLiteral("dark"));

    // 新增的列在升级后必须可用。
    CredentialRecord fresh;
    fresh.id = QStringLiteral("new-cred");
    fresh.name = QStringLiteral("新凭据");
    fresh.encryptedPayload = QByteArray(16, '\x43');
    fresh.cipher = QStringLiteral("xchacha20-poly1305-v1");
    fresh.nonce = QByteArray(24, '\x44');
    fresh.kdfAlgorithm = QStringLiteral("argon2id13");
    fresh.kdfSalt = QByteArray(16, '\x45');
    fresh.kdfIterations = 2;
    fresh.kdfMemoryKib = 65536;
    fresh.kdfParallelism = 1;
    QVERIFY2(upgraded.saveCredential(fresh, &error), qPrintable(error));
    QCOMPARE(upgraded.findCredential(fresh.id, &error)->kdfAlgorithm, QStringLiteral("argon2id13"));

    // 重复初始化（用户第二次启动）不得再次执行 ALTER TABLE。
    ApplicationDatabase reopened(path);
    QVERIFY2(reopened.initialize(&error), qPrintable(error));
    QCOMPARE(*reopened.schemaVersion(&error), ApplicationDatabase::expectedSchemaVersion());
    QVERIFY(reopened.findHost(QStringLiteral("old-host"), &error).has_value());
}

void ApplicationDatabaseTest::migrationToVersionSixSplitsSharedCredentialReferences()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString path = temporaryDirectory.filePath(QStringLiteral("shared-credentials-v5.db"));

    // 直接构造版本 5 的最小合法数据库，精确复现旧版“复制主机”留下的三个相同引用。迁移
    // 不需要解密密文，因此测试使用固定字节即可验证 payload、nonce 和公开元数据原样复制。
    const QString connectionName = QStringLiteral("shared-credential-v5-fixture");
    {
        QSqlDatabase legacy = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        legacy.setDatabaseName(path);
        QVERIFY2(legacy.open(), qPrintable(legacy.lastError().text()));
        QSqlQuery query(legacy);
        const QStringList statements = {
            QStringLiteral("CREATE TABLE schema_version (version INTEGER NOT NULL)"),
            QStringLiteral("INSERT INTO schema_version VALUES (5)"),
            QStringLiteral("CREATE TABLE hosts (id TEXT PRIMARY KEY, parent_id TEXT, kind INTEGER "
                           "NOT NULL, name TEXT NOT NULL, host TEXT, port INTEGER, username TEXT, "
                           "credential_id TEXT, identity_file TEXT, tags TEXT, proxy_jump TEXT, "
                           "legacy_hmac INTEGER NOT NULL DEFAULT 0, sort_order INTEGER NOT NULL "
                           "DEFAULT 0, created_at TEXT NOT NULL, updated_at TEXT NOT NULL)"),
            QStringLiteral("CREATE TABLE credentials (id TEXT PRIMARY KEY, name TEXT NOT NULL, "
                           "type INTEGER NOT NULL, encrypted_payload BLOB NOT NULL, cipher TEXT, "
                           "nonce BLOB, kdf_algorithm TEXT, kdf_salt BLOB, "
                           "kdf_iterations INTEGER NOT NULL DEFAULT 0, "
                           "kdf_memory_kib INTEGER NOT NULL DEFAULT 0, "
                           "kdf_parallelism INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL, "
                           "updated_at TEXT NOT NULL)"),
            QStringLiteral("CREATE TABLE host_chain_members (owner_host_id TEXT NOT NULL, "
                           "hop_index INTEGER NOT NULL, jump_host_id TEXT NOT NULL, "
                           "PRIMARY KEY(owner_host_id, hop_index))"),
            QStringLiteral("INSERT INTO credentials VALUES ('shared-cred', '共享凭据', 0, "
                           "X'11223344556677889900AABBCCDDEEFF', 'xchacha20-poly1305-v1', "
                           "X'000102030405060708090A0B0C0D0E0F1011121314151617', "
                           "'local-key-v1', X'A0A1A2A3A4A5A6A7A8A9AAABACADAEAF', 1, 0, 1, "
                           "'2026-01-01T00:00:00.000Z', '2026-01-01T00:00:00.000Z')"),
            QStringLiteral("INSERT INTO hosts VALUES "
                           "('h1', NULL, 1, '主机一', '192.0.2.1', 22, 'root', 'shared-cred', NULL, "
                           "NULL, NULL, 0, 0, '2026-01-01T00:00:00.000Z', "
                           "'2026-01-01T00:00:00.000Z'), "
                           "('h2', NULL, 1, '主机二', '192.0.2.2', 22, 'root', 'shared-cred', NULL, "
                           "NULL, NULL, 0, 1, '2026-01-01T00:00:00.000Z', "
                           "'2026-01-01T00:00:00.000Z'), "
                           "('h3', NULL, 1, '主机三', '192.0.2.3', 22, 'root', 'shared-cred', NULL, "
                           "NULL, NULL, 0, 2, '2026-01-01T00:00:00.000Z', "
                           "'2026-01-01T00:00:00.000Z')")
        };
        for (const QString &statement : statements) {
            QVERIFY2(query.exec(statement), qPrintable(query.lastError().text()));
        }
        legacy.close();
    }
    QSqlDatabase::removeDatabase(connectionName);

    ApplicationDatabase upgraded(path);
    QString error;
    QVERIFY2(upgraded.initialize(&error), qPrintable(error));
    QCOMPARE(*upgraded.schemaVersion(&error), 6);

    QSet<QString> credentialIds;
    for (const QString &hostId : {QStringLiteral("h1"), QStringLiteral("h2"),
                                  QStringLiteral("h3")}) {
        const std::optional<HostRecord> host = upgraded.findHost(hostId, &error);
        QVERIFY2(host.has_value(), qPrintable(error));
        QVERIFY2(!host->credentialId.isEmpty(), "迁移后主机丢失凭据引用");
        credentialIds.insert(host->credentialId);
        const std::optional<CredentialRecord> credential =
            upgraded.findCredential(host->credentialId, &error);
        QVERIFY2(credential.has_value(), qPrintable(error));
        QCOMPARE(credential->encryptedPayload,
                 QByteArray::fromHex("11223344556677889900AABBCCDDEEFF"));
        QCOMPARE(credential->nonce,
                 QByteArray::fromHex("000102030405060708090A0B0C0D0E0F1011121314151617"));
    }
    QCOMPARE(credentialIds.size(), 3);
}

QTEST_GUILESS_MAIN(ApplicationDatabaseTest)
#include "ApplicationDatabaseTest.moc"
