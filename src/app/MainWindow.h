#pragma once

#include <QMainWindow>

class QTabWidget;
class TerminalPane;
class TerminalWorkspace;

/**
 * @brief Shellium 主窗口，协调标签页、终端分屏和连接对话框。
 *
 * 每个 TerminalWorkspace 在标签页内部持有自己的远程文件管理器；主窗口不持有或转发
 * SSH/SFTP 会话状态，从而保证不同终端标签之间完全隔离。
 */
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    /** @brief 打开连接对话框，并在当前空面板中启动 SSH。 */
    void connectCurrentPane();

    /**
     * @brief 经当前分屏已连接的服务器，由本程序亲自建立下一层嵌套连接。
     *
     * 与用户在终端里手打 ssh 的区别在于：这样建立的层是确定事实，退出时机同样确定，
     * 文件管理器无需对终端输出做任何猜测。
     */
    void connectNestedFromCurrentPane();

    /** @brief 询问新目标后，在当前面板右侧或下方创建分屏。 */
    void splitCurrentPane(Qt::Orientation orientation);

    /** @brief 创建一个拥有独立分屏树的新标签页，并统一连接状态信号。 */
    TerminalWorkspace *createWorkspaceTab(const QString &title);

    /** @brief 返回当前标签页的终端工作区。 */
    [[nodiscard]] TerminalWorkspace *activeWorkspace() const;

    TerminalWorkspace *m_workspace = nullptr;
    QTabWidget *m_tabs = nullptr;
};
