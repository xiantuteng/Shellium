#include "forwarding/PortForwardManagerWidget.h"

#include "forwarding/PortForwardRuntime.h"
#include "ssh/SshSessionStack.h"
#include "storage/ApplicationDatabase.h"
#include "ui/UiIcons.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QUuid>

#include <algorithm>

#include <unistd.h>

namespace {

/** @brief 表格首列存放持久化规则 id 的自定义数据角色。 */
constexpr int kRuleIdRole = Qt::UserRole + 31;

/**
 * @brief 将端口转发侧栏的次要动作配置为紧凑图标按钮。
 * @param button 待配置按钮；不得为空。
 * @param accessibleName 完整动作名称，用于 Tooltip 与辅助技术。
 *
 * 端口转发始终位于 320 像素侧栏，五个文字按钮会造成横向滚动。保留“新建”主操作文字，
 * 编辑、删除、启动和停止使用图标，并在右键菜单中继续提供完整文字入口。
 */
void configureForwardingIconButton(QToolButton *button, const QString &accessibleName)
{
    Q_ASSERT(button != nullptr);
    button->setProperty("compactIconButton", true);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(false);
    button->setFixedSize(34, 34);
    button->setIconSize(QSize(17, 17));
    button->setToolTip(accessibleName);
    button->setAccessibleName(accessibleName);
}

/**
 * @brief 新建/编辑单条转发规则的模态表单。
 *
 * 表单只产生经过基础校验的值对象，不直接写数据库或启动网络；这样取消不会留下半保存
 * 规则，数据库外键和运行时安全确认仍由上层统一处理。
 */
class PortForwardRuleDialog final : public QDialog {
public:
    explicit PortForwardRuleDialog(const QString &hostId,
                                   const std::optional<PortForwardRule> &existing,
                                   QWidget *parent = nullptr)
        : QDialog(parent)
        , m_hostId(hostId)
        , m_id(existing.has_value()
                   ? existing->id
                   : QUuid::createUuid().toString(QUuid::WithoutBraces))
    {
        setWindowTitle(existing.has_value() ? tr("编辑端口转发") : tr("新建端口转发"));
        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        m_nameEdit = new QLineEdit(this);
        m_typeCombo = new QComboBox(this);
        m_typeCombo->addItem(tr("本地转发 (-L)"), static_cast<int>(PortForwardType::Local));
        m_typeCombo->addItem(tr("远程转发 (-R)"), static_cast<int>(PortForwardType::Remote));
        m_typeCombo->addItem(tr("动态 SOCKS5 (-D)"), static_cast<int>(PortForwardType::Dynamic));
        m_bindHostEdit = new QLineEdit(QStringLiteral("127.0.0.1"), this);
        m_bindPortSpin = new QSpinBox(this);
        m_bindPortSpin->setRange(1, 65535);
        m_bindPortSpin->setValue(8080);
        m_targetHostEdit = new QLineEdit(QStringLiteral("127.0.0.1"), this);
        m_targetPortSpin = new QSpinBox(this);
        m_targetPortSpin->setRange(1, 65535);
        m_targetPortSpin->setValue(80);
        m_autoStartCheck = new QCheckBox(tr("根 SSH 连接成功后自动启动"), this);
        form->addRow(tr("名称："), m_nameEdit);
        form->addRow(tr("类型："), m_typeCombo);
        form->addRow(tr("监听地址："), m_bindHostEdit);
        form->addRow(tr("监听端口："), m_bindPortSpin);
        form->addRow(tr("目标地址："), m_targetHostEdit);
        form->addRow(tr("目标端口："), m_targetPortSpin);
        form->addRow(QString(), m_autoStartCheck);
        layout->addLayout(form);

        auto *securityNotice = new QLabel(
            tr("安全提示：把监听地址设为 0.0.0.0 或 :: 会向局域网/公网暴露端口；"
               "是否可达仍受系统防火墙和 SSH 服务端 GatewayPorts 限制。"),
            this);
        securityNotice->setWordWrap(true);
        layout->addWidget(securityNotice);

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
            QString error;
            if (!rule().isValid(&error)) {
                QMessageBox::warning(this, tr("规则不完整"), error);
                return;
            }
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_typeCombo, &QComboBox::currentIndexChanged, this, [this](const int) {
            const bool dynamic = selectedType() == PortForwardType::Dynamic;
            m_targetHostEdit->setEnabled(!dynamic);
            m_targetPortSpin->setEnabled(!dynamic);
        });

