#include "ui/HostManagerWidget.h"

#include "core/ConnectionConfigCodec.h"
#include "security/Vault.h"
#include "storage/ApplicationDatabase.h"
#include "ui/UiIcons.h"
#include "ui/VaultDialog.h"

#include <QAction>
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSaveFile>
#include <QSpinBox>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUuid>
#include <QVBoxLayout>

namespace {

/** @brief 树节点保存数据库主键的角色，避开 Qt 内置显示和图标角色。 */
constexpr int HostIdRole = Qt::UserRole;

/** @brief 树节点保存 HostNodeKind 数值的角色，用于阻止分组被误连接。 */
constexpr int HostKindRole = Qt::UserRole + 1;

/** @brief 最近连接列表保存稳定主机 id 的角色。 */
constexpr int RecentHostIdRole = Qt::UserRole;

/**
 * @brief 编辑一条持久化主机或分组记录的内部对话框。
 *
 * 主机和分组共用 HostRecord，分组中的端口、用户名、私钥与 ProxyJump 会被子节点继承。
 * 因此不能只让分组编辑名称；本对话框显式提供全部可继承字段，并用“继承父分组端口”复选框
 * 保留 std::optional 端口的三态语义。密码只作为新建后立即连接的会话值返回，绝不写入记录。
 */
class HostRecordDialog final : public QDialog {
public:
    /**
     * @brief 创建记录编辑器。
     * @param initial 待编辑快照；新建时调用方应先填好 kind、id 和 parentId。
     * @param parent Qt 父对象。
     */
    explicit HostRecordDialog(const HostRecord &initial, QWidget *parent = nullptr)
        : QDialog(parent)
        , m_initial(initial)
        , m_nameEdit(new QLineEdit(initial.name, this))
        , m_hostEdit(new QLineEdit(initial.host, this))
        , m_portSpin(new QSpinBox(this))
        , m_inheritPortCheck(new QCheckBox(tr("继承父分组端口"), this))
        , m_usernameEdit(new QLineEdit(initial.username, this))
        , m_identityEdit(new QLineEdit(initial.identityFile, this))
        , m_proxyJumpsEdit(new QLineEdit(initial.proxyJumps.join(QLatin1Char(',')), this))
        , m_tagsEdit(new QLineEdit(initial.tags.join(QLatin1Char(',')), this))
        , m_passwordEdit(new QLineEdit(this))
        , m_savePasswordCheck(new QCheckBox(tr("加密保存到本地保险库"), this))
        , m_legacyHmacCheck(new QCheckBox(tr("仅为该主机允许 hmac-sha1（不继承）"), this))
    {
        const bool isHost = initial.kind == HostNodeKind::Host;
        setWindowTitle(isHost ? tr("主机配置") : tr("分组配置"));
        setMinimumWidth(500);

        m_portSpin->setRange(1, 65535);
        m_portSpin->setValue(initial.port.value_or(quint16{22}));
        m_inheritPortCheck->setChecked(!initial.port.has_value());
        m_portSpin->setEnabled(initial.port.has_value());
        m_usernameEdit->setPlaceholderText(tr("留空则继承父分组"));
        m_identityEdit->setPlaceholderText(tr("留空则继承父分组或使用 Agent/默认密钥"));
        m_proxyJumpsEdit->setPlaceholderText(tr("逗号分隔；留空则继承父分组"));
        m_tagsEdit->setPlaceholderText(tr("逗号分隔；会与祖先分组标签合并"));
        m_passwordEdit->setEchoMode(QLineEdit::Password);
        m_passwordEdit->setPlaceholderText(tr("仅用于本次立即连接；保存由保险库负责"));
        m_savePasswordCheck->setChecked(!initial.credentialId.isEmpty());
        m_legacyHmacCheck->setChecked(initial.allowLegacyHmacSha1);

        auto *identityContainer = new QWidget(this);
        auto *identityLayout = new QHBoxLayout(identityContainer);
        identityLayout->setContentsMargins(0, 0, 0, 0);
        auto *browseButton = new QPushButton(tr("浏览…"), identityContainer);
        UiIcons::decorateButton(
            browseButton,
            UiIcons::themed(this, QStringLiteral("document-open"), QStyle::SP_DialogOpenButton),
            tr("选择 SSH 私钥文件"));
        identityLayout->addWidget(m_identityEdit, 1);
        identityLayout->addWidget(browseButton);

        auto *portContainer = new QWidget(this);
        auto *portLayout = new QHBoxLayout(portContainer);
        portLayout->setContentsMargins(0, 0, 0, 0);
        portLayout->addWidget(m_portSpin);
        portLayout->addWidget(m_inheritPortCheck, 1);

        auto *form = new QFormLayout;
        form->addRow(tr("名称："), m_nameEdit);
        if (isHost) {
            form->addRow(tr("主机地址："), m_hostEdit);
        }
        form->addRow(tr("端口："), portContainer);
        form->addRow(tr("用户名："), m_usernameEdit);
        form->addRow(tr("私钥文件："), identityContainer);
        form->addRow(tr("ProxyJump："), m_proxyJumpsEdit);
        form->addRow(tr("标签："), m_tagsEdit);
        if (isHost) {
            form->addRow(tr("会话密码/口令："), m_passwordEdit);
            form->addRow(tr("凭据存储："), m_savePasswordCheck);
            form->addRow(tr("旧版兼容："), m_legacyHmacCheck);
        }

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                             this);
        auto *layout = new QVBoxLayout(this);
        layout->addLayout(form);
        layout->addWidget(buttons);

