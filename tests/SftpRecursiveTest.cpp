#include "sftp/SftpService.h"

#include <QTest>

/**
 * @brief 验证 SFTP 递归传输的纯逻辑：路径安全、遍历顺序、跳过判定、失败聚合与进度计算。
 *
 * 这些测试不访问网络也不访问文件系统。递归传输真正危险的部分——远端返回的名字把写入位置
 * 带到目标目录之外、符号链接导致无限递归、目录顺序错误导致 mkdir 失败——全部发生在把遍历
 * 结果转成传输计划这一步，因此可以在没有真实 SFTP 服务器的情况下被完整覆盖。
 */
class SftpRecursiveTest final : public QObject {
    Q_OBJECT

private slots:
    /** @brief 权限的符号形式必须与 ls -l 一致，包括 setuid/setgid/sticky 位的大小写切换。 */
    void formatsPermissionsAsSymbolicTextLikeLs_data();
    void formatsPermissionsAsSymbolicTextLikeLs();

    /** @brief 远端路径拼接必须使用 POSIX 分隔符，且不能在根目录上产生 "//"。 */
    void joinsRemotePathsWithoutDoubledSeparators();

    /** @brief 词法规范化必须消解 "."、".." 和重复分隔符。 */
    void normalizesPosixPaths_data();
    void normalizesPosixPaths();

    /** @brief 前缀相同但不是子路径的兄弟目录不得被判为"在根目录内"。 */
    void rejectsSiblingDirectoryWithSharedPrefix();

    /** @brief 含 ".." 的相对路径不得逃逸出目标根目录。 */
    void rejectsRelativePathEscapes_data();
    void rejectsRelativePathEscapes();

    /** @brief 遍历结果里的绝对路径不可信，必须直接拒绝。 */
    void rejectsAbsoluteRelativePath();

    /** @brief 计划中任一目录都必须排在它自己的内容之前。 */
    void ordersDirectoriesBeforeTheirContents();

    /** @brief 符号链接一律跳过并记录原因，不得进入传输项。 */
    void skipsSymbolicLinksWithReason();

    /** @brief 逃逸条目必须被挡在传输项之外并记入跳过清单。 */
    void skipsEscapingEntriesInsteadOfTransferringThem();

    /** @brief 计划必须统计出准确的文件数、目录数与总字节数。 */
    void countsFilesDirectoriesAndTotalBytes();

    /** @brief 进度百分比必须限幅，总量未知时返回不确定标记。 */
    void computesProgressPercent_data();
    void computesProgressPercent();

    /** @brief 汇总消息必须同时呈现成功计数、跳过清单和失败清单。 */
    void aggregatesSkippedAndFailedEntriesIntoMessage();

    /** @brief 超长失败清单必须截断并说明省略了多少项。 */
    void truncatesOverlongFailureList();

    /** @brief 被取消的传输必须在消息里明确说明，不能伪装成成功。 */
    void reportsCancellationDistinctlyFromSuccess();

    /** @brief 手动选择外层后，连接链的无关刷新不得重新抢回最内层。 */
    void preservesManualServerSelectionAcrossRefresh();

    /** @brief 列头点击必须按 升序→降序→默认 三态循环，换列则重新从升序开始。 */
    void cyclesColumnSortThroughAscendingDescendingDefault();

    /** @brief 只有开关开启且上报服务器与当前选中一致时才应跟随终端 cwd。 */
    void decidesWhenToFollowTerminalWorkingDirectory_data();
    void decidesWhenToFollowTerminalWorkingDirectory();
};

namespace {

/** @brief 构造一个遍历条目，让用例只写出与本次断言相关的字段。 */
SftpRecursive::SourceEntry makeEntry(const QString &relativePath,
                                     const bool isDirectory = false,
                                     const quint64 size = 0,
                                     const bool isSymbolicLink = false)
{
    SftpRecursive::SourceEntry entry;
    entry.relativePath = relativePath;
    entry.isDirectory = isDirectory;
    entry.size = size;
    entry.isSymbolicLink = isSymbolicLink;
    return entry;
}

/** @brief 提取计划中各传输项的目标路径，便于对顺序做整体断言。 */
QStringList destinationPaths(const SftpRecursive::TransferPlan &plan)
{
    QStringList paths;
    for (const SftpRecursive::TransferItem &item : plan.items) {
        paths.append(item.destinationPath);
    }
    return paths;
}

} // namespace

