#include "sftp/RemoteFileManagerWidget.h"
#include "ui/UiIcons.h"

#include <QAction>
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
#include <QMenu>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QPixmap>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <optional>
#include <utility>

namespace {

/** @brief 把字节数格式化为适合文件列表展示的 IEC 单位。 */
/**
 * @brief 远程条目在服务器返回列表中的原始序号，用于"还原默认排序"。
 *
 * Qt::UserRole..UserRole+4 已被路径、是否目录、大小、权限和是否符号链接占用，这里从
 * UserRole+5 起继续，避免与既有取值冲突。
 */
constexpr int kOriginalIndexRole = Qt::UserRole + 5;

/** @brief 条目修改时间的原值，按时间排序时不能用格式化后的文本比较。 */
constexpr int kModifiedAtRole = Qt::UserRole + 6;

/**
 * @brief 三路比较，返回负/零/正，供排序比较器统一表达"小于/等于/大于"。
 *
 * 直接写 a < b 的比较器在需要"相等则回落到次级条件"时会退化成两次比较，用三路结果
 * 表达可以让次级条件只写一次。
 */
template <typename T>
int compareValues(const T &left, const T &right)
{
    if (left < right) {
        return -1;
    }
    return right < left ? 1 : 0;
}

/** @brief 把秒数格式化成人类可读的剩余时间。 */
QString formatDuration(const double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return QObject::tr("未知");
    }
    const qint64 total = qint64(seconds + 0.5);
    if (total < 60) {
        return QObject::tr("%1 秒").arg(total);
    }
    if (total < 3600) {
        return QObject::tr("%1 分 %2 秒").arg(total / 60).arg(total % 60);
    }
    return QObject::tr("%1 小时 %2 分").arg(total / 3600).arg((total % 3600) / 60);
}

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
 * @brief 把文件管理高频动作配置为带完整辅助说明的紧凑图标按钮。
 * @param button 待配置按钮；不得为空。
 * @param icon 语义图标，已包含跨平台回退资源。
 * @param accessibleName 完整中文动作名称，同时用于 Tooltip 和无障碍树。
 *
 * 文件管理器经常与终端并排，文字工具条会比文件内容占用更多宽度。图标按钮仍通过
 * Tooltip、AccessibleName 与右键菜单提供完整文字，兼顾窄布局、键鼠发现性和辅助技术。
 */
