#include "ai/AiAssistantWidget.h"

#include "ai/AiClient.h"
#include "ai/AiSettings.h"
#include "ai/AiSettingsService.h"
#include "i18n/Retranslate.h"
#include "ai/CommandRiskClassifier.h"
#include "ui/UiIcons.h"

#include <QAction>
#include <QCheckBox>
#include <QAbstractTextDocumentLayout>
#include <QEvent>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QApplication>
#include <QGuiApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QMenu>
#include <QPointer>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QSizePolicy>
#include <QTextEdit>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace {

const QString kSystemPrompt = QStringLiteral(R"PROMPT(
你是嵌入 SSH 终端客户端 Shellium 的交互式运维智能体。你的职责不是一次性编写教程或安装
手册，而是基于当前终端的真实状态，每轮只决定一个最小、可验证的下一步。

你必须严格遵守以下状态机：
1. 用户提出目标后，先判断完成目标所需的“当前一步”。你可以在心里规划后续步骤，但绝不能
   输出、预告或编号列出完整计划，也不能描述第二步及之后的命令。
2. 如果需要执行命令，本轮只能给出一条命令。先用简洁中文说明“本步要确认/改变什么”以及
   “为什么现在需要执行它”，再给出命令。说明必须针对当前命令，不得包含后续步骤。
3. 命令由用户点击执行，客户端随后会把真实新增终端输出作为观察结果再次发给你。在看到观察
   结果前，不得假设命令成功，不得直接继续下一步，也不得编造退出码或输出。
4. 收到真实观察后，重新判断：目标已完成则结束；尚未完成则只返回下一条命令；输出存在异常
   则只返回当前最合适的诊断或修复命令；需要密码、业务选择、不可从终端判断的信息或危险授权
   时暂停并询问用户。
5. 终端观察是内部证据，不是聊天内容。title、reason 和 message 只能概括判断结论，不得逐行
   粘贴、复述或用代码块展示原始命令回显、日志、提示符、密码、令牌及查询结果。
6. 优先先探测再修改：在操作系统、包管理器、权限、已有安装状态不明确时，第一步通常应是只读
   检查，而不是直接安装。不要重复已经从真实输出中确认完成的动作。
7. 不得把多条命令用 &&、;、||、管道、换行、脚本解释器或 shell 包装器拼成“一条命令”来规避
   单步限制。只有单个原子操作自身必需的简单参数才允许出现在命令中。

每轮必须只输出一个 JSON 对象，不能使用 Markdown 围栏，不能在 JSON 前后添加任何文字。允许
三种格式：

需要执行一步：
{"state":"command","title":"本步目的，不带序号","reason":"为什么此刻执行这条命令","command":"唯一一条原子命令"}

目标已经完成：
{"state":"complete","message":"基于哪项真实输出确认完成，以及简短结果"}

需要用户提供信息或确认：
{"state":"question","message":"只询问继续推进所必需的问题"}

硬性限制：state=command 时 title、reason、command 都必须非空；command 不能包含换行；任何响应
最多只能出现一个 command 字段。不要输出 steps、plan、next、后续操作、完整方案或多个候选命令。
)PROMPT");

/** @brief 协议校验失败时最多自动纠正一次，避免网关模型持续违规造成无限请求循环。 */
constexpr int kMaxAgentProtocolRepairs = 1;

/** @brief 捕获轮询周期；太短会让 QPlainTextEdit 全文导出成为明显开销，太长会拖慢续问节奏。 */
constexpr int kCapturePollIntervalMs = 300;
/** @brief 输入框默认单行高度；包含文档边距、控件边框和上下内容边距。 */
constexpr int kInputMinimumHeight = 42;
/** @brief 输入框允许显示的最大文本行数，超过后改为输入框内部滚动。 */
constexpr int kInputMaximumVisibleLines = 10;
/** @brief 单个终端标签最多保留的 AI 会话数，避免长期操作后历史菜单和内存无限增长。 */
constexpr int kMaximumRecentConversations = 10;

/**
 * @brief 返回当前平台上物理 Command 键在 Qt 键盘事件中的修饰键表示。
 *
 * Qt 为了让通用的 Ctrl 系列快捷键在 macOS 上自动对应 Command，会交换 Control 与 Meta
 * 的平台语义：macOS 的物理 Command 是 ControlModifier，物理 Control 才是 MetaModifier。
 * 这里集中定义映射，避免快捷键注册和输入框事件过滤使用不同修饰键，造成“测试可用、实机
 * 无效”或 Command+Enter 被误当成普通 Enter 发送聊天消息。
 */
#if defined(Q_OS_MACOS)
constexpr Qt::KeyboardModifier kCommandKeyModifier = Qt::ControlModifier;
#else
constexpr Qt::KeyboardModifier kCommandKeyModifier = Qt::MetaModifier;
#endif

/**
 * @brief 判断键盘事件是否按下了当前平台的物理 Command 键。
 * @param event 待检查的按键事件，调用方保证指针有效。
 * @return 按下物理 Command 键时返回 true，否则返回 false。
 */
bool hasCommandKeyModifier(const QKeyEvent *event)
{
    Q_ASSERT(event != nullptr);
    return event->modifiers().testFlag(kCommandKeyModifier);
}

/** @brief 单步智能体响应的状态类型。 */
enum class AgentReplyState {
    Command,
    Complete,
    Question
};

/**
 * @brief 经过本地严格校验的智能体决策。
 *
 * command 状态使用 title、reason、command；complete/question 状态只使用 message。把协议先
 * 解析成强类型对象再渲染，可以从架构上阻断“模型输出十步、界面只隐藏九条命令”的漏洞。
 */
struct AgentReply {
    AgentReplyState state = AgentReplyState::Question;
    QString title;
    QString reason;
    QString command;
    QString message;
};

/**
 * @brief 判断命令是否疑似把多个 shell 操作拼接成一步。
 * @param command 已去除首尾空白的命令。
 * @return 检测到换行、命令连接符或常见 shell 包装器时返回 true。
 *
 * 这里采取保守校验：引号内的复杂 shell 语法也拒绝，因为智能体完全可以把它拆成下一轮再做；
 * 宁可触发一次格式纠正，也不能让模型用 `sh -c` 绕过用户逐步审批的安全边界。
 */
bool containsCompoundShellOperation(const QString &command)
{
    static const QRegularExpression compoundOperator(
        QStringLiteral("[\\r\\n]|&&|\\|\\||;|(?<!\\|)\\|(?!\\|)"));
    static const QRegularExpression shellWrapper(
        QStringLiteral("^(?:sudo\\s+)?(?:sh|bash|zsh|fish)\\s+(?:-[a-zA-Z]*c|--command)(?:\\s|$)"),
        QRegularExpression::CaseInsensitiveOption);
    return compoundOperator.match(command).hasMatch() || shellWrapper.match(command).hasMatch();
}

/**
 * @brief 解析并严格校验单步智能体 JSON 协议。
 * @param content 模型返回的原始文本。
 * @param reply 成功时写入强类型决策。
 * @param error 失败时写入可供协议纠正使用的中文原因。
 * @return 只有响应恰好是一个合法状态对象时返回 true。
 */
bool parseAgentReply(const QString &content, AgentReply *reply, QString *error)
{
    Q_ASSERT(reply != nullptr);
    Q_ASSERT(error != nullptr);

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(content.trimmed().toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("响应不是单个有效 JSON 对象");
        return false;
    }

    const QJsonObject object = document.object();
    const QString state = object.value(QStringLiteral("state")).toString().trimmed();
    if (state == QStringLiteral("command")) {
        const QSet<QString> allowedKeys{
            QStringLiteral("state"), QStringLiteral("title"),
            QStringLiteral("reason"), QStringLiteral("command")};
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (!allowedKeys.contains(it.key())) {
                *error = QStringLiteral("command 状态包含不允许的字段：%1").arg(it.key());
                return false;
            }
        }
        reply->state = AgentReplyState::Command;
        reply->title = object.value(QStringLiteral("title")).toString().trimmed();
        reply->reason = object.value(QStringLiteral("reason")).toString().trimmed();
        reply->command = object.value(QStringLiteral("command")).toString().trimmed();
        if (reply->title.isEmpty() || reply->reason.isEmpty() || reply->command.isEmpty()) {
            *error = QStringLiteral("command 状态缺少 title、reason 或 command");
            return false;
        }
        if (containsCompoundShellOperation(reply->command)) {
            *error = QStringLiteral("command 包含多条操作、换行、管道或 shell 包装器");
            return false;
        }
        return true;
    }

    if (state == QStringLiteral("complete") || state == QStringLiteral("question")) {
        const QSet<QString> allowedKeys{QStringLiteral("state"), QStringLiteral("message")};
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (!allowedKeys.contains(it.key())) {
                *error = QStringLiteral("%1 状态包含不允许的字段：%2")
                             .arg(state, it.key());
                return false;
            }
        }
        reply->state = state == QStringLiteral("complete")
            ? AgentReplyState::Complete : AgentReplyState::Question;
        reply->message = object.value(QStringLiteral("message")).toString().trimmed();
        if (reply->message.isEmpty()) {
            *error = QStringLiteral("complete/question 状态缺少 message");
            return false;
        }
        if (object.contains(QStringLiteral("command"))) {
            *error = QStringLiteral("complete/question 状态不允许包含 command");
            return false;
        }
        return true;
    }

    *error = QStringLiteral("state 必须是 command、complete 或 question");
    return false;
}

