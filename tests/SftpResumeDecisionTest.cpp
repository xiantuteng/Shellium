#include "sftp/SftpService.h"
#include "sftp/SftpResumeLedger.h"

#include <QTemporaryDir>
#include <QTest>

/**
 * @brief 验证"传输 / 续传 / 跳过"的判定与续传凭据账本。
 *
 * 这一层决定了远端文件到底会不会被更新，出错的后果是最恶劣的一类：文件没更新却报告成功，
 * 或者两个不同文件被拼成一个损坏文件——两者都不会抛错，用户只有在很久以后才会发现。判定
 * 本身不接触网络与 libssh，因此可以在没有 SFTP 服务器的情况下把每条边界都钉死。
 */
class SftpResumeDecisionTest final : public QObject {
    Q_OBJECT

private slots:
    /** @brief 大小相同但内容不同的文件必须全量重传，绝不能被判成"已完成"。 */
    void retransmitsWhenSizesMatchButContentDiffers();

    /** @brief 修改时间没被实测证明可信时，"大小 + 时间一致"不足以跳过。 */
    void refusesToSkipWhenMtimeIsNotProvenTrustworthy();

    /** @brief 没有任何证据时，大小与时间戳一致也不足以跳过。 */
    void refusesToSkipWithoutAnyEvidence();

    /** @brief 存档证明"上次正是我们把这一版写到那里"时才允许跳过。 */
    void skipsWhenArchiveProvesLastSync();

    /** @brief 同一秒内改成另一份等长内容必须被识破，不能跳过。 */
    void retransmitsWhenSourceChangedWithinTheSameSecond();

    /** @brief 目标端被别人改过（时间戳不再等于回读值）时不得跳过。 */
    void retransmitsWhenDestinationWasTouchedByOthers();

    /** @brief 内容哈希一致即可跳过，与修改时间是否相同无关。 */
    void skipsWhenContentHashesMatch();

    /** @brief 哈希不一致必须重传，即使大小完全相同。 */
    void retransmitsWhenContentHashesDiffer();

    /** @brief 目标比源小但没有残片和凭据时，绝不允许在正式文件上续写。 */
    void neverResumesOntoTheFinalFile();

    /** @brief 残片、凭据与源三者一致时从检查点续传。 */
    void resumesFromCheckpointWhenLedgerProvesSource();

    /** @brief 源在中断期间被改过（含只改尾部）时，残片必须整体作废。 */
    void discardsPartialWhenSourceChangedAfterInterruption();

    /** @brief 残片比检查点短说明有写入没落盘，必须重传。 */
    void retransmitsWhenPartialIsShorterThanCheckpoint();

    /** @brief 残片比检查点长时仍从检查点续写，尾部字节会被重新写一遍。 */
    void resumesFromCheckpointEvenIfPartialIsLonger();

    /** @brief 覆盖策略下一律全量重传，任何证据都不产生跳过。 */
    void alwaysTransfersUnderOverwritePolicy();

    /** @brief 无哈希凭据时退到大小 + 修改时间判据，未知时间戳一律不匹配。 */
    void fallsBackToQuickCheckWhenLedgerHasNoHash_data();
    void fallsBackToQuickCheckWhenLedgerHasNoHash();

    /** @brief 残片必须落在正式文件的同一目录，且是隐藏文件。 */
    void buildsPartialPathNextToDestination_data();
    void buildsPartialPathNextToDestination();

    /** @brief 账本写入后必须能原样读回，且超过 2^53 的大小不被精度损坏。 */
    void storesAndReloadsLedgerRecordsWithoutPrecisionLoss();

    /** @brief 从未实测过的目标，修改时间一律不可信。 */
    void treatsUnknownTargetsAsUntrustedMtime();

    /** @brief 传输成功后凭据必须被清除并在 flush 后落盘，否则下次会拿旧凭据去判定。 */
    void discardsLedgerRecordOnDemand();
};

