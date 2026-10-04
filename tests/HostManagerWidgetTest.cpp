#include "core/HostRecord.h"
#include "core/ExcelHostCodec.h"
#include "security/Vault.h"
#include "storage/ApplicationDatabase.h"
#include "ssh/SshSessionStack.h"
#include "ui/HostDistributionIcon.h"
#include "ui/HostManagerWidget.h"
#include "ui/UiIcons.h"

#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileIconProvider>
#include <QFrame>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTest>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <minizip/zip.h>

#include <algorithm>
#include <memory>

/**
 * @brief 主机侧栏的展示与动作布局回归。
 *
 * 全部用例跑在内存数据库上，绝不触碰用户真实数据文件。
 */
class HostManagerWidgetTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    /** @brief 主机副标题必须补齐从分组继承来的账号与端口，否则看不出实际连去哪里。 */
    void hostSubtitleShowsInheritedAccountHostAndPort();

    /** @brief 分组没有连接信息，不得被画成两行。 */
    void groupHasNoSubtitle();

    /** @brief 侧栏标题行只暴露新建主机、新建分组与刷新三个动作。 */
    void headerExposesOnlyThreePrimaryActions();

    /** @brief 主机右键菜单必须提供复制，并把新建主机排在新建分组之前。 */
    void hostContextMenuProvidesCopyAndRequestedCreationOrder();

    /** @brief Excel 模板必须是可读取的 xlsx，并使用约定的四个英文列名。 */
    void excelTemplateAndWorkbookUseEnglishHeaders();

    /** @brief Excel 行写入当前目录，非空密码必须进入加密凭据而不是主机明文字段。 */
    void excelImportPersistsHostsAndEncryptsPasswords();

    /**
     * @brief “在工作空间中连接”只应出现在分组右键菜单里、紧跟在“新建分组”之后，
     * 点击后必须携带正确的分组 id 与名称发出信号。
     */
    void folderContextMenuOffersConnectInWorkspaceAndEmitsGroupSignal();

    /**
     * @brief 主机右键菜单的“Connect”之后必须紧跟“通过主机链连接…”动作，它打开对话框
     * 而不是子菜单；对话框顶部“最近连接”按时间倒序列出成功连接到*本目标主机*所用的跳板，
     * 至多三个，且与其他目标的连接历史无关；“全部主机”不重复列出它们，也不含目标自身。
     * 可见行依次绑定 Cmd+1…Cmd+9，按下即发出“跳板 + 目标”两跳连接计划。
     */
    void hostContextMenuOpensHostChainDialogWithRecentConnections();

    /** @brief 搜索过滤后 Cmd+数字 必须按当前可见行重新编号，不能连到被隐藏的主机。 */
    void hostChainDialogShortcutsFollowVisibleRowsAfterSearch();

    /** @brief 旧版只记录一个“上次连接”跳板的设置在升级后仍作为最近连接展示。 */
    void hostChainDialogReadsLegacyLastJumpSetting();

    /** @brief 分组连接必须递归收集任意深度子分组下的主机，且不包含分组节点本身。 */
    void collectHostsInGroupRecursesNestedFolders();

    /** @brief 连续复制必须递增名称序号，同时保留完整配置与父分组。 */
    void copyHostKeepsConfigurationInSameGroup();

    /** @brief 分组配置窗口只能保留名称字段，且页脚按钮必须带语义图标。 */
    void groupDialogHasNoStrayWidgets();

    /** @brief 主机配置字段顺序、ProxyJump 高级入口、页脚和按钮图标必须符合新版交互。 */
    void hostDialogUsesRequestedFieldOrderAndIcons();
    void newHostFocusesAddressFieldAndDropsInheritanceCopy();

    /** @brief 新建主机输入的密码与私钥路径必须随保存自动持久化，不再依赖额外开关。 */
    void newHostAutomaticallyPersistsPasswordAndIdentityFile();

    /** @brief 编辑页默认掩码密码；未启用时眼睛图标给出引导，启用后才能切换明文。 */
    void editHostMasksPasswordAndGatesVisibility();

    /** @brief 编辑保存和手动刷新重建树后，必须按稳定 id 恢复原主机的选中高亮。 */
    void selectionSurvivesHostEditAndManualRefresh();

    /** @brief 目录每次展开/折叠都应立即保存，并在刷新、改名和下次启动时按稳定 id 恢复。 */
    void folderExpansionStatePersistsAcrossRefreshAndRestart();

    /** @brief 新建主机保存后的自动连接必须打开新标签，不得覆盖当前会话。 */
    void newlySavedHostRequestsConnectionInNewTab();

    /** @brief 编辑已有 ProxyJump 时必须自动展开并明确提示，避免兼容路由被视觉弱化后隐身。 */
    void configuredProxyJumpRemainsVisibleWhenEditing();

    /** @brief 名称为空时应根据账号、地址和端口自动命名，且不得覆盖人工名称。 */
    void emptyHostNameFollowsConnectionFieldsUntilManuallyEdited();

    /** @brief 主机链编辑器应能选择已有主机，并能现场新建主机后自动加入当前链路。 */
    void hostChainEditorCreatesAndSelectsManagedJumpHost();

    /** @brief 顶层节点顶格、分组下的主机右缩进，二者不能再被分支装饰列推乱。 */
    void topLevelNodesShareLeftEdgeAndChildHostsIndent();

    /** @brief 展开箭头必须位于分组左侧，且完整图标区域都能展开、收起。 */
    void groupToggleIsLeftAlignedVerticallyCenteredAndClickable();

    /** @brief 子主机悬停或选中时不得改变左侧中性连线，只允许右侧圆角卡片变化。 */
    void childHostInteractionDoesNotAlterGuideLines();

    /** @brief 密度切换缩小实际行高、缩进及图标，同时保留选中项和折叠状态。 */
    void compactModeShrinksTreeAndPreservesState();

    /** @brief 精简模式只画主机名称、不画地址副标题，且行高与缩进都比紧凑模式更紧。 */
    void minimalModeHidesAddressAndShrinksFurtherThanCompact();

    /** @brief 多层展开时祖先连线连续，末尾节点以直角收尾，隐藏兄弟不延长连线。 */
    void nestedGuideLinesConnectAndStopAtLastSibling();

    /** @brief 空分组保留为可用拖放目标，但右侧不得再绘制无信息价值的“0”徽章。 */
    void emptyGroupDoesNotPaintZeroCountBadge();

    /** @brief 主机与目录名称都使用常规字重，层级通过图标、缩进和连线表达。 */
    void hostAndFolderNamesUseRegularWeight();

    /** @brief OpenCloudOS 使用专用云形图标，不再错误显示通用 Linux 企鹅。 */
    void openCloudOsUsesDedicatedDistributionIcon();

    /** @brief 麒麟、统信 UOS 与深度系统的常见 os-release ID 都映射到各自图标。 */
    void domesticLinuxDistributionsUseDedicatedIcons();

    /** @brief 标题数量必须统计全部嵌套主机，不能把分组或当前搜索结果计入。 */
    void hostCountTracksAllNestedHosts();

    /** @brief 搜索框右侧的标签胶囊必须进入 #标签 查询模式并保留焦点。 */
    void tagShortcutEntersTagSearchMode();

    /** @brief 进入 #标签 模式后必须弹出全部已知标签供快捷选择，点击胶囊即完成精确筛选。 */
    void tagSearchModeShowsSuggestionPopupForQuickPick();

    /** @brief 同一个自绘图标实例必须在主题切换后改用新前景色，显式品牌色则保持不变。 */
    void semanticIconsFollowLightAndDarkPalettes();

    /** @brief 通用下拉弹层只能保留内部圆角列表，不得再由原生容器绘制直角外框。 */
    void comboPopupRemovesOuterSquareFrame();

    /** @brief 无父分组的主机必须排在全部顶层分组之后，并由一个不可选中的"未分组"标题隔开。 */
    void ungroupedHostsFollowFoldersBehindSectionHeader();

    /** @brief 没有任何无父分组主机时，树里不应出现"未分组"标题这个多余的空壳分区。 */
    void sectionHeaderAbsentWhenNoUngroupedHosts();

    /** @brief 顶层拖放保存必须跳过“未分组”展示标题，并为真实记录生成连续排序号。 */
    void rootReorderIgnoresUngroupedSectionHeader();

    /** @brief 同组主机之间的首个拖动事件必须立即生效，不得要求先悬停到任意分组刷新落点。 */
    void sameGroupReorderAcceptsFirstDragMove();

    /** @brief 名称行必须排在端口下方，并在左侧带分组下拉、框内右端带标签入口且宽度不变。 */
    void hostDialogPlacesNameBelowPortWithGroupAndTagEntry();

    /** @brief 标签编辑器写入的标签与配色必须落库，并在主机树徽章与历史候选中生效。 */
    void tagEditorPersistsTagWithChosenColorAndOffersHistory();

    /** @brief 名称行的分组下拉必须真正改变主机归属，而不只是显示用的装饰。 */
    void groupComboMovesHostIntoSelectedGroup();

    /** @brief 分组下拉框的顺序必须与侧边栏主机树一致（按 sortOrder），而不是按名称字母排序。 */
    void groupComboOrderMatchesSidebarTreeOrder();

    /** @brief 已识别发行版的主机必须换成对应发行版图标，并在悬停提示中给出具体版本。 */
    void detectedDistributionDrivesHostIconAndTooltip();

    /** @brief 国产发行版识别后使用专用图标，并在提示中保留规范化系统名称。 */
    void domesticDistributionKeepsLinuxIconInsteadOfGenericHost();

    /** @brief 改掉地址、端口或账号后必须清空缓存的发行版，避免图标长期停留在旧系统。 */
    void editingConnectionTargetClearsCachedDistribution();

    /** @brief 远端 os-release 输出不可信：非法 ID、控制字符与超长文本都必须被拒绝。 */
    void remoteDistributionParserRejectsUnsafeOutput();

private:
    [[nodiscard]] QTreeWidgetItem *findItem(const HostManagerWidget &widget,
                                            const QString &name) const;

    std::unique_ptr<ApplicationDatabase> m_database;
    std::unique_ptr<CredentialVault> m_vault;
};

namespace {

/**
 * @brief 为 Excel 解析回归测试生成一个最小但真实的 xlsx 文件。
 * @param filePath 测试文件路径。
 * @return ZIP 条目全部写入成功时返回 true。
 *
 * 生产代码只负责读取工作簿，测试不能把 CSV 或 XML 裸文件当作 xlsx；这里直接使用
 * minizip 生成标准 ZIP 容器，确保共享字符串/内联字符串之外的 ZIP 边界也被覆盖。
 */
bool writeExcelFixture(const QString &filePath)
{
    const QByteArray path = QFile::encodeName(filePath);
    zipFile archive = zipOpen64(path.constData(), APPEND_STATUS_CREATE);
    if (archive == nullptr) {
        return false;
    }
    const auto addEntry = [archive](const char *entryName, const QByteArray &content) {
        zip_fileinfo info{};
        if (zipOpenNewFileInZip64(archive,
                                  entryName,
                                  &info,
                                  nullptr,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  Z_DEFLATED,
                                  Z_DEFAULT_COMPRESSION,
                                  1)
            != ZIP_OK) {
            return false;
        }
        const bool written = zipWriteInFileInZip(archive, content.constData(), content.size())
            == ZIP_OK;
        const bool closed = zipCloseFileInZip(archive) == ZIP_OK;
        return written && closed;
    };
    const QByteArray types = R"xml(<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/></Types>)xml";
    const QByteArray rels = R"xml(<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>)xml";
    const QByteArray workbook = R"xml(<?xml version="1.0"?><workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="Hosts" sheetId="1" r:id="rId1"/></sheets></workbook>)xml";
    const QByteArray workbookRels = R"xml(<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/></Relationships>)xml";
    const QByteArray worksheet = R"xml(<?xml version="1.0"?><worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData><row r="1"><c r="A1" t="inlineStr"><is><t>name</t></is></c><c r="B1" t="inlineStr"><is><t>host</t></is></c><c r="C1" t="inlineStr"><is><t>username</t></is></c><c r="D1" t="inlineStr"><is><t>password</t></is></c></row><row r="2"><c r="A2" t="inlineStr"><is><t>Test host</t></is></c><c r="B2" t="inlineStr"><is><t>192.0.2.10</t></is></c><c r="C2" t="inlineStr"><is><t>alice</t></is></c><c r="D2" t="inlineStr"><is><t> secret </t></is></c></row><row r="3"><c r="A3" t="inlineStr"><is><t>No password</t></is></c><c r="B3" t="inlineStr"><is><t>192.0.2.11</t></is></c><c r="C3" t="inlineStr"><is><t>bob</t></is></c></row></sheetData></worksheet>)xml";
    const bool success = addEntry("[Content_Types].xml", types)
        && addEntry("_rels/.rels", rels)
        && addEntry("xl/workbook.xml", workbook)
        && addEntry("xl/_rels/workbook.xml.rels", workbookRels)
        && addEntry("xl/worksheets/sheet1.xml", worksheet);
    return success && zipClose(archive, nullptr) == ZIP_OK;
}

} // namespace

void HostManagerWidgetTest::init()
{
    // 内存数据库保证每个用例都从空库开始，也不会触碰用户真实数据文件。
    m_database = std::make_unique<ApplicationDatabase>(ApplicationDatabase::inMemoryPath());
    QString error;
    QVERIFY2(m_database->initialize(&error), qPrintable(error));
    m_vault = std::make_unique<CredentialVault>(*m_database);
}

void HostManagerWidgetTest::cleanup()
{
    m_vault.reset();
    m_database.reset();
}

QTreeWidgetItem *HostManagerWidgetTest::findItem(const HostManagerWidget &widget,
                                                 const QString &name) const
{
    auto *tree = widget.findChild<QTreeWidget *>();
    if (tree == nullptr) {
        return nullptr;
    }
    const QList<QTreeWidgetItem *> matches =
        tree->findItems(name, Qt::MatchExactly | Qt::MatchRecursive);
    return matches.isEmpty() ? nullptr : matches.constFirst();
}

void HostManagerWidgetTest::hostSubtitleShowsInheritedAccountHostAndPort()
{
    HostRecord group;
    group.id = QStringLiteral("group-1");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("武汉");
    group.username = QStringLiteral("root");
    group.port = 2222;
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    // 主机自身刻意不设账号和端口：空值代表"继承分组"，而不是"没有账号"。
    HostRecord host;
    host.id = QStringLiteral("host-1");
    host.parentId = group.id;
    host.kind = HostNodeKind::Host;
    host.name = QStringLiteral("市民热线预发布");
    host.host = QStringLiteral("10.194.135.11");
    QVERIFY2(m_database->saveHost(host, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    QTreeWidgetItem *item = findItem(widget, host.name);
    QVERIFY2(item != nullptr, "主机节点未出现在树中");

    const QString subtitle = item->data(0, Qt::UserRole + 2).toString();
    QCOMPARE(subtitle, QStringLiteral("root@10.194.135.11:2222"));
    // 悬停提示与副标题必须一致，避免两处各自拼一份地址后逐渐讲出不同的故事。
    QCOMPARE(item->toolTip(0), subtitle);
}

void HostManagerWidgetTest::groupHasNoSubtitle()
{
    HostRecord group;
    group.id = QStringLiteral("group-2");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("江夏镇");
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    QTreeWidgetItem *item = findItem(widget, group.name);
    QVERIFY2(item != nullptr, "分组节点未出现在树中");
    QVERIFY(item->data(0, Qt::UserRole + 2).toString().isEmpty());

    // 主机分组应与 QFileSystemModel 驱动的“本地文件”列表使用同源系统文件夹图标。直接
    // 比较最终位图而不是图标 cacheKey：不同 QIcon 实例可能绘制完全相同，但缓存键不同。
    QFileIconProvider iconProvider;
    QIcon expectedIcon = iconProvider.icon(QFileIconProvider::Folder);
    if (expectedIcon.isNull()) {
        expectedIcon = UiIcons::symbol(
            &widget,
            UiIcons::Symbol::Folder,
            widget.palette().color(QPalette::WindowText));
    }
    const QSize comparisonSize(20, 20);
    QCOMPARE(item->icon(0).pixmap(comparisonSize).toImage(),
             expectedIcon.pixmap(comparisonSize).toImage());
}

void HostManagerWidgetTest::headerExposesOnlyThreePrimaryActions()
{
    HostManagerWidget widget(m_database.get(), m_vault.get());
    QWidget *header = widget.headerActionsWidget();
    QVERIFY(header != nullptr);

    QStringList labels;
    const QList<QAbstractButton *> buttons = header->findChildren<QAbstractButton *>();
    for (const QAbstractButton *button : buttons) {
        labels.append(button->text());
    }
    std::sort(labels.begin(), labels.end());
    QCOMPARE(labels,
             (QStringList{QStringLiteral("New group"),
                          QStringLiteral("New host"),
                          QStringLiteral("Refresh")}));

    // 导入/导出已移入树的右键菜单，不应再占用侧栏顶部的常驻位置。
    QVERIFY(!labels.contains(QStringLiteral("导入")));
    QVERIFY(!labels.contains(QStringLiteral("导出")));

    auto *addHostButton = header->findChild<QPushButton *>(QStringLiteral("HostAddButton"));
    QVERIFY2(addHostButton != nullptr, "主标题动作区缺少可精确设置尺寸的新建主机按钮");
    QCOMPARE(addHostButton->height(), 30);
    QVERIFY2(addHostButton->minimumWidth() <= 90,
             "新建主机按钮仍占用过多横向空间，窄侧栏下会挤压标题或右侧动作");

    // "最近连接"整块已移除，面板里不应再残留任何列表控件。
    QVERIFY(widget.findChild<QListWidget *>() == nullptr);
}

void HostManagerWidgetTest::excelTemplateAndWorkbookUseEnglishHeaders()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QString templatePath = temporaryDirectory.filePath(QStringLiteral("template.xlsx"));
    QString error;
    QVERIFY2(ExcelHostCodec::writeTemplate(templatePath, &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(templatePath));
    QVERIFY(QFileInfo(templatePath).size() > 0);
    QList<ExcelHostCodec::HostRow> templateRows;
    QVERIFY(!ExcelHostCodec::readWorkbook(templatePath, &templateRows, &error));
    // 模板只有英文表头、没有示例数据；读取时应先通过表头校验，再明确报告“没有记录”，
    // 而不是把模板误判成 ZIP/XML 格式错误。
    QCOMPARE(error, QStringLiteral("Excel 文件中没有可导入的主机记录"));

    const QString fixturePath = temporaryDirectory.filePath(QStringLiteral("hosts.xlsx"));
    QVERIFY(writeExcelFixture(fixturePath));
    QList<ExcelHostCodec::HostRow> rows;
    QVERIFY2(ExcelHostCodec::readWorkbook(fixturePath, &rows, &error), qPrintable(error));
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0).name, QStringLiteral("Test host"));
    QCOMPARE(rows.at(0).host, QStringLiteral("192.0.2.10"));
    QCOMPARE(rows.at(0).username, QStringLiteral("alice"));
    QCOMPARE(rows.at(0).password, QStringLiteral(" secret "));
    QCOMPARE(rows.at(1).password, QString());
}

