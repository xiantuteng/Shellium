#include "core/ConnectionConfigCodec.h"
#include "forwarding/PortForwardRule.h"
#include "forwarding/ProxyNetwork.h"
#include "forwarding/Socks5Protocol.h"

#include <QHostAddress>
#include "security/SshKeyManager.h"
#include "storage/ApplicationDatabase.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

/**
 * @brief 验证 SSH 密钥管理和主机配置交换的安全边界。
 *
 * 全部文件均位于 QTemporaryDir，数据库使用内存连接；测试不会读取或覆盖用户真实的
 * ~/.ssh、OpenSSH config 或 Shellium 数据库。
 */
class ConnectionToolsTest final : public QObject {
    Q_OBJECT

private slots:
    /** @brief Ed25519 私钥必须原子落盘为 0600，并生成可重新导入的 OpenSSH 公钥。 */
    void generatesEd25519WithPrivatePermissions();

    /** @brief 加密 RSA 私钥必须要求正确口令，错误口令不得产生可用密钥信息。 */
    void generatesEncryptedRsaAndValidatesPassphrase();

    /** @brief 当前 libssh 支持 ECDSA 时必须能生成并重新校验 P-256 私钥。 */
    void generatesEcdsaWhenSupported();

    /** @brief OpenSSH 支持字段应完整往返，条件、通配符与未知语义必须拒绝。 */
    void openSshCodecRoundTripsAndRejectsLossyInput();

    /** @brief JSON 应保持主机树但完全省略凭据，并拒绝外部注入秘密字段。 */
    void jsonCodecOmitsCredentialsAndPersistsWithNewIds();

    /** @brief 三类端口转发规则必须持久往返，并拒绝字段缺失或绑定分组。 */
    void portForwardRulesValidateAndPersist();

    /** @brief SOCKS5 只把命中的网段送进隧道，其余目标保留给系统默认路由。 */
    void proxyNetworksMatchRangesAndPersist();

    /** @brief SOCKS5 增量解析必须覆盖分片、域名和不支持命令的拒绝码。 */
    void socks5ProtocolHandlesFragmentedAndRejectedRequests();
};

void ConnectionToolsTest::generatesEd25519WithPrivatePermissions()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString privatePath = directory.filePath(QStringLiteral("id_ed25519"));
    SshKeyManager::KeyInfo generated;
    QString error;
    QVERIFY2(SshKeyManager::generate(SshKeyManager::Algorithm::Ed25519,
                                     privatePath,
                                     {},
                                     QStringLiteral("shellium-test"),
                                     0,
                                     &generated,
                                     &error),
             qPrintable(error));
    QVERIFY(QFileInfo::exists(privatePath));
    QVERIFY(QFileInfo::exists(privatePath + QStringLiteral(".pub")));
    QVERIFY(generated.publicKeyLine.startsWith(QStringLiteral("ssh-ed25519 ")));
    QVERIFY(generated.publicKeyLine.endsWith(QStringLiteral(" shellium-test")));

    const QFileDevice::Permissions permissions = QFileInfo(privatePath).permissions();
    QVERIFY(permissions.testFlag(QFileDevice::ReadOwner));
    QVERIFY(permissions.testFlag(QFileDevice::WriteOwner));
    QVERIFY(!(permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                             | QFileDevice::ExeGroup | QFileDevice::ReadOther
                             | QFileDevice::WriteOther | QFileDevice::ExeOther)));

    SshKeyManager::KeyInfo imported;
    QCOMPARE(SshKeyManager::inspectPrivateKey(privatePath, {}, &imported, &error),
             SshKeyManager::ImportStatus::Valid);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(imported.publicKeyLine.startsWith(QStringLiteral("ssh-ed25519 ")));

    // 默认拒绝覆盖，避免用户选错路径时无提示地销毁既有私钥。
    QVERIFY(!SshKeyManager::generate(SshKeyManager::Algorithm::Ed25519,
                                     privatePath, {}, {}, 0, nullptr, &error));
}

