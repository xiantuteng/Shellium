#pragma once

#include "core/ConnectionTarget.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QList>
#include <QString>

#include <libssh/libssh.h>

#include <functional>
#include <utility>

/**
 * @brief 空闲连接保活的判定策略。
 *
 * libssh 没有 OpenSSH 的 ServerAliveInterval，保活报文必须由调用方按自己的节奏发出。
 * 这里把"该不该发"抽成无副作用的纯函数：工作线程每轮都要调用它，必须零成本；同时它是
 * 整个特性里唯一有分支的逻辑，独立出来才能在没有真实服务器的情况下被单元测试覆盖。
 */
namespace SshKeepAlive {

/**
 * @brief 默认保活间隔。
 *
 * 家用路由器与运营商 NAT 常见的 TCP 空闲表项寿命在 60 秒到数分钟之间，取 30 秒可以在
 * 最短的那一档被回收之前刷新一次。再短并不会更安全，只会在移动网络上白白唤醒无线电。
 */
inline constexpr qint64 kIdleIntervalMs = 30000;

/**
 * @brief 保活发送失败后的最小重试间隔。
 *
 * libssh 非阻塞发送缓冲满时会立刻失败；SSH 栈泵送周期约 10ms，若不退避就会每秒调用约
 * 100 次 ssh_send_ignore。1 秒仍足以在 NAT 过期前重试，同时避免故障链路上的忙循环。
 */
inline constexpr qint64 kMinimumRetryIntervalMs = 1000;

/**
 * @brief 判断某一层此刻是否应当发送保活探测。
 * @param sessionReady 该层是否已完成认证；建立阶段有独立的握手超时兜底，不需要保活。
 * @param idleMs 距最近一次数据往来的毫秒数；计时尚未开始时传负数。
 * @param intervalMs 保活间隔；<= 0 表示关闭保活。
 */
[[nodiscard]] constexpr bool shouldSend(const bool sessionReady,
                                        const qint64 idleMs,
                                        const qint64 intervalMs) noexcept
{
    if (!sessionReady || intervalMs <= 0 || idleMs < 0) {
        return false;
    }
    return idleMs >= intervalMs;
}

/** @brief 一层在判定时刻的状态快照。 */
struct LayerIdleState {
    bool ready = false;
    qint64 idleMs = -1;
    /** @brief 距上次实际调用发送 API 的毫秒数；从未尝试时为 -1。 */
    qint64 sinceLastAttemptMs = -1;
};

/**
 * @brief 逐层判定本轮需要发送保活的层，返回它们在层栈中的下标。
 * @param layers 从外层到内层的状态快照。
 * @param intervalMs 保活间隔。
 *
 * 必须逐层判定而不能只看栈顶：内层会话的传输架在外层的转发通道之上，外层被中间设备
 * 掐断会让内层一起完蛋。而用户长时间只在内层敲命令时，恰恰是外层最先超时——内层的
 * 数据虽然确实过境外层，但那是同一条 TCP 上的载荷，外层自身空闲与否要单独判断。
 */
[[nodiscard]] inline QList<int> layersNeedingKeepAlive(const QList<LayerIdleState> &layers,
                                                       const qint64 intervalMs)
{
    QList<int> indices;
    for (int index = 0; index < layers.size(); ++index) {
        const LayerIdleState &layer = layers.at(index);
        const bool retryWindowElapsed = layer.sinceLastAttemptMs < 0
            || layer.sinceLastAttemptMs >= kMinimumRetryIntervalMs;
        if (retryWindowElapsed && shouldSend(layer.ready, layer.idleMs, intervalMs)) {
            indices.append(index);
        }
    }
    return indices;
}

} // namespace SshKeepAlive

/**
 * @brief 连接链中的一层：一个 libssh 会话及其交互式 Shell 与 direct-tcpip 转发通道。
 *
 * 本类刻意不继承 QObject 也不持有线程：所有方法都必须由拥有它的 SshSessionStack 工作线程
 * 调用，因为同一个 ssh_session 上的 libssh 调用不是线程安全的。
 *
 * 建立过程被实现为**非阻塞状态机**，这是嵌套层能够工作的前提：第 N+1 层的传输架在第 N 层
 * 的转发通道之上，而该通道的数据搬运同样由工作线程完成。如果建立过程阻塞等待网络，第 N 层
 * 就得不到泵送，第 N+1 层的握手字节永远送不出去，必然死锁。因此调用方应当反复调用
 * advanceEstablish()，并在每轮之间泵送所有层的转发通道。
 */