void HostManagerWidgetTest::excelImportPersistsHostsAndEncryptsPasswords()
{
    HostRecord group;
    group.id = QStringLiteral("excel-target-group");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("Excel target");
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    const QList<ExcelHostCodec::HostRow> rows{
        {QStringLiteral("Imported with password"),
         QStringLiteral("192.0.2.30"),
         QStringLiteral("alice"),
         QStringLiteral("password-from-excel")},
        {QStringLiteral("Imported without password"),
         QStringLiteral("192.0.2.31"),
         QStringLiteral("bob"),
         QString()},
    };
    QVERIFY2(widget.persistExcelRows(rows, group.id, &error), qPrintable(error));

    bool loaded = false;
    const QList<HostRecord> imported = m_database->listHostsInGroup(group.id, &loaded, &error);
    QVERIFY2(loaded, qPrintable(error));
    QCOMPARE(imported.size(), 2);

    const auto passwordHost = std::find_if(imported.cbegin(), imported.cend(), [](const HostRecord &record) {
        return record.name == QStringLiteral("Imported with password");
    });
    QVERIFY(passwordHost != imported.cend());
    QVERIFY(!passwordHost->credentialId.isEmpty());
    const std::optional<CredentialRecord> credential =
        m_database->findCredential(passwordHost->credentialId, &error);
    QVERIFY2(credential.has_value(), qPrintable(error));
    QVERIFY(!credential->encryptedPayload.contains("password-from-excel"));

    ConnectionTarget target = passwordHost->toConnectionTarget();
    QVERIFY2(m_vault->applyHostCredential(*passwordHost, target, &error), qPrintable(error));
    QCOMPARE(target.password, QStringLiteral("password-from-excel"));

    const auto passwordlessHost = std::find_if(imported.cbegin(), imported.cend(), [](const HostRecord &record) {
        return record.name == QStringLiteral("Imported without password");
    });
    QVERIFY(passwordlessHost != imported.cend());
    QVERIFY(passwordlessHost->credentialId.isEmpty());
}

void HostManagerWidgetTest::hostContextMenuProvidesCopyAndRequestedCreationOrder()
{
    HostRecord host;
    host.id = QStringLiteral("context-host");
    host.kind = HostNodeKind::Host;
    host.name = QStringLiteral("菜单测试主机");
    host.host = QStringLiteral("192.0.2.20");
    QString error;
    QVERIFY2(m_database->saveHost(host, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    QTreeWidgetItem *hostItem = findItem(widget, host.name);
    QVERIFY2(hostItem != nullptr, "主机节点未出现在树中");

    // 直接调用独立菜单构造入口，不弹出依赖窗口系统的原生菜单；这样测试只关心动作集合和
    // 顺序，不会因 CI 的无头平台、焦点切换或菜单动画产生偶发失败。
    QMenu menu;
    widget.populateContextMenu(&menu, hostItem);
    QStringList actionTexts;
    for (const QAction *action : menu.actions()) {
        if (!action->isSeparator()) {
            actionTexts.append(action->text());
        }
    }

    const qsizetype addHostIndex = actionTexts.indexOf(QStringLiteral("New host"));
    const qsizetype addGroupIndex = actionTexts.indexOf(QStringLiteral("New group"));
    QVERIFY2(addHostIndex >= 0, "主机右键菜单缺少新建主机动作");
    QVERIFY2(addGroupIndex >= 0, "主机右键菜单缺少新建分组动作");
    QVERIFY2(addHostIndex < addGroupIndex, "新建主机没有排在新建分组之前");
    QVERIFY2(actionTexts.contains(QStringLiteral("Copy")), "主机右键菜单缺少复制动作");

    const qsizetype importIndex = actionTexts.indexOf(QStringLiteral("Import Hosts…"));
    const qsizetype excelImportIndex = actionTexts.indexOf(QStringLiteral("Import from Excel…"));
    QVERIFY2(importIndex >= 0, "主机右键菜单缺少普通导入动作");
    QVERIFY2(excelImportIndex == importIndex + 1,
             "从 Excel 导入必须紧跟在普通导入动作下面");

    // 主机不是分组，右键菜单不应提供“在工作空间中连接”——它没有可展开的子主机集合。
    QVERIFY2(!actionTexts.contains(QStringLiteral("Connect in Workspace")),
             "主机右键菜单不应出现分组专属的工作空间连接动作");
}

void HostManagerWidgetTest::folderContextMenuOffersConnectInWorkspaceAndEmitsGroupSignal()
{
    HostRecord group;
    group.id = QStringLiteral("workspace-connect-group");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("互联网");
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    QTreeWidgetItem *groupItem = findItem(widget, group.name);
    QVERIFY2(groupItem != nullptr, "分组节点未出现在树中");

    QMenu menu;
    widget.populateContextMenu(&menu, groupItem);
    QStringList actionTexts;
    QAction *connectGroupAction = nullptr;
    for (QAction *action : menu.actions()) {
        if (action->isSeparator()) {
            continue;
        }
        actionTexts.append(action->text());
        if (action->text() == QStringLiteral("Connect in Workspace")) {
            connectGroupAction = action;
        }
    }

    const qsizetype addGroupIndex = actionTexts.indexOf(QStringLiteral("New group"));
    const qsizetype connectGroupIndex = actionTexts.indexOf(QStringLiteral("Connect in Workspace"));
    QVERIFY2(addGroupIndex >= 0, "分组右键菜单缺少新建分组动作");
    QVERIFY2(connectGroupIndex == addGroupIndex + 1,
             "“在工作空间中连接”必须紧跟在“新建分组”之后");
    QVERIFY2(connectGroupAction != nullptr, "分组右键菜单缺少工作空间连接动作");

    QSignalSpy groupConnectSpy(&widget, &HostManagerWidget::hostGroupConnectionRequested);
    connectGroupAction->trigger();
    QCOMPARE(groupConnectSpy.count(), 1);
    const QList<QVariant> emitted = groupConnectSpy.constFirst();
    QCOMPARE(emitted.at(0).toString(), group.id);
    QCOMPARE(emitted.at(1).toString(), group.name);
}

namespace {

/** @brief 保存一台测试主机并返回其记录。 */
HostRecord saveChainTestHost(ApplicationDatabase *database,
                             const QString &id,
                             const QString &name,
                             const QString &address)
{
    HostRecord record;
    record.id = id;
    record.kind = HostNodeKind::Host;
    record.name = name;
    record.host = address;
    QString error;
    if (!database->saveHost(record, &error)) {
        qWarning() << "保存测试主机失败" << error;
    }
    return record;
}

/** @brief 返回列表中全部行的主机 id，按显示顺序。 */
QStringList listHostIds(const QListWidget *list)
{
    QStringList ids;
    for (int row = 0; row < list->count(); ++row) {
        ids.append(list->item(row)->data(Qt::UserRole).toString());
    }
    return ids;
}

/** @brief 返回 Cmd+数字 在本平台对应的修饰键；Qt/macOS 把物理 Command 映射为 ControlModifier。 */
constexpr Qt::KeyboardModifier kCommandModifier = Qt::ControlModifier;

} // namespace

void HostManagerWidgetTest::hostContextMenuOpensHostChainDialogWithRecentConnections()
{
    const HostRecord target = saveChainTestHost(
        m_database.get(), QStringLiteral("chain-target-host"), QStringLiteral("链目标主机"),
        QStringLiteral("192.0.2.30"));
    const HostRecord otherTarget = saveChainTestHost(
        m_database.get(), QStringLiteral("chain-other-target-host"), QStringLiteral("另一台目标主机"),
        QStringLiteral("192.0.2.33"));
    QList<HostRecord> jumps;
    for (int index = 1; index <= 5; ++index) {
        jumps.append(saveChainTestHost(m_database.get(),
                                       QStringLiteral("chain-jump-%1").arg(index),
                                       QStringLiteral("跳板%1").arg(index),
                                       QStringLiteral("192.0.2.%1").arg(40 + index)));
    }

    HostManagerWidget widget(m_database.get(), m_vault.get());

    // 跳板 5 只成功连接过“另一台目标主机”，不能出现在本目标的最近连接里。
    widget.recordSuccessfulChainConnection(otherTarget.id, jumps.at(4).id);
    // 本目标依次成功经过 1、2、3、4、2：最近连接应为 2、4、3（去重、最新在前、至多三个）。
    for (const int index : {0, 1, 2, 3, 1}) {
        widget.recordSuccessfulChainConnection(target.id, jumps.at(index).id);
    }

    QTreeWidgetItem *targetItem = findItem(widget, target.name);
    QVERIFY2(targetItem != nullptr, "目标主机节点未出现在树中");

    QMenu menu;
    widget.populateContextMenu(&menu, targetItem);
    const QList<QAction *> topLevelActions = menu.actions();
    const qsizetype connectIndex = std::find_if(topLevelActions.constBegin(),
                                                topLevelActions.constEnd(),
                                                [](const QAction *action) {
                                                    return action->text() == QStringLiteral("Connect");
                                                })
        - topLevelActions.constBegin();
    QVERIFY2(connectIndex + 1 < topLevelActions.size(), "Connect 之后缺少通过主机链连接动作");
    QAction *chainAction = topLevelActions.at(connectIndex + 1);
    QCOMPARE(chainAction->text(), QStringLiteral("Connect via Host Chain…"));
    QVERIFY2(chainAction->menu() == nullptr, "通过主机链连接必须弹出对话框，不能再是子菜单");

    std::unique_ptr<QDialog> dialog(widget.createHostChainConnectDialog(target.id));
    QVERIFY(dialog != nullptr);
    auto *recentList = dialog->findChild<QListWidget *>(QStringLiteral("hostChainConnectRecentList"));
    auto *hostList = dialog->findChild<QListWidget *>(QStringLiteral("hostChainConnectHostList"));
    QVERIFY(recentList != nullptr);
    QVERIFY(hostList != nullptr);

    QCOMPARE(listHostIds(recentList),
             QStringList({jumps.at(1).id, jumps.at(3).id, jumps.at(2).id}));
    const QStringList hostIds = listHostIds(hostList);
    QVERIFY2(!hostIds.contains(target.id), "全部主机不应把目标主机自身列为候选跳板");
    for (const QString &recentId : listHostIds(recentList)) {
        QVERIFY2(!hostIds.contains(recentId), "最近连接中的跳板不应在全部主机里重复出现");
    }
    QVERIFY2(hostIds.contains(jumps.at(4).id), "只连过其他目标的跳板仍应作为普通候选出现");
    QVERIFY2(hostIds.contains(jumps.at(0).id), "被挤出最近连接的跳板仍应作为普通候选出现");

    // 编号从最近连接开始，跨到全部主机继续递增。
    const QString cmd1 = QKeySequence(Qt::CTRL | Qt::Key_1).toString(QKeySequence::NativeText);
    const QString cmd4 = QKeySequence(Qt::CTRL | Qt::Key_4).toString(QKeySequence::NativeText);
    QCOMPARE(recentList->item(0)->data(Qt::UserRole + 2).toString(), cmd1);
    QCOMPARE(hostList->item(0)->data(Qt::UserRole + 2).toString(), cmd4);

    int connectionRequestCount = 0;
    QString requestedHostId;
    QString requestedJumpHostId;
    QList<ConnectionTarget> requestedPlan;
    bool requestedNewTab = false;
    connect(&widget,
            &HostManagerWidget::hostChainConnectionRequested,
            this,
            [&](const QString &hostId,
                const QString &jumpHostId,
                const QList<ConnectionTarget> &connectionPlan,
                const bool openInNewTab) {
                ++connectionRequestCount;
                requestedHostId = hostId;
                requestedJumpHostId = jumpHostId;
                requestedPlan = connectionPlan;
                requestedNewTab = openInNewTab;
            });

    dialog->show();
    dialog->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(dialog.get()));
    // 焦点停在搜索框时 Cmd+2 也必须生效：这是打开对话框后的默认状态。
    auto *searchEdit = dialog->findChild<QLineEdit *>(QStringLiteral("hostChainConnectSearchEdit"));
    QVERIFY(searchEdit != nullptr);
    QTRY_VERIFY(searchEdit->hasFocus());
    QTest::keyClick(searchEdit, Qt::Key_2, kCommandModifier);

    QTRY_COMPARE(connectionRequestCount, 1);
    QCOMPARE(dialog->result(), static_cast<int>(QDialog::Accepted));
    QCOMPARE(requestedHostId, target.id);
    QCOMPARE(requestedJumpHostId, jumps.at(3).id);
    QVERIFY2(requestedNewTab, "通过主机链连接必须像 Connect 一样新开标签页");
    QCOMPARE(requestedPlan.size(), 2);
    QCOMPARE(requestedPlan.at(0).host, jumps.at(3).host);
    QCOMPARE(requestedPlan.at(1).host, target.host);
}

void HostManagerWidgetTest::hostChainDialogShortcutsFollowVisibleRowsAfterSearch()
{
    const HostRecord target = saveChainTestHost(
        m_database.get(), QStringLiteral("filter-target"), QStringLiteral("过滤目标"),
        QStringLiteral("192.0.2.60"));
    const HostRecord alpha = saveChainTestHost(
        m_database.get(), QStringLiteral("filter-alpha"), QStringLiteral("alpha-bastion"),
        QStringLiteral("192.0.2.61"));
    saveChainTestHost(
        m_database.get(), QStringLiteral("filter-beta"), QStringLiteral("beta-bastion"),
        QStringLiteral("192.0.2.62"));
    const HostRecord gamma = saveChainTestHost(
        m_database.get(), QStringLiteral("filter-gamma"), QStringLiteral("gamma-relay"),
        QStringLiteral("192.0.2.63"));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    widget.recordSuccessfulChainConnection(target.id, alpha.id);

    std::unique_ptr<QDialog> dialog(widget.createHostChainConnectDialog(target.id));
    QVERIFY(dialog != nullptr);
    auto *recentList = dialog->findChild<QListWidget *>(QStringLiteral("hostChainConnectRecentList"));
    auto *hostList = dialog->findChild<QListWidget *>(QStringLiteral("hostChainConnectHostList"));
    auto *searchEdit = dialog->findChild<QLineEdit *>(QStringLiteral("hostChainConnectSearchEdit"));
    QVERIFY(recentList != nullptr && hostList != nullptr && searchEdit != nullptr);

    QSignalSpy requestSpy(&widget, &HostManagerWidget::hostChainConnectionRequested);
    dialog->show();
    dialog->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(dialog.get()));

    // 只剩 gamma 可见：它必须改绑为 Cmd+1，最近连接分区整体隐藏。
    searchEdit->setText(QStringLiteral("relay"));
    QVERIFY(recentList->isHidden());
    const QString cmd1 = QKeySequence(Qt::CTRL | Qt::Key_1).toString(QKeySequence::NativeText);
    for (int row = 0; row < hostList->count(); ++row) {
        QListWidgetItem *item = hostList->item(row);
        const bool isGamma = item->data(Qt::UserRole).toString() == gamma.id;
        QCOMPARE(item->isHidden(), !isGamma);
        QCOMPARE(item->data(Qt::UserRole + 2).toString(), isGamma ? cmd1 : QString());
    }

    // Cmd+2 已没有对应可见行，必须什么都不做，绝不能连到被隐藏的 beta。
    QTest::keyClick(searchEdit, Qt::Key_2, kCommandModifier);
    QTest::qWait(50);
    QCOMPARE(requestSpy.count(), 0);
    QVERIFY(dialog->isVisible());

    QTest::keyClick(searchEdit, Qt::Key_1, kCommandModifier);
    QTRY_COMPARE(requestSpy.count(), 1);
    QCOMPARE(requestSpy.at(0).at(1).toString(), gamma.id);
}

void HostManagerWidgetTest::hostChainDialogReadsLegacyLastJumpSetting()
{
    const HostRecord target = saveChainTestHost(
        m_database.get(), QStringLiteral("legacy-target"), QStringLiteral("旧版目标"),
        QStringLiteral("192.0.2.70"));
    const HostRecord jump = saveChainTestHost(
        m_database.get(), QStringLiteral("legacy-jump"), QStringLiteral("旧版跳板"),
        QStringLiteral("192.0.2.71"));
    saveChainTestHost(m_database.get(), QStringLiteral("legacy-other"), QStringLiteral("其他主机"),
                      QStringLiteral("192.0.2.72"));
    QVERIFY(m_database->setSetting(
        QStringLiteral("ui/host_chain_last_jump_v1"),
        QStringLiteral(R"({"version":1,"jumps":{"legacy-target":"legacy-jump"}})")));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    std::unique_ptr<QDialog> dialog(widget.createHostChainConnectDialog(target.id));
    QVERIFY(dialog != nullptr);
    auto *recentList = dialog->findChild<QListWidget *>(QStringLiteral("hostChainConnectRecentList"));
    QVERIFY(recentList != nullptr);
    QCOMPARE(listHostIds(recentList), QStringList({jump.id}));
}