        connect(m_inheritPortCheck, &QCheckBox::toggled,
                m_portSpin, [this](const bool inherit) { m_portSpin->setEnabled(!inherit); });
        connect(browseButton, &QPushButton::clicked, this, [this]() {
            const QString path = QFileDialog::getOpenFileName(this, tr("选择 SSH 私钥文件"));
            if (!path.isEmpty()) {
                m_identityEdit->setText(path);
            }
        });
        connect(buttons, &QDialogButtonBox::accepted, this, [this, isHost]() {
            if (m_nameEdit->text().trimmed().isEmpty()) {
                QMessageBox::warning(this, tr("配置不完整"), tr("请输入名称。"));
                m_nameEdit->setFocus();
                return;
            }
            if (isHost && m_hostEdit->text().trimmed().isEmpty()) {
                QMessageBox::warning(this, tr("配置不完整"), tr("请输入主机名或 IP 地址。"));
                m_hostEdit->setFocus();
                return;
            }
            QDialog::accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    }

    /** @brief 返回用户确认后的记录；id、kind、parentId、排序和创建时间保持不变。 */
    [[nodiscard]] HostRecord record() const
    {
        HostRecord result = m_initial;
        result.name = m_nameEdit->text().trimmed();
        result.host = m_hostEdit->text().trimmed();
        result.port = m_inheritPortCheck->isChecked()
            ? std::optional<quint16>{}
            : std::optional<quint16>{static_cast<quint16>(m_portSpin->value())};
        result.username = m_usernameEdit->text().trimmed();
        result.identityFile = m_identityEdit->text().trimmed();
        result.proxyJumps = splitList(m_proxyJumpsEdit->text());
        result.tags = splitList(m_tagsEdit->text());
        result.allowLegacyHmacSha1 = result.kind == HostNodeKind::Host
            && m_legacyHmacCheck->isChecked();
        return result;
    }

    /** @brief 返回只用于紧随保存之后首次连接的明文，不属于 HostRecord 持久化数据。 */
    [[nodiscard]] QString sessionPassword() const
    {
        return m_passwordEdit->text();
    }

    /**
     * @brief 用户是否要求把本次输入加密保存到保险库。
     * @return 只有主机记录且复选框选中时返回 true；分组永远不直接保存密码。
     */
    [[nodiscard]] bool shouldSavePassword() const
    {
        return m_initial.kind == HostNodeKind::Host && m_savePasswordCheck->isChecked();
    }

    /**
     * @brief 用户是否明确取消了一条既有凭据引用。
     * @return 原记录有关联凭据且复选框被取消时返回 true。
     */
    [[nodiscard]] bool shouldDetachCredential() const
    {
        return !m_initial.credentialId.isEmpty() && !m_savePasswordCheck->isChecked();
    }

private:
    /**
     * @brief 把逗号列表规范化为去空白、去空项、去重复的字符串列表。
     * @param text 用户输入文本。
     * @return 保留首次出现顺序的规范化列表。
     */
    [[nodiscard]] static QStringList splitList(const QString &text)
    {
        QStringList result;
        for (const QString &part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString normalized = part.trimmed();
            if (!normalized.isEmpty() && !result.contains(normalized)) {
                result.append(normalized);
            }
        }
        return result;
    }

    HostRecord m_initial;
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QCheckBox *m_inheritPortCheck = nullptr;
    QLineEdit *m_usernameEdit = nullptr;
    QLineEdit *m_identityEdit = nullptr;
    QLineEdit *m_proxyJumpsEdit = nullptr;
    QLineEdit *m_tagsEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QCheckBox *m_savePasswordCheck = nullptr;
    QCheckBox *m_legacyHmacCheck = nullptr;
};

} // namespace

HostManagerWidget::HostManagerWidget(ApplicationDatabase *database,
                                     CredentialVault *credentialVault,
                                     QWidget *parent)
    : QWidget(parent)
    , m_database(database)
    , m_credentialVault(credentialVault)
    , m_searchEdit(new QLineEdit(this))
    , m_tree(new QTreeWidget(this))
    , m_recentList(new QListWidget(this))
{
    Q_ASSERT(m_database != nullptr);
    Q_ASSERT(m_credentialVault != nullptr);

    m_searchEdit->setPlaceholderText(tr("搜索名称/地址；输入 #标签 精确筛选"));
    m_searchEdit->setClearButtonEnabled(true);

    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_recentList->setContextMenuPolicy(Qt::CustomContextMenu);

    auto *addGroupButton = new QPushButton(tr("新建分组"), this);
    auto *addHostButton = new QPushButton(tr("新建主机"), this);
    auto *editButton = new QPushButton(tr("编辑"), this);
    auto *deleteButton = new QPushButton(tr("删除"), this);
    auto *refreshButton = new QPushButton(tr("刷新"), this);
    auto *importButton = new QPushButton(tr("导入"), this);
    auto *exportButton = new QPushButton(tr("导出"), this);
    UiIcons::decorateButton(
        addGroupButton,
        UiIcons::themed(this, QStringLiteral("folder-new"), QStyle::SP_FileDialogNewFolder),
        tr("在当前层级新建主机分组"));
    UiIcons::decorateButton(
        addHostButton,
        UiIcons::themed(this, QStringLiteral("list-add"), QStyle::SP_DriveNetIcon),
        tr("在当前分组中新建 SSH 主机"));
    UiIcons::decorateButton(
        editButton,
        UiIcons::themed(this, QStringLiteral("document-edit"), QStyle::SP_FileDialogDetailedView),
        tr("编辑当前选中的主机或分组"));
    UiIcons::decorateButton(
        deleteButton,
        UiIcons::themed(this, QStringLiteral("edit-delete"), QStyle::SP_TrashIcon),
        tr("删除当前选中的主机或分组"));
    UiIcons::decorateButton(
        refreshButton,
        UiIcons::themed(this, QStringLiteral("view-refresh"), QStyle::SP_BrowserReload),
        tr("重新读取主机与最近连接"));
    UiIcons::decorateButton(
        importButton,
        UiIcons::themed(this, QStringLiteral("document-open"), QStyle::SP_DialogOpenButton),
        tr("从 OpenSSH 配置或 Shellium JSON 导入"));
    UiIcons::decorateButton(
        exportButton,
        UiIcons::themed(this, QStringLiteral("document-save"), QStyle::SP_DialogSaveButton),
        tr("导出不含凭据的主机配置"));

    auto *buttonLayout = new QHBoxLayout;
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->addWidget(addGroupButton);
    buttonLayout->addWidget(addHostButton);
    buttonLayout->addWidget(editButton);
    buttonLayout->addWidget(deleteButton);
    buttonLayout->addWidget(refreshButton);

    auto *transferLayout = new QHBoxLayout;
    transferLayout->setContentsMargins(0, 0, 0, 0);
    transferLayout->addWidget(importButton);
    transferLayout->addWidget(exportButton);
    transferLayout->addStretch(1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(m_searchEdit);
    layout->addLayout(buttonLayout);
    layout->addLayout(transferLayout);
    layout->addWidget(new QLabel(tr("主机与分组"), this));
    layout->addWidget(m_tree, 1);
    layout->addWidget(new QLabel(tr("最近连接"), this));
    m_recentList->setMaximumHeight(150);
    layout->addWidget(m_recentList);

    connect(m_searchEdit, &QLineEdit::textChanged, this, [this]() { applyFilter(); });
    connect(addGroupButton, &QPushButton::clicked, this, &HostManagerWidget::addGroup);
    connect(addHostButton, &QPushButton::clicked, this, &HostManagerWidget::addHost);
    connect(editButton, &QPushButton::clicked, this, &HostManagerWidget::editSelected);
    connect(deleteButton, &QPushButton::clicked, this, &HostManagerWidget::removeSelected);
    connect(refreshButton, &QPushButton::clicked, this, &HostManagerWidget::refresh);
    connect(importButton, &QPushButton::clicked, this, &HostManagerWidget::importHosts);
    connect(exportButton, &QPushButton::clicked, this, &HostManagerWidget::exportHosts);
    connect(m_tree, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem *item, const int) { activateTreeItem(item); });
    connect(m_recentList, &QListWidget::itemDoubleClicked,
            this, &HostManagerWidget::activateRecentItem);
    connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        QTreeWidgetItem *item = m_tree->itemAt(position);
        if (item != nullptr) {
            m_tree->setCurrentItem(item);
        }

        QMenu menu(this);
        const bool isHost = item != nullptr
            && static_cast<HostNodeKind>(item->data(0, HostKindRole).toInt())
                == HostNodeKind::Host;
        if (isHost) {
            QAction *connectAction = menu.addAction(
                UiIcons::themed(this, QStringLiteral("network-connect"), QStyle::SP_DriveNetIcon),
                tr("连接"));
            connect(connectAction, &QAction::triggered,
                    this, [this, item]() { activateTreeItem(item); });
            menu.addSeparator();
        }
        QAction *addGroupAction = menu.addAction(
            UiIcons::themed(this, QStringLiteral("folder-new"), QStyle::SP_FileDialogNewFolder),
            tr("新建分组"));
        QAction *addHostAction = menu.addAction(
            UiIcons::themed(this, QStringLiteral("list-add"), QStyle::SP_DriveNetIcon),
            tr("新建主机"));
        connect(addGroupAction, &QAction::triggered, this, &HostManagerWidget::addGroup);
        connect(addHostAction, &QAction::triggered, this, &HostManagerWidget::addHost);
        if (item != nullptr) {
            menu.addSeparator();
            QAction *editAction = menu.addAction(
                UiIcons::themed(this, QStringLiteral("document-edit"),
                                QStyle::SP_FileDialogDetailedView),
                tr("编辑"));
            QAction *deleteAction = menu.addAction(
                UiIcons::themed(this, QStringLiteral("edit-delete"), QStyle::SP_TrashIcon),
                tr("删除"));
            connect(editAction, &QAction::triggered, this, &HostManagerWidget::editSelected);
            connect(deleteAction, &QAction::triggered, this, &HostManagerWidget::removeSelected);
        }
        menu.addSeparator();
        QAction *refreshAction = menu.addAction(
            UiIcons::themed(this, QStringLiteral("view-refresh"), QStyle::SP_BrowserReload),
            tr("刷新"));
        connect(refreshAction, &QAction::triggered, this, &HostManagerWidget::refresh);
        menu.exec(m_tree->viewport()->mapToGlobal(position));
    });
    connect(m_recentList,
            &QWidget::customContextMenuRequested,
            this,
            [this](const QPoint &position) {
                QListWidgetItem *item = m_recentList->itemAt(position);
                if (item == nullptr) {
                    return;
                }
                m_recentList->setCurrentItem(item);
                QMenu menu(this);
                QAction *connectAction = menu.addAction(
                    UiIcons::themed(this, QStringLiteral("network-connect"),
                                    QStyle::SP_DriveNetIcon),
                    tr("重新连接"));
                QAction *refreshAction = menu.addAction(
                    UiIcons::themed(this, QStringLiteral("view-refresh"),
                                    QStyle::SP_BrowserReload),
                    tr("刷新最近连接"));
                connect(connectAction, &QAction::triggered,
                        this, [this, item]() { activateRecentItem(item); });
                connect(refreshAction, &QAction::triggered, this, &HostManagerWidget::refresh);
                menu.exec(m_recentList->viewport()->mapToGlobal(position));
            });

    refresh();
}