        if (existing.has_value()) {
            m_nameEdit->setText(existing->name);
            m_typeCombo->setCurrentIndex(
                m_typeCombo->findData(static_cast<int>(existing->type)));
            m_bindHostEdit->setText(existing->bindHost);
            m_bindPortSpin->setValue(existing->bindPort);
            m_targetHostEdit->setText(existing->targetHost);
            m_targetPortSpin->setValue(existing->targetPort == 0 ? 1 : existing->targetPort);
            m_autoStartCheck->setChecked(existing->autoStart);
        }
        m_typeCombo->currentIndexChanged(m_typeCombo->currentIndex());
    }

    /** @brief 返回当前表单的完整规则；调用者只在对话框 Accepted 后持久化。 */
    [[nodiscard]] PortForwardRule rule() const
    {
        PortForwardRule value;
        value.id = m_id;
        value.hostId = m_hostId;
        value.name = m_nameEdit->text().trimmed();
        value.type = selectedType();
        value.bindHost = m_bindHostEdit->text().trimmed();
        value.bindPort = static_cast<quint16>(m_bindPortSpin->value());
        value.targetHost = value.type == PortForwardType::Dynamic
            ? QString()
            : m_targetHostEdit->text().trimmed();
        value.targetPort = value.type == PortForwardType::Dynamic
            ? 0
            : static_cast<quint16>(m_targetPortSpin->value());
        value.autoStart = m_autoStartCheck->isChecked();
        return value;
    }

private:
    /** @brief 从下拉框稳定整数值还原转发类型。 */
    [[nodiscard]] PortForwardType selectedType() const
    {
        return static_cast<PortForwardType>(m_typeCombo->currentData().toInt());
    }

    QString m_hostId;
    QString m_id;
    QLineEdit *m_nameEdit = nullptr;
    QComboBox *m_typeCombo = nullptr;
    QLineEdit *m_bindHostEdit = nullptr;
    QSpinBox *m_bindPortSpin = nullptr;
    QLineEdit *m_targetHostEdit = nullptr;
    QSpinBox *m_targetPortSpin = nullptr;
    QCheckBox *m_autoStartCheck = nullptr;
};

/** @brief 转发类型的紧凑表格文本。 */
QString typeText(const PortForwardType type)
{
    switch (type) {
    case PortForwardType::Remote:
        return QStringLiteral("-R");
    case PortForwardType::Dynamic:
        return QStringLiteral("-D");
    case PortForwardType::Local:
    default:
        return QStringLiteral("-L");
    }
}

} // namespace

