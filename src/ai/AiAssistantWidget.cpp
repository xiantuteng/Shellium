#include "ai/AiAssistantWidget.h"

#include "ai/AiClient.h"
#include "ai/AiSettings.h"
#include "ai/AiSettingsService.h"
#include "ai/CommandRiskClassifier.h"
#include "ui/UiIcons.h"

#include <QCheckBox>
#include <QFrame>
#include <QGuiApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

namespace {

const QString kSystemPrompt = QStringLiteral(
    "你是嵌入终端客户端 Shellium 的运维助手，通过用户当前连接的服务器帮助排查问题、执行"
    "命令。这是一个单步交互流程：你每次回复最多给出一条命令，放进单独一个 Markdown 围栏代码"
    "块（```bash 起，``` 止），代码块内只能有一行命令；不要一次给出多条命令或多个代码块，也"
    "不要在代码块外重复它。用户点击“执行”后，客户端会把这条命令在当前终端真实执行，并把新增"
    "的终端输出整理成一条以“[命令 ... 的执行结果]”开头的消息自动发给你——这是真实结果，不是"
    "你的猜测，必须据此判断下一步，不要假设某条命令已经生效或凭空编造它的输出。如果任务还没"
    "完成，就只给出下一条命令；如果已经完成，或者需要用户先做出选择，就用简洁的中文说明，不"
    "要包含代码块。除代码块外请使用简洁的中文说明。");

/** @brief 捕获轮询周期；太短会让 QPlainTextEdit 全文导出成为明显开销，太长会拖慢续问节奏。 */
constexpr int kCapturePollIntervalMs = 300;
/** @brief 快照相对基线首次变化后，需要连续这么久没有新变化才认为命令已经跑完。 */
constexpr int kCaptureIdleMs = 900;
/** @brief 硬性超时：命令可能挂起或持续输出（如 tail -f），到点后仍把已捕获内容续问出去。 */
constexpr int kCaptureMaxWaitMs = 30000;

/** @brief 拆出的一段回复：narrative 为解说文字，command 非空时表示这是一条建议命令。 */
struct ReplySegment {
    QString narrative;
    QString command;
};

QList<ReplySegment> splitReplyIntoSegments(const QString &content)
{
    static const QRegularExpression codeFence(
        QStringLiteral("```[a-zA-Z0-9_+-]*\\n([\\s\\S]*?)```"));

    QList<ReplySegment> segments;
    int cursor = 0;
    QRegularExpressionMatchIterator it = codeFence.globalMatch(content);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString narrative = content.mid(cursor, match.capturedStart() - cursor).trimmed();
        if (!narrative.isEmpty()) {
            segments.append(ReplySegment{narrative, QString()});
        }
        const QString block = match.captured(1);
        for (const QString &line : block.split(QLatin1Char('\n'))) {
            const QString command = line.trimmed();
            if (!command.isEmpty()) {
                segments.append(ReplySegment{QString(), command});
            }
        }
        cursor = match.capturedEnd();
    }
    const QString trailing = content.mid(cursor).trimmed();
    if (!trailing.isEmpty()) {
        segments.append(ReplySegment{trailing, QString()});
    }
    if (segments.isEmpty() && !content.trimmed().isEmpty()) {
        segments.append(ReplySegment{content.trimmed(), QString()});
    }
    return segments;
}