void HostManagerWidgetTest::collectHostsInGroupRecursesNestedFolders()
{
    HostRecord topGroup;
    topGroup.id = QStringLiteral("nested-top-group");
    topGroup.kind = HostNodeKind::Folder;
    topGroup.name = QStringLiteral("顶层分组");
    QString error;
    QVERIFY2(m_database->saveHost(topGroup, &error), qPrintable(error));

    HostRecord childGroup;
    childGroup.id = QStringLiteral("nested-child-group");
    childGroup.parentId = topGroup.id;
    childGroup.kind = HostNodeKind::Folder;
    childGroup.name = QStringLiteral("子分组");
    QVERIFY2(m_database->saveHost(childGroup, &error), qPrintable(error));

    HostRecord directHost;
    directHost.id = QStringLiteral("nested-direct-host");
    directHost.parentId = topGroup.id;
    directHost.kind = HostNodeKind::Host;
    directHost.name = QStringLiteral("直属主机");
    directHost.host = QStringLiteral("192.0.2.30");
    QVERIFY2(m_database->saveHost(directHost, &error), qPrintable(error));

    HostRecord nestedHost;
    nestedHost.id = QStringLiteral("nested-child-host");
    nestedHost.parentId = childGroup.id;
    nestedHost.kind = HostNodeKind::Host;
    nestedHost.name = QStringLiteral("子分组主机");
    nestedHost.host = QStringLiteral("192.0.2.31");
    QVERIFY2(m_database->saveHost(nestedHost, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    QList<HostRecord> hosts;
    QVERIFY2(widget.collectHostsInGroup(topGroup.id, &hosts, &error), qPrintable(error));

    QStringList hostIds;
    for (const HostRecord &host : hosts) {
        QCOMPARE(static_cast<int>(host.kind), static_cast<int>(HostNodeKind::Host));
        hostIds.append(host.id);
    }
    hostIds.sort();
    QCOMPARE(hostIds, (QStringList{nestedHost.id, directHost.id}));
}

void HostManagerWidgetTest::copyHostKeepsConfigurationInSameGroup()
{
    HostRecord group;
    group.id = QStringLiteral("copy-group");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("复制测试分组");
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostRecord jumpHost;
    jumpHost.id = QStringLiteral("copy-jump-host");
    jumpHost.parentId = group.id;
    jumpHost.kind = HostNodeKind::Host;
    jumpHost.name = QStringLiteral("复制测试跳板机");
    jumpHost.host = QStringLiteral("198.51.100.10");
    QVERIFY2(m_database->saveHost(jumpHost, &error), qPrintable(error));

    HostRecord source;
    source.id = QStringLiteral("copy-source-host");
    source.parentId = group.id;
    source.kind = HostNodeKind::Host;
    source.name = QStringLiteral("生产主机");
    source.host = QStringLiteral("10.20.30.40");
    source.port = quint16{2202};
    source.username = QStringLiteral("deploy");
    source.identityFile = QStringLiteral("/keys/production_ed25519");
    source.proxyJumps = {QStringLiteral("legacy-bastion")};
    source.hostChain = {jumpHost.id};
    source.tags = {QStringLiteral("生产"), QStringLiteral("核心")};
    source.allowLegacyHmacSha1 = true;
    source.sortOrder = 7;
    source.createdAt = QDateTime::fromString(
        QStringLiteral("2024-01-02T03:04:05Z"), Qt::ISODate);
    source.updatedAt = source.createdAt;
    ConnectionTarget sourceTarget = source.toConnectionTarget();
    sourceTarget.password = QStringLiteral("copy-test-secret");
    QVERIFY2(m_vault->saveConnectionPassword(source, sourceTarget, {}, &error),
             qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    auto *tree = widget.findChild<QTreeWidget *>();
    QVERIFY(tree != nullptr);
    QTreeWidgetItem *sourceItem = findItem(widget, source.name);
    QVERIFY2(sourceItem != nullptr, "待复制主机节点未出现在树中");
    tree->setCurrentItem(sourceItem);

    // HostManagerWidgetTest 是生产类声明的友元，可直接触发私有业务入口，避免测试依赖菜单
    // 弹出和鼠标坐标；菜单是否接到该入口由上一条用例独立验证。
    widget.copySelectedHost();

    // 第一次刷新会重建树节点并清除选择，模拟用户再次右键原主机时必须重新选中新的节点。
    // 第二次复制应检查当前分组已经存在的“-1”，继而使用“-2”，而不是生成重名记录。
    sourceItem = findItem(widget, source.name);
    QVERIFY2(sourceItem != nullptr, "第一次复制后原主机节点丢失");
    tree->setCurrentItem(sourceItem);
    widget.copySelectedHost();

    bool ok = false;
    const QList<HostRecord> children = m_database->listHostsInGroup(group.id, &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    const auto copiedIterator = std::find_if(
        children.cbegin(), children.cend(), [](const HostRecord &record) {
            return record.name == QStringLiteral("生产主机-1");
        });
    QVERIFY2(copiedIterator != children.cend(), "当前分组中没有找到复制出的主机");
    const HostRecord &copied = *copiedIterator;
    const auto secondCopyIterator = std::find_if(
        children.cbegin(), children.cend(), [](const HostRecord &record) {
            return record.name == QStringLiteral("生产主机-2");
        });
    QVERIFY2(secondCopyIterator != children.cend(), "第二次复制没有生成名称后缀-2");

    QVERIFY(copied.id != source.id);
    QCOMPARE(copied.parentId, source.parentId);
    QCOMPARE(copied.kind, source.kind);
    QCOMPARE(copied.host, source.host);
    QCOMPARE(copied.port, source.port);
    QCOMPARE(copied.username, source.username);
    QVERIFY2(!copied.credentialId.isEmpty(), "复制主机没有克隆源凭据");
    QVERIFY2(copied.credentialId != source.credentialId,
             "复制主机仍与原主机共享 credentialId");
    QVERIFY2(secondCopyIterator->credentialId != source.credentialId
                 && secondCopyIterator->credentialId != copied.credentialId,
             "多个主机副本没有各自获得独立凭据");
    const std::optional<CredentialRecord> sourceCredential =
        m_database->findCredential(source.credentialId, &error);
    const std::optional<CredentialRecord> copiedCredential =
        m_database->findCredential(copied.credentialId, &error);
    QVERIFY2(sourceCredential.has_value() && copiedCredential.has_value(), qPrintable(error));
    QCOMPARE(copiedCredential->encryptedPayload, sourceCredential->encryptedPayload);
    QCOMPARE(copiedCredential->nonce, sourceCredential->nonce);
    QCOMPARE(copied.identityFile, source.identityFile);
    QCOMPARE(copied.proxyJumps, source.proxyJumps);
    QCOMPARE(copied.hostChain, source.hostChain);
    QCOMPARE(copied.tags, source.tags);
    QCOMPARE(copied.allowLegacyHmacSha1, source.allowLegacyHmacSha1);
    QCOMPARE(copied.sortOrder, source.sortOrder);
    QVERIFY2(copied.createdAt.isValid(), "复制记录没有生成新的创建时间");
    QVERIFY2(copied.updatedAt.isValid(), "复制记录没有生成新的更新时间");
    QVERIFY2(copied.createdAt != source.createdAt, "复制记录错误沿用了原主机创建时间");
}

namespace {

/** @brief 递归收集某个布局（含嵌套子布局）直接管理的全部控件。 */
void collectLayoutWidgets(QLayout *layout, QSet<QWidget *> *managed)
{
    if (layout == nullptr) {
        return;
    }
    for (int index = 0; index < layout->count(); ++index) {
        QLayoutItem *item = layout->itemAt(index);
        if (QWidget *widget = item->widget()) {
            managed->insert(widget);
        }
        collectLayoutWidgets(item->layout(), managed);
    }
}

} // namespace

void HostManagerWidgetTest::groupDialogHasNoStrayWidgets()
{
    HostManagerWidget widget(m_database.get(), m_vault.get());

    // 编辑对话框是 HostManagerWidget.cpp 内部类型，只能经"新建分组"动作弹出；它是模态的，
    // 所以在事件循环里取当前模态窗口来检查，再关掉它让 addGroup() 返回。
    QAbstractButton *addGroupButton = nullptr;
    for (QAbstractButton *button : widget.headerActionsWidget()->findChildren<QAbstractButton *>()) {
        if (button->text() == QStringLiteral("New group")) {
            addGroupButton = button;
        }
    }
    QVERIFY2(addGroupButton != nullptr, "侧栏没有新建分组按钮");

    QString failure;
    QTimer::singleShot(0, [&failure]() {
        QWidget *dialog = QApplication::activeModalWidget();
        if (dialog == nullptr) {
            failure = QStringLiteral("分组配置对话框未能弹出");
            return;
        }
        QSet<QWidget *> managed;
        collectLayoutWidgets(dialog->layout(), &managed);
        // QFocusFrame（macOS 焦点环）和 QSizeGrip 由 Qt 自行创建并围绕目标控件浮动定位，
        // 本就不该进入布局；把它们计入会让本用例随焦点时序随机失败。
        static const QSet<QString> qtFloatingHelpers = {
            QStringLiteral("QFocusFrame"), QStringLiteral("QSizeGrip")};
        for (QWidget *child : dialog->findChildren<QWidget *>(Qt::FindDirectChildrenOnly)) {
            const QString className = QString::fromLatin1(child->metaObject()->className());
            // 显式隐藏的控件不绘制、也不参与 Tab 顺序，是可接受的"备用控件"形态。
            if (child->isHidden() || managed.contains(child)
                || qtFloatingHelpers.contains(className)) {
                continue;
            }
            failure = QStringLiteral("控件 %1 可见但不在任何布局中，会浮在分组配置窗口左上角")
                          .arg(className);
            break;
        }

        // “只保留分组名称”不仅意味着其他字段不可见，也意味着它们不应继续以隐藏输入框
        // 潜伏在对象树中；否则辅助技术和后续维护仍可能误把它们当成可用配置项。
        const QList<QLineEdit *> lineEdits = dialog->findChildren<QLineEdit *>();
        if (failure.isEmpty()
            && (lineEdits.size() != 1
                || lineEdits.constFirst()->objectName()
                    != QStringLiteral("hostRecordNameEdit"))) {
            failure = QStringLiteral("分组配置窗口没有严格只保留名称输入框");
        }

        // 圆角和卡片底色由 QFrame#SettingsCard 样式选择器提供；业务代码若为了定位控件
        // 覆盖 objectName，字段虽然还在，承载它的圆角区域却会在视觉上完全消失。
        const QList<QWidget *> cards = dialog->findChildren<QWidget *>(
            QStringLiteral("SettingsCard"));
        if (failure.isEmpty() && cards.size() != 1) {
            failure = QStringLiteral("分组名称外层的圆角卡片区域未保留");
        }

        const QList<QPushButton *> buttons = dialog->findChildren<QPushButton *>();
        if (failure.isEmpty() && buttons.size() != 2) {
            failure = QStringLiteral("分组配置窗口应只保留取消和保存两个按钮");
        }
        for (const QPushButton *button : buttons) {
            if (failure.isEmpty() && button->icon().isNull()) {
                failure = QStringLiteral("分组配置按钮“%1”缺少图标").arg(button->text());
            }
        }
        dialog->close();
    });

    // 分组窗口只创建名称和页脚动作，任何主机地址、认证或跳板控件都不应存在。
    addGroupButton->click();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
}

void HostManagerWidgetTest::hostDialogUsesRequestedFieldOrderAndIcons()
{
    HostManagerWidget widget(m_database.get(), m_vault.get());

    QAbstractButton *addHostButton = nullptr;
    for (QAbstractButton *button : widget.headerActionsWidget()->findChildren<QAbstractButton *>()) {
        if (button->text() == QStringLiteral("New host")) {
            addHostButton = button;
            break;
        }
    }
    QVERIFY2(addHostButton != nullptr, "侧栏没有新建主机按钮");

    QString failure;
    QTimer::singleShot(0, [&failure]() {
        QWidget *dialog = QApplication::activeModalWidget();
        if (dialog == nullptr) {
            failure = QStringLiteral("主机配置对话框未能弹出");
            return;
        }

        QStringList labelTexts;
        for (const QLabel *label : dialog->findChildren<QLabel *>()) {
            labelTexts.append(label->text());
        }
        // 已移除项必须连标签都不存在；只把控件藏起来仍会让可访问性树和布局保留旧语义。
        for (const QString &removedLabel : {QStringLiteral("继承父分组端口"),
                                            QStringLiteral("标签"),
                                            QStringLiteral("会话密码/口令")}) {
            if (labelTexts.contains(removedLabel)) {
                failure = QStringLiteral("主机配置仍包含已移除字段“%1”").arg(removedLabel);
                break;
            }
        }

        const QList<QWidget *> cards = dialog->findChildren<QWidget *>(
            QStringLiteral("SettingsCard"));
        if (failure.isEmpty() && cards.size() != 2) {
            failure = QStringLiteral("主机基础信息和认证信息的圆角卡片区域未完整保留");
        }

        const auto *username = dialog->findChild<QLineEdit *>(
            QStringLiteral("hostUsernameEdit"));
        const auto *password = dialog->findChild<QLineEdit *>(
            QStringLiteral("hostPasswordEdit"));
        auto *passwordVisibility = dialog->findChild<QAction *>(
            QStringLiteral("hostPasswordVisibilityAction"));
        const auto *identity = dialog->findChild<QLineEdit *>(
            QStringLiteral("hostIdentityEdit"));
        const auto *proxyJumps = dialog->findChild<QLineEdit *>(
            QStringLiteral("hostProxyJumpsEdit"));
        auto *advancedSettings = dialog->findChild<QToolButton *>(
            QStringLiteral("hostAdvancedSettingsButton"));
        if (failure.isEmpty()
            && (username == nullptr || password == nullptr || passwordVisibility == nullptr
                || identity == nullptr || proxyJumps == nullptr || advancedSettings == nullptr)) {
            failure = QStringLiteral("主机配置缺少认证与跳板字段");
        }
        if (failure.isEmpty()
            && (password->echoMode() != QLineEdit::Password
                || !passwordVisibility->isEnabled())) {
            failure = QStringLiteral("未启用主密码保护时，密码未保持掩码或眼睛图标无法点击提示");
        }
        if (failure.isEmpty()
            && (!password->actions().contains(passwordVisibility)
                || dialog->findChild<QPushButton *>(
                       QStringLiteral("hostPasswordVisibilityButton")) != nullptr)) {
            failure = QStringLiteral("查看密码入口没有内嵌在密码框右侧，或仍残留独立按钮");
        }
        if (failure.isEmpty()) {
            bool guidanceShown = false;
            // 信息提示使用模态消息框；预先安排关闭动作，既能验证用户确实得到开启路径，也
            // 不让自动化用例阻塞在消息框自己的事件循环中。
            QTimer::singleShot(0, [&guidanceShown]() {
                auto *messageBox = qobject_cast<QMessageBox *>(
                    QApplication::activeModalWidget());
                if (messageBox != nullptr
                    && messageBox->text().contains(QStringLiteral("master password protection"))) {
                    guidanceShown = true;
                    messageBox->accept();
                }
            });
            passwordVisibility->trigger();
            if (!guidanceShown || password->echoMode() != QLineEdit::Password
                || passwordVisibility->isChecked()) {
                failure = QStringLiteral(
                    "未启用主密码保护时点击眼睛图标没有提示开启，或意外显示了明文");
            }
        }
        if (failure.isEmpty()) {
            const int usernameTop = username->mapTo(dialog, QPoint{}).y();
            const int passwordTop = password->mapTo(dialog, QPoint{}).y();
            const int identityTop = identity->mapTo(dialog, QPoint{}).y();
            const int advancedTop = advancedSettings->mapTo(dialog, QPoint{}).y();
            if (!(usernameTop < passwordTop && passwordTop < identityTop
                  && identityTop < advancedTop)) {
                failure = QStringLiteral(
                    "认证字段未按用户名、密码、私钥文件、高级设置的顺序排列");
            }
        }
        // 全新主机不应直接暴露自由文本 ProxyJump；点击高级入口后必须显示原输入框，确保
        // 兼容能力只是降低视觉优先级而非被删除。
        if (failure.isEmpty() && proxyJumps->isVisible()) {
            failure = QStringLiteral("全新主机默认展开了 ProxyJump 高级字段");
        }
        if (failure.isEmpty()) {
            advancedSettings->click();
            if (!proxyJumps->isVisible()
                || advancedSettings->arrowType() != Qt::DownArrow) {
                failure = QStringLiteral("ProxyJump 高级入口无法正常展开");
            }
        }

        const auto *testButton = dialog->findChild<QPushButton *>(
            QStringLiteral("hostTestConnectionButton"));
        const auto *cancelButton = dialog->findChild<QPushButton *>(
            QStringLiteral("hostRecordCancelButton"));
        const auto *saveButton = dialog->findChild<QPushButton *>(
            QStringLiteral("hostRecordSaveButton"));
        if (failure.isEmpty()
            && (testButton == nullptr || cancelButton == nullptr || saveButton == nullptr)) {
            failure = QStringLiteral("主机配置页脚缺少测试连接、取消或保存按钮");
        }
        if (failure.isEmpty()
            && testButton->mapTo(dialog, QPoint{}).x()
                >= cancelButton->mapTo(dialog, QPoint{}).x()) {
            failure = QStringLiteral("测试连接按钮没有位于页脚左侧");
        }

        // 这里检查所有可见文字按钮，覆盖浏览、测试连接、取消与保存；步进器的加减号本身
        // 就是约定俗成的图形符号，不属于需要额外前置图标的文字按钮。
        for (const QPushButton *button : dialog->findChildren<QPushButton *>()) {
            if (failure.isEmpty() && button->isVisible() && button->icon().isNull()) {
                failure = QStringLiteral("主机配置按钮“%1”缺少图标").arg(button->text());
            }
        }
        dialog->close();
    });

    addHostButton->click();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
}

void HostManagerWidgetTest::newHostFocusesAddressFieldAndDropsInheritanceCopy()
{
    HostManagerWidget widget(m_database.get(), m_vault.get());

    QAbstractButton *addHostButton = nullptr;
    for (QAbstractButton *button : widget.headerActionsWidget()->findChildren<QAbstractButton *>()) {
        if (button->text() == QStringLiteral("New host")) {
            addHostButton = button;
            break;
        }
    }
    QVERIFY2(addHostButton != nullptr, "侧栏没有新建主机按钮");

    QString failure;
    QTimer::singleShot(0, [&failure]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            failure = QStringLiteral("新建主机窗口未打开");
            return;
        }

        auto *address = dialog->findChild<QLineEdit *>(QStringLiteral("hostAddressEdit"));
        if (address == nullptr) {
            failure = QStringLiteral("主机配置缺少主机地址输入框");
        } else if (dialog->focusWidget() != address) {
            failure = QStringLiteral("新增主机时初始焦点没有落在主机地址输入框");
        }

        // 「继承」这一概念已从界面移除：任何占位符、说明标签或摘要都不得再提及。
        if (failure.isEmpty()) {
            for (const QLineEdit *edit : dialog->findChildren<QLineEdit *>()) {
                if (edit->placeholderText().contains(QStringLiteral("继承"))
                    || edit->placeholderText().contains(QStringLiteral("繼承"))
                    || edit->placeholderText().contains(QStringLiteral("inherit"), Qt::CaseInsensitive)) {
                    failure = QStringLiteral("占位符仍提及继承：%1").arg(edit->placeholderText());
                    break;
                }
            }
        }
        if (failure.isEmpty()) {
            for (const QLabel *label : dialog->findChildren<QLabel *>()) {
                if (label->text().contains(QStringLiteral("继承"))
                    || label->text().contains(QStringLiteral("繼承"))
                    || label->text().contains(QStringLiteral("inherit"), Qt::CaseInsensitive)) {
                    failure = QStringLiteral("说明文字仍提及继承：%1").arg(label->text());
                    break;
                }
            }
        }

        dialog->close();
    });

    addHostButton->click();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
}

void HostManagerWidgetTest::newHostAutomaticallyPersistsPasswordAndIdentityFile()
{
    HostManagerWidget widget(m_database.get(), m_vault.get());
    QString failure;
    QTimer::singleShot(0, [&failure]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            failure = QStringLiteral("新建主机窗口未打开");
            return;
        }
        auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("hostRecordNameEdit"));
        auto *address = dialog->findChild<QLineEdit *>(QStringLiteral("hostAddressEdit"));
        auto *password = dialog->findChild<QLineEdit *>(QStringLiteral("hostPasswordEdit"));
        auto *identity = dialog->findChild<QLineEdit *>(QStringLiteral("hostIdentityEdit"));
        auto *save = dialog->findChild<QPushButton *>(QStringLiteral("hostRecordSaveButton"));
        if (name == nullptr || address == nullptr || password == nullptr
            || identity == nullptr || save == nullptr) {
            failure = QStringLiteral("新建主机窗口缺少自动保存所需字段");
            dialog->reject();
            return;
        }
        address->setText(QStringLiteral("192.0.2.20"));
        // 地址会驱动空名称自动生成，因此最后写名称，模拟用户确认自动名称后再改成业务名。
        name->setText(QStringLiteral("自动保存主机"));
        password->setText(QStringLiteral("auto-saved-secret"));
        identity->setText(QStringLiteral("/tmp/id_ed25519_for_test"));
        save->click();
    });
    widget.addHost();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));

    bool ok = false;
    QString error;
    const QList<HostRecord> records =
        m_database->searchHosts(QStringLiteral("自动保存主机"), &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(records.size(), 1);
    const HostRecord saved = records.constFirst();
    QCOMPARE(saved.identityFile, QStringLiteral("/tmp/id_ed25519_for_test"));
    QVERIFY2(!saved.credentialId.isEmpty(), "密码未自动生成凭据引用");
    const std::optional<QByteArray> secret = m_vault->loadSecret(saved.credentialId, &error);
    QVERIFY2(secret.has_value(), qPrintable(error));
    QCOMPARE(QString::fromUtf8(*secret), QStringLiteral("auto-saved-secret"));
}

