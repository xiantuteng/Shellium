#include "sftp/RemoteFileManagerWidget.h"
#include "ssh/SshTunnelBroker.h"

#include <QCheckBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFocusEvent>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QProgressBar>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTranslator>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>

/**
 * @brief 为远程文件管理界面提供不访问网络的隧道 Broker 测试替身。
 *
 * RemoteFileManagerWidget 会构造正式 SftpService，而服务实现为了支持跳板机会链接
 * SshTunnelBroker。界面结构测试既不会设置服务器，也不会发起 SFTP 请求，因此不应为了
 * 构造一个控件而拉入完整终端会话栈。以下成员实现仅存在于独立测试可执行文件中：若未来
 * 某个用例意外触发网络路径，openSocket 会明确返回失败，而不是连接真实服务器。
 */
SshTunnelBroker &SshTunnelBroker::instance()
{
    static SshTunnelBroker broker;
    return broker;
}

/**
 * @brief 测试替身不保存终端会话路由。
 * @param routeId 正式实现使用的路由标识；界面测试不读取。
 * @param stack 正式实现使用的终端会话栈；界面测试不持有。
 * @param layerId 正式实现使用的目标层标识；界面测试不读取。
 * @param target 正式实现使用的目标服务器；界面测试不读取。
 * @param relayOrigin 正式实现远端 OpenSSH 兜底使用的外层服务器；界面测试不读取。
 */
void SshTunnelBroker::registerRoute(const QString &routeId,
                                    SshSessionStack *stack,
                                    const QString &layerId,
                                    const ConnectionTarget &target,
                                    const ConnectionTarget &relayOrigin)
{
    Q_UNUSED(routeId)
    Q_UNUSED(stack)
    Q_UNUSED(layerId)
    Q_UNUSED(target)
    Q_UNUSED(relayOrigin)
}

/**
 * @brief 测试替身无需移除路由，因为 registerRoute 从未保存任何状态。
 * @param routeId 待移除的正式路由标识；界面测试不读取。
 */
void SshTunnelBroker::unregisterRoute(const QString &routeId)
{
    Q_UNUSED(routeId)
}

/**
 * @brief 阻止界面测试意外建立真实网络连接。
 * @param routeId 请求的隧道路由标识。
 * @param errorMessage 非空时写入可诊断的失败原因。
 * @return 始终返回 InvalidNativeSocket，表示测试环境不提供 socket。
 */
NativeSocketHandle SshTunnelBroker::openSocket(const QString &routeId, QString *errorMessage)
{
    Q_UNUSED(routeId)
    if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("SSH tunnels are not enabled in UI tests");
    }
    return InvalidNativeSocket;
}

/**
 * @brief 界面测试不提供远端 OpenSSH SFTP 中继配置。
 * @param routeId 正式路由标识；测试不读取。
 * @param relayOrigin 正式实现的外层配置输出；测试不写入。
 * @param target 正式实现的内层配置输出；测试不写入。
 * @return 始终返回 false，保证界面结构测试不会连接真实服务器。
 */
bool SshTunnelBroker::remoteSftpRelay(const QString &routeId,
                                      ConnectionTarget *relayOrigin,
                                      ConnectionTarget *target)
{
    Q_UNUSED(routeId)
    Q_UNUSED(relayOrigin)
    Q_UNUSED(target)
    return false;
}

/**
 * @brief 远程文件管理改版的结构与响应式界面回归。
 *
 * 这些测试只检查设计稿要求且会直接影响可用性的稳定契约：默认远程单栏、全屏双栏、两侧
 * 导航顺序、远程四列，以及窄栏时动作退化为图标。颜色和像素级细节由 QSS 负责，不在这里
 * 比较截图，避免系统字体或缩放比例变化造成无意义失败。
 */
class RemoteFileManagerWidgetTest final : public QObject {
    Q_OBJECT

private slots:
    /** @brief 默认保持远程单栏，全屏后展示结构对称的本地/远程双栏。 */
    void fullScreenSwitchesFromRemoteOnlyToSymmetricDualPane();

    /** @brief 远程表格必须只保留设计稿中的名称、大小、权限、所有者四列。 */
    void remoteFileTableUsesReferenceColumns();

    /** @brief 本地栏须展示真实的权限与所有者列，且点击列头可按该列实际排序。 */
    void localFileTableListsPermissionsOwnerAndSortsByClickedColumn();

    /** @brief 本地栏不再有 QFileSystemModel 惰性懒加载兜底，构造完成时必须已列出主目录。 */
    void localFileTreePopulatesHomeDirectoryImmediatelyOnConstruction();

    /** @brief 合法拖拽悬停在文件栏上方时必须显示放置目标框，离开或落下后必须撤销。 */
    void fileTreesShowDropTargetHighlightWhileDraggingAndClearItAfterward();

    /** @brief 拖放必须一律落到当前浏览目录，不能按鼠标悬停的具体条目重新定向。 */
    void droppingOntoFileTreeAlwaysTargetsCurrentDirectoryNotHoveredItem();

    /** @brief 本地/远程路径框获得焦点时必须自动全选已有文字，方便整段替换。 */
    void pathEditsSelectAllExistingTextWhenFocused();

    /** @brief 宽窗口展示动作文字，窄窗口保留语义和提示但改用紧凑图标。 */
    void actionBarRespondsToAvailableWidth();

    /** @brief 进度、状态和队列必须归入同一底部信息带，并在空闲时整体隐藏。 */
    void transferWidgetsShareOneHiddenInformationBand();

    /** @brief 远端 SFTP 中继命令必须保留端口和用户名，并阻断 Shell 参数注入。 */
    void remoteSftpRelayCommandQuotesEveryUntrustedArgument();

