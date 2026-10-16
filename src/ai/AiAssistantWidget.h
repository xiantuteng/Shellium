#pragma once

#include "ai/AiChatTypes.h"
#include "ai/AiSettings.h"
#include "core/ConnectionTarget.h"

#include <QElapsedTimer>
#include <QDateTime>
#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <functional>

class AiClient;
class AiSettingsService;
class LocalCliClient;
class QCheckBox;
class QFrame;
class QLabel;
class QMenu;
class QPushButton;
class QTextEdit;
class QTimer;
class QToolButton;
class QVBoxLayout;
class QEvent;

/**
 * @brief AI 助手侧栏页面：与主机管理、效率工具、端口转发同级的第四个侧栏页面。
 *
 * 与 ProductivityWidget 的关系约定完全一致：本控件不持有 TerminalPane，只携带稳定
 * 目标信息和命令文本通过信号交给 MainWindow 路由，因此同一份实现可以在测试或未来的
 * 浮动窗口中复用。每个终端标签页拥有一个独立实例，因此聊天、模型请求和终端读取进度不会
 * 跨标签串线；单个实例内部保留最近十次会话，可通过顶部历史菜单恢复。上下文条只反映当前
 * 实例绑定的终端，是否把该终端最新输出主动附加到普通提问由用户在输入框下方的
 * 勾选框显式决定——这是"最小上下文"默认值的直接体现。
 *
 * 命令是否需要点击才下发由 AiExecutionPolicy 决定：Manual 下每条都要点击；AutoReadOnly
 * 下只有 CommandRiskClassifier 判定为只读且不会卡在交互界面的命令自动执行；FullAuto 下
 * 全部自动执行。风险判定只用本地静态规则，不采信模型自述；自动执行与手动点击走同一条
 * 按钮路径，并受每轮任务 kMaxAutoExecutedSteps 条的上限约束。
 *
 * 模型来源可以是 OpenAI 兼容网关（AiClient），也可以是本机已安装的智能体 CLI
 * （LocalCliClient），两者共用单步协议与执行审批。
 *
 * 对话流程是单步交互式的：模型每轮最多给出一条命令，用户点击"执行"后命令被发往当前终端，
 * 随后本控件轮询 terminalPlainText() 快照直到 Shell 提示符重新出现，把差量文本作为隐藏的
 * "执行结果"观察自动续问模型，模型据此给出下一条命令或收尾说明；原始输出不会回显到聊天区。
 * 这条捕获路径依赖终端本身
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
     * @brief 把当前非空会话归档到最近记录，清空聊天区并开启一轮全新会话。
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

    /**
     * @brief 请求把命令文本插入当前终端输入行，但不发送回车、不执行。
     * @param command 待插入的单行命令文本。
     * @param targetIds 恒为空列表，由主窗口路由到当前活动终端。
     *
     * 插入与执行必须使用不同信号，防止界面上的“插入/修改”动作误走 executeCommand() 并产生
     * 不可撤回的远端副作用。
     */
    void commandInsertionRequested(const QString &command, const QStringList &targetIds);

    /** @brief 可展示在主窗口顶部非阻塞提示区的状态或错误信息。 */
    void statusMessage(const QString &message);

