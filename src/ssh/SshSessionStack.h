#pragma once

#include "core/ConnectionTarget.h"
#include "ssh/SshSessionNode.h"

#include <QByteArray>
#include <QList>
#include <QHash>
#include <QMutex>
#include <QSharedPointer>
#include <QThread>
#include <QWaitCondition>

#include <memory>
#include <vector>

/**
 * @brief 在专用线程中维护一个终端的 SSH 连接层栈。
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
     * @param parent Qt 父对象；默认交互会话会安装 Shell 集成。
     */
    explicit SshSessionStack(const ConnectionTarget &rootTarget, QObject *parent = nullptr);

    /**
     * @brief 构造可控制 Shell 集成副作用的 SSH 栈。
     * @param rootTarget 用户直接连接的服务器。
     * @param installShellIntegration true 表示认证后安装 Shell 集成；连接测试应传 false，
     *        保证测试只验证连接能力而不向远端写入临时脚本。
     * @param parent Qt 父对象。
     */
    SshSessionStack(const ConnectionTarget &rootTarget,
                    bool installShellIntegration,
                    QObject *parent);
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
     * @brief 请求在当前栈顶之上建立新一层，终端 PTY 随后切换到该层。
     * @param target 内层服务器；其 host/port 由栈顶服务器的视角解析。
     *
     * 新层的传输经栈顶会话的 direct-tcpip 通道建立，因此不需要内层服务器从本机可达，
     * 也不会把中间跳板的凭据暴露给内层。结果通过 layersChanged 或 layerFailed 返回。
     *
     * @param outerShimSuspended 该层是否源于接管用户手打的 ssh。为真时外层 Shell 里有一个
     *        shim 正挂起等待，本栈会在该层结束或建立失败时向外层写入放行行；否则外层 Shell
     *        本就停在提示符上，不能凭空多发一个换行。
     */
    void requestPushLayer(const ConnectionTarget &target, bool outerShimSuspended = false);

    /**
     * @brief 请求关闭栈顶层并把终端交还外层；栈内只剩一层时忽略。
     *
     * 用户在内层执行 exit 时无需调用本方法：该层的 Shell 通道会自行 EOF，栈会据此
     * 确定性地弹出，这正是自持会话相对于解析终端输出的根本优势。
     */
    void requestPopLayer();

    /**
     * @brief 提交用户在终端里为正在建立的层输入的密码。
     * @param secret 密码或私钥口令；空字符串表示放弃该层。
     */
    void provideLayerCredential(const QString &secret);

    /**
     * @brief 通过栈顶层已认证会话打开到内网目标的 direct-tcpip 通道。
     * @param host 从栈顶服务器视角可访问的内层主机。
     * @param port 内层 SSH 端口。
     * @param errorMessage 失败原因输出。
     * @return 与转发通道相连的本进程 socket fd；失败返回 -1。调用方取得所有权。
     *
     * 此方法由 SFTP 后台线程调用，通过请求队列让所有 libssh API 仍只在本工作线程执行。
     */
    int openForwardSocket(const QString &layerId,
                          const QString &host,
                          quint16 port,
                          QString *errorMessage);

    /**
     * @brief 通过根 SSH 会话打开到根服务器视角目标的 direct-tcpip 通道。
     * @param host 根服务器能够解析和访问的目标地址。
     * @param port 目标端口。
     * @param errorMessage 失败原因输出。
     * @return 与 SSH 通道相连的本进程 socket fd；失败返回 -1，成功时调用方取得所有权。
     *
     * 本地与动态端口转发绑定的是持久化根主机，不能使用 openForwardSocket() 的“当前栈顶”
     * 语义。否则用户进入内层服务器后，同一条 -L/-D 规则会悄悄改从内层出网。
     */
    int openRootForwardSocket(const QString &host,
                              quint16 port,
                              QString *errorMessage);

    /**
     * @brief 在根 SSH 会话上申请服务端远程监听（OpenSSH -R 语义）。
     * @param ruleId 持久化端口转发规则 id。
     * @param bindHost SSH 服务器侧监听地址。
     * @param bindPort SSH 服务器侧监听端口。
     * @param errorMessage 失败原因输出。
     * @return 服务端确认监听成功返回 true。
     *
     * 调用方通常是 PortForwardRuntime 后台线程；本方法只等待受 QWaitCondition 保护的结果，
     * 所有 libssh API 仍由本栈工作线程执行。根层而非当前栈顶承载规则，保证规则始终与其
     * 持久化 hostId 对应，不会因用户进入临时嵌套层而悄悄改变服务端。
     */
    bool startRemoteForward(const QString &ruleId,
                            const QString &bindHost,
                            quint16 bindPort,
                            QString *errorMessage);

    /**
     * @brief 异步停止根会话上的一条远程监听。
     * @param ruleId 持久化规则 id。
     *
     * 停止不等待服务端网络往返；节点状态机会继续推进 cancel-tcpip-forward。调用方可立即
     * 关闭本地运行时，后续到达的入站 channel 会由节点直接释放。
     */
    void stopRemoteForward(const QString &ruleId);

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

    /**
     * @brief 层栈发生变化，携带从外层到内层的完整快照。
     *
     * 快照中的每一层都是本程序亲自建立并完成认证的会话，因此 confidence 均为 Confirmed；
     * 文件管理器可以直接信任这份列表，无需再对终端输出做任何推测。
     */
    void layersChanged(const QList<ConnectionTarget> &layers);

    /** @brief 嵌套层建立失败；栈已回退到失败前的状态，外层会话不受影响。 */
    void layerFailed(const ConnectionTarget &target, const QString &message);

    /**
     * @brief 某一层即将加载 Shell 集成脚本，携带其远端路径。
     *
     * 界面据此登记那条 source 命令的期望回显，把它从用户屏幕上扣掉。
     */
    void shellIntegrationPathReady(const QByteArray &path);

    /**
     * @brief 某一层的 Shell 集成脚本安装失败，该层已自动退回命令解析降级路径。
     * @param target 安装失败的服务器层。
     * @param message 远端退出码、stderr 或通道错误组成的可读原因。
     *
     * 安装失败不应断开 SSH 会话，但也不能静默忽略：用户需要知道自动识别已从确定性的
     * Shell 事件退化为推测式命令解析，文件操作层据此应保持更谨慎的预期。
     */
    void shellIntegrationFailed(const ConnectionTarget &target, const QString &message);

    /**
     * @brief 正在建立的层需要密码。
     * @param target 待认证的服务器。
     * @param retry 是否为上一次密码被拒后的重试。
     *
     * 上层必须在终端内联提示并通过 provideLayerCredential 回传。弹对话框会破坏"手打 ssh"
     * 的原生体验——用户预期的是终端里的 password 提示，不是一个窗口。
     */
    void layerCredentialRequired(const ConnectionTarget &target, bool retry);

    /**
     * @brief 远程监听收到一条新连接，交给对应规则运行时连接本机目标。
     * @param ruleId 持久化规则 id。
     * @param tunnelFd 与入站 SSH channel 相连的本地 socket，接收方取得所有权。
     */
    void remoteForwardConnection(const QString &ruleId, int tunnelFd);