PortForwardManagerWidget::PortForwardManagerWidget(ApplicationDatabase *database,
                                                     QWidget *parent)
    : QWidget(parent)
    , m_database(database)
    , m_hostCombo(new QComboBox(this))
    , m_ruleTable(new QTableWidget(this))
    , m_summaryLabel(new QLabel(tr("尚无运行中的端口转发"), this))
{
    Q_ASSERT(m_database != nullptr);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto *hostRow = new QHBoxLayout;
    hostRow->addWidget(new QLabel(tr("主机："), this));
    hostRow->addWidget(m_hostCombo, 1);
    layout->addLayout(hostRow);

    m_ruleTable->setColumnCount(7);
    m_ruleTable->setHorizontalHeaderLabels(
        {tr("名称"), tr("类型"), tr("监听"), tr("目标"), tr("自动"), tr("状态/连接"), tr("流量")});
    m_ruleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ruleTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_ruleTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_ruleTable->setAlternatingRowColors(false);
    m_ruleTable->setShowGrid(false);
    m_ruleTable->verticalHeader()->setVisible(false);
    m_ruleTable->verticalHeader()->setDefaultSectionSize(34);
    m_ruleTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_ruleTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < m_ruleTable->columnCount(); ++column) {
        m_ruleTable->horizontalHeader()->setSectionResizeMode(column,
                                                              QHeaderView::ResizeToContents);
    }
    // 该控件固定在窄侧栏中，主列表只保留“名称 / 类型 / 状态”。监听与目标地址、自动启动
    // 和流量仍保存在数据模型中，并可通过编辑对话框或右键操作查看，避免七列压成不可读。
    m_ruleTable->setColumnHidden(2, true);
    m_ruleTable->setColumnHidden(3, true);
    m_ruleTable->setColumnHidden(4, true);
    m_ruleTable->setColumnHidden(6, true);
    layout->addWidget(m_ruleTable, 1);

    auto *buttons = new QHBoxLayout;
    auto *addButton = new QPushButton(tr("新建"), this);
    addButton->setProperty("primaryAction", true);
    auto *editButton = new QToolButton(this);
    editButton->setText(tr("编辑"));
    auto *deleteButton = new QToolButton(this);
    deleteButton->setText(tr("删除"));
    auto *startButton = new QToolButton(this);
    startButton->setText(tr("启动"));
    auto *stopButton = new QToolButton(this);
    stopButton->setText(tr("停止"));
    UiIcons::decorateButton(
        addButton,
        UiIcons::themed(this, QStringLiteral("list-add"), QStyle::SP_FileDialogNewFolder),
        tr("新建端口转发规则"));
    UiIcons::decorateButton(
        editButton,
        UiIcons::themed(this, QStringLiteral("document-edit"), QStyle::SP_FileDialogDetailedView),
        tr("编辑当前选中的端口转发规则"));
    UiIcons::decorateButton(
        deleteButton,
        UiIcons::themed(this, QStringLiteral("edit-delete"), QStyle::SP_TrashIcon),
        tr("停止并删除当前规则"));
    UiIcons::decorateButton(
        startButton,
        UiIcons::themed(this, QStringLiteral("media-playback-start"), QStyle::SP_MediaPlay),
        tr("在当前主机 SSH 会话上启动规则"));
    UiIcons::decorateButton(
        stopButton,
        UiIcons::themed(this, QStringLiteral("media-playback-stop"), QStyle::SP_MediaStop),
        tr("停止当前规则并关闭活动转发连接"));
    configureForwardingIconButton(editButton, tr("编辑当前选中的端口转发规则"));
    configureForwardingIconButton(deleteButton, tr("停止并删除当前规则"));
    configureForwardingIconButton(startButton, tr("在当前主机 SSH 会话上启动规则"));
    configureForwardingIconButton(stopButton, tr("停止当前规则并关闭活动转发连接"));
    buttons->addWidget(addButton);
    buttons->addWidget(editButton);
    buttons->addWidget(deleteButton);
    buttons->addStretch(1);
    buttons->addWidget(startButton);
    buttons->addWidget(stopButton);
    layout->addLayout(buttons);
    m_summaryLabel->setWordWrap(true);
    layout->addWidget(m_summaryLabel);

    connect(m_hostCombo, &QComboBox::currentIndexChanged,
            this, [this](const int) { reloadRules(); });
    connect(addButton, &QPushButton::clicked,
            this, [this]() { editRule(); });
    connect(editButton, &QToolButton::clicked, this, [this]() {
        QString error;
        const std::optional<PortForwardRule> rule =
            m_database->findPortForward(selectedRuleId(), &error);
        if (!rule.has_value()) {
            emit statusMessage(error.isEmpty() ? tr("请选择要编辑的规则") : error);
            return;
        }
        editRule(rule);
    });
    connect(deleteButton, &QToolButton::clicked,
            this, &PortForwardManagerWidget::removeSelectedRule);
    connect(startButton, &QToolButton::clicked,
            this, &PortForwardManagerWidget::startSelectedRule);
    connect(stopButton, &QToolButton::clicked,
            this, &PortForwardManagerWidget::stopSelectedRule);
    connect(m_ruleTable, &QTableWidget::cellDoubleClicked,
            this, [this](const int, const int) { startSelectedRule(); });
    connect(m_ruleTable,
            &QWidget::customContextMenuRequested,
            this,
            [this, addButton, editButton, deleteButton, startButton, stopButton](
                const QPoint &position) {
                const QModelIndex index = m_ruleTable->indexAt(position);
                if (index.isValid()) {
                    m_ruleTable->setCurrentCell(index.row(), 0);
                }

                QMenu menu(this);
                QAction *addAction = menu.addAction(
                    UiIcons::themed(this, QStringLiteral("list-add"),
                                    QStyle::SP_FileDialogNewFolder),
                    tr("新建规则"));
                connect(addAction, &QAction::triggered, addButton, &QPushButton::click);
                if (index.isValid()) {
                    menu.addSeparator();
                    QAction *startAction = menu.addAction(
                        UiIcons::themed(this, QStringLiteral("media-playback-start"),
                                        QStyle::SP_MediaPlay),
                        tr("启动"));
                    QAction *stopAction = menu.addAction(
                        UiIcons::themed(this, QStringLiteral("media-playback-stop"),
                                        QStyle::SP_MediaStop),
                        tr("停止"));
                    QAction *editAction = menu.addAction(
                        UiIcons::themed(this, QStringLiteral("document-edit"),
                                        QStyle::SP_FileDialogDetailedView),
                        tr("编辑"));
                    QAction *deleteAction = menu.addAction(
                        UiIcons::themed(this, QStringLiteral("edit-delete"), QStyle::SP_TrashIcon),
                        tr("删除"));
                    connect(startAction, &QAction::triggered,
                            startButton, &QToolButton::click);
                    connect(stopAction, &QAction::triggered,
                            stopButton, &QToolButton::click);
                    connect(editAction, &QAction::triggered,
                            editButton, &QToolButton::click);
                    connect(deleteAction, &QAction::triggered,
                            deleteButton, &QToolButton::click);
                }
                menu.exec(m_ruleTable->viewport()->mapToGlobal(position));
            });

    reloadHosts();
}

