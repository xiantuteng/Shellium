#pragma once

#include "core/ConnectionTarget.h"
#include "sftp/SftpService.h"

#include <QElapsedTimer>
#include <QHash>
#include <QWidget>

#include <optional>

class QComboBox;
class QCheckBox;
class QFileSystemModel;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QResizeEvent;
class QToolButton;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;

/**
 * @brief 与当前标签页终端联动的远程文件管理页面。
 *
 * 顶部服务器选择器展示当前面板识别出的完整 SSH 连接链。用户可以关闭“跟随最内层”并
 * 自由选择外层或内层服务器；路径框支持直接输入绝对路径并跳转。
 */
class RemoteFileManagerWidget final : public QWidget {
    Q_OBJECT

public:
    explicit RemoteFileManagerWidget(QWidget *parent = nullptr);
    ~RemoteFileManagerWidget() override;

    /**
     * @brief 更新当前终端面板可管理的服务器链。
     * @param targets 从外层到内层排列的服务器列表。
     * @param followInnermost 是否默认切换到最新识别出的最内层服务器。
     */
    void setAvailableTargets(const QList<ConnectionTarget> &targets,
                             bool followInnermost = true);

    /**
     * @brief 打开文件选择器，把文件加入当前远程目录的上传队列。
     *
     * 该公开入口供同标签页的 TerminalView 右键菜单调用，不依赖 MainWindow；因此上传目标、
     * 凭据、队列和暂停状态仍严格隔离在当前 TerminalWorkspace 内。
     */
    void requestUploadToCurrentDirectory();

    /**
     * @brief 终端自持层上报了工作目录变化，按需跟随。
     * @param stableKey 上报该 cwd 的服务器稳定键。
     * @param workingDirectory 远端 Shell 当前的绝对路径。
     *
     * 只有在"跟随终端目录"开关开启、且上报服务器与当前选中服务器一致时才会跳转，
     * 判定逻辑见 SftpUiState::shouldFollowTerminalWorkingDirectory。
     */
    void followTerminalWorkingDirectory(const QString &stableKey, const QString &workingDirectory);

    /**
     * @brief 退出全屏并恢复终端显示。
     *
     * 供 TerminalWorkspace 在整体隐藏文件管理时调用：若不同步复位，终端会因为仍处于
     * 全屏让位状态而保持隐藏，标签页里两块内容同时消失。
     */
    void exitFullScreen();

signals:
    /** @brief 文件操作或连接状态发生变化，供主窗口顶部非阻塞提示区显示。 */
    void statusMessage(const QString &message);

    /**
     * @brief 用户切换了全屏。
     * @param fullScreen true 表示文件管理应覆盖整个标签页，终端暂时让出空间。
     */
    void fullScreenChanged(bool fullScreen);

    /**
     * @brief 用户请求关闭远程文件管理面板。
     *
     * 控件不自行隐藏：显隐由 TerminalWorkspace 统一管理，它还要同步退出全屏并回写终端
     * 顶部的文件管理按钮状态。自行 hide() 会让那两处状态与实际显示脱节。
     */
    void closeRequested();

protected:
    /**
     * @brief 根据可用宽度在完整文字动作和紧凑图标动作之间切换。
     * @param event Qt 尺寸变化事件，仅用于读取新宽度；函数不会持有该指针。
     *
     * 全屏双栏按设计图展示“上传、下载、预览”等完整文字；默认与终端并排时宽度通常只有
     * 420px，继续强塞文字会挤掉传输控制，因此窄态保留相同按钮但只显示图标与 Tooltip。
     */
    void resizeEvent(QResizeEvent *event) override;

private slots:
    /** @brief 使用路径输入框内容读取远端目录。 */
    void refreshDirectory();

    /** @brief 双击目录时进入该目录；双击文件时下载。 */
    void activateItem(QTreeWidgetItem *item, int column);

    /** @brief 打开本地文件选择器并上传到当前目录。 */
    void uploadSelectedFile();