/**
 * @brief 风险分级对应的强调色，与效果图集使用的语义色保持一致。
 *
 * 深色面板上同一组语义色会明显发闷，因此三档各自提供一个提亮版本；色相保持不变，用户
 * 在两套主题下仍然按“绿=只读、蓝=写入、红=高危”识别卡片。
 */
QColor riskAccentColor(const CommandRiskLevel level, const bool dark)
{
    switch (level) {
    case CommandRiskLevel::ReadOnly:
        return QColor(dark ? QStringLiteral("#35c98a") : QStringLiteral("#159a60"));
    case CommandRiskLevel::Write:
        return QColor(dark ? QStringLiteral("#6aa9f0") : QStringLiteral("#3b78c9"));
    case CommandRiskLevel::HighRisk:
        return QColor(dark ? QStringLiteral("#ef776e") : QStringLiteral("#d84a41"));
    }
    return QColor(Qt::gray);
}

/** @brief 截断过长的终端上下文，避免一次性把上万行历史塞进请求体。 */
QString truncateContext(const QString &text, const int maxLines = 400)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.size() <= maxLines) {
        return text;
    }
    return lines.mid(lines.size() - maxLines).join(QLatin1Char('\n'));
}

/**
 * @brief 在长路径/长参数里插入零宽空格，给排版引擎制造换行机会。
 *
 * QLabel 开了 setWordWrap(true) 也只会在空格处断行；命令里常见的长路径一整段没有空格，
 * 超出卡片宽度时会撑开整个聊天区，逼出横向滚动条。零宽空格不可见、复制粘贴也不受影响，
 * 只在常见分隔符（/ - _ . , :）后面、且已经连续跑了一段够长的无断点文本时才插入一个。
 */
QString insertSoftBreakpoints(const QString &text)
{
    static const QString breakChars = QStringLiteral("/-_.,:=");
    QString result;
    result.reserve(text.size() + 16);
    int runLength = 0;
    for (const QChar &ch : text) {
        result.append(ch);
        if (ch.isSpace()) {
            runLength = 0;
            continue;
        }
        ++runLength;
        if (runLength > 12 && breakChars.contains(ch)) {
            result.append(QChar(0x200B));
            runLength = 0;
        }
    }
    return result;
}

/**
 * @brief 从终端快照中取得最后一条非空可见行。
 * @param terminalText 终端当前纯文本快照。
 * @return 去除首尾空白的末行；没有可见内容时返回空字符串。
 *
 * 终端视图通常在提示符后保留大量屏幕填充空行，直接使用 QString::section(..., -1) 会取到
 * 空行而不是提示符，因此必须从末尾向前查找最后一条实际内容。
 */
QString lastVisibleTerminalLine(const QString &terminalText)
{
    const QStringList lines = terminalText.split(QLatin1Char('\n'));
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QString line = it->trimmed();
        if (!line.isEmpty()) {
            return line;
        }
    }
    return QString();
}

/**
 * @brief 判断终端末行是否为命令执行完毕后重新出现的 Shell 提示符。
 * @param terminalText 当前完整终端快照。
 * @param baselinePrompt 命令执行前记录的提示符末行。
 * @return 末行等于原提示符，或符合常见 Unix Shell 提示符形态时返回 true。
 *
 * 精确匹配覆盖绝大多数场景；通用正则兼容 `cd`、`sudo -s` 等导致目录或身份变化的命令。
 * 通用规则要求末行以 `$`、`#`、`%` 或 `>` 和可选空格结尾，并限制长度，降低普通日志行
 * 被误判为提示符的概率。无法识别的高度自定义提示符会继续等待，而不会提前读取半截输出。
 */
bool shellPromptHasReturned(const QString &terminalText, const QString &baselinePrompt)
{
    const QString lastLine = lastVisibleTerminalLine(terminalText);
    if (lastLine.isEmpty()) {
        return false;
    }
    if (!baselinePrompt.isEmpty() && lastLine == baselinePrompt) {
        return true;
    }

    static const QRegularExpression commonPrompt(QStringLiteral(
        "^(?=.{1,240}$)(?:\\[[^\\r\\n]+\\]|"
        "[^\\s@]+@[^\\s:]+(?::[^\\r\\n]*)?|[A-Za-z0-9_.-]+)\\s*[#$%>]\\s*$"));
    return commonPrompt.match(lastLine).hasMatch();
}

} // namespace

AiAssistantWidget::AiAssistantWidget(AiSettingsService *settingsService, QWidget *parent)
    : QWidget(parent)
    , m_settingsService(settingsService)
    , m_client(new AiClient(this))
{
    Q_ASSERT(m_settingsService != nullptr);
    // 空白新会话也预先分配稳定 id；只有产生了模型上下文后才会写入最近会话列表，避免用户
    // 连续点击“新会话”制造一串没有内容的历史项。
    m_activeConversationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    buildUi();
    if (qApp != nullptr) {
        qApp->installEventFilter(this);
    }

    // 智能体使用机器可校验的 JSON 协议。流式过程中 JSON 必然是不完整的，直接预览会把
    // state/command 等内部字段暴露给用户，也可能短暂显示后续违规内容，因此这里只维持
    // “正在判断下一步”状态，等完整响应通过协议校验后再一次性渲染用户可见内容。
    connect(m_client, &AiClient::replyChunkReceived, this, [this](const QString &) {
        scrollToBottom();
    });
    connect(m_client, &AiClient::replyFinished, this, [this](const QString &content) {
        if (m_thinkingBubble != nullptr) {
            m_thinkingBubble->deleteLater();
            m_thinkingBubble = nullptr;
        }
        handleAgentReply(content);
    });
    connect(m_client, &AiClient::replyFailed, this, [this](const QString &message) {
        if (m_thinkingBubble != nullptr) {
            m_thinkingBubble->deleteLater();
            m_thinkingBubble = nullptr;
        }
        appendNoticeBubble(message, true);
        m_agentProtocolRepairCount = 0;
        setBusy(false);
        scrollToBottom();
        emit statusMessage(message);
    });
}

AiAssistantWidget::PanelColors AiAssistantWidget::resolvePanelColors(const bool dark)
{
    PanelColors colors;
    colors.dark = dark;
    // 深色一组刻意与主窗口侧栏的 SIDEBAR/SURFACE/BORDER 取同一批灰蓝，避免 AI 面板在
    // 侧栏里显出一块色温不同的方块。
    colors.panel = dark ? QStringLiteral("#191f31") : QStringLiteral("#fbfcfd");
    colors.surface = dark ? QStringLiteral("#21283b") : QStringLiteral("#ffffff");
    colors.border = dark ? QStringLiteral("#30384f") : QStringLiteral("#cbd5e1");
    colors.text = dark ? QStringLiteral("#f0f3fa") : QStringLiteral("#1d2939");
    colors.muted = dark ? QStringLiteral("#929bb3") : QStringLiteral("#8a96a8");
    colors.userBubble = dark ? QStringLiteral("#24413d") : QStringLiteral("#e6f2ec");
    colors.userBubbleBorder = dark ? QStringLiteral("#2f6256") : QStringLiteral("#c6dfd3");
    colors.avatarTint = dark ? QStringLiteral("#24413d") : QStringLiteral("#e1f2ea");
    colors.accent = dark ? QStringLiteral("#2bd981") : QStringLiteral("#17834f");
    colors.codeBackground = dark ? QStringLiteral("#151a2b") : QStringLiteral("#f1f4f7");
    colors.codeText = dark ? QStringLiteral("#d7deec") : QStringLiteral("#263244");
    // 深色下再压黑等于看不见，半透明层必须反向提亮才能保持同样的层次感。
    colors.overlay = dark ? QStringLiteral("rgba(255,255,255,0.07)")
                          : QStringLiteral("rgba(0,0,0,0.04)");
    colors.hover = dark ? QStringLiteral("rgba(255,255,255,0.10)")
                        : QStringLiteral("rgba(0,0,0,0.06)");
    colors.actionBorder = dark ? QStringLiteral("#3c4560") : QStringLiteral("#ccd6e2");
    colors.actionHover = dark ? QStringLiteral("#2e3852") : QStringLiteral("#f1f5f9");
    colors.actionHoverBorder = dark ? QStringLiteral("#5a6480") : QStringLiteral("#94a3b8");
    colors.actionDisabledText = dark ? QStringLiteral("#6b748c") : QStringLiteral("#94a3b8");
    colors.actionDisabledBackground = dark ? QStringLiteral("#232a3d")
                                           : QStringLiteral("#eef2f6");
    colors.actionDisabledBorder = dark ? QStringLiteral("#333c54") : QStringLiteral("#d7dee7");
    colors.warning = dark ? QStringLiteral("#e2a94f") : QStringLiteral("#b8791f");
    colors.danger = dark ? QStringLiteral("#ef776e") : QStringLiteral("#d84a41");
    return colors;
}