void HostManagerWidget::refresh()
{
    applyFilter();

    m_recentList->clear();
    bool ok = false;
    QString error;
    const QList<ConnectionHistoryEntry> history =
        m_database->recentConnections(30, {}, &ok, &error);
    if (!ok) {
        showDatabaseError(tr("读取最近连接"), error);
        return;
    }

    // 同一主机可能连续重连多次。最近列表按主机去重，只保留最新一次，既节省空间也符合
    // “快速重连”的使用目的；完整历史仍然保留在数据库中供后续历史页面展示。
    QSet<QString> appendedHostIds;
    for (const ConnectionHistoryEntry &entry : history) {
        if (entry.hostId.isEmpty() || appendedHostIds.contains(entry.hostId)) {
            continue;
        }
        const std::optional<HostRecord> record = m_database->findHost(entry.hostId, &error);
        if (!record.has_value()) {
            // 主机已删除时历史记录仍可作为审计信息存在，但不能用于重连，因此不显示在快捷列表。
            continue;
        }
        appendedHostIds.insert(entry.hostId);
        auto *item = new QListWidgetItem(
            tr("%1  ·  %2")
                .arg(record->name,
                     entry.connectedAt.toLocalTime().toString(QStringLiteral("MM-dd HH:mm"))),
            m_recentList);
        item->setData(RecentHostIdRole, entry.hostId);
        item->setToolTip(record->host);
    }
    // 该信号放在整棵树和最近列表都成功刷新之后，订阅方不会读到一半旧、一半新的主机集。
    emit hostsChanged();
}