PortForwardManagerWidget::~PortForwardManagerWidget()
{
    // 运行时网络线程持有非拥有的 SshSessionStack 指针。侧栏可能先于中央标签析构，因此
    // 必须在自身析构中同步清理，不能把它们留给栈 QObject 的子对象清理阶段。
    const QList<QPointer<PortForwardRuntime>> runtimes = m_runtimes.values();
    m_runtimes.clear();
    for (PortForwardRuntime *runtime : runtimes) {
        if (runtime != nullptr) {
            delete runtime;
        }
    }
}

void PortForwardManagerWidget::registerSession(const QString &hostId,
                                                SshSessionStack *sessionStack)
{
    if (hostId.isEmpty() || sessionStack == nullptr) {
        return;
    }
    QList<QPointer<SshSessionStack>> &sessions = m_sessions[hostId];
    const bool alreadyRegistered = std::any_of(
        sessions.cbegin(), sessions.cend(),
        [sessionStack](const QPointer<SshSessionStack> &candidate) {
            return candidate == sessionStack;
        });
    if (!alreadyRegistered) {
        sessions.append(sessionStack);
        // QObject::UniqueConnection 对 lambda 不生效，因此只在首次登记准确对象时创建这两条
        // 连接。这样 shellConnected 重复通知也不会让一个 fd 被分发多次。
        connect(sessionStack, &QObject::destroyed, this,
                [this, hostId, sessionStack]() { unregisterSession(hostId, sessionStack); });
        connect(sessionStack,
                &SshSessionStack::remoteForwardConnection,
                this,
                [this, sessionStack](const QString &ruleId, const int tunnelFd) {
                    dispatchRemoteConnection(sessionStack, ruleId, tunnelFd);
                },
                Qt::QueuedConnection);
    }
    startAutomaticRules(hostId, sessionStack);
    reloadRules();
}

