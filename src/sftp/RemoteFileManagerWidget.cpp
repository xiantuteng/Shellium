#include "sftp/RemoteFileManagerWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <optional>

namespace {

/** @brief 把字节数格式化为适合文件列表展示的 IEC 单位。 */
QString formatSize(const quint64 bytes)
{
    static const QStringList units = {
        QStringLiteral("B"), QStringLiteral("KiB"), QStringLiteral("MiB"),
        QStringLiteral("GiB"), QStringLiteral("TiB")
    };
    double value = static_cast<double>(bytes);
    qsizetype unitIndex = 0;
    while (value >= 1024.0 && unitIndex + 1 < units.size()) {
        value /= 1024.0;
        ++unitIndex;
    }
    return unitIndex == 0
        ? QStringLiteral("%1 %2").arg(bytes).arg(units.at(unitIndex))
        : QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(units.at(unitIndex));
}

using SftpRecursive::joinRemotePath;

} // namespace

RemoteFileManagerWidget::RemoteFileManagerWidget(QWidget *parent)
    : QWidget(parent)
    , m_targetCombo(new QComboBox(this))
    , m_pathEdit(new QLineEdit(QStringLiteral("/"), this))
    , m_fileTree(new QTreeWidget(this))
    , m_statusLabel(new QLabel(tr("请选择终端服务器"), this))
    , m_progressBar(new QProgressBar(this))
    , m_service(new SftpService(this))
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    mainLayout->setSpacing(6);

    auto *serverRow = new QHBoxLayout;
    serverRow->addWidget(new QLabel(tr("服务器："), this));
    serverRow->addWidget(m_targetCombo, 1);
    mainLayout->addLayout(serverRow);

    auto *pathRow = new QHBoxLayout;
    auto *upButton = new QPushButton(tr("上级"), this);
    auto *refreshButton = new QPushButton(tr("刷新"), this);
    pathRow->addWidget(upButton);
    pathRow->addWidget(m_pathEdit, 1);
    pathRow->addWidget(refreshButton);
    mainLayout->addLayout(pathRow);

    m_fileTree->setColumnCount(5);
    m_fileTree->setHeaderLabels({tr("名称"), tr("大小"), tr("类型"), tr("所有者"), tr("修改时间")});
    m_fileTree->setRootIsDecorated(false);
    m_fileTree->setAlternatingRowColors(true);
    m_fileTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_fileTree->header()->setStretchLastSection(false);
    m_fileTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_fileTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_fileTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_fileTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_fileTree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    mainLayout->addWidget(m_fileTree, 1);

    auto *operationRow = new QHBoxLayout;
    auto *uploadButton = new QPushButton(tr("上传文件"), this);
    auto *uploadDirectoryButton = new QPushButton(tr("上传目录"), this);
    auto *downloadButton = new QPushButton(tr("下载"), this);
    auto *renameButton = new QPushButton(tr("重命名"), this);
    auto *deleteButton = new QPushButton(tr("删除"), this);
    m_cancelButton = new QPushButton(tr("取消传输"), this);
    m_cancelButton->setEnabled(false);
    operationRow->addWidget(uploadButton);
    operationRow->addWidget(uploadDirectoryButton);
    operationRow->addWidget(downloadButton);
    operationRow->addWidget(renameButton);
    operationRow->addWidget(deleteButton);
    operationRow->addStretch(1);
    operationRow->addWidget(m_cancelButton);
    mainLayout->addLayout(operationRow);

    m_progressBar->setVisible(false);
    m_progressBar->setRange(0, 100);
    mainLayout->addWidget(m_progressBar);
    mainLayout->addWidget(m_statusLabel);

    connect(refreshButton, &QPushButton::clicked, this, &RemoteFileManagerWidget::refreshDirectory);
    connect(upButton, &QPushButton::clicked, this, &RemoteFileManagerWidget::navigateUp);
    connect(m_pathEdit, &QLineEdit::returnPressed, this, &RemoteFileManagerWidget::refreshDirectory);
    connect(m_targetCombo, &QComboBox::currentIndexChanged, this, [this](const int) {
        if (m_targetCombo->currentIndex() >= 0) {
            m_fileTree->clear();
            restorePathForSelectedTarget();
            refreshDirectory();
        }
    });
    connect(m_fileTree,
            &QTreeWidget::itemDoubleClicked,
            this,
            &RemoteFileManagerWidget::activateItem);
    connect(uploadButton, &QPushButton::clicked, this, &RemoteFileManagerWidget::uploadSelectedFile);
    connect(uploadDirectoryButton,
            &QPushButton::clicked,
            this,
            &RemoteFileManagerWidget::uploadSelectedDirectory);
    connect(m_cancelButton,
            &QPushButton::clicked,
            this,
            &RemoteFileManagerWidget::cancelActiveTransfer);
    connect(downloadButton, &QPushButton::clicked, this, &RemoteFileManagerWidget::downloadSelectedFile);
    connect(renameButton, &QPushButton::clicked, this, &RemoteFileManagerWidget::renameSelectedPath);
    connect(deleteButton, &QPushButton::clicked, this, &RemoteFileManagerWidget::deleteSelectedPath);

    connect(m_service,
            &SftpService::directoryListed,
            this,
            [this](const quint64 requestId,
                   const ConnectionTarget &target,
                   const QString &path,
                   const QList<RemoteFileInfo> &entries) {
                // 用户可能已切换服务器或连续输入了新路径，过期响应不得覆盖最新页面。
                if (requestId != m_latestListRequest || target.stableKey() != selectedTarget().stableKey()) {
                    return;
                }
                m_fileTree->clear();
                for (const RemoteFileInfo &entry : entries) {
                    auto *item = new QTreeWidgetItem(m_fileTree);
                    item->setText(0, entry.name);
                    item->setText(1, entry.isDirectory ? QStringLiteral("—") : formatSize(entry.size));
                    item->setText(2, entry.isDirectory ? tr("目录")
                                                       : entry.isSymbolicLink ? tr("链接") : tr("文件"));
                    item->setText(3, entry.owner);
                    item->setText(4, entry.modifiedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
                    item->setData(0, Qt::UserRole, entry.path);
                    item->setData(0, Qt::UserRole + 1, entry.isDirectory);
                    item->setData(0, Qt::UserRole + 2, QVariant::fromValue(entry.size));
                }
                m_pathEdit->setText(path);
                // 记住这台服务器的当前位置：下次切回来时应当回到这里，而不是沿用别处的路径。
                m_pathByTarget.insert(target.stableKey(), path);
                m_directoryDisconnectRetries.remove(target.stableKey());
                setBusy(false, tr("%1 个条目").arg(entries.size()));
            });
    connect(m_service,
            &SftpService::operationSucceeded,
            this,
            &RemoteFileManagerWidget::handleOperationSuccess);
    connect(m_service,
            &SftpService::operationFailed,
            this,
            &RemoteFileManagerWidget::handleOperationFailure);
    connect(m_service,
            &SftpService::hostKeyVerificationRequired,
            this,
            [this](const ConnectionTarget &target, const QString &fingerprint) {
                if (m_hostKeyDialogOpen) {
                    return;
                }
                m_hostKeyDialogOpen = true;
                const QMessageBox::StandardButton answer = QMessageBox::question(
                    this,
                    tr("确认服务器主机密钥"),
                    tr("首次连接 %1。\n\nSHA-256 指纹：\n%2\n\n"
                       "请通过可信渠道核对指纹。确认信任并写入 known_hosts 吗？")
                        .arg(target.connectionSpecifier(true), fingerprint),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No);
                m_hostKeyDialogOpen = false;
                if (answer == QMessageBox::Yes) {
                    m_service->trustUnknownHost(target);
                    refreshDirectory();
                }
            });
    connect(m_service,
            &SftpService::transferProgress,
            this,
            [this](const quint64, const quint64 completed, const quint64 total) {
                m_progressBar->setVisible(true);
                const int percent = SftpRecursive::progressPercent(completed, total);
                if (percent < 0) {
                    m_progressBar->setRange(0, 0);
                } else {
                    m_progressBar->setRange(0, 100);
                    m_progressBar->setValue(percent);
                }
            });
    connect(m_service,
            &SftpService::recursiveTransferProgress,
            this,
            [this](const quint64 requestId,
                   const quint64 filesCompleted,
                   const quint64 filesTotal,
                   const QString &currentItem) {
                if (requestId != m_activeTransferRequest) {
                    return;
                }
                // 字节进度条无法区分"一个大文件"和"几千个小文件"，条目计数补上这层信息。
                const QString message = tr("正在传输 %1/%2：%3")
                                            .arg(filesCompleted + 1)
                                            .arg(filesTotal)
                                            .arg(currentItem);
                m_statusLabel->setText(message);
                emit statusMessage(message);
            });
    connect(m_service,
            &SftpService::recursiveTransferFinished,
            this,
            &RemoteFileManagerWidget::handleRecursiveTransferFinished);
}

