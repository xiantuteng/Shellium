#pragma once

#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QThread>

#include <atomic>

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

    /**
     * @brief GUI 线程消费完一批 outputReady 之后的回执，用于解除读取背压。
     * @param bytes 本次消费掉的字节数，必须与收到的 QByteArray 长度一致。
     *
     * 由 GUI 线程调用，线程安全。漏调会让在途计数只增不减，读取端最终永久停摆，因此
     * 消费方必须无条件回执——即使这批字节因为会话代次不符而被丢弃。
     */
    void notifyOutputConsumed(qint64 bytes);

    /**
     * @brief 生成 POSIX 下启动本地登录 Shell 的 execv 参数，首项即程序路径。
     * @param shellPath 要启动的 Shell。
     * @param homeDirectory Shell 的起始目录。
     * @param insideFlatpak 是否运行在 Flatpak 沙箱内。
     *
     * Flatpak 沙箱里的 /bin、/usr 来自运行时，看不到用户机器上的 Shell 和它的配置，必须经
     * flatpak-spawn --host 在宿主上启动；PTY 从机作为标准输入输出一并转交。沙箱内的环境变量
     * 不会传到宿主，终端变量和起始目录都要用参数显式带过去。
     */
    [[nodiscard]] static QList<QByteArray> posixLaunchArguments(const QString &shellPath,
                                                                const QString &homeDirectory,
                                                                bool insideFlatpak);

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

    /** @brief 已发给 GUI 线程、但尚未收到消费回执的字节数是否已达上限。 */
    [[nodiscard]] bool outputBacklogFull() const;

    QString m_shellPath;

    /**
     * @brief 已 emit 但 GUI 线程尚未消费的字节数。
     *
     * 这是本地终端的流控核心，详见 run() 中的注释。跨线程读写，必须用原子量。
     */
    std::atomic<qint64> m_outputInFlight {0};
    mutable QMutex m_mutex;
    QByteArray m_pendingInput;
    bool m_stopRequested = false;
    int m_columns = 80;
    int m_rows = 24;
    bool m_sizeDirty = true;
};