    /** @brief 打开本地目录选择器并递归上传整个目录到当前目录。 */
    void uploadSelectedDirectory();

    /** @brief 下载当前选中的远程条目；目录走递归下载。 */
    void downloadSelectedFile();

    /**
     * @brief 请求取消正在进行的递归传输。
     *
     * 大目录传输可能持续很久，用户必须有一条比"杀进程"更温和的退出路径。
     */
    void cancelActiveTransfer();

    /** @brief 暂停或恢复当前正在执行的队列项。 */
    void toggleActiveTransferPause();

    /** @brief 安全预览当前选中的远程文本或图片。 */
    void previewSelectedFile();

    /** @brief 编辑当前选中远程条目的 POSIX 八进制权限。 */
    void chmodSelectedPath();

    /** @brief 修改当前选中远程条目的名称。 */
    void renameSelectedPath();

    /** @brief 经二次确认后删除当前选中的文件或空目录。 */
    void deleteSelectedPath();

    /** @brief 返回当前路径的父目录，根目录保持不变。 */
    void navigateUp();

private:
    /** @brief 队列项的传输方向与源类型。 */
    enum class QueuedTransferKind {
        UploadFile,
        UploadDirectory,
        DownloadFile,
        DownloadDirectory
    };

    /**
     * @brief 当前标签页中的一个待执行传输。
     *
     * 入队时复制完整目标与续传策略，避免用户随后切换服务器或切换复选框导致已经排队的任务
     * 悄悄改目的地或覆盖语义。
     */
    struct QueuedTransfer {
        /** @brief 界面队列内单调递增的编号，用于取消尚未提交给服务的项目。 */
        quint64 queueId = 0;

        /** @brief 上传/下载以及文件/目录类型。 */
        QueuedTransferKind kind = QueuedTransferKind::UploadFile;

        /** @brief 入队时选中的完整服务器和跳板路由。 */
        ConnectionTarget target;

        /** @brief 上传时为本地路径，下载时为远程路径。 */
        QString sourcePath;

        /** @brief 上传时为远程目标，下载时为本地目标或父目录。 */
        QString destinationPath;

        /** @brief 入队时确定的续传或覆盖策略。 */
        SftpTransfer::ExistingFilePolicy policy =
            SftpTransfer::ExistingFilePolicy::ResumeWhenPossible;

        /** @brief 目录传输在入队前由用户就地选定的冲突处理方式。 */
        SftpTransfer::DirectoryConflictPolicy conflictPolicy =
            SftpTransfer::DirectoryConflictPolicy::Merge;

        /** @brief 可直接显示在队列中的简短说明，不包含凭据。 */
        QString description;
    };

    /**
     * @brief 一个已选好、但还在等待远端冲突探测结果的目录上传。
     *
     * 探测是异步的，期间用户可能切换服务器或改路径，因此这里完整复制目标与路径，不在回调
     * 里重新读界面状态。
     */
    struct PendingDirectoryUpload {
        ConnectionTarget target;
        QString localDirectory;
        QString remoteParentDirectory;
        QString remoteRoot;

        /** @brief 发起探测那一刻的续传策略；期间用户改动复选框不得影响这次传输。 */
        SftpTransfer::ExistingFilePolicy policy =
            SftpTransfer::ExistingFilePolicy::ResumeWhenPossible;
    };

    /**
     * @brief 探测远端是否已有同名目录，据结果决定直接入队还是先问用户。
     *
     * 冲突必须在**入队前**问清楚："合并更新"和"替换"会得到完全不同的远端结果，事后无法
     * 补救，也不该由程序替用户猜。
     */
    void requestDirectoryUpload(const ConnectionTarget &target,
                                const QString &localDirectory,
                                const QString &remoteParentDirectory);

    /** @brief 把一个已确定冲突策略的目录上传放进队列。 */
    void enqueueDirectoryUpload(const PendingDirectoryUpload &pending,
                                SftpTransfer::DirectoryConflictPolicy conflictPolicy);