RemoteFileManagerWidget::~RemoteFileManagerWidget()
{
    // QString 可能因隐式共享存在多个缓冲区，无法提供操作系统级“绝不留痕”保证；仍主动覆盖
    // 本控件直接持有的会话副本，缩短密码在正常退出路径中的内存驻留时间。
    for (QString &secret : m_sessionSecrets) {
        secret.fill(QChar::Null);
    }
    m_sessionSecrets.clear();
    for (ConnectionTarget &target : m_targets) {
        target.password.fill(QChar::Null);
        target.password.clear();
    }
}

void RemoteFileManagerWidget::setAvailableTargets(const QList<ConnectionTarget> &targets,
                                                  const bool followInnermost)
{
    const QString previousKey = selectedTarget().stableKey();
    m_targets = targets;
    for (ConnectionTarget &target : m_targets) {
        if (target.password.isEmpty()) {
            const auto cachedSecret = m_sessionSecrets.constFind(target.stableKey());
            if (cachedSecret != m_sessionSecrets.cend()) {
                // TerminalPane 只维护连接拓扑，不保存用户后来为 SFTP 输入的密码；在界面列表
                // 每次重建时恢复内存缓存，保证切目录、切层级和切标签页都不会重复询问。
                target.password = cachedSecret.value();
            }
        } else {
            // 初始连接对话框提供的会话密码也进入同一缓存，但仍然不会被持久化。
            m_sessionSecrets.insert(target.stableKey(), target.password);
        }
    }

    m_targetCombo->blockSignals(true);
    m_targetCombo->clear();
    for (qsizetype index = 0; index < m_targets.size(); ++index) {
        const ConnectionTarget &target = m_targets.at(index);
        // 使用缩进和层级编号表达 A → B → C，不依赖服务器别名是否重复。
        const QString label = target.displayName.isEmpty() ? target.connectionSpecifier()
                                                           : target.displayName;
        // 推测层必须一眼可辨：它可能与远端真实状态不一致，而在错误的服务器上删除文件
        // 是不可撤销的。
        m_targetCombo->addItem(
            target.confidence == ConnectionTarget::Confidence::Inferred
                ? tr("第 %1 层 · ⚠ 推测 · %2").arg(index + 1).arg(label)
                : tr("第 %1 层 · %2").arg(index + 1).arg(label));
    }

    int selectedIndex = -1;
    if (followInnermost && !m_targets.isEmpty()) {
        selectedIndex = static_cast<int>(m_targets.size() - 1);
    } else {
        for (qsizetype index = 0; index < m_targets.size(); ++index) {
            if (m_targets.at(index).stableKey() == previousKey) {
                selectedIndex = static_cast<int>(index);
                break;
            }
        }
    }
    m_targetCombo->setCurrentIndex(selectedIndex);
    m_targetCombo->blockSignals(false);

    if (selectedIndex >= 0) {
        if (m_targets.at(selectedIndex).stableKey() != previousKey) {
            // 切换层级后立即清空旧服务器列表，避免隧道尚未建立时把第一层缓存误认为第二层内容。
            m_fileTree->clear();
            restorePathForSelectedTarget();
        }
        refreshDirectory();
    } else {
        m_fileTree->clear();
        m_statusLabel->setText(tr("当前终端尚未连接服务器"));
    }
}