namespace {

using SftpTransfer::DestinationSnapshot;
using SftpTransfer::ExistingFilePolicy;
using SftpTransfer::PartialSnapshot;
using SftpTransfer::SourceSnapshot;
using SftpTransfer::TransferAction;

constexpr quint64 kSourceSize = 4096;
constexpr qint64 kSourceMtime = 1750000000;

/** @brief 构造一份"源与目标大小相同"的常见场景，用例只覆写与断言相关的字段。 */
SourceSnapshot makeSource(const QByteArray &hash = {},
                          const qint64 mtime = kSourceMtime,
                          const qint64 mtimeMs = kSourceMtime * 1000)
{
    return SourceSnapshot{kSourceSize, mtime, mtimeMs, hash};
}

DestinationSnapshot makeDestination(const QByteArray &hash = {}, const qint64 mtime = kSourceMtime)
{
    return DestinationSnapshot{true, kSourceSize, mtime, hash};
}

/** @brief 构造一条"上次我们正是把这一版写到那里"的完成存档。 */
SftpResume::LedgerRecord makeArchive(const qint64 sourceMtimeMs = kSourceMtime * 1000,
                                     const qint64 destinationMtime = kSourceMtime)
{
    SftpResume::LedgerRecord record;
    record.kind = SftpResume::RecordKind::Complete;
    record.sourcePath = QStringLiteral("/tmp/source.bin");
    record.sourceSize = kSourceSize;
    record.sourceMtime = kSourceMtime;
    record.sourceMtimeMs = sourceMtimeMs;
    record.destinationMtime = destinationMtime;
    record.updatedAt = QDateTime::currentSecsSinceEpoch();
    return record;
}

/** @brief 构造一条与给定源匹配的续传凭据。 */
SftpResume::LedgerRecord makeRecord(const quint64 bytesWritten,
                                    const QByteArray &hash,
                                    const qint64 mtime = kSourceMtime)
{
    SftpResume::LedgerRecord record;
    record.kind = SftpResume::RecordKind::Partial;
    record.sourcePath = QStringLiteral("/tmp/source.bin");
    record.sourceSize = kSourceSize;
    record.sourceMtime = mtime;
    record.sourceHash = hash;
    record.bytesWritten = bytesWritten;
    record.updatedAt = QDateTime::currentSecsSinceEpoch();
    return record;
}

} // namespace

void SftpResumeDecisionTest::retransmitsWhenSizesMatchButContentDiffers()
{
    // 用户报告的主症状：改了几个字符、大小恰好不变，于是远端根本没被更新，界面却报成功。
    // 判定层能拿到的证据是"两端时间戳不同"，因此必须重传。
    const SftpTransfer::TransferDecision decision = SftpTransfer::decideTransfer(
        makeSource({}, kSourceMtime + 120),
        makeDestination({}, kSourceMtime),
        PartialSnapshot{},
        std::nullopt,
        ExistingFilePolicy::ResumeWhenPossible,
        true);

    QCOMPARE(decision.action, TransferAction::FullTransfer);
    QCOMPARE(decision.resumeOffset, 0ULL);
}

void SftpResumeDecisionTest::refusesToSkipWhenMtimeIsNotProvenTrustworthy()
{
    // 目标端的时间戳没被实测证明能存住，存档里的回读值就不可信，不足以支撑"跳过"。
    const SftpTransfer::TransferDecision decision = SftpTransfer::decideTransfer(
        makeSource(),
        makeDestination(),
        PartialSnapshot{},
        makeArchive(),
        ExistingFilePolicy::ResumeWhenPossible,
        false);

    QCOMPARE(decision.action, TransferAction::FullTransfer);
}

void SftpResumeDecisionTest::refusesToSkipWithoutAnyEvidence()
{
    // 没有存档也没有哈希：大小与时间戳再怎么巧合一致，也证明不了内容相同。
    const SftpTransfer::TransferDecision decision = SftpTransfer::decideTransfer(
        makeSource(),
        makeDestination(),
        PartialSnapshot{},
        std::nullopt,
        ExistingFilePolicy::ResumeWhenPossible,
        true);

    QCOMPARE(decision.action, TransferAction::FullTransfer);
}