private:
    /**
     * @brief 面板在当前主题下使用的一整套颜色。
     *
     * 聊天气泡、命令卡片和紧凑按钮分散在多个 append*() 里，如果各自写死十六进制值，换成
     * 深色主题时必然漏掉几处；集中成一张表后，新增控件只能从这里取色。
     */
    struct PanelColors {
        /** @brief 当前解析结果属于深色主题；卡片强调色等少数分支仍需直接判定。 */
        bool dark = false;
        QString panel;
        QString surface;
        QString border;
        QString text;
        QString muted;
        QString userBubble;
        QString userBubbleBorder;
        QString avatarTint;
        QString accent;
        QString codeBackground;
        QString codeText;
        /** @brief 通知气泡等处的半透明底色，深浅主题分别压黑与提亮。 */
        QString overlay;
        /** @brief 上下文条按钮的悬停底色。 */
        QString hover;
        QString actionBorder;
        QString actionHover;
        QString actionHoverBorder;
        QString actionDisabledText;
        QString actionDisabledBackground;
        QString actionDisabledBorder;
        QString warning;
        QString danger;
    };

    /** @brief 按深/浅主题解析出整套面板配色。 */
    [[nodiscard]] static PanelColors resolvePanelColors(bool dark);

    /**
     * @brief 依据应用调色板重新解析配色，刷新面板样式表并重绘已有消息。
     *
     * 深浅判定取自 qApp 调色板而不是另接一份终端设置：主题服务切换时会给 QApplication
     * 换调色板，本控件只需在 ApplicationPaletteChange 中重新应用，无需新增依赖。
     */
    void applyPanelTheme();

    /** @brief 构建上下文条、聊天区和输入区，构造函数只调用一次。 */
    void buildUi();

    /**
     * @brief 处理输入框键盘交互（Enter 发送、Shift+Enter 换行）与应用主题切换。
     * @param watched 产生事件的对象，只处理 m_inputEdit 与 qApp。
     * @param event Qt 输入事件。
     * @return 已消费普通 Enter 时返回 true；其他事件交回 QWidget 默认处理。
     *
     * 使用事件过滤器而不是监听 textChanged 后删除换行，可以在输入法提交、自动重复按键和
     * Shift 修饰键场景下保持行为确定，也不会让用户看到“先换行再消失”的界面闪动。
     */
    bool eventFilter(QObject *watched, QEvent *event) override;

    /**
     * @brief 按当前文本的实际排版高度调整输入框，范围限制为 1～10 行。
     *
     * 高度依据 QTextDocument 布局结果计算，因此自动换行和手工 Shift+Enter 都能触发增长；
     * 超过十行后固定高度并开启内部滚动条，防止输入区继续挤压聊天历史。
     */
    void updateInputEditorHeight();

    /**
     * @brief 执行当前会话中最后一条仍可操作的建议命令。
     *
     * 该方法由 Command+Enter 快捷键调用。每轮智能体只生成一条命令，但历史中可能保留多张
     * 卡片，因此必须从后向前查找“可见且启用”的执行按钮；找不到时只提示状态，不会误触
     * 已执行的历史命令，也不会把输入框内容当作命令直接下发。
     */
    void executeLatestPendingCommand();

    /** @brief 发送当前输入框内容：拼装历史、可选终端上下文，交给 AiClient。 */
    void sendCurrentInput();

    /**
     * @brief 真正发起一次模型请求的公共路径，供手动提问和命令执行后的自动续问共用。
     * @param historyText 计入对话历史并显示为用户气泡的正文。
     * @param ephemeralContext 仅本次请求携带、不写入历史也不展示的附加上下文；为空则不附加。
     */
    void sendMessageInternal(const QString &historyText, const QString &ephemeralContext = QString());

    /**
     * @brief 校验并处理模型返回的单步智能体响应。
     * @param content 模型本轮返回的完整原始内容。
     *
     * 该入口只接受“当前一步命令 / 已完成 / 需要用户输入”三种结构化响应。任何包含完整
     * 操作计划、多条命令或格式损坏的响应都不会直接展示，而是触发一次协议纠正请求，避免
     * 用户先看到十步教程、随后客户端才在末尾提示“只执行第一条”的错误体验。
     */
    void handleAgentReply(const QString &content);

    /**
     * @brief 要求模型把违规响应重新整理成单步协议。
     * @param invalidContent 未通过校验的模型原始响应，仅用于本次纠正请求。
     * @param validationError 具体校验错误，帮助模型有针对性地修正。
     *
     * 纠正消息属于智能体控制层，不写入可见聊天历史。达到最大重试次数后会给用户显示明确
     * 错误并停止，绝不把无法确认边界的多步骤内容降级展示。
     */
    void requestAgentProtocolRepair(const QString &invalidContent,
                                    const QString &validationError);

    /** @brief 按设置里的模型来源，把一次决策请求交给 HTTP 客户端或本地 CLI 客户端。 */
    void dispatchAgentRequest(const AiSettings &settings, const QList<AiChatMessage> &messages);

    /**
     * @brief 判断在给定执行策略下，一条命令能否不经点击直接下发。
     * @param policy 当前执行策略。
     * @param command 模型给出的单条命令。
     */
    [[nodiscard]] static bool shouldAutoExecute(AiExecutionPolicy policy, const QString &command);

    /**
     * @brief 新命令卡片渲染完毕后，按执行策略决定是否立即替用户点击"执行"。
     * @param command 卡片上的命令。
     * @param executeButton 卡片的执行按钮；自动执行复用它的点击路径。
     */
    void maybeAutoExecute(const QString &command, QPushButton *executeButton);

    /**
     * @brief 切换并持久化执行策略；切到全自动前会弹出一次明确的风险确认。
     */
    void setExecutionPolicy(AiExecutionPolicy policy);

    /** @brief 按当前执行策略刷新模式按钮、菜单勾选状态和底部提示文案。 */
    void updateExecutionModeUi();

    /** @brief 把一条用户消息追加到聊天区。 */
    void appendUserBubble(const QString &text);

    /** @brief 把一段助手文本（已剔除代码块）以头像+正文的形式追加到聊天区；空文本不追加。 */
    void appendAssistantBubble(const QString &text);

    /**
     * @brief 把一条建议命令渲染成带风险徽标和操作按钮的卡片。
     *
     * 卡片只展示执行意图、命令和操作按钮。真实终端输出属于终端本身，只作为智能体的隐藏
     * 观察值使用，不回显进聊天区，避免大段日志挤占对话空间或泄露终端中的敏感内容。
     * @param command 待展示且可能由用户执行的单条命令。
     * @param title 可选的命令目的说明；为空时不创建卡片内说明区。
     * @param reason 可选的执行原因说明；为空时不创建对应标签。
     * @param alreadyExecuted true 表示从历史恢复的命令已执行，执行按钮会保持禁用；false
     *        表示仍等待用户确认。
     * @return 卡片的执行按钮，供自动执行复用同一条点击路径。
     */
    QPushButton *appendCommandCard(const QString &command,
                           const QString &title = QString(),
                           const QString &reason = QString(),
                           bool alreadyExecuted = false);

    /** @brief 把一条纯状态提示（如错误信息）追加到聊天区。 */
    void appendNoticeBubble(const QString &text, bool isError);

    /** @brief 清空聊天区的可见消息控件，但保留末尾负责吸收空白的伸缩项。 */
    void clearChatContent();

    /**
     * @brief 把当前运行态写回最近会话列表，并把它移动到列表首位。
     *
     * 空白会话不会进入历史；超过十条时删除最久未访问的一条。该方法只复制模型上下文和
     * 终端读取位置，不接触终端对象或网络请求。
     */
    void synchronizeActiveConversation();

    /** @brief 根据当前会话列表重建历史下拉菜单，最多显示十条并标记当前项。 */
    void populateConversationHistoryMenu();

    /**
     * @brief 加载指定历史会话并重建聊天区。
     * @param conversationId 会话稳定 UUID；不存在或当前正忙时不切换。
     */
    void loadConversation(const QString &conversationId);

    /** @brief 根据 m_history 中已校验的协议消息重建用户气泡、助手说明和命令卡片。 */
    void renderActiveConversation();

    /**
     * @brief 从会话的第一条普通用户消息生成历史菜单标题。
     * @param history 已校验并写入上下文的消息序列。
     * @return 单行、最多 28 个字符的标题；没有普通用户消息时返回“未命名会话”。
     */
    [[nodiscard]] QString conversationTitle(const QList<AiChatMessage> &history) const;

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

    /** @brief 按当前是否有进行中的请求切换输入区可用状态。 */
    void setBusy(bool busy);

    /**
     * @brief 按当前是否有进行中的工作，把发送按钮在"发送"与"停止"两种形态间切换。
     *
     * 忙碌时不再把按钮置灰：模型分析或命令等待可能长时间不返回，一个灰掉的按钮等于告诉
     * 用户只能干等。同一个位置换成停止按钮，用户随时可以收回控制权。
     */
    void updateSendButtonMode();

    /**
     * @brief 停止当前进行中的分析或命令等待。
     *
     * 优先停止模型请求；没有进行中的请求时停止终端输出捕获。两者都没有时不做任何事，
     * 因此重复点击或快捷键误触不会破坏会话状态。
     */
    void stopActiveWork();

    /**
     * @brief 放弃当前这次终端输出捕获，但不打断已经下发到终端的命令。
     *
     * 命令本身运行在远端，界面无权也不应替用户杀掉它；这里只解除"等待它结束再自动续问"
     * 的绑定，并明确告诉用户命令仍在终端继续运行，避免误以为已经被取消。
     */
    void cancelOutputCapture();

    /**
     * @brief 滚动聊天区到底部。
     * @param force true 无条件滚动（用户自己发消息时调用）；false 只在用户此前就停留在
     *        底部附近时才跟随滚动——用户手动往上翻看历史时不能被新内容硬拽回底部。
     */
    void scrollToBottom(bool force = false);

    /**
     * @brief 命令卡片点击"执行"后调用，轮询终端快照直到 Shell 提示符重新出现。
     * @param command 已下发的命令文本，仅用于回填续问消息。
     * @param card 命令所在的卡片，等待状态显示在该卡片内部；为空则只进行后台捕获。
     * @param actionsRow 卡片底部的操作按钮行，等待状态插在它前面。
     *
     * 不能以“输出暂时不再变化”判断完成，因为安装、编译和下载可能长时间没有输出。开始时
     * 记录当前 Shell 提示符，后续只有观察到终端变化且提示符重新出现在末行才进入下一轮。
     */
    void beginOutputCapture(const QString &command, QFrame *card, QWidget *actionsRow);

    /** @brief 捕获定时器每次触发调用，判断是否已经可以收尾。 */
    void pollCaptureTick();

    /** @brief 结束一次输出捕获：把隐藏的终端差量交给智能体并自动续问，不在聊天区回显。 */
    void finishOutputCapture();

    AiSettingsService *m_settingsService = nullptr;
    AiClient *m_client = nullptr;
    /** @brief 本地智能体 CLI 来源的客户端；与 m_client 二选一使用，由设置决定。 */
    LocalCliClient *m_cliClient = nullptr;
    /** @brief 本轮用户任务已自动执行的命令数，用户每次亲自发送消息时归零。 */
    int m_autoExecutedSteps = 0;
    std::function<QString()> m_terminalContextProvider;

    ConnectionTarget m_activeTarget;
    QList<AiChatMessage> m_history;
    /**
     * @brief 当前智能体决策请求的完整消息快照，包含系统规则和一次性终端观察。
     *
     * 协议纠正时必须复用这份快照；如果只从持久历史重建，一次性的命令执行输出会丢失，模型
     * 就可能在没有真实观察的情况下猜测下一步。
     */
    QList<AiChatMessage> m_lastAgentRequestMessages;

    /**
     * @brief 单次聊天会话的可恢复快照。
     *
     * history 保存实际提交给模型的长期上下文；lastReadPosition 保证恢复旧会话后不会重新
     * 附加已经消费过的终端输出。标题从第一条用户任务生成，updatedAt 用于历史菜单展示。
     */
    struct ConversationSnapshot {
        QString id;
        QString title;
        QList<AiChatMessage> history;
        int lastReadPosition = 0;
        QDateTime updatedAt;
    };

    /** @brief 最近访问优先的会话快照，始终限制为最多十条。 */
    QList<ConversationSnapshot> m_recentConversations;
    /** @brief 当前会话 UUID；空白新会话虽未进入列表，也预先持有稳定 id。 */
    QString m_activeConversationId;
    /** @brief 当前决策已经进行的协议纠正次数；新任务或新终端观察到来时归零。 */
    int m_agentProtocolRepairCount = 0;
    bool m_requestInFlight = false;

    QLabel *m_contextTargetLabel = nullptr;
    /** @brief 打开当前标签页最近十次 AI 会话的图标按钮。 */
    QToolButton *m_historyButton = nullptr;
    /** @brief 由 populateConversationHistoryMenu() 在每次打开前重建的历史菜单。 */
    class QMenu *m_historyMenu = nullptr;
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
    /**
     * @brief 程序主动滚动期间为 true，防止 valueChanged 把自动滚动误认为用户改变阅读位置。
     */
    bool m_programmaticChatScroll = false;
    /** @brief 命令执行后等待 Shell 提示符重新出现期间为真；本控件同一时间只支持一次捕获。 */
    bool m_captureInFlight = false;
    QTimer *m_captureTimer = nullptr;
    /** @brief 本次捕获对应的命令卡片和按钮行，用于在卡片内维护等待状态。 */
    QPointer<QFrame> m_captureCardFrame;
    QPointer<QWidget> m_captureCardActionsRow;
    /** @brief 卡片里的动态等待状态；捕获结束后移除，原始终端输出不会替换进来。 */
    QPointer<QLabel> m_captureWaitingLabel;
    QString m_captureCommand;
    /**
     * @brief 轮询期间用来判断"是否还在变化"的终端快照；不用于最终提取，最终内容由
     * consumeNewTerminalOutput() 按 m_lastReadPosition 切片给出。
     */
    QString m_captureLastText;
    /** @brief 捕获开始那一刻的快照长度，用来判断"相对开始时是否变化过"。 */
    int m_captureStartLength = 0;
    /** @brief 是否已经观察到快照相对捕获开始时发生过变化；避免命令刚发出、回显未到达就被误判完成。 */
    bool m_captureEverChanged = false;
    /**
     * @brief 命令下发前终端末行的 Shell 提示符。
     *
     * 大多数命令结束后会回到完全相同的提示符；它比“输出静止一段时间”更可靠。若命令改变
     * 目录或身份导致提示符变化，轮询逻辑还会使用保守的通用提示符形态作为兼容判断。
     */
    QString m_captureBaselinePrompt;
    /** @brief 上一次刷新等待秒数，用来避免 300ms 轮询周期内重复设置相同文案。 */
    int m_captureLastReportedSecond = -1;
    QElapsedTimer m_captureElapsed;

    /**
     * @brief 终端内容已经读到的位置（字符偏移）。自动续问和手动附加共用同一根指针，读过的
     * 内容不会再出现在下一次请求里；开启新会话会归零。
     */
    int m_lastReadPosition = 0;

    /** @brief 当前主题解析出的面板配色；buildUi() 前由 applyPanelTheme() 填好。 */
    PanelColors m_colors;
    /**
     * @brief applyPanelTheme() 正在执行；用于挡住它自己 setStyleSheet() 引发的
     * PaletteChange，否则 changeEvent 会无限递归。
     */
    bool m_applyingPanelTheme = false;

    QCheckBox *m_attachContextCheck = nullptr;
    QTextEdit *m_inputEdit = nullptr;
    QPushButton *m_sendButton = nullptr;
    QPushButton *m_newSessionButton = nullptr;
    /** @brief 输入区右下角的执行模式切换按钮。 */
    QToolButton *m_executionModeButton = nullptr;
    QMenu *m_executionModeMenu = nullptr;
    /** @brief 面板底部随执行模式变化的风险提示。 */
    QLabel *m_disclaimerLabel = nullptr;
};
