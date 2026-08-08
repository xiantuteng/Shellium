#include "sftp/RemoteFileManagerWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QPixmap>
#include <QRegularExpression>
#include <QSplitter>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <optional>
#include <utility>

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

/** @brief 把 POSIX 权限位格式化成固定四位八进制，包含 setuid/setgid/sticky 位。 */
QString formatPermissions(const quint32 permissions)
{
    return QString::number(permissions & 07777U, 8).rightJustified(4, QLatin1Char('0'));
}

/**
 * @brief 判断预览内容是否适合按只读 UTF-8 文本展示。
 *
 * MIME 仅由文件名和少量内容推断，不能作为安全边界；这里额外拒绝 NUL 和密集控制字符，
 * 避免把二进制数据塞进富文本/终端控件。预览最终使用 QPlainTextEdit，不解释 HTML 或脚本。
 */
bool looksLikeTextPreview(const QByteArray &data)
{
    if (data.contains('\0')) {
        return false;
    }
    qsizetype unsafeControls = 0;
    for (const unsigned char byte : data) {
        if ((byte < 0x20 && byte != '\n' && byte != '\r' && byte != '\t')
            || byte == 0x7f) {
            ++unsafeControls;
        }
    }
    return data.isEmpty() || unsafeControls * 100 <= data.size();
}

using SftpRecursive::joinRemotePath;

/** @brief 远程条目拖向本地栏时使用的进程内 MIME 类型。 */
constexpr char kRemotePathMimeType[] = "application/x-shellium-remote-path";

/** @brief 与远程路径配套的目录标志，避免仅凭扩展名猜测条目类型。 */
constexpr char kRemoteDirectoryMimeType[] = "application/x-shellium-remote-directory";

/**
 * @brief 支持把本地文件系统 URL 拖入的远程文件树。
 *
 * 控件只负责把 Qt 拖放事件转换成路径和落点；服务器选择、路径校验和实际 SFTP 操作仍由
 * RemoteFileManagerWidget 统一处理，避免视图类持有凭据或网络对象。
 */
class RemoteDropTree final : public QTreeWidget {
public:
    using LocalDropHandler =
        std::function<void(const QStringList &localPaths, QTreeWidgetItem *dropItem)>;

    explicit RemoteDropTree(QWidget *parent = nullptr)
        : QTreeWidget(parent)
    {
        setAcceptDrops(true);
        setDragEnabled(true);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::CopyAction);
    }

    /** @brief 注入本地路径落到远端树后的业务处理函数。 */
    void setLocalDropHandler(LocalDropHandler handler)
    {
        m_localDropHandler = std::move(handler);
    }

protected:
    /** @brief 只接受本地文件 URL；远程树内部不允许通过拖拽重排。 */
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasUrls()) {
            event->acceptProposedAction();
            return;
        }
        event->ignore();
    }

    /** @brief 在合法本地文件拖过时持续显示可放置反馈。 */
    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->mimeData()->hasUrls()) {
            event->acceptProposedAction();
            return;
        }
        event->ignore();
    }

    /** @brief 收集本地绝对路径，并把鼠标下的远程目录节点一并交给业务层。 */
    void dropEvent(QDropEvent *event) override
    {
        if (!event->mimeData()->hasUrls() || !m_localDropHandler) {
            event->ignore();
            return;
        }
        QStringList localPaths;
        for (const QUrl &url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                localPaths.append(QDir::cleanPath(url.toLocalFile()));
            }
        }
        if (localPaths.isEmpty()) {
            event->ignore();
            return;
        }
        m_localDropHandler(localPaths, itemAt(event->position().toPoint()));
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }

    /** @brief 把当前远程条目的完整路径和目录类型编码为自定义拖拽数据。 */
    void startDrag(Qt::DropActions supportedActions) override
    {
        Q_UNUSED(supportedActions)
        QTreeWidgetItem *item = currentItem();
        if (item == nullptr) {
            return;
        }
        auto *mimeData = new QMimeData;
        mimeData->setData(kRemotePathMimeType,
                          item->data(0, Qt::UserRole).toString().toUtf8());
        mimeData->setData(kRemoteDirectoryMimeType,
                          item->data(0, Qt::UserRole + 1).toBool()
                              ? QByteArrayLiteral("1")
                              : QByteArrayLiteral("0"));
        auto *drag = new QDrag(this);
        drag->setMimeData(mimeData);
        drag->exec(Qt::CopyAction);
    }

private:
    /** @brief 收到合法本地 URL 拖入后调用的业务回调。 */
    LocalDropHandler m_localDropHandler;
};

/**
 * @brief 接收 Shellium 远程条目并把目标解析为本地目录的文件树。
 *
 * QFileSystemModel 保持只读，拖入动作仅代表“下载副本”，绝不会调用本地移动或删除；这对
 * 运维文件管理尤为重要，因为一次误判为 MoveAction 就可能破坏用户本机原文件。
 */