void RemoteFileManagerWidget::restorePathForSelectedTarget()
{
    const ConnectionTarget target = selectedTarget();
    if (!target.isValid()) {
        return;
    }
    // 没有记录过的服务器留空，由服务端解析出主目录；不能沿用上一台的路径。
    m_pathEdit->setText(m_pathByTarget.value(target.stableKey()));
}

void RemoteFileManagerWidget::refreshDirectory()
{
    const ConnectionTarget target = selectedTarget();
    if (!target.isValid()) {
        return;
    }
    const QString path = m_pathEdit->text().trimmed();
    if (!path.isEmpty() && !path.startsWith(QLatin1Char('/'))) {
        QMessageBox::warning(this, tr("路径无效"), tr("请输入以 / 开头的远程绝对路径。"));
        return;
    }

    // 空路径交给服务端解析成该账号的主目录；解析结果会由 directoryListed 带回来。
    setBusy(true, path.isEmpty() ? tr("正在读取主目录…") : tr("正在读取 %1…").arg(path));
    m_latestListRequest = m_service->listDirectory(target, path);
}

void RemoteFileManagerWidget::activateItem(QTreeWidgetItem *item, const int column)
{
    Q_UNUSED(column)
    if (item == nullptr) {
        return;
    }
    const bool directory = item->data(0, Qt::UserRole + 1).toBool();
    if (directory) {
        m_pathEdit->setText(item->data(0, Qt::UserRole).toString());
        refreshDirectory();
    } else {
        downloadSelectedFile();
    }
}