void configureCompactFileButton(QToolButton *button,
                                const QIcon &icon,
                                const QString &accessibleName)
{
    Q_ASSERT(button != nullptr);
    button->setText(accessibleName);
    button->setIcon(icon);
    button->setIconSize(QSize(17, 17));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(false);
    button->setFixedSize(34, 34);
    button->setProperty("compactIconButton", true);
    button->setToolTip(accessibleName);
    button->setAccessibleName(accessibleName);
}

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
    , m_fullScreenButton(new QToolButton(this))
    , m_localPathEdit(new QLineEdit(QDir::homePath(), this))
    , m_localModel(new QFileSystemModel(this))
    , m_localTree(new LocalDropTree(this))
    , m_pathEdit(new QLineEdit(QStringLiteral("/"), this))
    , m_fileTree(new RemoteDropTree(this))
    , m_statusLabel(new QLabel(tr("请选择终端服务器"), this))
    , m_progressBar(new QProgressBar(this))
    , m_cancelButton(new QToolButton(this))
    , m_pauseButton(new QToolButton(this))
    , m_resumeTransfersCheck(new QCheckBox(tr("优先断点续传"), this))
    , m_transferQueueList(new QListWidget(this))
    , m_service(new SftpService(this))
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(10, 10, 10, 8);
    mainLayout->setSpacing(8);

    m_targetRow = new QWidget(this);
    auto *targetRowLayout = new QHBoxLayout(m_targetRow);
    targetRowLayout->setContentsMargins(0, 0, 0, 0);
    targetRowLayout->setSpacing(6);
    targetRowLayout->addWidget(new QLabel(tr("服务器："), m_targetRow));
    targetRowLayout->addWidget(m_targetCombo, 1);

    // 自动跟随最内层是唯一合理的默认行为，不再作为可见选项占用顶部空间；它退化为内部
    // 状态：用户一旦在下拉框里手动选择外层，这里会自动置否，避免后续刷新把人抢回内层。
    m_followInnermostCheck->setChecked(true);
    m_followInnermostCheck->setVisible(false);

    m_fullScreenButton->setCheckable(true);
    m_fullScreenButton->setChecked(false);
    m_fullScreenButton->setAutoRaise(true);
    m_fullScreenButton->setIconSize(QSize(18, 18));
    m_fullScreenButton->setToolTip(tr("全屏显示文件管理，并展开本地文件用于双栏对拷"));
    m_fullScreenButton->setAccessibleName(tr("全屏"));

    auto *serverRow = new QHBoxLayout;
    serverRow->addWidget(m_targetRow, 1);
    serverRow->addWidget(m_fullScreenButton);
    mainLayout->addLayout(serverRow);

    auto *contentSplitter = new QSplitter(Qt::Horizontal, this);
    m_localPanel = new QWidget(contentSplitter);
    auto *localPanel = m_localPanel;
    auto *localLayout = new QVBoxLayout(localPanel);
    localLayout->setContentsMargins(0, 0, 0, 0);
    localLayout->setSpacing(4);
    auto *localCaption = new QLabel(tr("本地文件"), localPanel);
    localCaption->setProperty("sectionCaption", true);
    localLayout->addWidget(localCaption);
    auto *localPathRow = new QHBoxLayout;
    localPathRow->setSpacing(5);
    auto *localUpButton = new QToolButton(localPanel);
    auto *localRefreshButton = new QToolButton(localPanel);
    configureCompactFileButton(
        localUpButton,
        UiIcons::symbol(this, UiIcons::Symbol::ArrowUp, this->palette().color(QPalette::WindowText)),
        tr("返回本地父目录"));
    configureCompactFileButton(
        localRefreshButton,
        UiIcons::symbol(this, UiIcons::Symbol::Refresh, this->palette().color(QPalette::WindowText)),
        tr("刷新本地目录"));
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
    m_localTree->setAlternatingRowColors(false);
    m_localTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_localTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_localTree->setColumnWidth(0, 230);
    // 本地树的大小、类型、时间列在窄双栏中会挤掉最重要的文件名；完整信息仍可通过系统
    // 文件属性查看，本区域优先承担“选中上传目标”的任务。
    for (int column = 1; column < m_localModel->columnCount(); ++column) {
        m_localTree->setColumnHidden(column, true);
    }
    localLayout->addWidget(m_localTree, 1);

    auto *remotePanel = new QWidget(contentSplitter);
    auto *remoteLayout = new QVBoxLayout(remotePanel);
    remoteLayout->setContentsMargins(0, 0, 0, 0);
    remoteLayout->setSpacing(4);
    auto *remoteCaption = new QLabel(tr("远程文件"), remotePanel);
    remoteCaption->setProperty("sectionCaption", true);
    remoteLayout->addWidget(remoteCaption);
    auto *pathRow = new QHBoxLayout;
    pathRow->setSpacing(5);
    auto *upButton = new QToolButton(this);
    auto *refreshButton = new QToolButton(this);
    configureCompactFileButton(
        upButton,
        UiIcons::symbol(this, UiIcons::Symbol::ArrowUp, this->palette().color(QPalette::WindowText)),
        tr("返回远程父目录"));
    configureCompactFileButton(
        refreshButton,
        UiIcons::symbol(this, UiIcons::Symbol::Refresh, this->palette().color(QPalette::WindowText)),
        tr("刷新当前远程目录"));
    pathRow->addWidget(upButton);
    pathRow->addWidget(m_pathEdit, 1);
    pathRow->addWidget(refreshButton);
    remoteLayout->addLayout(pathRow);

    // 目录/链接/普通文件已由名称列的图标区分，独立的"类型"列只是重复信息，删掉它可以
    // 把宽度让给真正需要横向空间的时间与所有者。
    m_fileTree->setColumnCount(5);
    m_fileTree->setHeaderLabels(
        {tr("名称"), tr("大小"), tr("修改时间"), tr("权限"), tr("所有者")});
    m_fileTree->setRootIsDecorated(false);
    m_fileTree->setAlternatingRowColors(false);
    m_fileTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_fileTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_fileTree->header()->setStretchLastSection(false);
    // 六列全部固定显示且可手动拖拽调整宽度。Stretch 会把列压缩进可视宽度、
    // ResizeToContents 则由内容独断宽度，两者都会剥夺用户调整列宽的能力；Interactive
    // 配合初始宽度既保留拖拽，又让总宽超出面板时自然出现横向滚动条。
    for (int column = 0; column < m_fileTree->columnCount(); ++column) {
        m_fileTree->header()->setSectionResizeMode(column, QHeaderView::Interactive);
    }
    m_fileTree->header()->setMinimumSectionSize(56);
    const QList<int> defaultColumnWidths = {240, 96, 168, 92, 116};
    for (int column = 0; column < defaultColumnWidths.size(); ++column) {
        m_fileTree->setColumnWidth(column, defaultColumnWidths.at(column));
    }
    // 自行处理排序而不是使用 setSortingEnabled：Qt 内置排序只有升/降两态，无法表达
    // 用户要求的"再点一次回到服务器原始顺序"。
    m_fileTree->setSortingEnabled(false);
    m_fileTree->header()->setSectionsClickable(true);
    m_fileTree->header()->setSortIndicatorShown(false);
    connect(m_fileTree->header(), &QHeaderView::sectionClicked,
            this, &RemoteFileManagerWidget::cycleRemoteSort);
    // 宽度不足时显示横向滚动条，而不是隐藏列；按像素滚动可停在任意列中间。
    m_fileTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_fileTree->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    remoteLayout->addWidget(m_fileTree, 1);

    contentSplitter->addWidget(localPanel);
    contentSplitter->addWidget(remotePanel);
    contentSplitter->setChildrenCollapsible(false);
    contentSplitter->setStretchFactor(0, 1);
    contentSplitter->setStretchFactor(1, 1);
    contentSplitter->setSizes({420, 520});
    mainLayout->addWidget(contentSplitter, 1);

    // 本地面板只在全屏时出现：并排模式下文件管理只占约三分之一宽度，塞进两棵目录树
    // 会让两边都窄到无法辨认路径。全屏与"双栏对拷"因此是同一个动作。
    m_localPanel->setVisible(false);
    connect(m_fullScreenButton, &QToolButton::toggled, this, [this](const bool fullScreen) {
        m_localPanel->setVisible(fullScreen);
        m_fullScreenButton->setToolTip(
            fullScreen ? tr("退出全屏并收起本地文件") : tr("全屏显示文件管理，并展开本地文件用于双栏对拷"));
        updateFullScreenIcon();
        emit fullScreenChanged(fullScreen);
    });

    auto *operationRow = new QHBoxLayout;
    operationRow->setSpacing(5);
    auto *uploadButton = new QToolButton(this);
    auto *uploadDirectoryButton = new QToolButton(this);
    auto *downloadButton = new QToolButton(this);
    auto *previewButton = new QToolButton(this);
    auto *chmodButton = new QToolButton(this);
    auto *renameButton = new QToolButton(this);
    auto *deleteButton = new QToolButton(this);
    configureCompactFileButton(
        uploadButton,
        UiIcons::symbol(this, UiIcons::Symbol::Send, this->palette().color(QPalette::WindowText)),
        tr("上传文件到当前远程目录"));
    configureCompactFileButton(
        uploadDirectoryButton,
        UiIcons::symbol(this, UiIcons::Symbol::Upload, this->palette().color(QPalette::WindowText)),
        tr("递归上传目录到当前远程目录"));
    configureCompactFileButton(
        downloadButton,
        UiIcons::symbol(this, UiIcons::Symbol::Download, this->palette().color(QPalette::WindowText)),
        tr("下载选中的远程文件或目录"));
    configureCompactFileButton(
        previewButton,
        UiIcons::symbol(this, UiIcons::Symbol::Eye, this->palette().color(QPalette::WindowText)),
        tr("安全预览选中的文本或图片"));
    configureCompactFileButton(
        chmodButton,
        UiIcons::symbol(this, UiIcons::Symbol::Info, this->palette().color(QPalette::WindowText)),
        tr("修改选中远程条目的 POSIX 权限"));
    configureCompactFileButton(
        renameButton,
        UiIcons::symbol(this, UiIcons::Symbol::Edit, this->palette().color(QPalette::WindowText)),
        tr("重命名选中的远程条目"));
    configureCompactFileButton(
        deleteButton,
        UiIcons::symbol(this, UiIcons::Symbol::Trash, this->palette().color(QPalette::WindowText)),
        tr("删除选中的远程文件或空目录"));
    configureCompactFileButton(
        m_pauseButton,
        UiIcons::symbol(this, UiIcons::Symbol::Pause, this->palette().color(QPalette::WindowText)),
        tr("暂停或恢复当前传输"));
    configureCompactFileButton(
        m_cancelButton,
        UiIcons::symbol(this, UiIcons::Symbol::Cancel, this->palette().color(QPalette::WindowText)),
        tr("取消当前传输或选中的等待任务"));
    m_cancelButton->setEnabled(false);
    m_pauseButton->setEnabled(false);
    m_cancelButton->setVisible(false);
    m_pauseButton->setVisible(false);
    m_resumeTransfersCheck->setChecked(true);
    m_resumeTransfersCheck->setText(tr("断点续传"));
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
    m_progressBar->setTextVisible(false);
    mainLayout->addWidget(m_progressBar);
    m_transferQueueList->setMaximumHeight(110);
    m_transferQueueList->setAlternatingRowColors(false);
    m_transferQueueList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_transferQueueList->setToolTip(
        tr("传输严格按顺序执行；选择等待项后点击“取消选中”可直接移除。"));
    mainLayout->addWidget(m_transferQueueList);
    mainLayout->addWidget(m_statusLabel);

    connect(refreshButton, &QToolButton::clicked, this, &RemoteFileManagerWidget::refreshDirectory);
    connect(upButton, &QToolButton::clicked, this, &RemoteFileManagerWidget::navigateUp);
    connect(m_pathEdit, &QLineEdit::returnPressed, this, &RemoteFileManagerWidget::refreshDirectory);
    connect(localUpButton, &QToolButton::clicked, this, [this]() {
        setLocalDirectory(QFileInfo(m_localPathEdit->text().trimmed()).absolutePath());
    });
    connect(localRefreshButton, &QToolButton::clicked, this, [this]() {
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
    connect(m_localTree,
            &QWidget::customContextMenuRequested,
            this,
            [this](const QPoint &position) {
                const QModelIndex index = m_localTree->indexAt(position);
                if (index.isValid()) {
                    m_localTree->setCurrentIndex(index);
                }
                QMenu menu(this);
                UiIcons::modernizePopup(&menu);
                if (index.isValid()) {
                    const QString localPath = m_localModel->filePath(index);
                    if (m_localModel->isDir(index)) {
                        QAction *openAction = menu.addAction(
                            UiIcons::symbol(this, UiIcons::Symbol::Folder, this->palette().color(QPalette::WindowText)),
                            tr("打开目录"));
                        connect(openAction, &QAction::triggered,
                                this, [this, localPath]() { setLocalDirectory(localPath); });
                    }
                    QAction *uploadAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Send, this->palette().color(QPalette::WindowText)),
                        tr("上传到当前远程目录"));
                    connect(uploadAction, &QAction::triggered, this, [this, localPath]() {
                        uploadLocalPaths({localPath}, m_pathEdit->text().trimmed());
                    });
                    menu.addSeparator();
                }
                QAction *refreshAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Refresh, this->palette().color(QPalette::WindowText)),
                    tr("刷新本地目录"));
                connect(refreshAction, &QAction::triggered, this, [this]() {
                    setLocalDirectory(m_localPathEdit->text().trimmed());
                });
                menu.exec(m_localTree->viewport()->mapToGlobal(position));
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
    connect(m_fileTree,
            &QWidget::customContextMenuRequested,
            this,
            [this](const QPoint &position) {
                QTreeWidgetItem *item = m_fileTree->itemAt(position);
                if (item != nullptr && !item->isSelected()) {
                    // 右键未选中行时切换到该行；右键已有多选中的任意一行则保留批量选择，
                    // 让“下载”继续按用户明确选择的全部条目入队。
                    m_fileTree->clearSelection();
                    m_fileTree->setCurrentItem(item);
                    item->setSelected(true);
                }
                QMenu menu(this);
                UiIcons::modernizePopup(&menu);
                QAction *uploadFileAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Send, this->palette().color(QPalette::WindowText)),
                    tr("上传文件到当前目录"));
                QAction *uploadDirectoryAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Upload, this->palette().color(QPalette::WindowText)),
                    tr("上传目录到当前目录"));
                connect(uploadFileAction,
                        &QAction::triggered,
                        this,
                        &RemoteFileManagerWidget::uploadSelectedFile);
                connect(uploadDirectoryAction,
                        &QAction::triggered,
                        this,
                        &RemoteFileManagerWidget::uploadSelectedDirectory);
                if (item != nullptr) {
                    menu.addSeparator();
                    QAction *downloadAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Download, this->palette().color(QPalette::WindowText)),
                        tr("下载"));
                    QAction *previewAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Eye, this->palette().color(QPalette::WindowText)),
                        tr("预览"));
                    QAction *chmodAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Info, this->palette().color(QPalette::WindowText)),
                        tr("修改权限"));
                    QAction *renameAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Edit, this->palette().color(QPalette::WindowText)),
                        tr("重命名"));
                    QAction *deleteAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Trash, this->palette().color(QPalette::WindowText)),
                        tr("删除"));
                    connect(downloadAction, &QAction::triggered,
                            this, &RemoteFileManagerWidget::downloadSelectedFile);
                    connect(previewAction, &QAction::triggered,
                            this, &RemoteFileManagerWidget::previewSelectedFile);
                    connect(chmodAction, &QAction::triggered,
                            this, &RemoteFileManagerWidget::chmodSelectedPath);
                    connect(renameAction, &QAction::triggered,
                            this, &RemoteFileManagerWidget::renameSelectedPath);
                    connect(deleteAction, &QAction::triggered,
                            this, &RemoteFileManagerWidget::deleteSelectedPath);
                }
                menu.addSeparator();
                QAction *refreshAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Refresh, this->palette().color(QPalette::WindowText)),
                    tr("刷新远程目录"));
                connect(refreshAction, &QAction::triggered,
                        this, &RemoteFileManagerWidget::refreshDirectory);
                menu.exec(m_fileTree->viewport()->mapToGlobal(position));
            });
    connect(uploadButton, &QToolButton::clicked, this, &RemoteFileManagerWidget::uploadSelectedFile);
    connect(uploadDirectoryButton,
            &QToolButton::clicked,
            this,
            &RemoteFileManagerWidget::uploadSelectedDirectory);
    connect(m_cancelButton,
            &QToolButton::clicked,
            this,
            &RemoteFileManagerWidget::cancelActiveTransfer);
    connect(m_pauseButton,
            &QToolButton::clicked,
            this,
            &RemoteFileManagerWidget::toggleActiveTransferPause);
    connect(downloadButton, &QToolButton::clicked, this, &RemoteFileManagerWidget::downloadSelectedFile);
    connect(previewButton, &QToolButton::clicked, this, &RemoteFileManagerWidget::previewSelectedFile);
    connect(chmodButton, &QToolButton::clicked, this, &RemoteFileManagerWidget::chmodSelectedPath);
    connect(renameButton, &QToolButton::clicked, this, &RemoteFileManagerWidget::renameSelectedPath);
    connect(deleteButton, &QToolButton::clicked, this, &RemoteFileManagerWidget::deleteSelectedPath);
    connect(m_transferQueueList,
            &QWidget::customContextMenuRequested,
            this,
            [this](const QPoint &position) {
                QListWidgetItem *item = m_transferQueueList->itemAt(position);
                if (item != nullptr) {
                    m_transferQueueList->setCurrentItem(item);
                }
                if (item == nullptr && !m_activeQueuedTransfer.has_value()) {
                    return;
                }
                QMenu menu(this);
                UiIcons::modernizePopup(&menu);
                if (m_activeQueuedTransfer.has_value()) {
                    QAction *pauseAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Pause, this->palette().color(QPalette::WindowText)),
                        m_activeTransferPaused ? tr("继续当前传输") : tr("暂停当前传输"));
                    connect(pauseAction,
                            &QAction::triggered,
                            this,
                            &RemoteFileManagerWidget::toggleActiveTransferPause);
                }
                QAction *cancelAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Cancel, this->palette().color(QPalette::WindowText)),
                    tr("取消选中任务"));
                connect(cancelAction,
                        &QAction::triggered,
                        this,
                        &RemoteFileManagerWidget::cancelActiveTransfer);
                menu.exec(m_transferQueueList->viewport()->mapToGlobal(position));
            });

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
                int originalIndex = 0;
                for (const RemoteFileInfo &entry : entries) {
                    auto *item = new QTreeWidgetItem(m_fileTree);
                    item->setText(0, entry.name);
                    item->setIcon(
                        0,
                        entry.isDirectory
                            ? UiIcons::symbol(m_fileTree,
                                             UiIcons::Symbol::Folder,
                                             m_fileTree->palette().color(QPalette::WindowText))
                            : UiIcons::symbol(m_fileTree,
                                             UiIcons::Symbol::File,
                                             m_fileTree->palette().color(QPalette::WindowText)));
                    item->setText(1, entry.isDirectory ? QStringLiteral("—") : formatSize(entry.size));
                    item->setText(2, entry.modifiedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
                    item->setText(3, formatPermissions(entry.permissions));
                    item->setText(4, entry.owner);
                    item->setData(0, Qt::UserRole, entry.path);
                    item->setData(0, Qt::UserRole + 1, entry.isDirectory);
                    item->setData(0, Qt::UserRole + 2, QVariant::fromValue(entry.size));
                    item->setData(0, Qt::UserRole + 3, QVariant::fromValue(entry.permissions));
                    item->setData(0, Qt::UserRole + 4, entry.isSymbolicLink);
                    // 排序必须比较真实值而不是显示文本：大小按字节排否则 "9 B" 会大于
                    // "10 MiB"，时间按 QDateTime 排否则跨格式会乱。原始序号用于"还原默认"。
                    item->setData(0, kOriginalIndexRole, originalIndex++);
                    item->setData(0, kModifiedAtRole, entry.modifiedAt);
                }
                applyRemoteSort();
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
                // 百分比无法回答"还要多久""是不是卡住了"。补上已传/总量和实时速度后，
                // 用户才能判断一次大文件传输是正常慢还是已经停滞。
                m_progressBar->setTextVisible(true);
                updateTransferProgressText(completed, total);
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
    updateFullScreenIcon();
}