void AiAssistantWidget::applyPanelTheme()
{
    if (m_applyingPanelTheme) {
        return;
    }
    m_applyingPanelTheme = true;
    const auto releaseGuard = qScopeGuard([this]() { m_applyingPanelTheme = false; });
    const bool dark =
        QGuiApplication::palette().color(QPalette::Window).lightnessF() < 0.5;
    m_colors = resolvePanelColors(dark);

    // 选择器全部限定在 AiAssistantPanel 子树，不会改变主机管理、终端和设置页的全局按钮
    // 与输入框样式。
    QString styleSheet = QStringLiteral(R"QSS(
        QWidget#AiAssistantPanel,
        QWidget#AiChatContainer,
        QScrollArea#AiChatScroll,
        QScrollArea#AiChatScroll > QWidget > QWidget {
            background: @PANEL@;
            color: @TEXT@;
        }
        QFrame#AiComposer {
            background: @SURFACE@;
            border: 1px solid @BORDER@;
            border-radius: 12px;
        }
        QFrame#AiComposer QTextEdit#aiInputEdit {
            color: @TEXT@;
            background: transparent;
            border: 0;
            border-radius: 0;
            padding: 4px 6px;
            font-size: 14px;
        }
        QFrame#AiComposer QCheckBox {
            color: @MUTED@;
            spacing: 6px;
            font-size: 12px;
        }
        QPushButton[aiCompactAction="true"] {
            min-height: 20px;
            max-height: 20px;
            padding: 2px 8px;
            border: 1px solid @ACTION_BORDER@;
            border-radius: 6px;
            font-size: 12px;
            font-weight: 600;
        }
        QPushButton[aiCompactAction="true"]:hover {
            background: @ACTION_HOVER@;
            border-color: @ACTION_HOVER_BORDER@;
        }
        QPushButton[aiCompactAction="true"][aiPrimaryAction="true"] {
            color: #ffffff;
            background: #17834f;
            border-color: #17834f;
        }
        QPushButton[aiCompactAction="true"][aiPrimaryAction="true"]:hover {
            background: #126b40;
        }
        QPushButton[aiCompactAction="true"]:disabled {
            color: @ACTION_DISABLED_TEXT@;
            background: @ACTION_DISABLED_BG@;
            border-color: @ACTION_DISABLED_BORDER@;
        }
    )QSS");
    styleSheet.replace(QStringLiteral("@PANEL@"), m_colors.panel);
    styleSheet.replace(QStringLiteral("@SURFACE@"), m_colors.surface);
    styleSheet.replace(QStringLiteral("@BORDER@"), m_colors.border);
    styleSheet.replace(QStringLiteral("@TEXT@"), m_colors.text);
    styleSheet.replace(QStringLiteral("@MUTED@"), m_colors.muted);
    styleSheet.replace(QStringLiteral("@ACTION_BORDER@"), m_colors.actionBorder);
    styleSheet.replace(QStringLiteral("@ACTION_HOVER@"), m_colors.actionHover);
    styleSheet.replace(QStringLiteral("@ACTION_HOVER_BORDER@"), m_colors.actionHoverBorder);
    styleSheet.replace(QStringLiteral("@ACTION_DISABLED_TEXT@"), m_colors.actionDisabledText);
    styleSheet.replace(QStringLiteral("@ACTION_DISABLED_BG@"), m_colors.actionDisabledBackground);
    styleSheet.replace(QStringLiteral("@ACTION_DISABLED_BORDER@"), m_colors.actionDisabledBorder);
    setStyleSheet(styleSheet);

    // 图标是按当时的文字色离线渲染成位图的，不会跟随调色板重绘，必须显式重新生成。
    if (auto *serverIcon = findChild<QLabel *>(QStringLiteral("aiContextServerIcon"))) {
        serverIcon->setPixmap(UiIcons::renderSymbolPixmap(
            UiIcons::Symbol::HostItem, QColor(m_colors.text), QSize(14, 14),
            devicePixelRatioF()));
    }
    if (m_historyButton != nullptr) {
        m_historyButton->setIcon(
            UiIcons::symbol(this, UiIcons::Symbol::Clock, QColor(m_colors.text)));
        m_historyButton->setStyleSheet(QStringLiteral(
            "QToolButton { color: palette(text); background: transparent;"
            "border: 1px solid palette(mid); border-radius: 9px; padding: 5px; }"
            "QToolButton:hover, QToolButton:pressed { background: %1; }"
            "QToolButton::menu-indicator { image: none; width: 0; }").arg(m_colors.hover));
    }
    if (m_newSessionButton != nullptr) {
        m_newSessionButton->setStyleSheet(QStringLiteral(
            "QPushButton { color: palette(text); background: transparent;"
            "border: 1px solid palette(mid); border-radius: 9px; padding: 2px 9px; }"
            "QPushButton:hover { background: %1; }").arg(m_colors.hover));
    }

    // 已经在屏幕上的气泡和卡片是构建时按旧配色写死的内联样式，只能整段重绘。请求或命令
    // 捕获进行中时跳过：那些临时控件（思考动画、等待标签）不在会话历史里，重建会把它们
    // 连同正在进行的状态一起抹掉。
    if (m_chatLayout != nullptr && !m_requestInFlight && !m_captureInFlight) {
        clearChatContent();
        renderActiveConversation();
    }
}

