#pragma once

#include "ai/AiChatTypes.h"
#include "core/ConnectionTarget.h"

#include <QElapsedTimer>
#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <functional>

class AiClient;
class AiSettingsService;
struct AiSettings;
class QCheckBox;
class QFrame;
class QLabel;
class QPushButton;
class QTextEdit;
class QTimer;
class QVBoxLayout;

/**
 * @brief AI 助手侧栏页面：与主机管理、效率工具、端口转发同级的第四个侧栏页面。
 *
 * 与 ProductivityWidget 的关系约定完全一致：本控件不持有 TerminalPane，只携带稳定
 * 目标信息和命令文本通过信号交给 MainWindow 路由，因此同一份实现可以在测试或未来的
 * 浮动窗口中复用。当前版本对话历史是进程内单一会话（不按主机拆分、不持久化），上下文
 * 条只反映"当前绑定的终端"，是否把该终端最新输出主动附加到普通提问由用户在输入框下方的
 * 勾选框显式决定——这是"最小上下文"默认值的直接体现；"只读/需审批"分级只影响建议命令卡片
 * 的展示与按钮颜色，不会有任何命令在用户点击"执行"之前被下发。
 *
 * 对话流程是单步交互式的：模型每轮最多给出一条命令，用户点击"执行"后命令被发往当前终端，
 * 随后本控件轮询 terminalPlainText() 快照直到新增输出稳定或超时，把差量文本整理成一条
 * "执行结果"消息自动续问模型，模型据此给出下一条命令或收尾说明。这条捕获路径依赖终端本身
 * 的纯文本快照，不新增独立的 exec 通道，因此对本地 Shell 和任意深度的嵌套 SSH 层都同样
 * 适用；代价是它只能感知"终端屏幕上出现了什么"，不能像独立 exec 那样拿到精确的退出码。
 */
class AiAssistantWidget final : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief 创建 AI 助手面板。
     * @param settingsService 应用级 AI 配置服务，生命周期必须覆盖本控件，不可为空。
     * @param parent Qt 父对象。
     */
    explicit AiAssistantWidget(AiSettingsService *settingsService, QWidget *parent = nullptr);

    /**
     * @brief 更新上下文条显示的"当前服务器"，并作为后续 attachTerminalContext 的标签来源。
     * @param target 当前活动标签页的连接目标；无效目标会让上下文条回退到"未连接"。
     */
    void setActiveTarget(const ConnectionTarget &target);

    /**
     * @brief 注入"读取当前终端输出"的回调。
     * @param provider 返回当前终端纯文本快照的函数；为空表示暂不支持附加终端上下文。
     *
     * 用回调而不是持有 TerminalPane 指针，是为了保持本控件与 ProductivityWidget 相同的
     * "不持有终端、只携带数据"边界；MainWindow 可以随标签切换更新这个回调指向的目标。
     */
    void setTerminalContextProvider(std::function<QString()> provider);

public slots:
    /**
     * @brief 清空对话历史和聊天区，开启一轮全新会话。
     *
     * 同时把终端读取位置归零：新会话不应该带着上一轮已经读过的旧输出增量，下一次读取
     * （无论是自动续问还是手动勾选附加）都会从终端当前内容重新起算。
     */
    void startNewSession();

signals:
    /**
     * @brief 用户点击建议命令卡片的"执行"后发出，交给上层路由到实际终端。
     * @param command 完整命令文本，不含结尾换行。
     * @param targetIds 恒为空列表，语义与 ProductivityWidget 一致：由上层使用当前终端。
     */
    void commandExecutionRequested(const QString &command, const QStringList &targetIds);

    /** @brief 可展示在主窗口顶部非阻塞提示区的状态或错误信息。 */
    void statusMessage(const QString &message);