void RemoteFileManagerWidget::exitFullScreen()
{
    // setChecked 会触发 toggled，进而收起本地面板并发出 fullScreenChanged(false)，
    // 让工作区把终端恢复出来；因此这里不需要再重复做同样的事。
    m_fullScreenButton->setChecked(false);
}

void RemoteFileManagerWidget::updateFullScreenIcon()
{
    m_fullScreenButton->setIcon(
        m_fullScreenButton->isChecked()
            ? UiIcons::symbol(this, UiIcons::Symbol::FullscreenExit, this->palette().color(QPalette::WindowText))
            : UiIcons::symbol(this, UiIcons::Symbol::FullscreenEnter, this->palette().color(QPalette::WindowText)));
}

void RemoteFileManagerWidget::cycleRemoteSort(const int column)
{
    // 三态推进属于纯状态计算，放在 SftpUiState 中由单元测试覆盖；这里只负责把结果
    // 反映到表头指示器和实际排序上。
    const SftpUiState::SortState next =
        SftpUiState::nextSortState({m_sortColumn, m_sortOrder}, column);
    m_sortColumn = next.column;
    m_sortOrder = next.order;

    if (m_sortColumn < 0) {
        m_fileTree->header()->setSortIndicatorShown(false);
    } else {
        m_fileTree->header()->setSortIndicatorShown(true);
        m_fileTree->header()->setSortIndicator(m_sortColumn, m_sortOrder);
    }
    applyRemoteSort();
}