class SshSessionNode final {
    Q_DECLARE_TR_FUNCTIONS(SshSessionNode)

public:
    /**
     * @brief 当前认证会话在连接链中的职责。
     *
     * Transit 只完成 SSH 认证并提供 direct-tcpip/exec 中继，不申请 PTY 或远端 Shell。
     * 这既减少跳板机资源占用，也兼容明确禁止交互登录、但允许端口转发的堡垒机账号。
     */
    enum class Role { Interactive, Transit };

    /**
     * @brief 一次状态推进的结构化结果。
     *
     * pending 为 true 表示本轮没有产生结论，调用方稍后应再次推进。hostKeyUnknown、
     * hostKeyChanged 与 legacyHmacRequired 都不是普通连接错误，而是必须由人确认后才能
     * 继续的安全分支。
     */
    struct StepResult {
        bool ok = false;
        bool pending = false;
        /** @brief 服务器没有任何本地信任记录，需要首次核验指纹。 */
        bool hostKeyUnknown = false;
        /** @brief 服务器密钥与已有记录冲突，需要按高风险变更流程核验指纹。 */
        bool hostKeyChanged = false;
        bool legacyHmacRequired = false;
        /**
         * @brief 服务器要求密码，但本层还没有可用凭据。
         *
         * 接管用户手打的 ssh 时必然出现：命令行里没有密码，而用户本来预期在终端里输入。
         * 上层应当在终端内联提示并回传，绝不能弹对话框——那和原生 ssh 的体验完全不同。
         */
        bool credentialRequired = false;
        /** @brief 该次索要是否源于上一次密码被服务器拒绝，用于区分首次提示与重试提示。 */
        bool credentialRetry = false;
        QString message;
        QString fingerprint;
    };

    /**
     * @brief 构造一层，但不发起任何网络操作。
     * @param target 该层的服务器配置；password 只在本对象生命周期内驻留内存。
     */
    explicit SshSessionNode(ConnectionTarget target, Role role = Role::Interactive);
    ~SshSessionNode();

    SshSessionNode(const SshSessionNode &) = delete;
    SshSessionNode &operator=(const SshSessionNode &) = delete;
    SshSessionNode(SshSessionNode &&) = delete;
    SshSessionNode &operator=(SshSessionNode &&) = delete;

    /**
     * @brief 创建会话、应用选项并切换为非阻塞模式；不进行任何网络往返。
     * @param transportFd 已连接的 socket；>= 0 时复用外层隧道，libssh 不再自行发起 TCP 连接。
     * @param columns 初始 PTY 列数。
     * @param rows 初始 PTY 行数。
     * @return 选项配置失败时返回失败结果，否则返回 pending 并等待 advanceEstablish 推进。
     *
     * 复用隧道时不会设置 SSH_OPTIONS_PROXYJUMP：外层路径已经由隧道本身表达，重复配置会让
     * libssh 试图从内层服务器再跳一次。
     */
    StepResult beginEstablish(int transportFd, int columns, int rows);

    /**
     * @brief 向前推进一步握手、认证或通道创建，绝不阻塞等待网络。
     * @return ok 且非 pending 表示 Shell 已就绪；pending 表示需要再次调用；
     *         hostKeyUnknown 或 hostKeyChanged 表示必须先调用 provideHostKeyDecision。
     */
    StepResult advanceEstablish();

    /**
     * @brief 提交用户对未知或已变更主机指纹的核验结论。
     * @param trusted true 表示允许写入或替换 known_hosts 并继续，false 表示中止本层。
     */
    void provideHostKeyDecision(bool trusted);

    /**
     * @brief 提交用户在终端里输入的密码，继续认证。
     * @param secret 密码或私钥口令；空字符串表示用户放弃，本层将失败。
     *
     * 密码同时写入本层目标，使随后的 SFTP 能复用同一份凭据而不必再次询问用户。
     */
    void provideCredential(const QString &secret);

    /** @brief Shell 是否已经就绪并可以收发终端数据。 */
    [[nodiscard]] bool isReady() const;

    /**
     * @brief 本层是否正在等待用户提供密码。
     *
     * 调用方必须据此判断再投递凭据：过早投递会被丢弃（用户抢先输入的密码就此丢失），
     * 而把上一层遗留的凭据投给新层则会让新层直接按"用户取消"失败。
     */
    [[nodiscard]] bool isAwaitingCredential() const;

