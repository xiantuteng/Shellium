#pragma once

#include "core/ConnectionTarget.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QList>
#include <QString>

#include <libssh/libssh.h>

#include <functional>

/**
 * @brief 连接链中的一层：一个 libssh 会话及其交互式 Shell 与 direct-tcpip 转发通道。
 *
 * 本类刻意不继承 QObject 也不持有线程：所有方法都必须由拥有它的 SshSessionStack 工作线程
 * 调用，因为同一个 ssh_session 上的 libssh 调用不是线程安全的。连接过程被拆成若干独立步骤
 * 并返回结构化结果，使“未知主机指纹”“仅支持 hmac-sha1”这类需要用户决策的状态可以交回
 * GUI，而不必把 QMessageBox 塞进网络代码。
 *
 * 每层可以经由 SSH_OPTIONS_FD 复用外层会话提供的隧道 socket，从而在不暴露中间跳板凭据的
 * 前提下把嵌套服务器变成本程序亲自持有的会话。
 */
class SshSessionNode final {
    Q_DECLARE_TR_FUNCTIONS(SshSessionNode)

public:
    /**
     * @brief 一个连接步骤的结构化结果。
     *
     * 除 ok 之外的标志用于区分“失败”和“需要用户决策”：hostKeyUnknown 与 legacyHmacRequired
     * 都不是错误，而是必须由人确认后才能继续的分支。
     */
    struct StepResult {
        bool ok = false;
        bool hostKeyUnknown = false;
        bool legacyHmacRequired = false;
        QString message;
        QString fingerprint;
    };

    /**
     * @brief 构造一层，但不发起任何网络操作。
     * @param target 该层的服务器配置；password 只在本对象生命周期内驻留内存。
     */
    explicit SshSessionNode(ConnectionTarget target);
    ~SshSessionNode();

    SshSessionNode(const SshSessionNode &) = delete;
    SshSessionNode &operator=(const SshSessionNode &) = delete;
    SshSessionNode(SshSessionNode &&) = delete;
    SshSessionNode &operator=(SshSessionNode &&) = delete;

    /**
     * @brief 建立 SSH 传输层。
     * @param transportFd 已连接的 socket；>= 0 时复用外层隧道，libssh 不再自行发起 TCP 连接。
     * @param timeoutSeconds 直连模式下的握手超时。
     * @return legacyHmacRequired 为 true 时表示服务器仅提供 hmac-sha1，需要用户知情确认。
     *
     * 复用隧道时不会设置 SSH_OPTIONS_PROXYJUMP：外层路径已经由隧道本身表达，重复配置会让
     * libssh 试图从内层服务器再跳一次。
     */
    StepResult connectTransport(int transportFd, long timeoutSeconds);

    /**
     * @brief 对照 known_hosts 校验服务器公钥。
     * @return hostKeyUnknown 为 true 时 fingerprint 字段携带待用户核验的 SHA-256 指纹。
     */
    StepResult verifyHostKey();

    /** @brief 用户核验指纹后写入 known_hosts；仅应在 verifyHostKey 报告未知主机后调用。 */
    StepResult acceptHostKey();

    /** @brief 依次尝试 none、公钥/Agent、密码认证。 */
    StepResult authenticate();

    /** @brief 申请交互式 PTY 并启动远端登录 Shell。 */
    StepResult openShell(int columns, int rows);

    /** @brief 通知远端 PTY 尺寸变化；Shell 尚未建立时静默忽略。 */
    void resizePty(int columns, int rows);

    /**
     * @brief 向 Shell 通道写入一段输入。
     * @return libssh 语义：>0 为已写字节数，SSH_AGAIN 表示暂不可写，<0 为错误。
     */
    int writeToShell(const char *data, int size);

    /**
     * @brief 从 Shell 通道非阻塞读取。
     * @param stderrStream 为 true 时读取扩展数据流。
     * @return 读到的字节数；无数据时返回 0。
     */
    int readFromShell(char *buffer, int size, bool stderrStream);

    /** @brief Shell 通道是否仍处于打开且未收到 EOF 的可交互状态。 */
    [[nodiscard]] bool isShellAlive() const;

    /** @brief 读取远端 Shell 退出码；服务器未提供时返回 -1。 */
    [[nodiscard]] int shellExitStatus() const;

    /**
     * @brief 在本层会话上打开一条到内网目标的 direct-tcpip 通道。
     * @param host 从本层服务器视角可达的主机。
     * @param port 目标端口。
     * @param callerFd 成功时输出 socketpair 的调用方一端，调用方取得所有权。
     * @param shouldAbort 轮询 SSH_AGAIN 期间的中止判定，用于让关闭请求立即生效。
     * @param error 失败原因输出。
     * @return 是否成功建立通道。
     *
     * 工作线程一侧的 socket 被设置为非阻塞并交由 pumpForwards 泵送；调用方一端保持阻塞
     * 语义，可直接交给 libssh 的 SSH_OPTIONS_FD 或其他同步消费者。
     */
    bool openForward(const QString &host,
                     quint16 port,
                     int *callerFd,
                     const std::function<bool()> &shouldAbort,
                     QString *error);

    /** @brief 在本层所有转发通道与其本地 socket 之间搬运数据，并回收已结束的通道。 */
    void pumpForwards();

    /** @brief 主动关闭 Shell 通道，用于上层请求退出该层。 */
    void closeShell();

    /** @brief 该层 libssh 会话的最近一次错误文本，用于补全 IO 失败的诊断信息。 */
    [[nodiscard]] QString lastError() const;

    /** @brief 该层的服务器配置。 */
    [[nodiscard]] const ConnectionTarget &target() const { return m_target; }

    /** @brief 底层 libssh 会话；供同线程内的 SFTP 子系统复用已认证连接。 */
    [[nodiscard]] ssh_session rawSession() const { return m_session; }

private:
    /** @brief 一条本地 socket ↔ SSH direct-tcpip 通道的双向桥及其缓冲。 */
    struct ActiveForward {
        int socketFd = -1;
        ssh_channel channel = nullptr;
        QByteArray socketToChannel;
        QByteArray channelToSocket;
    };

    /** @brief 关闭并释放一条转发通道及其本地 socket。 */
    static void releaseForward(ActiveForward &forward);

    ConnectionTarget m_target;
    ssh_session m_session = nullptr;
    ssh_channel m_shell = nullptr;
    QList<ActiveForward> m_forwards;
};