class LocalDropTree final : public QTreeView {
public:
    using RemoteDropHandler =
        std::function<void(const QString &remotePath,
                           bool directory,
                           const QString &localDirectory)>;

    explicit LocalDropTree(QWidget *parent = nullptr)
        : QTreeView(parent)
    {
        setAcceptDrops(true);
        setDragEnabled(true);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::CopyAction);
    }

    /** @brief 注入远程条目落到本地树后的下载处理函数。 */
    void setRemoteDropHandler(RemoteDropHandler handler)
    {
        m_remoteDropHandler = std::move(handler);
    }

protected:
    /** @brief 只接收本控件定义的远程条目格式，本地内部拖拽不会触发文件移动。 */
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasFormat(kRemotePathMimeType)) {
            event->acceptProposedAction();
            return;
        }
        event->ignore();
    }

    /** @brief 在远程条目拖过时持续显示复制反馈。 */
    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->mimeData()->hasFormat(kRemotePathMimeType)) {
            event->acceptProposedAction();
            return;
        }
        event->ignore();
    }

    /** @brief 将落点文件转换为其父目录，将落点目录作为直接下载目标。 */
    void dropEvent(QDropEvent *event) override
    {
        if (!event->mimeData()->hasFormat(kRemotePathMimeType)
            || !m_remoteDropHandler) {
            event->ignore();
            return;
        }
        const auto *fileModel = qobject_cast<const QFileSystemModel *>(model());
        if (fileModel == nullptr) {
            event->ignore();
            return;
        }

        const QModelIndex dropIndex = indexAt(event->position().toPoint());
        QString localDirectory = fileModel->filePath(dropIndex.isValid() ? dropIndex
                                                                         : rootIndex());
        if (dropIndex.isValid() && !fileModel->isDir(dropIndex)) {
            localDirectory = QFileInfo(localDirectory).absolutePath();
        }
        const QString remotePath = QString::fromUtf8(
            event->mimeData()->data(kRemotePathMimeType));
        const bool directory =
            event->mimeData()->data(kRemoteDirectoryMimeType) == QByteArrayLiteral("1");
        m_remoteDropHandler(remotePath, directory, localDirectory);
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }

private:
    /** @brief 收到合法远程条目拖入后调用的业务回调。 */
    RemoteDropHandler m_remoteDropHandler;
};

} // namespace