void SftpRecursiveTest::formatsPermissionsAsSymbolicTextLikeLs_data()
{
    QTest::addColumn<quint32>("permissions");
    QTest::addColumn<bool>("isDirectory");
    QTest::addColumn<bool>("isSymbolicLink");
    QTest::addColumn<QString>("expected");

    QTest::newRow("0644 regular file") << quint32(0644) << false << false
                                       << QStringLiteral("-rw-r--r--");
    QTest::newRow("0755 directory") << quint32(0755) << true << false
                                    << QStringLiteral("drwxr-xr-x");
    QTest::newRow("0777 symlink") << quint32(0777) << false << true
                                  << QStringLiteral("lrwxrwxrwx");
    // 1777（/tmp 的典型权限）：sticky 位配合其他人有执行权限，显示为小写 t。
    QTest::newRow("1777 sticky with execute") << quint32(01777) << true << false
                                              << QStringLiteral("drwxrwxrwt");
    // 4750：setuid 位配合属主有执行权限，显示为小写 s；没有执行权限的位置仍是短横线。
    QTest::newRow("4750 setuid with execute") << quint32(04750) << false << false
                                              << QStringLiteral("-rwsr-x---");
    // 4600：setuid 位但属主没有执行权限，显示为大写 S，信息不能因此丢失。
    QTest::newRow("4600 setuid without execute") << quint32(04600) << false << false
                                                 << QStringLiteral("-rwS------");
}

void SftpRecursiveTest::formatsPermissionsAsSymbolicTextLikeLs()
{
    QFETCH(quint32, permissions);
    QFETCH(bool, isDirectory);
    QFETCH(bool, isSymbolicLink);
    QFETCH(QString, expected);

    RemoteFileInfo entry;
    entry.permissions = permissions;
    entry.isDirectory = isDirectory;
    entry.isSymbolicLink = isSymbolicLink;
    QCOMPARE(entry.permissionText(), expected);
}

void SftpRecursiveTest::joinsRemotePathsWithoutDoubledSeparators()
{
    QCOMPARE(SftpRecursive::joinRemotePath(QStringLiteral("/"), QStringLiteral("etc")),
             QStringLiteral("/etc"));
    QCOMPARE(SftpRecursive::joinRemotePath(QStringLiteral("/srv"), QStringLiteral("app")),
             QStringLiteral("/srv/app"));
    QCOMPARE(SftpRecursive::joinRemotePath(QStringLiteral("/srv/"), QStringLiteral("app")),
             QStringLiteral("/srv/app"));
    // 遍历根目录自身时相对路径为空，拼接结果必须仍是根目录而不是带尾随斜杠的变体。
    QCOMPARE(SftpRecursive::joinRemotePath(QStringLiteral("/srv/app"), QString()),
             QStringLiteral("/srv/app"));
}

void SftpRecursiveTest::normalizesPosixPaths_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("expected");

    QTest::newRow("重复分隔符") << QStringLiteral("/srv//app///bin") << QStringLiteral("/srv/app/bin");
    QTest::newRow("当前目录") << QStringLiteral("/srv/./app") << QStringLiteral("/srv/app");
    QTest::newRow("上级目录") << QStringLiteral("/srv/app/../data") << QStringLiteral("/srv/data");
    QTest::newRow("根目录之上") << QStringLiteral("/../../etc") << QStringLiteral("/etc");
    QTest::newRow("根目录本身") << QStringLiteral("/") << QStringLiteral("/");
    QTest::newRow("尾随斜杠") << QStringLiteral("/srv/app/") << QStringLiteral("/srv/app");
    // 相对路径的前导 ".." 必须原样保留：一旦被悄悄吞掉，"../secret" 就会伪装成 "secret"
    // 通过越界检查，正是这个功能最需要防住的攻击面。
    QTest::newRow("相对上级保留") << QStringLiteral("../secret") << QStringLiteral("../secret");
    QTest::newRow("相对内部抵消") << QStringLiteral("a/../b") << QStringLiteral("b");
    QTest::newRow("相对穿出") << QStringLiteral("a/../../b") << QStringLiteral("../b");
}

