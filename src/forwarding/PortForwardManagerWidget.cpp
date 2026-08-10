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
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>
#include <QUuid>

#include <algorithm>
#include <utility>

#include <unistd.h>

namespace {

/** @brief 表格首列存放持久化规则 id 的自定义数据角色。 */
constexpr int kRuleIdRole = Qt::UserRole + 31;

/**
 * @brief 将端口转发侧栏的次要动作配置为紧凑图标按钮。
 * @param button 待配置按钮；不得为空。
 * @param accessibleName 完整动作名称，用于 Tooltip 与辅助技术。
 *
 * 端口转发默认位于窄侧栏，即使用户可拖动加宽，五个文字按钮在最小宽度下仍会造成横向
 * 滚动。保留“新建”主操作文字，编辑、删除、启动和停止使用图标，并通过 Tooltip 与右键
 * 菜单提供完整动作名称。
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

/**
 * @brief 将主机与端口格式化为不会混淆 IPv6 冒号的端点文本。
 * @param host IP、主机名或特殊监听地址。
 * @param port TCP 端口。
 * @return IPv4/主机名形如 host:port，IPv6 形如 [host]:port。
 */
QString formatEndpoint(const QString &host, const quint16 port)
{
    const QString normalizedHost = host.trimmed();
    // IPv6 地址本身包含冒号，若继续直接拼接端口会无法判断哪一段是端口；方括号形式与
    // URL/OpenSSH 的常见表示一致，同时不会改变实际持久化的监听或目标地址。
    return normalizedHost.contains(QLatin1Char(':'))
        ? QStringLiteral("[%1]:%2").arg(normalizedHost).arg(port)
        : QStringLiteral("%1:%2").arg(normalizedHost).arg(port);
}

/**
 * @brief 转发类型徽章的背景/文字配色，用半透明背景色叠加在列表项实际背景上。
 *
 * 用 rgba() 而不是"深色/浅色主题各一套固定色号"，是因为侧栏的 QPalette 不保证和应用
 * 自己选择的深浅主题同步（系统外观与应用内主题不一致时会读到错的一套）——半透明背景色
 * 由 Qt 直接跟当前实际显示的底色做 alpha 合成，不需要猜测,同一份颜色在深浅主题下都读得清。
 */
QString typeChipStyleSheet(const PortForwardType type)
{
    QString background;
    QString foreground;
    switch (type) {
    case PortForwardType::Remote:
        background = QStringLiteral("rgba(59,120,201,40)");
        foreground = QStringLiteral("#3b78c9");
        break;
    case PortForwardType::Dynamic:
        background = QStringLiteral("rgba(132,82,201,40)");
        foreground = QStringLiteral("#8452c9");
        break;
    case PortForwardType::Local:
    default:
        background = QStringLiteral("rgba(43,217,129,40)");
        foreground = QStringLiteral("#159a60");
        break;
    }
    return QStringLiteral("background:%1; color:%2; border-radius:6px; padding:1px 7px; "
                          "font-weight:700; font-size:10px;")
        .arg(background, foreground);
}

/**
 * @brief 状态点颜色；固定单一色号，不按深浅主题分支。
 *
 * 圆点是纯色实心图形，不存在"和背景对比不足"的问题，没必要为了区区一个点引入同一类
 * 深浅误判风险（参见主机树选中态曾经踩过的坑）。
 */
QColor statusDotColor(const PortForwardState state)
{
    switch (state) {
    case PortForwardState::Running:
        return QColor(QStringLiteral("#2bd981"));
    case PortForwardState::Failed:
        return QColor(QStringLiteral("#e5484d"));
    case PortForwardState::Starting:
    case PortForwardState::Stopping:
        return QColor(QStringLiteral("#e2a94f"));
    case PortForwardState::Stopped:
    default:
        return QColor(QStringLiteral("#8a94a6"));
    }
}

/**
 * @brief 端口转发卡片式列表的单行内容：名称+类型徽章一行，地址映射+状态一行。
 *
 * 原来的四列表格在默认 320px 侧栏里要么互相挤占，要么被裁到看不清；改成两行卡片后，
 * 地址映射可以占满整行宽度，不再需要横向滚动或极窄的列。行内容由调用方（reloadRules）
 * 预先算好文本传入，本类只负责排版和配色，不重新访问数据库或规则对象。
 */
class PortForwardRuleRow final : public QWidget {
public:
    PortForwardRuleRow(const QString &name,
                       const PortForwardType type,
                       const QString &endpointText,
                       const PortForwardState state,
                       const QString &stateLabel,
                       const int activeConnections,
                       QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 5, 6, 5);
        layout->setSpacing(2);