void RemoteFileManagerWidget::applyRemoteSort()
{
    if (m_fileTree->topLevelItemCount() == 0) {
        return;
    }

    QList<QTreeWidgetItem *> items;
    items.reserve(m_fileTree->topLevelItemCount());
    while (m_fileTree->topLevelItemCount() > 0) {
        items.append(m_fileTree->takeTopLevelItem(0));
    }

    const int column = m_sortColumn;
    const bool descending = m_sortOrder == Qt::DescendingOrder;
    std::stable_sort(items.begin(), items.end(),
                     [column, descending](const QTreeWidgetItem *left,
                                          const QTreeWidgetItem *right) {
        if (column < 0) {
            // 还原默认：完全按服务器返回顺序，不再做任何分组。
            return left->data(0, kOriginalIndexRole).toInt()
                < right->data(0, kOriginalIndexRole).toInt();
        }

        // 目录恒排在文件之前。文件管理的首要用途是逐层进入目录，若按大小排序把目录
        // 冲散到列表中间，导航会变得非常费力；这也是各类 SFTP 客户端的一致做法。
        const bool leftIsDirectory = left->data(0, Qt::UserRole + 1).toBool();
        const bool rightIsDirectory = right->data(0, Qt::UserRole + 1).toBool();
        if (leftIsDirectory != rightIsDirectory) {
            return leftIsDirectory;
        }

        int comparison = 0;
        switch (column) {
        case 1:
            comparison = compareValues(left->data(0, Qt::UserRole + 2).toULongLong(),
                                       right->data(0, Qt::UserRole + 2).toULongLong());
            break;
        case 2:
            comparison = compareValues(left->data(0, kModifiedAtRole).toDateTime(),
                                       right->data(0, kModifiedAtRole).toDateTime());
            break;
        case 3:
            comparison = compareValues(left->data(0, Qt::UserRole + 3).toUInt(),
                                       right->data(0, Qt::UserRole + 3).toUInt());
            break;
        case 4:
            comparison = QString::compare(left->text(4), right->text(4), Qt::CaseInsensitive);
            break;
        case 0:
        default:
            comparison = QString::compare(left->text(0), right->text(0), Qt::CaseInsensitive);
            break;
        }
        if (comparison == 0) {
            // 同值时回落到名称，保证结果稳定可预期，不随刷新前后跳动。
            comparison = QString::compare(left->text(0), right->text(0), Qt::CaseInsensitive);
        }
        return descending ? comparison > 0 : comparison < 0;
    });

    m_fileTree->addTopLevelItems(items);
}