void SftpRecursiveTest::normalizesPosixPaths()
{
    QFETCH(QString, input);
    QFETCH(QString, expected);
    QCOMPARE(SftpRecursive::normalizePosixPath(input), expected);
}

void SftpRecursiveTest::rejectsSiblingDirectoryWithSharedPrefix()
{
    QVERIFY(SftpRecursive::isWithinRoot(QStringLiteral("/home/user"),
                                        QStringLiteral("/home/user")));
    QVERIFY(SftpRecursive::isWithinRoot(QStringLiteral("/home/user"),
                                        QStringLiteral("/home/user/docs/a.txt")));
    // 纯字符串前缀比较会把 /home/user-evil 误判成 /home/user 的子路径。
    QVERIFY(!SftpRecursive::isWithinRoot(QStringLiteral("/home/user"),
                                         QStringLiteral("/home/user-evil/a.txt")));
    QVERIFY(!SftpRecursive::isWithinRoot(QStringLiteral("/home/user"),
                                         QStringLiteral("/home/other")));
}

void SftpRecursiveTest::rejectsRelativePathEscapes_data()
{
    QTest::addColumn<QString>("relativePath");
    QTest::addColumn<bool>("accepted");

    QTest::newRow("普通文件") << QStringLiteral("docs/a.txt") << true;
    QTest::newRow("内部抵消") << QStringLiteral("docs/../a.txt") << true;
    QTest::newRow("直接上级") << QStringLiteral("../a.txt") << false;
    QTest::newRow("先入后出") << QStringLiteral("docs/../../a.txt") << false;
    QTest::newRow("深度穿出") << QStringLiteral("a/b/../../../../etc/passwd") << false;
    QTest::newRow("仅上级") << QStringLiteral("..") << false;
}

void SftpRecursiveTest::rejectsRelativePathEscapes()
{
    QFETCH(QString, relativePath);
    QFETCH(bool, accepted);
    const QString resolved = SftpRecursive::resolveWithinRoot(
        QStringLiteral("/home/user/download"), relativePath);
    QCOMPARE(!resolved.isEmpty(), accepted);
    if (accepted) {
        QVERIFY(resolved.startsWith(QStringLiteral("/home/user/download/")));
    }
}

void SftpRecursiveTest::rejectsAbsoluteRelativePath()
{
    QVERIFY(SftpRecursive::resolveWithinRoot(QStringLiteral("/home/user/download"),
                                             QStringLiteral("/etc/passwd"))
                .isEmpty());
}

void SftpRecursiveTest::ordersDirectoriesBeforeTheirContents()
{
    // 刻意打乱顺序输入：真实的 readdir 与 QDirIterator 都不保证父目录先于子项返回。
    const QList<SftpRecursive::SourceEntry> entries{
        makeEntry(QStringLiteral("a/b/deep.txt"), false, 1),
        makeEntry(QStringLiteral("z.txt"), false, 1),
        makeEntry(QStringLiteral("a"), true),
        makeEntry(QStringLiteral("a/b"), true),
        makeEntry(QStringLiteral("a/first.txt"), false, 1),
    };
    const SftpRecursive::TransferPlan plan = SftpRecursive::buildTransferPlan(
        QStringLiteral("/local/src"), QStringLiteral("/remote/dst"), entries);

    const QStringList paths = destinationPaths(plan);
    QCOMPARE(paths.size(), 5);
    const auto positionOf = [&paths](const QString &suffix) {
        return paths.indexOf(QStringLiteral("/remote/dst/") + suffix);
    };
    QVERIFY(positionOf(QStringLiteral("a")) >= 0);
    QVERIFY(positionOf(QStringLiteral("a")) < positionOf(QStringLiteral("a/b")));
    QVERIFY(positionOf(QStringLiteral("a")) < positionOf(QStringLiteral("a/first.txt")));
    QVERIFY(positionOf(QStringLiteral("a/b")) < positionOf(QStringLiteral("a/b/deep.txt")));

    // 源路径必须挂在源根目录上，不能与目标根目录串位。
    QCOMPARE(plan.items.constFirst().sourcePath,
             QStringLiteral("/local/src/") + plan.items.constFirst().destinationPath.section(
                 QStringLiteral("/remote/dst/"), 1));
}