        auto *topRow = new QHBoxLayout;
        auto *nameLabel = new QLabel(name, this);
        QFont nameFont = nameLabel->font();
        nameFont.setBold(true);
        nameLabel->setFont(nameFont);
        nameLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto *typeLabel = new QLabel(typeText(type), this);
        typeLabel->setStyleSheet(typeChipStyleSheet(type));
        topRow->addWidget(nameLabel, 1);
        topRow->addWidget(typeLabel, 0, Qt::AlignRight | Qt::AlignVCenter);
        layout->addLayout(topRow);

        auto *endpointLabel = new QLabel(endpointText, this);
        QFont monoFont(QStringLiteral("Menlo"));
        monoFont.setStyleHint(QFont::Monospace);
        monoFont.setPointSizeF(std::max(7.0, this->font().pointSizeF() - 1.0));
        endpointLabel->setFont(monoFont);
        endpointLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));
        layout->addWidget(endpointLabel);

        auto *statusRow = new QHBoxLayout;
        statusRow->setSpacing(5);
        auto *dotLabel = new QLabel(QStringLiteral("●"), this);
        dotLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 9px;").arg(statusDotColor(state).name()));
        auto *statusLabel = new QLabel(
            tr("%1 / %2").arg(stateLabel).arg(activeConnections), this);
        statusLabel->setStyleSheet(QStringLiteral("color: palette(mid); font-size: 11px;"));
        statusRow->addWidget(dotLabel, 0, Qt::AlignVCenter);
        statusRow->addWidget(statusLabel, 0, Qt::AlignVCenter);
        statusRow->addStretch(1);
        layout->addLayout(statusRow);
    }
};

} // namespace

PortForwardManagerWidget::PortForwardManagerWidget(ApplicationDatabase *database,
                                                     QWidget *parent)
    : QWidget(parent)
    , m_database(database)
    , m_hostCombo(new QComboBox(this))
    , m_ruleList(new QListWidget(this))
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

    // 原来的四列表格在默认 320px 侧栏里放不下，改成卡片式单列列表：每条规则用
    // PortForwardRuleRow 画成两行，天然不需要横向滚动，侧栏加宽后也只是卡片变宽。
    m_ruleList->setObjectName(QStringLiteral("PortForwardRuleList"));
    m_ruleList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_ruleList->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ruleList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_ruleList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_ruleList, 1);

    auto *buttons = new QHBoxLayout;
    auto *addButton = new QPushButton(tr("新建"), this);
    addButton->setProperty("primaryAction", true);
    auto *editButton = new QToolButton(this);
    editButton->setText(tr("编辑"));
    auto *deleteButton = new QToolButton(this);
    deleteButton->setText(tr("删除"));
    auto *startButton = new QToolButton(this);
    startButton->setText(tr("启动"));
    startButton->setObjectName(QStringLiteral("StartAllPortForwardsButton"));
    auto *stopButton = new QToolButton(this);
    stopButton->setText(tr("停止"));
    stopButton->setObjectName(QStringLiteral("StopAllPortForwardsButton"));
    UiIcons::decorateButton(
        addButton,
        UiIcons::symbol(this, UiIcons::Symbol::Plus, this->palette().color(QPalette::WindowText)),
        tr("新建端口转发规则"));
    UiIcons::decorateButton(
        editButton,
        UiIcons::symbol(this, UiIcons::Symbol::Edit, this->palette().color(QPalette::WindowText)),
        tr("编辑当前选中的端口转发规则"));
    UiIcons::decorateButton(
        deleteButton,
        UiIcons::symbol(this, UiIcons::Symbol::Trash, this->palette().color(QPalette::WindowText)),
        tr("停止并删除当前规则"));
    UiIcons::decorateButton(
        startButton,
        UiIcons::symbol(this, UiIcons::Symbol::Play, this->palette().color(QPalette::WindowText)),
        tr("启动当前主机的全部转发规则"));
    UiIcons::decorateButton(
        stopButton,
        UiIcons::symbol(this, UiIcons::Symbol::Stop, this->palette().color(QPalette::WindowText)),
        tr("停止当前主机的全部转发规则"));
    configureForwardingIconButton(editButton, tr("编辑当前选中的端口转发规则"));
    configureForwardingIconButton(deleteButton, tr("停止并删除当前规则"));
    configureForwardingIconButton(startButton, tr("启动当前主机的全部转发规则"));
    configureForwardingIconButton(stopButton, tr("停止当前主机的全部转发规则"));
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
            this, &PortForwardManagerWidget::startAllRules);
    connect(stopButton, &QToolButton::clicked,
            this, &PortForwardManagerWidget::stopAllRules);
    connect(m_ruleList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *) { startSelectedRule(); });
    connect(m_ruleList,
            &QWidget::customContextMenuRequested,
            this,
            [this, addButton, editButton, deleteButton](const QPoint &position) {
                QListWidgetItem *item = m_ruleList->itemAt(position);
                if (item != nullptr) {
                    m_ruleList->setCurrentItem(item);
                }

                QMenu menu(this);
                UiIcons::modernizePopup(&menu);
                QAction *addAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Plus, this->palette().color(QPalette::WindowText)),
                    tr("新建规则"));
                connect(addAction, &QAction::triggered, addButton, &QPushButton::click);
                if (item != nullptr) {
                    menu.addSeparator();
                    QAction *startAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Play, this->palette().color(QPalette::WindowText)),
                        tr("启动"));
                    QAction *stopAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Stop, this->palette().color(QPalette::WindowText)),
                        tr("停止"));
                    QAction *editAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Edit, this->palette().color(QPalette::WindowText)),
                        tr("编辑"));
                    QAction *deleteAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Trash, this->palette().color(QPalette::WindowText)),
                        tr("删除"));
                    // 底部图标现为当前主机的批量动作；右键菜单仍明确针对当前行，必须直接
                    // 调用单条规则方法，不能再借用底部按钮，否则会意外操作整台主机。
                    connect(startAction, &QAction::triggered,
                            this, &PortForwardManagerWidget::startSelectedRule);
                    connect(stopAction, &QAction::triggered,
                            this, &PortForwardManagerWidget::stopSelectedRule);
                    connect(editAction, &QAction::triggered,
                            editButton, &QToolButton::click);
                    connect(deleteAction, &QAction::triggered,
                            deleteButton, &QToolButton::click);
                }
                menu.exec(m_ruleList->viewport()->mapToGlobal(position));
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
    // QListWidget::clear() 只删除 QListWidgetItem 本身，不会删除 setItemWidget() 挂上去的
    // 卡片控件——那是单独 reparent 出去的子控件，clear() 之后仍留在列表里泄漏，且残留内容
    // 还会被 findChildren 之类的遍历重新找到。必须先逐个摘下并手动删除，再清空条目。
    for (int row = 0; row < m_ruleList->count(); ++row) {
        QListWidgetItem *item = m_ruleList->item(row);
        QWidget *oldRowWidget = m_ruleList->itemWidget(item);
        m_ruleList->removeItemWidget(item);
        delete oldRowWidget;
    }
    m_ruleList->clear();
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
        const PortForwardStatistics statistics = m_statistics.value(rule.id);

        auto *item = new QListWidgetItem(m_ruleList);
        item->setData(kRuleIdRole, rule.id);
        auto *row = new PortForwardRuleRow(rule.name,
                                           rule.type,
                                           endpointMappingText(rule),
                                           statistics.state,
                                           stateText(statistics.state),
                                           statistics.activeConnections,
                                           m_ruleList);
        if (!statistics.lastError.isEmpty()) {
            row->setToolTip(statistics.lastError);
        }
        item->setSizeHint(row->sizeHint());
        m_ruleList->setItemWidget(item, row);

        const quint64 traffic = statistics.bytesToTarget + statistics.bytesFromTarget;
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