/** @brief 风险分级对应的强调色，与效果图集使用的语义色保持一致。 */
QColor riskAccentColor(const CommandRiskLevel level)
{
    switch (level) {
    case CommandRiskLevel::ReadOnly:
        return QColor(QStringLiteral("#159a60"));
    case CommandRiskLevel::Write:
        return QColor(QStringLiteral("#3b78c9"));
    case CommandRiskLevel::HighRisk:
        return QColor(QStringLiteral("#d84a41"));
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

} // namespace

AiAssistantWidget::AiAssistantWidget(AiSettingsService *settingsService, QWidget *parent)
    : QWidget(parent)
    , m_settingsService(settingsService)
    , m_client(new AiClient(this))
{
    Q_ASSERT(m_settingsService != nullptr);
    buildUi();

    connect(m_client, &AiClient::replyFinished, this, [this](const QString &content) {
        if (m_thinkingBubble != nullptr) {
            m_thinkingBubble->deleteLater();
            m_thinkingBubble = nullptr;
        }
        m_history.append(AiChatMessage{AiChatRole::Assistant, content});
        renderAssistantReply(content);
        setBusy(false);
        scrollToBottom();
    });
    connect(m_client, &AiClient::replyFailed, this, [this](const QString &message) {
        if (m_thinkingBubble != nullptr) {
            m_thinkingBubble->deleteLater();
            m_thinkingBubble = nullptr;
        }
        appendNoticeBubble(message, true);
        setBusy(false);
        scrollToBottom();
        emit statusMessage(message);
    });
}

void AiAssistantWidget::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // 上下文条：服务器 + “最小上下文”提示 + 新会话入口，呼应设计稿里的三段式徽标。
    auto *contextBar = new QWidget(this);
    contextBar->setObjectName(QStringLiteral("AiContextBar"));
    auto *contextLayout = new QHBoxLayout(contextBar);
    contextLayout->setContentsMargins(8, 6, 8, 6);
    contextLayout->setSpacing(6);
    auto *serverIcon = new QLabel(contextBar);
    serverIcon->setPixmap(UiIcons::renderSymbolPixmap(
        UiIcons::Symbol::HostItem, palette().color(QPalette::WindowText), QSize(16, 16), devicePixelRatioF()));
    contextLayout->addWidget(serverIcon);
    auto *contextCaption = new QLabel(tr("当前服务器"), contextBar);
    contextCaption->setStyleSheet(QStringLiteral("color: palette(mid);"));
    contextLayout->addWidget(contextCaption);
    m_contextTargetLabel = new QLabel(tr("未连接终端"), contextBar);
    QFont targetFont = m_contextTargetLabel->font();
    targetFont.setBold(true);
    m_contextTargetLabel->setFont(targetFont);
    m_contextTargetLabel->setWordWrap(true);
    contextLayout->addWidget(m_contextTargetLabel, 1);
    auto *minContextChip = new QLabel(tr("🛡 最小上下文"), contextBar);
    minContextChip->setStyleSheet(QStringLiteral(
        "color: #159a60; background: rgba(21,154,96,0.14); border-radius: 9px; padding: 2px 8px;"));
    contextLayout->addWidget(minContextChip);
    m_newSessionButton = new QPushButton(tr("新会话"), contextBar);
    m_newSessionButton->setObjectName(QStringLiteral("aiNewSessionButton"));
    m_newSessionButton->setCursor(Qt::PointingHandCursor);
    m_newSessionButton->setStyleSheet(QStringLiteral(
        "QPushButton { color: palette(text); background: transparent; border: 1px solid palette(mid);"
        "border-radius: 9px; padding: 2px 9px; }"
        "QPushButton:hover { background: rgba(0,0,0,0.06); }"));
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
    m_chatLayout = new QVBoxLayout(m_chatContainer);
    m_chatLayout->setContentsMargins(8, 8, 8, 8);
    m_chatLayout->setSpacing(8);
    m_chatLayout->addStretch(1);
    m_chatScroll->setWidget(m_chatContainer);
    root->addWidget(m_chatScroll, 1);

    // 附加终端上下文的勾选行，紧贴输入框上方。
    auto *attachRow = new QWidget(this);
    auto *attachLayout = new QHBoxLayout(attachRow);
    attachLayout->setContentsMargins(8, 0, 8, 4);
    attachLayout->setSpacing(6);
    auto *paperclip = new QLabel(attachRow);
    paperclip->setPixmap(UiIcons::renderSymbolPixmap(
        UiIcons::Symbol::Paperclip, palette().color(QPalette::WindowText).lighter(140),
        QSize(13, 13), devicePixelRatioF()));
    attachLayout->addWidget(paperclip);
    m_attachContextCheck = new QCheckBox(tr("可添加选中的终端输出"), attachRow);
    attachLayout->addWidget(m_attachContextCheck);
    attachLayout->addStretch(1);
    root->addWidget(attachRow);

    // 输入区。
    auto *inputRow = new QWidget(this);
    auto *inputLayout = new QHBoxLayout(inputRow);
    inputLayout->setContentsMargins(8, 0, 8, 6);
    inputLayout->setSpacing(8);
    m_inputEdit = new QTextEdit(inputRow);
    m_inputEdit->setObjectName(QStringLiteral("aiInputEdit"));
    m_inputEdit->setPlaceholderText(tr("描述你希望在当前服务器完成的任务…"));
    m_inputEdit->setFixedHeight(64);
    m_inputEdit->setAcceptRichText(false);
    inputLayout->addWidget(m_inputEdit, 1);
    m_sendButton = new QPushButton(inputRow);
    m_sendButton->setObjectName(QStringLiteral("aiSendButton"));
    m_sendButton->setIcon(UiIcons::symbol(this, UiIcons::Symbol::ArrowUp, QColor(Qt::white)));
    m_sendButton->setFixedSize(36, 36);
    m_sendButton->setToolTip(tr("发送"));
    m_sendButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: #159a60; border: none; border-radius: 9px; }"
        "QPushButton:disabled { background: palette(mid); }"));
    inputLayout->addWidget(m_sendButton, 0, Qt::AlignBottom);
    root->addWidget(inputRow);

    auto *disclaimer = new QLabel(
        tr("AI 可能出错。所有修改系统的命令都需要你明确确认。"), this);
    disclaimer->setAlignment(Qt::AlignCenter);
    disclaimer->setWordWrap(true);
    disclaimer->setStyleSheet(QStringLiteral("color: palette(mid); font-size: 10.5px;"));
    disclaimer->setContentsMargins(4, 0, 4, 6);
    root->addWidget(disclaimer);

    connect(m_sendButton, &QPushButton::clicked, this, &AiAssistantWidget::sendCurrentInput);
    connect(m_newSessionButton, &QPushButton::clicked, this, &AiAssistantWidget::startNewSession);
}