void ConnectionToolsTest::generatesEncryptedRsaAndValidatesPassphrase()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString privatePath = directory.filePath(QStringLiteral("id_rsa"));
    QString error;
    QVERIFY2(SshKeyManager::generate(SshKeyManager::Algorithm::Rsa,
                                     privatePath,
                                     QStringLiteral("rsa-protection-passphrase"),
                                     QStringLiteral("rsa-test"),
                                     2048,
                                     nullptr,
                                     &error),
             qPrintable(error));

    QCOMPARE(SshKeyManager::inspectPrivateKey(privatePath, {}, nullptr, &error),
             SshKeyManager::ImportStatus::PassphraseRequired);
    QCOMPARE(SshKeyManager::inspectPrivateKey(privatePath,
                                              QStringLiteral("wrong-passphrase"),
                                              nullptr,
                                              &error),
             SshKeyManager::ImportStatus::Invalid);
    SshKeyManager::KeyInfo imported;
    error.clear();
    QCOMPARE(SshKeyManager::inspectPrivateKey(privatePath,
                                              QStringLiteral("rsa-protection-passphrase"),
                                              &imported,
                                              &error),
             SshKeyManager::ImportStatus::Valid);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(imported.publicKeyLine.startsWith(QStringLiteral("ssh-rsa ")));

    QFile privateFile(privatePath);
    QVERIFY(privateFile.open(QIODevice::ReadOnly));
    QVERIFY(!privateFile.readAll().contains("rsa-protection-passphrase"));
}

