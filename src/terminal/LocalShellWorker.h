#pragma once

#include <QByteArray>
#include <QMutex>
#include <QThread>

/**
 * @brief 在当前平台的原生伪终端中运行交互式本地 Shell。
 *
 * 普通 QProcess 使用管道而非终端，zsh 会关闭提示符、行编辑和颜色，因此不能用于验证 SSH
 * POSIX 使用 forkpty，Windows 10 1809+ 使用 ConPTY。上层始终只收发 UTF-8 与 VT 字节，
 * 因此 TerminalPane 不需要了解进程、管道和句柄差异，也不会因平台分支污染终端渲染逻辑。
 */
class LocalShellWorker final : public QThread {
    Q_OBJECT

public:
    explicit LocalShellWorker(QString shellPath = {},
                              QObject *parent = nullptr);
    ~LocalShellWorker() override;

    /** @brief 将键盘字节加入待写入 PTY 的线程安全队列。 */
    void enqueueInput(const QByteArray &bytes);

    /** @brief 请求改变伪终端字符行列数；后端会转换为 SIGWINCH 或 ConPTY resize。 */
    void updateTerminalSize(int columns, int rows);

    /** @brief 请求停止本地 Shell，并在宽限期后终止仍未退出的子进程树。 */
    void requestStop();

signals:
    /** @brief 本地 PTY 产生可供终端渲染的原始字节。 */
    void outputReady(const QByteArray &bytes);

    /** @brief 本地 Shell 已成功创建并可以接收输入。 */
    void shellStarted();

    /** @brief 本地 Shell 创建、读写或等待失败。 */
    void shellError(const QString &message);

    /** @brief 本地 Shell 已退出，exitCode 为 Shell 返回码或信号映射结果。 */
    void shellFinished(int exitCode);

protected:
    /** @brief 创建当前平台伪终端子进程并执行双向 IO 与退出清理循环。 */
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