    /** @brief 勾选时立即应用已知 cwd，之后也只跟随当前选中服务器的新上报。 */
    void followsTerminalWorkingDirectoryOnlyWhenEnabledAndMatchingTarget();

    /** @brief 面板变窄时"名称"列不能被压缩到低于最小宽度，多余内容改用横向滚动条。 */
    void remoteNameColumnNeverShrinksBelowMinimumWidth();

    /** @brief 顶栏全屏按钮右侧必须有关闭按钮，点击只上报请求、不自行隐藏。 */
    void closeButtonSitsNextToFullScreenAndOnlyRequestsClose();

    /** @brief 全屏与关闭按钮必须接入远程文件管理专用的加深悬停样式。 */
    void topBarActionsUseRemoteFileHoverStyle();

    /**
     * @brief 一次真实上传成功结束后必须发出 transferResultAvailable(true, ...)。
     *
     * uploadFilesToDirectory 是给终端拖拽等面板外部入口用的公开入口，直接传入 target
     * 而不依赖面板自身选中状态；连接失败场景不适合在这里用不存在的服务器模拟——本沙箱
     * 环境里"连接被拒绝"并不会像真实机器那样快速返回 RST，用不可达目标会让用例挂起
     * 数分钟。真正连到 AGENT.md 记录的集成测试服务器上传一个真实文件，既避开了这个环境
     * 差异，也顺带验证了 uploadFilesToDirectory 这条独立路径本身可用。缺少
     * SHELLIUM_IT_* 环境变量时自动跳过。
     */
    void queuedUploadEmitsSuccessfulTransferResult();

    /**
     * @brief 终端拖入的上传必须经 terminalDropUploadProgress 报告"开始 → 百分比 → 结束"。
     *
     * 终端标题栏的进度完全依赖这条信号；漏发"结束"会让进度区永远挂在标题栏上。连接真实
     * 集成测试服务器，缺少 SHELLIUM_IT_* 环境变量时自动跳过，理由同上一个用例。
     */
    void terminalDropUploadReportsProgress();

    /**
     * @brief 语言切换时上传菜单里的 QAction 文案必须一起重译。
     *
     * 这两个动作挂在常驻 QMenu 上、只在构造期设过一次文案，而命令面板通过
     * findChildren<QAction*> 收录它们。曾经 onRetranslate 闭包漏掉了这两项，切到新语言
     * 后命令面板里会残留上一次语言的“上传文件/上传目录”。
     */
    void uploadMenuActionsRetranslateOnLanguageChange();

private:
    /**
     * @brief 断言指定工具栏的直接布局顺序与设计稿一致。
     * @param toolbarObjectName 待检查工具栏 objectName。
     * @param expectedObjectNames 由左至右期望的控件 objectName。
     */
    static void compareToolbarOrder(RemoteFileManagerWidget *widget,
                                    const QString &toolbarObjectName,
                                    const QStringList &expectedObjectNames);
};

void RemoteFileManagerWidgetTest::compareToolbarOrder(
    RemoteFileManagerWidget *widget,
    const QString &toolbarObjectName,
    const QStringList &expectedObjectNames)
{
    QWidget *toolbar = widget->findChild<QWidget *>(toolbarObjectName);
    QVERIFY2(toolbar != nullptr, qPrintable(QStringLiteral("未找到工具栏：%1").arg(toolbarObjectName)));
    auto *layout = qobject_cast<QHBoxLayout *>(toolbar->layout());
    QVERIFY2(layout != nullptr, "文件栏导航必须使用单行水平布局");
    QCOMPARE(layout->count(), expectedObjectNames.size());

    for (qsizetype index = 0; index < expectedObjectNames.size(); ++index) {
        QWidget *child = layout->itemAt(int(index))->widget();
        QVERIFY2(child != nullptr, "导航行中不应夹入无法识别的空布局项");
        QCOMPARE(child->objectName(), expectedObjectNames.at(index));
    }
}

void RemoteFileManagerWidgetTest::fullScreenSwitchesFromRemoteOnlyToSymmetricDualPane()
{
    RemoteFileManagerWidget widget;
    widget.resize(960, 680);
    // 本用例只关心显式 show/hide 状态和布局结构。CI 可能没有可用屏幕，因此不调用 show；
    // 响应式 resizeEvent 由下方独立用例通过显式事件覆盖，避免这里引入桌面会话依赖。
    QCoreApplication::processEvents();

    QWidget *localPane = widget.findChild<QWidget *>(QStringLiteral("SftpLocalPane"));
    QWidget *remotePane = widget.findChild<QWidget *>(QStringLiteral("SftpRemotePane"));
    auto *fullScreenButton =
        widget.findChild<QToolButton *>(QStringLiteral("SftpFullScreenButton"));
    QVERIFY(localPane != nullptr);
    QVERIFY(remotePane != nullptr);
    QVERIFY(fullScreenButton != nullptr);
    QVERIFY2(localPane->isHidden(), "默认布局只能显示远程栏，避免与终端并排时内容过窄");
    QVERIFY2(!remotePane->isHidden(), "默认布局必须保留远程文件栏");

    QSignalSpy fullScreenChangedSpy(&widget, &RemoteFileManagerWidget::fullScreenChanged);
    fullScreenButton->click();
    QCoreApplication::processEvents();

    QCOMPARE(fullScreenChangedSpy.count(), 1);
    QCOMPARE(fullScreenChangedSpy.takeFirst().at(0).toBool(), true);
    QVERIFY2(!localPane->isHidden(), "全屏后必须展开本地文件栏形成双栏对拷");
    QVERIFY2(!remotePane->isHidden(), "展开本地栏时不能遮蔽或移除远程栏");

    compareToolbarOrder(
        &widget,
        QStringLiteral("SftpLocalToolbar"),
        {QStringLiteral("SftpPaneLabel"),
         QStringLiteral("SftpLocalUpButton"),
         QStringLiteral("SftpLocalRefreshButton"),
         QStringLiteral("SftpLocalPathEdit")});
    compareToolbarOrder(
        &widget,
        QStringLiteral("SftpRemoteToolbar"),
        {QStringLiteral("SftpPaneLabel"),
         QStringLiteral("SftpRemoteUpButton"),
         QStringLiteral("SftpRemoteRefreshButton"),
         QStringLiteral("SftpRemotePathEdit"),
         QStringLiteral("SftpFollowTerminalCwdCheck")});

    auto *localTree = widget.findChild<QTreeWidget *>(QStringLiteral("LocalFileTree"));
    QVERIFY(localTree != nullptr);
    QCOMPARE(localTree->columnCount(), 5);
    for (int column = 0; column < localTree->columnCount(); ++column) {
        QVERIFY2(!localTree->isColumnHidden(column),
                 "本地栏列布局须与远程栏对齐，不再隐藏任何一列");
    }
    QCOMPARE(localTree->headerItem()->text(0), QStringLiteral("Name"));
    QCOMPARE(localTree->headerItem()->text(1), QStringLiteral("Size"));
    QCOMPARE(localTree->headerItem()->text(2), QStringLiteral("Modified"));
    QCOMPARE(localTree->headerItem()->text(3), QStringLiteral("Permissions"));
    QCOMPARE(localTree->headerItem()->text(4), QStringLiteral("Owner"));
}