void ConnectionToolsTest::generatesEcdsaWhenSupported()
{
    if (!SshKeyManager::supportedAlgorithms().contains(SshKeyManager::Algorithm::EcdsaP256)) {
        QSKIP("当前 libssh 构建不支持 ECDSA P-256");
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString privatePath = directory.filePath(QStringLiteral("id_ecdsa"));
    QString error;
    QVERIFY2(SshKeyManager::generate(SshKeyManager::Algorithm::EcdsaP256,
                                     privatePath, {}, {}, 0, nullptr, &error),
             qPrintable(error));
    SshKeyManager::KeyInfo imported;
    QCOMPARE(SshKeyManager::inspectPrivateKey(privatePath, {}, &imported, &error),
             SshKeyManager::ImportStatus::Valid);
    QVERIFY(imported.publicKeyLine.startsWith(QStringLiteral("ecdsa-sha2-nistp256 ")));
}

void ConnectionToolsTest::openSshCodecRoundTripsAndRejectsLossyInput()
{
    const QByteArray source = QByteArrayLiteral(
        "# production database\n"
        "Host prod-db\n"
        "    HostName db.internal\n"
        "    User deploy\n"
        "    Port 2222\n"
        "    IdentityFile \"~/.ssh/id production\"\n"
        "    ProxyJump edge@example.com:22,bastion@inside:2200\n");
    QList<HostRecord> records;
    QString error;
    QVERIFY2(ConnectionConfigCodec::importOpenSsh(source, &records, &error), qPrintable(error));
    QCOMPARE(records.size(), 1);
    QCOMPARE(records.first().name, QStringLiteral("prod-db"));
    QCOMPARE(records.first().host, QStringLiteral("db.internal"));
    QCOMPARE(records.first().username, QStringLiteral("deploy"));
    QCOMPARE(records.first().port, std::optional<quint16>(2222));
    QCOMPARE(records.first().identityFile, QStringLiteral("~/.ssh/id production"));
    QCOMPARE(records.first().proxyJumps.size(), 2);

    const QByteArray exported = ConnectionConfigCodec::exportOpenSsh(records, &error);
    QVERIFY2(!exported.isEmpty(), qPrintable(error));
    QVERIFY(!exported.contains("Password"));
    QList<HostRecord> reparsed;
    QVERIFY2(ConnectionConfigCodec::importOpenSsh(exported, &reparsed, &error), qPrintable(error));
    QCOMPARE(reparsed.first().host, records.first().host);
    QCOMPARE(reparsed.first().identityFile, records.first().identityFile);

    for (const QByteArray invalid : {
             QByteArrayLiteral("Include ~/.ssh/conf.d/*\n"),
             QByteArrayLiteral("Host *\n  User root\n"),
             QByteArrayLiteral("Host one\n  Match user root\n"),
             QByteArrayLiteral("Host one\n  ForwardAgent yes\n")}) {
        error.clear();
        QVERIFY(!ConnectionConfigCodec::importOpenSsh(invalid, &records, &error));
        QVERIFY2(!error.isEmpty(), invalid.constData());
    }
}

void ConnectionToolsTest::jsonCodecOmitsCredentialsAndPersistsWithNewIds()
{
    HostRecord group;
    group.id = QStringLiteral("export-group");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("生产环境");
    HostRecord jump;
    jump.id = QStringLiteral("export-jump");
    jump.parentId = group.id;
    jump.name = QStringLiteral("生产跳板机");
    jump.host = QStringLiteral("edge.example.com");
    HostRecord host;
    host.id = QStringLiteral("export-host");
    host.parentId = group.id;
    host.name = QStringLiteral("数据库");
    host.host = QStringLiteral("db.internal");
    host.username = QStringLiteral("deploy");
    host.port = 2200;
    host.credentialId = QStringLiteral("must-not-be-exported");
    host.proxyJumps = {QStringLiteral("edge@example.com")};
    host.hostChain = {jump.id};

    QString error;
    const QByteArray json = ConnectionConfigCodec::exportJson({group, jump, host}, &error);
    QVERIFY2(!json.isEmpty(), qPrintable(error));
    QVERIFY(!json.contains("credentialId"));
    QVERIFY(!json.contains("must-not-be-exported"));
    QVERIFY(!json.contains("password"));

    QList<HostRecord> imported;
    QVERIFY2(ConnectionConfigCodec::importJson(json, &imported, &error), qPrintable(error));
    QCOMPARE(imported.size(), 3);
    QVERIFY(imported.at(2).credentialId.isEmpty());
    QCOMPARE(imported.at(2).hostChain, QStringList{jump.id});

    ApplicationDatabase database(ApplicationDatabase::inMemoryPath());
    QVERIFY2(database.initialize(&error), qPrintable(error));
    QVERIFY2(ConnectionConfigCodec::persistImported(database, imported, {}, &error), qPrintable(error));
    bool ok = false;
    const QList<HostRecord> roots = database.listHostsInGroup({}, &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(roots.size(), 1);
    QVERIFY(roots.first().id != group.id);
    const QList<HostRecord> children = database.listHostsInGroup(roots.first().id, &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(children.size(), 2);
    QString importedJumpId;
    QString importedTargetId;
    for (const HostRecord &child : children) {
        if (child.name == jump.name) {
            importedJumpId = child.id;
        } else if (child.name == host.name) {
            importedTargetId = child.id;
            QVERIFY(child.credentialId.isEmpty());
        }
    }
    QVERIFY(!importedJumpId.isEmpty());
    QVERIFY(!importedTargetId.isEmpty());
    QCOMPARE(database.findHost(importedTargetId, &error)->hostChain,
             QStringList{importedJumpId});

    const QByteArray malicious = QByteArrayLiteral(
        R"({"format":"shellium-hosts","version":1,"hosts":[],"password":"leak"})");
    error.clear();
    QVERIFY(!ConnectionConfigCodec::importJson(malicious, &imported, &error));
    QVERIFY(error.contains(QStringLiteral("password")));
}

void ConnectionToolsTest::portForwardRulesValidateAndPersist()
{
    ApplicationDatabase database(ApplicationDatabase::inMemoryPath());
    QString error;
    QVERIFY2(database.initialize(&error), qPrintable(error));

    HostRecord host;
    host.id = QStringLiteral("forward-host");
    host.kind = HostNodeKind::Host;
    host.name = QStringLiteral("转发目标主机");
    host.host = QStringLiteral("server.example.test");
    host.username = QStringLiteral("tester");
    QVERIFY2(database.saveHost(host, &error), qPrintable(error));

    PortForwardRule local;
    local.id = QStringLiteral("local-rule");
    local.hostId = host.id;
    local.name = QStringLiteral("数据库本地转发");
    local.type = PortForwardType::Local;
    local.bindHost = QStringLiteral("127.0.0.1");
    local.bindPort = 15432;
    local.targetHost = QStringLiteral("db.internal");
    local.targetPort = 5432;
    local.autoStart = true;
    QVERIFY(local.isValid(&error));
    QVERIFY2(database.savePortForward(local, &error), qPrintable(error));

    PortForwardRule dynamic = local;
    dynamic.id = QStringLiteral("dynamic-rule");
    dynamic.name = QStringLiteral("动态代理");
    dynamic.type = PortForwardType::Dynamic;
    dynamic.bindPort = 1080;
    dynamic.targetHost.clear();
    dynamic.targetPort = 0;
    dynamic.autoStart = false;
    QVERIFY2(database.savePortForward(dynamic, &error), qPrintable(error));

    bool ok = false;
    const QList<PortForwardRule> rules = database.listPortForwards(host.id, &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(rules.size(), 2);
    const std::optional<PortForwardRule> restored = database.findPortForward(local.id, &error);
    QVERIFY(restored.has_value());
    QCOMPARE(restored->targetPort, quint16(5432));

    PortForwardRule invalid = local;
    invalid.id = QStringLiteral("invalid-rule");
    invalid.targetPort = 0;
    QVERIFY(!database.savePortForward(invalid, &error));
}

void ConnectionToolsTest::proxyNetworksMatchRangesAndPersist()
{
    const auto egress = [](const QStringList &networks, const QString &host,
                           const QStringList &resolved = {}, const bool failed = false) {
        return ProxyNetwork::selectEgress(networks, host, resolved, failed).egress;
    };
    const auto target = [](const QStringList &networks, const QString &host,
                           const QStringList &resolved = {}, const bool failed = false) {
        return ProxyNetwork::selectEgress(networks, host, resolved, failed).targetHost;
    };

    // 没填网段时保持原行为：域名和 IP 都进 SSH，而且不要求本地已经解析出地址。
    QCOMPARE(egress({}, QStringLiteral("example.com"), {}, true), ProxyNetwork::Egress::Tunnel);
    QCOMPARE(egress({}, QStringLiteral("8.8.8.8")), ProxyNetwork::Egress::Tunnel);

    const QStringList intranet = {QStringLiteral("10.0.0.0/8"), QStringLiteral("192.168.1.20/24")};
    QCOMPARE(egress(intranet, QStringLiteral("10.1.2.3")), ProxyNetwork::Egress::Tunnel);
    QCOMPARE(egress(intranet, QStringLiteral("11.0.0.1")), ProxyNetwork::Egress::Direct);
    QCOMPARE(egress(intranet, QStringLiteral("192.168.1.255")), ProxyNetwork::Egress::Tunnel);
    QCOMPARE(egress(intranet, QStringLiteral("192.168.2.1")), ProxyNetwork::Egress::Direct);
    QCOMPARE(egress(intranet, QStringLiteral("::ffff:10.8.0.1")), ProxyNetwork::Egress::Tunnel);
    QCOMPARE(egress({QStringLiteral("2001:db8::/32")}, QStringLiteral("2001:db8::1")),
             ProxyNetwork::Egress::Tunnel);
    QCOMPARE(egress({QStringLiteral("2001:db8::/32")}, QStringLiteral("2001:db9::1")),
             ProxyNetwork::Egress::Direct);
    // 域名命中后改连解析出的那个 IP，避免远端 DNS 把它解析到网段外。
    QCOMPARE(egress(intranet, QStringLiteral("intranet.example"),
                    {QStringLiteral("8.8.8.8"), QStringLiteral("10.9.9.9")}),
             ProxyNetwork::Egress::Tunnel);
    QCOMPARE(target(intranet, QStringLiteral("intranet.example"),
                    {QStringLiteral("8.8.8.8"), QStringLiteral("10.9.9.9")}),
             QStringLiteral("10.9.9.9"));
    QCOMPARE(egress(intranet, QStringLiteral("example.com"),
                    {QStringLiteral("1.1.1.1")}),
             ProxyNetwork::Egress::Direct);
    QCOMPARE(target(intranet, QStringLiteral("example.com"), {QStringLiteral("1.1.1.1")}),
             QStringLiteral("example.com"));
    QCOMPARE(egress(intranet, QStringLiteral("missing.example"), {}, true),
             ProxyNetwork::Egress::Direct);

    const ProxyNetwork::ParseResult canonical = ProxyNetwork::normalize(
        {QStringLiteral("  10.1.2.3，192.168.1.20/24\n2001:0DB8::/32、10.0.0.0/8 ")});
    QVERIFY(canonical.invalidEntry.isEmpty());
    QVERIFY(!canonical.tooMany);
    QCOMPARE(canonical.networks,
             QStringList({QStringLiteral("10.1.2.3/32"),
                          QStringLiteral("192.168.1.0/24"),
                          QStringLiteral("2001:db8::/32"),
                          QStringLiteral("10.0.0.0/8")}));
    QVERIFY(!ProxyNetwork::normalize({QStringLiteral("example.com")}).invalidEntry.isEmpty());
    QStringList tooMany;
    for (int index = 0; index < ProxyNetwork::kMaxProxyNetworks + 1; ++index) {
        tooMany.append(QStringLiteral("10.%1.%2.1/32").arg(index / 256).arg(index % 256));
    }
    QVERIFY(ProxyNetwork::normalize(tooMany).tooMany);
    // 运行时拿到的是 QHostAddress 的文本，解析器必须接受这一种写法。
    const QHostAddress qtAddress(QStringLiteral("2001:db8::1"));
    QVERIFY(ProxyNetwork::isIpLiteral(qtAddress.toString()));
    QCOMPARE(egress({QStringLiteral("2001:db8::/32")}, qtAddress.toString()),
             ProxyNetwork::Egress::Tunnel);

    ApplicationDatabase database(ApplicationDatabase::inMemoryPath());
    QString error;
    QVERIFY2(database.initialize(&error), qPrintable(error));
    HostRecord host;
    host.id = QStringLiteral("proxy-host");
    host.kind = HostNodeKind::Host;
    host.name = QStringLiteral("网段主机");
    host.host = QStringLiteral("server.example.test");
    host.username = QStringLiteral("tester");
    QVERIFY2(database.saveHost(host, &error), qPrintable(error));

    PortForwardRule dynamic;
    dynamic.id = QStringLiteral("socks-split");
    dynamic.hostId = host.id;
    dynamic.name = QStringLiteral("分流代理");
    dynamic.type = PortForwardType::Dynamic;
    dynamic.bindHost = QStringLiteral("127.0.0.1");
    dynamic.bindPort = 1080;
    dynamic.proxyNetworks = QStringList{QStringLiteral("10.0.0.0/8"), QStringLiteral("192.168.1.10")};
    QVERIFY2(dynamic.isValid(&error), qPrintable(error));
    QVERIFY2(database.savePortForward(dynamic, &error), qPrintable(error));
    const std::optional<PortForwardRule> restored = database.findPortForward(dynamic.id, &error);
    QVERIFY2(restored.has_value(), qPrintable(error));
    QCOMPARE(restored->proxyNetworks,
             QStringList({QStringLiteral("10.0.0.0/8"), QStringLiteral("192.168.1.10/32")}));

    PortForwardRule local = dynamic;
    local.id = QStringLiteral("local-ignores-networks");
    local.name = QStringLiteral("本地转发");
    local.type = PortForwardType::Local;
    local.targetHost = QStringLiteral("db.internal");
    local.targetPort = 5432;
    local.proxyNetworks = QStringList{QStringLiteral("10.0.0.0/8")};
    QVERIFY2(database.savePortForward(local, &error), qPrintable(error));
    QCOMPARE(database.findPortForward(local.id, &error)->proxyNetworks, QStringList());

    PortForwardRule rejected = dynamic;
    rejected.id = QStringLiteral("bad-network");
    rejected.proxyNetworks = QStringList{QStringLiteral("not-a-cidr")};
    QVERIFY(!rejected.isValid(&error));
    QVERIFY(!database.savePortForward(rejected, &error));
    QVERIFY(error.contains(QStringLiteral("not-a-cidr")));
}

void ConnectionToolsTest::socks5ProtocolHandlesFragmentedAndRejectedRequests()
{
    QCOMPARE(Socks5Protocol::parseGreeting(QByteArray::fromHex("05")).status,
             Socks5Protocol::ParseStatus::NeedMore);
    const Socks5Protocol::GreetingResult greeting =
        Socks5Protocol::parseGreeting(QByteArray::fromHex("05020002"));
    QCOMPARE(greeting.status, Socks5Protocol::ParseStatus::Complete);
    QCOMPARE(greeting.response, QByteArray::fromHex("0500"));

    QByteArray domainRequest = QByteArray::fromHex("05010003");
    domainRequest.append(char(11));
    domainRequest.append("example.com", 11);
    domainRequest.append(QByteArray::fromHex("01bb"));
    const Socks5Protocol::ConnectResult domain =
        Socks5Protocol::parseConnectRequest(domainRequest);
    QCOMPARE(domain.status, Socks5Protocol::ParseStatus::Complete);
    QCOMPARE(domain.host, QStringLiteral("example.com"));
    QCOMPARE(domain.port, quint16(443));

    const Socks5Protocol::ConnectResult unsupported =
        Socks5Protocol::parseConnectRequest(QByteArray::fromHex("050200017f0000010016"));
    QCOMPARE(unsupported.status, Socks5Protocol::ParseStatus::Reject);
    QCOMPARE(unsupported.replyCode, quint8(0x07));
    QCOMPARE(Socks5Protocol::reply(0x00), QByteArray::fromHex("05000001000000000000"));
}

QTEST_GUILESS_MAIN(ConnectionToolsTest)
#include "ConnectionToolsTest.moc"