void HostManagerWidgetTest::editHostMasksPasswordAndGatesVisibility()
{
    QString error;
    QVERIFY2(m_vault->initialize(QStringLiteral("用于界面测试的主密码"), &error),
             qPrintable(error));

    HostRecord host;
    host.id = QStringLiteral("masked-password-host");
    host.kind = HostNodeKind::Host;
    host.name = QStringLiteral("掩码测试主机");
    host.host = QStringLiteral("192.0.2.30");
    ConnectionTarget target = host.toConnectionTarget();
    target.password = QStringLiteral("visible-after-authorization");
    QVERIFY2(m_vault->saveConnectionPassword(host, target, {}, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    QTreeWidget *tree = widget.findChild<QTreeWidget *>();
    QVERIFY(tree != nullptr);
    QTreeWidgetItem *item = findItem(widget, host.name);
    QVERIFY(item != nullptr);
    tree->setCurrentItem(item);

    QString failure;
    QTimer::singleShot(0, [&failure]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        auto *password = dialog == nullptr ? nullptr : dialog->findChild<QLineEdit *>(
            QStringLiteral("hostPasswordEdit"));
        auto *visibility = dialog == nullptr ? nullptr : dialog->findChild<QAction *>(
            QStringLiteral("hostPasswordVisibilityAction"));
        if (password == nullptr || visibility == nullptr) {
            failure = QStringLiteral("编辑主机窗口缺少密码框或右侧眼睛图标");
        } else if (password->text() != QStringLiteral("visible-after-authorization")
                   || password->echoMode() != QLineEdit::Password
                   || !visibility->isEnabled()) {
            failure = QStringLiteral("已保存密码未以可授权查看的掩码状态加载");
        } else {
            visibility->trigger();
            if (password->echoMode() != QLineEdit::Normal) {
                failure = QStringLiteral("启用主密码保护后眼睛图标未显示明文");
            } else {
                visibility->trigger();
                if (password->echoMode() != QLineEdit::Password) {
                    failure = QStringLiteral("再次点击眼睛图标后密码未恢复掩码");
                }
            }
        }
        if (dialog != nullptr) {
            dialog->reject();
        }
    });
    widget.editSelected();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
}

void HostManagerWidgetTest::selectionSurvivesHostEditAndManualRefresh()
{
    HostRecord group;
    group.id = QStringLiteral("selection-group");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("选中状态测试分组");
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostRecord host;
    host.id = QStringLiteral("selection-host-stable-id");
    host.parentId = group.id;
    host.kind = HostNodeKind::Host;
    host.name = QStringLiteral("修改前的主机名称");
    host.host = QStringLiteral("192.0.2.81");
    QVERIFY2(m_database->saveHost(host, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    auto *tree = widget.findChild<QTreeWidget *>();
    QTreeWidgetItem *originalItem = findItem(widget, host.name);
    QVERIFY(tree != nullptr);
    QVERIFY(originalItem != nullptr);
    tree->setCurrentItem(originalItem);
    QVERIFY(originalItem->isSelected());

    QString dialogFailure;
    const QString updatedName = QStringLiteral("修改后的主机名称");
    QTimer::singleShot(0, [&dialogFailure, &updatedName]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        auto *nameEdit = dialog == nullptr
            ? nullptr
            : dialog->findChild<QLineEdit *>(QStringLiteral("hostRecordNameEdit"));
        auto *saveButton = dialog == nullptr
            ? nullptr
            : dialog->findChild<QPushButton *>(QStringLiteral("hostRecordSaveButton"));
        if (dialog == nullptr || nameEdit == nullptr || saveButton == nullptr) {
            dialogFailure = QStringLiteral("主机配置窗口缺少名称输入框或保存按钮");
            if (dialog != nullptr) {
                dialog->reject();
            }
            return;
        }

        // 名称变化用于证明恢复逻辑依赖稳定 id，而不是恰好用旧显示文本重新匹配成功。
        nameEdit->setText(updatedName);
        saveButton->click();
    });
    widget.editSelected();
    QVERIFY2(dialogFailure.isEmpty(), qPrintable(dialogFailure));

    QTreeWidgetItem *itemAfterSave = findItem(widget, updatedName);
    QVERIFY2(itemAfterSave != nullptr, "编辑保存后未找到改名后的主机节点");
    QCOMPARE(itemAfterSave->data(0, Qt::UserRole).toString(), host.id);
    QCOMPARE(tree->currentItem(), itemAfterSave);
    QVERIFY2(itemAfterSave->isSelected(), "编辑保存重建主机树后选中高亮丢失");
    QVERIFY2(itemAfterSave->parent() != nullptr && itemAfterSave->parent()->isExpanded(),
             "恢复选择后主机所在分组没有保持展开");

    // 再执行一次用户点击刷新按钮所走的同一公开入口，确保选择恢复不是编辑流程的偶然效果。
    widget.refresh();
    QTreeWidgetItem *itemAfterManualRefresh = findItem(widget, updatedName);
    QVERIFY2(itemAfterManualRefresh != nullptr, "手动刷新后主机节点消失");
    QCOMPARE(itemAfterManualRefresh->data(0, Qt::UserRole).toString(), host.id);
    QCOMPARE(tree->currentItem(), itemAfterManualRefresh);
    QVERIFY2(itemAfterManualRefresh->isSelected(), "手动刷新主机列表后选中高亮丢失");
}

void HostManagerWidgetTest::folderExpansionStatePersistsAcrossRefreshAndRestart()
{
    HostRecord group;
    group.id = QStringLiteral("remembered-expansion-group-id");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("待记忆展开状态的目录");
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostRecord child;
    child.id = QStringLiteral("remembered-expansion-child-id");
    child.parentId = group.id;
    child.kind = HostNodeKind::Host;
    child.name = QStringLiteral("用于显示展开箭头的子主机");
    child.host = QStringLiteral("192.0.2.91");
    QVERIFY2(m_database->saveHost(child, &error), qPrintable(error));

    {
        HostManagerWidget widget(m_database.get(), m_vault.get());
        QTreeWidgetItem *groupItem = findItem(widget, group.name);
        QVERIFY(groupItem != nullptr);
        QVERIFY2(groupItem->isExpanded(), "从未保存状态的新目录没有沿用默认展开行为");

        // setExpanded() 与鼠标、键盘入口最终发出相同的 itemCollapsed 信号。直接调用可以稳定
        // 验证业务监听与同步写库，不让测试依赖某个平台的箭头点击几何。
        groupItem->setExpanded(false);
        QVERIFY2(!groupItem->isExpanded(), "测试前置操作未能折叠目录");

        const std::optional<QString> storedState = m_database->setting(
            QStringLiteral("ui/host_tree_folder_expansion_v1"), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(storedState.has_value(), "折叠目录后没有立即创建持久化设置");
        QVERIFY2(storedState->contains(group.id), "持久化设置没有使用目录稳定 id");

        // 普通刷新会销毁全部 QTreeWidgetItem；新节点仍应按内存快照恢复为折叠状态。
        widget.refresh();
        groupItem = findItem(widget, group.name);
        QVERIFY(groupItem != nullptr);
        QVERIFY2(!groupItem->isExpanded(), "刷新主机树后目录折叠状态丢失");
    }

    // 模拟应用退出后目录改名：状态键必须依赖稳定 id，不能依赖已经变化的显示名称。
    group.name = QStringLiteral("改名后仍应保持折叠的目录");
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));
    {
        HostManagerWidget restartedWidget(m_database.get(), m_vault.get());
        QTreeWidgetItem *groupItem = findItem(restartedWidget, group.name);
        QVERIFY(groupItem != nullptr);
        QVERIFY2(!groupItem->isExpanded(), "重新启动或目录改名后没有恢复折叠状态");

        // 再次展开也必须立即覆盖之前的 false，确保记忆功能不是只能保存折叠的单向状态。
        groupItem->setExpanded(true);
        QVERIFY2(groupItem->isExpanded(), "测试前置操作未能重新展开目录");
    }

    {
        HostManagerWidget restartedAgainWidget(m_database.get(), m_vault.get());
        QTreeWidgetItem *groupItem = findItem(restartedAgainWidget, group.name);
        QVERIFY(groupItem != nullptr);
        QVERIFY2(groupItem->isExpanded(), "重新启动后没有恢复最近一次展开状态");
    }
}

void HostManagerWidgetTest::newlySavedHostRequestsConnectionInNewTab()
{
    HostManagerWidget widget(m_database.get(), m_vault.get());

    QAbstractButton *addHostButton = nullptr;
    for (QAbstractButton *button : widget.headerActionsWidget()->findChildren<QAbstractButton *>()) {
        if (button->text() == QStringLiteral("New host")) {
            addHostButton = button;
            break;
        }
    }
    QVERIFY2(addHostButton != nullptr, "侧栏没有新建主机按钮");

    int connectionRequestCount = 0;
    QString requestedHostId;
    QList<ConnectionTarget> requestedPlan;
    bool requestedNewTab = false;
    connect(&widget,
            &HostManagerWidget::hostConnectionRequested,
            this,
            [&](const QString &hostId,
                const QList<ConnectionTarget> &connectionPlan,
                const bool openInNewTab) {
                ++connectionRequestCount;
                requestedHostId = hostId;
                requestedPlan = connectionPlan;
                requestedNewTab = openInNewTab;
            });

    QString failure;
    QTimer::singleShot(0, [&failure]() {
        QWidget *dialog = QApplication::activeModalWidget();
        if (dialog == nullptr) {
            failure = QStringLiteral("主机配置对话框未能弹出");
            return;
        }

        auto *hostAddress = dialog->findChild<QLineEdit *>(
            QStringLiteral("hostAddressEdit"));
        auto *saveButton = dialog->findChild<QPushButton *>(
            QStringLiteral("hostRecordSaveButton"));
        if (hostAddress == nullptr || saveButton == nullptr) {
            failure = QStringLiteral("主机配置对话框缺少地址或保存控件");
            dialog->close();
            return;
        }

        // 输入地址会同步生成名称，使用文档专用地址避免测试数据被误解为真实可连主机。
        hostAddress->setText(QStringLiteral("192.0.2.25"));
        saveButton->click();
    });

    // addHost() 内部使用模态对话框；定时器在对话框事件循环中填写并保存，
    // 返回后即可同步验证完整的“落库 → 请求连接”业务链路。
    addHostButton->click();

    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QCOMPARE(connectionRequestCount, 1);
    QVERIFY2(requestedNewTab, "新建主机保存后的自动连接仍在复用当前会话标签");
    QVERIFY2(!requestedHostId.isEmpty(), "连接请求没有携带已保存主机的稳定 id");
    QCOMPARE(requestedPlan.size(), 1);
    QCOMPARE(requestedPlan.constFirst().host, QStringLiteral("192.0.2.25"));
}

void HostManagerWidgetTest::configuredProxyJumpRemainsVisibleWhenEditing()
{
    QString error;
    HostRecord host;
    host.id = QStringLiteral("manual-proxy-host");
    host.kind = HostNodeKind::Host;
    host.name = QStringLiteral("兼容跳板目标");
    host.host = QStringLiteral("private.example.com");
    host.proxyJumps = {QStringLiteral("ops@edge.example.com:2200")};
    QVERIFY2(m_database->saveHost(host, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    auto *tree = widget.findChild<QTreeWidget *>();
    QTreeWidgetItem *hostItem = findItem(widget, host.name);
    QVERIFY(tree != nullptr);
    QVERIFY(hostItem != nullptr);
    tree->setCurrentItem(hostItem);

    QString failure;
    QTimer::singleShot(0, [&failure]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        auto *advancedSettings = dialog == nullptr
            ? nullptr
            : dialog->findChild<QToolButton *>(
                  QStringLiteral("hostAdvancedSettingsButton"));
        auto *proxyJumps = dialog == nullptr
            ? nullptr
            : dialog->findChild<QLineEdit *>(QStringLiteral("hostProxyJumpsEdit"));
        if (dialog == nullptr || advancedSettings == nullptr || proxyJumps == nullptr) {
            failure = QStringLiteral("已有主机的 ProxyJump 高级控件不完整");
        } else if (!advancedSettings->isChecked() || !proxyJumps->isVisible()
                   || !advancedSettings->text().contains(QStringLiteral("configured"))) {
            // 既有路由必须在打开窗口时主动显露；否则用户可能误以为当前目标是直连并保存
            // 其他改动，给连接排障和安全审计带来歧义。
            failure = QStringLiteral("已有 ProxyJump 未自动展开或缺少已配置提示");
        }
        if (dialog != nullptr) {
            dialog->reject();
        }
    });

    widget.editSelected();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
}

void HostManagerWidgetTest::emptyHostNameFollowsConnectionFieldsUntilManuallyEdited()
{
    HostManagerWidget widget(m_database.get(), m_vault.get());

    QAbstractButton *addHostButton = nullptr;
    for (QAbstractButton *button : widget.headerActionsWidget()->findChildren<QAbstractButton *>()) {
        if (button->text() == QStringLiteral("New host")) {
            addHostButton = button;
            break;
        }
    }
    QVERIFY2(addHostButton != nullptr, "侧栏没有新建主机按钮");

    QString failure;
    QTimer::singleShot(0, [&failure]() {
        QWidget *dialog = QApplication::activeModalWidget();
        if (dialog == nullptr) {
            failure = QStringLiteral("主机配置对话框未能弹出");
            return;
        }

        auto *nameEdit = dialog->findChild<QLineEdit *>(
            QStringLiteral("hostRecordNameEdit"));
        auto *hostEdit = dialog->findChild<QLineEdit *>(
            QStringLiteral("hostAddressEdit"));
        auto *usernameEdit = dialog->findChild<QLineEdit *>(
            QStringLiteral("hostUsernameEdit"));
        auto *portSpin = dialog->findChild<QSpinBox *>(
            QStringLiteral("hostPortSpin"));
        if (nameEdit == nullptr || hostEdit == nullptr
            || usernameEdit == nullptr || portSpin == nullptr) {
            failure = QStringLiteral("主机配置缺少自动命名所需的输入控件");
            dialog->close();
            return;
        }

        // 用户通常先填地址、后填认证信息。地址输入期间名称必须持续同步完整文本，不能在
        // 第一个字符触发 setText() 后便把自动生成内容误当成人工名称而停止更新。
        QTest::keyClicks(hostEdit, "server.example.com");
        if (nameEdit->text() != QStringLiteral("server.example.com:22")) {
            failure = QStringLiteral("未按主机地址和默认端口自动生成名称");
        }

        if (failure.isEmpty()) {
            QTest::keyClicks(usernameEdit, "deploy");
            if (nameEdit->text() != QStringLiteral("deploy@server.example.com:22")) {
                failure = QStringLiteral("用户名变化后自动名称未同步");
            }
        }

        if (failure.isEmpty()) {
            portSpin->setValue(2222);
            if (nameEdit->text() != QStringLiteral("deploy@server.example.com:2222")) {
                failure = QStringLiteral("端口变化后自动名称未同步");
            }
        }

        if (failure.isEmpty()) {
            // 通过真实键盘事件产生 textEdited，验证业务名称一经人工指定便不会被后续连接参数
            // 覆盖；直接调用 setText() 不代表用户操作，无法覆盖这条交互边界。
            nameEdit->selectAll();
            QTest::keyClicks(nameEdit, "Production API");
            hostEdit->setText(QStringLiteral("replacement.example.com"));
            portSpin->setValue(2200);
            if (nameEdit->text() != QStringLiteral("Production API")) {
                failure = QStringLiteral("修改连接参数覆盖了用户手动填写的名称");
            }
        }

        dialog->close();
    });

    addHostButton->click();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
}

void HostManagerWidgetTest::hostChainEditorCreatesAndSelectsManagedJumpHost()
{
    QString error;
    HostRecord existingJump;
    existingJump.id = QStringLiteral("existing-jump");
    existingJump.kind = HostNodeKind::Host;
    existingJump.name = QStringLiteral("已有跳板机");
    existingJump.host = QStringLiteral("edge.example.com");
    QVERIFY2(m_database->saveHost(existingJump, &error), qPrintable(error));

    HostRecord target;
    target.id = QStringLiteral("managed-target");
    target.kind = HostNodeKind::Host;
    target.name = QStringLiteral("内网数据库");
    target.host = QStringLiteral("10.20.3.15");
    QVERIFY2(m_database->saveHost(target, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    QTreeWidgetItem *targetItem = findItem(widget, target.name);
    QVERIFY(targetItem != nullptr);
    auto *tree = widget.findChild<QTreeWidget *>();
    QVERIFY(tree != nullptr);
    tree->setCurrentItem(targetItem);

    QString failure;
    QTimer::singleShot(0, [&failure]() {
        auto *hostDialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (hostDialog == nullptr) {
            failure = QStringLiteral("没有打开主机配置窗口");
            return;
        }
        auto *chainButton = hostDialog->findChild<QPushButton *>(
            QStringLiteral("hostChainButton"));
        if (chainButton == nullptr) {
            failure = QStringLiteral("主机配置缺少主机链入口");
            hostDialog->reject();
            return;
        }

        QTimer::singleShot(0, [&failure]() {
            auto *chainDialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (chainDialog == nullptr
                || chainDialog->objectName() != QStringLiteral("HostChainDialog")) {
                failure = QStringLiteral("没有打开独立主机链编辑器");
                return;
            }
            auto *addButton = chainDialog->findChild<QPushButton *>(
                QStringLiteral("hostChainAddButton"));
            auto *chainList = chainDialog->findChild<QListWidget *>(
                QStringLiteral("hostChainList"));
            auto *targetCard = chainDialog->findChild<QFrame *>(
                QStringLiteral("hostChainTargetCard"));
            auto *removeButton = chainDialog->findChild<QPushButton *>(
                QStringLiteral("hostChainRemoveButton"));
            auto *footer = chainDialog->findChild<QWidget *>(
                QStringLiteral("HostChainFooter"));
            if (addButton == nullptr || chainList == nullptr || targetCard == nullptr
                || removeButton == nullptr || footer == nullptr) {
                failure = QStringLiteral("主机链编辑器缺少添加、列表、目标卡片或页脚结构");
                chainDialog->reject();
                return;
            }
            // 目标主机必须位于 QListWidget 外，才能从结构上保证它不可拖动、不可删除；空链
            // 默认不选中任何跳板，危险移除动作也必须保持禁用。
            if (chainList->isAncestorOf(targetCard) || removeButton->isEnabled()
                || !addButton->property("primaryAction").toBool()) {
                failure = QStringLiteral("目标主机或危险移除动作未遵守参考界面的结构边界");
                chainDialog->reject();
                return;
            }

            QTimer::singleShot(0, [&failure]() {
                auto *picker = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (picker == nullptr
                    || picker->objectName() != QStringLiteral("HostPickerDialog")) {
                    failure = QStringLiteral("没有打开已有主机选择器");
                    return;
                }
                auto *candidates = picker->findChild<QListWidget *>(
                    QStringLiteral("hostChainCandidateList"));
                auto *createButton = picker->findChild<QPushButton *>(
                    QStringLiteral("hostChainCreateHostButton"));
                auto *searchEdit = picker->findChild<QLineEdit *>(
                    QStringLiteral("hostChainSearchEdit"));
                auto *pickerFooter = picker->findChild<QWidget *>(
                    QStringLiteral("HostPickerFooter"));
                if (candidates == nullptr || candidates->count() != 1
                    || !candidates->item(0)->text().contains(QStringLiteral("已有跳板机"))) {
                    failure = QStringLiteral("已有主机没有作为可选跳板出现，或目标主机未被排除");
                    picker->reject();
                    return;
                }
                if (createButton == nullptr) {
                    failure = QStringLiteral("选择器缺少新建主机入口");
                    picker->reject();
                    return;
                }
                if (searchEdit == nullptr || searchEdit->actions().isEmpty()
                    || pickerFooter == nullptr
                    || candidates->item(0)->sizeHint().height() != 62) {
                    failure = QStringLiteral("跳板机选择器缺少带图标搜索、卡片列表或独立页脚");
                    picker->reject();
                    return;
                }

                // 点击“新建主机”会先关闭选择器，随后由链编辑器打开完整主机配置窗口；提前
                // 排队处理器才能跨越两个嵌套 exec()，真实覆盖“保存后自动加入”的流程。
                QTimer::singleShot(0, [&failure]() {
                    auto *newHostDialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                    if (newHostDialog == nullptr
                        || newHostDialog->objectName() != QStringLiteral("ConnectionDialog")) {
                        failure = QStringLiteral("新建跳板机没有复用完整主机配置窗口");
                        return;
                    }
                    auto *name = newHostDialog->findChild<QLineEdit *>(
                        QStringLiteral("hostRecordNameEdit"));
                    auto *address = newHostDialog->findChild<QLineEdit *>(
                        QStringLiteral("hostAddressEdit"));
                    auto *save = newHostDialog->findChild<QPushButton *>(
                        QStringLiteral("hostRecordSaveButton"));
                    if (name == nullptr || address == nullptr || save == nullptr) {
                        failure = QStringLiteral("新建跳板机窗口字段不完整");
                        newHostDialog->reject();
                        return;
                    }
                    address->setText(QStringLiteral("198.51.100.8"));
                    name->selectAll();
                    QTest::keyClicks(name, "On-demand jump");
                    save->click();
                });
                createButton->click();
            });
            addButton->click();

            if (failure.isEmpty()
                && (chainList == nullptr || chainList->count() != 1
                    || !chainList->item(0)->text().contains(QStringLiteral("On-demand jump")))) {
                failure = QStringLiteral("现场新建的主机没有自动加入当前链路");
            }
            if (failure.isEmpty()) {
                const QListWidgetItem *jumpItem = chainList->item(0);
                if ((jumpItem->flags() & Qt::ItemIsDragEnabled) == 0
                    || jumpItem->sizeHint().height() != 68) {
                    failure = QStringLiteral("主机链跳板卡片不可拖拽或未使用参考行高");
                } else {
                    // 只有明确选中可拖拽跳板行后才允许移除；目标卡片不在列表中，因此永远
                    // 无法通过这个状态门成为删除目标。
                    chainList->setCurrentRow(0);
                    if (!removeButton->isEnabled()) {
                        failure = QStringLiteral("选中跳板卡片后移除动作仍未启用");
                    }
                }
            }
            auto *saveChain = chainDialog->findChild<QPushButton *>(
                QStringLiteral("hostChainSaveButton"));
            if (failure.isEmpty() && saveChain != nullptr) {
                saveChain->click();
            } else {
                chainDialog->reject();
            }
        });
        chainButton->click();

        auto *saveHost = hostDialog->findChild<QPushButton *>(
            QStringLiteral("hostRecordSaveButton"));
        if (failure.isEmpty() && saveHost != nullptr) {
            saveHost->click();
        } else {
            hostDialog->reject();
        }
    });

    widget.editSelected();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));

    const std::optional<HostRecord> savedTarget = m_database->findHost(target.id, &error);
    QVERIFY2(savedTarget.has_value(), qPrintable(error));
    QCOMPARE(savedTarget->hostChain.size(), 1);
    const std::optional<HostRecord> createdJump =
        m_database->findHost(savedTarget->hostChain.constFirst(), &error);
    QVERIFY2(createdJump.has_value(), "链路引用的新建跳板机没有独立保存到主机管理");
    QCOMPARE(createdJump->name, QStringLiteral("On-demand jump"));
    QCOMPARE(createdJump->host, QStringLiteral("198.51.100.8"));
}

namespace {

/** @brief 建一个带子主机的分组，外加一台顶层主机，凑出"分组/主机"两种层级关系。 */
bool seedGroupWithChildAndTopLevelHost(ApplicationDatabase *database, QString *error)
{
    HostRecord group;
    group.id = QStringLiteral("group-3");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("上饶E呼即办");
    if (!database->saveHost(group, error)) {
        return false;
    }

    HostRecord child;
    child.id = QStringLiteral("host-3");
    child.parentId = group.id;
    child.kind = HostNodeKind::Host;
    child.name = QStringLiteral("中间件");
    child.host = QStringLiteral("115.159.35.31");
    if (!database->saveHost(child, error)) {
        return false;
    }

    HostRecord topLevel;
    topLevel.id = QStringLiteral("host-4");
    topLevel.kind = HostNodeKind::Host;
    topLevel.name = QStringLiteral("10.38.217.99");
    topLevel.host = QStringLiteral("10.38.217.99");
    return database->saveHost(topLevel, error);
}

} // namespace

void HostManagerWidgetTest::topLevelNodesShareLeftEdgeAndChildHostsIndent()
{
    QString error;
    QVERIFY2(seedGroupWithChildAndTopLevelHost(m_database.get(), &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    widget.resize(320, 400);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));

    auto *tree = widget.findChild<QTreeWidget *>();
    QVERIFY(tree != nullptr);
    QTreeWidgetItem *groupItem = findItem(widget, QStringLiteral("上饶E呼即办"));
    QTreeWidgetItem *childItem = findItem(widget, QStringLiteral("中间件"));
    QTreeWidgetItem *topLevelItem = findItem(widget, QStringLiteral("10.38.217.99"));
    QVERIFY(groupItem != nullptr);
    QVERIFY(childItem != nullptr);
    QVERIFY(topLevelItem != nullptr);
    QVERIFY(groupItem->isExpanded());

    // 顶层分组与顶层主机必须顶格并对齐：只为箭头存在的分支装饰列一旦还在，这里就会差出
    // 一整列空白。
    const QRect groupRect = tree->visualItemRect(groupItem);
    const QRect topLevelRect = tree->visualItemRect(topLevelItem);
    QCOMPARE(topLevelRect.left(), groupRect.left());
    QCOMPARE(groupRect.left(), 0);

    // 分组下的主机必须右缩进一级，否则看不出它属于这个分组。
    const QRect childRect = tree->visualItemRect(childItem);
    QCOMPARE(childRect.left() - groupRect.left(), tree->indentation());
    QVERIFY2(tree->indentation() > 0, "层级缩进被压成 0，分组与主机会顶成一列");
}

void HostManagerWidgetTest::groupToggleIsLeftAlignedVerticallyCenteredAndClickable()
{
    QString error;
    QVERIFY2(seedGroupWithChildAndTopLevelHost(m_database.get(), &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    widget.resize(320, 400);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));

    auto *tree = widget.findChild<QTreeWidget *>();
    QVERIFY(tree != nullptr);
    QTreeWidgetItem *groupItem = findItem(widget, QStringLiteral("上饶E呼即办"));
    QVERIFY(groupItem != nullptr);
    QVERIFY(groupItem->isExpanded());
    const QRect groupRect = tree->visualItemRect(groupItem);

    // 先在最后一行下方的空白处点一下：窗口刚显示时的第一次点击会被平台层用于激活窗口，
    // 落在树上会让"这一下到底算不算命中"变得不稳定。空白处没有任何节点，点它不改变状态。
    QTest::mouseClick(tree->viewport(),
                      Qt::LeftButton,
                      {},
                      QPoint(groupRect.center().x(), tree->viewport()->height() - 4));

    // 点一下再点回来，只用于探测某个点是不是热区，不改变探测前后的展开状态。
    const auto togglesAt = [&](const QPoint &point) {
        const bool before = groupItem->isExpanded();
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, point);
        const bool changed = groupItem->isExpanded() != before;
        groupItem->setExpanded(before);
        return changed;
    };

    // 横向：热区必须覆盖行首完整图标区域，而不是只有 8 像素宽的箭头图形。逐像素探测既
    // 验证它左对齐，也防止后续重构又把可点击范围缩回三角形本身。
    int leftmostHit = -1;
    int rightmostHit = -1;
    for (int x = groupRect.left(); x < groupRect.right(); ++x) {
        if (togglesAt(QPoint(x, groupRect.center().y()))) {
            leftmostHit = leftmostHit < 0 ? x : leftmostHit;
            rightmostHit = x;
        }
    }
    QVERIFY2(leftmostHit >= 0, "分组行上找不到能展开收起的箭头");
    QVERIFY2(leftmostHit <= groupRect.left() + 1,
             qPrintable(QStringLiteral("箭头热区左缘距行首 %1 像素，没有左对齐")
                            .arg(leftmostHit - groupRect.left())));
    QVERIFY2(rightmostHit < groupRect.left() + 30,
             qPrintable(QStringLiteral("箭头热区延伸到 x=%1，覆盖了后面的文件夹或名称")
                            .arg(rightmostHit)));
    QVERIFY2(rightmostHit - leftmostHit + 1 >= 20,
             qPrintable(QStringLiteral("左侧展开按钮热区只有 %1 像素宽，仍然过小")
                            .arg(rightmostHit - leftmostHit + 1)));

    // 文件夹与名称是选中/拖拽入口，不应顺带改变展开状态；参考设计只把左侧箭头区作为开关。
    QVERIFY2(!togglesAt(QPoint(groupRect.left() + 34, groupRect.center().y())),
             "文件夹图标区域被误当成展开按钮");

    // 纵向：左侧图标区域应覆盖当前分组行的完整高度。循环没有探测 QRect::bottom() 这一
    // 个边界像素，因此上下各允许 1 像素取整误差。
    const int probeX = (leftmostHit + rightmostHit) / 2;
    int topHit = -1;
    int bottomHit = -1;
    for (int y = groupRect.top(); y < groupRect.bottom(); ++y) {
        if (togglesAt(QPoint(probeX, y))) {
            topHit = topHit < 0 ? y : topHit;
            bottomHit = y;
        }
    }
    QVERIFY(topHit >= 0);
    QVERIFY2(topHit <= groupRect.top() + 1,
             qPrintable(QStringLiteral("左侧展开按钮热区顶部留出了 %1 像素死区")
                            .arg(topHit - groupRect.top())));
    QVERIFY2(bottomHit >= groupRect.bottom() - 2,
             qPrintable(QStringLiteral("左侧展开按钮热区底部留出了 %1 像素死区")
                            .arg(groupRect.bottom() - bottomHit)));

    // 收起后再点同一个位置必须能重新展开，避免做成“单向收起”。
    const QPoint arrowCenter(probeX, (topHit + bottomHit) / 2);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, arrowCenter);
    QVERIFY2(!groupItem->isExpanded(), "点击箭头没有收起分组");
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, arrowCenter);
    QVERIFY2(groupItem->isExpanded(), "再次点击箭头没有重新展开分组");
}