void PortForwardManagerWidget::unregisterSession(const QString &hostId,
                                                  SshSessionStack *sessionStack)
{
    QList<QPointer<SshSessionStack>> &sessions = m_sessions[hostId];
    sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                  [sessionStack](const QPointer<SshSessionStack> &candidate) {
                                      return candidate.isNull() || candidate == sessionStack;
                                  }),
                   sessions.end());
    if (sessions.isEmpty()) {
        m_sessions.remove(hostId);
    }

    for (auto iterator = m_runtimes.begin(); iterator != m_runtimes.end();) {
        PortForwardRuntime *runtime = iterator.value();
        if (runtime == nullptr) {
            iterator = m_runtimes.erase(iterator);
            continue;
        }
        if (runtime->rule().hostId == hostId
            && runtime->parent() == sessionStack) {
            iterator = m_runtimes.erase(iterator);
            // forwardingSessionAboutToClose 在 SSH 栈收到 requestStop 后、实际析构前同步
            // 发出；此处同步析构网络线程，保证其不再调用即将失效的栈对象。
            delete runtime;
        } else {
            ++iterator;
        }
    }

    SshSessionStack *replacement = availableSession(hostId);
    if (replacement != nullptr) {
        // 即使 QObject::destroyed 到达时旧 Runtime 已作为栈子对象被清理，也要重新扫描全部
        // autoStart 规则；startRule 会幂等跳过仍在运行的规则。
        startAutomaticRules(hostId, replacement);
    }
    reloadRules();
}

void PortForwardManagerWidget::dispatchRemoteConnection(SshSessionStack *sessionStack,
                                                         const QString &ruleId,
                                                         const int tunnelFd)
{
    if (tunnelFd < 0) {
        return;
    }
    PortForwardRuntime *runtime = m_runtimes.value(ruleId);
    if (runtime == nullptr || runtime->parent() != sessionStack
        || runtime->rule().type != PortForwardType::Remote) {
        // 规则可能在服务端接受 channel 与 GUI 收到排队信号之间被停止、删除或迁移。管理器
        // 是信号的唯一接收者，因此找不到准确所有者时必须在此关闭，避免长期泄漏 fd。
        ::close(tunnelFd);
        return;
    }
    runtime->acceptRemoteConnection(tunnelFd);
}

void PortForwardManagerWidget::refresh()
{
    reloadHosts();
}

void PortForwardManagerWidget::reloadHosts()
{
    const QString previousHostId = m_hostCombo->currentData().toString();
    bool ok = false;
    QString error;
    const QList<HostRecord> records = m_database->searchHosts({}, &ok, &error);
    m_hostCombo->blockSignals(true);
    m_hostCombo->clear();
    if (ok) {
        for (const HostRecord &record : records) {
            if (record.kind == HostNodeKind::Host) {
                m_hostCombo->addItem(record.name, record.id);
            }
        }
    }
    const int previousIndex = m_hostCombo->findData(previousHostId);
    if (previousIndex >= 0) {
        m_hostCombo->setCurrentIndex(previousIndex);
    }
    m_hostCombo->blockSignals(false);
    if (!ok) {
        emit statusMessage(tr("读取端口转发主机失败：%1").arg(error));
    }
    reloadRules();
}

void PortForwardManagerWidget::reloadRules()
{
    m_ruleTable->setRowCount(0);
    const QString hostId = m_hostCombo->currentData().toString();
    if (hostId.isEmpty()) {
        m_summaryLabel->setText(tr("请先在主机管理中保存一台主机"));
        return;
    }
    bool ok = false;
    QString error;
    const QList<PortForwardRule> rules = m_database->listPortForwards(hostId, &ok, &error);
    if (!ok) {
        emit statusMessage(tr("读取端口转发规则失败：%1").arg(error));
        return;
    }

    quint64 activeConnections = 0;
    quint64 totalTraffic = 0;
    for (const PortForwardRule &rule : rules) {
        const int row = m_ruleTable->rowCount();
        m_ruleTable->insertRow(row);
        auto *nameItem = new QTableWidgetItem(rule.name);
        nameItem->setData(kRuleIdRole, rule.id);
        m_ruleTable->setItem(row, 0, nameItem);
        m_ruleTable->setItem(row, 1, new QTableWidgetItem(typeText(rule.type)));
        m_ruleTable->setItem(row, 2, new QTableWidgetItem(
            QStringLiteral("%1:%2").arg(rule.bindHost).arg(rule.bindPort)));
        m_ruleTable->setItem(row, 3, new QTableWidgetItem(
            rule.type == PortForwardType::Dynamic
                ? tr("按 SOCKS 请求")
                : QStringLiteral("%1:%2").arg(rule.targetHost).arg(rule.targetPort)));
        m_ruleTable->setItem(row, 4, new QTableWidgetItem(rule.autoStart ? tr("是") : tr("否")));
        const PortForwardStatistics statistics = m_statistics.value(rule.id);
        m_ruleTable->setItem(row, 5, new QTableWidgetItem(
            tr("%1 / %2").arg(stateText(statistics.state))
                            .arg(statistics.activeConnections)));
        const quint64 traffic = statistics.bytesToTarget + statistics.bytesFromTarget;
        m_ruleTable->setItem(row, 6, new QTableWidgetItem(formatBytes(traffic)));
        if (!statistics.lastError.isEmpty()) {
            for (int column = 0; column < m_ruleTable->columnCount(); ++column) {
                m_ruleTable->item(row, column)->setToolTip(statistics.lastError);
            }
        }
        activeConnections += statistics.activeConnections;
        totalTraffic += traffic;
    }
    m_summaryLabel->setText(
        tr("%1 条规则，%2 条活动连接，累计流量 %3；%4")
            .arg(rules.size())
            .arg(activeConnections)
            .arg(formatBytes(totalTraffic))
            .arg(availableSession(hostId) == nullptr ? tr("主机未连接") : tr("SSH 会话可用")));
}