void SftpRecursiveTest::skipsSymbolicLinksWithReason()
{
    const QList<SftpRecursive::SourceEntry> entries{
        makeEntry(QStringLiteral("real.txt"), false, 10),
        makeEntry(QStringLiteral("link-to-etc"), false, 0, true),
        makeEntry(QStringLiteral("loop"), true, 0, true),
    };
    const SftpRecursive::TransferPlan plan = SftpRecursive::buildTransferPlan(
        QStringLiteral("/remote/src"), QStringLiteral("/local/dst"), entries);

    QCOMPARE(plan.items.size(), 1);
    QCOMPARE(plan.items.constFirst().destinationPath, QStringLiteral("/local/dst/real.txt"));
    QCOMPARE(plan.skipped.size(), 2);
    for (const SftpRecursive::SkippedEntry &skipped : plan.skipped) {
        QVERIFY(skipped.reason.contains(QStringLiteral("Symlink")));
    }
    // 指向目录的符号链接同样不得进入计划，否则遍历会在环形链接上无限展开。
    QCOMPARE(plan.directoryCount, 0ULL);
}

void SftpRecursiveTest::skipsEscapingEntriesInsteadOfTransferringThem()
{
    const QList<SftpRecursive::SourceEntry> entries{
        makeEntry(QStringLiteral("ok.txt"), false, 5),
        makeEntry(QStringLiteral("../../etc/passwd"), false, 5),
        makeEntry(QStringLiteral("sub/../../escape.txt"), false, 5),
    };
    const SftpRecursive::TransferPlan plan = SftpRecursive::buildTransferPlan(
        QStringLiteral("/remote/src"), QStringLiteral("/local/dst"), entries);

    QCOMPARE(plan.items.size(), 1);
    QCOMPARE(plan.items.constFirst().destinationPath, QStringLiteral("/local/dst/ok.txt"));
    QCOMPARE(plan.skipped.size(), 2);
    for (const SftpRecursive::SkippedEntry &skipped : plan.skipped) {
        QVERIFY(skipped.reason.contains(QStringLiteral("escapes the target directory")));
    }
    for (const SftpRecursive::TransferItem &item : plan.items) {
        QVERIFY(SftpRecursive::isWithinRoot(QStringLiteral("/local/dst"), item.destinationPath));
    }
}

void SftpRecursiveTest::countsFilesDirectoriesAndTotalBytes()
{
    const QList<SftpRecursive::SourceEntry> entries{
        makeEntry(QStringLiteral("dir"), true, 4096),
        makeEntry(QStringLiteral("dir/a.bin"), false, 1024),
        makeEntry(QStringLiteral("dir/b.bin"), false, 2048),
        makeEntry(QStringLiteral("link"), false, 999999, true),
    };
    const SftpRecursive::TransferPlan plan = SftpRecursive::buildTransferPlan(
        QStringLiteral("/src"), QStringLiteral("/dst"), entries);

    QCOMPARE(plan.fileCount, 2ULL);
    QCOMPARE(plan.directoryCount, 1ULL);
    // 目录自身的 st_size 不是要传输的字节，跳过的链接也不能计入，否则进度条永远到不了 100%。
    QCOMPARE(plan.totalBytes, 3072ULL);
}