void PortForwardManagerWidget::startAllRules()
{
    const QString hostId = m_hostCombo->currentData().toString();
    if (hostId.isEmpty()) {
        QMessageBox::information(this,
                                 tr("没有保存主机"),
                                 tr("请先在主机管理中创建并选择一台主机。"));
        return;
    }

    bool ok = false;
    QString error;
    const QList<PortForwardRule> rules = m_database->listPortForwards(hostId, &ok, &error);
    if (!ok) {
        QMessageBox::critical(this, tr("读取转发规则失败"), error);
        return;
    }
    if (rules.isEmpty()) {
        QMessageBox::information(this,
                                 tr("没有转发规则"),
                                 tr("当前主机还没有可启动的端口转发规则。"));
        return;
    }

    SshSessionStack *sessionStack = availableSession(hostId);
    if (sessionStack == nullptr) {
        QMessageBox::information(this,
                                 tr("主机尚未连接"),
                                 tr("请先从主机树连接“%1”，再启动全部转发规则。")
                                     .arg(m_hostCombo->currentText()));
        return;
    }

    if (QMessageBox::question(
            this,
            tr("确认启动全部转发"),
            tr("将启动主机“%1”下的全部 %2 条转发规则；已在运行的规则会保持不变。是否继续？")
                .arg(m_hostCombo->currentText())
                .arg(rules.size()),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    int failedCount = 0;
    QStringList failedRules;
    for (const PortForwardRule &rule : rules) {
        QString startError;
        if (!startRule(rule, sessionStack, &startError)) {
            ++failedCount;
            failedRules.append(tr("%1：%2").arg(rule.name, startError));
        }
    }
    reloadRules();

    // startRule 只负责同步前置校验，真正的监听与 SSH 请求异步执行；异步失败仍会由
    // statisticsChanged 更新行状态。这里仅汇总在提交阶段就能确定的失败，避免误报成功。
    if (failedCount > 0) {
        QMessageBox::warning(
            this,
            tr("部分规则未能启动"),
            tr("%1 条规则未能提交启动：\n%2")
                .arg(failedCount)
                .arg(failedRules.join(QLatin1Char('\n'))));
        return;
    }
    emit statusMessage(tr("已提交当前主机的 %1 条端口转发启动任务").arg(rules.size()));
}

void PortForwardManagerWidget::stopAllRules()
{
    const QString hostId = m_hostCombo->currentData().toString();
    if (hostId.isEmpty()) {
        QMessageBox::information(this,
                                 tr("没有保存主机"),
                                 tr("请先在主机管理中创建并选择一台主机。"));
        return;
    }

    bool ok = false;
    QString error;
    const QList<PortForwardRule> rules = m_database->listPortForwards(hostId, &ok, &error);
    if (!ok) {
        QMessageBox::critical(this, tr("读取转发规则失败"), error);
        return;
    }

    QList<PortForwardRuntime *> activeRuntimes;
    for (const PortForwardRule &rule : rules) {
        PortForwardRuntime *runtime = m_runtimes.value(rule.id);
        if (runtime == nullptr) {
            continue;
        }
        const PortForwardState state = runtime->statistics().state;
        // 新建运行时的异步 Starting 通知可能尚未回到 GUI，此时 m_statistics 中还没有条目；
        // 把这种对象也视为活动运行时，确保用户紧接着点击“停止全部”不会漏掉排队中的启动。
        if (!m_statistics.contains(rule.id) || state != PortForwardState::Stopped) {
            activeRuntimes.append(runtime);
        }
    }
    if (activeRuntimes.isEmpty()) {
        emit statusMessage(tr("当前主机没有正在运行的端口转发规则"));
        return;
    }

    if (QMessageBox::question(
            this,
            tr("确认停止全部转发"),
            tr("将停止主机“%1”下的 %2 条活动转发规则，并关闭相关连接。是否继续？")
                .arg(m_hostCombo->currentText())
                .arg(activeRuntimes.size()),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    // 每个 runtime 在自己的网络线程异步收尾；逐一提交 stop 不会阻塞 GUI，也不会改变或
    // 删除数据库中的规则。统计信号返回后，表格会分别进入“停止中”和“已停止”状态。
    for (PortForwardRuntime *runtime : std::as_const(activeRuntimes)) {
        runtime->stop();
    }
    reloadRules();
    emit statusMessage(tr("已提交当前主机的 %1 条端口转发停止任务")
                           .arg(activeRuntimes.size()));
}

QString PortForwardManagerWidget::selectedRuleId() const
{
    QListWidgetItem *item = m_ruleList->currentItem();
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
    QString validationError;
    if (!rule.isValid(&validationError)) {
        // 数据库通常已保证字段合法，但导入旧版本数据或未来迁移异常时仍可能到达这里；在
        // 创建网络线程前再次校验，可以让单条和批量启动都得到可定位错误而不是触发断言。
        if (errorMessage != nullptr) {
            *errorMessage = validationError;
        }
        return false;
    }
    if (sessionStack == nullptr || !sessionStack->isRunning()) {
        // Runtime 构造函数要求非空会话，且异步启动若接到已断开的会话只会稍后失败。这里
        // 同步拒绝可让批量汇总准确指出前置条件，并关闭会话断开与点击启动之间的竞态窗口。
        if (errorMessage != nullptr) {
            *errorMessage = tr("主机 SSH 会话不可用");
        }
        return false;
    }

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

QString PortForwardManagerWidget::endpointMappingText(const PortForwardRule &rule)
{
    const QString bindEndpoint = formatEndpoint(rule.bindHost, rule.bindPort);
    switch (rule.type) {
    case PortForwardType::Remote:
        // -R 在远程 SSH 服务器监听 bind，在本机连接 target；反向箭头用于避免把两端角色
        // 错画成本地转发，同时第二列始终保持“本机在前、远程在后”的阅读顺序。
        return tr("本机 %1\n← 远程 %2")
            .arg(formatEndpoint(rule.targetHost, rule.targetPort), bindEndpoint);
    case PortForwardType::Dynamic:
        // SOCKS5 的远程目标由每个客户端请求动态给出，不能展示一个并不存在的固定端口。
        return tr("本机 %1\n→ 远程（按 SOCKS 请求）").arg(bindEndpoint);
    case PortForwardType::Local:
    default:
        return tr("本机 %1\n→ 远程 %2")
            .arg(bindEndpoint, formatEndpoint(rule.targetHost, rule.targetPort));
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