    /**
     * @brief 在本层会话上非阻塞地执行一条命令，并收集受限长度的失败诊断。
     * @param command 交给远端 Shell 执行的完整命令。
     * @return 是否成功发起；已有执行在途或会话未建立时返回 false。
     *
     * 用于把 Shell 集成脚本写入远端临时文件：正文经此通道传输，绝不经过交互式终端。
     */
    bool beginDetachedExec(const QByteArray &command);

    /** @brief 推进在途的 exec；结束时把经退出码校验的结果交给 takeExecResult() 消费。 */
    void pumpExec();

    /**
     * @brief 取走最近一次后台 exec 的完成结果。
     * @param succeeded 输出远端命令是否明确以退出码 0 结束；可传 nullptr。
     * @param standardOutput 输出受长度限制的标准输出；可传 nullptr。
     * @param errorMessage 失败时输出通道错误、退出码和受限长度的 stderr 诊断；可传 nullptr。
     * @return 有尚未消费的完成结果返回 true；命令仍在执行或没有发起过返回 false。
     *
     * 仅看到通道 EOF 不能证明命令成功：例如临时目录不可写时 `cat` 同样会很快 EOF。
     * 因此调用方必须通过本接口消费经过退出状态校验后的结果，不能自行把 EOF 当成功。Shell
     * 集成安装不读取标准输出，但发行版探测需要读取它；复用这一条受限通道可避免两个后台
     * exec 同时竞争同一个 libssh 会话。
     */
    bool takeExecResult(bool *succeeded,
                        QByteArray *standardOutput = nullptr,
                        QString *errorMessage = nullptr);

    /**
     * @brief 当前层是否有正在执行的后台命令。
     * @return true 表示 exec 通道尚未得到最终退出结果。
     *
     * libssh 会话允许多个通道，但 Shellium 的后台任务刻意串行：Shell 集成安装与发行版
     * 探测若并发执行，会让任务归属和失败处理变得模糊，也会增加精简 SSH 服务端的兼容风险。
     */
    [[nodiscard]] bool hasDetachedExecInFlight() const { return m_execChannel != nullptr; }

    /**
     * @brief 写出内部排队的字节（目前只有 Shell 集成注入）。
     * @return 队列已清空返回 true；仍有剩余返回 false，调用方本轮不应再写用户输入。
     *
     * 必须先于用户输入写出：注入要抢在远端 Shell 接管终端之前送达，那时 tty 还处于规范
     * 模式，内核会逐字回显，本地过滤器才能精确扣掉。等 Shell 起来后再发就落进 readline，
     * 超宽行会被它按自己的方式重绘，回显不再与发出的字节一一对应。
     */
    bool flushOutgoing();

    /** @brief 追加一段由本程序发出、需优先写给远端的字节。 */
    void queueOutgoing(const QByteArray &bytes);

    /** @brief 通知远端 PTY 尺寸变化；Shell 尚未建立时只记录，待建立后应用。 */
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

    /**
     * @brief 记录一次数据往来，重置本层的空闲计时。
     *
     * Shell 的收发与转发通道的搬运都算活跃：只要这条 TCP 上刚刚有过密文，链路上的状态
     * 表项就是新鲜的，此时再补一个 SSH_MSG_IGNORE 毫无意义。
     */
    void noteActivity();

    /** @brief 距最近一次数据往来的毫秒数；尚未开始计时返回 -1。 */
    [[nodiscard]] qint64 idleMs() const;

    /** @brief 本层用于保活判定的状态快照。 */
    [[nodiscard]] SshKeepAlive::LayerIdleState keepAliveState() const;

    /**
     * @brief 立即发出一次 SSH_MSG_IGNORE 保活报文。
     * @return 成功送出返回 true；本轮写不出去返回 false。
     *
     * 失败不代表会话故障，调用方不应据此中断连接：非阻塞会话上发送队列可能暂时满。
     * 失败时不重置空闲计时，下一轮泵送会自然重试。
     */
    bool sendKeepAlive();

    /** @brief Shell 通道是否仍处于打开且未收到 EOF 的可交互状态。 */
    [[nodiscard]] bool isShellAlive() const;

    /** @brief 读取远端 Shell 退出码；服务器未提供时返回 -1。 */
    [[nodiscard]] int shellExitStatus() const;

    /** @brief 一次 direct-tcpip 建立请求的最终结果。 */
    struct ForwardOutcome {
        quint64 id = 0;
        bool ok = false;
        int callerFd = -1;
        QString error;
    };