void SftpRecursiveTest::computesProgressPercent_data()
{
    QTest::addColumn<quint64>("completed");
    QTest::addColumn<quint64>("total");
    QTest::addColumn<int>("expected");

    QTest::newRow("起点") << 0ULL << 200ULL << 0;
    QTest::newRow("过半") << 101ULL << 200ULL << 50;
    QTest::newRow("完成") << 200ULL << 200ULL << 100;
    // 服务器给出的大小可能小于实际读到的字节数，进度条不得越界。
    QTest::newRow("超出限幅") << 400ULL << 200ULL << 100;
    QTest::newRow("总量未知") << 10ULL << 0ULL << -1;
}

void SftpRecursiveTest::computesProgressPercent()
{
    QFETCH(quint64, completed);
    QFETCH(quint64, total);
    QFETCH(int, expected);
    QCOMPARE(SftpRecursive::progressPercent(completed, total), expected);
}

void SftpRecursiveTest::aggregatesSkippedAndFailedEntriesIntoMessage()
{
    SftpRecursive::TransferSummary summary;
    summary.rootDescription = QStringLiteral("/local/dst/project");
    summary.filesTransferred = 8;
    summary.fileTotal = 10;
    summary.bytesTransferred = 4096;
    summary.skipped.append({QStringLiteral("logs/current"), QStringLiteral("Symlink (not followed)")});
    summary.failed.append({QStringLiteral("secret.key"), QStringLiteral("No permission to read the file")});
    summary.failed.append({QStringLiteral("big.bin"), QStringLiteral("connection dropped")});

    QVERIFY(summary.hasWarnings());
    const QString message = summary.toMessage();
    // 成功计数与两份清单必须同时出现：只报成功会让用户以为整个目录都传完了。
    QVERIFY(message.contains(QStringLiteral("8")));
    QVERIFY(message.contains(QStringLiteral("10")));
    QVERIFY(message.contains(QStringLiteral("logs/current")));
    QVERIFY(message.contains(QStringLiteral("secret.key")));
    QVERIFY(message.contains(QStringLiteral("No permission")));
    QVERIFY(message.contains(QStringLiteral("big.bin")));
    QVERIFY(message.contains(QStringLiteral("connection dropped")));

    SftpRecursive::TransferSummary clean;
    clean.rootDescription = QStringLiteral("/local/dst/project");
    clean.filesTransferred = 10;
    clean.fileTotal = 10;
    QVERIFY(!clean.hasWarnings());
}

void SftpRecursiveTest::truncatesOverlongFailureList()
{
    SftpRecursive::TransferSummary summary;
    summary.rootDescription = QStringLiteral("/local/dst/project");
    for (int index = 0; index < 25; ++index) {
        summary.failed.append({QStringLiteral("file-%1").arg(index), QStringLiteral("权限不足")});
    }
    const QString message = summary.toMessage();
    QVERIFY(message.contains(QStringLiteral("25")));
    QVERIFY(message.contains(QStringLiteral("file-0")));
    // 只展示前若干条并说明省略数量；整份清单原样倒出会淹没真正要看的第一条原因。
    QVERIFY(!message.contains(QStringLiteral("file-24")));
    QVERIFY(message.contains(QStringLiteral("more omitted")));
}

void SftpRecursiveTest::reportsCancellationDistinctlyFromSuccess()
{
    SftpRecursive::TransferSummary summary;
    summary.rootDescription = QStringLiteral("/local/dst/project");
    summary.filesTransferred = 3;
    summary.fileTotal = 100;
    summary.cancelled = true;

    QVERIFY(summary.hasWarnings());
    const QString message = summary.toMessage();
    QVERIFY(message.contains(QStringLiteral("cancel")));
    QVERIFY(!message.contains(QStringLiteral("Transferred")));
}