namespace {

/**
 * @brief 统计某块区域里"有墨"的最右一列，用来量出一段文字实际画了多宽。
 * @param background 该行的底色，与之明显不同的像素即视为文字笔画。
 * @return 相对于 area.left() 的墨迹宽度；整块空白返回 0。
 */
int inkWidth(const QImage &image, const QRect &area, const QColor &background)
{
    int rightmost = -1;
    for (int x = area.left(); x <= area.right() && x < image.width(); ++x) {
        for (int y = area.top(); y <= area.bottom() && y < image.height(); ++y) {
            const QColor pixel = image.pixelColor(x, y);
            // 阈值取 24：反锯齿边缘也算墨，但挡得住选中态之类的淡背景过渡。
            if (std::abs(pixel.red() - background.red()) > 24
                || std::abs(pixel.green() - background.green()) > 24
                || std::abs(pixel.blue() - background.blue()) > 24) {
                rightmost = x;
                break;
            }
        }
    }
    return rightmost < 0 ? 0 : rightmost - area.left() + 1;
}

/**
 * @brief 统计某块区域里"有墨"的纵向跨度，用来区分一行文字和两行文字的绘制结果。
 * @param background 该行的底色，与之明显不同的像素即视为文字笔画。
 * @return 从最上一行墨迹到最下一行墨迹的像素跨度；整块空白返回 0。
 */
int textInkHeight(const QImage &image, const QRect &area, const QColor &background)
{
    int top = -1;
    int bottom = -1;
    for (int y = area.top(); y <= area.bottom() && y < image.height(); ++y) {
        for (int x = area.left(); x <= area.right() && x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (std::abs(pixel.red() - background.red()) > 24
                || std::abs(pixel.green() - background.green()) > 24
                || std::abs(pixel.blue() - background.blue()) > 24) {
                top = top < 0 ? y : top;
                bottom = y;
                break;
            }
        }
    }
    return bottom < 0 ? 0 : bottom - top + 1;
}

} // namespace

void HostManagerWidgetTest::compactModeShrinksTreeAndPreservesState()
{
    QString error;
    QVERIFY2(seedGroupWithChildAndTopLevelHost(m_database.get(), &error), qPrintable(error));
    HostManagerWidget widget(m_database.get(), m_vault.get());
    widget.resize(400, 480);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
    auto *tree = widget.findChild<QTreeWidget *>(QStringLiteral("HostTree"));
    auto *folder = findItem(widget, QStringLiteral("上饶E呼即办"));
    auto *host = findItem(widget, QStringLiteral("中间件"));
    QVERIFY(tree && folder && host);
    folder->setExpanded(true);
    tree->setCurrentItem(host);
    const QRect relaxedFolder = tree->visualItemRect(folder);
    const QRect relaxedHost = tree->visualItemRect(host);
    const QSize relaxedIcon = tree->iconSize();
    widget.setSidebarDensity(SidebarDensity::Compact);
    QCOMPARE(tree->visualItemRect(folder).height(), 28);
    QCOMPARE(tree->visualItemRect(host).height(), 40);
    QCOMPARE(tree->indentation(), 24);
    QVERIFY(tree->iconSize().width() < relaxedIcon.width());
    QCOMPARE(tree->currentItem(), host);
    QVERIFY(folder->isExpanded());
    // 切换后仍可通过箭头点击折叠，验证紧凑布局的绘制与点击坐标保持一致。
    const QRect compactFolder = tree->visualItemRect(folder);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(compactFolder.left() + 9, compactFolder.center().y()));
    QVERIFY(!folder->isExpanded());
    widget.setSidebarDensity(SidebarDensity::Relaxed);
    QVERIFY(!folder->isExpanded());
    folder->setExpanded(true);
    QCOMPARE(tree->visualItemRect(folder), relaxedFolder);
    QCOMPARE(tree->visualItemRect(host), relaxedHost);
    QCOMPARE(tree->iconSize(), relaxedIcon);
}

void HostManagerWidgetTest::minimalModeHidesAddressAndShrinksFurtherThanCompact()
{
    QString error;
    QVERIFY2(seedGroupWithChildAndTopLevelHost(m_database.get(), &error), qPrintable(error));
    HostManagerWidget widget(m_database.get(), m_vault.get());
    widget.resize(400, 480);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
    auto *tree = widget.findChild<QTreeWidget *>(QStringLiteral("HostTree"));
    auto *folder = findItem(widget, QStringLiteral("上饶E呼即办"));
    auto *host = findItem(widget, QStringLiteral("中间件"));
    QVERIFY(tree && folder && host);
    folder->setExpanded(true);

    widget.setSidebarDensity(SidebarDensity::Compact);
    const QRect compactHostRect = tree->visualItemRect(host);
    const int compactFolderHeight = tree->visualItemRect(folder).height();
    const int compactIndentation = tree->indentation();
    QVERIFY(!compactHostRect.isEmpty());
    const QImage compactImage = tree->viewport()->grab(compactHostRect).toImage();
    // 名称从 metric(kHostTextLeft, 39) 起画；45px 起扫描能跳过左侧图标容器（最右到
    // x=31），只统计名称/地址文字的墨迹。
    const QColor background = compactImage.pixelColor(compactImage.width() - 1, 0);
    const QRect compactTextArea(45, 0, compactImage.width() - 46, compactImage.height());
    const int compactTextInkHeight = textInkHeight(compactImage, compactTextArea, background);

    widget.setSidebarDensity(SidebarDensity::Minimal);
    // 精简必须比紧凑更紧：行高和缩进都进一步收窄，而不是简单复用紧凑的取值。
    QVERIFY(tree->visualItemRect(folder).height() < compactFolderHeight);
    QVERIFY(tree->indentation() < compactIndentation);
    const QRect minimalHostRect = tree->visualItemRect(host);
    QVERIFY(!minimalHostRect.isEmpty());
    QVERIFY(minimalHostRect.height() < compactHostRect.height());

    const QImage minimalImage = tree->viewport()->grab(minimalHostRect).toImage();
    const QRect minimalTextArea(45, 0, minimalImage.width() - 46, minimalImage.height());
    const int minimalTextInkHeight = textInkHeight(minimalImage, minimalTextArea, background);
    // 地址副标题（HostDetailRole）仍留在数据里供其它逻辑使用，精简只是不再绘制它；
    // 用文字墨迹的纵向跨度代替逐像素快照——两行文字的墨迹跨度明显高于一行，且不受
    // 具体字体渲染细节影响，比对图像相等更不容易在不同平台上误报。
    QVERIFY2(minimalTextInkHeight > 0, "精简模式下应该仍然画出主机名称");
    QVERIFY2(minimalTextInkHeight < compactTextInkHeight,
             qPrintable(QStringLiteral("精简模式文字墨迹高度 %1 应明显小于紧凑模式的两行高度 %2")
                            .arg(minimalTextInkHeight)
                            .arg(compactTextInkHeight)));
}