    /** @brief 一次远程监听申请的异步完成结果。 */
    struct RemoteListenOutcome {
        quint64 requestId = 0;
        QString ruleId;
        bool ok = false;
        quint16 boundPort = 0;
        QString error;
    };

    /** @brief SSH 服务器向某条 -R 监听送来的一条新连接。 */
    struct RemoteAcceptedConnection {
        QString ruleId;
        int callerFd = -1; ///< 与 SSH channel 相连的 socket；调用方取得所有权。
    };

    /**
     * @brief 发起一条到内网目标的 direct-tcpip 通道，立即返回，绝不等待远端应答。
     * @param host 从本层服务器视角可达的主机。
     * @param port 目标端口。
     * @param nonBlockingCaller 调用方一端是否也设置为非阻塞；嵌套层的 libssh 需要，
     *        SFTP 的同步会话不需要。
     * @param timeoutMs 建立时限。远端向不可路由地址转发时会长时间无响应，没有时限就会
     *        让该请求永远悬挂。
     * @param error 立即失败（socketpair 或 channel 分配失败）时的原因输出。
     * @return 用于匹配后续结果的请求编号；立即失败返回 0。
     *
     * 结果由 pumpForwards() 在后续轮次中产出。这样拆分是必须的：同步等待远端应答会把
     * 工作线程钉住，既冻结本层终端，也饿死承载内层会话的外层通道。
     */
    quint64 beginForward(const QString &host,
                         quint16 port,
                         bool nonBlockingCaller,
                         int timeoutMs,
                         QString *error);

    /**
     * @brief 推进待建立的转发，并在已建立的通道与其本地 socket 之间搬运数据。
     * @return 本轮得出结论的建立结果；没有则为空。
     */
    QList<ForwardOutcome> pumpForwards();

    /**
     * @brief 非阻塞发起一条 SSH 服务器侧监听（OpenSSH -R 语义）。
     * @param ruleId 持久化转发规则 id，用于把后续入站连接路由回对应运行时。
     * @param bindHost 服务端监听地址；是否允许非回环地址由服务端 GatewayPorts 决定。
     * @param bindPort 服务端监听端口，必须为 1..65535。
     * @param error 同步参数错误输出；异步结果由 pumpRemoteForwarding 返回。
     * @return 非零请求 id 表示已进入状态机；0 表示未发起。
     */
    quint64 beginRemoteListen(const QString &ruleId,
                              const QString &bindHost,
                              quint16 bindPort,
                              QString *error = nullptr);

    /**
     * @brief 请求取消一条服务器侧监听。
     * @param ruleId 持久化规则 id。
     *
     * 取消同样按非阻塞状态机推进；调用后本地立即停止接收该规则的新连接，服务端确认包在
     * 后续 pump 中完成，避免 GUI 或 SSH 栈线程等待网络。
     */
    void cancelRemoteListen(const QString &ruleId);

    /**
     * @brief 推进远程监听申请/取消并接收服务端送来的入站 channel。
     * @param outcomes 本轮完成的监听申请。
     * @param accepted 本轮接收的入站连接；callerFd 所有权交给调用方。
     */
    void pumpRemoteForwarding(QList<RemoteListenOutcome> &outcomes,
                              QList<RemoteAcceptedConnection> &accepted);

    /** @brief 是否还有尚未得出结论的转发建立请求。 */
    [[nodiscard]] bool hasPendingForwards() const { return !m_pendingForwards.isEmpty(); }

    /** @brief 主动关闭 Shell 通道，用于上层请求退出该层。 */
    void closeShell();

    /** @brief 该层 libssh 会话的最近一次错误文本，用于补全 IO 失败的诊断信息。 */
    [[nodiscard]] QString lastError() const;

    /** @brief 该层的服务器配置。 */
    [[nodiscard]] const ConnectionTarget &target() const { return m_target; }

    /** @brief 底层 libssh 会话；供同线程内的 SFTP 子系统复用已认证连接。 */
    [[nodiscard]] ssh_session rawSession() const { return m_session; }

    /**
     * @brief 标记这一层的外层 Shell 里有一个 shim 正挂起等待放行。
     *
     * 只有用户在外层手打 ssh、由 Shellium 接管的层才为真。本层结束时必须据此向外层写入
     * ShellShim::resumeResponse()，否则外层终端会永远停在一次不再有意义的 read 上。
     */
    void setOuterShimSuspended(const bool suspended) { m_outerShimSuspended = suspended; }