private:
    /** @brief 构建上下文条、聊天区和输入区，构造函数只调用一次。 */
    void buildUi();

    /** @brief 发送当前输入框内容：拼装历史、可选终端上下文，交给 AiClient。 */
    void sendCurrentInput();

    /**
     * @brief 真正发起一次模型请求的公共路径，供手动提问和命令执行后的自动续问共用。
     * @param historyText 计入对话历史并显示为用户气泡的正文。
     * @param ephemeralContext 仅本次请求携带、不写入历史也不展示的附加上下文；为空则不附加。
     */
    void sendMessageInternal(const QString &historyText, const QString &ephemeralContext = QString());

    /** @brief 把一条用户消息追加到聊天区。 */
    void appendUserBubble(const QString &text);

    /** @brief 把一段助手文本（已剔除代码块）以头像+正文的形式追加到聊天区；空文本不追加。 */
    void appendAssistantBubble(const QString &text);

    /**
     * @brief 把一条建议命令渲染成带风险徽标和操作按钮的卡片。
     *
     * 命令、执行后的真实输出、操作按钮都装在同一张卡片里——参考图里的工具调用是一个完整
     * 板块，不是散落的几条独立气泡；beginOutputCapture() 后续会把输出直接插回这张卡片。
     */
    void appendCommandCard(const QString &command);

    /** @brief 把一条纯状态提示（如错误信息）追加到聊天区。 */
    void appendNoticeBubble(const QString &text, bool isError);

    /** @brief 构建"AI 图标闪烁 + 正在分析…"的等待指示器，图标透明度循环呼吸。 */
    QWidget *buildThinkingIndicator();

    /**
     * @brief 返回终端自上次读取位置以来新增的纯文本，并把读取位置推进到当前末尾；不追加
     * 到聊天区——命令的真实输出只喂给模型，不在界面上重复展示一遍。
     * @return 没有终端上下文提供者、或自上次读取以来没有任何新内容时返回空字符串。
     *
     * 从不整屏读取：无论是命令执行后的自动续问，还是用户手动勾选"附加终端输出"，都只
     * 消费自己尚未读过的那一段增量，读过的内容不会重复出现在下一次请求里。
     */
    [[nodiscard]] QString consumeNewTerminalOutput();

    /**
     * @brief 拆分模型回复：围栏代码块视为待执行命令，其余文本作为普通助手消息。
     * @param content 模型返回的完整文本。
     */
    void renderAssistantReply(const QString &content);

    /** @brief 按当前是否有进行中的请求切换输入区可用状态。 */
    void setBusy(bool busy);

    /**
     * @brief 滚动聊天区到底部。
     * @param force true 无条件滚动（用户自己发消息时调用）；false 只在用户此前就停留在
     *        底部附近时才跟随滚动——用户手动往上翻看历史时不能被新内容硬拽回底部。
     */
    void scrollToBottom(bool force = false);

    /**
     * @brief 命令卡片点击"执行"后调用，开始轮询终端快照直到新增输出稳定或超时。
     * @param command 已下发的命令文本，仅用于回填续问消息。
     * @param card 命令所在的卡片，捕获结束后把真实输出插回这张卡片；为空则跳过界面更新。
     * @param actionsRow 卡片底部的操作按钮行，输出要插在它前面，不能插到按钮下面去。
     *
     * 没有终端上下文提供者时静默跳过——命令仍会照常执行，只是不会自动续问，行为退化为
     * 一次性建议卡片。
     */
    void beginOutputCapture(const QString &command, QFrame *card, QWidget *actionsRow);

    /** @brief 捕获定时器每次触发调用，判断是否已经可以收尾。 */
    void pollCaptureTick();

    /** @brief 结束一次输出捕获：整理差量文本，追加气泡并自动续问模型。 */
    void finishOutputCapture(bool timedOut);

    AiSettingsService *m_settingsService = nullptr;
    AiClient *m_client = nullptr;
    std::function<QString()> m_terminalContextProvider;

    ConnectionTarget m_activeTarget;
    QList<AiChatMessage> m_history;
    bool m_requestInFlight = false;

    QLabel *m_contextTargetLabel = nullptr;
    QWidget *m_chatContainer = nullptr;
    QVBoxLayout *m_chatLayout = nullptr;
    QWidget *m_thinkingBubble = nullptr;
    class QScrollArea *m_chatScroll = nullptr;
    /**
     * @brief 用户此前是否停留在聊天区底部附近；由滚动条 valueChanged 持续更新。
     *
     * 只有用户自己拖动或程序自己滚到底部才会改变这个值——单纯因为新内容让 maximum() 变大
     * 不会触发 valueChanged，因此"用户手动往上翻"这件事不会被内容增长悄悄抹掉。
     */
    bool m_stickyBottom = true;
    /** @brief 流式回复期间实时更新的预览气泡；收到完整回复后销毁，改由结构化渲染接管。 */
    QLabel *m_streamingPreviewLabel = nullptr;

    /** @brief 命令执行后等待输出稳定期间为真；本控件同一时间只支持一次捕获。 */
    bool m_captureInFlight = false;
    QTimer *m_captureTimer = nullptr;
    /** @brief 本次捕获对应的命令卡片和按钮行；捕获结束后真实输出要插回这张卡片。 */
    QPointer<QFrame> m_captureCardFrame;
    QPointer<QWidget> m_captureCardActionsRow;
    /** @brief 卡片里"等待输出…"的占位行；捕获结束后被真实输出替换掉。 */
    QPointer<QLabel> m_captureWaitingLabel;
    QString m_captureCommand;
    /**
     * @brief 轮询期间用来判断"是否还在变化"的终端快照；不用于最终提取，最终内容由
     * consumeNewTerminalOutput() 按 m_lastReadPosition 切片给出。
     */
    QString m_captureLastText;
    /** @brief 捕获开始那一刻的快照长度，用来判断"相对开始时是否变化过"。 */
    int m_captureStartLength = 0;
    /** @brief 快照连续多少个轮询周期未变化；配合 m_captureEverChanged 判定"已经稳定"。 */
    int m_captureStableTicks = 0;
    /** @brief 是否已经观察到快照相对捕获开始时发生过变化；避免命令刚发出、回显未到达就被误判完成。 */
    bool m_captureEverChanged = false;
    QElapsedTimer m_captureElapsed;

    /**
     * @brief 终端内容已经读到的位置（字符偏移）。自动续问和手动附加共用同一根指针，读过的
     * 内容不会再出现在下一次请求里；开启新会话会归零。
     */
    int m_lastReadPosition = 0;

    QCheckBox *m_attachContextCheck = nullptr;
    QTextEdit *m_inputEdit = nullptr;
    QPushButton *m_sendButton = nullptr;
    QPushButton *m_newSessionButton = nullptr;
};