void RemoteFileManagerWidget::resetTransferSpeed()
{
    m_transferClock.start();
    m_lastSpeedSampleMs = 0;
    m_lastSpeedSampleBytes = 0;
    m_smoothedBytesPerSecond = 0.0;
}

void RemoteFileManagerWidget::updateTransferProgressText(const quint64 completed,
                                                          const quint64 total)
{
    if (!m_transferClock.isValid()) {
        m_transferClock.start();
    }
    const qint64 nowMs = m_transferClock.elapsed();
    const qint64 deltaMs = nowMs - m_lastSpeedSampleMs;
    // 至少间隔 300ms 再采样：SFTP 每个数据块都会回调，间隔过短会让瞬时速度剧烈跳动，
    // 数字快到看不清。
    if (deltaMs >= 300 && completed >= m_lastSpeedSampleBytes) {
        const double instant =
            double(completed - m_lastSpeedSampleBytes) * 1000.0 / double(deltaMs);
        // 指数滑动平均进一步抑制网络抖动；首个样本直接采用，避免从 0 缓慢爬升。
        m_smoothedBytesPerSecond = m_smoothedBytesPerSecond > 0.0
            ? m_smoothedBytesPerSecond * 0.7 + instant * 0.3
            : instant;
        m_lastSpeedSampleMs = nowMs;
        m_lastSpeedSampleBytes = completed;
    }

    QString text = total > 0
        ? tr("%1 / %2").arg(formatSize(completed), formatSize(total))
        : tr("已传输 %1").arg(formatSize(completed));
    if (m_smoothedBytesPerSecond > 0.0) {
        text += tr(" · %1/s").arg(formatSize(quint64(m_smoothedBytesPerSecond)));
        if (total > completed) {
            const double remainingSeconds =
                double(total - completed) / m_smoothedBytesPerSecond;
            // 剩余时间只在能算得出时给出，避免总量未知时显示一个凭空的数字。
            text += tr(" · 剩余 %1").arg(formatDuration(remainingSeconds));
        }
    }
    if (total > 0) {
        text.prepend(QStringLiteral("%p%  "));
    }
    m_progressBar->setFormat(text);
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
    m_pauseButton->setToolTip(tr("暂停当前传输"));
    m_pauseButton->setAccessibleName(tr("暂停当前传输"));
    m_pauseButton->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Pause, this->palette().color(QPalette::WindowText)));
    setBusy(true, tr("正在执行：%1").arg(transfer.description));
    // 每个传输独立计速：沿用上一次的样本会让新任务开头显示出前一个任务的速度。
    resetTransferSpeed();

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
    m_pauseButton->setToolTip(tr("暂停当前传输"));
    m_pauseButton->setAccessibleName(tr("暂停当前传输"));
    m_pauseButton->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Pause, this->palette().color(QPalette::WindowText)));
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
                                                     const QString &state,
                                                     const QIcon &icon) {
        auto *item = new QListWidgetItem(state + transfer.description, m_transferQueueList);
        item->setIcon(icon);
        item->setData(Qt::UserRole, QVariant::fromValue(transfer.queueId));
        if (transfer.queueId == selectedQueueId) {
            m_transferQueueList->setCurrentItem(item);
        }
    };
    if (m_activeQueuedTransfer.has_value()) {
        appendItem(*m_activeQueuedTransfer,
                   m_activeTransferPaused ? tr("已暂停 · ") : tr("进行中 · "),
                   UiIcons::symbol(m_transferQueueList,
                                  m_activeTransferPaused ? UiIcons::Symbol::Pause
                                                          : UiIcons::Symbol::Play,
                                  m_transferQueueList->palette().color(QPalette::WindowText)));
    }
    for (const QueuedTransfer &transfer : std::as_const(m_pendingTransfers)) {
        appendItem(transfer,
                   tr("等待 · "),
                   UiIcons::symbol(m_transferQueueList, UiIcons::Symbol::Clock, m_transferQueueList->palette().color(QPalette::WindowText)));
    }

    const bool hasActive = m_activeQueuedTransfer.has_value();
    const bool hasAnyTransfer = hasActive || !m_pendingTransfers.isEmpty();
    m_pauseButton->setEnabled(hasActive);
    m_cancelButton->setEnabled(hasAnyTransfer);
    // 空闲状态隐藏传输控制，比保留两个禁用按钮更能把空间留给文件名；任务出现时再以稳定
    // 顺序显示，不会影响文件操作按钮的位置。
    m_pauseButton->setVisible(hasActive);
    m_cancelButton->setVisible(hasAnyTransfer);
    m_transferQueueList->setVisible(hasAnyTransfer);
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

    // 校验通过后仍走和页面操作按钮完全相同的选择与队列方法，不创建第二套隐式传输状态。
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
        m_pauseButton->setToolTip(tr("恢复当前传输"));
        m_pauseButton->setAccessibleName(tr("恢复当前传输"));
        m_pauseButton->setIcon(
            UiIcons::symbol(this, UiIcons::Symbol::Play, this->palette().color(QPalette::WindowText)));
        emit statusMessage(tr("传输将在下一个 64 KiB 分块边界暂停"));
    } else {
        m_service->resumeRequest(m_activeTransferRequest);
        m_pauseButton->setText(tr("暂停"));
        m_pauseButton->setToolTip(tr("暂停当前传输"));
        m_pauseButton->setAccessibleName(tr("暂停当前传输"));
        m_pauseButton->setIcon(
            UiIcons::symbol(this, UiIcons::Symbol::Pause, this->palette().color(QPalette::WindowText)));
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
        // 清掉上一次传输遗留的"已传/总量·速度"文字，否则下次进度条一出现就先闪出旧数据。
        m_progressBar->setFormat(QStringLiteral("%p%"));
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