void AiAssistantWidget::buildUi()
{
    setObjectName(QStringLiteral("AiAssistantPanel"));
    applyPanelTheme();
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    // 页面标题本身复用侧栏共享的 SidebarHeader（"AI 助手"三字），不在这里重复渲染；
    // 这一行只保留当前标签的目标名称和会话动作。按设计要求移除“当前服务器”前缀，让窄
    // 侧栏把更多横向空间留给真实目标名，同时仍用服务器图标表达字段语义。
    auto *contextBar = new QWidget(this);
    auto *contextLayout = new QHBoxLayout(contextBar);
    contextLayout->setContentsMargins(0, 0, 0, 0);
    contextLayout->setSpacing(6);
    auto *serverIcon = new QLabel(contextBar);
    serverIcon->setObjectName(QStringLiteral("aiContextServerIcon"));
    serverIcon->setPixmap(UiIcons::renderSymbolPixmap(
        UiIcons::Symbol::HostItem, QColor(m_colors.text), QSize(14, 14), devicePixelRatioF()));
    contextLayout->addWidget(serverIcon);
    m_contextTargetLabel = new QLabel(tr("No connected terminal"), contextBar);
    QFont targetFont = m_contextTargetLabel->font();
    targetFont.setBold(true);
    m_contextTargetLabel->setFont(targetFont);
    m_contextTargetLabel->setWordWrap(true);
    contextLayout->addWidget(m_contextTargetLabel, 1);

    // 历史按钮使用“时钟”语义图标并采用即时弹出模式：一次点击就能看到当前终端标签最近
    // 十次会话，不额外占用文字宽度。菜单在每次打开前重建，以便当前会话的最新标题、时间
    // 和选中状态立即反映出来。
    m_historyButton = new QToolButton(contextBar);
    m_historyButton->setObjectName(QStringLiteral("aiHistoryButton"));
    m_historyButton->setAccessibleName(tr("Chat history"));
    m_historyButton->setToolTip(tr("View the chat history for the current tab"));
    m_historyButton->setCursor(Qt::PointingHandCursor);
    m_historyButton->setAutoRaise(false);
    m_historyButton->setFixedSize(32, 32);
    m_historyButton->setIcon(UiIcons::symbol(this, UiIcons::Symbol::Clock, QColor(m_colors.text)));
    m_historyButton->setIconSize(QSize(17, 17));
    m_historyMenu = new QMenu(m_historyButton);
    m_historyMenu->setObjectName(QStringLiteral("aiConversationHistoryMenu"));
    m_historyButton->setMenu(m_historyMenu);
    m_historyButton->setPopupMode(QToolButton::InstantPopup);
    m_historyButton->setStyleSheet(QStringLiteral(
        "QToolButton { color: palette(text); background: transparent;"
        "border: 1px solid palette(mid); border-radius: 9px; padding: 5px; }"
        "QToolButton:hover, QToolButton:pressed { background: %1; }"
        "QToolButton::menu-indicator { image: none; width: 0; }").arg(m_colors.hover));
    contextLayout->addWidget(m_historyButton);

    m_newSessionButton = new QPushButton(tr("New session"), contextBar);
    m_newSessionButton->setObjectName(QStringLiteral("aiNewSessionButton"));
    m_newSessionButton->setCursor(Qt::PointingHandCursor);
    m_newSessionButton->setStyleSheet(QStringLiteral(
        "QPushButton { color: palette(text); background: transparent;"
        "border: 1px solid palette(mid); border-radius: 9px; padding: 2px 9px; }"
        "QPushButton:hover { background: %1; }").arg(m_colors.hover));
    contextLayout->addWidget(m_newSessionButton);
    root->addWidget(contextBar);

    // 聊天区：可滚动，消息与命令卡片自上而下堆叠，末尾始终留一个伸缩项吸收多余空间。
    m_chatScroll = new QScrollArea(this);
    m_chatScroll->setObjectName(QStringLiteral("AiChatScroll"));
    m_chatScroll->setWidgetResizable(true);
    m_chatScroll->setFrameShape(QFrame::NoFrame);
    // 内容一律换行自适应宽度，不允许横向滚动——见 insertSoftBreakpoints()，负责给长路径/
    // 长命令制造换行机会，两者配合才能真正杜绝横向滚动条。
    m_chatScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_chatContainer = new QWidget(m_chatScroll);
    m_chatContainer->setObjectName(QStringLiteral("AiChatContainer"));
    m_chatLayout = new QVBoxLayout(m_chatContainer);
    m_chatLayout->setContentsMargins(10, 12, 10, 12);
    m_chatLayout->setSpacing(14);
    m_chatLayout->addStretch(1);
    m_chatScroll->setWidget(m_chatContainer);
    root->addWidget(m_chatScroll, 1);
    // valueChanged 同时覆盖滚轮、拖动滑块和键盘滚动。用户离开底部后暂停自动跟随；重新回到
    // 底部时 m_stickyBottom 恢复为 true，后续消息继续自动贴底。程序自己的 setValue 通过
    // m_programmaticChatScroll 标记隔离，避免布局变化期间误改用户阅读意图。
    connect(m_chatScroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        QScrollBar *bar = m_chatScroll->verticalScrollBar();
        if (!m_programmaticChatScroll) {
            m_stickyBottom = bar == nullptr || value >= bar->maximum() - 2;
        }
    });
    connect(m_chatScroll->verticalScrollBar(), &QScrollBar::rangeChanged,
            this, [this](int, int maximum) {
        if (!m_stickyBottom || m_chatScroll == nullptr) {
            return;
        }
        QScrollBar *bar = m_chatScroll->verticalScrollBar();
        if (bar == nullptr || bar->value() == maximum) {
            return;
        }
        // QLabel 的 Markdown/自动换行可能在首次布局后再次修正高度，导致 maximum 连续变化。
        // 只要用户仍处于底部阅读状态，每次范围增长都贴住新底部；用户上滚时 sticky=false，
        // 此分支完全不改动其位置。
        m_programmaticChatScroll = true;
        bar->setValue(maximum);
        m_programmaticChatScroll = false;
    });

    // 效果图把输入、附件开关和发送动作合并在同一个带边框的编辑器面板中；输入框增长时整个
    // 面板一起向上扩展，附件行始终停留在底部，不会漂在编辑区外面。
    auto *composer = new QFrame(this);
    composer->setObjectName(QStringLiteral("AiComposer"));
    auto *composerLayout = new QVBoxLayout(composer);
    composerLayout->setContentsMargins(10, 7, 8, 7);
    composerLayout->setSpacing(3);

    m_inputEdit = new QTextEdit(composer);
    m_inputEdit->setObjectName(QStringLiteral("aiInputEdit"));
    m_inputEdit->setPlaceholderText(tr("Describe what you want to accomplish on the current server…"));
    m_inputEdit->setAcceptRichText(false);
    m_inputEdit->setMinimumHeight(kInputMinimumHeight);
    m_inputEdit->setMaximumHeight(kInputMinimumHeight);
    m_inputEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_inputEdit->installEventFilter(this);
    composerLayout->addWidget(m_inputEdit);

    auto *attachRow = new QWidget(composer);
    auto *attachLayout = new QHBoxLayout(attachRow);
    attachLayout->setContentsMargins(3, 0, 0, 0);
    attachLayout->setSpacing(6);
    m_attachContextCheck = new QCheckBox(tr("Include new terminal output"), attachRow);
    attachLayout->addWidget(m_attachContextCheck);
    attachLayout->addStretch(1);
    m_sendButton = new QPushButton(attachRow);
    m_sendButton->setObjectName(QStringLiteral("aiSendButton"));
    m_sendButton->setIcon(UiIcons::symbol(this, UiIcons::Symbol::ArrowUp, QColor(Qt::white)));
    m_sendButton->setFixedSize(34, 34);
    m_sendButton->setToolTip(tr("Send (Enter)"));
    m_sendButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: #159a60; border: none; border-radius: 9px; }"
        "QPushButton:disabled { background: palette(mid); }"));
    attachLayout->addWidget(m_sendButton);
    composerLayout->addWidget(attachRow);
    root->addWidget(composer);

    auto *disclaimer = new QLabel(
        tr("The AI can make mistakes. Every command that changes the system requires your explicit confirmation."), this);
    disclaimer->setAlignment(Qt::AlignCenter);
    disclaimer->setWordWrap(true);
    disclaimer->setStyleSheet(QStringLiteral("color: palette(mid); font-size: 10.5px;"));
    disclaimer->setContentsMargins(0, 0, 0, 4);
    root->addWidget(disclaimer);

    connect(m_sendButton, &QPushButton::clicked, this, [this]() {
        // 同一个按钮承担两种语义，路由必须以运行态为准而不是以图标为准：图标只是运行态
        // 的呈现，若两者判断依据不同，某次漏刷新图标就会变成"看着是停止、点下去却重发"。
        if (m_requestInFlight || m_captureInFlight) {
            stopActiveWork();
            return;
        }
        sendCurrentInput();
    });
    connect(m_newSessionButton, &QPushButton::clicked, this, &AiAssistantWidget::startNewSession);
    connect(m_historyMenu,
            &QMenu::aboutToShow,
            this,
            &AiAssistantWidget::populateConversationHistoryMenu);
    connect(m_inputEdit, &QTextEdit::textChanged, this, &AiAssistantWidget::updateInputEditorHeight);

    // 快捷键作用于 AI 助手及其全部子控件，所以焦点在输入框、聊天区或命令卡片上都能执行
    // 最后一条待处理命令。macOS 不能直接使用 MetaModifier：Qt 在该平台把物理 Command
    // 映射成 ControlModifier；复用上方统一映射可确保注册的序列与真实键盘事件完全一致。
    auto registerExecuteShortcut = [this](const QKeySequence &sequence,
                                          const QString &objectName) {
        auto *shortcut = new QShortcut(sequence, this);
        shortcut->setObjectName(objectName);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated,
                this, &AiAssistantWidget::executeLatestPendingCommand);
    };
    registerExecuteShortcut(
        QKeySequence(static_cast<int>(kCommandKeyModifier) | Qt::Key_Return),
                            QStringLiteral("aiExecuteLatestReturnShortcut"));
    // Qt 区分主键盘 Return 与数字键盘 Enter；两者都注册，用户看到的“Command+Enter”在
    // 外接全尺寸键盘和笔记本键盘上保持一致行为。
    registerExecuteShortcut(
        QKeySequence(static_cast<int>(kCommandKeyModifier) | Qt::Key_Enter),
                            QStringLiteral("aiExecuteLatestKeypadShortcut"));
    // 常驻聊天面板：语言切换时重设固定文案。上下文目标标签是动态内容（当前服务器名 /
    // 未连接），由 setActiveTarget() 维护，这里只在未连接占位时兜底重译。
    i18n::onRetranslate(this, [this, disclaimer] {
        m_newSessionButton->setText(tr("New session"));
        m_historyButton->setToolTip(tr("View the chat history for the current tab"));
        m_inputEdit->setPlaceholderText(
            tr("Describe what you want to accomplish on the current server…"));
        m_sendButton->setToolTip(tr("Send (Enter)"));
        disclaimer->setText(tr("The AI can make mistakes. Every command that changes the "
                               "system requires your explicit confirmation."));
        // 重新套用当前目标：未连接时回到本地化占位，已连接时保持服务器名不变。
        setActiveTarget(m_activeTarget);
    });

    updateInputEditorHeight();
    updateSendButtonMode();
}