void HostManagerWidgetTest::childHostInteractionDoesNotAlterGuideLines()
{
    QString error;
    QVERIFY2(seedGroupWithChildAndTopLevelHost(m_database.get(), &error), qPrintable(error));

    HostRecord sibling;
    sibling.id = QStringLiteral("host-3-sibling");
    sibling.parentId = QStringLiteral("group-3");
    sibling.kind = HostNodeKind::Host;
    sibling.name = QStringLiteral("对照主机");
    sibling.host = QStringLiteral("115.159.35.32");
    QVERIFY2(m_database->saveHost(sibling, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    widget.resize(320, 400);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));

    auto *tree = widget.findChild<QTreeWidget *>(QStringLiteral("HostTree"));
    QTreeWidgetItem *groupItem = findItem(widget, QStringLiteral("上饶E呼即办"));
    QTreeWidgetItem *childItem = findItem(widget, QStringLiteral("中间件"));
    QTreeWidgetItem *siblingItem = findItem(widget, sibling.name);
    QVERIFY(tree != nullptr);
    QVERIFY(groupItem != nullptr);
    QVERIFY(childItem != nullptr);
    QVERIFY(siblingItem != nullptr);
    const QRect groupRect = tree->visualItemRect(groupItem);
    const QRect childRect = tree->visualItemRect(childItem);
    QVERIFY(!groupRect.isEmpty());
    QVERIFY(!childRect.isEmpty());
    QVERIFY2(childRect.left() > groupRect.left(), "测试数据没有形成可检查的子节点缩进区");

    const auto indentationDifferenceCount = [&](const QImage &rendered) {
        const int scale = std::max(1, qRound(rendered.devicePixelRatio()));
        const QRect currentChildRect = tree->visualItemRect(childItem);
        const QRect currentSiblingRect = tree->visualItemRect(siblingItem);
        const int indentationWidth =
            std::max(0, currentChildRect.left() - groupRect.left() - 2);
        const int comparisonHeight = std::max(
            0, std::min(currentChildRect.height(), currentSiblingRect.height()) / 2 - 4);
        const QRect childIndentation((groupRect.left() + 1) * scale,
                                     (currentChildRect.top() + 2) * scale,
                                     indentationWidth * scale,
                                     comparisonHeight * scale);
        const QRect siblingIndentation((groupRect.left() + 1) * scale,
                                       (currentSiblingRect.top() + 2) * scale,
                                       indentationWidth * scale,
                                       comparisonHeight * scale);
        int changedPixelCount = 0;
        for (int offsetX = 0; offsetX < childIndentation.width(); ++offsetX) {
            for (int offsetY = 0; offsetY < childIndentation.height(); ++offsetY) {
                const QColor pixel = rendered.pixelColor(childIndentation.left() + offsetX,
                                                         childIndentation.top() + offsetY);
                const QColor siblingPixel = rendered.pixelColor(
                    siblingIndentation.left() + offsetX,
                    siblingIndentation.top() + offsetY);
                // 仅比较横线以上的共同线段，末尾兄弟横线以下应留白；以无交互兄弟作为基线，可
                // 兼容系统行底色，同时拦截选中条、连接线变色和悬停填充。
                if (std::abs(pixel.red() - siblingPixel.red()) > 8
                    || std::abs(pixel.green() - siblingPixel.green()) > 8
                    || std::abs(pixel.blue() - siblingPixel.blue()) > 8) {
                    ++changedPixelCount;
                }
            }
        }
        return changedPixelCount;
    };

    tree->clearSelection();
    tree->setCurrentItem(nullptr);
    QTest::mouseMove(tree->viewport(), QPoint(2, tree->viewport()->height() - 2));
    QCoreApplication::processEvents();

    const QImage neutralRendering = tree->viewport()->grab().toImage();
    const int neutralScale = std::max(1, qRound(neutralRendering.devicePixelRatio()));
    const QRect neutralChildRect = tree->visualItemRect(childItem);
    const int guideLineX = (neutralChildRect.left() - 15) * neutralScale;
    const int backgroundSampleX = (neutralChildRect.left() - 25) * neutralScale;
    int visibleGuidePixels = 0;
    for (int y = (neutralChildRect.top() + 2) * neutralScale;
         y < (neutralChildRect.bottom() - 2) * neutralScale;
         ++y) {
        const QColor linePixel = neutralRendering.pixelColor(guideLineX, y);
        const QColor backgroundPixel = neutralRendering.pixelColor(backgroundSampleX, y);
        if (std::abs(linePixel.red() - backgroundPixel.red()) > 8
            || std::abs(linePixel.green() - backgroundPixel.green()) > 8
            || std::abs(linePixel.blue() - backgroundPixel.blue()) > 8) {
            ++visibleGuidePixels;
        }
    }
    QVERIFY2(visibleGuidePixels > neutralChildRect.height() * neutralScale / 2,
             "子主机左侧的中性父子连线没有被绘制");

    QTest::mouseMove(tree->viewport(), childRect.center());
    QCoreApplication::processEvents();
    QCOMPARE(indentationDifferenceCount(tree->viewport()->grab().toImage()), 0);

    tree->setCurrentItem(childItem);
    QTest::mouseMove(tree->viewport(), QPoint(2, tree->viewport()->height() - 2));
    QCoreApplication::processEvents();
    QCOMPARE(indentationDifferenceCount(tree->viewport()->grab().toImage()), 0);
}

void HostManagerWidgetTest::nestedGuideLinesConnectAndStopAtLastSibling()
{
    HostManagerWidget widget(m_database.get(), m_vault.get());
    widget.resize(480, 640);
    auto *tree = widget.findChild<QTreeWidget *>(QStringLiteral("HostTree"));
    QVERIFY(tree != nullptr);
    tree->clear();
    // 直接构造三层分组，隔离数据库排序，让每个末尾节点及祖先关系完全确定。
    auto *root = new QTreeWidgetItem(tree, {QStringLiteral("Root")});
    auto *first = new QTreeWidgetItem(root, {QStringLiteral("First")});
    auto *nested = new QTreeWidgetItem(first, {QStringLiteral("Nested")});
    auto *leaf = new QTreeWidgetItem(nested, {QStringLiteral("Leaf")});
    auto *last = new QTreeWidgetItem(root, {QStringLiteral("Last")});
    auto *lastChild = new QTreeWidgetItem(last, {QStringLiteral("Last child")});
    tree->expandAll();
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
    tree->clearSelection();
    QTest::mouseMove(tree->viewport(), QPoint(2, tree->viewport()->height() - 2));

    // 比较同一行的线条与邻近空白像素，兼容浅色/深色主题及高 DPI，不绑定具体颜色。
    // 参数为视口逻辑坐标及空白采样偏移（横线需避开整段）；返回是否存在可见线条。
    const auto hasLine = [&](int x, int y, int backgroundOffset = 4) {
        const QImage image = tree->viewport()->grab().toImage();
        const qreal scale = image.devicePixelRatio();
        const QColor line = image.pixelColor(qRound(x * scale), qRound(y * scale));
        const QColor blank = image.pixelColor(qRound((x - backgroundOffset) * scale), qRound(y * scale));
        return std::abs(line.red() - blank.red()) > 8
            || std::abs(line.green() - blank.green()) > 8
            || std::abs(line.blue() - blank.blue()) > 8;
    };
    const int ancestorX = tree->visualItemRect(first).left() - 15;
    const QRect leafRect = tree->visualItemRect(leaf);
    // First 后面仍有 Last，因此祖先线必须穿过全部子孙行，包括行边界。
    for (int y = tree->visualItemRect(first).top(); y < tree->visualItemRect(last).center().y(); ++y) {
        QVERIFY2(hasLine(ancestorX, y), "祖先连线在展开的子孙行中断开");
    }
    QVERIFY(hasLine(leafRect.left() - 15, leafRect.top() + 2));
    QVERIFY(hasLine(leafRect.left() - 8, leafRect.center().y(), 12));
    QVERIFY2(!hasLine(leafRect.left() - 15, leafRect.bottom() - 2), "末尾叶节点竖线不应越过横线");
    const QRect lastRect = tree->visualItemRect(last);
    QVERIFY(!hasLine(ancestorX, lastRect.bottom() - 2));
    QVERIFY(!hasLine(ancestorX, tree->visualItemRect(lastChild).center().y()));

    // 搜索会隐藏节点；隐藏的后续兄弟不能让当前可见末尾继续拖出竖线。
    last->setHidden(true);
    QCoreApplication::processEvents();
    QVERIFY(!hasLine(ancestorX, tree->visualItemRect(first).bottom() - 2));
    QVERIFY(!hasLine(ancestorX, leafRect.center().y()));
}

void HostManagerWidgetTest::emptyGroupDoesNotPaintZeroCountBadge()
{
    HostRecord emptyGroup;
    emptyGroup.id = QStringLiteral("empty-group-no-count");
    emptyGroup.kind = HostNodeKind::Folder;
    emptyGroup.name = QStringLiteral("空分组");
    QString error;
    QVERIFY2(m_database->saveHost(emptyGroup, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    widget.resize(320, 180);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));

    auto *tree = widget.findChild<QTreeWidget *>(QStringLiteral("HostTree"));
    QTreeWidgetItem *groupItem = findItem(widget, emptyGroup.name);
    QVERIFY(tree != nullptr);
    QVERIFY(groupItem != nullptr);
    tree->clearSelection();
    tree->setCurrentItem(nullptr);
    QTest::mouseMove(tree->viewport(), QPoint(2, tree->viewport()->height() - 2));
    QCoreApplication::processEvents();

    const QRect groupRect = tree->visualItemRect(groupItem);
    QVERIFY(!groupRect.isEmpty());
    const QImage rendered = tree->viewport()->grab().toImage();
    const int scale = std::max(1, qRound(rendered.devicePixelRatio()));
    const QRect rightDecorationArea((groupRect.right() - 31) * scale,
                                    (groupRect.top() + 5) * scale,
                                    26 * scale,
                                    std::max(0, groupRect.height() - 10) * scale);
    const QColor background = rendered.pixelColor(rightDecorationArea.right(),
                                                  rightDecorationArea.top());
    QCOMPARE(inkWidth(rendered, rightDecorationArea, background), 0);
}

void HostManagerWidgetTest::semanticIconsFollowLightAndDarkPalettes()
{
    QWidget context;
    const QColor lightThemeIcon(QStringLiteral("#1b2230"));
    const QColor darkThemeIcon(QStringLiteral("#f0f3fa"));
    const QColor fixedBrandIcon(QStringLiteral("#0c2f24"));

    const auto setWindowText = [&context](const QColor &color) {
        QPalette palette = context.palette();
        // 正常和禁用模式分别读取对应 ColorGroup；测试正常模式时也同步其它组，避免平台默认
        // 调色板在控件尚未显示时把 Active 自动解析为 Inactive 而造成无关差异。
        palette.setColor(QPalette::Active, QPalette::WindowText, color);
        palette.setColor(QPalette::Inactive, QPalette::WindowText, color);
        palette.setColor(QPalette::Disabled, QPalette::WindowText, color);
        context.setPalette(palette);
    };
    const auto strongestPixel = [](const QIcon &icon) {
        const QImage image = icon.pixmap(QSize(20, 20), QIcon::Normal).toImage();
        QColor result;
        int greatestAlpha = -1;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.alpha() > greatestAlpha) {
                    greatestAlpha = pixel.alpha();
                    result = pixel;
                }
            }
        }
        return result;
    };
    const auto totalOpacity = [](const QIcon &icon) {
        const QImage image = icon.pixmap(QSize(64, 64), QIcon::Normal).toImage();
        qint64 opacity = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                opacity += image.pixelColor(x, y).alpha();
            }
        }
        return opacity;
    };

    setWindowText(lightThemeIcon);
    const QIcon adaptive = UiIcons::symbol(
        &context, UiIcons::Symbol::Refresh, context.palette().color(QPalette::WindowText));
    const QIcon fixed = UiIcons::symbol(&context, UiIcons::Symbol::Plus, fixedBrandIcon);
    QCOMPARE(strongestPixel(adaptive).rgb(), lightThemeIcon.rgb());
    QCOMPARE(strongestPixel(fixed).rgb(), fixedBrandIcon.rgb());

    // 主机树图标与导航栏服务器图标保持同一轮廓，但重复密集出现时必须使用更细笔画。
    // 以相同尺寸和颜色比较总不透明度，可直接约束实际渲染重量，而不依赖实现中的线宽数字。
    const QIcon navigationHosts = UiIcons::symbol(
        &context, UiIcons::Symbol::Hosts, context.palette().color(QPalette::WindowText));
    const QIcon treeHost = UiIcons::symbol(
        &context, UiIcons::Symbol::HostItem, context.palette().color(QPalette::WindowText));
    QVERIFY2(totalOpacity(treeHost) < totalOpacity(navigationHosts),
             "主机树服务器图标没有比导航栏图标使用更轻的线条");

    // 关键回归：不重新创建 QIcon，只切换所属控件的调色板。同一个实例必须立即绘制成深色
    // 主题前景色，否则应用设置页实时切换主题后仍会保留旧位图并消失在新背景上。
    setWindowText(darkThemeIcon);
    QCOMPARE(strongestPixel(adaptive).rgb(), darkThemeIcon.rgb());
    // 主按钮的绿色背景不随主题改变，它的深色“+”属于显式品牌色，不能被全局前景色覆盖。
    QCOMPARE(strongestPixel(fixed).rgb(), fixedBrandIcon.rgb());
}

void HostManagerWidgetTest::comboPopupRemovesOuterSquareFrame()
{
    QComboBox combo;
    UiIcons::modernizePopup(&combo);

    // QComboBox 的内部视图与承载它的顶层容器不是同一个控件。视觉问题来自后者仍使用
    // StyledPanel，因此回归必须直接检查容器，而不能只验证列表本身的 QSS 或圆角半径。
    QAbstractItemView *view = combo.view();
    QVERIFY2(view != nullptr, "下拉框必须创建候选项视图");
    QWidget *container = view->window();
    QVERIFY2(container != nullptr && container != view, "下拉列表必须具有独立顶层容器");
    QVERIFY2(container->testAttribute(Qt::WA_TranslucentBackground),
             "下拉容器背景必须透明，避免圆角之外重新露出方形底色");

    auto *containerFrame = qobject_cast<QFrame *>(container);
    QVERIFY2(containerFrame != nullptr, "Qt 下拉容器应为可关闭原生框架的 QFrame");
    QCOMPARE(containerFrame->frameShape(), QFrame::NoFrame);
    QCOMPARE(containerFrame->lineWidth(), 0);
    QCOMPARE(containerFrame->midLineWidth(), 0);
}