void PortForwardManagerWidget::editRule(const std::optional<PortForwardRule> &existing)
{
    const QString hostId = m_hostCombo->currentData().toString();
    if (hostId.isEmpty()) {
        QMessageBox::information(this, tr("没有保存主机"), tr("请先在主机管理中创建主机。"));
        return;
    }
    if (existing.has_value() && m_runtimes.contains(existing->id)
        && m_runtimes.value(existing->id) != nullptr
        && m_runtimes.value(existing->id)->statistics().state == PortForwardState::Running) {
        QMessageBox::information(this,
                                 tr("规则正在运行"),
                                 tr("请先停止规则，再修改监听或目标参数。"));
        return;
    }
    PortForwardRuleDialog dialog(hostId, existing, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const PortForwardRule rule = dialog.rule();
    const bool publiclyExposed = rule.bindHost == QStringLiteral("0.0.0.0")
        || rule.bindHost == QStringLiteral("::");
    if (publiclyExposed
        && QMessageBox::warning(this,
                                tr("确认暴露监听端口"),
                                tr("规则会监听所有网络接口，其他设备可能访问该端口。"
                                   "确认已配置防火墙并继续保存吗？"),
                                QMessageBox::Yes | QMessageBox::Cancel,
                                QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    QString error;
    if (!m_database->savePortForward(rule, &error)) {
        QMessageBox::critical(this, tr("保存规则失败"), error);
        return;
    }
    reloadRules();
}

void PortForwardManagerWidget::removeSelectedRule()
{
    const QString ruleId = selectedRuleId();
    if (ruleId.isEmpty()) {
        emit statusMessage(tr("请选择要删除的端口转发规则"));
        return;
    }
    if (QMessageBox::warning(this,
                             tr("确认删除规则"),
                             tr("删除后无法撤销；正在运行的监听会立即停止。"),
                             QMessageBox::Yes | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    if (PortForwardRuntime *runtime = m_runtimes.take(ruleId); runtime != nullptr) {
        delete runtime;
    }
    int removed = 0;
    QString error;
    if (!m_database->removePortForward(ruleId, &removed, &error)) {
        QMessageBox::critical(this, tr("删除规则失败"), error);
        return;
    }
    m_statistics.remove(ruleId);
    reloadRules();
}

void PortForwardManagerWidget::startSelectedRule()
{
    const QString ruleId = selectedRuleId();
    QString error;
    const std::optional<PortForwardRule> rule = m_database->findPortForward(ruleId, &error);
    if (!rule.has_value()) {
        emit statusMessage(error.isEmpty() ? tr("请选择要启动的规则") : error);
        return;
    }
    SshSessionStack *sessionStack = availableSession(rule->hostId);
    if (sessionStack == nullptr) {
        QMessageBox::information(this,
                                 tr("主机尚未连接"),
                                 tr("请先从主机树连接“%1”，再启动这条规则。")
                                     .arg(m_hostCombo->currentText()));
        return;
    }
    if (!startRule(*rule, sessionStack, &error)) {
        QMessageBox::critical(this, tr("启动转发失败"), error);
    }
}

void PortForwardManagerWidget::stopSelectedRule()
{
    const QString ruleId = selectedRuleId();
    PortForwardRuntime *runtime = m_runtimes.value(ruleId);
    if (runtime == nullptr) {
        emit statusMessage(tr("所选规则当前未运行"));
        return;
    }
    runtime->stop();
    reloadRules();
}

QString PortForwardManagerWidget::selectedRuleId() const
{
    const int row = m_ruleTable->currentRow();
    QTableWidgetItem *item = row < 0 ? nullptr : m_ruleTable->item(row, 0);
    return item == nullptr ? QString() : item->data(kRuleIdRole).toString();
}

SshSessionStack *PortForwardManagerWidget::availableSession(const QString &hostId)
{
    QList<QPointer<SshSessionStack>> &sessions = m_sessions[hostId];
    sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                  [](const QPointer<SshSessionStack> &session) {
                                      return session.isNull() || !session->isRunning();
                                  }),
                   sessions.end());
    if (sessions.isEmpty()) {
        m_sessions.remove(hostId);
        return nullptr;
    }
    return sessions.constFirst().data();
}

bool PortForwardManagerWidget::startRule(const PortForwardRule &rule,
                                         SshSessionStack *sessionStack,
                                         QString *errorMessage)
{
    if (PortForwardRuntime *existing = m_runtimes.value(rule.id); existing != nullptr) {
        const PortForwardState state = existing->statistics().state;
        if (state == PortForwardState::Running || state == PortForwardState::Starting) {
            return true;
        }
        m_runtimes.remove(rule.id);
        delete existing;
    }
    auto *runtime = new PortForwardRuntime(rule, sessionStack, sessionStack);
    m_runtimes.insert(rule.id, runtime);
    connect(runtime,
            &PortForwardRuntime::statisticsChanged,
            this,
            [this](const QString &ruleId, const PortForwardStatistics &statistics) {
                m_statistics.insert(ruleId, statistics);
                reloadRules();
                if (statistics.state == PortForwardState::Failed) {
                    emit statusMessage(tr("端口转发失败：%1").arg(statistics.lastError));
                }
            });
    connect(runtime,
            &PortForwardRuntime::statusMessage,
            this,
            &PortForwardManagerWidget::statusMessage);
    // Runtime 的本地监听和远程请求都在专属网络线程异步执行；字段与会话前置条件已在
    // 本方法完成，后续 bind/服务端拒绝通过 statisticsChanged/statusMessage 明确上报。
    runtime->start();
    emit statusMessage(tr("正在启动端口转发：%1").arg(rule.name));
    return true;
}

void PortForwardManagerWidget::startAutomaticRules(const QString &hostId,
                                                    SshSessionStack *sessionStack)
{
    bool ok = false;
    QString error;
    const QList<PortForwardRule> rules = m_database->listPortForwards(hostId, &ok, &error);
    if (!ok) {
        emit statusMessage(tr("读取自动转发规则失败：%1").arg(error));
        return;
    }
    for (const PortForwardRule &rule : rules) {
        if (rule.autoStart) {
            QString startError;
            if (!startRule(rule, sessionStack, &startError)) {
                emit statusMessage(tr("自动启动“%1”失败：%2").arg(rule.name, startError));
            }
        }
    }
}

QString PortForwardManagerWidget::stateText(const PortForwardState state)
{
    switch (state) {
    case PortForwardState::Starting:
        return tr("启动中");
    case PortForwardState::Running:
        return tr("运行中");
    case PortForwardState::Stopping:
        return tr("停止中");
    case PortForwardState::Failed:
        return tr("失败");
    case PortForwardState::Stopped:
    default:
        return tr("已停止");
    }
}

QString PortForwardManagerWidget::formatBytes(const quint64 bytes)
{
    static const QStringList units{
        QStringLiteral("B"), QStringLiteral("KiB"), QStringLiteral("MiB"),
        QStringLiteral("GiB"), QStringLiteral("TiB")};
    double value = static_cast<double>(bytes);
    qsizetype unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? tr("%1 %2").arg(bytes).arg(units.at(unit))
                     : tr("%1 %2").arg(value, 0, 'f', 1).arg(units.at(unit));
}