bool AiAssistantWidget::eventFilter(QObject *watched, QEvent *event)
{
    // 主题切换只能从 QApplication 身上监听：本控件设置了自己的样式表，Qt 不会再把应用
    // 调色板变化当作对它有效的 PaletteChange 送进来，changeEvent() 里等不到任何事件。
    if (watched == qApp && event != nullptr
        && event->type() == QEvent::ApplicationPaletteChange) {
        applyPanelTheme();
        return false;
    }
    if (watched == m_inputEdit && event != nullptr && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const bool isEnter = keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter;
        const bool shiftPressed = keyEvent->modifiers().testFlag(Qt::ShiftModifier);
        const bool commandPressed = hasCommandKeyModifier(keyEvent);
        if (isEnter && commandPressed) {
            // 在输入框层再提供一条确定性的执行路径：部分平台/输入法会先由 QTextEdit 消费
            // Return，导致父级 QShortcut 收不到激活事件。直接消费该组合既能可靠执行，也能
            // 保证聊天草稿不会被发送或插入换行；按钮 click() 内部仍保留原有风险审批流程。
            executeLatestPendingCommand();
            return true;
        }
        if (isEnter && !shiftPressed && !commandPressed) {
            // 发送后阻止 QTextEdit 插入换行；输入为空或当前忙碌时 sendCurrentInput 会安全忽略，
            // 事件仍被消费，避免在不可发送状态下无意产生空行。
            sendCurrentInput();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AiAssistantWidget::executeLatestPendingCommand()
{
    const QList<QPushButton *> executeButtons =
        findChildren<QPushButton *>(QStringLiteral("aiExecuteButton"));
    for (auto it = executeButtons.crbegin(); it != executeButtons.crend(); ++it) {
        QPushButton *button = *it;
        if (button != nullptr && button->isVisible() && button->isEnabled()) {
            // 复用按钮 click() 路径，确保高风险审批、状态锁定、终端捕获和自动续问与鼠标操作
            // 完全一致，快捷键不会绕过任何安全判断。
            button->click();
            return;
        }
    }
    emit statusMessage(tr("There is no command waiting to run"));
}

void AiAssistantWidget::updateInputEditorHeight()
{
    if (m_inputEdit == nullptr || m_inputEdit->document() == nullptr) {
        return;
    }

    const QFontMetrics metrics(m_inputEdit->font());
    const int lineHeight = qMax(1, metrics.lineSpacing());
    const qreal documentHeight = m_inputEdit->document()->documentLayout()->documentSize().height();
    // QTextDocument 的高度包含 documentMargin，控件还需要为 frame 与 contentsMargins 留出空间。
    // 使用 ceil 防止最后一行只差小数像素时被裁掉，最大值严格对应十行可见文本。
    const int chromeHeight = 12;
    const int naturalHeight = qCeil(documentHeight) + chromeHeight;
    const int maximumHeight = kInputMaximumVisibleLines * lineHeight + chromeHeight;
    const int targetHeight = qBound(kInputMinimumHeight, naturalHeight, maximumHeight);
    m_inputEdit->setFixedHeight(targetHeight);
    m_inputEdit->setVerticalScrollBarPolicy(
        naturalHeight > maximumHeight ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
}

void AiAssistantWidget::setActiveTarget(const ConnectionTarget &target)
{
    m_activeTarget = target;
    if (m_contextTargetLabel == nullptr) {
        return;
    }
    if (!target.isValid()) {
        m_contextTargetLabel->setText(tr("No connected terminal"));
        return;
    }
    m_contextTargetLabel->setText(target.displayName.isEmpty() ? target.host : target.displayName);
}

void AiAssistantWidget::setTerminalContextProvider(std::function<QString()> provider)
{
    m_terminalContextProvider = std::move(provider);
}

void AiAssistantWidget::startNewSession()
{
    if (m_requestInFlight || m_captureInFlight) {
        emit statusMessage(tr("A request is in progress; wait before starting a new session"));
        return;
    }

    // 先把旧会话的最终上下文和终端读取位置保存到当前标签自己的最近记录中，再创建一个
    // 全新 id。空白会话由 synchronizeActiveConversation() 自动忽略，因此反复点击不会污染
    // 历史菜单。
    synchronizeActiveConversation();
    m_activeConversationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_history.clear();
    m_lastAgentRequestMessages.clear();
    m_agentProtocolRepairCount = 0;
    m_lastReadPosition = 0;
    clearChatContent();
    m_thinkingBubble = nullptr;
    m_captureCardFrame.clear();
    m_captureCardActionsRow.clear();
    m_captureWaitingLabel.clear();

    if (m_inputEdit != nullptr) {
        m_inputEdit->clear();
    }
}

void AiAssistantWidget::clearChatContent()
{
    if (m_chatLayout == nullptr) {
        return;
    }
    // 末尾伸缩项负责让短对话贴在聊天区顶部，必须保留；其余行都属于当前会话的可重建
    // 展示内容。deleteLater() 允许正在返回的按钮/菜单事件先安全结束，下一轮事件循环再释放。
    while (m_chatLayout->count() > 1) {
        QLayoutItem *item = m_chatLayout->takeAt(0);
        if (item->widget() != nullptr) {
            // 先隐藏可避免历史切换后的一个事件循环内，旧控件仍按原几何位置与新消息重叠闪烁。
            item->widget()->hide();
            item->widget()->deleteLater();
        }
        delete item;
    }
}

QString AiAssistantWidget::conversationTitle(const QList<AiChatMessage> &history) const
{
    for (const AiChatMessage &message : history) {
        if (message.role != AiChatRole::User) {
            continue;
        }
        const QString normalized = message.content.simplified();
        // 命令执行结果是隐藏观察，不是用户发起的新任务，绝不能拿它当历史标题或显示在聊天区。
        if (normalized.isEmpty() || normalized.startsWith(QStringLiteral("[命令 `"))) {
            continue;
        }
        constexpr int kMaximumTitleCharacters = 28;
        return normalized.size() > kMaximumTitleCharacters
            ? normalized.left(kMaximumTitleCharacters - 1) + QChar(0x2026)
            : normalized;
    }
    return tr("Untitled session");
}

void AiAssistantWidget::synchronizeActiveConversation()
{
    if (m_history.isEmpty()) {
        return;
    }

    ConversationSnapshot snapshot;
    snapshot.id = m_activeConversationId;
    snapshot.title = conversationTitle(m_history);
    snapshot.history = m_history;
    snapshot.lastReadPosition = m_lastReadPosition;
    snapshot.updatedAt = QDateTime::currentDateTimeUtc();

    // 先移除旧位置，再把当前项放到列表首位，列表顺序因此同时代表“最近访问”顺序。按 id
    // 更新而不是按标题更新，可让两个相同问题保持为两次独立会话。
    for (int index = 0; index < m_recentConversations.size(); ++index) {
        if (m_recentConversations.at(index).id == m_activeConversationId) {
            m_recentConversations.removeAt(index);
            break;
        }
    }
    m_recentConversations.prepend(std::move(snapshot));
    while (m_recentConversations.size() > kMaximumRecentConversations) {
        m_recentConversations.removeLast();
    }
}

void AiAssistantWidget::populateConversationHistoryMenu()
{
    if (m_historyMenu == nullptr) {
        return;
    }
    synchronizeActiveConversation();
    m_historyMenu->clear();

    if (m_recentConversations.isEmpty()) {
        QAction *emptyAction = m_historyMenu->addAction(tr("No chat history yet"));
        emptyAction->setEnabled(false);
        return;
    }

    for (const ConversationSnapshot &conversation : std::as_const(m_recentConversations)) {
        const QString localTime = conversation.updatedAt.toLocalTime().toString(
            QStringLiteral("MM-dd HH:mm"));
        QAction *action = m_historyMenu->addAction(
            tr("%1  ·  %2").arg(conversation.title, localTime));
        action->setData(conversation.id);
        action->setCheckable(true);
        action->setChecked(conversation.id == m_activeConversationId);
        action->setEnabled(!m_requestInFlight && !m_captureInFlight);
        connect(action, &QAction::triggered, this, [this, conversationId = conversation.id]() {
            loadConversation(conversationId);
        });
    }
}

void AiAssistantWidget::loadConversation(const QString &conversationId)
{
    if (m_requestInFlight || m_captureInFlight) {
        emit statusMessage(tr("The current session is still processing; wait before switching chat history"));
        return;
    }
    synchronizeActiveConversation();

    auto selected = std::find_if(
        m_recentConversations.cbegin(),
        m_recentConversations.cend(),
        [&conversationId](const ConversationSnapshot &conversation) {
            return conversation.id == conversationId;
        });
    if (selected == m_recentConversations.cend() || conversationId == m_activeConversationId) {
        return;
    }

    const ConversationSnapshot snapshot = *selected;
    m_activeConversationId = snapshot.id;
    m_history = snapshot.history;
    m_lastReadPosition = snapshot.lastReadPosition;
    m_lastAgentRequestMessages.clear();
    m_agentProtocolRepairCount = 0;
    clearChatContent();
    renderActiveConversation();
    // 加载也属于一次访问；立即同步会把选中项移到菜单首位，并更新时间，符合“最近记录”语义。
    synchronizeActiveConversation();
}

void AiAssistantWidget::renderActiveConversation()
{
    for (int index = 0; index < m_history.size(); ++index) {
        const AiChatMessage &message = m_history.at(index);
        if (message.role == AiChatRole::User) {
            // 命令输出只作为模型观察存在，恢复历史时也必须继续隐藏，避免敏感终端内容从聊天
            // 记录中重新暴露；普通用户任务则恢复成右侧气泡。
            if (!message.content.startsWith(QStringLiteral("[命令 `"))) {
                appendUserBubble(message.content);
            }
            continue;
        }
        if (message.role != AiChatRole::Assistant) {
            continue;
        }

        AgentReply reply;
        QString validationError;
        if (!parseAgentReply(message.content, &reply, &validationError)) {
            // m_history 原则上只保存已通过校验的响应；若内存因未来版本格式变化出现旧记录，
            // 安全地跳过无法确认的命令，不能把未校验文本降级成可执行卡片。
            continue;
        }
        if (reply.state == AgentReplyState::Command) {
            appendAssistantBubble(tr("%1  %2").arg(reply.title, reply.reason));
            const bool alreadyExecuted = index + 1 < m_history.size()
                && m_history.at(index + 1).role == AiChatRole::User
                && m_history.at(index + 1).content.startsWith(
                    tr("[Result of command `%1`]").arg(reply.command));
            appendCommandCard(reply.command, QString(), QString(), alreadyExecuted);
        } else {
            appendAssistantBubble(reply.message);
        }
    }
    m_stickyBottom = true;
    scrollToBottom(true);
}

void AiAssistantWidget::sendCurrentInput()
{
    if (m_requestInFlight || m_captureInFlight || m_inputEdit == nullptr) {
        return;
    }
    const QString text = m_inputEdit->toPlainText().trimmed();
    if (text.isEmpty()) {
        return;
    }
    m_inputEdit->clear();
    appendUserBubble(text);

    QString ephemeralContext;
    if (m_attachContextCheck != nullptr && m_attachContextCheck->isChecked()) {
        const QString newOutput = consumeNewTerminalOutput();
        if (!newOutput.isEmpty()) {
            ephemeralContext = tr("[New terminal output]\n%1").arg(newOutput);
        }
    }
    sendMessageInternal(text, ephemeralContext);
}

QString AiAssistantWidget::consumeNewTerminalOutput()
{
    if (!m_terminalContextProvider) {
        return QString();
    }
    const QString full = m_terminalContextProvider();
    // 位置比当前全文还长，说明终端在两次读取之间被清空或重新连接过，旧位置已经失效；
    // 从头读一次好过什么都读不到。
    const int from = qMin(m_lastReadPosition, full.length());
    const QString delta = full.mid(from);
    m_lastReadPosition = full.length();
    return truncateContext(delta).trimmed();
}

void AiAssistantWidget::sendMessageInternal(const QString &historyText,
                                            const QString &ephemeralContext)
{
    const AiSettings settings = m_settingsService->currentSettings();
    if (!settings.isConfigured()) {
        appendNoticeBubble(
            tr("Configure the LLM URL and model name in Settings → AI Assistant first"), true);
        scrollToBottom();
        return;
    }

    QList<AiChatMessage> outgoing;
    // 系统规则必须在每一次决策中存在。旧实现只在 m_history 为空的首轮添加规则，第二轮请求
    // 因历史已非空而丢掉单步约束，正是模型在执行结果回来后开始输出完整教程的根因。
    outgoing.append(AiChatMessage{AiChatRole::System, kSystemPrompt});
    outgoing += m_history;
    if (!ephemeralContext.isEmpty()) {
        outgoing.append(AiChatMessage{AiChatRole::User, ephemeralContext});
    }
    outgoing.append(AiChatMessage{AiChatRole::User, historyText});
    m_history.append(AiChatMessage{AiChatRole::User, historyText});
    // 每次长期上下文变化都同步快照，使历史菜单即使在模型返回前打开，也能看到刚提交的任务。
    synchronizeActiveConversation();
    m_lastAgentRequestMessages = outgoing;
    m_agentProtocolRepairCount = 0;

    setBusy(true);
    m_thinkingBubble = buildThinkingIndicator();
    m_chatLayout->insertWidget(m_chatLayout->count() - 1, m_thinkingBubble);
    scrollToBottom();

    m_client->sendMessage(settings, outgoing);
}

void AiAssistantWidget::handleAgentReply(const QString &content)
{
    AgentReply reply;
    QString validationError;
    if (!parseAgentReply(content, &reply, &validationError)) {
        requestAgentProtocolRepair(content, validationError);
        return;
    }

    // 只有本地验证通过的决策才进入长期历史。这样后续轮次看到的是明确的单步状态，而不是
    // 曾被拒绝的十步教程，避免违规内容继续污染模型上下文并诱发下一轮重复违规。
    m_history.append(AiChatMessage{AiChatRole::Assistant, content.trimmed()});
    synchronizeActiveConversation();
    m_agentProtocolRepairCount = 0;

    switch (reply.state) {
    case AgentReplyState::Command:
        // 效果图把智能体的判断与可审批工具卡片分成两个层级：先由助手说明“做什么、为什么”，
        // 再单独呈现命令和审批动作。这样用户即使暂不执行，也能直接理解当前一步的依据。
        appendAssistantBubble(tr("%1  %2").arg(reply.title, reply.reason));
        appendCommandCard(reply.command);
        break;
    case AgentReplyState::Complete:
    case AgentReplyState::Question:
        appendAssistantBubble(reply.message);
        break;
    }

    setBusy(false);
    scrollToBottom();
}

void AiAssistantWidget::requestAgentProtocolRepair(const QString &invalidContent,
                                                   const QString &validationError)
{
    if (m_agentProtocolRepairCount >= kMaxAgentProtocolRepairs) {
        appendNoticeBubble(
            tr("The AI did not return a result following the single-step interaction protocol; this round has been stopped. Retry or switch to a model that supports JSON instructions."),
            true);
        m_agentProtocolRepairCount = 0;
        setBusy(false);
        scrollToBottom();
        emit statusMessage(tr("The AI response does not conform to the single-step agent protocol: %1").arg(validationError));
        return;
    }

    ++m_agentProtocolRepairCount;
    QList<AiChatMessage> repairMessages = m_lastAgentRequestMessages;
    repairMessages.append(AiChatMessage{AiChatRole::Assistant, invalidContent});
    repairMessages.append(AiChatMessage{
        AiChatRole::User,
        tr("Your last response violated the single-step protocol: %1. Decide again and return only one JSON object as defined by the system message; do not output a full plan, multiple commands, or an explanation of the protocol itself.")
            .arg(validationError)});

    const AiSettings settings = m_settingsService->currentSettings();
    m_thinkingBubble = buildThinkingIndicator();
    m_chatLayout->insertWidget(m_chatLayout->count() - 1, m_thinkingBubble);
    scrollToBottom();
    m_client->sendMessage(settings, repairMessages);
}

void AiAssistantWidget::appendUserBubble(const QString &text)
{
    auto *row = new QWidget(m_chatContainer);
    row->setObjectName(QStringLiteral("AiUserMessageRow"));
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(0);

    // 用户消息固定采用“左侧 30% 留白 + 右侧 70% 气泡”的响应式比例。旧实现只限制最大
    // 宽度为 680px，QLabel 仍会按短文本的自然宽度收缩，导致一句话气泡仅占聊天区约三成；
    // 通过布局伸缩因子分配可用空间后，短消息和长消息都会保持约 70% 宽度，并能随侧栏或
    // 浮动窗口缩放，不依赖某个特定屏幕尺寸。这里不再保留固定 52px 左边距，否则它会先
    // 扣减可用空间，再计算 3:7，最终气泡占整个聊天区的比例会明显低于 70%。
    rowLayout->addStretch(3);

    auto *bubble = new QLabel(text, row);
    bubble->setObjectName(QStringLiteral("AiUserMessageBubble"));
    bubble->setWordWrap(true);
    bubble->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Expanding 是比例布局生效的必要条件：若继续使用 QLabel 默认的 Preferred 策略，短文本
    // 会优先停留在 sizeHint() 宽度，右侧 70% 的布局份额无法真正填满。
    bubble->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bubble->setStyleSheet(QStringLiteral(
        "color: %1; background: %2; border: 1px solid %3;"
        "border-radius: 10px; padding: 9px 13px; font-size: 14px; font-weight: 600;")
        .arg(m_colors.text, m_colors.userBubble, m_colors.userBubbleBorder));
    rowLayout->addWidget(bubble, 7);
    m_chatLayout->insertWidget(m_chatLayout->count() - 1, row);
    // 默认让新消息贴底；但用户已明确上滚查看历史时继续尊重该位置。只有用户把滚动条重新
    // 拉到底部，valueChanged 才会恢复自动跟随，完全符合效果图这类长对话的阅读习惯。
    scrollToBottom();
}

QWidget *AiAssistantWidget::buildThinkingIndicator()
{
    auto *row = new QWidget(m_chatContainer);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(6);

    auto *icon = new QLabel(row);
    icon->setFixedSize(30, 30);
    icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(UiIcons::renderSymbolPixmap(
        UiIcons::Symbol::Sparkle, QColor(m_colors.accent), QSize(18, 18),
        devicePixelRatioF()));
    icon->setStyleSheet(
        QStringLiteral("background: %1; border-radius: 8px;").arg(m_colors.avatarTint));
    rowLayout->addWidget(icon);
    auto *text = new QLabel(tr("Analyzing…"), row);
    text->setStyleSheet(QStringLiteral("color: palette(mid); font-style: italic;"));
    rowLayout->addWidget(text);
    rowLayout->addStretch(1);

    // 图标透明度循环呼吸，给用户"模型正在动"的直观反馈——比一句静态文案更能传达"没卡住"。
    auto *effect = new QGraphicsOpacityEffect(icon);
    icon->setGraphicsEffect(effect);
    auto *animation = new QPropertyAnimation(effect, "opacity", row);
    animation->setStartValue(1.0);
    animation->setKeyValueAt(0.5, 0.25);
    animation->setEndValue(1.0);
    animation->setDuration(1100);
    animation->setLoopCount(-1);
    animation->start();

    return row;
}

void AiAssistantWidget::appendAssistantBubble(const QString &text)
{
    if (text.trimmed().isEmpty()) {
        return;
    }
    // 参考设计里助手的解说文字是头像+纯文本，没有气泡边框——边框留给下面真正的工具调用
    // 卡片，两者要能一眼区分开：这一段是"助手在说话"，卡片才是"发生了一次操作"。
    auto *row = new QWidget(m_chatContainer);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(10);
    auto *avatar = new QLabel(row);
    avatar->setFixedSize(30, 30);
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setPixmap(UiIcons::renderSymbolPixmap(
        UiIcons::Symbol::Sparkle, QColor(m_colors.accent), QSize(18, 18),
        devicePixelRatioF()));
    avatar->setStyleSheet(
        QStringLiteral("background: %1; border-radius: 8px;").arg(m_colors.avatarTint));
    rowLayout->addWidget(avatar, 0, Qt::AlignTop);

    auto *bubble = new QLabel(row);
    // 模型回复本来就是 Markdown（表格、加粗、列表都很常见），按纯文本显示会把 **、| 这些
    // 语法符号原样甩给用户；QLabel 从 Qt 5.14 起能直接解析 Markdown，不需要额外依赖。
    bubble->setTextFormat(Qt::MarkdownText);
    bubble->setText(text);
    bubble->setWordWrap(true);
    bubble->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    bubble->setOpenExternalLinks(true);
    bubble->setStyleSheet(QStringLiteral(
        "color: %1; background: transparent; font-size: 14px;").arg(m_colors.text));
    rowLayout->addWidget(bubble, 1);

    m_chatLayout->insertWidget(m_chatLayout->count() - 1, row);
}

void AiAssistantWidget::appendNoticeBubble(const QString &text, const bool isError)
{
    auto *bubble = new QLabel(text, m_chatContainer);
    bubble->setWordWrap(true);
    const QString color = isError ? m_colors.danger : m_colors.warning;
    bubble->setStyleSheet(QStringLiteral(
        "color: %1; background: %2; border-left: 3px solid %1;"
        "border-radius: 6px; padding: 7px 10px;").arg(color, m_colors.overlay));
    m_chatLayout->insertWidget(m_chatLayout->count() - 1, bubble);
}

void AiAssistantWidget::appendCommandCard(const QString &command,
                                          const QString &title,
                                          const QString &reason,
                                          const bool alreadyExecuted)
{
    const CommandRiskLevel level = CommandRiskClassifier::classify(command);
    const QColor accent = riskAccentColor(level, m_colors.dark);

    auto *cardRow = new QWidget(m_chatContainer);
    auto *cardRowLayout = new QHBoxLayout(cardRow);
    cardRowLayout->setContentsMargins(40, 0, 0, 0);
    cardRowLayout->setSpacing(0);

    auto *card = new QFrame(cardRow);
    card->setObjectName(QStringLiteral("AiCommandCard"));
    card->setFrameShape(QFrame::StyledPanel);
    card->setStyleSheet(QStringLiteral(
        "QFrame#AiCommandCard { border: 1px solid %1; border-radius: 10px; background: %2; }")
        .arg(accent.name(), m_colors.surface));
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    auto *header = new QWidget(card);
    header->setStyleSheet(QStringLiteral("background: %1; border-top-left-radius: 9px;"
                                         "border-top-right-radius: 9px;")
        .arg(QColor(accent.red(), accent.green(), accent.blue(), 36).name(QColor::HexArgb)));
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(11, 7, 11, 7);
    auto *statusLabel = new QLabel(tr("Awaiting confirmation"), header);
    QFont statusFont = statusLabel->font();
    statusFont.setBold(true);
    statusLabel->setFont(statusFont);
    headerLayout->addWidget(statusLabel);
    headerLayout->addStretch(1);
    auto *badge = new QLabel(CommandRiskClassifier::label(level), header);
    QFont badgeFont = badge->font();
    badgeFont.setBold(true);
    badgeFont.setPointSizeF(badgeFont.pointSizeF() - 0.5);
    badge->setFont(badgeFont);
    badge->setStyleSheet(QStringLiteral("color: %1;").arg(accent.name()));
    headerLayout->addWidget(badge);
    cardLayout->addWidget(header);

    if (!title.trimmed().isEmpty() || !reason.trimmed().isEmpty()) {
        auto *explanation = new QWidget(card);
        auto *explanationLayout = new QVBoxLayout(explanation);
        explanationLayout->setContentsMargins(11, 9, 11, 7);
        explanationLayout->setSpacing(4);

        if (!title.trimmed().isEmpty()) {
            auto *titleLabel = new QLabel(title.trimmed(), explanation);
            titleLabel->setWordWrap(true);
            QFont explanationTitleFont = titleLabel->font();
            explanationTitleFont.setBold(true);
            titleLabel->setFont(explanationTitleFont);
            titleLabel->setStyleSheet(QStringLiteral(
                "color: palette(text); background: transparent; border: 0;"));
            explanationLayout->addWidget(titleLabel);
        }
        if (!reason.trimmed().isEmpty()) {
            auto *reasonLabel = new QLabel(reason.trimmed(), explanation);
            reasonLabel->setWordWrap(true);
            reasonLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            reasonLabel->setStyleSheet(QStringLiteral(
                "color: palette(mid); background: transparent; border: 0;"));
            explanationLayout->addWidget(reasonLabel);
        }
        cardLayout->addWidget(explanation);
    }

    auto *commandLabel = new QLabel(insertSoftBreakpoints(command), card);
    commandLabel->setWordWrap(true);
    commandLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont monoFont(QStringLiteral("Menlo"));
    monoFont.setStyleHint(QFont::Monospace);
    commandLabel->setFont(monoFont);
    commandLabel->setStyleSheet(QStringLiteral(
        "color: %1; background: %2; border: 0; font-size: 13px;")
        .arg(m_colors.codeText, m_colors.codeBackground));
    commandLabel->setContentsMargins(12, 10, 12, 10);
    cardLayout->addWidget(commandLabel);

    auto *actions = new QWidget(card);
    auto *actionsLayout = new QHBoxLayout(actions);
    actionsLayout->setContentsMargins(8, 5, 8, 7);
    actionsLayout->setSpacing(5);
    auto *insertButton = new QPushButton(tr("Insert"), actions);
    auto *copyButton = new QPushButton(tr("Copy"), actions);
    auto *modifyButton = new QPushButton(tr("Edit"), actions);
    auto *executeButton = new QPushButton(tr("Run  ⌘↵"), actions);
    insertButton->setObjectName(QStringLiteral("aiInsertButton"));
    copyButton->setObjectName(QStringLiteral("aiCopyButton"));
    modifyButton->setObjectName(QStringLiteral("aiModifyButton"));
    executeButton->setObjectName(QStringLiteral("aiExecuteButton"));
    executeButton->setToolTip(tr("Run the last command awaiting confirmation (Command+Enter)"));
    for (QPushButton *button : {insertButton, copyButton, modifyButton, executeButton}) {
        button->setProperty("aiCompactAction", true);
        button->setCursor(Qt::PointingHandCursor);
    }
    executeButton->setProperty("aiPrimaryAction", true);
    if (alreadyExecuted) {
        // 从历史记录恢复的已执行命令只能保留查看、复制和再次插入能力，不能重新出现为待确认
        // 状态；否则用户可能把一次旧操作误认为当前智能体刚提出的新步骤并重复执行。
        statusLabel->setText(tr("Executed"));
        executeButton->setText(tr("Executed"));
        executeButton->setEnabled(false);
    }
    actionsLayout->addWidget(insertButton);
    actionsLayout->addWidget(copyButton);
    actionsLayout->addWidget(modifyButton);
    actionsLayout->addStretch(1);
    actionsLayout->addWidget(executeButton);
    cardLayout->addWidget(actions);

    connect(insertButton, &QPushButton::clicked, this, [this, command]() {
        emit commandInsertionRequested(command, QStringList{});
    });
    connect(copyButton, &QPushButton::clicked, this, [this, command]() {
        QGuiApplication::clipboard()->setText(command);
        // 使用主窗口现有的非阻塞提示区反馈复制结果，不弹模态 QMessageBox 打断连续操作。
        emit statusMessage(tr("Command copied to clipboard"));
    });
    connect(modifyButton, &QPushButton::clicked, this, [this, command]() {
        bool accepted = false;
        const QString editedCommand = QInputDialog::getText(
            this,
            tr("Edit command"),
            tr("After confirmation it is inserted into the current terminal and not run automatically:"),
            QLineEdit::Normal,
            command,
            &accepted).trimmed();
        if (!accepted || editedCommand.isEmpty()) {
            return;
        }
        if (editedCommand.contains(QLatin1Char('\n')) || editedCommand.contains(QLatin1Char('\r'))) {
            appendNoticeBubble(tr("Only a single-line command can be inserted into the terminal"), true);
            return;
        }
        emit commandInsertionRequested(editedCommand, QStringList{});
    });
    connect(executeButton, &QPushButton::clicked, this,
            [this, command, level, executeButton, statusLabel, card, actions]() {
        if (m_captureInFlight) {
            emit statusMessage(tr("The previous command is still waiting for output; please wait before running another"));
            return;
        }
        if (level == CommandRiskLevel::HighRisk) {
            const auto choice = QMessageBox::warning(
                this,
                tr("Confirm running a high-risk command"),
                tr("This command may have irreversible or wide-ranging effects:\n\n%1\n\nRun it in the current terminal?")
                    .arg(command),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel);
            if (choice != QMessageBox::Yes) {
                return;
            }
        }
        executeButton->setEnabled(false);
        executeButton->setText(tr("Executed"));
        statusLabel->setText(tr("Executed"));
        // 必须先拍下终端基线再下发命令。executeOnTargets 是同步信号槽，极快命令可能在 emit
        // 返回前就已经产生回显；若下发后才记录位置，这部分真实输出会被误当成旧内容丢弃。
        beginOutputCapture(command, card, actions);
        emit commandExecutionRequested(command, QStringList{});
        emit statusMessage(tr("Sent to the current terminal to run; the AI will wait for it to finish automatically"));
    });

    cardRowLayout->addWidget(card, 1);
    m_chatLayout->insertWidget(m_chatLayout->count() - 1, cardRow);
}

void AiAssistantWidget::setBusy(const bool busy)
{
    m_requestInFlight = busy;
    updateSendButtonMode();
}

void AiAssistantWidget::updateSendButtonMode()
{
    if (m_sendButton == nullptr) {
        return;
    }
    const bool stoppable = m_requestInFlight || m_captureInFlight;
    m_sendButton->setEnabled(true);
    m_sendButton->setProperty("aiButtonMode",
                              stoppable ? QStringLiteral("stop") : QStringLiteral("send"));
    m_sendButton->setIcon(UiIcons::symbol(this,
                                          stoppable ? UiIcons::Symbol::Stop
                                                    : UiIcons::Symbol::ArrowUp,
                                          QColor(Qt::white)));
    m_sendButton->setToolTip(stoppable ? tr("Stop") : tr("Send (Enter)"));
    m_sendButton->setStyleSheet(
        stoppable
            ? QStringLiteral(
                  "QPushButton { background: #b42318; border: none; border-radius: 9px; }")
            : QStringLiteral(
                  "QPushButton { background: #159a60; border: none; border-radius: 9px; }"
                  "QPushButton:disabled { background: palette(mid); }"));
}

void AiAssistantWidget::stopActiveWork()
{
    if (m_requestInFlight) {
        // 先落 UI 状态再中止：AiClient 在用户主动中止后不会再发任何信号，界面收尾只能
        // 由这里完成。
        m_client->abortActiveRequest();
        if (m_thinkingBubble != nullptr) {
            m_thinkingBubble->deleteLater();
            m_thinkingBubble = nullptr;
        }
        m_agentProtocolRepairCount = 0;
        setBusy(false);
        appendNoticeBubble(tr("This analysis has been stopped. Add more detail and send again."), false);
        emit statusMessage(tr("AI analysis stopped"));
        scrollToBottom();
        return;
    }
    if (m_captureInFlight) {
        cancelOutputCapture();
    }
}

void AiAssistantWidget::cancelOutputCapture()
{
    if (!m_captureInFlight) {
        return;
    }
    if (m_captureTimer != nullptr) {
        m_captureTimer->stop();
    }
    m_captureInFlight = false;
    if (!m_captureWaitingLabel.isNull()) {
        m_captureWaitingLabel->deleteLater();
    }
    m_captureCardFrame.clear();
    m_captureCardActionsRow.clear();
    m_captureWaitingLabel.clear();
    // 读取位置保持在停止的这一刻，不回退：这段输出用户已经能在终端里看到，下一次请求
    // 若把它重新灌给模型，等于把用户刚刚放弃的那一轮又拉回上下文。
    updateSendButtonMode();
    appendNoticeBubble(
        tr("Stopped waiting for the command result; the command is still running in the terminal."), false);
    emit statusMessage(tr("Stopped waiting for the command result"));
    scrollToBottom();
}

void AiAssistantWidget::beginOutputCapture(const QString &command, QFrame *card, QWidget *actionsRow)
{
    if (m_captureInFlight) {
        return;
    }
    if (!m_terminalContextProvider) {
        // 没有可读的终端快照时无法安全判断完成，也不能要求用户手工复制。显示明确状态，
        // 保持命令已下发的事实，同时停止自动链路，避免伪造执行结果。
        appendNoticeBubble(tr("Could not read the current terminal state; cannot wait for the command to finish automatically."), true);
        return;
    }

    m_captureCommand = command;
    m_captureCardFrame = card;
    m_captureCardActionsRow = actionsRow;
    m_captureLastText = m_terminalContextProvider();
    m_captureStartLength = m_captureLastText.length();
    m_captureBaselinePrompt = lastVisibleTerminalLine(m_captureLastText);
    // 把读取位置推进到命令下发前的这一刻：命令的输出从这里才真正开始，之前的旧内容
    // （如果还没被读过）不属于"这条命令的执行结果"，不应该被算进接下来的增量里。
    m_lastReadPosition = m_captureStartLength;
    m_captureEverChanged = false;
    m_captureLastReportedSecond = -1;
    m_captureInFlight = true;
    m_captureElapsed.start();
    updateSendButtonMode();

    // 等待状态直接插进命令卡片并动态显示耗时。命令完成后只移除状态，不把终端输出替换
    // 进来；用户在聊天区看到的是流程状态，原始日志仍只存在于终端。
    if (card != nullptr && actionsRow != nullptr) {
        auto *cardLayout = qobject_cast<QVBoxLayout *>(card->layout());
        if (cardLayout != nullptr) {
            auto *waitingLabel = new QLabel(tr("Waiting for the command to finish…"), card);
            waitingLabel->setObjectName(QStringLiteral("aiCommandWaitingLabel"));
            waitingLabel->setStyleSheet(
                QStringLiteral("color: %1; font-style: italic; padding: 5px 11px;")
                    .arg(m_colors.warning));
            const int actionsIndex = cardLayout->indexOf(actionsRow);
            cardLayout->insertWidget(actionsIndex < 0 ? cardLayout->count() : actionsIndex,
                                     waitingLabel);
            m_captureWaitingLabel = waitingLabel;
        }
    }
    scrollToBottom();

    if (m_captureTimer == nullptr) {
        m_captureTimer = new QTimer(this);
        m_captureTimer->setInterval(kCapturePollIntervalMs);
        connect(m_captureTimer, &QTimer::timeout, this, &AiAssistantWidget::pollCaptureTick);
    }
    m_captureTimer->start();
}

void AiAssistantWidget::pollCaptureTick()
{
    if (!m_captureInFlight || !m_terminalContextProvider) {
        if (m_captureTimer != nullptr) {
            m_captureTimer->stop();
        }
        return;
    }

    const QString current = m_terminalContextProvider();
    if (current != m_captureLastText || current.length() != m_captureStartLength) {
        m_captureEverChanged = true;
    }
    m_captureLastText = current;

    const int elapsedSeconds = qMax(0, static_cast<int>(m_captureElapsed.elapsed() / 1000));
    if (!m_captureWaitingLabel.isNull() && elapsedSeconds != m_captureLastReportedSecond) {
        m_captureLastReportedSecond = elapsedSeconds;
        // 等待状态只报"还在等、等了多久"这一件事。附带的操作说明每秒重绘一次，却在整个
        // 等待期间一字不变，读一遍之后就只是把命令卡片撑高、把上一轮回答挤出视野。
        m_captureWaitingLabel->setText(
            tr("Waiting for the command to finish… waited %1 s").arg(elapsedSeconds));
    }

    // 必须先观察到终端变化，再接受提示符回归；否则命令下发与第一次轮询之间的短暂窗口里，
    // 旧提示符仍在屏幕末行，会被误当成“瞬间完成”。
    if (m_captureEverChanged && shellPromptHasReturned(current, m_captureBaselinePrompt)) {
        finishOutputCapture();
    }
}

void AiAssistantWidget::finishOutputCapture()
{
    if (m_captureTimer != nullptr) {
        m_captureTimer->stop();
    }
    m_captureInFlight = false;
    updateSendButtonMode();

    // 终端差量只作为智能体观察值。这里刻意不创建 QLabel、不改命令卡片正文，避免输出
    // 在终端和聊天区重复出现，也避免密码、令牌、数据库结果等内容被长期留在对话界面。
    QString output = consumeNewTerminalOutput();
    if (output.isEmpty()) {
        output = tr("(no output)");
    }

    if (!m_captureWaitingLabel.isNull()) {
        m_captureWaitingLabel->deleteLater();
    }
    m_captureCardFrame.clear();
    m_captureCardActionsRow.clear();
    m_captureWaitingLabel.clear();

    const QString marker = tr("[Result of command `%1`]\n%2").arg(m_captureCommand, output);
    sendMessageInternal(marker);
}

void AiAssistantWidget::scrollToBottom(const bool force)
{
    if (m_chatScroll == nullptr) {
        return;
    }
    if (!force && !m_stickyBottom) {
        // 用户此前主动往上翻查看历史，新内容不该把视口硬拽回底部。
        return;
    }
    if (force) {
        // 用户主动发送消息属于明确的“回到当前会话”动作，应重新启用自动跟随；之后若再次
        // 手动上滚，valueChanged 会立即把 m_stickyBottom 关闭。
        m_stickyBottom = true;
    }
    // 布局在新增子控件后要等下一次事件循环才会重新计算滚动范围，同步 setValue 会拿到
    // 上一次的 maximum，看起来像是"差一条消息才滚到底"。
    QPointer<AiAssistantWidget> owner(this);
    QPointer<QScrollArea> scroll(m_chatScroll);
    QTimer::singleShot(0, this, [owner, scroll]() {
        if (owner.isNull() || scroll.isNull()) {
            return;
        }
        QScrollBar *bar = scroll->verticalScrollBar();
        if (bar != nullptr) {
            owner->m_programmaticChatScroll = true;
            bar->setValue(bar->maximum());
            owner->m_programmaticChatScroll = false;
        }
    });
}
