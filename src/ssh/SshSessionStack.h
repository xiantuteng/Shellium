#pragma once

#include "core/ConnectionTarget.h"

#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QSharedPointer>
#include <QThread>
#include <QWaitCondition>

#include <memory>
#include <vector>

class SshSessionNode;

/**
 * @brief 在专用线程中维护一个分屏的 SSH 连接层栈。
 *
 * 栈底是用户直接连接的服务器；后续每一层都由本程序经外层会话的 direct-tcpip 通道亲自
 * 建立，因此连接链的层数与身份是确定事实，而不是对终端输出的推测。终端 PTY 始终绑定
 * 栈顶层，SFTP 可以复用任意一层已认证的会话。
 *
 * libssh 的连接、认证和通道读取都可能阻塞，且同一会话上的调用不是线程安全的，因此所有
 * libssh 操作都收敛在本类的工作线程内：GUI 通过受互斥锁保护的队列提交输入，通过 Qt 队列
 * 信号接收输出。
 */
class SshSessionStack final : public QThread {
    Q_OBJECT

public:
    /**
     * @brief 构造栈并记录栈底目标；连接在 start() 后于工作线程内发起。
     * @param rootTarget 用户直接连接的服务器。
     */
    explicit SshSessionStack(const ConnectionTarget &rootTarget, QObject *parent = nullptr);
    ~SshSessionStack() override;

    /**
     * @brief 把终端输入加入待发送队列，由工作线程写入栈顶层的 Shell 通道。
     * @param bytes 键盘输入或控制序列；方法可从 GUI 线程安全调用。
     */
    void enqueueInput(const QByteArray &bytes);

    /**
     * @brief 更新远端 PTY 行列数，在工作线程下一轮循环中发送窗口变化。
     * @param columns 终端字符列数，最小为 1。
     * @param rows 终端字符行数，最小为 1。
     */
    void updateTerminalSize(int columns, int rows);

    /** @brief 请求停止网络循环并逐层关闭会话。 */
    void requestStop();

    /**
     * @brief 回复未知主机指纹确认请求。
     * @param trusted true 表示已人工核验并允许写入 known_hosts；false 表示拒绝连接。
     */
    void confirmUnknownHost(bool trusted);

    /**
     * @brief 通过栈顶层已认证会话打开到内网目标的 direct-tcpip 通道。
     * @param host 从栈顶服务器视角可访问的内层主机。
     * @param port 内层 SSH 端口。
     * @param errorMessage 失败原因输出。
     * @return 与转发通道相连的本进程 socket fd；失败返回 -1。调用方取得所有权。
     *
     * 此方法由 SFTP 后台线程调用，通过请求队列让所有 libssh API 仍只在本工作线程执行。
     */
    int openForwardSocket(const QString &host, quint16 port, QString *errorMessage);

signals:
    /** @brief 栈顶层的远端 PTY 产生了新的原始输出。 */
    void outputReady(const QByteArray &bytes);

    /** @brief SSH、认证或通道初始化失败。 */
    void connectionError(const QString &message);

    /**
     * @brief 服务器仅支持 hmac-sha1，需要 GUI 告知风险并决定是否兼容重试。
     * @param originalError libssh 返回的完整协商错误，供用户和日志诊断。
     */
    void legacyHmacRequired(const QString &originalError);

    /** @brief 栈底 Shell 通道已完成认证并可以交互。 */
    void shellConnected();

    /** @brief 整个层栈结束，可能由远端退出或用户关闭面板触发。 */
    void shellDisconnected(int exitStatus);

    /** @brief 首次遇到主机时要求 GUI 展示并确认 SHA-256 指纹。 */
    void hostKeyVerificationRequired(const ConnectionTarget &target,
                                     const QString &fingerprint);

protected:
    /** @brief 建立栈底层并执行统一的输入、输出与转发泵送循环。 */
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

    /**
     * @brief 完成一层的连接、指纹确认、认证与 PTY 创建。
     * @param node 待建立的层。
     * @param transportFd 复用的外层隧道 socket；-1 表示由本层自行发起 TCP 连接。
     * @return 成功返回 true；失败时已发出对应的错误或降级信号。
     */
    bool establishLayer(SshSessionNode &node, int transportFd);

    /**
     * @brief 主循环：搬运终端输入输出、服务转发请求并泵送所有层的隧道。
     * @return 因远端 Shell 正常结束而退出返回 true；因写入失败等异常中止返回 false。
     */
    bool pumpLoop();

    /** @brief 处理 GUI/SFTP 线程排队的 direct-tcpip 请求。 */
    void serviceForwardRequests();

    /** @brief 栈顶层，即终端 PTY 当前绑定的那一层。 */
    [[nodiscard]] SshSessionNode &activeLayer();

    /** @brief 判断停止标志；内部统一加锁以避免数据竞争。 */
    [[nodiscard]] bool shouldStop() const;

    ConnectionTarget m_rootTarget;

    /**
     * @brief 从外层到内层的会话层栈，仅由工作线程访问。
     *
     * 使用 unique_ptr 是因为 SshSessionNode 持有裸 libssh 句柄且不可移动；栈的增删只发生在
     * 工作线程内，因此不需要额外加锁。
     */
    std::vector<std::unique_ptr<SshSessionNode>> m_layers;

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