void RemoteFileManagerWidget::uploadSelectedFile()
{
    const ConnectionTarget target = selectedTarget();
    if (!target.isValid()) {
        return;
    }
    const QString localPath = QFileDialog::getOpenFileName(this, tr("选择要上传的文件"));
    if (localPath.isEmpty()) {
        return;
    }
    const QString remotePath = joinRemotePath(m_pathEdit->text(), QFileInfo(localPath).fileName());
    setBusy(true, tr("正在上传 %1…").arg(QFileInfo(localPath).fileName()));
    m_service->uploadFile(target, localPath, remotePath);
}

void RemoteFileManagerWidget::uploadSelectedDirectory()
{
    const ConnectionTarget target = selectedTarget();
    if (!target.isValid()) {
        return;
    }
    if (m_activeTransferRequest != 0) {
        QMessageBox::information(this, tr("传输进行中"), tr("请先等待或取消当前的目录传输。"));
        return;
    }
    const QString localDirectory = QFileDialog::getExistingDirectory(
        this, tr("选择要上传的目录"));
    if (localDirectory.isEmpty()) {
        return;
    }
    const QString remoteParent = m_pathEdit->text().trimmed();
    if (remoteParent.isEmpty()) {
        QMessageBox::warning(this, tr("路径未知"),
                             tr("请先进入一个远程目录，再上传本地目录。"));
        return;
    }
    setBusy(true, tr("正在遍历并上传 %1…").arg(QDir(localDirectory).dirName()));
    setActiveTransfer(m_service->uploadDirectory(target, localDirectory, remoteParent));
}