bool HostManagerWidget::appendChildren(QTreeWidgetItem *parentItem,
                                       const QString &parentId,
                                       QString *errorMessage)
{
    bool ok = false;
    const QList<HostRecord> children =
        m_database->listHostsInGroup(parentId, &ok, errorMessage);
    if (!ok) {
        return false;
    }

    for (const HostRecord &record : children) {
        QTreeWidgetItem *item = createTreeItem(record);
        if (parentItem == nullptr) {
            m_tree->addTopLevelItem(item);
        } else {
            parentItem->addChild(item);
        }
        if (record.kind == HostNodeKind::Folder
            && !appendChildren(item, record.id, errorMessage)) {
            return false;
        }
    }
    return true;
}

void HostManagerWidget::applyFilter()
{
    m_tree->clear();
    const QString query = m_searchEdit->text().trimmed();
    QString error;

    if (query.isEmpty()) {
        if (!appendChildren(nullptr, {}, &error)) {
            showDatabaseError(tr("加载主机树"), error);
            return;
        }
        m_tree->expandAll();
        return;
    }

    bool ok = false;
    QList<HostRecord> results;
    if (query.startsWith(QLatin1Char('#'))) {
        results = m_database->hostsWithTag(query.mid(1).trimmed(), &ok, &error);
    } else {
        results = m_database->searchHosts(query, &ok, &error);
    }
    if (!ok) {
        showDatabaseError(tr("搜索主机"), error);
        return;
    }

    // 搜索结果使用扁平视图，避免为了展示一个命中项而加载并展开整条祖先链；节点 id 仍是
    // 稳定数据库主键，双击连接和编辑不会受显示结构变化影响。
    for (const HostRecord &record : results) {
        m_tree->addTopLevelItem(createTreeItem(record));
    }
}

