#pragma once

#include "core/ConnectionTarget.h"
#include "sftp/SftpService.h"

#include <QHash>
#include <QWidget>

#include <optional>

class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
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

signals:
    /** @brief 文件操作或连接状态发生变化，供主窗口状态栏显示。 */
    void statusMessage(const QString &message);

private slots:
    /** @brief 使用路径输入框内容读取远端目录。 */
    void refreshDirectory();

    /** @brief 双击目录时进入该目录；双击文件时下载。 */
    void activateItem(QTreeWidgetItem *item, int column);

    /** @brief 打开本地文件选择器并上传到当前目录。 */
    void uploadSelectedFile();

    /** @brief 下载当前选中的远程文件。 */
    void downloadSelectedFile();

    /** @brief 修改当前选中远程条目的名称。 */
    void renameSelectedPath();

    /** @brief 经二次确认后删除当前选中的文件或空目录。 */
    void deleteSelectedPath();

    /** @brief 返回当前路径的父目录，根目录保持不变。 */
    void navigateUp();

private:
    /** @brief 把路径框恢复成当前服务器自己的位置；未记录过则留空以进入主目录。 */
    void restorePathForSelectedTarget();

    /** @brief 返回服务器选择器当前对应的完整连接配置。 */
    [[nodiscard]] ConnectionTarget selectedTarget() const;

    /** @brief 返回当前文件列表选中的结构化条目；未选择时返回空值。 */
    [[nodiscard]] std::optional<RemoteFileInfo> selectedFile() const;

    /** @brief 统一刷新按钮、进度条和状态文字。 */
    void setBusy(bool busy, const QString &message = {});

    /** @brief SFTP 操作成功后刷新目录并显示结果。 */
    void handleOperationSuccess(quint64 requestId, const QString &description);

    /** @brief 显示结构化错误；首次未知主机由单独指纹确认流程处理。 */
    void handleOperationFailure(quint64 requestId,
                                const ConnectionTarget &target,
                                SftpService::ErrorKind kind,
                                const QString &message);

    QComboBox *m_targetCombo = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QTreeWidget *m_fileTree = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    SftpService *m_service = nullptr;
    QList<ConnectionTarget> m_targets;
    quint64 m_latestListRequest = 0;
    bool m_hostKeyDialogOpen = false;

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