void SftpResumeDecisionTest::skipsWhenArchiveProvesLastSync()
{
    const SftpTransfer::TransferDecision decision = SftpTransfer::decideTransfer(
        makeSource(),
        makeDestination(),
        PartialSnapshot{},
        makeArchive(),
        ExistingFilePolicy::ResumeWhenPossible,
        true);

    QCOMPARE(decision.action, TransferAction::SkipIdentical);
}

void SftpResumeDecisionTest::retransmitsWhenSourceChangedWithinTheSameSecond()
{
    // 秒级时间戳的致命盲区：同一秒内把文件改成另一份等长内容，大小与秒数都不变。只有毫秒
    // 精度能分辨这是两次不同的写入——真实服务器上的集成测试正是在这里抓到了漏判。
    const SftpTransfer::TransferDecision decision = SftpTransfer::decideTransfer(
        makeSource({}, kSourceMtime, kSourceMtime * 1000 + 400),
        makeDestination(),
        PartialSnapshot{},
        makeArchive(kSourceMtime * 1000 + 120),
        ExistingFilePolicy::ResumeWhenPossible,
        true);

    QCOMPARE(decision.action, TransferAction::FullTransfer);
}

void SftpResumeDecisionTest::retransmitsWhenDestinationWasTouchedByOthers()
{
    // 目标端的时间戳不再等于我们当时回读到的值，说明那个文件被别人动过，存档不再成立。
    const SftpTransfer::TransferDecision decision = SftpTransfer::decideTransfer(
        makeSource(),
        makeDestination({}, kSourceMtime + 30),
        PartialSnapshot{},
        makeArchive(kSourceMtime * 1000, kSourceMtime),
        ExistingFilePolicy::ResumeWhenPossible,
        true);

    QCOMPARE(decision.action, TransferAction::FullTransfer);
}

void SftpResumeDecisionTest::skipsWhenContentHashesMatch()
{
    const QByteArray hash("a1b2c3");
    const SftpTransfer::TransferDecision decision = SftpTransfer::decideTransfer(
        makeSource(hash, kSourceMtime),
        makeDestination(hash, kSourceMtime + 999),
        PartialSnapshot{},
        std::nullopt,
        ExistingFilePolicy::ResumeWhenPossible,
        false);

    QCOMPARE(decision.action, TransferAction::SkipIdentical);
}

void SftpResumeDecisionTest::retransmitsWhenContentHashesDiffer()
{
    // 哈希证据优先于时间戳：时间戳相同只说明两次写入发生在同一秒，说明不了内容一致。
    const SftpTransfer::TransferDecision decision = SftpTransfer::decideTransfer(
        makeSource("a1b2c3", kSourceMtime),
        makeDestination("ffffff", kSourceMtime),
        PartialSnapshot{},
        std::nullopt,
        ExistingFilePolicy::ResumeWhenPossible,
        true);

    QCOMPARE(decision.action, TransferAction::FullTransfer);
}

void SftpResumeDecisionTest::neverResumesOntoTheFinalFile()
{
    // 旧实现正是在这里出事：目标比源小就从目标末尾接着写，于是"另一个文件的前半段 + 本文件
    // 的后半段"被拼成一个既不报错也无法察觉的坏文件。
    DestinationSnapshot destination = makeDestination();
    destination.size = kSourceSize / 2;

    const SftpTransfer::TransferDecision decision = SftpTransfer::decideTransfer(
        makeSource(),
        destination,
        PartialSnapshot{},
        std::nullopt,
        ExistingFilePolicy::ResumeWhenPossible,
        true);

    QCOMPARE(decision.action, TransferAction::FullTransfer);
    QCOMPARE(decision.resumeOffset, 0ULL);
}

void SftpResumeDecisionTest::resumesFromCheckpointWhenLedgerProvesSource()
{
    const QByteArray hash("a1b2c3");
    const SftpTransfer::TransferDecision decision = SftpTransfer::decideTransfer(
        makeSource(hash),
        DestinationSnapshot{},
        PartialSnapshot{true, 1024},
        makeRecord(1024, hash),
        ExistingFilePolicy::ResumeWhenPossible,
        false);

    QCOMPARE(decision.action, TransferAction::Resume);
    QCOMPARE(decision.resumeOffset, 1024ULL);
}