void RemoteFileManagerWidgetTest::remoteSftpRelayCommandQuotesEveryUntrustedArgument()
{
    ConnectionTarget target;
    target.host = QStringLiteral("192.168.1.27;touch /tmp/should-not-run");
    target.username = QStringLiteral("root'operator");
    target.port = 52231;

    // 分号和单引号刻意模拟不可信终端输入。期望值中的 `'\\''` 会让 POSIX Shell 结束
    // 单引号、写入一个字面单引号后重新进入单引号；因此整个值始终只形成一个 argv。
    QCOMPARE(
        SftpRemoteRelay::buildCommand(target),
        QByteArrayLiteral(
            "command ssh -q -o BatchMode=yes -o ConnectTimeout=12 -p 52231 "
            "-l 'root'\\''operator' "
            "-s -- '192.168.1.27;touch /tmp/should-not-run' sftp"));

    // 用户没有显式用户名时不应生成空的 -l 参数，交由外层 OpenSSH 按登录用户/config 决定。
    target.username.clear();
    QCOMPARE(
        SftpRemoteRelay::buildCommand(target),
        QByteArrayLiteral(
            "command ssh -q -o BatchMode=yes -o ConnectTimeout=12 -p 52231 "
            "-s -- '192.168.1.27;touch /tmp/should-not-run' sftp"));
}

void RemoteFileManagerWidgetTest::followsTerminalWorkingDirectoryOnlyWhenEnabledAndMatchingTarget()
{
    RemoteFileManagerWidget widget;

    ConnectionTarget target;
    target.id = QStringLiteral("layer-1");
    target.displayName = QStringLiteral("test-host");
    // 127.0.0.1 上几乎不会有服务监听 1 号端口：真正触发的后台 SFTP 连接会立即被拒绝，
    // 不会挂起或打到外部网络，测试因此可以直接检查同步完成的路径框状态。
    target.host = QStringLiteral("127.0.0.1");
    target.port = 1;
    target.username = QStringLiteral("tester");

    widget.setAvailableTargets({target});

    auto *pathEdit = widget.findChild<QLineEdit *>(QStringLiteral("SftpRemotePathEdit"));
    auto *followCheck =
        widget.findChild<QCheckBox *>(QStringLiteral("SftpFollowTerminalCwdCheck"));
    QVERIFY(pathEdit != nullptr);
    QVERIFY(followCheck != nullptr);
    QVERIFY2(!followCheck->isChecked(), "跟随终端目录默认必须关闭，避免存量用户被突然改变行为");

    const QString stableKey = target.stableKey();
    pathEdit->setText(QStringLiteral("/initial"));

    // 开关关闭时，即使上报服务器与当前选中一致也不能立即跳转，但必须记住这次 cwd，
    // 因为它就是用户稍后勾选时所期望的“当前终端目录”。
    widget.followTerminalWorkingDirectory(stableKey, QStringLiteral("/srv/app"));
    QCOMPARE(pathEdit->text(), QStringLiteral("/initial"));

    followCheck->setChecked(true);
    QCOMPARE(pathEdit->text(), QStringLiteral("/srv/app"));

    // 上报服务器不是当前选中的服务器时不能跳转，否则用户正在看的目录会被意外顶掉。
    widget.followTerminalWorkingDirectory(QStringLiteral("other-layer"),
                                          QStringLiteral("/srv/other"));
    QCOMPARE(pathEdit->text(), QStringLiteral("/srv/app"));

    // 开关开启且服务器匹配：后续目录变化仍须实时跳转，不能只在勾选瞬间同步一次。
    widget.followTerminalWorkingDirectory(stableKey, QStringLiteral("/srv/next"));
    QCOMPARE(pathEdit->text(), QStringLiteral("/srv/next"));
}