void SftpRecursiveTest::preservesManualServerSelectionAcrossRefresh()
{
    const QStringList threeLayers{
        QStringLiteral("outer"), QStringLiteral("middle"), QStringLiteral("inner")};

    QCOMPARE(SftpUiState::selectTargetIndex(threeLayers,
                                            QStringLiteral("middle"),
                                            1,
                                            false),
             1);
    // 开启自动跟随后，新进入一层必须立即选择最内层，与原手工选择无关。
    QCOMPARE(SftpUiState::selectTargetIndex(threeLayers,
                                            QStringLiteral("outer"),
                                            0,
                                            true),
             2);

    const QStringList afterInnerExit{QStringLiteral("outer"), QStringLiteral("middle")};
    // 原层已经退出时回落到最接近的有效层，不能留下 -1 导致整个文件管理器不可操作。
    QCOMPARE(SftpUiState::selectTargetIndex(afterInnerExit,
                                            QStringLiteral("inner"),
                                            2,
                                            false),
             1);
    QCOMPARE(SftpUiState::selectTargetIndex({}, QString(), -1, false), -1);
}

void SftpRecursiveTest::cyclesColumnSortThroughAscendingDescendingDefault()
{
    using SftpUiState::nextSortState;
    using SftpUiState::SortState;

    // 默认态点击"大小"列（下标 1）：进入升序。
    const SortState ascending = nextSortState(SortState{}, 1);
    QCOMPARE(ascending.column, 1);
    QCOMPARE(ascending.order, Qt::AscendingOrder);

    // 再次点击同一列：转为降序。
    const SortState descending = nextSortState(ascending, 1);
    QCOMPARE(descending.column, 1);
    QCOMPARE(descending.order, Qt::DescendingOrder);

    // 第三次点击必须回到"还原默认"，即不排序、保持服务器返回顺序。用 column < 0 表达，
    // 不能退化成"按第 0 列升序"——那会把用户的三态循环变成两态。
    const SortState restored = nextSortState(descending, 1);
    QCOMPARE(restored.column, -1);
    QVERIFY(restored == SortState{});

    // 循环必须闭合：从默认态再点同一列应重新进入升序。
    QVERIFY(nextSortState(restored, 1) == ascending);

    // 中途改点其他列时，新列从升序开始，而不是继承上一列的降序方向。
    const SortState otherColumn = nextSortState(descending, 2);
    QCOMPARE(otherColumn.column, 2);
    QCOMPARE(otherColumn.order, Qt::AscendingOrder);
}

void SftpRecursiveTest::decidesWhenToFollowTerminalWorkingDirectory_data()
{
    QTest::addColumn<bool>("followEnabled");
    QTest::addColumn<QString>("eventStableKey");
    QTest::addColumn<QString>("selectedStableKey");
    QTest::addColumn<QString>("workingDirectory");
    QTest::addColumn<bool>("expected");

    QTest::newRow("开关开且同一服务器")
        << true << QStringLiteral("host-a") << QStringLiteral("host-a")
        << QStringLiteral("/srv/app") << true;
    QTest::newRow("开关关闭")
        << false << QStringLiteral("host-a") << QStringLiteral("host-a")
        << QStringLiteral("/srv/app") << false;
    // 用户正浏览外层服务器时，内层终端的 cwd 上报不该把路径框跳到另一台机器。
    QTest::newRow("上报服务器与当前选中不一致")
        << true << QStringLiteral("host-inner") << QStringLiteral("host-outer")
        << QStringLiteral("/srv/app") << false;
    QTest::newRow("路径为空")
        << true << QStringLiteral("host-a") << QStringLiteral("host-a")
        << QString() << false;
    QTest::newRow("尚未选中任何服务器")
        << true << QStringLiteral("host-a") << QString()
        << QStringLiteral("/srv/app") << false;
}

void SftpRecursiveTest::decidesWhenToFollowTerminalWorkingDirectory()
{
    QFETCH(bool, followEnabled);
    QFETCH(QString, eventStableKey);
    QFETCH(QString, selectedStableKey);
    QFETCH(QString, workingDirectory);
    QFETCH(bool, expected);

    QCOMPARE(SftpUiState::shouldFollowTerminalWorkingDirectory(
                 followEnabled, eventStableKey, selectedStableKey, workingDirectory),
             expected);
}

QTEST_MAIN(SftpRecursiveTest)
#include "SftpRecursiveTest.moc"