    /** @brief 返回界面当前选择的续传/覆盖策略。 */
    [[nodiscard]] SftpTransfer::ExistingFilePolicy selectedTransferPolicy() const;

    /** @brief 将一个传输追加到当前标签队列，并在空闲时启动。 */
    void enqueueTransfer(QueuedTransfer transfer);

    /** @brief 串行启动下一项；已有活动项或队列为空时不做任何事。 */
    void startNextQueuedTransfer();

    /** @brief 完成活动项并刷新队列，随后异步启动下一项。 */
    void finishQueuedTransfer(quint64 requestId);

    /** @brief 根据活动项和等待列表重建队列视图。 */
    void rebuildTransferQueueView();

    /**
     * @brief 切换本地侧当前目录并同步路径框与树根节点。
     * @param directory 必须存在的本地绝对目录；无效目录会显示提示但不会改变原位置。
     */
    void setLocalDirectory(const QString &directory);

    /**
     * @brief 把本地拖拽条目上传到指定远程目录。
     * @param localPaths 本地绝对文件或目录路径。
     * @param remoteDirectory 远程目标目录。
     *
     * 目录复用递归上传且同一时间只允许一个；普通文件可批量提交。这样拖拽入口与按钮入口
     * 共享同一套路径校验、取消和进度状态，不会出现两种传输语义。
     */
    void uploadLocalPaths(const QStringList &localPaths, const QString &remoteDirectory);

    /**
     * @brief 把远程条目拖拽下载到指定本地目录。
     * @param entry 远程文件或目录的结构化信息。
     * @param localDirectory 已由本地树确定的目标目录。
     */
    void downloadRemoteEntryToDirectory(const RemoteFileInfo &entry,
                                        const QString &localDirectory);

    /** @brief 把路径框恢复成当前服务器自己的位置；未记录过则留空以进入主目录。 */
    void restorePathForSelectedTarget();

    /** @brief 返回服务器选择器当前对应的完整连接配置。 */
    [[nodiscard]] ConnectionTarget selectedTarget() const;

    /** @brief 返回当前文件列表选中的结构化条目；未选择时返回空值。 */
    [[nodiscard]] std::optional<RemoteFileInfo> selectedFile() const;

    /** @brief 返回远程树中所有选中条目，用于批量下载入队。 */
    [[nodiscard]] QList<RemoteFileInfo> selectedFiles() const;

    /** @brief 从指定树节点还原结构化远程条目；空节点返回空值。 */
    [[nodiscard]] std::optional<RemoteFileInfo> fileFromItem(QTreeWidgetItem *item) const;

    /** @brief 统一刷新传输信息带、进度条和状态文字。 */
    void setBusy(bool busy, const QString &message = {});

    /** @brief 按当前宽度刷新操作栏按钮的文字/图标展示方式。 */
    void updateResponsiveActionButtons();

    /**
     * @brief 按可用空间重新计算远程文件树"名称"列宽度，并钳在最小值以下不再收缩。
     *
     * 面板足够宽时名称列吃掉剩余空间；不够宽时钳在 kRemoteNameColumnMinWidth，多出的
     * 内容交给横向滚动条，而不是继续压缩文件名直到看不清。
     */
    void updateRemoteNameColumnWidth();

    /** @brief SFTP 操作成功后刷新目录并显示结果。 */
    void handleOperationSuccess(quint64 requestId, const QString &description);

    /** @brief 显示结构化错误；首次未知主机由单独指纹确认流程处理。 */
    void handleOperationFailure(quint64 requestId,
                                const ConnectionTarget &target,
                                SftpService::ErrorKind kind,
                                const QString &message);

    /** @brief 递归传输结束后展示汇总；有跳过或失败时用对话框而不是一行状态文字。 */
    void handleRecursiveTransferFinished(quint64 requestId,
                                         const SftpRecursive::TransferSummary &summary);