void RemoteFileManagerWidget::downloadSelectedFile()
{
    const std::optional<RemoteFileInfo> entry = selectedFile();
    if (!entry.has_value()) {
        QMessageBox::information(this, tr("请选择条目"), tr("请选择一个要下载的远程文件或目录。"));
        return;
    }
    if (entry->isDirectory) {
        if (m_activeTransferRequest != 0) {
            QMessageBox::information(this, tr("传输进行中"), tr("请先等待或取消当前的目录传输。"));
            return;
        }
        const QString localParent = QFileDialog::getExistingDirectory(
            this, tr("选择保存目录的位置"));
        if (localParent.isEmpty()) {
            return;
        }
        setBusy(true, tr("正在遍历并下载 %1…").arg(entry->name));
        setActiveTransfer(m_service->downloadDirectory(selectedTarget(), entry->path, localParent));
        return;
    }
    const QString localPath = QFileDialog::getSaveFileName(this, tr("保存远程文件"), entry->name);
    if (localPath.isEmpty()) {
        return;
    }
    setBusy(true, tr("正在下载 %1…").arg(entry->name));
    m_service->downloadFile(selectedTarget(), entry->path, localPath);
}

void RemoteFileManagerWidget::cancelActiveTransfer()
{
    if (m_activeTransferRequest == 0) {
        return;
    }
    m_service->cancelRequest(m_activeTransferRequest);
    // 后台任务要跑到下一个分块边界才会停，先把按钮禁掉避免用户反复点击并以为没生效。
    m_cancelButton->setEnabled(false);
    const QString message = tr("已请求取消传输，正在收尾…");
    m_statusLabel->setText(message);
    emit statusMessage(message);
}

void RemoteFileManagerWidget::renameSelectedPath()
{
    const std::optional<RemoteFileInfo> entry = selectedFile();
    if (!entry.has_value()) {
        QMessageBox::information(this, tr("请选择条目"), tr("请选择要重命名的文件或目录。"));
        return;
    }
    bool accepted = false;
    const QString newName = QInputDialog::getText(this,
                                                   tr("重命名"),
                                                   tr("新名称："),
                                                   QLineEdit::Normal,
                                                   entry->name,
                                                   &accepted).trimmed();
    if (!accepted || newName.isEmpty() || newName == entry->name) {
        return;
    }
    if (newName.contains(QLatin1Char('/'))) {
        QMessageBox::warning(this, tr("名称无效"), tr("名称中不能包含路径分隔符 /。"));
        return;
    }
    const QString newPath = joinRemotePath(m_pathEdit->text(), newName);
    setBusy(true, tr("正在重命名…"));
    m_service->renamePath(selectedTarget(), entry->path, newPath);
}

void RemoteFileManagerWidget::deleteSelectedPath()
{
    const std::optional<RemoteFileInfo> entry = selectedFile();
    if (!entry.has_value()) {
        QMessageBox::information(this, tr("请选择条目"), tr("请选择要删除的文件或空目录。"));
        return;
    }
    const ConnectionTarget target = selectedTarget();
    // 确认框必须写明服务器：删除是不可撤销的，而用户很可能同时开着多层、多标签页，
    // 只显示路径不足以让人确认自己在哪台机器上操作。
    QString message = tr("即将在服务器 %1 上永久删除%2：\n\n%3\n\n此操作无法撤销。")
                          .arg(target.connectionSpecifier(true),
                               entry->isDirectory ? tr("目录") : tr("文件"),
                               entry->path);
    if (target.confidence == ConnectionTarget::Confidence::Inferred) {
        message += tr("\n\n⚠ 该服务器是由解析终端命令推测得出的，可能并非你以为的那一台。"
                      "请先在终端执行 hostname 核对后再继续。");
    }
    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this,
        tr("确认删除"),
        message,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    setBusy(true, tr("正在删除 %1…").arg(entry->name));
    m_service->removePath(target, entry->path, entry->isDirectory);
}