    /** @brief 外层 Shell 是否有一个因本层而挂起、仍需放行的 shim。 */
    [[nodiscard]] bool outerShimSuspended() const { return m_outerShimSuspended; }

private:
    /** @brief 建立过程所处的阶段。 */
    enum class Stage {
        Idle,
        Connecting,
        AwaitingHostKeyDecision,
        Authenticating,
        AwaitingCredential,
        OpeningChannel,
        RequestingPty,
        RequestingShell,
        Ready,
        Failed
    };

    /** @brief 认证阶段内部依次尝试的方法。 */
    enum class AuthPhase { None, PublicKey, Password };

    /** @brief 一条本地 socket ↔ SSH direct-tcpip 通道的双向桥及其缓冲。 */
    struct ActiveForward {
        int socketFd = -1;
        ssh_channel channel = nullptr;
        QByteArray socketToChannel;
        QByteArray channelToSocket;
    };

    /**
     * @brief 建立字节通道的两种传输方式。
     *
     * DirectTcpIp 是首选；服务器设了 AllowTcpForwarding no 时它会被按策略拒绝，此时自动
     * 改用 ExecRelay——在远端执行一条中继命令，用它的标准输入输出当字节管道。后者只用到
     * session 通道，不受转发策略管辖。
     */
    enum class ForwardTransport { DirectTcpIp, ExecRelay };

    /**
     * @brief ExecRelay 的推进阶段。
     *
     * ConfirmingExec 会短暂收集 stderr 并观察通道是否立即退出。若远端没有 bash `/dev/tcp`、
     * nc 或 socat，旧实现会先把本地 socket 当作成功交给调用方，随后只留下一个无法解释的
     * EOF；增加确认阶段后可把真实诊断随 ForwardOutcome 返回。
     */
    enum class RelayStage { OpeningSession, RequestingExec, ConfirmingExec };

    /** @brief 一条已发起但远端尚未应答的通道建立请求。 */
    struct PendingForward {
        quint64 id = 0;
        ssh_channel channel = nullptr;
        int workerFd = -1;
        int callerFd = -1;
        QByteArray host;
        quint16 port = 22;
        int timeoutMs = 20000;
        QElapsedTimer timer;
        ForwardTransport transport = ForwardTransport::DirectTcpIp;
        RelayStage relayStage = RelayStage::OpeningSession;
        /** @brief exec 请求被服务端接受时相对 timer 起点的毫秒数，用于短暂失败观察窗。 */
        qint64 relayExecStartedAtMs = -1;
        /** @brief 确认阶段收集的远端 stderr，限制长度后用于向用户解释中继失败。 */
        QByteArray relayDiagnostic;
    };

    /** @brief 尚未收到服务端确认的 tcpip-forward 请求。 */
    struct PendingRemoteListen {
        quint64 requestId = 0;
        QString ruleId;
        QByteArray bindHost;
        quint16 bindPort = 0;
        QElapsedTimer timer;
    };

    /** @brief 已由服务端建立的远程监听。 */
    struct RemoteListener {
        QString ruleId;
        QByteArray bindHost;
        quint16 boundPort = 0;
        bool cancelRequested = false;
    };

    /** @brief 推进 TCP/SSH 握手并在成功后立即校验主机密钥。 */
    StepResult advanceConnecting();

    /** @brief 依次尝试 none、公钥/Agent、密码认证；每次调用最多推进一个方法一步。 */
    StepResult advanceAuthenticating();

    /**
     * @brief 统一处理认证成功后的角色分流。
     * @return 中转层直接返回成功；交互层进入 Shell 通道建立阶段并返回 pending。
     */
    StepResult authenticationSucceeded();

    /** @brief 对照 known_hosts 校验服务器公钥，决定进入认证还是等待用户核验。 */
    StepResult evaluateHostKey();

    /** @brief 关闭并释放一条转发通道及其本地 socket。 */
    static void releaseForward(ActiveForward &forward);

    /** @brief 推进所有待建立的转发，产出本轮得出结论的结果。 */
    QList<ForwardOutcome> advancePendingForwards();

    /** @brief 推进一条 direct-tcpip 请求；返回 true 表示本轮已得出结论。 */
    bool advanceDirectForward(PendingForward &pending, QList<ForwardOutcome> &outcomes);

    /** @brief 推进一条 exec 中继请求；返回 true 表示本轮已得出结论。 */
    bool advanceRelayForward(PendingForward &pending, QList<ForwardOutcome> &outcomes);