    /** @brief 当前标签页连接链的服务器层选择器；始终显示。 */
    QComboBox *m_targetCombo = nullptr;

    /** @brief 服务器选择器所在整行。 */
    QWidget *m_targetRow = nullptr;

    /**
     * @brief 全屏开关：让文件管理覆盖整个终端会话窗口。
     *
     * 全屏是"本地/远程双栏对拷"的唯一入口——并排模式下宽度不足以容纳两棵目录树，
     * 因此本地面板的显隐直接绑定在该开关上，不再单独提供本地文件按钮。
     */
    QToolButton *m_fullScreenButton = nullptr;

    /** @brief 关闭开关：收起整个远程文件管理面板，把宽度还给终端。 */
    QToolButton *m_closeButton = nullptr;

    /**
     * @brief 当前标签页独立的自动跟随偏好；手选外层后保持关闭直至连接链重建。
     *
     * 该控件不显示在界面上，仅作为内部状态载体：默认行为固定为跟随最内层。
     */
    QCheckBox *m_followInnermostCheck = nullptr;

    /**
     * @brief 用户可见的"跟随终端目录"开关；默认关闭。
     *
     * 开启后，收到当前选中服务器发来的 cwd 上报就自动跳转路径框，判定逻辑见
     * SftpUiState::shouldFollowTerminalWorkingDirectory。
     */
    QCheckBox *m_followTerminalCwdCheck = nullptr;

    /** @brief 本地文件面板容器；仅在全屏时显示，默认隐藏。 */
    QWidget *m_localPanel = nullptr;

    /** @brief 本地侧当前绝对目录输入框。 */
    QLineEdit *m_localPathEdit = nullptr;

    /** @brief 只读、按需加载的本地文件系统模型。 */
    QFileSystemModel *m_localModel = nullptr;

    /** @brief 本地文件浏览树，同时作为远程条目的下载拖放目标。 */
    QTreeView *m_localTree = nullptr;

    /** @brief 远程侧当前绝对目录输入框。 */
    QLineEdit *m_pathEdit = nullptr;

    /** @brief 远程目录条目树，同时作为本地文件的上传拖放目标。 */
    QTreeWidget *m_fileTree = nullptr;

    /**
     * @brief 当前排序列；-1 表示"还原默认"，即保持服务器返回的原始顺序。
     *
     * 点击列头在 正序 → 倒序 → 默认 之间循环，因此必须显式区分"未排序"与"按第 0 列
     * 正序"两种状态，不能只用 Qt 的两态排序指示器表达。
     */
    int m_sortColumn = -1;

    /** @brief 当前排序方向，仅在 m_sortColumn >= 0 时有意义。 */
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;

    /** @brief 按当前全屏状态刷新按钮图标（进入/退出用两个不同图形）。 */
    void updateFullScreenIcon();

    /** @brief 按当前 m_sortColumn/m_sortOrder 重排远程条目树。 */
    void applyRemoteSort();

    /** @brief 处理列头点击，按 正序→倒序→默认 三态推进。 */
    void cycleRemoteSort(int column);

    /** @brief 计时起点与上一次采样，用于估算传输速度。 */
    QElapsedTimer m_transferClock;
    qint64 m_lastSpeedSampleMs = 0;
    quint64 m_lastSpeedSampleBytes = 0;

    /** @brief 平滑后的速度（字节/秒）；瞬时值抖动过大，不适合直接展示。 */
    double m_smoothedBytesPerSecond = 0.0;

    /** @brief 复位速度估算状态，每次新传输开始时调用。 */
    void resetTransferSpeed();

    /**
     * @brief 依据字节进度刷新进度条文字。
     * @param completed 已传输字节数。
     * @param total 总字节数；为 0 表示总量未知。
     */
    void updateTransferProgressText(quint64 completed, quint64 total);
    /** @brief 传输信息带左侧状态，显示当前条目或一般 SFTP 操作进度。 */
    QLabel *m_statusLabel = nullptr;