void SftpResumeDecisionTest::discardsPartialWhenSourceChangedAfterInterruption()
{
    // 只改了文件尾部时，残片覆盖的那段前缀依然一模一样。判据必须是**整个文件**的哈希，
    // 否则续传会把旧头和新尾拼在一起。
    const SftpTransfer::TransferDecision decision = SftpTransfer::decideTransfer(
        makeSource("tail-changed"),
        DestinationSnapshot{},
        PartialSnapshot{true, 1024},
        makeRecord(1024, "original-hash"),
        ExistingFilePolicy::ResumeWhenPossible,
        false);

    QCOMPARE(decision.action, TransferAction::FullTransfer);
    QCOMPARE(decision.resumeOffset, 0ULL);
}

void SftpResumeDecisionTest::retransmitsWhenPartialIsShorterThanCheckpoint()
{
    const QByteArray hash("a1b2c3");
    const SftpTransfer::TransferDecision decision = SftpTransfer::decideTransfer(
        makeSource(hash),
        DestinationSnapshot{},
        PartialSnapshot{true, 512},
        makeRecord(1024, hash),
        ExistingFilePolicy::ResumeWhenPossible,
        false);

    QCOMPARE(decision.action, TransferAction::FullTransfer);
}

void SftpResumeDecisionTest::resumesFromCheckpointEvenIfPartialIsLonger()
{
    // 最后一批写入可能已经落到服务端却来不及记账。检查点之后的字节重写一遍即可，绝不能
    // 反过来信任残片的实际长度——那部分没有任何凭据担保。
    const QByteArray hash("a1b2c3");
    const SftpTransfer::TransferDecision decision = SftpTransfer::decideTransfer(
        makeSource(hash),
        DestinationSnapshot{},
        PartialSnapshot{true, 2048},
        makeRecord(1024, hash),
        ExistingFilePolicy::ResumeWhenPossible,
        false);

    QCOMPARE(decision.action, TransferAction::Resume);
    QCOMPARE(decision.resumeOffset, 1024ULL);
}

void SftpResumeDecisionTest::alwaysTransfersUnderOverwritePolicy()
{
    const QByteArray hash("a1b2c3");
    const SftpTransfer::TransferDecision decision = SftpTransfer::decideTransfer(
        makeSource(hash),
        makeDestination(hash),
        PartialSnapshot{true, 1024},
        makeArchive(),
        ExistingFilePolicy::Overwrite,
        true);

    QCOMPARE(decision.action, TransferAction::FullTransfer);
}

void SftpResumeDecisionTest::fallsBackToQuickCheckWhenLedgerHasNoHash_data()
{
    QTest::addColumn<qint64>("recordMtime");
    QTest::addColumn<qint64>("sourceMtime");
    QTest::addColumn<int>("expectedAction");

    QTest::newRow("时间戳一致可续传")
        << kSourceMtime << kSourceMtime << static_cast<int>(TransferAction::Resume);
    QTest::newRow("时间戳不同必须重传")
        << kSourceMtime << (kSourceMtime + 1) << static_cast<int>(TransferAction::FullTransfer);
    // -1 表示"服务器没给出时间戳"。未知绝不能当成匹配，否则判据其实只剩下大小。
    QTest::newRow("凭据时间戳未知")
        << qint64{-1} << kSourceMtime << static_cast<int>(TransferAction::FullTransfer);
    QTest::newRow("源时间戳未知")
        << kSourceMtime << qint64{-1} << static_cast<int>(TransferAction::FullTransfer);
}

void SftpResumeDecisionTest::fallsBackToQuickCheckWhenLedgerHasNoHash()
{
    QFETCH(qint64, recordMtime);
    QFETCH(qint64, sourceMtime);
    QFETCH(int, expectedAction);

    const SftpTransfer::TransferDecision decision = SftpTransfer::decideTransfer(
        makeSource({}, sourceMtime),
        DestinationSnapshot{},
        PartialSnapshot{true, 1024},
        makeRecord(1024, {}, recordMtime),
        ExistingFilePolicy::ResumeWhenPossible,
        false);

    QCOMPARE(static_cast<int>(decision.action), expectedAction);
}