void AiAssistantWidget::setActiveTarget(const ConnectionTarget &target)
{
    m_activeTarget = target;
    if (m_contextTargetLabel == nullptr) {
        return;
    }
    if (!target.isValid()) {
        m_contextTargetLabel->setText(tr("未连接终端"));
        return;
    }
    m_contextTargetLabel->setText(
        target.displayName.isEmpty() ? target.host : target.displayName);
}

void AiAssistantWidget::setTerminalContextProvider(std::function<QString()> provider)
{
    m_terminalContextProvider = std::move(provider);
}

void AiAssistantWidget::startNewSession()
{
    if (m_requestInFlight || m_captureInFlight) {
        emit statusMessage(tr("有请求正在进行，请稍候再开启新会话"));
        return;
    }

    m_history.clear();
    m_lastReadPosition = 0;

    if (m_chatLayout != nullptr) {
        // 末尾那个伸缩项要留着吸收多余空间，其余全部清空——从头数第 0 项开始删，删到只剩
        // 伸缩项为止。
        while (m_chatLayout->count() > 1) {
            QLayoutItem *item = m_chatLayout->takeAt(0);
            if (item->widget() != nullptr) {
                item->widget()->deleteLater();
            }
            delete item;
        }
    }
    m_thinkingBubble = nullptr;
    m_captureNoticeBubble = nullptr;

    if (m_inputEdit != nullptr) {
        m_inputEdit->clear();
    }
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
            ephemeralContext = tr("[终端新增输出]\n%1").arg(newOutput);
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
            tr("请先在设置 → AI 助手中配置大模型地址和模型名称"), true);
        scrollToBottom();
        return;
    }

    QList<AiChatMessage> outgoing;
    if (m_history.isEmpty()) {
        outgoing.append(AiChatMessage{AiChatRole::System, kSystemPrompt});
    }
    outgoing += m_history;
    if (!ephemeralContext.isEmpty()) {
        outgoing.append(AiChatMessage{AiChatRole::User, ephemeralContext});
    }
    outgoing.append(AiChatMessage{AiChatRole::User, historyText});
    m_history.append(AiChatMessage{AiChatRole::User, historyText});

    setBusy(true);
    m_thinkingBubble = new QLabel(tr("AI 正在处理…"), m_chatContainer);
    m_thinkingBubble->setStyleSheet(QStringLiteral("color: palette(mid); font-style: italic;"));
    m_chatLayout->insertWidget(m_chatLayout->count() - 1, m_thinkingBubble);
    scrollToBottom();

    m_client->sendMessage(settings, outgoing);
}

