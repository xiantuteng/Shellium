#pragma once

#include <QByteArray>
#include <QMutex>
#include <QThread>

/**
 * @brief 在本地伪终端（PTY）中运行登录式 zsh。
 *
 * 普通 QProcess 使用管道而非终端，zsh 会关闭提示符、行编辑和颜色，因此不能用于验证 SSH
 * 终端行为。本类通过 forkpty 创建与远端 SSH PTY 等价的本地会话，既提供“本地终端”功能，
 * 也为终端输入、尺寸协商、分屏和命令历史提供无需服务器的自动化测试环境。
 */
class LocalShellWorker final : public QThread {
    Q_OBJECT

public:
    explicit LocalShellWorker(QString shellPath = QStringLiteral("/bin/zsh"),
                              QObject *parent = nullptr);
    ~LocalShellWorker() override;

    /** @brief 将键盘字节加入待写入 PTY 的线程安全队列。 */
    void enqueueInput(const QByteArray &bytes);

    /** @brief 请求改变 PTY 字符行列数，zsh 会收到 SIGWINCH。 */
    void updateTerminalSize(int columns, int rows);

    /** @brief 请求停止本地 Shell，并在必要时向子进程发送 SIGHUP。 */
    void requestStop();

signals:
    /** @brief 本地 PTY 产生可供终端渲染的原始字节。 */
    void outputReady(const QByteArray &bytes);

    /** @brief zsh 已成功创建并可以接收输入。 */
    void shellStarted();

    /** @brief zsh 创建、读写或等待失败。 */
    void shellError(const QString &message);

    /** @brief zsh 已退出，exitCode 为 Shell 返回码或信号映射结果。 */
    void shellFinished(int exitCode);

protected:
    /** @brief 创建 PTY 子进程并执行非阻塞双向 IO 循环。 */
    void run() override;

private:
    /** @brief 在线程锁保护下读取停止状态。 */
    [[nodiscard]] bool shouldStop() const;

    QString m_shellPath;
    mutable QMutex m_mutex;
    QByteArray m_pendingInput;
    bool m_stopRequested = false;
    int m_columns = 80;
    int m_rows = 24;
    bool m_sizeDirty = true;
};
