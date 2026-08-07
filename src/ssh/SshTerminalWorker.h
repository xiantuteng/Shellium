#pragma once

#include "core/ConnectionTarget.h"

#include <QByteArray>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <QSharedPointer>

/**
 * @brief 在专用线程中维护一个 libssh 交互式 Shell 通道。
 *
 * libssh 的连接、认证和通道读取可能阻塞，不能在 GUI 线程执行。本类通过受互斥锁保护的
 * 输入队列接收按键，通过 Qt 队列信号返回输出，确保终端网络抖动不会冻结主窗口。
 */
class SshTerminalWorker final : public QThread {
    Q_OBJECT

public:
    explicit SshTerminalWorker(const ConnectionTarget &target, QObject *parent = nullptr);
    ~SshTerminalWorker() override;

    /**
     * @brief 把终端输入加入待发送队列。
     * @param bytes 键盘输入或控制序列；方法可从 GUI 线程安全调用。
     */
    void enqueueInput(const QByteArray &bytes);

    /**
     * @brief 更新远端 PTY 行列数，在工作线程下一轮循环中发送窗口变化。
     * @param columns 终端字符列数，最小为 1。
     * @param rows 终端字符行数，最小为 1。
     */
    void updateTerminalSize(int columns, int rows);

    /** @brief 请求停止网络循环并关闭远端通道。 */
    void requestStop();

    /**
     * @brief 回复未知主机指纹确认请求。
     * @param trusted true 表示已人工核验并允许写入 known_hosts；false 表示拒绝连接。
     */
    void confirmUnknownHost(bool trusted);

    /**
     * @brief 通过当前已认证 SSH 会话打开到内网目标的 direct-tcpip 通道。
     * @param host 从当前服务器视角可访问的内层主机。
     * @param port 内层 SSH 端口。
     * @param errorMessage 失败原因输出。
     * @return 与转发通道相连的本进程 socket fd；失败返回 -1。调用方取得所有权。
     *
     * 此方法由 SFTP 后台线程调用，通过请求队列让所有 libssh API 仍只在终端工作线程执行。
     */
    int openForwardSocket(const QString &host, quint16 port, QString *errorMessage);

signals:
    /** @brief 远端 PTY 产生了新的原始输出。 */
    void outputReady(const QByteArray &bytes);

    /** @brief SSH、认证或通道初始化失败。 */
    void connectionError(const QString &message);

    /**
     * @brief 服务器仅支持 hmac-sha1，需要 GUI 告知风险并决定是否兼容重试。
     * @param originalError libssh 返回的完整协商错误，供用户和日志诊断。
     */
    void legacyHmacRequired(const QString &originalError);

    /** @brief SSH Shell 通道已完成认证并可以交互。 */
    void shellConnected();

    /** @brief Shell 通道结束，可能由远端退出或用户关闭面板触发。 */
    void shellDisconnected(int exitStatus);

    /** @brief 首次遇到主机时要求 GUI 展示并确认 SHA-256 指纹。 */
    void hostKeyVerificationRequired(const ConnectionTarget &target,
                                     const QString &fingerprint);

protected:
    /** @brief 执行完整的连接、验证、认证、PTY 创建和双向 IO 循环。 */
    void run() override;

private:
    /** @brief 一个跨线程等待 direct-tcpip 通道创建结果的请求。 */
    struct ForwardRequest {
        QString host;
        quint16 port = 22;
        QMutex mutex;
        QWaitCondition condition;
        int clientFd = -1;
        QString error;
        bool completed = false;
        bool cancelled = false;
    };

    /** @brief 判断停止标志；内部统一加锁以避免数据竞争。 */
    [[nodiscard]] bool shouldStop() const;

    ConnectionTarget m_target;
    mutable QMutex m_mutex;
    QWaitCondition m_hostKeyCondition;
    QByteArray m_pendingInput;
    bool m_stopRequested = false;
    bool m_waitingForHostKey = false;
    bool m_hostKeyTrusted = false;
    int m_columns = 80;
    int m_rows = 24;
    bool m_sizeDirty = true;
    QList<QSharedPointer<ForwardRequest>> m_forwardRequests;
};