void RemoteFileManagerWidget::navigateUp()
{
    QString path = m_pathEdit->text().trimmed();
    if (path.isEmpty() || path == QStringLiteral("/")) {
        m_pathEdit->setText(QStringLiteral("/"));
        return;
    }
    while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    const qsizetype separator = path.lastIndexOf(QLatin1Char('/'));
    m_pathEdit->setText(separator <= 0 ? QStringLiteral("/") : path.left(separator));
    refreshDirectory();
}

ConnectionTarget RemoteFileManagerWidget::selectedTarget() const
{
    const int index = m_targetCombo->currentIndex();
    return index >= 0 && index < m_targets.size() ? m_targets.at(index) : ConnectionTarget{};
}

std::optional<RemoteFileInfo> RemoteFileManagerWidget::selectedFile() const
{
    QTreeWidgetItem *item = m_fileTree->currentItem();
    if (item == nullptr) {
        return std::nullopt;
    }
    RemoteFileInfo entry;
    entry.name = item->text(0);
    entry.path = item->data(0, Qt::UserRole).toString();
    entry.isDirectory = item->data(0, Qt::UserRole + 1).toBool();
    entry.size = item->data(0, Qt::UserRole + 2).toULongLong();
    return entry;
}

void RemoteFileManagerWidget::setBusy(const bool busy, const QString &message)
{
    if (!message.isEmpty()) {
        m_statusLabel->setText(message);
        emit statusMessage(message);
    }
    if (busy) {
        m_progressBar->setRange(0, 0);
        m_progressBar->setVisible(true);
    } else {
        m_progressBar->setVisible(false);
        m_progressBar->setRange(0, 100);
    }
}

void RemoteFileManagerWidget::setActiveTransfer(const quint64 requestId)
{
    m_activeTransferRequest = requestId;
    m_cancelButton->setEnabled(requestId != 0);
}

void RemoteFileManagerWidget::handleRecursiveTransferFinished(
    const quint64 requestId,
    const SftpRecursive::TransferSummary &summary)
{
    if (requestId != m_activeTransferRequest) {
        return;
    }
    setActiveTransfer(0);
    const QString summaryText = summary.toMessage();
    setBusy(false, summaryText.section(QLatin1Char('\n'), 0, 0));

    // 跳过和失败必须主动弹给用户：把"有 37 个文件没传成功"埋进一行状态栏文字里，等同于
    // 让用户以为整个目录都传完了。
    if (summary.hasWarnings()) {
        QMessageBox::warning(this, tr("目录传输已结束（存在未完成项）"), summaryText);
    }
    emit statusMessage(summaryText.section(QLatin1Char('\n'), 0, 0));
    refreshDirectory();
}

void RemoteFileManagerWidget::handleOperationSuccess(const quint64 requestId,
                                                     const QString &description)
{
    Q_UNUSED(requestId)
    setBusy(false, description);
    refreshDirectory();
}