void SftpResumeDecisionTest::buildsPartialPathNextToDestination_data()
{
    QTest::addColumn<QString>("destination");
    QTest::addColumn<QString>("expected");

    QTest::newRow("普通远端路径")
        << QStringLiteral("/srv/app/config.yaml")
        << QStringLiteral("/srv/app/.config.yaml.shellium-part");
    QTest::newRow("根目录下的文件")
        << QStringLiteral("/payload.bin")
        << QStringLiteral("/.payload.bin.shellium-part");
    QTest::newRow("名字里带点")
        << QStringLiteral("/srv/a.b/c.tar.gz")
        << QStringLiteral("/srv/a.b/.c.tar.gz.shellium-part");
}

void SftpResumeDecisionTest::buildsPartialPathNextToDestination()
{
    QFETCH(QString, destination);
    QFETCH(QString, expected);

    QCOMPARE(SftpTransfer::partialPathFor(destination), expected);
}

void SftpResumeDecisionTest::storesAndReloadsLedgerRecordsWithoutPrecisionLoss()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("ledger.json"));

    // 超过 2^53 的大小若被当成 JSON 数值保存就会被静默舍入，而续传判定要求字节数**精确**
    // 相等，一个被舍入的大小会让超大文件永远续不上。
    constexpr quint64 hugeSize = 9007199254740993ULL;
    SftpResume::LedgerRecord record;
    record.sourcePath = QStringLiteral("/tmp/huge.img");
    record.sourceSize = hugeSize;
    record.sourceMtime = kSourceMtime;
    record.sourceHash = QByteArray("deadbeef");
    record.bytesWritten = hugeSize - 1;

    const QString key = SftpResume::Ledger::makeKey(QStringLiteral("host-a"),
                                                    QStringLiteral("/srv/huge.img"));
    {
        SftpResume::Ledger ledger(path);
        ledger.store(key, record);
        ledger.setMtimeTrusted(QStringLiteral("up|host-a"), true);
        // 析构同样会 flush，这里显式调用以表明"落盘"正是本用例要验证的契约。
        ledger.flush();
    }

    SftpResume::Ledger reloaded(path);
    const std::optional<SftpResume::LedgerRecord> loaded = reloaded.lookup(key);
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->sourceSize, hugeSize);
    QCOMPARE(loaded->bytesWritten, hugeSize - 1);
    QCOMPARE(loaded->sourceMtime, kSourceMtime);
    QCOMPARE(loaded->sourceHash, QByteArray("deadbeef"));
    QVERIFY(reloaded.mtimeTrusted(QStringLiteral("up|host-a")));
}

void SftpResumeDecisionTest::treatsUnknownTargetsAsUntrustedMtime()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SftpResume::Ledger ledger(directory.filePath(QStringLiteral("ledger.json")));

    QVERIFY(!ledger.mtimeTrusted(QStringLiteral("up|never-seen")));
    ledger.setMtimeTrusted(QStringLiteral("up|never-seen"), false);
    QVERIFY(!ledger.mtimeTrusted(QStringLiteral("up|never-seen")));
}

void SftpResumeDecisionTest::discardsLedgerRecordOnDemand()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("ledger.json"));
    const QString key = SftpResume::Ledger::makeKey(QStringLiteral("host-a"),
                                                    QStringLiteral("/srv/app.bin"));

    SftpResume::Ledger ledger(path);
    ledger.store(key, makeRecord(1024, "a1b2c3"));
    QVERIFY(ledger.lookup(key).has_value());
    ledger.discard(key);
    QVERIFY(!ledger.lookup(key).has_value());

    // 落盘按时间节流（几千个文件的目录树上每个文件都整份重写会退化成 O(n²) 次写盘），
    // 因此请求收尾必须 flush；否则下次启动又会拿一份早已作废的凭据去续传。
    ledger.flush();
    SftpResume::Ledger reloaded(path);
    QVERIFY(!reloaded.lookup(key).has_value());
}

QTEST_MAIN(SftpResumeDecisionTest)
#include "SftpResumeDecisionTest.moc"