void AiAssistantWidget::appendUserBubble(const QString &text)
{
    auto *bubble = new QLabel(text, m_chatContainer);
    bubble->setWordWrap(true);
    bubble->setStyleSheet(QStringLiteral(
        "background: rgba(21,154,96,0.14); border: 1px solid rgba(21,154,96,0.35);"
        "border-radius: 10px; padding: 8px 11px;"));
    m_chatLayout->insertWidget(m_chatLayout->count() - 1, bubble);
    scrollToBottom();
}

void AiAssistantWidget::appendAssistantBubble(const QString &text)
{
    if (text.trimmed().isEmpty()) {
        return;
    }
    auto *bubble = new QLabel(m_chatContainer);
    // 模型回复本来就是 Markdown（表格、加粗、列表都很常见），按纯文本显示会把 **、| 这些
    // 语法符号原样甩给用户；QLabel 从 Qt 5.14 起能直接解析 Markdown，不需要额外依赖。
    bubble->setTextFormat(Qt::MarkdownText);
    bubble->setText(text);
    bubble->setWordWrap(true);
    bubble->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    bubble->setOpenExternalLinks(true);
    bubble->setStyleSheet(QStringLiteral(
        "background: palette(base); border: 1px solid palette(mid);"
        "border-radius: 10px; padding: 8px 11px;"));
    m_chatLayout->insertWidget(m_chatLayout->count() - 1, bubble);
}

void AiAssistantWidget::appendNoticeBubble(const QString &text, const bool isError)
{
    auto *bubble = new QLabel(text, m_chatContainer);
    bubble->setWordWrap(true);
    const QString color = isError ? QStringLiteral("#d84a41") : QStringLiteral("#b8791f");
    bubble->setStyleSheet(QStringLiteral(
        "color: %1; background: rgba(0,0,0,0.04); border-left: 3px solid %1;"
        "border-radius: 6px; padding: 7px 10px;").arg(color));
    m_chatLayout->insertWidget(m_chatLayout->count() - 1, bubble);
}