void HostManagerWidgetTest::hostAndFolderNamesUseRegularWeight()
{
    // 两个节点刻意使用同一名称，避免文字内容干扰字体属性检查；产品层级只由节点类型决定。
    const QString sharedName = QStringLiteral("上饶E呼即办");

    HostRecord group;
    group.id = QStringLiteral("group-5");
    group.kind = HostNodeKind::Folder;
    group.name = sharedName;
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostRecord host;
    host.id = QStringLiteral("host-5");
    host.kind = HostNodeKind::Host;
    host.name = sharedName;
    host.host = QStringLiteral("10.38.217.99");
    QVERIFY2(m_database->saveHost(host, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    QTreeWidgetItem *groupItem = findItem(widget, sharedName);
    QVERIFY(groupItem != nullptr);
    // findItems 会返回同名的第一项；再从全部匹配项中按稳定类型角色识别具体主机，避免依赖
    // 数据库或树控件对同名记录的偶然排序。
    auto *tree = widget.findChild<QTreeWidget *>();
    QVERIFY(tree != nullptr);
    const QList<QTreeWidgetItem *> matches =
        tree->findItems(sharedName, Qt::MatchExactly | Qt::MatchRecursive);
    QTreeWidgetItem *hostItem = nullptr;
    groupItem = nullptr;
    for (QTreeWidgetItem *item : matches) {
        const auto kind = static_cast<HostNodeKind>(item->data(0, Qt::UserRole + 1).toInt());
        if (kind == HostNodeKind::Folder) {
            groupItem = item;
        } else if (kind == HostNodeKind::Host) {
            hostItem = item;
        }
    }
    QVERIFY(groupItem != nullptr);
    QVERIFY(hostItem != nullptr);

    auto *newHostButton = widget.findChild<QPushButton *>(QStringLiteral("HostAddButton"));
    QVERIFY(newHostButton != nullptr);
    // 主操作仍保留绿色强调色，但按钮文案应使用常规字重，避免标题区视觉过重。
    QVERIFY2(newHostButton->font().weight() <= QFont::Normal,
             "新增主机按钮文字不应使用粗体");

    // 模型字体是委托 QStyleOptionViewItem::font 的来源；目录与主机都必须明确回到 Normal，
    // 不能只依赖平台默认值碰巧不加粗，否则换主题后会重新出现粗体。
    QCOMPARE(groupItem->font(0).weight(), QFont::Normal);
    QCOMPARE(hostItem->font(0).weight(), QFont::Normal);
    QVERIFY(!groupItem->font(0).bold());
    QVERIFY(!hostItem->font(0).bold());
}

void HostManagerWidgetTest::openCloudOsUsesDedicatedDistributionIcon()
{
    // 映射层先验证 ID 归类，再比较像素图，确保图标不是“名称看似支持、实际仍走企鹅兜底”。
    QCOMPARE(HostDistributionIcon::familyForId(QStringLiteral("OpenCloudOS")),
             HostDistributionIcon::Family::OpenCloudOS);
    const QIcon openCloud = HostDistributionIcon::iconFor(QStringLiteral("opencloudos"));
    const QIcon generic = HostDistributionIcon::iconFor(QStringLiteral("some-unknown-linux"));
    QVERIFY(!openCloud.isNull());
    QVERIFY(!generic.isNull());
    QVERIFY(openCloud.pixmap(QSize(48, 48)).toImage()
            != generic.pixmap(QSize(48, 48)).toImage());
}

void HostManagerWidgetTest::domesticLinuxDistributionsUseDedicatedIcons()
{
    const QList<QPair<QString, HostDistributionIcon::Family>> expected = {
        {QStringLiteral("kylin"), HostDistributionIcon::Family::Kylin},
        {QStringLiteral("uos"), HostDistributionIcon::Family::UOS},
        {QStringLiteral("uniontechos"), HostDistributionIcon::Family::UOS},
        {QStringLiteral("deepin"), HostDistributionIcon::Family::Deepin},
        {QStringLiteral("deepin linux"), HostDistributionIcon::Family::Deepin}};
    for (const auto &[id, family] : expected) {
        QCOMPARE(HostDistributionIcon::familyForId(id), family);
        QVERIFY2(!HostDistributionIcon::iconFor(id).isNull(), qPrintable(id));
    }
    // 三类国产系统都不能退回未知发行版的通用企鹅图标。
    const QImage generic = HostDistributionIcon::iconFor(QStringLiteral("unknown-linux"))
                               .pixmap(QSize(48, 48)).toImage();
    for (const auto &[id, family] : expected) {
        Q_UNUSED(family)
        QVERIFY(HostDistributionIcon::iconFor(id).pixmap(QSize(48, 48)).toImage() != generic);
    }
}

void HostManagerWidgetTest::hostCountTracksAllNestedHosts()
{
    HostRecord group;
    group.id = QStringLiteral("count-group");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("测试环境");
    group.tags = {QStringLiteral("测试")};
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostRecord child;
    child.id = QStringLiteral("count-child");
    child.parentId = group.id;
    child.kind = HostNodeKind::Host;
    child.name = QStringLiteral("应用服务器");
    child.host = QStringLiteral("10.0.0.1");
    QVERIFY2(m_database->saveHost(child, &error), qPrintable(error));

    HostRecord topLevel;
    topLevel.id = QStringLiteral("count-top-level");
    topLevel.kind = HostNodeKind::Host;
    topLevel.name = QStringLiteral("运维入口");
    topLevel.host = QStringLiteral("10.0.0.2");
    QVERIFY2(m_database->saveHost(topLevel, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    QCOMPARE(widget.hostCount(), 2);

    // 分组标签会由 resolveHost() 继承给子主机，模型角色必须保存解析后的值，委托才能画出
    // 与 #标签 搜索语义一致的“测试”徽章。
    QTreeWidgetItem *childItem = findItem(widget, child.name);
    QVERIFY(childItem != nullptr);
    QCOMPARE(childItem->data(0, Qt::UserRole + 3).toStringList(),
             QStringList{QStringLiteral("测试")});

    // 搜索只改变当前树的展示结果，标题数量仍代表整个数据库，不能随过滤条件跳动。
    auto *searchEdit = widget.findChild<QLineEdit *>(QStringLiteral("HostSearchEdit"));
    QVERIFY(searchEdit != nullptr);
    searchEdit->setText(QStringLiteral("运维入口"));
    QCOMPARE(widget.hostCount(), 2);

    QSignalSpy countSpy(&widget, &HostManagerWidget::hostCountChanged);
    HostRecord anotherChild;
    anotherChild.id = QStringLiteral("count-child-2");
    anotherChild.parentId = group.id;
    anotherChild.kind = HostNodeKind::Host;
    anotherChild.name = QStringLiteral("数据库服务器");
    anotherChild.host = QStringLiteral("10.0.0.3");
    QVERIFY2(m_database->saveHost(anotherChild, &error), qPrintable(error));
    widget.refresh();
    QCOMPARE(widget.hostCount(), 3);
    QCOMPARE(countSpy.count(), 1);
    QCOMPARE(countSpy.constFirst().constFirst().toInt(), 3);
}

void HostManagerWidgetTest::tagShortcutEntersTagSearchMode()
{
    HostManagerWidget widget(m_database.get(), m_vault.get());
    widget.resize(320, 400);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));

    auto *searchEdit = widget.findChild<QLineEdit *>(QStringLiteral("HostSearchEdit"));
    auto *tagButton = widget.findChild<QToolButton *>(QStringLiteral("HostTagFilterButton"));
    QVERIFY2(searchEdit != nullptr, "主机页没有设计稿要求的组合搜索输入框");
    QVERIFY2(tagButton != nullptr, "搜索框右侧缺少 #标签 快捷入口");
    QCOMPARE(tagButton->text(), QStringLiteral("#tag"));

    tagButton->click();
    QCOMPARE(searchEdit->text(), QStringLiteral("#"));
    QVERIFY2(searchEdit->hasFocus(), "点击 #标签 后焦点没有回到输入框，用户无法连续输入标签名");

    // 已在标签模式时再次点击只负责聚焦，不能把用户已经输入的标签名重置成单个井号。
    searchEdit->setText(QStringLiteral("#生产"));
    tagButton->click();
    QCOMPARE(searchEdit->text(), QStringLiteral("#生产"));
}

void HostManagerWidgetTest::tagSearchModeShowsSuggestionPopupForQuickPick()
{
    HostRecord prodHost;
    prodHost.id = QStringLiteral("tag-suggest-prod");
    prodHost.kind = HostNodeKind::Host;
    prodHost.name = QStringLiteral("生产入口");
    prodHost.host = QStringLiteral("10.0.9.1");
    prodHost.tags = QStringList{QStringLiteral("生产")};
    QString error;
    QVERIFY2(m_database->saveHost(prodHost, &error), qPrintable(error));

    HostRecord testHost;
    testHost.id = QStringLiteral("tag-suggest-test");
    testHost.kind = HostNodeKind::Host;
    testHost.name = QStringLiteral("测试入口");
    testHost.host = QStringLiteral("10.0.9.2");
    testHost.tags = QStringList{QStringLiteral("测试")};
    QVERIFY2(m_database->saveHost(testHost, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    widget.resize(320, 400);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));

    auto *searchEdit = widget.findChild<QLineEdit *>(QStringLiteral("HostSearchEdit"));
    auto *tagButton = widget.findChild<QToolButton *>(QStringLiteral("HostTagFilterButton"));
    auto *tree = widget.findChild<QTreeWidget *>();
    QVERIFY(searchEdit != nullptr);
    QVERIFY(tagButton != nullptr);
    QVERIFY(tree != nullptr);

    auto *popup = widget.findChild<QFrame *>(QStringLiteral("HostTagSuggestionsPopup"));
    QVERIFY2(popup != nullptr, "缺少标签建议悬浮面板，无法在 #标签 模式下快捷选择");
    QVERIFY2(!popup->isVisible(), "尚未进入标签模式时不应提前显示建议面板");

    tagButton->click();
    QCOMPARE(searchEdit->text(), QStringLiteral("#"));
    QVERIFY2(popup->isVisible(), "点击 #标签 后应立即弹出全部已知标签供选择");

    const auto chips = popup->findChildren<QToolButton *>(QStringLiteral("hostTagChip"));
    QStringList chipTexts;
    for (QToolButton *chip : chips) {
        chipTexts.append(chip->text());
    }
    chipTexts.sort();
    QCOMPARE(chipTexts, (QStringList{QStringLiteral("#测试"), QStringLiteral("#生产")}));

    QToolButton *prodChip = nullptr;
    for (QToolButton *chip : chips) {
        if (chip->text() == QStringLiteral("#生产")) {
            prodChip = chip;
            break;
        }
    }
    QVERIFY2(prodChip != nullptr, "建议面板没有列出已存在的 生产 标签");
    prodChip->click();

    QCOMPARE(searchEdit->text(), QStringLiteral("#生产"));
    QVERIFY2(!popup->isVisible(), "点击某个标签胶囊后建议面板应自动收起");
    QCOMPARE(tree->topLevelItemCount(), 1);
    QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("生产入口"));

    // 清空搜索框回到浏览态后再次进入标签模式，建议面板必须能重新弹出，而不是只在首次
    // 点击时生效。
    searchEdit->clear();
    QVERIFY2(!popup->isVisible(), "清空搜索框应连同建议面板一起退出标签模式");
    tagButton->click();
    QVERIFY2(popup->isVisible(), "再次点击 #标签 入口必须能重新唤出建议面板");
}

void HostManagerWidgetTest::ungroupedHostsFollowFoldersBehindSectionHeader()
{
    HostRecord group;
    group.id = QStringLiteral("group-6");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("武汉");
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostRecord groupedHost;
    groupedHost.id = QStringLiteral("host-6a");
    groupedHost.parentId = group.id;
    groupedHost.kind = HostNodeKind::Host;
    groupedHost.name = QStringLiteral("预发布");
    groupedHost.host = QStringLiteral("10.194.135.11");
    QVERIFY2(m_database->saveHost(groupedHost, &error), qPrintable(error));

    // 无父分组，代表"未分组"主机。
    HostRecord ungroupedHost;
    ungroupedHost.id = QStringLiteral("host-6b");
    ungroupedHost.kind = HostNodeKind::Host;
    ungroupedHost.name = QStringLiteral("10.38.217.99");
    ungroupedHost.host = QStringLiteral("10.38.217.99");
    QVERIFY2(m_database->saveHost(ungroupedHost, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    auto *tree = widget.findChild<QTreeWidget *>();
    QVERIFY(tree != nullptr);

    // 顶层只应有三项：分组、"未分组"分区标题、无父主机；顺序必须是分组在前、主机在后，
    // 中间用标题隔开，否则两类节点混排会让用户分不清哪些是组织结构、哪些是可连接目标。
    QCOMPARE(tree->topLevelItemCount(), 3);

    QTreeWidgetItem *folderItem = tree->topLevelItem(0);
    QCOMPARE(folderItem->text(0), QStringLiteral("武汉"));
    QCOMPARE(static_cast<HostNodeKind>(folderItem->data(0, Qt::UserRole + 1).toInt()),
             HostNodeKind::Folder);

    QTreeWidgetItem *headerItem = tree->topLevelItem(1);
    QCOMPARE(headerItem->text(0), QStringLiteral("Ungrouped"));
    // 标题的 kind 既不是 Folder 也不是 Host：它不对应任何数据库记录，不能被误当成两者
    // 之一参与拖拽、编辑或删除。
    const int headerKind = headerItem->data(0, Qt::UserRole + 1).toInt();
    QVERIFY(headerKind != static_cast<int>(HostNodeKind::Folder));
    QVERIFY(headerKind != static_cast<int>(HostNodeKind::Host));
    QVERIFY2(!(headerItem->flags() & Qt::ItemIsSelectable), "分区标题不能被选中");
    QVERIFY2(!(headerItem->flags() & Qt::ItemIsDragEnabled), "分区标题不能被拖动");
    QVERIFY2(!(headerItem->flags() & Qt::ItemIsDropEnabled), "分区标题不能接收拖放");

    QTreeWidgetItem *ungroupedItem = tree->topLevelItem(2);
    QCOMPARE(ungroupedItem->text(0), QStringLiteral("10.38.217.99"));
    QCOMPARE(static_cast<HostNodeKind>(ungroupedItem->data(0, Qt::UserRole + 1).toInt()),
             HostNodeKind::Host);
    QVERIFY2(ungroupedItem->flags() & Qt::ItemIsSelectable, "无父主机本身仍必须可选中");

    // 分组下的主机仍是子节点，不受"未分组"分区影响。
    QCOMPARE(folderItem->childCount(), 1);
    QCOMPARE(folderItem->child(0)->text(0), QStringLiteral("预发布"));
}

void HostManagerWidgetTest::sectionHeaderAbsentWhenNoUngroupedHosts()
{
    HostRecord group;
    group.id = QStringLiteral("group-7");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("东西湖城运");
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostRecord groupedHost;
    groupedHost.id = QStringLiteral("host-7");
    groupedHost.parentId = group.id;
    groupedHost.kind = HostNodeKind::Host;
    groupedHost.name = QStringLiteral("10.70.165.50");
    groupedHost.host = QStringLiteral("10.70.165.50");
    QVERIFY2(m_database->saveHost(groupedHost, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    auto *tree = widget.findChild<QTreeWidget *>();
    QVERIFY(tree != nullptr);

    // 没有无父主机时不该凭空多出一个"未分组"空壳分区。
    QCOMPARE(tree->topLevelItemCount(), 1);
    QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("东西湖城运"));
}

void HostManagerWidgetTest::rootReorderIgnoresUngroupedSectionHeader()
{
    HostRecord firstGroup;
    firstGroup.id = QStringLiteral("reorder-group-a");
    firstGroup.kind = HostNodeKind::Folder;
    firstGroup.name = QStringLiteral("第一分组");
    firstGroup.sortOrder = 0;
    QString error;
    QVERIFY2(m_database->saveHost(firstGroup, &error), qPrintable(error));

    HostRecord secondGroup;
    secondGroup.id = QStringLiteral("reorder-group-b");
    secondGroup.kind = HostNodeKind::Folder;
    secondGroup.name = QStringLiteral("第二分组");
    secondGroup.sortOrder = 1;
    QVERIFY2(m_database->saveHost(secondGroup, &error), qPrintable(error));

    HostRecord ungroupedHost;
    ungroupedHost.id = QStringLiteral("reorder-host");
    ungroupedHost.kind = HostNodeKind::Host;
    ungroupedHost.name = QStringLiteral("未分组主机");
    ungroupedHost.host = QStringLiteral("192.0.2.10");
    ungroupedHost.sortOrder = 2;
    QVERIFY2(m_database->saveHost(ungroupedHost, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    auto *tree = widget.findChild<QTreeWidget *>();
    QVERIFY(tree != nullptr);
    QCOMPARE(tree->topLevelItemCount(), 4);

    // 用 take/insert 精确构造 Qt 完成 InternalMove 后的树状态，避免鼠标坐标和平台拖放动画
    // 让用例变得不稳定。此时树中仍保留没有数据库 id 的“未分组”标题，正是原故障条件。
    QTreeWidgetItem *movedGroup = tree->takeTopLevelItem(1);
    QVERIFY(movedGroup != nullptr);
    QCOMPARE(movedGroup->data(0, Qt::UserRole).toString(), secondGroup.id);
    tree->insertTopLevelItem(0, movedGroup);

    error.clear();
    QVERIFY2(widget.reindexChildren(nullptr, &error), qPrintable(error));

    const std::optional<HostRecord> persistedSecond = m_database->findHost(secondGroup.id, &error);
    QVERIFY2(persistedSecond.has_value(), qPrintable(error));
    QCOMPARE(persistedSecond->sortOrder, 0);

    const std::optional<HostRecord> persistedFirst = m_database->findHost(firstGroup.id, &error);
    QVERIFY2(persistedFirst.has_value(), qPrintable(error));
    QCOMPARE(persistedFirst->sortOrder, 1);

    const std::optional<HostRecord> persistedHost = m_database->findHost(ungroupedHost.id, &error);
    QVERIFY2(persistedHost.has_value(), qPrintable(error));
    // 分区标题位于树下标 2，但不对应记录；主机排序号应紧接两个真实分组，而不是被抬到 3。
    QCOMPARE(persistedHost->sortOrder, 2);
}

void HostManagerWidgetTest::sameGroupReorderAcceptsFirstDragMove()
{
    HostRecord group;
    group.id = QStringLiteral("same-group-reorder-folder");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("同组排序");
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostRecord firstHost;
    firstHost.id = QStringLiteral("same-group-reorder-first");
    firstHost.parentId = group.id;
    firstHost.kind = HostNodeKind::Host;
    firstHost.name = QStringLiteral("第一台主机");
    firstHost.host = QStringLiteral("192.0.2.21");
    firstHost.sortOrder = 0;
    QVERIFY2(m_database->saveHost(firstHost, &error), qPrintable(error));

    HostRecord secondHost;
    secondHost.id = QStringLiteral("same-group-reorder-second");
    secondHost.parentId = group.id;
    secondHost.kind = HostNodeKind::Host;
    secondHost.name = QStringLiteral("第二台主机");
    secondHost.host = QStringLiteral("192.0.2.22");
    secondHost.sortOrder = 1;
    QVERIFY2(m_database->saveHost(secondHost, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    widget.resize(420, 520);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));

    auto *tree = widget.findChild<QTreeWidget *>();
    QVERIFY(tree != nullptr);
    QTreeWidgetItem *groupItem = tree->topLevelItem(0);
    QVERIFY(groupItem != nullptr);
    tree->expandItem(groupItem);
    QCoreApplication::processEvents();
    QCOMPARE(groupItem->childCount(), 2);

    QTreeWidgetItem *draggedItem = groupItem->child(0);
    QTreeWidgetItem *targetItem = groupItem->child(1);
    tree->setCurrentItem(draggedItem);

    // 单元测试直接投递拖放事件，没有 QDragManager 可提供 event->source()；InternalMove 会
    // 因而在 dragEnter 阶段把这份数据当成外部来源拒绝。临时改为 DragDrop 仅绕过来源检查，
    // 落点计算、HostTreeWidget::dragMoveEvent() 和业务合法性校验仍走与生产代码相同的路径。
    tree->setDragDropMode(QAbstractItemView::DragDrop);

    // 使用模型正式生成的拖放 MIME 数据，确保基类按真实项目模型与 flags 计算落点。
    // 坐标选择目标行底部边缘，语义是“插入到第二台主机之后”，目标父节点仍应是当前分组。
    // 该事件就是用户开始同组排序后抵达有效插入线的第一个 dragMove；旧实现此时会先读取
    // 尚未更新的 OnItem 状态，把第二台主机误判为容器并直接拒绝。
    const QModelIndex draggedIndex = tree->indexFromItem(draggedItem);
    std::unique_ptr<QMimeData> mimeData(tree->model()->mimeData({draggedIndex}));
    QVERIFY(mimeData != nullptr);
    const QRect targetRect = tree->visualItemRect(targetItem);
    QVERIFY(targetRect.isValid());
    const QPoint dropPosition(targetRect.center().x(), targetRect.bottom() - 1);
    QDragEnterEvent dragEnterEvent(dropPosition,
                                   Qt::MoveAction,
                                   mimeData.get(),
                                   Qt::LeftButton,
                                   Qt::NoModifier);
    QCoreApplication::sendEvent(tree->viewport(), &dragEnterEvent);
    QVERIFY2(dragEnterEvent.isAccepted(), "主机树必须接受自身模型产生的内部拖放数据");
    QDragMoveEvent dragMoveEvent(dropPosition,
                                 Qt::MoveAction,
                                 mimeData.get(),
                                 Qt::LeftButton,
                                 Qt::NoModifier);
    QCoreApplication::sendEvent(tree->viewport(), &dragMoveEvent);

    QVERIFY2(dragMoveEvent.isAccepted(),
             "同组排序的首个有效拖动事件应被接受，不能依赖此前悬停过分组节点");
}

void HostManagerWidgetTest::hostDialogPlacesNameBelowPortWithGroupAndTagEntry()
{
    HostRecord group;
    group.id = QStringLiteral("name-row-group");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("名称行分组");
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    QString failure;
    QTimer::singleShot(0, [&failure]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            failure = QStringLiteral("主机配置窗口未打开");
            return;
        }
        auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("hostRecordNameEdit"));
        auto *address = dialog->findChild<QLineEdit *>(QStringLiteral("hostAddressEdit"));
        auto *port = dialog->findChild<QSpinBox *>(QStringLiteral("hostPortSpin"));
        auto *groupCombo = dialog->findChild<QComboBox *>(
            QStringLiteral("hostRecordGroupCombo"));
        auto *tagButton = dialog->findChild<QToolButton *>(
            QStringLiteral("hostRecordTagButton"));
        if (name == nullptr || address == nullptr || port == nullptr || groupCombo == nullptr
            || tagButton == nullptr) {
            failure = QStringLiteral("名称行缺少分组下拉框或框内标签入口");
            dialog->reject();
            return;
        }

        const int addressTop = address->mapTo(dialog, QPoint{}).y();
        const int portTop = port->mapTo(dialog, QPoint{}).y();
        const int nameTop = name->mapTo(dialog, QPoint{}).y();
        if (!(addressTop < portTop && portTop < nameTop)) {
            failure = QStringLiteral("名称没有排在主机地址与端口的下方");
        }
        if (failure.isEmpty()
            && groupCombo->mapTo(dialog, QPoint{}).x() >= name->mapTo(dialog, QPoint{}).x()) {
            failure = QStringLiteral("分组下拉框没有位于名称输入框左侧");
        }
        // 分组下拉必须真的列出库中分组，否则用户只能看到一个永远选不到目标的空壳。
        if (failure.isEmpty() && groupCombo->findData(QStringLiteral("name-row-group")) < 0) {
            failure = QStringLiteral("分组下拉框没有列出已有分组");
        }
        if (failure.isEmpty() && name->minimumWidth() != 260) {
            failure = QStringLiteral("名称输入框宽度被新增控件改变");
        }
        if (failure.isEmpty()
            && (tagButton->parentWidget() != name || tagButton->text() != QStringLiteral("#tag"))) {
            failure = QStringLiteral("标签入口没有嵌在名称输入框内部");
        }
        // 入口必须贴在输入框右端，并为文本让出等宽的右边距，长名称才不会画到按钮下面。
        if (failure.isEmpty()
            && (tagButton->geometry().right() > name->width()
                || tagButton->geometry().left() < name->width() / 2
                || name->textMargins().right() < tagButton->width())) {
            failure = QStringLiteral("标签入口没有停靠在名称输入框最右侧或未预留文本边距");
        }
        dialog->reject();
    });
    widget.addHost();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
}

void HostManagerWidgetTest::tagEditorPersistsTagWithChosenColorAndOffersHistory()
{
    HostManagerWidget widget(m_database.get(), m_vault.get());
    QString failure;
    QString chosenColor;
    QTimer::singleShot(0, [&failure, &chosenColor]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        auto *name = dialog == nullptr
            ? nullptr
            : dialog->findChild<QLineEdit *>(QStringLiteral("hostRecordNameEdit"));
        auto *address = dialog == nullptr
            ? nullptr
            : dialog->findChild<QLineEdit *>(QStringLiteral("hostAddressEdit"));
        auto *tagButton = dialog == nullptr
            ? nullptr
            : dialog->findChild<QToolButton *>(QStringLiteral("hostRecordTagButton"));
        auto *save = dialog == nullptr
            ? nullptr
            : dialog->findChild<QPushButton *>(QStringLiteral("hostRecordSaveButton"));
        if (name == nullptr || address == nullptr || tagButton == nullptr || save == nullptr) {
            failure = QStringLiteral("主机配置窗口缺少标签入口或保存按钮");
            if (dialog != nullptr) {
                dialog->reject();
            }
            return;
        }

        // 标签窗口以模态方式弹在主机窗口之上；先排好回调再点击，回调会在其嵌套事件循环里执行。
        QTimer::singleShot(0, [&failure, &chosenColor]() {
            auto *tagDialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            auto *tagName = tagDialog == nullptr
                ? nullptr
                : tagDialog->findChild<QLineEdit *>(QStringLiteral("hostTagNameEdit"));
            auto *chooser = tagDialog == nullptr
                ? nullptr
                : tagDialog->findChild<QWidget *>(QStringLiteral("hostTagColorChooser"));
            auto *accept = tagDialog == nullptr
                ? nullptr
                : tagDialog->findChild<QPushButton *>(QStringLiteral("hostTagAcceptButton"));
            if (tagName == nullptr || chooser == nullptr || accept == nullptr) {
                failure = QStringLiteral("标签窗口缺少名称、配色或确定入口");
                if (tagDialog != nullptr) {
                    tagDialog->reject();
                }
                return;
            }

            const QList<QAbstractButton *> swatches = chooser->findChildren<QAbstractButton *>();
            if (swatches.size() < 4) {
                failure = QStringLiteral("标签窗口没有提供可选配色");
                tagDialog->reject();
                return;
            }
            // 故意不取第一个色块：默认色与强调色相同，选它无法证明用户选择真的被采用。
            QAbstractButton *picked = swatches.at(3);
            picked->setChecked(true);
            chosenColor = picked->toolTip();
            tagName->setText(QStringLiteral("生产"));
            // 添加按钮和中间提交步骤都已去掉：回车与点击"确定"等价，直接按名称框和颜色
            // 选择器的当前内容确认并关闭窗口。
            QTest::keyClick(tagName, Qt::Key_Return);
        });
        tagButton->click();
        if (!failure.isEmpty()) {
            dialog->reject();
            return;
        }
        if (tagButton->text() != QStringLiteral("#生产")) {
            failure = QStringLiteral("名称框内的入口没有回显已选标签");
            dialog->reject();
            return;
        }

        address->setText(QStringLiteral("192.0.2.44"));
        name->setText(QStringLiteral("带标签的主机"));
        save->click();
    });
    widget.addHost();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY(!chosenColor.isEmpty());

    bool ok = false;
    QString error;
    const QList<HostRecord> saved =
        m_database->hostsWithTag(QStringLiteral("生产"), &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(saved.size(), 1);
    QCOMPARE(saved.constFirst().name, QStringLiteral("带标签的主机"));

    // 徽章颜色来自全局配色表；主机树必须按用户所选颜色而不是固定强调色绘制。
    QTreeWidgetItem *item = findItem(widget, QStringLiteral("带标签的主机"));
    QVERIFY(item != nullptr);
    const QColor badgeColor = qvariant_cast<QColor>(item->data(0, Qt::UserRole + 4));
    QCOMPARE(badgeColor.name(), chosenColor);

    // 另一台主机打开标签窗口时，必须能直接从历史候选里复用同一个标签，而不是重新拼写。
    QString historyFailure;
    QTimer::singleShot(0, [&historyFailure]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        auto *tagButton = dialog == nullptr
            ? nullptr
            : dialog->findChild<QToolButton *>(QStringLiteral("hostRecordTagButton"));
        if (tagButton == nullptr) {
            historyFailure = QStringLiteral("第二台主机的配置窗口缺少标签入口");
            if (dialog != nullptr) {
                dialog->reject();
            }
            return;
        }
        QTimer::singleShot(0, [&historyFailure]() {
            auto *tagDialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            auto *history = tagDialog == nullptr
                ? nullptr
                : tagDialog->findChild<QWidget *>(QStringLiteral("hostTagHistoryFlow"));
            if (history == nullptr) {
                historyFailure = QStringLiteral("标签窗口缺少历史标签流式布局");
            } else {
                const QList<QToolButton *> chips =
                    history->findChildren<QToolButton *>(QStringLiteral("hostTagChip"));
                const bool found = std::any_of(chips.begin(), chips.end(), [](const QToolButton *chip) {
                    return chip->text() == QStringLiteral("#生产");
                });
                if (!found) {
                    historyFailure = QStringLiteral("历史标签胶囊没有列出已在别处使用过的标签");
                }
            }
            if (tagDialog != nullptr) {
                tagDialog->reject();
            }
        });
        tagButton->click();
        dialog->reject();
    });
    widget.addHost();
    QVERIFY2(historyFailure.isEmpty(), qPrintable(historyFailure));
}

void HostManagerWidgetTest::groupComboMovesHostIntoSelectedGroup()
{
    HostRecord group;
    group.id = QStringLiteral("target-group-id");
    group.kind = HostNodeKind::Folder;
    group.name = QStringLiteral("目标分组");
    QString error;
    QVERIFY2(m_database->saveHost(group, &error), qPrintable(error));

    HostRecord host;
    host.id = QStringLiteral("regrouped-host-id");
    host.kind = HostNodeKind::Host;
    host.name = QStringLiteral("待改组主机");
    host.host = QStringLiteral("192.0.2.55");
    QVERIFY2(m_database->saveHost(host, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    auto *tree = widget.findChild<QTreeWidget *>();
    QVERIFY(tree != nullptr);
    QTreeWidgetItem *item = findItem(widget, host.name);
    QVERIFY(item != nullptr);
    tree->setCurrentItem(item);

    QString failure;
    QTimer::singleShot(0, [&failure]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        auto *groupCombo = dialog == nullptr
            ? nullptr
            : dialog->findChild<QComboBox *>(QStringLiteral("hostRecordGroupCombo"));
        auto *save = dialog == nullptr
            ? nullptr
            : dialog->findChild<QPushButton *>(QStringLiteral("hostRecordSaveButton"));
        if (groupCombo == nullptr || save == nullptr) {
            failure = QStringLiteral("编辑窗口缺少分组下拉框或保存按钮");
            if (dialog != nullptr) {
                dialog->reject();
            }
            return;
        }
        const int targetIndex = groupCombo->findData(QStringLiteral("target-group-id"));
        if (targetIndex < 0) {
            failure = QStringLiteral("分组下拉框没有列出目标分组");
            dialog->reject();
            return;
        }
        groupCombo->setCurrentIndex(targetIndex);
        save->click();
    });
    widget.editSelected();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));

    const std::optional<HostRecord> stored = m_database->findHost(host.id, &error);
    QVERIFY2(stored.has_value(), qPrintable(error));
    QCOMPARE(stored->parentId, QStringLiteral("target-group-id"));

    QTreeWidgetItem *movedItem = findItem(widget, host.name);
    QVERIFY(movedItem != nullptr);
    QVERIFY2(movedItem->parent() != nullptr
                 && movedItem->parent()->text(0) == QStringLiteral("目标分组"),
             "改组保存后主机没有出现在目标分组下");
}

void HostManagerWidgetTest::groupComboOrderMatchesSidebarTreeOrder()
{
    // sortOrder 故意与名称字母序相反：侧边栏应按 sortOrder 显示 Zeta 在前、Alpha 在后，
    // 而按字母排序会得到相反结果——用这个反例才能分辨下拉框到底依据哪一种顺序。
    HostRecord zeta;
    zeta.id = QStringLiteral("zeta-group-id");
    zeta.kind = HostNodeKind::Folder;
    zeta.name = QStringLiteral("Zeta");
    zeta.sortOrder = 0;
    QString error;
    QVERIFY2(m_database->saveHost(zeta, &error), qPrintable(error));

    HostRecord alpha;
    alpha.id = QStringLiteral("alpha-group-id");
    alpha.kind = HostNodeKind::Folder;
    alpha.name = QStringLiteral("Alpha");
    alpha.sortOrder = 1;
    QVERIFY2(m_database->saveHost(alpha, &error), qPrintable(error));

    HostRecord host;
    host.id = QStringLiteral("order-check-host-id");
    host.kind = HostNodeKind::Host;
    host.name = QStringLiteral("顺序检查主机");
    host.host = QStringLiteral("192.0.2.60");
    QVERIFY2(m_database->saveHost(host, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    QTreeWidgetItem *item = findItem(widget, host.name);
    QVERIFY(item != nullptr);
    widget.findChild<QTreeWidget *>()->setCurrentItem(item);

    QString failure;
    QStringList observedOrder;
    QTimer::singleShot(0, [&failure, &observedOrder]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        auto *groupCombo = dialog == nullptr
            ? nullptr
            : dialog->findChild<QComboBox *>(QStringLiteral("hostRecordGroupCombo"));
        if (groupCombo == nullptr) {
            failure = QStringLiteral("编辑窗口缺少分组下拉框");
            if (dialog != nullptr) {
                dialog->reject();
            }
            return;
        }
        for (int i = 0; i < groupCombo->count(); ++i) {
            observedOrder.append(groupCombo->itemText(i));
        }
        dialog->reject();
    });
    widget.editSelected();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));

    QCOMPARE(observedOrder,
             QStringList({QStringLiteral("Ungrouped"), QStringLiteral("Zeta"), QStringLiteral("Alpha")}));
}