void RemoteFileManagerWidget::handleOperationFailure(const quint64 requestId,
                                                     const ConnectionTarget &target,
                                                     const SftpService::ErrorKind kind,
                                                     const QString &message)
{
    setBusy(false, message);

    // 递归传输在连接阶段或根目录创建阶段失败时走的是 operationFailed，而不是汇总信号；
    // 这里必须一并解除活动状态，否则取消按钮会永远亮着且挡住下一次传输。
    if (requestId == m_activeTransferRequest) {
        setActiveTransfer(0);
    }

    if (requestId == m_latestListRequest
        && kind == SftpService::ErrorKind::Connection
        && message.contains(QStringLiteral("disconnected"), Qt::CaseInsensitive)
        && target.stableKey() == selectedTarget().stableKey()) {
        int &retryCount = m_directoryDisconnectRetries[target.stableKey()];
        if (retryCount < 2) {
            ++retryCount;
            const int retryDelayMs = retryCount * 400;
            const QString retryMessage = tr("跳板连接暂时断开，正在进行第 %1 次重试…")
                                             .arg(retryCount);
            m_statusLabel->setText(retryMessage);
            emit statusMessage(retryMessage);
            // 仅自动重试幂等的目录读取；上传、重命名和删除绝不能在结果未知时自动重复。
            QTimer::singleShot(retryDelayMs, this, [this, target]() {
                if (target.stableKey() == selectedTarget().stableKey()) {
                    refreshDirectory();
                }
            });
            return;
        }
    }

    if (requestId == m_latestListRequest
        && kind == SftpService::ErrorKind::RemoteOperation
        && target.stableKey() == selectedTarget().stableKey()
        && !m_pathEdit->text().trimmed().isEmpty()) {
        // 记录的目录可能已经被删除或改名，也可能是用户手输了错误路径。退回主目录，
        // 否则用户会被卡在一个打不开的位置，只能自己猜一个存在的路径。
        m_pathByTarget.remove(target.stableKey());
        m_pathEdit->clear();
        emit statusMessage(tr("%1；已返回主目录").arg(message));
        refreshDirectory();
        return;
    }

    if (kind == SftpService::ErrorKind::Authentication
        && target.stableKey() == selectedTarget().stableKey()) {
        if (target.interactiveAuthenticationPending) {
            // 内层 OpenSSH 可能正在显示第二次 password 提示。此时弹出独立输入框会迫使用户
            // 输入两次；保持文件管理器静默，TerminalPane 捕获下一次终端输入后会更新凭据
            // 并通过 targetsChanged 自动重新发起当前目录读取。
            const QString retryMessage = tr("内层 SSH 认证尚未完成，请直接在终端重新输入密码");
            m_statusLabel->setText(retryMessage);
            emit statusMessage(retryMessage);
            return;
        }

        if (m_credentialDialogOpen) {
            // 多个请求可能同时失败（例如切换目录时前一个请求尚未返回）。只保留一个对话框，
            // 否则用户会看到密码框层层叠加，且后面几个填了也没用。
            return;
        }
        m_credentialDialogOpen = true;
        bool accepted = false;
        const QString password = QInputDialog::getText(
            this,
            tr("输入内层服务器凭据"),
            tr("服务器拒绝了当前会话凭据。请输入 %1 的新密码或私钥口令：")
                .arg(target.connectionSpecifier(true)),
            QLineEdit::Password,
            {},
            &accepted);
        m_credentialDialogOpen = false;
        if (accepted && !password.isEmpty()) {
            const int selectedIndex = m_targetCombo->currentIndex();
            if (selectedIndex >= 0 && selectedIndex < m_targets.size()) {
                // 密码只保存在当前进程的目标副本中，不写入数据库、日志或错误信息。
                m_targets[selectedIndex].password = password;
                m_sessionSecrets.insert(m_targets[selectedIndex].stableKey(), password);
                refreshDirectory();
            }
        }
        return;
    }

    if (kind == SftpService::ErrorKind::LegacyAlgorithmRequired
        && target.stableKey() == selectedTarget().stableKey()) {
        const QMessageBox::StandardButton answer = QMessageBox::warning(
            this,
            tr("服务器仅支持旧版 MAC 算法"),
            tr("服务器 %1 的 SFTP 服务仅提供 hmac-sha1。\n\n"
               "该算法已过时，建议优先升级服务器 SSH 配置。是否仅为这个连接启用并重试？\n\n"
               "%2")
                .arg(target.connectionSpecifier(true), message),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer == QMessageBox::Yes) {
            const int selectedIndex = m_targetCombo->currentIndex();
            if (selectedIndex >= 0 && selectedIndex < m_targets.size()) {
                // 兼容标志只写入当前文件管理器内存中的目标副本，不影响其他主机或后续新连接。
                m_targets[selectedIndex].allowLegacyHmacSha1 = true;
                refreshDirectory();
            }
        }
        return;
    }

    // 未知主机错误紧随指纹确认信号到达，由确认对话框负责提示，避免重复弹窗。
    if (kind != SftpService::ErrorKind::HostKeyUnknown) {
        QMessageBox::critical(this, tr("SFTP 操作失败"), message);
    }
}