void AiAssistantWidget::appendCommandCard(const QString &command)
{
    const CommandRiskLevel level = CommandRiskClassifier::classify(command);
    const QColor accent = riskAccentColor(level);

    auto *card = new QFrame(m_chatContainer);
    card->setFrameShape(QFrame::StyledPanel);
    card->setStyleSheet(QStringLiteral(
        "QFrame { border: 1px solid %1; border-radius: 10px; background: palette(base); }")
        .arg(accent.name()));
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    auto *header = new QWidget(card);
    header->setStyleSheet(QStringLiteral("background: %1;")
        .arg(QColor(accent.red(), accent.green(), accent.blue(), 36).name(QColor::HexArgb)));
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 6, 10, 6);
    auto *badge = new QLabel(CommandRiskClassifier::label(level), header);
    QFont badgeFont = badge->font();
    badgeFont.setBold(true);
    badgeFont.setPointSizeF(badgeFont.pointSizeF() - 0.5);
    badge->setFont(badgeFont);
    badge->setStyleSheet(QStringLiteral("color: %1;").arg(accent.name()));
    headerLayout->addWidget(badge);
    headerLayout->addStretch(1);
    cardLayout->addWidget(header);

    auto *commandLabel = new QLabel(insertSoftBreakpoints(command), card);
    commandLabel->setWordWrap(true);
    commandLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont monoFont(QStringLiteral("Menlo"));
    monoFont.setStyleHint(QFont::Monospace);
    commandLabel->setFont(monoFont);
    commandLabel->setContentsMargins(11, 8, 11, 8);
    cardLayout->addWidget(commandLabel);

    auto *actions = new QWidget(card);
    auto *actionsLayout = new QHBoxLayout(actions);
    actionsLayout->setContentsMargins(8, 6, 8, 8);
    actionsLayout->setSpacing(6);
    auto *insertButton = new QPushButton(tr("插入"), actions);
    auto *copyButton = new QPushButton(tr("复制"), actions);
    auto *executeButton = new QPushButton(tr("执行"), actions);
    executeButton->setObjectName(QStringLiteral("aiExecuteButton"));
    if (level == CommandRiskLevel::HighRisk) {
        executeButton->setStyleSheet(QStringLiteral(
            "QPushButton { background: #d84a41; color: white; border: none;"
            "border-radius: 6px; padding: 5px 12px; font-weight: 600; }"
            "QPushButton:hover { background: #c23f37; }"));
    } else {
        executeButton->setStyleSheet(QStringLiteral(
            "QPushButton { background: #159a60; color: white; border: none;"
            "border-radius: 6px; padding: 5px 12px; font-weight: 600; }"
            "QPushButton:hover { background: #128a56; }"));
    }
    actionsLayout->addWidget(insertButton);
    actionsLayout->addWidget(copyButton);
    actionsLayout->addStretch(1);
    actionsLayout->addWidget(executeButton);
    cardLayout->addWidget(actions);

    connect(insertButton, &QPushButton::clicked, this, [this, command]() {
        if (m_inputEdit == nullptr) {
            return;
        }
        QString merged = m_inputEdit->toPlainText();
        if (!merged.isEmpty() && !merged.endsWith(QLatin1Char('\n'))) {
            merged += QLatin1Char('\n');
        }
        merged += command;
        m_inputEdit->setPlainText(merged);
        m_inputEdit->setFocus();
    });
    connect(copyButton, &QPushButton::clicked, this, [command]() {
        QGuiApplication::clipboard()->setText(command);
    });
    connect(executeButton, &QPushButton::clicked, this, [this, command, level, executeButton]() {
        if (m_captureInFlight) {
            emit statusMessage(tr("上一条命令还在等待输出，请稍候再执行"));
            return;
        }
        if (level == CommandRiskLevel::HighRisk) {
            const auto choice = QMessageBox::warning(
                this,
                tr("确认执行高风险命令"),
                tr("这条命令可能造成不可逆或大范围影响：\n\n%1\n\n确定要在当前终端执行吗？")
                    .arg(command),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel);
            if (choice != QMessageBox::Yes) {
                return;
            }
        }
        executeButton->setEnabled(false);
        executeButton->setText(tr("已执行"));
        executeButton->setStyleSheet(QStringLiteral(
            "QPushButton:disabled { background: palette(mid); color: palette(dark);"
            "border: none; border-radius: 6px; padding: 5px 12px; font-weight: 600; }"));
        emit commandExecutionRequested(command, QStringList{});
        emit statusMessage(tr("已发送到当前终端执行"));
        beginOutputCapture(command);
    });

    m_chatLayout->insertWidget(m_chatLayout->count() - 1, card);
}

void AiAssistantWidget::renderAssistantReply(const QString &content)
{
    // 单步流程只允许一条可执行命令；模型偶尔仍会在同一轮里多给几行，多出的那些只做只读
    // 展示，不生成可点击的执行卡片，避免用户在还没看到第一条命令结果前就误触后续命令。
    bool commandCardShown = false;
    QStringList ignoredCommands;
    for (const auto &segment : splitReplyIntoSegments(content)) {
        if (!segment.command.isEmpty()) {
            if (!commandCardShown) {
                appendCommandCard(segment.command);
                commandCardShown = true;
            } else {
                ignoredCommands.append(segment.command);
            }
        } else {
            appendAssistantBubble(segment.narrative);
        }
    }
    if (!ignoredCommands.isEmpty()) {
        appendNoticeBubble(
            tr("本轮只会执行第一条命令，以下内容已忽略：\n%1")
                .arg(ignoredCommands.join(QLatin1Char('\n'))),
            false);
    }
}