    /** @brief 把一条被策略拒绝的 direct-tcpip 请求就地改造成 exec 中继。 */
    bool convertToRelay(PendingForward &pending);

    /** @brief 释放该请求持有的通道与两端 socket。 */
    static void discardPending(PendingForward &pending);

    ConnectionTarget m_target;
    /** @brief 该层是最终交互会话还是仅承担受管链传输。 */
    Role m_role = Role::Interactive;

    /** @brief 外层 Shell 中是否有一个 shim 因本层而挂起，等待本层结束后放行。 */
    bool m_outerShimSuspended = false;

    ssh_session m_session = nullptr;
    ssh_channel m_shell = nullptr;
    QList<ActiveForward> m_forwards;
    QList<PendingForward> m_pendingForwards;
    QList<PendingRemoteListen> m_pendingRemoteListeners;
    QList<RemoteListener> m_remoteListeners;

    /** @brief 本层内单调递增的转发请求编号，用于把结果匹配回发起方。 */
    quint64 m_nextForwardId = 1;
    /** @brief 远程监听申请编号，与普通 direct-tcpip 编号分开，避免两个状态机互相耦合。 */
    quint64 m_nextRemoteListenId = 1;

    /**
     * @brief 本层服务器是否已确认禁用 TCP 转发。
     *
     * 一旦收到 SSH_OPEN_ADMINISTRATIVELY_PROHIBITED，后续请求直接走 ExecRelay，不必每次
     * 都先撞一次墙再重试。
     */
    bool m_forwardingProhibited = false;

    Stage m_stage = Stage::Idle;
    AuthPhase m_authPhase = AuthPhase::None;

    /**
     * @brief 认证密钥/密码的稳定副本。
     *
     * libssh 在非阻塞模式下要求 SSH_AUTH_AGAIN 之后用**完全相同的参数**重试，因此不能每轮
     * 重新从 QString 生成临时 QByteArray。
     */
    QByteArray m_secret;

    /** @brief PTY 模式字节串，出于同样的重试稳定性要求而缓存。 */
    QByteArray m_terminalModes;

    /** @brief 尚未写完的内部字节。 */
    QByteArray m_outgoing;

    /** @brief 后台 exec 的推进阶段。 */
    enum class ExecStage { Idle, Opening, Requesting, Draining, Finished };

    ExecStage m_execStage = ExecStage::Idle;
    ssh_channel m_execChannel = nullptr;
    QByteArray m_execCommand;
    QElapsedTimer m_execTimer;
    bool m_execHasResult = false;
    bool m_execSucceeded = false;
    /** @brief 最近一次后台 exec 的可读失败原因；成功时为空。 */
    QString m_execError;
    /** @brief 后台 exec 的 stderr 摘要，防止诊断输出无限占用内存。 */
    QByteArray m_execDiagnostic;
    /** @brief 后台 exec 的 stdout 摘要，供只读系统识别读取，同样严格限制长度。 */
    QByteArray m_execOutput;

    int m_columns = 80;
    int m_rows = 24;
    bool m_ptyApplied = false;

    bool m_hostKeyDecided = false;
    bool m_hostKeyTrusted = false;
    /** @brief 当前确认是否针对冲突旧记录；为 true 时确认后必须替换而非简单追加。 */
    bool m_hostKeyReplacementRequired = false;

    /** @brief 用户已提交凭据、等待下一轮推进消费。 */
    bool m_credentialProvided = false;

    /** @brief 是否已经有一份密码被服务器拒绝过；决定提示语用首次还是重试措辞。 */
    bool m_passwordRejected = false;

    /**
     * @brief 已经尝试过的密码次数。
     *
     * 与 OpenSSH 一致地允许重试：输错一次不该让整层连接失败，否则用户必须从头再敲一遍
     * ssh 命令。达到上限后才判定失败。
     */
    int m_passwordAttempts = 0;

    /** @brief 建立过程的整体超时计时；非阻塞模式下 libssh 自身不再提供握手超时保护。 */
    QElapsedTimer m_establishTimer;

    /** @brief 距最近一次数据往来的计时，保活判定的唯一依据；未就绪前不启动。 */
    QElapsedTimer m_idleTimer;

    /**
     * @brief 距最近一次保活发送尝试的计时。
     *
     * 成功时 m_idleTimer 会重置，失败时本计时器单独提供最小退避；二者不能共用，否则一次
     * 失败会被误当作真实网络活动并把下一次必要探测推迟完整 30 秒。
     */
    QElapsedTimer m_keepAliveAttemptTimer;
};