void RemoteFileManagerWidgetTest::remoteFileTableUsesReferenceColumns()
{
    RemoteFileManagerWidget widget;
    auto *remoteTree = widget.findChild<QTreeWidget *>(QStringLiteral("RemoteFileTree"));
    QVERIFY(remoteTree != nullptr);
    QCOMPARE(remoteTree->columnCount(), 5);
    QCOMPARE(remoteTree->headerItem()->text(0), QStringLiteral("Name"));
    QCOMPARE(remoteTree->headerItem()->text(1), QStringLiteral("Size"));
    QCOMPARE(remoteTree->headerItem()->text(2), QStringLiteral("Modified"));
    QCOMPARE(remoteTree->headerItem()->text(3), QStringLiteral("Permissions"));
    QCOMPARE(remoteTree->headerItem()->text(4), QStringLiteral("Owner"));
    // 名称列宽度由 updateRemoteNameColumnWidth() 手动管理（保底 + 横向滚动），不再用
    // Qt 内置的 Stretch——Stretch 会在面板变窄时把它一路压到 setMinimumSectionSize 的下限。
    QCOMPARE(remoteTree->header()->sectionResizeMode(0), QHeaderView::Interactive);
}

void RemoteFileManagerWidgetTest::localFileTableListsPermissionsOwnerAndSortsByClickedColumn()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    // 刻意让文件名的字母序与文件大小的顺序相反：默认列表按名称排列，只有真正按"大小"
    // 排序时顺序才会翻转，否则退化成的名称排序也能碰巧通过测试。
    QFile smallFile(tempDir.filePath(QStringLiteral("a_small.txt")));
    QVERIFY(smallFile.open(QIODevice::WriteOnly));
    smallFile.write(QByteArray(5, 'x'));
    smallFile.close();

    QFile bigFile(tempDir.filePath(QStringLiteral("z_big.txt")));
    QVERIFY(bigFile.open(QIODevice::WriteOnly));
    bigFile.write(QByteArray(500, 'y'));
    bigFile.close();

    RemoteFileManagerWidget widget;
    auto *pathEdit = widget.findChild<QLineEdit *>(QStringLiteral("SftpLocalPathEdit"));
    auto *localTree = widget.findChild<QTreeWidget *>(QStringLiteral("LocalFileTree"));
    QVERIFY(pathEdit != nullptr);
    QVERIFY(localTree != nullptr);

    pathEdit->setText(tempDir.path());
    emit pathEdit->returnPressed();

    QCOMPARE(localTree->topLevelItemCount(), 2);
    // 默认（未排序）状态沿用目录枚举顺序，即按名称升序。
    QCOMPARE(localTree->topLevelItem(0)->text(0), QStringLiteral("a_small.txt"));
    QCOMPARE(localTree->topLevelItem(1)->text(0), QStringLiteral("z_big.txt"));
    QVERIFY2(QRegularExpression(QStringLiteral("^[-dl][-rwxsStT]{9}$"))
                 .match(localTree->topLevelItem(0)->text(3))
                 .hasMatch(),
             qPrintable(QStringLiteral("权限列必须是 ls -l 风格的符号，实际为 %1")
                            .arg(localTree->topLevelItem(0)->text(3))));
    QCOMPARE(localTree->headerItem()->text(4), QStringLiteral("Owner"));

    QHeaderView *header = localTree->header();
    QVERIFY(header->sectionsClickable());

    // 点击"大小"列头一次：升序，最小的文件排在最前——与按名称排序的结果一致，尚不能
    // 单独证明确实比较了大小而非名称，因此还需要第二次点击。
    emit header->sectionClicked(1);
    QCOMPARE(localTree->topLevelItem(0)->text(0), QStringLiteral("a_small.txt"));
    QCOMPARE(localTree->topLevelItem(1)->text(0), QStringLiteral("z_big.txt"));

    // 再次点击同一列头切到降序：只有真的按字节数比较，最大的文件才会排到最前；
    // 若退化成按名称排序，此时顺序不会变化，测试会在这里失败。
    emit header->sectionClicked(1);
    QCOMPARE(localTree->topLevelItem(0)->text(0), QStringLiteral("z_big.txt"));
    QCOMPARE(localTree->topLevelItem(1)->text(0), QStringLiteral("a_small.txt"));

    // 第三次点击回到"还原默认"，即重新按名称升序排列。
    emit header->sectionClicked(1);
    QCOMPARE(localTree->topLevelItem(0)->text(0), QStringLiteral("a_small.txt"));
    QCOMPARE(localTree->topLevelItem(1)->text(0), QStringLiteral("z_big.txt"));
}

void RemoteFileManagerWidgetTest::localFileTreePopulatesHomeDirectoryImmediatelyOnConstruction()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    QFile probeFile(homeDir.filePath(QStringLiteral("probe.txt")));
    QVERIFY(probeFile.open(QIODevice::WriteOnly));
    probeFile.write(QByteArrayLiteral("x"));
    probeFile.close();

    // 本地栏构造时用 QDir::homePath() 作为初始路径，因此临时接管 HOME 环境变量，让
    // 断言不依赖运行测试的真实账户主目录里到底有什么文件。
    const QByteArray originalHome = qgetenv("HOME");
    qputenv("HOME", homeDir.path().toUtf8());

    RemoteFileManagerWidget widget;

    if (originalHome.isNull()) {
        qunsetenv("HOME");
    } else {
        qputenv("HOME", originalHome);
    }

    auto *pathEdit = widget.findChild<QLineEdit *>(QStringLiteral("SftpLocalPathEdit"));
    auto *localTree = widget.findChild<QTreeWidget *>(QStringLiteral("LocalFileTree"));
    QVERIFY(pathEdit != nullptr);
    QVERIFY(localTree != nullptr);

    // 本地栏不再挂 QFileSystemModel 的按需加载，构造函数必须主动列一次主目录；否则用户
    // 第一次展开本地面板时会看到空列表，得自己敲一次回车或点刷新才会加载出内容。
    QCOMPARE(pathEdit->text(), QDir(homeDir.path()).absolutePath());
    QCOMPARE(localTree->topLevelItemCount(), 1);
    QCOMPARE(localTree->topLevelItem(0)->text(0), QStringLiteral("probe.txt"));
}