    /** @brief 传输信息带右侧摘要，显示已传字节、速度和预计剩余时间。 */
    QLabel *m_transferDetailLabel = nullptr;

    /** @brief 合并进度、状态和队列的底部信息带；空闲且无队列时整体隐藏。 */
    QFrame *m_transferPanel = nullptr;

    QProgressBar *m_progressBar = nullptr;
    QToolButton *m_cancelButton = nullptr;
    /** @brief 暂停/恢复当前活动传输的按钮。 */
    QToolButton *m_pauseButton = nullptr;
    /** @brief 操作行里"传输控制"分组前的竖线，随暂停/取消按钮一起显隐，避免空转时
     *  单独留一条没有对应按钮的分隔线。 */
    QFrame *m_transferControlSeparator = nullptr;

    /** @brief 用户选择“优先续传”或“覆盖重传”的当前标签偏好。 */
    QCheckBox *m_resumeTransfersCheck = nullptr;

    /** @brief 展示活动项和所有待处理项，并允许选择待处理项取消。 */
    QListWidget *m_transferQueueList = nullptr;
    SftpService *m_service = nullptr;
    QList<ConnectionTarget> m_targets;
    quint64 m_latestListRequest = 0;
    bool m_hostKeyDialogOpen = false;

    /**
     * @brief 正在进行的传输请求编号，0 表示没有。
     *
     * 所有文件和目录都经当前标签队列串行提交，确保进度、暂停和取消始终对应唯一请求。
     */
    quint64 m_activeTransferRequest = 0;

    /** @brief 当前已经提交给 SftpService 的活动队列项。 */
    std::optional<QueuedTransfer> m_activeQueuedTransfer;

    /** @brief 尚未提交给 SftpService、可立即取消的待处理队列。 */
    QList<QueuedTransfer> m_pendingTransfers;

    /** @brief 为待处理队列项生成进程内唯一编号。 */
    quint64 m_nextTransferQueueId = 1;

    /** @brief 等待远端冲突探测结果的目录上传，按 statPath 请求编号索引。 */
    QHash<quint64, PendingDirectoryUpload> m_pendingDirectoryChecks;

    /**
     * @brief 上次选择的目录冲突处理方式，作为下一次对话框的默认项。
     *
     * 只在本控件生命周期内记忆，不持久化：跨会话沿用一个"替换"选择，风险远大于省下的一次
     * 点击。
     */
    SftpTransfer::DirectoryConflictPolicy m_lastDirectoryConflictChoice =
        SftpTransfer::DirectoryConflictPolicy::Merge;

    /** @brief 当前活动请求是否已由用户暂停，用于同步按钮文字。 */
    bool m_activeTransferPaused = false;

    /**
     * @brief 是否已有凭据对话框在显示。
     *
     * 同一目标的多个请求可能接连失败，逐个弹框会让密码窗口层层叠加。
     */
    bool m_credentialDialogOpen = false;
    QHash<QString, int> m_directoryDisconnectRetries;

    /**
     * @brief 每台服务器各自的当前目录，键为 ConnectionTarget::stableKey()。
     *
     * 路径是服务器专属的：切层时沿用上一台的路径几乎必然落空（第 1 层的 /app/software
     * 在第 2 层通常不存在），用户会看到一个莫名其妙的"目录不存在"。尚未记录的服务器
     * 传空路径，由服务端解析出该账号的主目录。
     */
    QHash<QString, QString> m_pathByTarget;

    /**
     * @brief 当前应用会话内的 SFTP 凭据缓存，键包含完整跳板链。
     *
     * 缓存不会写入 SQLite、日志或配置文件；服务器列表因焦点切换而重建时，从这里恢复内层
     * 密码，避免每次刷新目录都重新询问。进程退出后由 Qt 容器析构释放。
     */
    QHash<QString, QString> m_sessionSecrets;
};