void AiAssistantWidget::setBusy(const bool busy)
{
    m_requestInFlight = busy;
    if (m_sendButton != nullptr) {
        m_sendButton->setEnabled(!busy && !m_captureInFlight);
    }
}

void AiAssistantWidget::beginOutputCapture(const QString &command)
{
    if (m_captureInFlight) {
        return;
    }
    if (!m_terminalContextProvider) {
        // 没有可读的终端快照——命令仍会正常执行，只是退化成一次性建议卡片，不自动续问。
        return;
    }

    m_captureCommand = command;
    m_captureLastText = m_terminalContextProvider();
    m_captureStartLength = m_captureLastText.length();
    // 把读取位置推进到命令下发前的这一刻：命令的输出从这里才真正开始，之前的旧内容
    // （如果还没被读过）不属于"这条命令的执行结果"，不应该被算进接下来的增量里。
    m_lastReadPosition = m_captureStartLength;
    m_captureStableTicks = 0;
    m_captureEverChanged = false;
    m_captureInFlight = true;
    m_captureElapsed.start();
    if (m_sendButton != nullptr) {
        m_sendButton->setEnabled(false);
    }

    m_captureNoticeBubble = new QLabel(tr("等待命令执行结果…"), m_chatContainer);
    m_captureNoticeBubble->setStyleSheet(QStringLiteral("color: palette(mid); font-style: italic;"));
    m_chatLayout->insertWidget(m_chatLayout->count() - 1, m_captureNoticeBubble);
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
    const bool changedSinceLastTick = current != m_captureLastText;
    m_captureLastText = current;
    if (current.length() != m_captureStartLength) {
        m_captureEverChanged = true;
    }
    m_captureStableTicks = changedSinceLastTick ? 0 : (m_captureStableTicks + 1);

    const bool stableLongEnough =
        m_captureEverChanged && m_captureStableTicks * kCapturePollIntervalMs >= kCaptureIdleMs;
    const bool timedOut = m_captureElapsed.hasExpired(kCaptureMaxWaitMs);
    if (stableLongEnough || timedOut) {
        finishOutputCapture(timedOut);
    }
}

void AiAssistantWidget::finishOutputCapture(const bool timedOut)
{
    if (m_captureTimer != nullptr) {
        m_captureTimer->stop();
    }
    m_captureInFlight = false;
    if (m_captureNoticeBubble != nullptr) {
        m_captureNoticeBubble->deleteLater();
        m_captureNoticeBubble = nullptr;
    }
    if (m_sendButton != nullptr) {
        m_sendButton->setEnabled(!m_requestInFlight);
    }

    // 复用同一套"自上次读取位置起"的增量读取：命令下发前没有任何手动附加动作，此刻
    // m_lastReadPosition 必然还停在命令下发前的位置，切出来的就是这条命令的真实输出。
    QString output = consumeNewTerminalOutput();
    if (output.isEmpty()) {
        output = tr("(无输出)");
    }

    const QString marker = timedOut
        ? tr("[命令 `%1` 的执行结果（等待输出超时，可能仍在运行或没有更多输出）]\n%2")
              .arg(m_captureCommand, output)
        : tr("[命令 `%1` 的执行结果]\n%2").arg(m_captureCommand, output);
    sendMessageInternal(marker);
}

void AiAssistantWidget::scrollToBottom()
{
    if (m_chatScroll == nullptr) {
        return;
    }
    // 布局在新增子控件后要等下一次事件循环才会重新计算滚动范围，同步 setValue 会拿到
    // 上一次的 maximum，看起来像是"差一条消息才滚到底"。
    QPointer<QScrollArea> scroll(m_chatScroll);
    QTimer::singleShot(0, this, [scroll]() {
        if (scroll.isNull()) {
            return;
        }
        QScrollBar *bar = scroll->verticalScrollBar();
        if (bar != nullptr) {
            bar->setValue(bar->maximum());
        }
    });
}