void RemoteFileManagerWidgetTest::fileTreesShowDropTargetHighlightWhileDraggingAndClearItAfterward()
{
    // 与 RemoteFileManagerWidget.cpp 匿名命名空间里的 kRemotePathMimeType 保持一致；
    // 该常量不对外暴露，测试只能按值复制一份。
    constexpr char kRemotePathMimeType[] = "application/x-shellium-remote-path";

    RemoteFileManagerWidget widget;
    auto *remoteTree = widget.findChild<QTreeWidget *>(QStringLiteral("RemoteFileTree"));
    auto *localTree = widget.findChild<QTreeWidget *>(QStringLiteral("LocalFileTree"));
    QVERIFY(remoteTree != nullptr);
    QVERIFY(localTree != nullptr);
    QVERIFY2(!remoteTree->property("dragActive").toBool(), "远程栏初始状态不应显示放置目标框");
    QVERIFY2(!localTree->property("dragActive").toBool(), "本地栏初始状态不应显示放置目标框");

    // 远程栏接收操作系统/本地栏拖来的本地文件 URL。
    QMimeData localFileMimeData;
    localFileMimeData.setUrls({QUrl::fromLocalFile(QDir::tempPath())});
    QDragEnterEvent remoteEnter(QPoint(5, 5), Qt::CopyAction, &localFileMimeData,
                                Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(remoteTree->viewport(), &remoteEnter);
    QVERIFY2(remoteTree->property("dragActive").toBool(),
             "本地文件拖入远程栏时必须显示放置目标框");

    QDragLeaveEvent remoteLeave;
    QCoreApplication::sendEvent(remoteTree->viewport(), &remoteLeave);
    QVERIFY2(!remoteTree->property("dragActive").toBool(),
             "拖拽离开远程栏而未落下时必须撤销放置目标框");

    // 本地栏接收远程栏拖来的自定义远程条目格式。
    QMimeData remoteEntryMimeData;
    remoteEntryMimeData.setData(kRemotePathMimeType, QByteArrayLiteral("/root/a.txt"));
    QDragEnterEvent localEnter(QPoint(5, 5), Qt::CopyAction, &remoteEntryMimeData,
                               Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(localTree->viewport(), &localEnter);
    QVERIFY2(localTree->property("dragActive").toBool(),
             "远程条目拖入本地栏时必须显示放置目标框");

    QDropEvent localDrop(QPoint(5, 5), Qt::CopyAction, &remoteEntryMimeData,
                         Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(localTree->viewport(), &localDrop);
    QVERIFY2(!localTree->property("dragActive").toBool(),
             "落下后必须立即撤销放置目标框，不能停留到下一次拖拽");
}

void RemoteFileManagerWidgetTest::droppingOntoFileTreeAlwaysTargetsCurrentDirectoryNotHoveredItem()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QFile payload(tempDir.filePath(QStringLiteral("payload.txt")));
    QVERIFY(payload.open(QIODevice::WriteOnly));
    payload.write(QByteArrayLiteral("x"));
    payload.close();

    ConnectionTarget target;
    target.id = QStringLiteral("layer-1");
    target.displayName = QStringLiteral("test-host");
    // 与本文件其它用例一致：127.0.0.1 上几乎不会有服务监听 1 号端口，真正的连接尝试会
    // 立即失败，不影响下面同步完成的入队断言。
    target.host = QStringLiteral("127.0.0.1");
    target.port = 1;
    target.username = QStringLiteral("tester");

    RemoteFileManagerWidget widget;
    widget.setAvailableTargets({target});

    auto *pathEdit = widget.findChild<QLineEdit *>(QStringLiteral("SftpRemotePathEdit"));
    auto *remoteTree = widget.findChild<QTreeWidget *>(QStringLiteral("RemoteFileTree"));
    auto *queueList = widget.findChild<QListWidget *>(QStringLiteral("SftpTransferQueue"));
    QVERIFY(pathEdit != nullptr);
    QVERIFY(remoteTree != nullptr);
    QVERIFY(queueList != nullptr);

    pathEdit->setText(QStringLiteral("/current"));

    // 手工塞一个"另一个目录"条目，模拟拖拽时鼠标恰好悬停在别的子目录行上——旧实现会把
    // 上传目标错误地重定向到这个悬停条目，而不是地址框显示的当前目录。
    auto *decoyItem = new QTreeWidgetItem(remoteTree);
    decoyItem->setText(0, QStringLiteral("decoy"));
    decoyItem->setData(0, Qt::UserRole, QStringLiteral("/current/decoy"));
    decoyItem->setData(0, Qt::UserRole + 1, true);
    const QRect decoyRect = remoteTree->visualItemRect(decoyItem);
    QVERIFY2(!decoyRect.isEmpty(), "测试条目必须先出现在可见视口内才能被拖放命中");

    QMimeData mimeData;
    mimeData.setUrls({QUrl::fromLocalFile(payload.fileName())});
    // QAbstractItemView 的拖放状态机要求先有 DragEnter 才会在 viewportEvent 里转发
    // 随后的 Drop；直接发 Drop 会被内部状态判定为"不在拖拽中"而静默忽略。
    QDragEnterEvent dragEnter(decoyRect.center(), Qt::CopyAction, &mimeData,
                              Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(remoteTree->viewport(), &dragEnter);

    QDropEvent dropEvent(decoyRect.center(), Qt::CopyAction, &mimeData,
                         Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(remoteTree->viewport(), &dropEvent);

    QCOMPARE(queueList->count(), 1);
    auto *nameLabel = queueList->findChild<QLabel *>(QStringLiteral("SftpQueueItemName"));
    QVERIFY(nameLabel != nullptr);
    QCOMPARE(nameLabel->text(), QStringLiteral("Upload payload.txt → /current"));
}

void RemoteFileManagerWidgetTest::queuedUploadEmitsSuccessfulTransferResult()
{
    const QByteArray host = qgetenv("SHELLIUM_IT_HOST");
    const QByteArray user = qgetenv("SHELLIUM_IT_USER");
    const QByteArray password = qgetenv("SHELLIUM_IT_PASSWORD");
    if (host.isEmpty() || user.isEmpty() || password.isEmpty()) {
        QSKIP("未设置 SHELLIUM_IT_HOST / SHELLIUM_IT_USER / SHELLIUM_IT_PASSWORD，跳过真实上传结果回归");
    }

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QFile payload(tempDir.filePath(QStringLiteral("shellium-transfer-result-test.txt")));
    QVERIFY(payload.open(QIODevice::WriteOnly));
    payload.write(QByteArrayLiteral("shellium drop-upload regression"));
    payload.close();

    ConnectionTarget target;
    target.id = QStringLiteral("integration-layer-1");
    target.host = QString::fromUtf8(host);
    target.username = QString::fromUtf8(user);
    target.password = QString::fromUtf8(password);
    const uint port = qgetenv("SHELLIUM_IT_PORT").toUInt();
    target.port = port == 0 ? 22 : static_cast<quint16>(port);

    RemoteFileManagerWidget widget;
    // 刻意不调用 setAvailableTargets：uploadFilesToDirectory 本就不依赖面板选中状态，
    // 这里直接验证这条独立路径——正是终端拖拽入口实际调用它的方式。
    QSignalSpy resultSpy(&widget, &RemoteFileManagerWidget::transferResultAvailable);
    widget.uploadFilesToDirectory(target, QStringLiteral("/tmp"), {payload.fileName()});

    QVERIFY2(resultSpy.wait(30000), "真实上传结束时未发出 transferResultAvailable");
    QCOMPARE(resultSpy.count(), 1);
    QCOMPARE(resultSpy.constFirst().at(0).toBool(), true);
    QVERIFY2(!resultSpy.constFirst().at(1).toString().isEmpty(),
             "成功结果也必须带上可读文字，而不是空字符串");
}

void RemoteFileManagerWidgetTest::terminalDropUploadReportsProgress()
{
    const QByteArray host = qgetenv("SHELLIUM_IT_HOST");
    const QByteArray user = qgetenv("SHELLIUM_IT_USER");
    const QByteArray password = qgetenv("SHELLIUM_IT_PASSWORD");
    if (host.isEmpty() || user.isEmpty() || password.isEmpty()) {
        QSKIP("未设置 SHELLIUM_IT_HOST / SHELLIUM_IT_USER / SHELLIUM_IT_PASSWORD，跳过真实上传进度回归");
    }

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QFile payload(tempDir.filePath(QStringLiteral("shellium-drop-progress-test.bin")));
    QVERIFY(payload.open(QIODevice::WriteOnly));
    // 足够大才会有中间进度；每次内容不同，避免续传账本把它判成"已完整"直接跳过。
    QByteArray chunk(1024 * 1024, '\0');
    for (int index = 0; index < 4; ++index) {
        chunk.fill(static_cast<char>(QRandomGenerator::global()->bounded(256)));
        payload.write(chunk);
    }
    payload.close();

    ConnectionTarget target;
    target.id = QStringLiteral("integration-layer-1");
    target.host = QString::fromUtf8(host);
    target.username = QString::fromUtf8(user);
    target.password = QString::fromUtf8(password);
    const uint port = qgetenv("SHELLIUM_IT_PORT").toUInt();
    target.port = port == 0 ? 22 : static_cast<quint16>(port);

    RemoteFileManagerWidget widget;
    QSignalSpy progressSpy(&widget, &RemoteFileManagerWidget::terminalDropUploadProgress);
    QSignalSpy resultSpy(&widget, &RemoteFileManagerWidget::transferResultAvailable);
    widget.uploadFilesToDirectory(target, QStringLiteral("/tmp"), {payload.fileName()});

    QVERIFY2(!progressSpy.isEmpty(), "入队那一刻就必须通知终端标题栏开始展示");
    QCOMPARE(progressSpy.constFirst().at(0).toBool(), true);
    QCOMPARE(progressSpy.constFirst().at(1).toString(),
             QStringLiteral("shellium-drop-progress-test.bin"));

    QVERIFY2(resultSpy.wait(60000), "真实上传结束时未发出 transferResultAvailable");
    QCOMPARE(resultSpy.constFirst().at(0).toBool(), true);
    QTRY_VERIFY_WITH_TIMEOUT(!progressSpy.constLast().at(0).toBool(), 5000);

    bool sawPercent = false;
    for (const QList<QVariant> &arguments : std::as_const(progressSpy)) {
        sawPercent = sawPercent || (arguments.at(0).toBool() && arguments.at(2).toInt() >= 0);
    }
    QVERIFY2(sawPercent, "上传过程中必须至少报告一次具体百分比");
}

void RemoteFileManagerWidgetTest::pathEditsSelectAllExistingTextWhenFocused()
{
    RemoteFileManagerWidget widget;
    auto *localPathEdit = widget.findChild<QLineEdit *>(QStringLiteral("SftpLocalPathEdit"));
    auto *remotePathEdit = widget.findChild<QLineEdit *>(QStringLiteral("SftpRemotePathEdit"));
    QVERIFY(localPathEdit != nullptr);
    QVERIFY(remotePathEdit != nullptr);

    for (QLineEdit *pathEdit : {localPathEdit, remotePathEdit}) {
        pathEdit->setText(QStringLiteral("/var/www/html"));
        pathEdit->deselect();
        QVERIFY2(pathEdit->selectedText().isEmpty(),
                 "测试前置条件：焦点事件之前不应该已经选中文字");

        QFocusEvent focusIn(QEvent::FocusIn, Qt::MouseFocusReason);
        QCoreApplication::sendEvent(pathEdit, &focusIn);
        // 全选被安排在 QTimer::singleShot(0, ...) 里执行，必须先把事件循环转一圈。
        QCoreApplication::processEvents();

        QCOMPARE(pathEdit->selectedText(), QStringLiteral("/var/www/html"));
    }
}

void RemoteFileManagerWidgetTest::remoteNameColumnNeverShrinksBelowMinimumWidth()
{
    // 190px 与 RemoteFileManagerWidget.cpp 中 kRemoteNameColumnMinWidth 保持一致。
    constexpr int kExpectedMinWidth = 190;

    RemoteFileManagerWidget widget;
    auto *remoteTree = widget.findChild<QTreeWidget *>(QStringLiteral("RemoteFileTree"));
    QVERIFY(remoteTree != nullptr);

    // 无桌面的 CI 节点上不能 show() 顶层窗口，隐藏控件的内层 QSplitter 也就不会真正重新
    // 分配像素——remoteTree 的实际尺寸会停留在构造时的初始值，不随顶层 resize 变化。因此
    // 这里直接摆布 updateRemoteNameColumnWidth() 真正读取的输入（视口宽度），仍然只通过
    // RemoteFileManagerWidget 的公开虚函数入口（resizeEvent）触发计算，不直接调用私有方法。
    remoteTree->viewport()->resize(80, 400);
    QResizeEvent narrowResizeEvent(widget.size(), widget.size());
    QCoreApplication::sendEvent(&widget, &narrowResizeEvent);
    QCoreApplication::processEvents();
    QVERIFY2(remoteTree->columnWidth(0) >= kExpectedMinWidth,
             qPrintable(QStringLiteral("视口极窄时名称列只有 %1px").arg(remoteTree->columnWidth(0))));
    QCOMPARE(remoteTree->horizontalScrollBarPolicy(), Qt::ScrollBarAsNeeded);

    remoteTree->viewport()->resize(3000, 400);
    QResizeEvent wideResizeEvent(widget.size(), widget.size());
    QCoreApplication::sendEvent(&widget, &wideResizeEvent);
    QCoreApplication::processEvents();
    QVERIFY2(remoteTree->columnWidth(0) > kExpectedMinWidth,
             "视口足够宽时名称列应当吃掉多余空间，而不是停留在最小宽度");
}

void RemoteFileManagerWidgetTest::actionBarRespondsToAvailableWidth()
{
    RemoteFileManagerWidget widget;
    const QStringList actionObjectNames = {
        QStringLiteral("SftpUploadButton"),
        QStringLiteral("SftpDownloadButton"),
        QStringLiteral("SftpPreviewButton"),
        QStringLiteral("SftpChmodButton"),
        QStringLiteral("SftpRenameButton"),
        QStringLiteral("SftpDeleteButton"),
    };

    const QSize originalSize = widget.size();
    widget.resize(960, 620);
    // 不显示顶层窗口即可触发 RemoteFileManagerWidget::resizeEvent；这样测试在无桌面的构建
    // 节点上也能运行。Qt 会把隐藏顶层控件的 resizeEvent 延迟到首次显示，因此这里显式
    // 投递等价的尺寸事件，仍然经过生产控件的虚函数分发，而不是直接调用私有刷新函数。
    QResizeEvent wideResizeEvent(widget.size(), originalSize);
    QCoreApplication::sendEvent(&widget, &wideResizeEvent);
    QCoreApplication::processEvents();
    for (const QString &objectName : actionObjectNames) {
        auto *button = widget.findChild<QToolButton *>(objectName);
        QVERIFY2(button != nullptr, qPrintable(QStringLiteral("未找到操作按钮：%1").arg(objectName)));
        QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
        QVERIFY2(!button->text().isEmpty(), "宽态按钮必须直接显示操作名称");
    }

    const QSize wideSize = widget.size();
    widget.resize(620, 620);
    QResizeEvent narrowResizeEvent(widget.size(), wideSize);
    QCoreApplication::sendEvent(&widget, &narrowResizeEvent);
    QCoreApplication::processEvents();
    for (const QString &objectName : actionObjectNames) {
        auto *button = widget.findChild<QToolButton *>(objectName);
        QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonIconOnly);
        QVERIFY2(!button->text().isEmpty(), "紧凑态也必须保留动作文本供辅助功能读取");
        QVERIFY2(!button->toolTip().isEmpty(), "紧凑态不能只剩一个无解释图标");
        QCOMPARE(button->width(), 34);
    }
}

void RemoteFileManagerWidgetTest::transferWidgetsShareOneHiddenInformationBand()
{
    RemoteFileManagerWidget widget;
    auto *transferBand = widget.findChild<QFrame *>(QStringLiteral("SftpTransferBand"));
    auto *progress = widget.findChild<QProgressBar *>(QStringLiteral("SftpTransferProgress"));
    auto *queue = widget.findChild<QListWidget *>(QStringLiteral("SftpTransferQueue"));
    auto *resume = widget.findChild<QCheckBox *>(QStringLiteral("SftpResumeCheck"));
    QVERIFY(transferBand != nullptr);
    QVERIFY(progress != nullptr);
    QVERIFY(queue != nullptr);
    QVERIFY(resume != nullptr);

    QVERIFY2(transferBand->isHidden(), "没有传输任务时底部信息带必须整体收起");
    QVERIFY2(progress->parentWidget() == transferBand, "进度条必须属于统一传输信息带");
    QVERIFY2(queue->parentWidget() == transferBand, "传输队列必须属于统一传输信息带");
    QVERIFY2(!progress->isTextVisible(), "细进度条只表达比例，明细应放在独立状态行");
    QVERIFY2(resume->isChecked(), "断点续传必须沿用现有默认开启语义");
}

void RemoteFileManagerWidgetTest::closeButtonSitsNextToFullScreenAndOnlyRequestsClose()
{
    RemoteFileManagerWidget widget;
    auto *fullScreenButton = widget.findChild<QToolButton *>(QStringLiteral("SftpFullScreenButton"));
    auto *closeButton = widget.findChild<QToolButton *>(QStringLiteral("SftpCloseButton"));
    QVERIFY(fullScreenButton != nullptr);
    QVERIFY2(closeButton != nullptr, "顶栏必须提供关闭远程文件管理的入口");

    // 位置是需求的一部分：关闭必须紧挨在全屏右边，而不是散落到别的工具栏里。
    auto *topBarLayout = qobject_cast<QHBoxLayout *>(
        widget.findChild<QWidget *>(QStringLiteral("SftpTopBar"))->layout());
    QVERIFY(topBarLayout != nullptr);
    const int fullScreenIndex = topBarLayout->indexOf(fullScreenButton);
    QVERIFY(fullScreenIndex >= 0);
    QCOMPARE(topBarLayout->indexOf(closeButton), fullScreenIndex + 1);

    QVERIFY(!closeButton->icon().isNull());

    // 控件不能自行隐藏：显隐由 TerminalWorkspace 统一处理，它还要同步退出全屏并回写终端
    // 顶部按钮。自行 hide() 会让那两处状态与实际显示脱节。
    QSignalSpy closeSpy(&widget, &RemoteFileManagerWidget::closeRequested);
    // 用例不 show()（CI 可能没有屏幕），因此比较点击前后的显隐意图，而不是绝对可见性。
    const bool hiddenBeforeClick = widget.isHidden();
    closeButton->click();
    QCOMPARE(closeSpy.count(), 1);
    QCOMPARE(widget.isHidden(), hiddenBeforeClick);
}

void RemoteFileManagerWidgetTest::topBarActionsUseRemoteFileHoverStyle()
{
    RemoteFileManagerWidget widget;
    auto *fullScreenButton = widget.findChild<QToolButton *>(QStringLiteral("SftpFullScreenButton"));
    auto *closeButton = widget.findChild<QToolButton *>(QStringLiteral("SftpCloseButton"));
    QVERIFY(fullScreenButton != nullptr);
    QVERIFY(closeButton != nullptr);

    // 两个顶栏动作必须带同一个样式属性，生产 QSS 才能将它们同时切换到比顶栏底色
    // 更深的悬停背景；若遗漏任意一个按钮，视觉反馈会再次出现左右不一致。
    QVERIFY2(fullScreenButton->property("sftpFullScreenAction").toBool(),
             "全屏按钮必须接入远程文件管理顶栏动作样式");
    QVERIFY2(closeButton->property("sftpFullScreenAction").toBool(),
             "关闭按钮必须接入远程文件管理顶栏动作样式");
}

namespace {

/**
 * @brief 把任意源文案翻成「!!<原文>」的测试替身翻译器。
 *
 * 不需要 .qm，也不拉入 LocalizationService；只为触发 QEvent::LanguageChange 并让
 * tr() 返回可辨识的结果。
 */
class MarkerTranslator final : public QTranslator {
public:
    QString translate(const char *context,
                      const char *sourceText,
                      const char *disambiguation = nullptr,
                      int n = -1) const override
    {
        Q_UNUSED(context)
        Q_UNUSED(disambiguation)
        Q_UNUSED(n)
        return QStringLiteral("!!") + QString::fromUtf8(sourceText);
    }

    bool isEmpty() const override { return false; }
};

} // namespace

void RemoteFileManagerWidgetTest::uploadMenuActionsRetranslateOnLanguageChange()
{
    RemoteFileManagerWidget widget;
    auto *uploadFilesAction =
        widget.findChild<QAction *>(QStringLiteral("SftpUploadFilesAction"));
    auto *uploadDirectoryAction =
        widget.findChild<QAction *>(QStringLiteral("SftpUploadDirectoryAction"));
    QVERIFY(uploadFilesAction != nullptr);
    QVERIFY(uploadDirectoryAction != nullptr);
    QCOMPARE(uploadFilesAction->text(), QStringLiteral("Upload Files…"));

    MarkerTranslator translator;
    QVERIFY(QCoreApplication::installTranslator(&translator));
    // installTranslator 会向 qApp 投递 LanguageChange；Retranslator 的事件过滤器据此重跑闭包。
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LanguageChange);

    QCOMPARE(uploadFilesAction->text(), QStringLiteral("!!Upload Files…"));
    QCOMPARE(uploadDirectoryAction->text(), QStringLiteral("!!Upload Directory…"));

    QVERIFY(QCoreApplication::removeTranslator(&translator));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LanguageChange);
    QCOMPARE(uploadFilesAction->text(), QStringLiteral("Upload Files…"));
}

QTEST_MAIN(RemoteFileManagerWidgetTest)
#include "RemoteFileManagerWidgetTest.moc"