void HostManagerWidget::addGroup()
{
    HostRecord record;
    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.parentId = selectedParentGroupId();
    record.kind = HostNodeKind::Folder;
    HostRecordDialog dialog(record, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    record = dialog.record();

    QString error;
    if (!m_database->saveHost(record, &error)) {
        showDatabaseError(tr("新建分组"), error);
        return;
    }
    refresh();
}

void HostManagerWidget::addHost()
{
    HostRecord record;
    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.parentId = selectedParentGroupId();
    record.kind = HostNodeKind::Host;
    record.port = quint16{22};

    HostRecordDialog dialog(record, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    record = dialog.record();
    QString error;
    ConnectionTarget target = record.toConnectionTarget();
    target.password = dialog.sessionPassword();
    bool hostSaved = false;
    if (dialog.shouldSavePassword() && !dialog.sessionPassword().isEmpty()) {
        if (!VaultDialog::ensureUnlocked(m_credentialVault, this)) {
            return;
        }
        // 凭据密文与 host.credentialId 必须处在同一事务中；先 saveSecret 再 saveHost 会在
        // 第二步失败时留下永远无人引用的密文。服务层的组合接口专门维护该原子性边界。
        hostSaved = m_credentialVault->saveConnectionPassword(
            record, target, tr("%1 的登录凭据").arg(record.name), &error);
        if (!hostSaved) {
            showDatabaseError(tr("保存加密凭据"), error);
            return;
        }
    }
    if (!hostSaved && !m_database->saveHost(record, &error)) {
        showDatabaseError(tr("保存主机"), error);
        return;
    }
    refresh();

    // 用户在“新建主机”对话框中可能输入了本次会话密码。密码不会落库，但立即连接可以
    // 复用这份进程内数据；从主机树再次连接时则由 Vault 或终端内联提示提供凭据。
    // saveConnectionPassword 可能为记录补上新 credentialId，因此持久字段重新从 record
    // 生成；明文仍只取对话框的本次会话副本，不会进入数据库。
    target = record.toConnectionTarget();
    target.password = dialog.sessionPassword();
    emit hostConnectionRequested(record.id, target);
}

void HostManagerWidget::editSelected()
{
    QString error;
    const std::optional<HostRecord> existing = selectedRecord(&error);
    if (!existing.has_value()) {
        if (!error.isEmpty()) {
            showDatabaseError(tr("读取节点"), error);
        }
        return;
    }

    HostRecordDialog dialog(*existing, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    HostRecord updated = dialog.record();
    if (dialog.shouldDetachCredential()) {
        // 只解除当前主机的引用，不删除 credentials 行：一个凭据可能被多台主机共享，未经
        // 反向引用检查就删除会让其他连接静默失效。孤立凭据由后续凭据管理页显式清理。
        updated.credentialId.clear();
    }
    ConnectionTarget editedTarget = updated.toConnectionTarget();
    editedTarget.password = dialog.sessionPassword();
    bool hostSaved = false;
    if (dialog.shouldSavePassword() && !dialog.sessionPassword().isEmpty()) {
        if (!VaultDialog::ensureUnlocked(m_credentialVault, this)) {
            return;
        }
        hostSaved = m_credentialVault->saveConnectionPassword(
            updated, editedTarget, tr("%1 的登录凭据").arg(updated.name), &error);
        if (!hostSaved) {
            showDatabaseError(tr("更新加密凭据"), error);
            return;
        }
    }

    if (!hostSaved && !m_database->saveHost(updated, &error)) {
        showDatabaseError(tr("更新节点"), error);
        return;
    }
    refresh();
}

void HostManagerWidget::removeSelected()
{
    QString error;
    const std::optional<HostRecord> record = selectedRecord(&error);
    if (!record.has_value()) {
        if (!error.isEmpty()) {
            showDatabaseError(tr("读取节点"), error);
        }
        return;
    }

    const QString warning = record->kind == HostNodeKind::Folder
        ? tr("删除分组“%1”会同时删除其全部子分组和主机，且无法撤销。确定继续吗？")
              .arg(record->name)
        : tr("确定删除主机“%1”吗？此操作无法撤销。").arg(record->name);
    if (QMessageBox::warning(this,
                             tr("确认删除"),
                             warning,
                             QMessageBox::Yes | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    int removedCount = 0;
    if (!m_database->removeHost(record->id, &removedCount, &error)) {
        showDatabaseError(tr("删除节点"), error);
        return;
    }
    refresh();
}

void HostManagerWidget::importHosts()
{
    const QString defaultConfig = QDir::home().filePath(QStringLiteral(".ssh/config"));
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("导入主机配置"),
        QFileInfo::exists(defaultConfig) ? defaultConfig : QDir::homePath(),
        tr("支持的配置 (*.json *.conf *.config);;所有文件 (*)"));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this,
                              tr("无法读取导入文件"),
                              tr("无法打开 %1：%2").arg(path, file.errorString()));
        return;
    }
    const QByteArray content = file.readAll();
    QList<HostRecord> importedRecords;
    QString error;
    const bool parsed = content.trimmed().startsWith('{')
        ? ConnectionConfigCodec::importJson(content, &importedRecords, &error)
        : ConnectionConfigCodec::importOpenSsh(content, &importedRecords, &error);
    if (!parsed) {
        QMessageBox::critical(this, tr("导入失败"), error);
        return;
    }
    if (importedRecords.isEmpty()) {
        QMessageBox::information(this, tr("没有可导入主机"), tr("文件中没有可用记录。"));
        return;
    }

    const QString confirmation =
        tr("将导入 %1 条主机/分组记录。所有记录会分配新的本地主键，凭据不会从配置文件导入。")
            .arg(importedRecords.size());
    if (QMessageBox::question(this,
                              tr("确认导入"),
                              confirmation,
                              QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    // 编解码服务在单个事务内统一重映射 id、清除凭据引用并保持父子关系。把这段安全边界
    // 收口在一处，可以避免其他导入入口漏掉外部 id 冲突或半批写入回滚。
    if (!ConnectionConfigCodec::persistImported(*m_database,
                                                importedRecords,
                                                selectedParentGroupId(),
                                                &error)) {
        showDatabaseError(tr("导入主机"), error);
        return;
    }
    refresh();
    QMessageBox::information(this,
                             tr("导入完成"),
                             tr("已成功导入 %1 条记录。").arg(importedRecords.size()));
}

void HostManagerWidget::exportHosts()
{
    QList<HostRecord> records;
    QString error;
    if (!collectRecords({}, &records, &error)) {
        showDatabaseError(tr("读取导出数据"), error);
        return;
    }
    if (records.isEmpty()) {
        QMessageBox::information(this, tr("没有可导出主机"), tr("主机树当前为空。"));
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this,
        tr("导出 Shellium 主机"),
        QDir::home().filePath(QStringLiteral("shellium-hosts.json")),
        tr("Shellium JSON (*.json)"));
    if (path.isEmpty()) {
        return;
    }

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        QMessageBox::critical(this,
                              tr("无法写入导出文件"),
                              tr("无法打开 %1：%2").arg(path, output.errorString()));
        return;
    }
    const QByteArray json = ConnectionConfigCodec::exportJson(records, &error);
    if (json.isEmpty()) {
        QMessageBox::critical(this,
                              tr("导出失败"),
                              error.isEmpty() ? tr("无法编码主机配置") : error);
        output.cancelWriting();
        return;
    }
    if (output.write(json) != json.size() || !output.commit()) {
        QMessageBox::critical(this,
                              tr("导出失败"),
                              tr("写入 %1 失败：%2").arg(path, output.errorString()));
        return;
    }
    QMessageBox::information(this,
                             tr("导出完成"),
                             tr("已导出 %1 条主机/分组记录。文件不包含任何凭据。")
                                 .arg(records.size()));
}

bool HostManagerWidget::collectRecords(const QString &parentId,
                                       QList<HostRecord> *records,
                                       QString *errorMessage) const
{
    if (records == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = tr("导出记录容器为空");
        }
        return false;
    }
    bool ok = false;
    const QList<HostRecord> children =
        m_database->listHostsInGroup(parentId, &ok, errorMessage);
    if (!ok) {
        return false;
    }
    for (const HostRecord &record : children) {
        records->append(record);
        if (record.kind == HostNodeKind::Folder
            && !collectRecords(record.id, records, errorMessage)) {
            return false;
        }
    }
    return true;
}

void HostManagerWidget::activateTreeItem(QTreeWidgetItem *item)
{
    if (item == nullptr
        || static_cast<HostNodeKind>(item->data(0, HostKindRole).toInt())
            != HostNodeKind::Host) {
        return;
    }
    requestConnection(item->data(0, HostIdRole).toString());
}

void HostManagerWidget::activateRecentItem(QListWidgetItem *item)
{
    if (item != nullptr) {
        requestConnection(item->data(RecentHostIdRole).toString());
    }
}

void HostManagerWidget::requestConnection(const QString &hostId,
                                          const ConnectionTarget &sessionTarget)
{
    if (sessionTarget.isValid()) {
        // 新建主机后立即连接时，使用对话框产生的会话快照以保留未落库密码；其他字段仍与
        // 刚保存的记录一致。后续从树中连接会重新 resolveHost，获取最新的分组继承结果。
        emit hostConnectionRequested(hostId, sessionTarget);
        return;
    }

    QString error;
    const std::optional<HostRecord> record = m_database->resolveHost(hostId, &error);
    if (!record.has_value()) {
        showDatabaseError(tr("解析主机配置"),
                          error.isEmpty() ? tr("主机记录不存在") : error);
        return;
    }
    ConnectionTarget target = record->toConnectionTarget();
    if (!record->credentialId.isEmpty()) {
        if (!VaultDialog::ensureUnlocked(m_credentialVault, this)) {
            return;
        }
        if (!m_credentialVault->applyHostCredential(*record, target, &error)) {
            showDatabaseError(tr("读取主机凭据"), error);
            return;
        }
    }
    emit hostConnectionRequested(hostId, target);
}

std::optional<HostRecord> HostManagerWidget::selectedRecord(QString *errorMessage) const
{
    const QList<QTreeWidgetItem *> selected = m_tree->selectedItems();
    if (selected.isEmpty()) {
        return std::nullopt;
    }
    return m_database->findHost(selected.constFirst()->data(0, HostIdRole).toString(), errorMessage);
}

QString HostManagerWidget::selectedParentGroupId() const
{
    QString error;
    const std::optional<HostRecord> record = selectedRecord(&error);
    if (!record.has_value()) {
        return {};
    }
    return record->kind == HostNodeKind::Folder ? record->id : record->parentId;
}

QTreeWidgetItem *HostManagerWidget::createTreeItem(const HostRecord &record) const
{
    auto *item = new QTreeWidgetItem;
    item->setText(0, record.name);
    item->setIcon(
        0,
        record.kind == HostNodeKind::Folder
            ? UiIcons::themed(m_tree, QStringLiteral("folder"), QStyle::SP_DirIcon)
            : UiIcons::themed(m_tree, QStringLiteral("network-server"), QStyle::SP_DriveNetIcon));
    item->setData(0, HostIdRole, record.id);
    item->setData(0, HostKindRole, static_cast<int>(record.kind));
    if (record.kind == HostNodeKind::Host) {
        item->setToolTip(0, record.toConnectionTarget().connectionSpecifier(true));
    }
    return item;
}

void HostManagerWidget::showDatabaseError(const QString &operation, const QString &details) const
{
    QMessageBox::critical(const_cast<HostManagerWidget *>(this),
                          tr("主机数据操作失败"),
                          tr("%1失败：%2").arg(operation, details));
}
