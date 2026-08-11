#include "sftp/RemoteFileManagerWidget.h"
#include "ssh/SshTunnelBroker.h"

#include <QCheckBox>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QListWidget>
#include <QProgressBar>
#include <QResizeEvent>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>

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
 * @return 始终返回 -1，表示测试环境不提供 socket。
 */
int SshTunnelBroker::openSocket(const QString &routeId, QString *errorMessage)
{
    Q_UNUSED(routeId)
    if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("界面测试未启用 SSH 隧道");
    }
    return -1;
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

    /** @brief 宽窗口展示动作文字，窄窗口保留语义和提示但改用紧凑图标。 */
    void actionBarRespondsToAvailableWidth();

    /** @brief 进度、状态和队列必须归入同一底部信息带，并在空闲时整体隐藏。 */
    void transferWidgetsShareOneHiddenInformationBand();

    /** @brief 远端 SFTP 中继命令必须保留端口和用户名，并阻断 Shell 参数注入。 */
    void remoteSftpRelayCommandQuotesEveryUntrustedArgument();

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
         QStringLiteral("SftpRemotePathEdit")});

    auto *localTree = widget.findChild<QTreeView *>(QStringLiteral("LocalFileTree"));
    QVERIFY(localTree != nullptr);
    QVERIFY(!localTree->isColumnHidden(0));
    QVERIFY(!localTree->isColumnHidden(1));
    QVERIFY2(localTree->isColumnHidden(2), "名称图标已表达文件类型，不应再浪费一列重复展示");
    QVERIFY(!localTree->isColumnHidden(3));
    QCOMPARE(localTree->model()->headerData(0, Qt::Horizontal).toString(), QStringLiteral("名称"));
    QCOMPARE(localTree->model()->headerData(1, Qt::Horizontal).toString(), QStringLiteral("大小"));
    QCOMPARE(localTree->model()->headerData(3, Qt::Horizontal).toString(), QStringLiteral("修改时间"));
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

void RemoteFileManagerWidgetTest::remoteFileTableUsesReferenceColumns()
{
    RemoteFileManagerWidget widget;
    auto *remoteTree = widget.findChild<QTreeWidget *>(QStringLiteral("RemoteFileTree"));
    QVERIFY(remoteTree != nullptr);
    QCOMPARE(remoteTree->columnCount(), 4);
    QCOMPARE(remoteTree->headerItem()->text(0), QStringLiteral("名称"));
    QCOMPARE(remoteTree->headerItem()->text(1), QStringLiteral("大小"));
    QCOMPARE(remoteTree->headerItem()->text(2), QStringLiteral("权限"));
    QCOMPARE(remoteTree->headerItem()->text(3), QStringLiteral("所有者"));
    QCOMPARE(remoteTree->header()->sectionResizeMode(0), QHeaderView::Stretch);
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

QTEST_MAIN(RemoteFileManagerWidgetTest)
#include "RemoteFileManagerWidgetTest.moc"
