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
     * @brief 请求在当前栈顶之上建立新一层，终端 PTY 随后切换到该层。
     * @param target 内层服务器；其 host/port 由栈顶服务器的视角解析。
     *
     * 新层的传输经栈顶会话的 direct-tcpip 通道建立，因此不需要内层服务器从本机可达，
     * 也不会把中间跳板的凭据暴露给内层。结果通过 layersChanged 或 layerFailed 返回。
     */
    void requestPushLayer(const ConnectionTarget &target);

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
     * @brief 正在建立的层需要密码。
     * @param target 待认证的服务器。
     * @param retry 是否为上一次密码被拒后的重试。
     *
     * 上层必须在终端内联提示并通过 provideLayerCredential 回传。弹对话框会破坏"手打 ssh"
     * 的原生体验——用户预期的是终端里的 password 提示，不是一个窗口。
     */
    void layerCredentialRequired(const ConnectionTarget &target, bool retry);

protected:
    /** @brief 建立栈底层并执行统一的输入、输出与转发泵送循环。 */
    void run() override;

private:
    /** @brief GUI 线程对未知主机指纹的作答，由工作线程在下一轮推进时消费。 */
    enum class HostKeyDecision { None, Trusted, Rejected };

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

    /** @brief 关闭并弹出栈顶层，返回被弹出层的目标描述。 */
    ConnectionTarget popTopLayer();

    /** @brief 当前层栈从外层到内层的目标快照。 */
    [[nodiscard]] QList<ConnectionTarget> layerTargets() const;

    /**
     * @brief 泵送所有层的转发通道并派发本轮得出结论的建立结果。
     *
     * 内层传输依赖外层通道，必须整栈一起搬运。结果统一在遍历结束后派发：派发过程可能
     * 压入新层，而 std::vector 的 push_back 会让正在使用的迭代器失效。
     */
    void pumpAllForwards();

    /** @brief 把一条转发建立结果交给它的发起方——建层流程或 SFTP 请求。 */
    void handleForwardOutcome(const SshSessionNode::ForwardOutcome &outcome);

    /** @brief 把 GUI/SFTP 线程排队的 direct-tcpip 请求转成异步建立，不等待结果。 */
    void serviceForwardRequests();

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
    QList<ConnectionTarget> m_pendingPushes;

    /**
     * @brief 正在等待隧道就绪的建层请求；同一时刻至多一个。
     *
     * 新层必须先建成才能承载下一层，因此建层天然串行；forwardId 为 0 表示当前没有在途请求。
     */
    ConnectionTarget m_layerPushTarget;
    quint64 m_layerPushForwardId = 0;

    /** @brief 已发起但尚未得出结论的 SFTP 隧道请求，按转发编号索引。 */
    QHash<quint64, QSharedPointer<ForwardRequest>> m_forwardsInFlight;

    /** @brief GUI 排队的退层次数。 */
    int m_pendingPops = 0;

    int m_columns = 80;
    int m_rows = 24;
    bool m_sizeDirty = true;
    QList<QSharedPointer<ForwardRequest>> m_forwardRequests;
};