protected:
    /** @brief 建立栈底层并执行统一的输入、输出与转发泵送循环。 */
    void run() override;

private:
    /** @brief GUI 线程对未知主机指纹的作答，由工作线程在下一轮推进时消费。 */
    enum class HostKeyDecision { None, Trusted, Rejected };

    /** @brief 一次建层请求：目标，外加"外层是否有 shim 正为它挂起"。 */
    struct PendingPush {
        ConnectionTarget target;
        bool outerShimSuspended = false;
    };

    /** @brief 一个跨线程等待 direct-tcpip 通道创建结果的请求。 */
    struct ForwardRequest {
        QString layerId;
        QString host;
        quint16 port = 22;
        bool rootOrigin = false;
        QMutex mutex;
        QWaitCondition condition;
        int clientFd = -1;
        QString error;
        bool completed = false;
        bool cancelled = false;
    };

    /** @brief 跨线程等待 tcpip-forward 全局请求结果。 */
    struct RemoteListenRequest {
        QString ruleId;
        QString bindHost;
        quint16 bindPort = 0;
        QMutex mutex;
        QWaitCondition condition;
        bool completed = false;
        bool succeeded = false;
        bool cancelled = false;
        QString error;
    };

    /**
     * @brief 主循环：推进建立中的层、搬运终端输入输出并泵送所有层的隧道。
     * @return 因远端 Shell 正常结束而退出返回 true；因连接失败或写入错误中止返回 false。
     */
    bool pumpLoop();

    /**
     * @brief 把栈顶尚未就绪的层向前推进一步，绝不阻塞。
     * @return 栈仍然可用返回 true；栈底建立失败、整个会话不可继续时返回 false。
     *
     * 嵌套层建立失败只弹出该层并发出 layerFailed，外层会话保持可用；只有栈底失败才是
     * 致命的。
     */
    bool advanceEstablishingLayers();

    /** @brief 处理 GUI 排队的建层与退层请求。 */
    void servicePendingLayerRequests();

    /**
     * @brief 关闭并弹出栈顶层，返回被弹出层的目标描述。
     *
     * 被弹出的层若源于接管用户手打的 ssh，会顺带向新的栈顶写入放行行，让外层挂起的 shim
     * 返回、Shell 重新画出提示符。
     */
    ConnectionTarget popTopLayer();

    /**
     * @brief 建层在压栈之前就失败时，放行外层那个已经挂起的 shim。
     * @param push 失败的建层请求。
     */
    void releaseSuspendedOuterShim(const PendingPush &push);

    /** @brief 当前层栈从外层到内层的目标快照。 */
    [[nodiscard]] QList<ConnectionTarget> layerTargets() const;

    /**
     * @brief 泵送所有层的转发通道并派发本轮得出结论的建立结果。
     *
     * 内层传输依赖外层通道，必须整栈一起搬运。结果统一在遍历结束后派发：派发过程可能
     * 压入新层，而 std::vector 的 push_back 会让正在使用的迭代器失效。
     */
    void pumpAllForwards();

    /**
     * @brief 逐层检查空闲时长，为需要的层各发一次保活探测。
     *
     * 长时间只看不敲的会话会被中间的 NAT 与状态防火墙静默回收表项，用户下一次按键才发现
     * 连接早已死掉且没有任何提示。SSH 协议层的 SSH_MSG_IGNORE 由本方法定期补上。
     */
    void serviceKeepAlive();

    /** @brief 把一条转发建立结果交给它的发起方——建层流程或 SFTP 请求。 */
    void handleForwardOutcome(const SshSessionNode::ForwardOutcome &outcome);

    /** @brief 把 GUI/SFTP 线程排队的 direct-tcpip 请求转成异步建立，不等待结果。 */
    void serviceForwardRequests();

    /** @brief 构造并同步等待一条跨线程 direct-tcpip 请求，供两种公开入口复用。 */
    int openForwardSocketImpl(bool rootOrigin,
                              const QString &layerId,
                              const QString &host,
                              quint16 port,
                              QString *errorMessage);

    /** @brief 把跨线程的远程监听启动/停止请求转交根节点的非阻塞状态机。 */
    void serviceRemoteForwardRequests();

    /**
     * @brief 找出应当由谁发起通往 layerId 的转发。
     * @return 目标层的下一层外层；layerId 为空时返回栈顶；无法解析时返回 nullptr。
     *
     * 要抵达第 N 层必须从第 N-1 层发起。用栈顶去连自己既绕远，在禁用转发的服务器上还会
     * 平白多失败一次。
     */
    [[nodiscard]] SshSessionNode *originLayerFor(const QString &layerId);

    /** @brief 该层 Shell 集成脚本的远端临时路径。 */
    [[nodiscard]] static QByteArray shimPathFor(const SshSessionNode &node);

    /** @brief 经独立 exec 通道把 Shell 集成脚本写入远端临时文件。 */
    void beginShellIntegrationInstall(SshSessionNode &node);

    /** @brief 丢弃尚未被消费的层凭据；新建层、层失败与退层时都必须调用。 */
    void discardPendingCredential();

    /** @brief 以给定原因终结所有在途的转发与建层请求，用于弹层和停止。 */
    void abortInFlightForwards(const QString &reason);

    /** @brief 判断停止标志；内部统一加锁以避免数据竞争。 */
    [[nodiscard]] bool shouldStop() const;

    ConnectionTarget m_rootTarget;
    /** @brief 是否在每层认证完成后安装 Shell 集成；连接测试会显式关闭以保持只读。 */
    bool m_installShellIntegration = true;

    /**
     * @brief 从外层到内层的会话层栈，仅由工作线程访问。
     *
     * 使用 unique_ptr 是因为 SshSessionNode 持有裸 libssh 句柄且不可移动；栈的增删只发生在
     * 工作线程内，因此不需要额外加锁。
     */
    std::vector<std::unique_ptr<SshSessionNode>> m_layers;

    /** @brief 栈底就绪信号是否已发出，保证 shellConnected 在一次运行中只发一次。 */
    bool m_rootAnnounced = false;


    mutable QMutex m_mutex;
    QByteArray m_pendingInput;
    bool m_stopRequested = false;
    HostKeyDecision m_hostKeyDecision = HostKeyDecision::None;

    /** @brief GUI 提交但工作线程尚未消费的层凭据。 */
    QString m_pendingCredential;
    bool m_hasPendingCredential = false;

    /** @brief GUI 排队的建层请求，按提交顺序在工作线程中逐个处理。 */
    QList<PendingPush> m_pendingPushes;

    /**
     * @brief 正在等待隧道就绪的建层请求；同一时刻至多一个。
     *
     * 新层必须先建成才能承载下一层，因此建层天然串行；forwardId 为 0 表示当前没有在途请求。
     */
    PendingPush m_layerPush;
    quint64 m_layerPushForwardId = 0;

    /** @brief 已发起但尚未得出结论的 SFTP 隧道请求，按转发编号索引。 */
    QHash<quint64, QSharedPointer<ForwardRequest>> m_forwardsInFlight;

    /** @brief GUI 排队的退层次数。 */
    int m_pendingPops = 0;

    int m_columns = 80;
    int m_rows = 24;
    bool m_sizeDirty = true;
    QList<QSharedPointer<ForwardRequest>> m_forwardRequests;

    /** @brief 尚未由工作线程发起的远程监听申请。 */
    QList<QSharedPointer<RemoteListenRequest>> m_remoteListenRequests;

    /** @brief 已进入节点状态机的远程监听申请，按节点请求编号索引。 */
    QHash<quint64, QSharedPointer<RemoteListenRequest>> m_remoteListensInFlight;

    /** @brief GUI/运行时排队的远程监听停止请求。 */
    QStringList m_pendingRemoteStops;
};