void HostManagerWidgetTest::detectedDistributionDrivesHostIconAndTooltip()
{
    HostRecord detected;
    detected.id = QStringLiteral("distribution-detected-host");
    detected.kind = HostNodeKind::Host;
    detected.name = QStringLiteral("已识别系统的主机");
    detected.host = QStringLiteral("192.0.2.21");
    detected.username = QStringLiteral("root");
    detected.port = 22;
    detected.distributionId = QStringLiteral("ubuntu");
    detected.distributionVersionId = QStringLiteral("24.04");
    QString error;
    QVERIFY2(m_database->saveHost(detected, &error), qPrintable(error));

    HostRecord undetected;
    undetected.id = QStringLiteral("distribution-unknown-host");
    undetected.kind = HostNodeKind::Host;
    undetected.name = QStringLiteral("尚未登录过的主机");
    undetected.host = QStringLiteral("192.0.2.22");
    undetected.username = QStringLiteral("root");
    undetected.port = 22;
    QVERIFY2(m_database->saveHost(undetected, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    QTreeWidgetItem *detectedItem = findItem(widget, detected.name);
    QTreeWidgetItem *undetectedItem = findItem(widget, undetected.name);
    QVERIFY(detectedItem != nullptr);
    QVERIFY(undetectedItem != nullptr);

    // 比较最终位图而不是 QIcon 实例：图标每次都会重新绘制，只有像素才能证明用户确实
    // 看到了 Ubuntu 图形而不是通用服务器轮廓。
    const QSize comparisonSize(20, 20);
    const QImage detectedPixels = detectedItem->icon(0).pixmap(comparisonSize).toImage();
    const QImage genericPixels =
        UiIcons::symbol(&widget,
                        UiIcons::Symbol::HostItem,
                        widget.palette().color(QPalette::WindowText))
            .pixmap(comparisonSize)
            .toImage();
    QCOMPARE(detectedPixels,
             HostDistributionIcon::iconFor(QStringLiteral("ubuntu"))
                 .pixmap(comparisonSize)
                 .toImage());
    QVERIFY2(detectedPixels != genericPixels, "已识别发行版仍在显示通用主机图标");
    QCOMPARE(undetectedItem->icon(0).pixmap(comparisonSize).toImage(), genericPixels);

    QVERIFY2(detectedItem->toolTip(0).contains(QStringLiteral("Ubuntu 24.04")),
             qPrintable(QStringLiteral("悬停提示缺少发行版本：%1").arg(detectedItem->toolTip(0))));
    QVERIFY2(detectedItem->toolTip(0).contains(QStringLiteral("root@192.0.2.21:22")),
             "发行版信息不应挤掉原有的连接地址提示");
    QVERIFY2(!undetectedItem->toolTip(0).contains(QStringLiteral("系统：")),
             "尚未识别的主机不应出现空的系统行");
}

void HostManagerWidgetTest::domesticDistributionKeepsLinuxIconInsteadOfGenericHost()
{
    HostRecord host;
    host.id = QStringLiteral("distribution-unlisted-host");
    host.kind = HostNodeKind::Host;
    host.name = QStringLiteral("私有发行版主机");
    host.host = QStringLiteral("192.0.2.23");
    // 麒麟 V10 使用 kylin ID；识别成功后应显示麒麟专用图标而不是通用企鹅。
    host.distributionId = QStringLiteral("kylin");
    host.distributionVersionId = QStringLiteral("V10");
    QString error;
    QVERIFY2(m_database->saveHost(host, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    QTreeWidgetItem *item = findItem(widget, host.name);
    QVERIFY(item != nullptr);

    const QSize comparisonSize(20, 20);
    const QImage pixels = item->icon(0).pixmap(comparisonSize).toImage();
    QCOMPARE(pixels,
             HostDistributionIcon::iconFor(QStringLiteral("kylin"))
                 .pixmap(comparisonSize)
                 .toImage());
    QVERIFY2(pixels
                 != UiIcons::symbol(&widget,
                                    UiIcons::Symbol::HostItem,
                                    widget.palette().color(QPalette::WindowText))
                        .pixmap(comparisonSize)
                        .toImage(),
             "未收录的发行版不应退回通用主机图标");
    // 展示名称使用人类可读的规范名称，同时保留版本号，便于诊断远端系统。
    QVERIFY2(item->toolTip(0).contains(QStringLiteral("Kylin Linux V10")),
             qPrintable(item->toolTip(0)));
}

void HostManagerWidgetTest::editingConnectionTargetClearsCachedDistribution()
{
    HostRecord host;
    host.id = QStringLiteral("distribution-retarget-host");
    host.kind = HostNodeKind::Host;
    host.name = QStringLiteral("换机器的主机");
    host.host = QStringLiteral("192.0.2.24");
    host.username = QStringLiteral("root");
    host.port = 22;
    host.distributionId = QStringLiteral("ubuntu");
    host.distributionVersionId = QStringLiteral("24.04");
    QString error;
    QVERIFY2(m_database->saveHost(host, &error), qPrintable(error));

    HostManagerWidget widget(m_database.get(), m_vault.get());
    auto *tree = widget.findChild<QTreeWidget *>();
    QVERIFY(tree != nullptr);
    QTreeWidgetItem *item = findItem(widget, host.name);
    QVERIFY(item != nullptr);
    tree->setCurrentItem(item);

    // 第一次编辑只改名称：目标服务器没变，已识别的发行版必须原样保留，否则每次改备注
    // 都会白白丢掉一次远端探测结果。
    QString dialogFailure;
    QTimer::singleShot(0, [&dialogFailure]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        auto *nameEdit = dialog == nullptr
            ? nullptr
            : dialog->findChild<QLineEdit *>(QStringLiteral("hostRecordNameEdit"));
        auto *saveButton = dialog == nullptr
            ? nullptr
            : dialog->findChild<QPushButton *>(QStringLiteral("hostRecordSaveButton"));
        if (nameEdit == nullptr || saveButton == nullptr) {
            dialogFailure = QStringLiteral("主机配置窗口缺少名称输入框或保存按钮");
            if (dialog != nullptr) {
                dialog->reject();
            }
            return;
        }
        nameEdit->setText(QStringLiteral("只是改了备注"));
        saveButton->click();
    });
    widget.editSelected();
    QVERIFY2(dialogFailure.isEmpty(), qPrintable(dialogFailure));

    std::optional<HostRecord> afterRename = m_database->findHost(host.id, &error);
    QVERIFY2(afterRename.has_value(), qPrintable(error));
    QCOMPARE(afterRename->distributionId, QStringLiteral("ubuntu"));
    QCOMPARE(afterRename->distributionVersionId, QStringLiteral("24.04"));

    QTreeWidgetItem *renamedItem = findItem(widget, QStringLiteral("只是改了备注"));
    QVERIFY(renamedItem != nullptr);
    tree->setCurrentItem(renamedItem);

    // 第二次编辑改掉地址：记录已经指向另一台服务器，缓存的 Ubuntu 图标必须失效。
    QTimer::singleShot(0, [&dialogFailure]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        auto *hostEdit = dialog == nullptr
            ? nullptr
            : dialog->findChild<QLineEdit *>(QStringLiteral("hostAddressEdit"));
        auto *saveButton = dialog == nullptr
            ? nullptr
            : dialog->findChild<QPushButton *>(QStringLiteral("hostRecordSaveButton"));
        if (hostEdit == nullptr || saveButton == nullptr) {
            dialogFailure = QStringLiteral("主机配置窗口缺少地址输入框或保存按钮");
            if (dialog != nullptr) {
                dialog->reject();
            }
            return;
        }
        hostEdit->setText(QStringLiteral("192.0.2.25"));
        saveButton->click();
    });
    widget.editSelected();
    QVERIFY2(dialogFailure.isEmpty(), qPrintable(dialogFailure));

    const std::optional<HostRecord> afterRetarget = m_database->findHost(host.id, &error);
    QVERIFY2(afterRetarget.has_value(), qPrintable(error));
    QCOMPARE(afterRetarget->host, QStringLiteral("192.0.2.25"));
    QVERIFY2(afterRetarget->distributionId.isEmpty(), "换了服务器后仍保留旧发行版标识");
    QVERIFY2(afterRetarget->distributionVersionId.isEmpty(), "换了服务器后仍保留旧发行版本号");

    QTreeWidgetItem *retargetedItem = findItem(widget, QStringLiteral("只是改了备注"));
    QVERIFY(retargetedItem != nullptr);
    const QSize comparisonSize(20, 20);
    QCOMPARE(retargetedItem->icon(0).pixmap(comparisonSize).toImage(),
             UiIcons::symbol(&widget,
                             UiIcons::Symbol::HostItem,
                             widget.palette().color(QPalette::WindowText))
                 .pixmap(comparisonSize)
                 .toImage());
}

void HostManagerWidgetTest::remoteDistributionParserRejectsUnsafeOutput()
{
    QString distributionId;
    QString versionId;

    QVERIFY(SshSessionStack::parseRemoteDistributionOutput(
        QByteArrayLiteral("ubuntu\n24.04\n"), &distributionId, &versionId));
    QCOMPARE(distributionId, QStringLiteral("ubuntu"));
    QCOMPARE(versionId, QStringLiteral("24.04"));

    // 大小写与首尾空白由解析层归一，图标映射与数据库因此只需处理一种形态。
    QVERIFY(SshSessionStack::parseRemoteDistributionOutput(
        QByteArrayLiteral(" Rocky \r\n 9.4 \r\n"), &distributionId, &versionId));
    QCOMPARE(distributionId, QStringLiteral("rocky"));
    QCOMPARE(versionId, QStringLiteral("9.4"));

    // 滚动发行版没有 VERSION_ID，缺版本行不算失败。
    QVERIFY(SshSessionStack::parseRemoteDistributionOutput(
        QByteArrayLiteral("arch\n"), &distributionId, &versionId));
    QCOMPARE(distributionId, QStringLiteral("arch"));
    QVERIFY(versionId.isEmpty());

    for (const QByteArray &rejected : {QByteArrayLiteral(""),
                                       QByteArrayLiteral("\n9.4\n"),
                                       QByteArrayLiteral("ubuntu; rm -rf /\n24.04\n"),
                                       QByteArrayLiteral("ubun\x1b[31mtu\n"),
                                       QByteArrayLiteral("-ubuntu\n"),
                                       QByteArray(200, 'a') + QByteArrayLiteral("\n")}) {
        QString rejectedId = QStringLiteral("unchanged");
        QString rejectedVersion = QStringLiteral("unchanged");
        QVERIFY2(!SshSessionStack::parseRemoteDistributionOutput(
                     rejected, &rejectedId, &rejectedVersion),
                 qPrintable(QStringLiteral("非法输出被当成合法发行版：%1")
                                .arg(QString::fromUtf8(rejected.toPercentEncoding()))));
        QCOMPARE(rejectedId, QStringLiteral("unchanged"));
        QCOMPARE(rejectedVersion, QStringLiteral("unchanged"));
    }

    // 超长版本号必须被截断而不是原样落库，避免副标题与 Tooltip 被远端任意拉长。
    QVERIFY(SshSessionStack::parseRemoteDistributionOutput(
        QByteArrayLiteral("debian\n") + QByteArray(200, '9'), &distributionId, &versionId));
    QCOMPARE(distributionId, QStringLiteral("debian"));
    QCOMPARE(versionId.size(), 64);
}

QTEST_MAIN(HostManagerWidgetTest)

#include "HostManagerWidgetTest.moc"