RemoteFileManagerWidget::RemoteFileManagerWidget(QWidget *parent)
    : QWidget(parent)
    , m_targetCombo(new QComboBox(this))
    , m_followInnermostCheck(new QCheckBox(tr("自动跟随最内层"), this))
    , m_localPathEdit(new QLineEdit(QDir::homePath(), this))
    , m_localModel(new QFileSystemModel(this))
    , m_localTree(new LocalDropTree(this))
    , m_pathEdit(new QLineEdit(QStringLiteral("/"), this))
    , m_fileTree(new RemoteDropTree(this))
    , m_statusLabel(new QLabel(tr("请选择终端服务器"), this))
    , m_progressBar(new QProgressBar(this))
    , m_cancelButton(new QPushButton(tr("取消选中"), this))
    , m_pauseButton(new QPushButton(tr("暂停"), this))
    , m_resumeTransfersCheck(new QCheckBox(tr("优先断点续传"), this))
    , m_transferQueueList(new QListWidget(this))
    , m_service(new SftpService(this))
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    mainLayout->setSpacing(6);

    auto *serverRow = new QHBoxLayout;
    serverRow->addWidget(new QLabel(tr("服务器："), this));
    serverRow->addWidget(m_targetCombo, 1);
    m_followInnermostCheck->setChecked(true);
    m_followInnermostCheck->setToolTip(
        tr("开启后自动切换到连接链的最新内层；手动选择外层会自动关闭。"));
    serverRow->addWidget(m_followInnermostCheck);
    mainLayout->addLayout(serverRow);

    auto *contentSplitter = new QSplitter(Qt::Horizontal, this);
    auto *localPanel = new QWidget(contentSplitter);
    auto *localLayout = new QVBoxLayout(localPanel);
    localLayout->setContentsMargins(0, 0, 0, 0);
    localLayout->setSpacing(4);
    localLayout->addWidget(new QLabel(tr("本地文件"), localPanel));
    auto *localPathRow = new QHBoxLayout;
    auto *localUpButton = new QPushButton(tr("上级"), localPanel);
    auto *localRefreshButton = new QPushButton(tr("刷新"), localPanel);
    localPathRow->addWidget(localUpButton);
    localPathRow->addWidget(m_localPathEdit, 1);
    localPathRow->addWidget(localRefreshButton);
    localLayout->addLayout(localPathRow);

    // QFileSystemModel 自带按需加载和文件系统监听，适合大目录；保持只读可确保拖放只触发
    // Shellium 的上传/下载，不会意外在本机执行移动、重命名或删除。
    m_localModel->setReadOnly(true);
    m_localModel->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    m_localModel->setRootPath(QDir::rootPath());
    m_localTree->setModel(m_localModel);
    m_localTree->setRootIndex(m_localModel->index(QDir::homePath()));
    m_localTree->setAlternatingRowColors(true);
    m_localTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_localTree->setColumnWidth(0, 230);
    localLayout->addWidget(m_localTree, 1);

    auto *remotePanel = new QWidget(contentSplitter);
    auto *remoteLayout = new QVBoxLayout(remotePanel);
    remoteLayout->setContentsMargins(0, 0, 0, 0);
    remoteLayout->setSpacing(4);
    remoteLayout->addWidget(new QLabel(tr("远程文件"), remotePanel));
    auto *pathRow = new QHBoxLayout;
    auto *upButton = new QPushButton(tr("上级"), this);
    auto *refreshButton = new QPushButton(tr("刷新"), this);
    pathRow->addWidget(upButton);
    pathRow->addWidget(m_pathEdit, 1);
    pathRow->addWidget(refreshButton);
    remoteLayout->addLayout(pathRow);

    m_fileTree->setColumnCount(6);
    m_fileTree->setHeaderLabels(
        {tr("名称"), tr("大小"), tr("类型"), tr("权限"), tr("所有者"), tr("修改时间")});
    m_fileTree->setRootIsDecorated(false);
    m_fileTree->setAlternatingRowColors(true);
    m_fileTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_fileTree->header()->setStretchLastSection(false);
    m_fileTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_fileTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_fileTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_fileTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_fileTree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_fileTree->header()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    remoteLayout->addWidget(m_fileTree, 1);

    contentSplitter->addWidget(localPanel);
    contentSplitter->addWidget(remotePanel);
    contentSplitter->setChildrenCollapsible(false);
    contentSplitter->setStretchFactor(0, 1);
    contentSplitter->setStretchFactor(1, 1);
    contentSplitter->setSizes({420, 520});
    mainLayout->addWidget(contentSplitter, 1);

    auto *operationRow = new QHBoxLayout;
    auto *uploadButton = new QPushButton(tr("上传文件"), this);
    auto *uploadDirectoryButton = new QPushButton(tr("上传目录"), this);
    auto *downloadButton = new QPushButton(tr("下载"), this);
    auto *previewButton = new QPushButton(tr("预览"), this);
    auto *chmodButton = new QPushButton(tr("权限"), this);
    auto *renameButton = new QPushButton(tr("重命名"), this);
    auto *deleteButton = new QPushButton(tr("删除"), this);
    m_cancelButton->setEnabled(false);
    m_pauseButton->setEnabled(false);
    m_resumeTransfersCheck->setChecked(true);
    m_resumeTransfersCheck->setToolTip(
        tr("开启时从已有较小文件继续；关闭时始终截断并覆盖重传。"));
    operationRow->addWidget(uploadButton);
    operationRow->addWidget(uploadDirectoryButton);
    operationRow->addWidget(downloadButton);
    operationRow->addWidget(previewButton);
    operationRow->addWidget(chmodButton);
    operationRow->addWidget(renameButton);
    operationRow->addWidget(deleteButton);
    operationRow->addStretch(1);
    operationRow->addWidget(m_resumeTransfersCheck);
    operationRow->addWidget(m_pauseButton);
    operationRow->addWidget(m_cancelButton);
    mainLayout->addLayout(operationRow);

    m_progressBar->setVisible(false);
    m_progressBar->setRange(0, 100);
    mainLayout->addWidget(m_progressBar);
    m_transferQueueList->setMaximumHeight(110);
    m_transferQueueList->setAlternatingRowColors(true);
    m_transferQueueList->setToolTip(
        tr("传输严格按顺序执行；选择等待项后点击“取消选中”可直接移除。"));
    mainLayout->addWidget(m_transferQueueList);
    mainLayout->addWidget(m_statusLabel);

    connect(refreshButton, &QPushButton::clicked, this, &RemoteFileManagerWidget::refreshDirectory);
    connect(upButton, &QPushButton::clicked, this, &RemoteFileManagerWidget::navigateUp);
    connect(m_pathEdit, &QLineEdit::returnPressed, this, &RemoteFileManagerWidget::refreshDirectory);
    connect(localUpButton, &QPushButton::clicked, this, [this]() {
        setLocalDirectory(QFileInfo(m_localPathEdit->text().trimmed()).absolutePath());
    });
    connect(localRefreshButton, &QPushButton::clicked, this, [this]() {
        setLocalDirectory(m_localPathEdit->text().trimmed());
    });
    connect(m_localPathEdit, &QLineEdit::returnPressed, this, [this]() {
        setLocalDirectory(m_localPathEdit->text().trimmed());
    });
    connect(m_localTree, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        if (m_localModel->isDir(index)) {
            setLocalDirectory(m_localModel->filePath(index));
        }
    });
    connect(m_targetCombo, &QComboBox::currentIndexChanged, this, [this](const int) {
        if (m_targetCombo->currentIndex() >= 0) {
            if (m_followInnermostCheck->isChecked()
                && m_targetCombo->currentIndex() != m_targets.size() - 1) {
                // 选择外层代表明确的人工意图；自动关闭跟随后，目录刷新、凭据更新等无关的
                // targetsChanged 不会再把用户抢回最内层。
                m_followInnermostCheck->setChecked(false);
            }
            m_fileTree->clear();
            restorePathForSelectedTarget();
            refreshDirectory();
        }
    });
    connect(m_followInnermostCheck, &QCheckBox::toggled, this, [this](const bool enabled) {
        if (!enabled || m_targets.isEmpty()) {
            return;
        }
        const int innermostIndex = static_cast<int>(m_targets.size() - 1);
        if (m_targetCombo->currentIndex() != innermostIndex) {
            m_targetCombo->setCurrentIndex(innermostIndex);
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
    connect(m_pauseButton,
            &QPushButton::clicked,
            this,
            &RemoteFileManagerWidget::toggleActiveTransferPause);
    connect(downloadButton, &QPushButton::clicked, this, &RemoteFileManagerWidget::downloadSelectedFile);
    connect(previewButton, &QPushButton::clicked, this, &RemoteFileManagerWidget::previewSelectedFile);
    connect(chmodButton, &QPushButton::clicked, this, &RemoteFileManagerWidget::chmodSelectedPath);
    connect(renameButton, &QPushButton::clicked, this, &RemoteFileManagerWidget::renameSelectedPath);
    connect(deleteButton, &QPushButton::clicked, this, &RemoteFileManagerWidget::deleteSelectedPath);

    auto *remoteDropTree = static_cast<RemoteDropTree *>(m_fileTree);
    remoteDropTree->setLocalDropHandler(
        [this](const QStringList &localPaths, QTreeWidgetItem *dropItem) {
            QString remoteDirectory = m_pathEdit->text().trimmed();
            const std::optional<RemoteFileInfo> dropEntry = fileFromItem(dropItem);
            if (dropEntry.has_value() && dropEntry->isDirectory) {
                remoteDirectory = dropEntry->path;
            }
            uploadLocalPaths(localPaths, remoteDirectory);
        });
    auto *localDropTree = static_cast<LocalDropTree *>(m_localTree);
    localDropTree->setRemoteDropHandler(
        [this](const QString &remotePath, const bool directory, const QString &localDirectory) {
            RemoteFileInfo entry;
            entry.path = remotePath;
            entry.name = remotePath.section(QLatin1Char('/'), -1);
            entry.isDirectory = directory;
            downloadRemoteEntryToDirectory(entry, localDirectory);
        });

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
                    item->setText(3, formatPermissions(entry.permissions));
                    item->setText(4, entry.owner);
                    item->setText(5, entry.modifiedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
                    item->setData(0, Qt::UserRole, entry.path);
                    item->setData(0, Qt::UserRole + 1, entry.isDirectory);
                    item->setData(0, Qt::UserRole + 2, QVariant::fromValue(entry.size));
                    item->setData(0, Qt::UserRole + 3, QVariant::fromValue(entry.permissions));
                    item->setData(0, Qt::UserRole + 4, entry.isSymbolicLink);
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
            [this](const quint64 requestId, const quint64 completed, const quint64 total) {
                if (requestId != m_activeTransferRequest) {
                    return;
                }
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
    connect(m_service,
            &SftpService::filePreviewReady,
            this,
            [this](const quint64,
                   const ConnectionTarget &target,
                   const QString &remotePath,
                   const QByteArray &data,
                   const QString &mimeType,
                   const bool truncated) {
                if (target.stableKey() != selectedTarget().stableKey()) {
                    // 用户已经切换服务器时不弹出旧请求内容，避免把 A 服务器文件误认为 B。
                    return;
                }
                setBusy(false, tr("已读取 %1 的安全预览").arg(remotePath));

                auto *dialog = new QDialog(this);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                dialog->setWindowTitle(tr("安全预览 · %1").arg(remotePath));
                dialog->resize(760, 560);
                auto *layout = new QVBoxLayout(dialog);
                if (truncated) {
                    auto *notice = new QLabel(
                        tr("仅显示前 %1，文件内容已截断。")
                            .arg(formatSize(static_cast<quint64>(data.size()))),
                        dialog);
                    notice->setWordWrap(true);
                    layout->addWidget(notice);
                }

                static const QStringList safeRasterTypes{
                    QStringLiteral("image/png"), QStringLiteral("image/jpeg"),
                    QStringLiteral("image/gif"), QStringLiteral("image/webp"),
                    QStringLiteral("image/bmp")};
                QImage image;
                if (safeRasterTypes.contains(mimeType)) {
                    image = QImage::fromData(data);
                }
                if (!image.isNull()) {
                    auto *imageLabel = new QLabel(dialog);
                    imageLabel->setAlignment(Qt::AlignCenter);
                    imageLabel->setPixmap(QPixmap::fromImage(image).scaled(
                        QSize(720, 500), Qt::KeepAspectRatio, Qt::SmoothTransformation));
                    layout->addWidget(imageLabel, 1);
                } else if (mimeType.startsWith(QStringLiteral("text/"))
                           || looksLikeTextPreview(data)) {
                    auto *textView = new QPlainTextEdit(dialog);
                    textView->setReadOnly(true);
                    textView->setLineWrapMode(QPlainTextEdit::NoWrap);
                    // QPlainTextEdit 只按纯文本展示，不解析 HTML、终端转义序列或脚本。
                    textView->setPlainText(QString::fromUtf8(data));
                    layout->addWidget(textView, 1);
                } else {
                    auto *unsupported = new QLabel(
                        tr("该文件不是受支持的文本或安全栅格图片，未展示二进制内容。\nMIME：%1")
                            .arg(mimeType),
                        dialog);
                    unsupported->setWordWrap(true);
                    layout->addWidget(unsupported);
                }
                dialog->show();
            });

    rebuildTransferQueueView();
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
    const int previousIndex = m_targetCombo->currentIndex();
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

    QStringList stableKeys;
    stableKeys.reserve(m_targets.size());
    for (const ConnectionTarget &target : std::as_const(m_targets)) {
        stableKeys.append(target.stableKey());
    }
    // followInnermost 是调用方对本次连接链变化的建议，复选框才是用户在当前标签页内持久的
    // 偏好。两者同时为 true 才自动切到内层；人工关闭后，普通刷新不得覆盖其选择。
    const int selectedIndex = SftpUiState::selectTargetIndex(
        stableKeys,
        previousKey,
        previousIndex,
        followInnermost && m_followInnermostCheck->isChecked());
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

void RemoteFileManagerWidget::setLocalDirectory(const QString &directory)
{
    const QFileInfo directoryInfo(QDir::cleanPath(directory));
    if (!directoryInfo.exists() || !directoryInfo.isDir()) {
        QMessageBox::warning(this,
                             tr("本地路径无效"),
                             tr("本地目录不存在或不可访问：\n%1").arg(directory));
        return;
    }

    const QString absolutePath = directoryInfo.absoluteFilePath();
    const QModelIndex rootIndex = m_localModel->index(absolutePath);
    if (!rootIndex.isValid()) {
        QMessageBox::warning(this,
                             tr("无法浏览本地目录"),
                             tr("无法为该目录创建文件系统视图：\n%1").arg(absolutePath));
        return;
    }
    m_localPathEdit->setText(absolutePath);
    m_localTree->setRootIndex(rootIndex);
}

SftpTransfer::ExistingFilePolicy RemoteFileManagerWidget::selectedTransferPolicy() const
{
    return m_resumeTransfersCheck->isChecked()
        ? SftpTransfer::ExistingFilePolicy::ResumeWhenPossible
        : SftpTransfer::ExistingFilePolicy::Overwrite;
}

void RemoteFileManagerWidget::enqueueTransfer(QueuedTransfer transfer)
{
    transfer.queueId = m_nextTransferQueueId++;
    m_pendingTransfers.append(std::move(transfer));
    rebuildTransferQueueView();
    startNextQueuedTransfer();
}

void RemoteFileManagerWidget::startNextQueuedTransfer()
{
    if (m_activeQueuedTransfer.has_value() || m_pendingTransfers.isEmpty()) {
        return;
    }

    m_activeQueuedTransfer = m_pendingTransfers.takeFirst();
    const QueuedTransfer &transfer = *m_activeQueuedTransfer;
    m_activeTransferPaused = false;
    m_pauseButton->setText(tr("暂停"));
    setBusy(true, tr("正在执行：%1").arg(transfer.description));

    switch (transfer.kind) {
    case QueuedTransferKind::UploadFile:
        m_activeTransferRequest = m_service->uploadFile(transfer.target,
                                                        transfer.sourcePath,
                                                        transfer.destinationPath,
                                                        transfer.policy);
        break;
    case QueuedTransferKind::UploadDirectory:
        m_activeTransferRequest = m_service->uploadDirectory(transfer.target,
                                                             transfer.sourcePath,
                                                             transfer.destinationPath,
                                                             transfer.policy);
        break;
    case QueuedTransferKind::DownloadFile:
        m_activeTransferRequest = m_service->downloadFile(transfer.target,
                                                          transfer.sourcePath,
                                                          transfer.destinationPath,
                                                          transfer.policy);
        break;
    case QueuedTransferKind::DownloadDirectory:
        m_activeTransferRequest = m_service->downloadDirectory(transfer.target,
                                                               transfer.sourcePath,
                                                               transfer.destinationPath,
                                                               transfer.policy);
        break;
    }
    rebuildTransferQueueView();
}

void RemoteFileManagerWidget::finishQueuedTransfer(const quint64 requestId)
{
    if (requestId == 0 || requestId != m_activeTransferRequest) {
        return;
    }
    m_activeTransferRequest = 0;
    m_activeQueuedTransfer.reset();
    m_activeTransferPaused = false;
    m_pauseButton->setText(tr("暂停"));
    rebuildTransferQueueView();

    // 信号处理函数可能仍在展示失败或汇总对话框；把下一项放到事件循环末尾启动，避免前一项
    // 的状态更新和后一项首个进度信号交错覆盖。
    QTimer::singleShot(0, this, &RemoteFileManagerWidget::startNextQueuedTransfer);
}

void RemoteFileManagerWidget::rebuildTransferQueueView()
{
    const quint64 selectedQueueId = m_transferQueueList->currentItem() == nullptr
        ? 0
        : m_transferQueueList->currentItem()->data(Qt::UserRole).toULongLong();
    m_transferQueueList->clear();

    const auto appendItem = [this, selectedQueueId](const QueuedTransfer &transfer,
                                                     const QString &prefix) {
        auto *item = new QListWidgetItem(prefix + transfer.description, m_transferQueueList);
        item->setData(Qt::UserRole, QVariant::fromValue(transfer.queueId));
        if (transfer.queueId == selectedQueueId) {
            m_transferQueueList->setCurrentItem(item);
        }
    };
    if (m_activeQueuedTransfer.has_value()) {
        appendItem(*m_activeQueuedTransfer,
                   m_activeTransferPaused ? tr("⏸ 已暂停 · ") : tr("▶ 进行中 · "));
    }
    for (const QueuedTransfer &transfer : std::as_const(m_pendingTransfers)) {
        appendItem(transfer, tr("○ 等待 · "));
    }

    const bool hasActive = m_activeQueuedTransfer.has_value();
    m_pauseButton->setEnabled(hasActive);
    m_cancelButton->setEnabled(hasActive || !m_pendingTransfers.isEmpty());
    m_transferQueueList->setVisible(hasActive || !m_pendingTransfers.isEmpty());
}

void RemoteFileManagerWidget::uploadLocalPaths(const QStringList &localPaths,
                                               const QString &remoteDirectory)
{
    const ConnectionTarget target = selectedTarget();
    if (!target.isValid()) {
        QMessageBox::information(this, tr("尚未连接"), tr("请先连接并选择一台远程服务器。"));
        return;
    }
    if (remoteDirectory.isEmpty() || !remoteDirectory.startsWith(QLatin1Char('/'))) {
        QMessageBox::warning(this,
                             tr("远程路径未知"),
                             tr("请先进入一个远程目录，再拖入本地文件。"));
        return;
    }

    QList<QFileInfo> acceptedEntries;
    for (const QString &localPath : localPaths) {
        const QFileInfo info(localPath);
        if (!info.exists() || info.isSymbolicLink()) {
            // 拖拽入口也遵守递归传输“不跟随符号链接”的边界。单个链接若被当作普通文件
            // 上传，可能把用户未选择的目标内容泄漏到服务器。
            continue;
        }
        if (info.isDir() || info.isFile()) {
            acceptedEntries.append(info);
        }
    }
    if (acceptedEntries.isEmpty()) {
        QMessageBox::information(this,
                                 tr("没有可上传条目"),
                                 tr("拖入的条目不存在、不可读取或是不会跟随的符号链接。"));
        return;
    }

    const SftpTransfer::ExistingFilePolicy policy = selectedTransferPolicy();
    for (const QFileInfo &info : std::as_const(acceptedEntries)) {
        QueuedTransfer transfer;
        transfer.kind = info.isDir() ? QueuedTransferKind::UploadDirectory
                                     : QueuedTransferKind::UploadFile;
        transfer.target = target;
        transfer.sourcePath = info.absoluteFilePath();
        // 目录 API 接收远程父目录，文件 API 接收完整目标路径，两者不能混用。
        transfer.destinationPath = info.isDir()
            ? remoteDirectory
            : joinRemotePath(remoteDirectory, info.fileName());
        transfer.policy = policy;
        transfer.description = tr("上传 %1 → %2")
                                   .arg(info.fileName(), remoteDirectory);
        enqueueTransfer(std::move(transfer));
    }
    emit statusMessage(tr("已将 %1 个本地条目加入传输队列").arg(acceptedEntries.size()));
}

void RemoteFileManagerWidget::downloadRemoteEntryToDirectory(
    const RemoteFileInfo &entry,
    const QString &localDirectory)
{
    const QFileInfo destinationInfo(localDirectory);
    if (!destinationInfo.exists() || !destinationInfo.isDir()) {
        QMessageBox::warning(this,
                             tr("本地路径无效"),
                             tr("下载目标不是可访问目录：\n%1").arg(localDirectory));
        return;
    }
    if (entry.path.isEmpty() || entry.name.isEmpty()) {
        return;
    }

    QueuedTransfer transfer;
    transfer.kind = entry.isDirectory ? QueuedTransferKind::DownloadDirectory
                                      : QueuedTransferKind::DownloadFile;
    transfer.target = selectedTarget();
    transfer.sourcePath = entry.path;
    // 目录 API 接收本地父目录；文件 API 必须得到包含文件名的完整目标路径。
    transfer.destinationPath = entry.isDirectory
        ? destinationInfo.absoluteFilePath()
        : QDir(destinationInfo.absoluteFilePath()).filePath(entry.name);
    transfer.policy = selectedTransferPolicy();
    transfer.description = tr("下载 %1 → %2")
                               .arg(entry.name, destinationInfo.absoluteFilePath());
    enqueueTransfer(std::move(transfer));
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
    const QStringList localPaths = QFileDialog::getOpenFileNames(
        this, tr("选择要上传的一个或多个文件"), m_localPathEdit->text().trimmed());
    if (localPaths.isEmpty()) {
        return;
    }
    uploadLocalPaths(localPaths, m_pathEdit->text().trimmed());
}

void RemoteFileManagerWidget::requestUploadToCurrentDirectory()
{
    // 从终端右键进入时文件管理面板可能处于隐藏或尚未加载路径的状态。必须在弹出本地文件
    // 选择器之前验证目标，避免用户选完文件后才得知没有可上传的服务器或目录。
    if (!selectedTarget().isValid()) {
        QMessageBox::information(this, tr("尚未连接"), tr("请先连接并选择一台远程服务器。"));
        return;
    }
    const QString remoteDirectory = m_pathEdit->text().trimmed();
    if (remoteDirectory.isEmpty() || !remoteDirectory.startsWith(QLatin1Char('/'))) {
        QMessageBox::warning(this,
                             tr("远程路径未知"),
                             tr("请先在远程文件管理器中进入一个目录，再从终端发起上传。"));
        return;
    }

    // 校验通过后仍走和工具栏完全相同的选择与队列方法，不创建第二套隐式传输状态。
    uploadSelectedFile();
}

void RemoteFileManagerWidget::uploadSelectedDirectory()
{
    const QString localDirectory = QFileDialog::getExistingDirectory(
        this, tr("选择要上传的目录"), m_localPathEdit->text().trimmed());
    if (localDirectory.isEmpty()) {
        return;
    }
    const QString remoteParent = m_pathEdit->text().trimmed();
    if (remoteParent.isEmpty()) {
        QMessageBox::warning(this, tr("路径未知"),
                             tr("请先进入一个远程目录，再上传本地目录。"));
        return;
    }
    uploadLocalPaths({localDirectory}, remoteParent);
}

void RemoteFileManagerWidget::downloadSelectedFile()
{
    const QList<RemoteFileInfo> entries = selectedFiles();
    if (entries.isEmpty()) {
        QMessageBox::information(this, tr("请选择条目"), tr("请选择一个要下载的远程文件或目录。"));
        return;
    }
    const QString localParent = QFileDialog::getExistingDirectory(
        this, tr("选择批量下载的本地目录"), m_localPathEdit->text().trimmed());
    if (localParent.isEmpty()) {
        return;
    }
    for (const RemoteFileInfo &entry : entries) {
        downloadRemoteEntryToDirectory(entry, localParent);
    }
}

void RemoteFileManagerWidget::cancelActiveTransfer()
{
    quint64 selectedQueueId = 0;
    if (m_transferQueueList->currentItem() != nullptr) {
        selectedQueueId = m_transferQueueList->currentItem()->data(Qt::UserRole).toULongLong();
    }

    if (selectedQueueId != 0
        && (!m_activeQueuedTransfer.has_value()
            || selectedQueueId != m_activeQueuedTransfer->queueId)) {
        for (auto iterator = m_pendingTransfers.begin(); iterator != m_pendingTransfers.end(); ++iterator) {
            if (iterator->queueId == selectedQueueId) {
                const QString description = iterator->description;
                m_pendingTransfers.erase(iterator);
                rebuildTransferQueueView();
                const QString message = tr("已取消等待项：%1").arg(description);
                m_statusLabel->setText(message);
                emit statusMessage(message);
                return;
            }
        }
    }

    if (m_activeTransferRequest == 0 || !m_activeQueuedTransfer.has_value()) {
        return;
    }
    m_service->cancelRequest(m_activeTransferRequest);
    // 后台任务要跑到下一个分块边界才会停，先把按钮禁掉避免用户反复点击并以为没生效。
    m_cancelButton->setEnabled(false);
    const QString message = tr("已请求取消传输，正在收尾…");
    m_statusLabel->setText(message);
    emit statusMessage(message);
}

void RemoteFileManagerWidget::toggleActiveTransferPause()
{
    if (m_activeTransferRequest == 0 || !m_activeQueuedTransfer.has_value()) {
        return;
    }
    m_activeTransferPaused = !m_activeTransferPaused;
    if (m_activeTransferPaused) {
        m_service->pauseRequest(m_activeTransferRequest);
        m_pauseButton->setText(tr("恢复"));
        emit statusMessage(tr("传输将在下一个 64 KiB 分块边界暂停"));
    } else {
        m_service->resumeRequest(m_activeTransferRequest);
        m_pauseButton->setText(tr("暂停"));
        emit statusMessage(tr("传输已恢复"));
    }
    rebuildTransferQueueView();
}

void RemoteFileManagerWidget::previewSelectedFile()
{
    const std::optional<RemoteFileInfo> entry = selectedFile();
    if (!entry.has_value() || entry->isDirectory || entry->isSymbolicLink) {
        QMessageBox::information(this,
                                 tr("无法预览"),
                                 tr("请选择一个普通远程文件；目录和符号链接不进入安全预览。"));
        return;
    }
    setBusy(true, tr("正在安全读取 %1 的预览…").arg(entry->name));
    m_service->previewFile(selectedTarget(),
                           entry->path,
                           SftpTransfer::MaximumPreviewBytes);
}

void RemoteFileManagerWidget::chmodSelectedPath()
{
    const std::optional<RemoteFileInfo> entry = selectedFile();
    if (!entry.has_value()) {
        QMessageBox::information(this, tr("请选择条目"), tr("请选择要修改权限的文件或目录。"));
        return;
    }
    bool accepted = false;
    const QString initial = formatPermissions(entry->permissions);
    const QString input = QInputDialog::getText(
        this,
        tr("修改远程权限"),
        tr("请输入三位或四位八进制权限（例如 0644、0755）："),
        QLineEdit::Normal,
        initial,
        &accepted).trimmed();
    if (!accepted) {
        return;
    }
    static const QRegularExpression octalPattern(QStringLiteral("^[0-7]{3,4}$"));
    if (!octalPattern.match(input).hasMatch()) {
        QMessageBox::warning(this,
                             tr("权限格式无效"),
                             tr("权限只能包含 0 到 7，长度必须为三位或四位。"));
        return;
    }
    bool converted = false;
    const quint32 permissions = input.toUInt(&converted, 8);
    if (!converted || permissions > 07777U) {
        QMessageBox::warning(this, tr("权限格式无效"), tr("权限值超出 POSIX 允许范围。"));
        return;
    }
    setBusy(true, tr("正在修改 %1 的权限…").arg(entry->name));
    m_service->chmodPath(selectedTarget(), entry->path, permissions);
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
    return fileFromItem(m_fileTree->currentItem());
}

QList<RemoteFileInfo> RemoteFileManagerWidget::selectedFiles() const
{
    QList<RemoteFileInfo> entries;
    for (QTreeWidgetItem *item : m_fileTree->selectedItems()) {
        const std::optional<RemoteFileInfo> entry = fileFromItem(item);
        if (entry.has_value()) {
            entries.append(*entry);
        }
    }
    return entries;
}

std::optional<RemoteFileInfo> RemoteFileManagerWidget::fileFromItem(
    QTreeWidgetItem *item) const
{
    if (item == nullptr) {
        return std::nullopt;
    }
    RemoteFileInfo entry;
    entry.name = item->text(0);
    entry.path = item->data(0, Qt::UserRole).toString();
    entry.isDirectory = item->data(0, Qt::UserRole + 1).toBool();
    entry.size = item->data(0, Qt::UserRole + 2).toULongLong();
    entry.permissions = item->data(0, Qt::UserRole + 3).toUInt();
    entry.isSymbolicLink = item->data(0, Qt::UserRole + 4).toBool();
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

void RemoteFileManagerWidget::handleRecursiveTransferFinished(
    const quint64 requestId,
    const SftpRecursive::TransferSummary &summary)
{
    if (requestId != m_activeTransferRequest) {
        return;
    }
    const QString summaryText = summary.toMessage();
    setBusy(false, summaryText.section(QLatin1Char('\n'), 0, 0));

    // 跳过和失败必须主动弹给用户：把"有 37 个文件没传成功"埋进一行状态栏文字里，等同于
    // 让用户以为整个目录都传完了。
    if (summary.hasWarnings()) {
        QMessageBox::warning(this, tr("目录传输已结束（存在未完成项）"), summaryText);
    }
    emit statusMessage(summaryText.section(QLatin1Char('\n'), 0, 0));
    refreshDirectory();
    finishQueuedTransfer(requestId);
}

void RemoteFileManagerWidget::handleOperationSuccess(const quint64 requestId,
                                                     const QString &description)
{
    setBusy(false, description);
    refreshDirectory();
    finishQueuedTransfer(requestId);
}

void RemoteFileManagerWidget::handleOperationFailure(const quint64 requestId,
                                                     const ConnectionTarget &target,
                                                     const SftpService::ErrorKind kind,
                                                     const QString &message)
{
    const bool wasActiveTransfer = requestId == m_activeTransferRequest;
    setBusy(false, message);

    // 单文件失败以及递归传输在连接/根目录阶段的失败都走本信号。先释放活动队列项，再让
    // 事件循环启动下一项；后续认证或错误对话框仍可完整展示，不会卡死整个队列。
    if (wasActiveTransfer) {
        finishQueuedTransfer(requestId);
    }

    if (wasActiveTransfer && message.contains(tr("已取消"))) {
        // 用户主动取消属于预期控制流，不应再弹红色错误框；保留半成品供下一次续传。
        emit statusMessage(message);
        return;
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
