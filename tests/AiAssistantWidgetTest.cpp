#include "ai/AiAssistantWidget.h"
#include "ai/AiChatTypes.h"
#include "ai/AiClient.h"
#include "ai/AiSettings.h"
#include "ai/AiSettingsService.h"
#include "ai/CommandRiskClassifier.h"
#include "ai/LocalAgentCli.h"
#include "i18n/LocalizationService.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QInputDialog>
#include <QMenu>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSharedPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QColor>
#include <QImage>
#include <QFrame>
#include <QGuiApplication>
#include <QTest>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QTemporaryDir>

#include <cmath>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>

namespace {

QByteArray chatCompletionBody(const QString &content)
{
    const QJsonObject message{{QStringLiteral("role"), QStringLiteral("assistant")},
                              {QStringLiteral("content"), content}};
    const QJsonObject choice{{QStringLiteral("message"), message}};
    const QJsonObject root{{QStringLiteral("choices"), QJsonArray{choice}}};
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

/**
 * @brief 判断助手是否已回到空闲态。
 *
 * 忙碌时发送按钮不再置灰，而是就地变成停止入口，因此"按钮可点"不再等于"这一轮结束了"；
 * 用例统一按按钮形态判断空闲，避免在模型还在分析时就去断言渲染结果。
 */
bool aiIsIdle(const QPushButton *sendButton)
{
    return sendButton != nullptr
        && sendButton->property("aiButtonMode").toString() == QStringLiteral("send");
}

QJsonArray requestMessages(const QByteArray &requestBody)
{
    return QJsonDocument::fromJson(requestBody).object().value(QStringLiteral("messages")).toArray();
}

/**
 * @brief 在临时目录里写一个可执行的 sh 脚本，冒充本机智能体 CLI。
 * @return 脚本绝对路径；写入或授权失败时为空。
 */
QString writeFakeCli(const QTemporaryDir &dir, const QString &name, const QByteArray &body)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {};
    }
    file.write("#!/bin/sh\n" + body);
    file.close();
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                             | QFileDevice::ExeOwner)) {
        return {};
    }
    return path;
}

QString readTextFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

/**
 * @brief 测试期间自动处理弹出的模态框，防止确认框把用例永远挂住。
 *
 * accept 为 true 时点"是"，否则直接关闭（等价于取消）；sawModal 记录是否出现过模态框，
 * 用来断言"全自动不应弹确认框"或"切到全自动必须先确认"。
 */
class ModalResponder final : public QObject {
public:
    explicit ModalResponder(const bool accept)
        : m_accept(accept)
    {
        m_timer.setInterval(50);
        connect(&m_timer, &QTimer::timeout, this, [this]() {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (box == nullptr) {
                return;
            }
            sawModal = true;
            if (m_accept && box->button(QMessageBox::Yes) != nullptr) {
                box->button(QMessageBox::Yes)->click();
            } else {
                box->reject();
            }
        });
        m_timer.start();
    }

    bool sawModal = false;

private:
    bool m_accept = false;
    QTimer m_timer;
};

} // namespace

/**
 * @brief 极简 OpenAI 兼容网关：按 responses 队列顺序应答，同时记录每次请求的完整正文。
 *
 * 手写 HTTP/1.1 解析而不是引入测试专用依赖，是因为仓库里目前没有任何 mock HTTP 服务器，
 * 这条最小实现（读到空行拿 Content-Length，凑够字节数即应答并关闭连接）足够覆盖
 * AiClient::sendMessage 的单个 POST 请求，不需要支持分块编码或长连接复用。
 */
class MiniAiServer final : public QObject {
    Q_OBJECT

public:
    explicit MiniAiServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, &MiniAiServer::acceptConnection);
        m_server.listen(QHostAddress::LocalHost);
    }

    [[nodiscard]] bool isListening() const { return m_server.isListening(); }
    [[nodiscard]] quint16 port() const { return m_server.serverPort(); }

    QList<QByteArray> responses;
    QList<QByteArray> capturedBodies;
    /**
     * @brief 收到请求后故意不应答，用来模拟"模型迟迟不返回"的长分析。
     *
     * 连接保持打开而不是断开：断开会让客户端立刻收到网络错误，那测的就是失败路径，
     * 而不是用户在漫长等待中按下停止的这条路径。
     */
    bool holdRequests = false;

private:
    void acceptConnection()
    {
        while (m_server.hasPendingConnections()) {
            QTcpSocket *socket = m_server.nextPendingConnection();
            auto buffer = QSharedPointer<QByteArray>::create();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer]() {
                buffer->append(socket->readAll());
                const int headerEnd = buffer->indexOf("\r\n\r\n");
                if (headerEnd < 0) {
                    return;
                }
                int contentLength = 0;
                const int lengthPos = buffer->indexOf("Content-Length:");
                if (lengthPos >= 0 && lengthPos < headerEnd) {
                    const int lineEnd = buffer->indexOf("\r\n", lengthPos);
                    contentLength =
                        buffer->mid(lengthPos + 15, lineEnd - lengthPos - 15).trimmed().toInt();
                }
                const int bodyStart = headerEnd + 4;
                if (buffer->size() - bodyStart < contentLength) {
                    return;
                }
                capturedBodies.append(buffer->mid(bodyStart, contentLength));
                if (holdRequests) {
                    return;
                }

                const QByteArray responseBody =
                    responses.isEmpty() ? QByteArrayLiteral("{}") : responses.takeFirst();
                const QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                            "Content-Length: "
                    + QByteArray::number(responseBody.size()) + "\r\nConnection: close\r\n\r\n"
                    + responseBody;
                socket->write(response);
                socket->flush();
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
        }
    }

    QTcpServer m_server;
};

/**
 * @brief 覆盖 AI 助手改版后的单步交互回路：执行命令后自动读取终端真实输出并续问模型。
 *
 * 这是发现"命令执行完不会自动续问"这一体验问题后补的用例：改版前客户端只把建议命令展示
 * 给用户，从不读取真实执行结果；现在改成轮询 terminalPlainText() 快照，等待 Shell 提示符
 * 重新出现后把差量文本作为隐藏观察自动发给模型。这里用一个手写的极简 HTTP 服务器顶替
 * 真实网关，断言长命令的短暂停顿不会触发下一轮、第二次请求确实带着新增终端文本，同时这些
 * 原始输出不会回显到聊天框。
 */
class AiAssistantWidgetTest final : public QObject {
    Q_OBJECT

private slots:
    /** @brief 输入框获得焦点时，承担描边的编辑器面板必须换成强调色。 */
    void composerHighlightsBorderWhenInputFocused();
    void executingCommandAutoCapturesOutputAndContinuesConversation();
    void newSessionClearsHistoryAndReadPosition();
    void assistantNarrativeRendersAsMarkdown();
    void multiStepResponseIsRejectedAndRepairedBeforeDisplay();
    void inputEditorSendsOnEnterAndGrowsToTenLines();
    void userMessageBubbleUsesSeventyPercentOfChatWidth();
    void conversationHistoryRestoresPreviousSession();
    void commandEnterExecutesLatestPendingCommand();
    void commandCardInsertModifyAndCopyUseCorrectRoutes();
    void chatAutoScrollResumesAfterUserReturnsToBottom();
    void aiClientParsesStreamingResponseIncrementally();
    void stopButtonEndsLongRunningAnalysis();
    /** @brief 推理型网关的 reasoning_content 单独成流，不混进最终回答。 */
    void aiClientStreamsReasoningSeparately();
    /**
     * @brief 分析过程中实时展示思考过程与已到达的结论文字，协议字段和命令不外露；
     * 完整回复到达后面板让位给正式回复。
     */
    void thinkingIndicatorShowsLiveAnalysis();
    /** @brief claude stream-json 输出被任意切分时仍能拼出正文、思考过程与最终结果。 */
    void claudeStreamParserHandlesSplitOutput();
    /** @brief 思考开关关闭时请求体不得出现相关字段，打开后才附加。 */
    void thinkingToggleControlsRequestField();
    /** @brief 模型列表拉取要从 /models 响应里解析出全部模型 id。 */
    void fetchModelsParsesGatewayModelIds();
    /** @brief 切换到深色主题后面板与已有气泡都不能继续用浅色底。 */
    void panelFollowsApplicationThemeToDark();
    /** @brief 语言热切换后聊天区的固定文案（附件开关、系统提示气泡）必须跟随新语言。 */
    void chatChromeFollowsLanguageSwitch();

    /**
     * @brief 自动执行只放行"只读且会自行结束"的命令。
     *
     * 解释器、git、包管理器曾整体被当作只读；命令替换、跟随日志、需要密码的 sudo 也都
     * 不能无人值守地跑。
     */
    void riskClassifierGatesAutoExecution();
    /** @brief 只读自动模式下，只读命令不经点击即下发，并照常捕获输出、自动续问。 */
    void autoReadOnlyRunsSafeCommandWithoutClick();
    /** @brief 只读自动模式下，修改系统的命令仍停在卡片上等待用户确认。 */
    void autoReadOnlyStillWaitsForWriteCommands();
    /** @brief 全自动模式连高风险命令也直接下发，且不弹确认框。 */
    void fullAutoRunsHighRiskCommandWithoutPrompt();
    /** @brief 连续自动执行到上限后暂停，把控制权交还用户。 */
    void autoExecutionPausesAtStepLimit();
    /** @brief 面板上的执行模式菜单会持久化策略；切到全自动必须先通过确认框。 */
    void executionModeMenuPersistsPolicy();
    /** @brief 只在给定目录中识别受支持的 CLI，并能读出版本号。 */
    void detectorFindsAgentsInGivenDirectories();
    /** @brief 每个 CLI 都以关闭工具 / 只读模式启动，提示词走约定的通道。 */
    void cliInvocationsStayReadOnly();
    /** @brief CLI 输出里的 ANSI 与 Markdown 围栏被剥掉；多个 JSON 对象原样交给协议校验。 */
    void cliReplyExtraction();
    /** @brief 选择本地 CLI 作为模型来源后，对话经由 CLI 的标准输入完成一次决策。 */
    void localCliBackendDrivesAgentLoop();
    /** @brief CLI 非零退出时，把 stderr 中的原因展示给用户而不是静默失败。 */
    void localCliFailureSurfacesStderr();
};

namespace {

/** @brief 取外框左边缘垂直居中处的像素色，即描边颜色。 */
QColor leftBorderColor(QWidget *frame)
{
    const QImage image = frame->grab().toImage();
    return image.pixelColor(0, image.height() / 2);
}

/** @brief 两种颜色的 RGB 欧氏距离，用来容忍抗锯齿带来的轻微色差。 */
double colorDistance(const QColor &left, const QColor &right)
{
    const double red = left.red() - right.red();
    const double green = left.green() - right.green();
    const double blue = left.blue() - right.blue();
    return std::sqrt(red * red + green * green + blue * blue);
}

} // namespace

void AiAssistantWidgetTest::composerHighlightsBorderWhenInputFocused()
{
    AiSettingsService settingsService(nullptr, nullptr);
    AiAssistantWidget widget(&settingsService);
    widget.resize(420, 640);
    widget.show();
    widget.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&widget));

    auto *composer = widget.findChild<QFrame *>(QStringLiteral("AiComposer"));
    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    QVERIFY(composer != nullptr && inputEdit != nullptr);
    const bool dark = QGuiApplication::palette().color(QPalette::Window).lightnessF() < 0.5;
    const QColor accent(dark ? QStringLiteral("#2bd981") : QStringLiteral("#17834f"));

    widget.setFocus();
    inputEdit->clearFocus();
    QVERIFY(!inputEdit->hasFocus());
    QVERIFY2(colorDistance(leftBorderColor(composer), accent) > 60,
             "输入框未聚焦时编辑器面板不应显示强调色描边");
    inputEdit->setFocus();
    QVERIFY(inputEdit->hasFocus());
    QVERIFY2(colorDistance(leftBorderColor(composer), accent) < 60,
             "AI 输入框获得焦点后编辑器面板没有高亮");
}

void AiAssistantWidgetTest::executingCommandAutoCapturesOutputAndContinuesConversation()
{
    MiniAiServer server;
    QVERIFY(server.isListening());
    server.responses.append(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"command","title":"确认 Nginx 是否已安装","reason":"先读取当前环境，避免重复安装或覆盖已有版本。","command":"which nginx"})JSON")));
    server.responses.append(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"complete","message":"根据真实输出已确认当前未安装 Nginx，本次检查目标已完成。"})JSON")));

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());
    settings.model = QStringLiteral("test-model");
    // 这些用例覆盖的是"用户点击执行"的手动路径，固定为逐条确认，不受自动执行默认值影响。
    settings.executionPolicy = AiExecutionPolicy::Manual;
    QVERIFY(settingsService.save(settings));

    AiAssistantWidget widget(&settingsService);
    widget.resize(420, 640);

    QString terminalText = QStringLiteral("[user@host ~]$ ");
    widget.setTerminalContextProvider([&terminalText]() { return terminalText; });

    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    QVERIFY(inputEdit != nullptr);
    QVERIFY(sendButton != nullptr);

    QSignalSpy execSpy(&widget, &AiAssistantWidget::commandExecutionRequested);

    inputEdit->setPlainText(QStringLiteral("帮我确认一下 nginx 装了没有"));
    sendButton->click();

    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !widget.findChildren<QPushButton *>(QStringLiteral("aiExecuteButton")).isEmpty(), 5000);

    // 第一次请求还不该带任何自动续问的"命令执行结果"消息，只有系统提示和用户的任务描述。
    const QJsonArray firstMessages = requestMessages(server.capturedBodies.at(0));
    for (const QJsonValue &entry : firstMessages) {
        QVERIFY(!entry.toObject().value(QStringLiteral("content")).toString().contains(
            QStringLiteral("[命令 `")));
    }

    auto *executeButton =
        widget.findChildren<QPushButton *>(QStringLiteral("aiExecuteButton")).last();
    QVERIFY(executeButton->isEnabled());
    executeButton->click();

    QCOMPARE(execSpy.count(), 1);
    QCOMPARE(execSpy.takeFirst().at(0).toString(), QStringLiteral("which nginx"));
    // 单步流程里同一条命令不能被重复执行，点击后必须立刻自锁，按钮文案也要能让用户一眼
    // 看出这条已经处理过，不是还没点。
    QVERIFY(!executeButton->isEnabled());
    QCOMPARE(executeButton->text(), QStringLiteral("Executed"));

    // 先模拟一个仍在运行但暂时没有新输出的长命令。旧实现只等待 900ms 的“输出稳定”，会在
    // 此处错误地认为命令已经完成；新实现必须继续等待提示符回归并给出明确等待状态。
    terminalText += QStringLiteral("which nginx\n正在执行耗时检查");
    QTest::qWait(1300);
    QCOMPARE(server.capturedBodies.size(), 1);
    // 等待命令执行期间按钮同样切换成停止形态：它保持可点，但点下去是中止等待而不是发送。
    QVERIFY(!aiIsIdle(sendButton));
    QVERIFY(sendButton->isEnabled());
    auto *waitingLabel = widget.findChild<QLabel *>(QStringLiteral("aiCommandWaitingLabel"));
    QVERIFY(waitingLabel != nullptr);
    QVERIFY(waitingLabel->text().contains(QStringLiteral("Waiting for the command to finish")));
    // 等待状态只保留一行：秒数在变，附带的固定操作说明已移除，卡片高度不再随等待增长。
    QVERIFY(!waitingLabel->text().contains(QChar::fromLatin1('\n')));
    QVERIFY(!waitingLabel->text().contains(QStringLiteral("无需复制")));

    // 提示符重新出现才代表交互式 Shell 已重新接管输入，此时客户端应自动读取隐藏差量并续问。
    terminalText += QStringLiteral("\nnginx not found\n[user@host ~]$ ");

    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), 2, 5000);

    const QJsonArray secondMessages = requestMessages(server.capturedBodies.at(1));
    QVERIFY(!secondMessages.isEmpty());
    // 系统级智能体规则必须每轮都携带；若第二轮丢失，模型在看到终端输出后很容易退回一次性
    // 输出完整教程的普通问答模式。
    QCOMPARE(secondMessages.first().toObject().value(QStringLiteral("role")).toString(),
             QStringLiteral("system"));
    const QString lastMessage =
        secondMessages.last().toObject().value(QStringLiteral("content")).toString();
    QVERIFY2(lastMessage.contains(QStringLiteral("[Result of command `which nginx`]")),
             qPrintable(lastMessage));
    QVERIFY2(lastMessage.contains(QStringLiteral("nginx not found")), qPrintable(lastMessage));
    // 续问只应携带命令下发之后新增的那部分文本，不应该把命令发出前就存在的旧提示符也带回去
    // ——否则说明实现退化成了整份快照重发，而不是基于基线的差量。
    QVERIFY2(!lastMessage.contains(QStringLiteral("[user@host ~]$ which nginx")),
             qPrintable(lastMessage));

    // 第二轮回复处理完毕后输入区应当恢复可用，不能停留在"AI 正在处理…"的忙碌态。
    QTRY_VERIFY_WITH_TIMEOUT(aiIsIdle(sendButton), 5000);
    // 第二轮回复没有代码块，不应该再多出一张可执行命令卡片。
    QCOMPARE(widget.findChildren<QPushButton *>(QStringLiteral("aiExecuteButton")).size(), 1);

    // 原始终端输出只能作为发给智能体的隐藏观察存在，聊天区不得重复显示命令回显、日志或
    // 提示符。最终完成说明可以概括结论，但不能把原始 `nginx not found` 文本直接贴回来。
    for (QLabel *label : widget.findChildren<QLabel *>()) {
        QVERIFY2(!label->text().contains(QStringLiteral("执行结果")), qPrintable(label->text()));
        QVERIFY2(!label->text().contains(QStringLiteral("nginx not found")), qPrintable(label->text()));
        QVERIFY2(!label->text().contains(QStringLiteral("正在执行耗时检查")), qPrintable(label->text()));
        QVERIFY2(!label->text().contains(QStringLiteral("[user@host ~]$")), qPrintable(label->text()));
    }

    bool foundCurrentStepReason = false;
    for (QLabel *label : widget.findChildren<QLabel *>()) {
        if (label->text().contains(QStringLiteral("避免重复安装"))) {
            foundCurrentStepReason = true;
        }
    }
    QVERIFY(foundCurrentStepReason);
}

/**
 * @brief "新会话"必须真正清空历史和终端读取位置，而不只是清空聊天区的界面控件。
 *
 * 用第二轮请求的 messages 数组长度和内容作为唯一可靠的验证点：如果历史或读取位置没有
 * 真的清零，新会话之后的第一次提问要么会带着上一轮的对话历史，要么会把新会话之前那段
 * 早就该被丢弃的终端增量也带回来。
 */
void AiAssistantWidgetTest::newSessionClearsHistoryAndReadPosition()
{
    MiniAiServer server;
    QVERIFY(server.isListening());
    server.responses.append(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"complete","message":"好的，已经记下这个任务。"})JSON")));
    server.responses.append(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"complete","message":"这是全新会话里的第一条回复。"})JSON")));

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());
    settings.model = QStringLiteral("test-model");
    // 这些用例覆盖的是"用户点击执行"的手动路径，固定为逐条确认，不受自动执行默认值影响。
    settings.executionPolicy = AiExecutionPolicy::Manual;
    QVERIFY(settingsService.save(settings));

    AiAssistantWidget widget(&settingsService);
    QString terminalText = QStringLiteral("[user@host ~]$ old output before new session\n");
    widget.setTerminalContextProvider([&terminalText]() { return terminalText; });

    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    auto *attachCheck = widget.findChild<QCheckBox *>();
    auto *newSessionButton =
        widget.findChild<QPushButton *>(QStringLiteral("aiNewSessionButton"));
    QVERIFY(inputEdit != nullptr);
    QVERIFY(sendButton != nullptr);
    QVERIFY(newSessionButton != nullptr);

    inputEdit->setPlainText(QStringLiteral("这是旧会话的提问"));
    sendButton->click();
    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(aiIsIdle(sendButton), 5000);

    newSessionButton->click();
    // 聊天区应当清空回只剩末尾的伸缩项，不再残留上一轮的气泡或命令卡片。
    QVERIFY(widget.findChildren<QPushButton *>(QStringLiteral("aiExecuteButton")).isEmpty());

    if (attachCheck != nullptr) {
        attachCheck->setChecked(true);
    }
    inputEdit->setPlainText(QStringLiteral("新会话的提问"));
    sendButton->click();
    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), 2, 5000);

    const QJsonArray secondRequestMessages = requestMessages(server.capturedBodies.at(1));
    // system + 本轮用户提问，最多再加一条终端增量；不应该出现上一轮的用户提问或助手回复。
    QVERIFY2(secondRequestMessages.size() <= 3,
             qPrintable(QString::number(secondRequestMessages.size())));
    for (const QJsonValue &entry : secondRequestMessages) {
        const QString content = entry.toObject().value(QStringLiteral("content")).toString();
        QVERIFY2(!content.contains(QStringLiteral("旧会话")), qPrintable(content));
        QVERIFY2(!content.contains(QStringLiteral("好的，已经记下这个任务")), qPrintable(content));
    }
}

/**
 * @brief 模型回复本来就是 Markdown，界面必须按 Markdown 渲染，而不是把 **、# 这些语法符号
 * 原样甩给用户。
 */
void AiAssistantWidgetTest::assistantNarrativeRendersAsMarkdown()
{
    MiniAiServer server;
    QVERIFY(server.isListening());
    server.responses.append(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"complete","message":"**已完成**，服务状态正常。"})JSON")));

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());
    settings.model = QStringLiteral("test-model");
    // 这些用例覆盖的是"用户点击执行"的手动路径，固定为逐条确认，不受自动执行默认值影响。
    settings.executionPolicy = AiExecutionPolicy::Manual;
    QVERIFY(settingsService.save(settings));

    AiAssistantWidget widget(&settingsService);
    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    QVERIFY(inputEdit != nullptr);
    QVERIFY(sendButton != nullptr);

    inputEdit->setPlainText(QStringLiteral("总结一下"));
    sendButton->click();
    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(aiIsIdle(sendButton), 5000);

    bool foundMarkdownBubble = false;
    for (QLabel *label : widget.findChildren<QLabel *>()) {
        if (label->text().contains(QStringLiteral("已完成"))) {
            foundMarkdownBubble = true;
            QCOMPARE(label->textFormat(), Qt::MarkdownText);
        }
    }
    QVERIFY(foundMarkdownBubble);
}

/**
 * @brief 模型即使输出完整教程，界面也不能先展示再在末尾提示“已忽略”。
 *
 * 第一份响应故意使用旧式 Markdown 并包含三条命令；客户端应把它视为协议违规，在后台追加
 * 一条纠正请求。只有第二份合法 JSON 中的单个检查命令可以进入界面和执行按钮，原教程里的
 * 安装、启动命令及说明均不得出现在任何可见标签中。
 */
void AiAssistantWidgetTest::multiStepResponseIsRejectedAndRepairedBeforeDisplay()
{
    MiniAiServer server;
    QVERIFY(server.isListening());
    server.responses.append(chatCompletionBody(QStringLiteral(
        "1. 检查系统\n```bash\ncat /etc/os-release\n```\n"
        "2. 安装 Nginx\n```bash\nyum install -y nginx\n```\n"
        "3. 启动服务\n```bash\nsystemctl start nginx\n```")));
    server.responses.append(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"command","title":"识别操作系统","reason":"需要先确认发行版，才能在下一轮选择正确的包管理器。","command":"cat /etc/os-release"})JSON")));

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());
    settings.model = QStringLiteral("test-model");
    // 这些用例覆盖的是"用户点击执行"的手动路径，固定为逐条确认，不受自动执行默认值影响。
    settings.executionPolicy = AiExecutionPolicy::Manual;
    QVERIFY(settingsService.save(settings));

    AiAssistantWidget widget(&settingsService);
    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    QVERIFY(inputEdit != nullptr);
    QVERIFY(sendButton != nullptr);

    inputEdit->setPlainText(QStringLiteral("帮我安装 Nginx"));
    sendButton->click();

    // 两次请求分别是原始决策和客户端自动发起的协议纠正；用户不需要手工重试。
    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), 2, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(aiIsIdle(sendButton), 5000);
    QCOMPARE(widget.findChildren<QPushButton *>(QStringLiteral("aiExecuteButton")).size(), 1);

    bool foundReason = false;
    for (QLabel *label : widget.findChildren<QLabel *>()) {
        const QString visibleText = label->text();
        QVERIFY2(!visibleText.contains(QStringLiteral("yum install")), qPrintable(visibleText));
        QVERIFY2(!visibleText.contains(QStringLiteral("systemctl start")), qPrintable(visibleText));
        if (visibleText.contains(QStringLiteral("确认发行版"))) {
            foundReason = true;
        }
    }
    QVERIFY(foundReason);

    const QJsonArray repairMessages = requestMessages(server.capturedBodies.at(1));
    QCOMPARE(repairMessages.first().toObject().value(QStringLiteral("role")).toString(),
             QStringLiteral("system"));
    QVERIFY(repairMessages.last().toObject().value(QStringLiteral("content")).toString().contains(
        QStringLiteral("violated the single-step protocol")));
}

/**
 * @brief 输入框应使用桌面聊天工具的键盘语义，并按内容高度增长但不超过十行。
 *
 * 先用 Shift+Enter 验证换行不会发送，再用 Enter 验证完整两行文本只发出一次请求。随后在
 * 请求结束后写入十二行草稿，确认输入框达到十行上限后启用内部滚动，而不是继续挤压聊天区。
 */
void AiAssistantWidgetTest::inputEditorSendsOnEnterAndGrowsToTenLines()
{
    MiniAiServer server;
    QVERIFY(server.isListening());
    server.responses.append(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"complete","message":"已收到任务。"})JSON")));

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());
    settings.model = QStringLiteral("test-model");
    // 这些用例覆盖的是"用户点击执行"的手动路径，固定为逐条确认，不受自动执行默认值影响。
    settings.executionPolicy = AiExecutionPolicy::Manual;
    QVERIFY(settingsService.save(settings));

    AiAssistantWidget widget(&settingsService);
    widget.resize(420, 640);
    widget.show();
    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    QVERIFY(inputEdit != nullptr);
    const int initialHeight = inputEdit->height();

    inputEdit->setFocus();
    // QtTest::keyClicks 仅支持 ASCII 键表，中文文本使用编辑器 API 注入；Enter/Shift+Enter 仍用
    // 真实键事件触发，测试目标（事件过滤器是否发送或换行）不受影响。
    inputEdit->setPlainText(QStringLiteral("第一行"));
    inputEdit->moveCursor(QTextCursor::End);
    QTest::keyClick(inputEdit, Qt::Key_Return, Qt::ShiftModifier);
    inputEdit->insertPlainText(QStringLiteral("第二行"));
    QCOMPARE(server.capturedBodies.size(), 0);
    QVERIFY(inputEdit->toPlainText().contains(QLatin1Char('\n')));
    QTRY_VERIFY(inputEdit->height() > initialHeight);

    QTest::keyClick(inputEdit, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), 1, 5000);
    QVERIFY(inputEdit->toPlainText().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"))->isEnabled(),
                             5000);

    QStringList twelveLines;
    for (int index = 1; index <= 12; ++index) {
        twelveLines.append(QStringLiteral("第 %1 行").arg(index));
    }
    inputEdit->setPlainText(twelveLines.join(QLatin1Char('\n')));
    QTRY_VERIFY(inputEdit->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded);
    const int tenLineUpperBound = inputEdit->fontMetrics().lineSpacing() * 10 + 16;
    QVERIFY2(inputEdit->height() <= tenLineUpperBound,
             qPrintable(QStringLiteral("height=%1 upper=%2")
                            .arg(inputEdit->height()).arg(tenLineUpperBound)));
}

/**
 * @brief 用户消息气泡应响应式占据消息行约 70% 宽度。
 *
 * 使用短文本覆盖最容易退化的场景：QLabel 的自然宽度远小于聊天区时，气泡仍须由 3:7
 * 伸缩布局扩展到目标比例。测试同时以消息行而非整个窗口为基准，避免标题栏、滚动条和聊天
 * 容器内边距影响断言；保留少量像素级容差，以兼容不同平台 Qt 布局舍入方式。
 */
void AiAssistantWidgetTest::userMessageBubbleUsesSeventyPercentOfChatWidth()
{
    AiSettingsService settingsService(nullptr, nullptr);
    AiAssistantWidget widget(&settingsService);
    widget.resize(800, 640);
    widget.show();

    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    QVERIFY(inputEdit != nullptr);
    QVERIFY(sendButton != nullptr);

    // 即使未配置模型，发送动作也会先渲染用户消息，再显示配置提示，足以独立验证本地布局，
    // 且不会引入网络请求或依赖测试服务器。
    inputEdit->setPlainText(QStringLiteral("短消息"));
    sendButton->click();

    auto *messageRow = widget.findChild<QWidget *>(QStringLiteral("AiUserMessageRow"));
    auto *messageBubble = widget.findChild<QLabel *>(QStringLiteral("AiUserMessageBubble"));
    QVERIFY(messageRow != nullptr);
    QVERIFY(messageBubble != nullptr);
    QTRY_VERIFY(messageRow->width() > 0);
    // 新消息插入后 Qt 会在下一轮事件循环才激活嵌套布局；等待左侧留白实际分配完成，避免
    // 读取到 QLabel 创建时继承的临时全宽几何尺寸。
    QTRY_VERIFY(messageBubble->width() < messageRow->width());

    const qreal bubbleWidthRatio = static_cast<qreal>(messageBubble->width())
        / static_cast<qreal>(messageRow->contentsRect().width());
    QVERIFY2(bubbleWidthRatio >= 0.68 && bubbleWidthRatio <= 0.72,
             qPrintable(QStringLiteral("用户消息气泡宽度比例应约为 70%，实际为 %1")
                            .arg(bubbleWidthRatio, 0, 'f', 3)));
}

/**
 * @brief 历史按钮应列出当前标签最近会话，并能完整恢复选中的旧聊天。
 *
 * 先完成一轮旧会话，再新建并完成第二轮；菜单应同时显示两条记录且标题来自各自第一条用户
 * 消息。选中旧记录后，聊天区只能显示旧用户气泡，证明加载的不是全局共享历史或仅修改菜单
 * 勾选状态。测试还断言上下文行不再出现“当前服务器”固定前缀。
 */
void AiAssistantWidgetTest::conversationHistoryRestoresPreviousSession()
{
    MiniAiServer server;
    QVERIFY(server.isListening());
    server.responses.append(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"complete","message":"旧会话已经完成。"})JSON")));
    server.responses.append(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"complete","message":"新会话已经完成。"})JSON")));

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());
    settings.model = QStringLiteral("test-model");
    // 这些用例覆盖的是"用户点击执行"的手动路径，固定为逐条确认，不受自动执行默认值影响。
    settings.executionPolicy = AiExecutionPolicy::Manual;
    QVERIFY(settingsService.save(settings));

    AiAssistantWidget widget(&settingsService);
    widget.resize(620, 640);
    widget.show();
    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    auto *newSessionButton =
        widget.findChild<QPushButton *>(QStringLiteral("aiNewSessionButton"));
    auto *historyButton = widget.findChild<QToolButton *>(QStringLiteral("aiHistoryButton"));
    auto *historyMenu = widget.findChild<QMenu *>(QStringLiteral("aiConversationHistoryMenu"));
    QVERIFY(inputEdit != nullptr);
    QVERIFY(sendButton != nullptr);
    QVERIFY(newSessionButton != nullptr);
    QVERIFY(historyButton != nullptr);
    QVERIFY(historyMenu != nullptr);

    for (const QLabel *label : widget.findChildren<QLabel *>()) {
        QVERIFY2(label->text() != QStringLiteral("当前服务器"),
                 "AI 助手上下文行不应继续显示固定的“当前服务器”前缀");
    }

    inputEdit->setPlainText(QStringLiteral("旧会话任务"));
    sendButton->click();
    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(aiIsIdle(sendButton), 5000);
    newSessionButton->click();

    inputEdit->setPlainText(QStringLiteral("新会话任务"));
    sendButton->click();
    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), 2, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(aiIsIdle(sendButton), 5000);

    // aboutToShow 是菜单刷新历史快照的唯一入口；直接经元对象触发可避免离屏测试依赖真实
    // 鼠标弹窗，同时仍覆盖生产环境点击按钮时执行的同一信号槽连接。
    QVERIFY(QMetaObject::invokeMethod(historyMenu, "aboutToShow", Qt::DirectConnection));
    QCOMPARE(historyMenu->actions().size(), 2);
    QAction *oldConversationAction = nullptr;
    for (QAction *action : historyMenu->actions()) {
        if (action->text().contains(QStringLiteral("旧会话任务"))) {
            oldConversationAction = action;
            break;
        }
    }
    QVERIFY(oldConversationAction != nullptr);
    oldConversationAction->trigger();

    // clearChatContent() 使用 deleteLater() 保证菜单回调安全返回；这里只统计可见气泡，既
    // 验证恢复结果，也不会把等待下一事件循环析构的旧控件误算进去。
    QTRY_VERIFY([&widget]() {
        bool oldMessageVisible = false;
        bool newMessageVisible = false;
        const QList<QLabel *> bubbles =
            widget.findChildren<QLabel *>(QStringLiteral("AiUserMessageBubble"));
        for (const QLabel *bubble : bubbles) {
            if (!bubble->isVisible()) {
                continue;
            }
            oldMessageVisible = oldMessageVisible || bubble->text() == QStringLiteral("旧会话任务");
            newMessageVisible = newMessageVisible || bubble->text() == QStringLiteral("新会话任务");
        }
        return oldMessageVisible && !newMessageVisible;
    }());
}

/**
 * @brief Command+Enter 必须执行最后一条待确认命令，而不是发送输入框草稿。
 *
 * 输入框保留一段未发送文字，然后触发 Meta+Return：执行信号应携带智能体建议命令，输入草稿
 * 仍应原样保留。按钮同时要展示快捷键提示；执行后再次按快捷键不会重复触发已经禁用的命令。
 */
void AiAssistantWidgetTest::commandEnterExecutesLatestPendingCommand()
{
    MiniAiServer server;
    QVERIFY(server.isListening());
    server.responses.append(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"command","title":"检查 Nginx","reason":"先确认当前安装状态。","command":"which nginx"})JSON")));

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());
    settings.model = QStringLiteral("test-model");
    // 这些用例覆盖的是"用户点击执行"的手动路径，固定为逐条确认，不受自动执行默认值影响。
    settings.executionPolicy = AiExecutionPolicy::Manual;
    QVERIFY(settingsService.save(settings));

    AiAssistantWidget widget(&settingsService);
    widget.resize(420, 640);
    widget.show();
    QString terminalText = QStringLiteral("[user@host ~]$ ");
    widget.setTerminalContextProvider([&terminalText]() { return terminalText; });

    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    QVERIFY(inputEdit != nullptr);
    QVERIFY(sendButton != nullptr);
    QSignalSpy execSpy(&widget, &AiAssistantWidget::commandExecutionRequested);

    inputEdit->setPlainText(QStringLiteral("帮我检查 nginx"));
    sendButton->click();
    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !widget.findChildren<QPushButton *>(QStringLiteral("aiExecuteButton")).isEmpty(), 5000);

    QPushButton *executeButton =
        widget.findChildren<QPushButton *>(QStringLiteral("aiExecuteButton")).last();
    QVERIFY(executeButton->text().contains(QStringLiteral("⌘")));
    QVERIFY(executeButton->toolTip().contains(QStringLiteral("Command+Enter")));

    inputEdit->setPlainText(QStringLiteral("这段草稿不能被发送"));
    inputEdit->setFocus();
    // Qt/macOS 把物理 Command 映射为 ControlModifier；测试必须模拟真实平台事件，不能使用
    // MetaModifier（它在 macOS 对应物理 Control），否则会再次掩盖实机快捷键失效的问题。
#if defined(Q_OS_MACOS)
    constexpr Qt::KeyboardModifier commandModifier = Qt::ControlModifier;
#else
    constexpr Qt::KeyboardModifier commandModifier = Qt::MetaModifier;
#endif
    QTest::keyClick(inputEdit, Qt::Key_Return, commandModifier);
    QTRY_COMPARE(execSpy.count(), 1);
    QCOMPARE(execSpy.at(0).at(0).toString(), QStringLiteral("which nginx"));
    QCOMPARE(inputEdit->toPlainText(), QStringLiteral("这段草稿不能被发送"));
    QVERIFY(!executeButton->isEnabled());

    QTest::keyClick(inputEdit, Qt::Key_Return, commandModifier);
    QTest::qWait(50);
    QCOMPARE(execSpy.count(), 1);
}

/**
 * @brief 命令卡片的插入、修改、复制必须各走独立且可感知的交互路径。
 *
 * “插入”直接发出终端插入信号且不改变 AI 输入框；“修改”弹出编辑对话框，确认后发送修改值；
 * “复制”只写剪贴板并产生成功提示。三者都不能触发 commandExecutionRequested。
 */
void AiAssistantWidgetTest::commandCardInsertModifyAndCopyUseCorrectRoutes()
{
    MiniAiServer server;
    QVERIFY(server.isListening());
    server.responses.append(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"command","title":"检查服务","reason":"读取当前服务状态。","command":"systemctl status nginx"})JSON")));

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());
    settings.model = QStringLiteral("test-model");
    // 这些用例覆盖的是"用户点击执行"的手动路径，固定为逐条确认，不受自动执行默认值影响。
    settings.executionPolicy = AiExecutionPolicy::Manual;
    QVERIFY(settingsService.save(settings));

    AiAssistantWidget widget(&settingsService);
    widget.resize(420, 640);
    widget.show();
    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    QVERIFY(inputEdit != nullptr);
    QVERIFY(sendButton != nullptr);
    QSignalSpy insertSpy(&widget, &AiAssistantWidget::commandInsertionRequested);
    QSignalSpy executeSpy(&widget, &AiAssistantWidget::commandExecutionRequested);
    QSignalSpy statusSpy(&widget, &AiAssistantWidget::statusMessage);

    inputEdit->setPlainText(QStringLiteral("检查 nginx"));
    sendButton->click();
    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        widget.findChild<QPushButton *>(QStringLiteral("aiInsertButton")) != nullptr, 5000);

    inputEdit->setPlainText(QStringLiteral("保留的聊天草稿"));
    widget.findChild<QPushButton *>(QStringLiteral("aiInsertButton"))->click();
    QCOMPARE(insertSpy.count(), 1);
    QCOMPARE(insertSpy.at(0).at(0).toString(), QStringLiteral("systemctl status nginx"));
    QCOMPARE(inputEdit->toPlainText(), QStringLiteral("保留的聊天草稿"));
    QCOMPARE(executeSpy.count(), 0);

    widget.findChild<QPushButton *>(QStringLiteral("aiCopyButton"))->click();
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("systemctl status nginx"));
    QVERIFY(!statusSpy.isEmpty());
    QCOMPARE(statusSpy.last().at(0).toString(), QStringLiteral("Command copied to clipboard"));

    // QInputDialog::getText 使用嵌套事件循环；预先排队的回调在对话框显示后填入修改值并确认，
    // 从而覆盖真实的弹窗交互，而不是直接调用内部信号绕过实现。
    QTimer::singleShot(0, &widget, []() {
        for (QWidget *topLevel : QApplication::topLevelWidgets()) {
            auto *dialog = qobject_cast<QInputDialog *>(topLevel);
            if (dialog != nullptr && dialog->isVisible()) {
                dialog->setTextValue(QStringLiteral("systemctl is-active nginx"));
                dialog->accept();
                return;
            }
        }
    });
    widget.findChild<QPushButton *>(QStringLiteral("aiModifyButton"))->click();
    QCOMPARE(insertSpy.count(), 2);
    QCOMPARE(insertSpy.at(1).at(0).toString(), QStringLiteral("systemctl is-active nginx"));
    QCOMPARE(executeSpy.count(), 0);
}

/**
 * @brief 用户上滚时暂停自动跟随，重新滚到底部后必须恢复自动贴底。
 *
 * 用多轮 complete 消息制造可滚动内容。第一次手动上滚后新增消息不应改变视口位置；用户再把
 * 滑块拖到底部，下一条消息到达后滚动条应跟随新的 maximum，而不是停在旧的底部位置。
 */
void AiAssistantWidgetTest::chatAutoScrollResumesAfterUserReturnsToBottom()
{
    MiniAiServer server;
    QVERIFY(server.isListening());
    for (int index = 0; index < 14; ++index) {
        server.responses.append(chatCompletionBody(QStringLiteral(
            R"JSON({"state":"complete","message":"这是一段用于验证聊天滚动行为的较长回复内容。"})JSON")));
    }

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());
    settings.model = QStringLiteral("test-model");
    // 这些用例覆盖的是"用户点击执行"的手动路径，固定为逐条确认，不受自动执行默认值影响。
    settings.executionPolicy = AiExecutionPolicy::Manual;
    QVERIFY(settingsService.save(settings));

    AiAssistantWidget widget(&settingsService);
    widget.resize(360, 360);
    widget.show();
    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    auto *chatScroll = widget.findChild<QScrollArea *>(QStringLiteral("AiChatScroll"));
    QVERIFY(inputEdit != nullptr);
    QVERIFY(sendButton != nullptr);
    QVERIFY(chatScroll != nullptr);

    for (int index = 0; index < 7; ++index) {
        inputEdit->setPlainText(QStringLiteral("消息 %1").arg(index));
        sendButton->click();
        QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), index + 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(aiIsIdle(sendButton), 5000);
    }
    QScrollBar *bar = chatScroll->verticalScrollBar();
    QTRY_VERIFY(bar->maximum() > 0);
    QTRY_COMPARE(bar->value(), bar->maximum());

    bar->setValue(0);
    inputEdit->setPlainText(QStringLiteral("上滚后新增消息"));
    sendButton->click();
    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), 8, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(aiIsIdle(sendButton), 5000);
    QCOMPARE(bar->value(), 0);

    // 用户重新拖到底部后，下一轮消息应跟随增长后的新 maximum，证明自动跟随已经恢复。
    bar->setValue(bar->maximum());
    inputEdit->setPlainText(QStringLiteral("回到底部后新增消息"));
    sendButton->click();
    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), 9, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(aiIsIdle(sendButton), 5000);
    QTRY_COMPARE(bar->value(), bar->maximum());
}

/**
 * @brief 流式解析必须真的按增量到达处理，而不是"凑巧因为整包一次性到达才工作"。
 *
 * 手动分两次往 socket 里写数据，中间用 QTRY 强制事件循环先跑一轮——这样能确认
 * processStreamBuffer() 在缓冲区里只躺着半包数据时会正确等待，不会提前把不完整的
 * SSE 事件当成完整的解析掉，也不会在第二批字节到达后重复处理第一批已经消费过的内容。
 */
void AiAssistantWidgetTest::aiClientParsesStreamingResponseIncrementally()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    QPointer<QTcpSocket> serverSide;
    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        serverSide = server.nextPendingConnection();
    });

    AiClient client;
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    settings.model = QStringLiteral("test-model");
    // 这些用例覆盖的是"用户点击执行"的手动路径，固定为逐条确认，不受自动执行默认值影响。
    settings.executionPolicy = AiExecutionPolicy::Manual;

    QSignalSpy chunkSpy(&client, &AiClient::replyChunkReceived);
    QSignalSpy finishedSpy(&client, &AiClient::replyFinished);

    client.sendMessage(settings, QList<AiChatMessage>{AiChatMessage{AiChatRole::User, QStringLiteral("hi")}});

    QTRY_VERIFY_WITH_TIMEOUT(!serverSide.isNull(), 5000);

    const QByteArray ssePart1 = "data: {\"choices\":[{\"delta\":{\"content\":\"你好\"}}]}\n\n";
    const QByteArray ssePart2 = "data: {\"choices\":[{\"delta\":{\"content\":\"，世界\"}}]}\n\n"
                                "data: [DONE]\n\n";
    const QByteArray header = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                              "Content-Length: "
        + QByteArray::number(ssePart1.size() + ssePart2.size()) + "\r\nConnection: close\r\n\r\n";

    serverSide->write(header + ssePart1);
    serverSide->flush();

    // 第一批字节只包含一个完整事件；此时必须只看到第一段增量，第二段还没发出去。
    QTRY_COMPARE_WITH_TIMEOUT(chunkSpy.count(), 1, 5000);
    QCOMPARE(chunkSpy.at(0).at(0).toString(), QStringLiteral("你好"));
    QCOMPARE(finishedSpy.count(), 0);

    serverSide->write(ssePart2);
    serverSide->flush();
    serverSide->disconnectFromHost();

    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
    QCOMPARE(chunkSpy.count(), 2);
    // 每次信号都带的是"累计到目前为止"的完整文本，不是单独的增量片段。
    QCOMPARE(chunkSpy.at(1).at(0).toString(), QStringLiteral("你好，世界"));
    QCOMPARE(finishedSpy.at(0).at(0).toString(), QStringLiteral("你好，世界"));
}


void AiAssistantWidgetTest::stopButtonEndsLongRunningAnalysis()
{
    MiniAiServer server;
    QVERIFY(server.isListening());
    // 网关收下请求后一直不回，正是用户看到"正在分析…"停在那里不动的现场。
    server.holdRequests = true;

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());
    settings.model = QStringLiteral("test-model");
    // 这些用例覆盖的是"用户点击执行"的手动路径，固定为逐条确认，不受自动执行默认值影响。
    settings.executionPolicy = AiExecutionPolicy::Manual;
    QVERIFY(settingsService.save(settings));

    AiAssistantWidget widget(&settingsService);
    widget.resize(420, 640);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));

    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    QVERIFY(inputEdit != nullptr && sendButton != nullptr);

    const auto labelWithText = [&widget](const QString &needle) {
        for (const QLabel *label : widget.findChildren<QLabel *>()) {
            if (label->text().contains(needle)) {
                return true;
            }
        }
        return false;
    };

    inputEdit->setPlainText(QStringLiteral("查看一下集群状态"));
    sendButton->click();
    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), 1, 5000);
    QTRY_VERIFY(labelWithText(QStringLiteral("Analyzing")));

    // 等待期间按钮不能是灰的：它必须就地变成停止入口，否则用户只能干等或关掉整个标签。
    QVERIFY(sendButton->isEnabled());
    QCOMPARE(sendButton->property("aiButtonMode").toString(), QStringLiteral("stop"));

    sendButton->click();

    QTRY_VERIFY2(!labelWithText(QStringLiteral("Analyzing")), "停止后等待指示器必须消失");
    QVERIFY(labelWithText(QStringLiteral("This analysis has been stopped")));
    QCOMPARE(sendButton->property("aiButtonMode").toString(), QStringLiteral("send"));
    QVERIFY(sendButton->isEnabled());

    // 中止会让底层 QNetworkReply 以 OperationCanceledError 收尾。若这条错误照常上抛，
    // 用户主动停止之后会平白多出一条"请求失败"，这里确认它被判定为正常结束而不是故障。
    QTest::qWait(300);
    QVERIFY2(!labelWithText(QStringLiteral("请求失败")),
             "用户主动停止不应该显示成网络请求失败");

    // 停止之后必须能立刻发起下一轮，而不是把面板永久锁在忙碌态。
    server.holdRequests = false;
    server.responses.append(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"complete","message":"已重新确认集群状态。"})JSON")));
    inputEdit->setPlainText(QStringLiteral("再试一次"));
    sendButton->click();
    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), 2, 5000);
    QTRY_VERIFY(labelWithText(QStringLiteral("已重新确认集群状态")));
}

void AiAssistantWidgetTest::aiClientStreamsReasoningSeparately()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QPointer<QTcpSocket> serverSide;
    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        serverSide = server.nextPendingConnection();
    });

    AiClient client;
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    settings.model = QStringLiteral("test-model");
    QSignalSpy reasoningSpy(&client, &AiClient::reasoningChunkReceived);
    QSignalSpy chunkSpy(&client, &AiClient::replyChunkReceived);
    QSignalSpy finishedSpy(&client, &AiClient::replyFinished);
    client.sendMessage(settings, {AiChatMessage{AiChatRole::User, QStringLiteral("hi")}});
    QTRY_VERIFY_WITH_TIMEOUT(!serverSide.isNull(), 5000);

    // DeepSeek / 千问用 reasoning_content，OpenRouter / Ollama 用 reasoning，两种都要认。
    const QByteArray body =
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"先看\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"reasoning\":\"内存\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"答案\"}}]}\n\n"
        "data: [DONE]\n\n";
    serverSide->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: "
                      + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    serverSide->flush();
    serverSide->disconnectFromHost();

    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
    QCOMPARE(reasoningSpy.count(), 2);
    QCOMPARE(reasoningSpy.last().at(0).toString(), QStringLiteral("先看内存"));
    QCOMPARE(chunkSpy.count(), 1);
    // 思考过程只用于展示，绝不能拼进交给协议校验的回答正文。
    QCOMPARE(finishedSpy.at(0).at(0).toString(), QStringLiteral("答案"));
}

void AiAssistantWidgetTest::thinkingIndicatorShowsLiveAnalysis()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QPointer<QTcpSocket> serverSide;
    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        serverSide = server.nextPendingConnection();
    });

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    settings.model = QStringLiteral("test-model");
    settings.executionPolicy = AiExecutionPolicy::Manual;
    QVERIFY(settingsService.save(settings));

    AiAssistantWidget widget(&settingsService);
    widget.resize(420, 640);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    QVERIFY(inputEdit != nullptr && sendButton != nullptr);
    const auto visibleText = [&widget](const QString &name) {
        const QLabel *label = widget.findChild<QLabel *>(name);
        return label != nullptr && label->isVisible() ? label->text() : QString();
    };

    inputEdit->setPlainText(QStringLiteral("看看内存都用在哪里"));
    sendButton->click();
    QTRY_VERIFY_WITH_TIMEOUT(!serverSide.isNull(), 5000);
    QTRY_VERIFY(widget.findChild<QWidget *>(QStringLiteral("aiThinkingIndicator")) != nullptr);
    // 还没有任何增量时只显示"正在分析…"，两块过程区域不能先空着占位。
    QVERIFY(visibleText(QStringLiteral("aiThinkingReasoning")).isEmpty());
    QVERIFY(visibleText(QStringLiteral("aiThinkingDraft")).isEmpty());

    // 不带 Content-Length、分批写入，模拟模型边想边输出的真实流式响应。
    serverSide->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
                      "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"free 已确认总量，"
                      "下一步按进程排序\"}}]}\n\n");
    serverSide->flush();
    QTRY_VERIFY_WITH_TIMEOUT(
        visibleText(QStringLiteral("aiThinkingReasoning")).contains(QStringLiteral("下一步按进程排序")),
        5000);

    // 回答写到 reason 中间、转义序列被切断：草稿只显示已到达的结论文字。
    serverSide->write("data: {\"choices\":[{\"delta\":{\"content\":"
                      "\"{\\\"state\\\":\\\"command\\\",\\\"title\\\":\\\"查看高占用进程\\\","
                      "\\\"reason\\\":\\\"按常驻内存排序\\\\u30\"}}]}\n\n");
    serverSide->flush();
    QTRY_VERIFY_WITH_TIMEOUT(
        visibleText(QStringLiteral("aiThinkingDraft")).contains(QStringLiteral("按常驻内存排序")),
        5000);
    const QString draft = visibleText(QStringLiteral("aiThinkingDraft"));
    QVERIFY2(draft.startsWith(QStringLiteral("查看高占用进程")), qPrintable(draft));
    QVERIFY2(!draft.contains(QStringLiteral("state")) && !draft.contains(QLatin1Char('{'))
                 && !draft.contains(QLatin1Char('\\')),
             qPrintable(draft));
    QVERIFY(widget.findChild<QPushButton *>(QStringLiteral("aiExecuteButton")) == nullptr);

    // 命令字段到达时也不提前预览，必须等整条回复通过协议校验后以卡片出现。
    serverSide->write("data: {\"choices\":[{\"delta\":{\"content\":"
                      "\"02\\\",\\\"command\\\":\\\"ps aux --sort=-rss\"}}]}\n\n");
    serverSide->flush();
    QTest::qWait(200);
    QVERIFY(!visibleText(QStringLiteral("aiThinkingDraft")).contains(QStringLiteral("ps aux")));

    serverSide->write("data: {\"choices\":[{\"delta\":{\"content\":\"\\\"}\"}}]}\n\n"
                      "data: [DONE]\n\n");
    serverSide->flush();
    serverSide->disconnectFromHost();

    QTRY_VERIFY_WITH_TIMEOUT(
        widget.findChild<QPushButton *>(QStringLiteral("aiExecuteButton")) != nullptr, 5000);
    QTRY_VERIFY(widget.findChild<QWidget *>(QStringLiteral("aiThinkingIndicator")) == nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(aiIsIdle(sendButton), 5000);
}

void AiAssistantWidgetTest::claudeStreamParserHandlesSplitOutput()
{
    const QByteArray output =
        "{\"type\":\"system\",\"subtype\":\"init\"}\n"
        "{\"type\":\"stream_event\",\"event\":{\"type\":\"message_start\"}}\n"
        "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"被重试作废\"}}}\n"
        "{\"type\":\"stream_event\",\"event\":{\"type\":\"message_start\"}}\n"
        "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
        "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"想一想\"}}}\n"
        "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"你好\"}}}\n"
        "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"，世界\"}}}\n"
        "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"result\":\"你好，世界\"}";

    // 每 7 个字节切一刀，必然把行和多字节 UTF-8 字符都切断。
    LocalAgentCli::ClaudeStreamParser parser;
    for (qsizetype offset = 0; offset < output.size(); offset += 7) {
        parser.feed(output.mid(offset, 7));
    }
    QCOMPARE(parser.text(), QStringLiteral("你好，世界"));
    QCOMPARE(parser.thinking(), QStringLiteral("想一想"));
    // 最后一行没有换行，只有 finish() 之后才算收到结果。
    QVERIFY(!parser.hasResult());
    parser.finish();
    QVERIFY(parser.hasResult());
    QVERIFY(!parser.resultIsError());
    QCOMPARE(parser.result(), QStringLiteral("你好，世界"));

    LocalAgentCli::ClaudeStreamParser failing;
    failing.feed("{\"type\":\"result\",\"subtype\":\"error_during_execution\",\"is_error\":true,"
                 "\"errors\":[\"Not logged in\"]}\n");
    QVERIFY(failing.hasResult());
    QVERIFY(failing.resultIsError());
    QCOMPARE(failing.result(), QStringLiteral("Not logged in"));
}

void AiAssistantWidgetTest::thinkingToggleControlsRequestField()
{
    MiniAiServer server;
    QVERIFY(server.isListening());
    server.responses.append(chatCompletionBody(QStringLiteral("ok")));
    server.responses.append(chatCompletionBody(QStringLiteral("ok")));

    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());
    settings.model = QStringLiteral("test-model");
    // 这些用例覆盖的是"用户点击执行"的手动路径，固定为逐条确认，不受自动执行默认值影响。
    settings.executionPolicy = AiExecutionPolicy::Manual;

    AiClient client;
    const QList<AiChatMessage> messages{
        AiChatMessage{AiChatRole::User, QStringLiteral("你好")}};

    // 关闭思考：请求体必须与本开关引入之前完全一致，一个多余字段都不能有——OpenAI 这类
    // 严格网关会对无法识别的字段直接返回 400。
    QSignalSpy finishedSpy(&client, &AiClient::replyFinished);
    client.sendMessage(settings, messages);
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
    const QJsonObject withoutThinking =
        QJsonDocument::fromJson(server.capturedBodies.at(0)).object();
    QVERIFY(!withoutThinking.contains(QStringLiteral("enable_thinking")));

    settings.thinkingEnabled = true;
    client.sendMessage(settings, messages);
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 2, 5000);
    const QJsonObject withThinking = QJsonDocument::fromJson(server.capturedBodies.at(1)).object();
    QCOMPARE(withThinking.value(QStringLiteral("enable_thinking")).toBool(), true);
    // 其余字段不受影响，思考开关只是附加项。
    QCOMPARE(withThinking.value(QStringLiteral("model")).toString(),
             QStringLiteral("test-model"));
}

void AiAssistantWidgetTest::fetchModelsParsesGatewayModelIds()
{
    MiniAiServer server;
    QVERIFY(server.isListening());
    // 重复 id 只保留一条：有的网关会把同一个模型按多个 owner 列两次，下拉框里出现两个
    // 一模一样的条目只会让人怀疑自己选错了。
    server.responses.append(QByteArrayLiteral(
        R"({"data":[{"id":"alpha"},{"id":"beta"},{"id":"alpha"},{"id":""}]})"));

    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());

    AiClient client;
    QSignalSpy spy(&client, &AiClient::modelsFetched);
    client.fetchModels(settings);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
    QVERIFY(spy.at(0).at(0).toBool());
    QCOMPARE(spy.at(0).at(1).toStringList(), QStringList({QStringLiteral("alpha"),
                                                          QStringLiteral("beta")}));

    // 网关返回空列表时必须报错而不是"成功但没有模型"，否则用户会以为下拉框是界面卡了。
    server.responses.append(QByteArrayLiteral(R"({"data":[]})"));
    client.fetchModels(settings);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 2, 5000);
    QVERIFY(!spy.at(1).at(0).toBool());
    QVERIFY(spy.at(1).at(1).toStringList().isEmpty());
}

/**
 * @brief 深色主题下 AI 助手面板必须整体转深，而不是留一块白底。
 *
 * 面板样式表和每条消息的内联样式原先都写死浅色，主题切到深色时只有主窗口跟着变，侧栏里的
 * AI 页面仍是白底黑字。这里在浅色主题下先渲染出一条用户消息，再换上深色应用调色板，断言
 * 面板背景和已经存在的气泡都重新取了深色——只测面板不测气泡的话，"新消息变深、旧消息还白"
 * 这种更常见的半吊子实现会被漏掉。
 */
void AiAssistantWidgetTest::panelFollowsApplicationThemeToDark()
{
    MiniAiServer server;
    QVERIFY(server.isListening());
    server.responses.append(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"complete","message":"当前目标已完成。"})JSON")));

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());
    settings.model = QStringLiteral("test-model");
    // 这些用例覆盖的是"用户点击执行"的手动路径，固定为逐条确认，不受自动执行默认值影响。
    settings.executionPolicy = AiExecutionPolicy::Manual;
    QVERIFY(settingsService.save(settings));

    const QPalette originalPalette = QGuiApplication::palette();
    QPalette lightPalette = originalPalette;
    lightPalette.setColor(QPalette::Window, QColor(QStringLiteral("#f2f4f7")));
    QApplication::setPalette(lightPalette);

    AiAssistantWidget widget(&settingsService);
    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    QVERIFY(inputEdit != nullptr);
    QVERIFY(sendButton != nullptr);
    QVERIFY(widget.styleSheet().contains(QStringLiteral("#fbfcfd")));

    inputEdit->setPlainText(QStringLiteral("看看磁盘占用"));
    sendButton->click();
    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(aiIsIdle(sendButton), 5000);

    const auto userBubbleStyle = [&widget]() {
        auto *bubble = widget.findChild<QLabel *>(QStringLiteral("AiUserMessageBubble"));
        return bubble == nullptr ? QString() : bubble->styleSheet();
    };
    QVERIFY(userBubbleStyle().contains(QStringLiteral("#e6f2ec")));

    QPalette darkPalette = originalPalette;
    darkPalette.setColor(QPalette::Window, QColor(QStringLiteral("#20232a")));
    QApplication::setPalette(darkPalette);

    QTRY_VERIFY_WITH_TIMEOUT(widget.styleSheet().contains(QStringLiteral("#191f31")), 2000);
    QVERIFY(!widget.styleSheet().contains(QStringLiteral("#fbfcfd")));
    // 换主题后旧气泡是重建出来的，findChild 会拿到新控件；等待一轮事件循环让 deleteLater
    // 释放旧对象，避免断言命中即将销毁的浅色气泡。
    QTRY_VERIFY_WITH_TIMEOUT(userBubbleStyle().contains(QStringLiteral("#24413d")), 2000);
    QVERIFY(!userBubbleStyle().contains(QStringLiteral("#e6f2ec")));

    QApplication::setPalette(originalPalette);
}

void AiAssistantWidgetTest::chatChromeFollowsLanguageSwitch()
{
    MiniAiServer server;
    QVERIFY(server.isListening());
    server.responses.append(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"complete","message":"检查完成。"})JSON")));

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());
    settings.model = QStringLiteral("test-model");
    // 这些用例覆盖的是"用户点击执行"的手动路径，固定为逐条确认，不受自动执行默认值影响。
    settings.executionPolicy = AiExecutionPolicy::Manual;
    QVERIFY(settingsService.save(settings));

    LocalizationService localization;
    QVERIFY(localization.apply(QStringLiteral("en")));

    AiAssistantWidget widget(&settingsService);
    auto *attachCheck = widget.findChild<QCheckBox *>();
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    QVERIFY(attachCheck != nullptr);
    QVERIFY(sendButton != nullptr);
    QVERIFY(inputEdit != nullptr);
    QCOMPARE(attachCheck->text(), QStringLiteral("Include new terminal output"));

    // 先渲染一张命令卡片：它的固定标签也是构造时按当时语言写死的，语言切换后必须一起重译。
    server.responses.prepend(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"command","title":"看看磁盘","reason":"先读取现状。","command":"df -h"})JSON")));
    inputEdit->setPlainText(QStringLiteral("看看磁盘占用"));
    sendButton->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        widget.findChild<QPushButton *>(QStringLiteral("aiExecuteButton")) != nullptr, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(aiIsIdle(sendButton), 5000);

    QVERIFY(localization.apply(QStringLiteral("zh_CN")));
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();

    QCOMPARE(attachCheck->text(), QString::fromUtf8("附带终端新增输出"));
    auto *executeButton = widget.findChild<QPushButton *>(QStringLiteral("aiExecuteButton"));
    QVERIFY(executeButton != nullptr);
    QCOMPARE(executeButton->text(), QString::fromUtf8("执行  ⌘↵"));

    QVERIFY(localization.apply(QStringLiteral("en")));
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    QCOMPARE(attachCheck->text(), QStringLiteral("Include new terminal output"));
}


void AiAssistantWidgetTest::riskClassifierGatesAutoExecution()
{
    const QStringList autoRunnable = {
        QStringLiteral("df -h"),
        QStringLiteral("systemctl status nginx"),
        QStringLiteral("cat /etc/os-release"),
        QStringLiteral("git log --oneline"),
        QStringLiteral("python3 --version"),
        QStringLiteral("dpkg -l nginx"),
        QStringLiteral("ping -c 3 example.com"),
        QStringLiteral("journalctl -u nginx --no-pager -n 50"),
        QStringLiteral("top -b -n 1"),
        QStringLiteral("mount"),
    };
    for (const QString &command : autoRunnable) {
        QVERIFY2(CommandRiskClassifier::isSafeForAutoExecution(command), qPrintable(command));
    }

    const QStringList needsApproval = {
        QStringLiteral("python3 -c \"import os; os.remove('x')\""),
        QStringLiteral("git push origin main"),
        QStringLiteral("git branch -D feature"),
        QStringLiteral("dpkg -i package.deb"),
        QStringLiteral("rpm -e nginx"),
        QStringLiteral("nginx -s stop"),
        QStringLiteral("mount /dev/sdb1 /mnt"),
        QStringLiteral("echo $(rm -rf /tmp/data)"),
        QStringLiteral("cat `which rm`"),
        QStringLiteral("find /tmp -name '*.log' -delete"),
        QStringLiteral("tail -f /var/log/syslog"),
        QStringLiteral("journalctl -fu nginx"),
        QStringLiteral("top"),
        QStringLiteral("less /etc/hosts"),
        QStringLiteral("ping example.com"),
        QStringLiteral("sudo cat /etc/shadow"),
        QStringLiteral("apt-get install -y nginx"),
        QStringLiteral("echo hi > /etc/motd"),
        QStringLiteral("rm -rf /"),
    };
    for (const QString &command : needsApproval) {
        QVERIFY2(!CommandRiskClassifier::isSafeForAutoExecution(command), qPrintable(command));
    }

    QCOMPARE(CommandRiskClassifier::classify(QStringLiteral("git push")), CommandRiskLevel::Write);
    QCOMPARE(CommandRiskClassifier::classify(QStringLiteral("python3 script.py")),
             CommandRiskLevel::Write);
    QCOMPARE(CommandRiskClassifier::classify(QStringLiteral("echo $(id)")), CommandRiskLevel::Write);
    QCOMPARE(CommandRiskClassifier::classify(QStringLiteral("reboot")), CommandRiskLevel::HighRisk);
    // 交互式命令本身仍是只读，只是不能无人值守地跑。
    QCOMPARE(CommandRiskClassifier::classify(QStringLiteral("top")), CommandRiskLevel::ReadOnly);
}

void AiAssistantWidgetTest::autoReadOnlyRunsSafeCommandWithoutClick()
{
    MiniAiServer server;
    QVERIFY(server.isListening());
    server.responses.append(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"command","title":"查看磁盘","reason":"先读取磁盘占用。","command":"df -h"})JSON")));
    server.responses.append(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"complete","message":"磁盘空间充足。"})JSON")));

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());
    settings.model = QStringLiteral("test-model");
    settings.executionPolicy = AiExecutionPolicy::AutoReadOnly;
    QVERIFY(settingsService.save(settings));

    AiAssistantWidget widget(&settingsService);
    QString terminalText = QStringLiteral("[user@host ~]$ ");
    widget.setTerminalContextProvider([&terminalText]() { return terminalText; });
    QSignalSpy execSpy(&widget, &AiAssistantWidget::commandExecutionRequested);
    connect(&widget, &AiAssistantWidget::commandExecutionRequested, &widget,
            [&terminalText](const QString &command) {
                terminalText += command + QStringLiteral("\n/dev/sda1  50G  10G  40G  20% /\n[user@host ~]$ ");
            });

    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    inputEdit->setPlainText(QStringLiteral("看看磁盘"));
    sendButton->click();

    // 没有任何点击，命令就应当被下发。
    QTRY_COMPARE_WITH_TIMEOUT(execSpy.count(), 1, 5000);
    QCOMPARE(execSpy.first().at(0).toString(), QStringLiteral("df -h"));
    auto *executeButton = widget.findChild<QPushButton *>(QStringLiteral("aiExecuteButton"));
    QVERIFY(executeButton != nullptr);
    QVERIFY(!executeButton->isEnabled());
    bool sawAutoStatus = false;
    for (QLabel *label : widget.findChildren<QLabel *>()) {
        sawAutoStatus = sawAutoStatus || label->text() == QStringLiteral("Ran automatically");
    }
    QVERIFY2(sawAutoStatus, "自动执行的卡片应标明是自动下发的，而不是用户点的");

    // 自动执行同样要等提示符回来再带着真实输出续问。
    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), 2, 5000);
    const QString lastMessage = requestMessages(server.capturedBodies.at(1))
                                    .last().toObject().value(QStringLiteral("content")).toString();
    QVERIFY2(lastMessage.contains(QStringLiteral("/dev/sda1")), qPrintable(lastMessage));
    QTRY_VERIFY_WITH_TIMEOUT(aiIsIdle(sendButton), 5000);
    QCOMPARE(execSpy.count(), 1);
}

void AiAssistantWidgetTest::autoReadOnlyStillWaitsForWriteCommands()
{
    MiniAiServer server;
    QVERIFY(server.isListening());
    server.responses.append(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"command","title":"安装 Nginx","reason":"确认未安装后执行安装。","command":"apt-get install -y nginx"})JSON")));

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());
    settings.model = QStringLiteral("test-model");
    settings.executionPolicy = AiExecutionPolicy::AutoReadOnly;
    QVERIFY(settingsService.save(settings));

    AiAssistantWidget widget(&settingsService);
    widget.setTerminalContextProvider([]() { return QStringLiteral("[user@host ~]$ "); });
    QSignalSpy execSpy(&widget, &AiAssistantWidget::commandExecutionRequested);

    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    inputEdit->setPlainText(QStringLiteral("装一下 nginx"));
    sendButton->click();

    QTRY_VERIFY_WITH_TIMEOUT(
        widget.findChild<QPushButton *>(QStringLiteral("aiExecuteButton")) != nullptr, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(aiIsIdle(sendButton), 5000);
    QTest::qWait(200);
    QCOMPARE(execSpy.count(), 0);
    QVERIFY(widget.findChild<QPushButton *>(QStringLiteral("aiExecuteButton"))->isEnabled());
}

void AiAssistantWidgetTest::fullAutoRunsHighRiskCommandWithoutPrompt()
{
    MiniAiServer server;
    QVERIFY(server.isListening());
    server.responses.append(chatCompletionBody(QStringLiteral(
        R"JSON({"state":"command","title":"重启主机","reason":"内核更新后需要重启。","command":"reboot"})JSON")));

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());
    settings.model = QStringLiteral("test-model");
    settings.executionPolicy = AiExecutionPolicy::FullAuto;
    QVERIFY(settingsService.save(settings));

    AiAssistantWidget widget(&settingsService);
    widget.setTerminalContextProvider([]() { return QStringLiteral("[user@host ~]$ "); });
    QSignalSpy execSpy(&widget, &AiAssistantWidget::commandExecutionRequested);
    ModalResponder responder(false);

    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    inputEdit->setPlainText(QStringLiteral("更新完内核后重启"));
    sendButton->click();

    QTRY_COMPARE_WITH_TIMEOUT(execSpy.count(), 1, 5000);
    QCOMPARE(execSpy.first().at(0).toString(), QStringLiteral("reboot"));
    QVERIFY2(!responder.sawModal, "全自动模式不应再弹高风险确认框");

    // 自动批准只作用于那一次：手动点击历史卡片之外的新卡片仍走确认（这里按钮已锁定）。
    auto *executeButton = widget.findChild<QPushButton *>(QStringLiteral("aiExecuteButton"));
    QVERIFY(!executeButton->property("aiAutoApproved").toBool());
}

void AiAssistantWidgetTest::autoExecutionPausesAtStepLimit()
{
    constexpr int kLimit = 20;
    MiniAiServer server;
    QVERIFY(server.isListening());
    for (int i = 0; i <= kLimit; ++i) {
        server.responses.append(chatCompletionBody(QStringLiteral(
            R"JSON({"state":"command","title":"再看一次","reason":"继续确认状态。","command":"uptime"})JSON")));
    }

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.endpointUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.port());
    settings.model = QStringLiteral("test-model");
    settings.executionPolicy = AiExecutionPolicy::AutoReadOnly;
    QVERIFY(settingsService.save(settings));

    AiAssistantWidget widget(&settingsService);
    QString terminalText = QStringLiteral("[user@host ~]$ ");
    widget.setTerminalContextProvider([&terminalText]() { return terminalText; });
    QSignalSpy execSpy(&widget, &AiAssistantWidget::commandExecutionRequested);
    connect(&widget, &AiAssistantWidget::commandExecutionRequested, &widget,
            [&terminalText](const QString &command) {
                terminalText += command + QStringLiteral("\n up 3 days\n[user@host ~]$ ");
            });

    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    inputEdit->setPlainText(QStringLiteral("一直检查"));
    sendButton->click();

    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), kLimit + 1, 60000);
    QTRY_VERIFY_WITH_TIMEOUT(aiIsIdle(sendButton), 5000);
    QCOMPARE(execSpy.count(), kLimit);
    bool sawPause = false;
    for (QLabel *label : widget.findChildren<QLabel *>()) {
        sawPause = sawPause || label->text().contains(QStringLiteral("Paused after 20 automatic steps"));
    }
    QVERIFY(sawPause);
    // 最后一张卡片停在待确认状态，用户仍可手动继续。
    QVERIFY(widget.findChildren<QPushButton *>(QStringLiteral("aiExecuteButton")).last()->isEnabled());
}

void AiAssistantWidgetTest::executionModeMenuPersistsPolicy()
{
    AiSettingsService settingsService(nullptr, nullptr);
    QCOMPARE(settingsService.currentSettings().executionPolicy, AiExecutionPolicy::AutoReadOnly);
    AiAssistantWidget widget(&settingsService);
    auto *modeButton = widget.findChild<QToolButton *>(QStringLiteral("aiExecutionModeButton"));
    auto *manual = widget.findChild<QAction *>(QStringLiteral("aiModeManual"));
    auto *fullAuto = widget.findChild<QAction *>(QStringLiteral("aiModeFullAuto"));
    QVERIFY(modeButton != nullptr && manual != nullptr && fullAuto != nullptr);
    QVERIFY(modeButton->text().startsWith(QStringLiteral("Auto read-only")));

    manual->trigger();
    QCOMPARE(settingsService.currentSettings().executionPolicy, AiExecutionPolicy::Manual);
    QVERIFY(modeButton->text().startsWith(QStringLiteral("Ask first")));
    QVERIFY(manual->isChecked());

    {
        ModalResponder decline(false);
        fullAuto->trigger();
        QVERIFY2(decline.sawModal, "切到全自动前必须弹出风险确认");
    }
    QCOMPARE(settingsService.currentSettings().executionPolicy, AiExecutionPolicy::Manual);
    QVERIFY(manual->isChecked());

    {
        ModalResponder accept(true);
        fullAuto->trigger();
        QVERIFY(accept.sawModal);
    }
    QCOMPARE(settingsService.currentSettings().executionPolicy, AiExecutionPolicy::FullAuto);
    QVERIFY(modeButton->text().startsWith(QStringLiteral("Full auto")));
    auto *disclaimer = widget.findChild<QLabel *>(QStringLiteral("aiDisclaimerLabel"));
    QVERIFY(disclaimer != nullptr);
    QVERIFY(disclaimer->text().startsWith(QStringLiteral("Full auto")));
}

void AiAssistantWidgetTest::detectorFindsAgentsInGivenDirectories()
{
    QTemporaryDir first;
    QTemporaryDir second;
    QVERIFY(first.isValid() && second.isValid());
    const QString codex = writeFakeCli(first, QStringLiteral("codex"),
                                       "echo 'codex-cli 9.9.9'\n");
    const QString cursor = writeFakeCli(second, QStringLiteral("cursor-agent"), "echo 1.0\n");
    // 同名但不可执行的文件不能被当成已安装。
    QFile notExecutable(first.filePath(QStringLiteral("gemini")));
    QVERIFY(notExecutable.open(QIODevice::WriteOnly));
    notExecutable.write("#!/bin/sh\n");
    notExecutable.close();
    QVERIFY(!codex.isEmpty() && !cursor.isEmpty());

    const QList<LocalAgentCli::DetectedAgent> agents =
        LocalAgentCli::findAgents({first.path(), second.path()});
    QCOMPARE(agents.size(), 2);
    // 结果按 catalog() 顺序排列：codex 在 cursor-agent 之前。
    QCOMPARE(agents.at(0).id, QStringLiteral("codex"));
    QCOMPARE(QFileInfo(agents.at(0).executablePath).canonicalFilePath(),
             QFileInfo(codex).canonicalFilePath());
    QCOMPARE(agents.at(1).id, QStringLiteral("cursor-agent"));

    // 空目录列表不能回落去搜当前进程 PATH，否则结果就不再由调用方控制。
    QVERIFY(LocalAgentCli::findAgents({}).isEmpty());

    const QString version = LocalAgentCli::probeVersion(
        codex, LocalAgentCli::childEnvironment({first.path()}));
    QCOMPARE(version, QStringLiteral("codex-cli 9.9.9"));

    // 子进程 PATH 必须被替换成搜索目录，npm 安装的 CLI 才能找到 node。
    const QProcessEnvironment environment =
        LocalAgentCli::childEnvironment({first.path(), second.path()});
    QCOMPARE(environment.value(QStringLiteral("PATH")),
             first.path() + QDir::listSeparator() + second.path());

    // 搜索目录合并登录 Shell PATH，去重并剔除不存在的目录。
    const QStringList directories = LocalAgentCli::searchDirectories(
        first.path() + QDir::listSeparator() + QStringLiteral("/definitely/missing")
        + QDir::listSeparator() + first.path());
    QCOMPARE(directories.first(), QDir::cleanPath(first.path()));
    QCOMPARE(directories.count(QDir::cleanPath(first.path())), 1);
    QVERIFY(!directories.contains(QStringLiteral("/definitely/missing")));
}

void AiAssistantWidgetTest::cliInvocationsStayReadOnly()
{
    const QString prompt = QStringLiteral("PROMPT");
    const auto invocationFor = [&prompt](const QString &id) {
        const std::optional<LocalAgentCli::AgentSpec> spec = LocalAgentCli::specById(id);
        Q_ASSERT(spec.has_value());
        return LocalAgentCli::buildInvocation(*spec, QStringLiteral("/bin/") + id,
                                              QStringLiteral("some-model"), prompt,
                                              QStringLiteral("/tmp/reply.txt"));
    };

    const LocalAgentCli::Invocation claude = invocationFor(QStringLiteral("claude"));
    // 实时展示分析过程依赖 claude 按增量事件输出；少了任何一个参数都只会在结束时吐一次。
    const int formatIndex = claude.arguments.indexOf(QStringLiteral("--output-format"));
    QVERIFY(formatIndex >= 0);
    QCOMPARE(claude.arguments.at(formatIndex + 1), QStringLiteral("stream-json"));
    QVERIFY(claude.arguments.contains(QStringLiteral("--verbose")));
    QVERIFY(claude.arguments.contains(QStringLiteral("--include-partial-messages")));
    QVERIFY(claude.outputFormat == LocalAgentCli::OutputFormat::ClaudeStreamJson);
    const int toolsIndex = claude.arguments.indexOf(QStringLiteral("--tools"));
    QVERIFY(toolsIndex >= 0 && toolsIndex + 1 < claude.arguments.size());
    QVERIFY2(claude.arguments.at(toolsIndex + 1).isEmpty(), "claude 必须以 --tools \"\" 关掉全部工具");
    QVERIFY(claude.arguments.contains(QStringLiteral("-p")));
    QVERIFY(!claude.arguments.contains(prompt));
    QCOMPARE(claude.standardInput, prompt.toUtf8());

    const LocalAgentCli::Invocation codex = invocationFor(QStringLiteral("codex"));
    const int sandboxIndex = codex.arguments.indexOf(QStringLiteral("--sandbox"));
    QVERIFY(sandboxIndex >= 0);
    QCOMPARE(codex.arguments.at(sandboxIndex + 1), QStringLiteral("read-only"));
    QCOMPARE(codex.arguments.last(), QStringLiteral("-"));
    QCOMPARE(codex.outputFilePath, QStringLiteral("/tmp/reply.txt"));
    QCOMPARE(codex.standardInput, prompt.toUtf8());

    const LocalAgentCli::Invocation cursor = invocationFor(QStringLiteral("cursor-agent"));
    const int modeIndex = cursor.arguments.indexOf(QStringLiteral("--mode"));
    QVERIFY(modeIndex >= 0);
    QCOMPARE(cursor.arguments.at(modeIndex + 1), QStringLiteral("ask"));
    QCOMPARE(cursor.arguments.last(), prompt);

    const LocalAgentCli::Invocation gemini = invocationFor(QStringLiteral("gemini"));
    QCOMPARE(gemini.arguments.at(gemini.arguments.size() - 2), QStringLiteral("-p"));
    QCOMPARE(gemini.arguments.last(), prompt);

    for (const LocalAgentCli::AgentSpec &spec : LocalAgentCli::catalog()) {
        const LocalAgentCli::Invocation invocation = invocationFor(spec.id);
        const int modelIndex = invocation.arguments.indexOf(QStringLiteral("--model"));
        QVERIFY2(modelIndex >= 0, qPrintable(spec.id));
        QCOMPARE(invocation.arguments.at(modelIndex + 1), QStringLiteral("some-model"));
        // 任何 CLI 都不能被带上"跳过审批 / 全权限"一类参数。
        for (const QString &argument : invocation.arguments) {
            QVERIFY2(!argument.contains(QStringLiteral("dangerously"))
                         && argument != QStringLiteral("--force")
                         && argument != QStringLiteral("--yolo")
                         && argument != QStringLiteral("--full-auto"),
                     qPrintable(spec.id + QStringLiteral(": ") + argument));
        }
    }

    const QString transcript = LocalAgentCli::renderTranscript(
        {AiChatMessage{AiChatRole::System, QStringLiteral("RULES")},
         AiChatMessage{AiChatRole::User, QStringLiteral("TASK")}});
    QVERIFY(transcript.indexOf(QStringLiteral("### SYSTEM\nRULES"))
            < transcript.indexOf(QStringLiteral("### USER\nTASK")));
    QVERIFY(transcript.endsWith(QStringLiteral("### ASSISTANT\n")));
    QVERIFY(transcript.contains(QStringLiteral("Do NOT use any tools")));
}

void AiAssistantWidgetTest::cliReplyExtraction()
{
    const QString object = QStringLiteral(R"({"state":"complete","message":"a {b} \"c\""})");
    QCOMPARE(LocalAgentCli::extractReply(object), object);
    QCOMPARE(LocalAgentCli::extractReply(QStringLiteral("\x1b[32m") + object
                                         + QStringLiteral("\x1b[0m\n")),
             object);
    QCOMPARE(LocalAgentCli::extractReply(QStringLiteral("Sure!\n```json\n") + object
                                         + QStringLiteral("\n```\n")),
             object);
    QCOMPARE(LocalAgentCli::extractReply(QStringLiteral("Here you go: ") + object
                                         + QStringLiteral(" hope it helps")),
             object);
    // 两个顶层对象意味着模型一次给了多步，不能替它挑一个。
    const QString twoObjects = object + QLatin1Char('\n') + object;
    QCOMPARE(LocalAgentCli::extractReply(twoObjects), twoObjects);
}

void AiAssistantWidgetTest::localCliBackendDrivesAgentLoop()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 冒充 claude 的 stream-json 输出：记录参数与标准输入，先吐出思考过程和半截回答，
    // 等测试放行（创建 go 文件）后再补齐，用来确认界面在进程结束前就在实时展示。最终结果
    // 故意带上 Markdown 围栏，确认 result 事件同样经过 extractReply 整理。
    const QByteArray script =
        "dir=$(dirname \"$0\")\n"
        "printf '%s\\n' \"$@\" > \"$dir/args.txt\"\n"
        "cat > \"$dir/stdin.txt\"\n"
        "printf '%s\\n' '{\"type\":\"system\",\"subtype\":\"init\"}'\n"
        "printf '%s\\n' '{\"type\":\"stream_event\",\"event\":{\"type\":\"message_start\"}}'\n"
        "printf '%s\\n' '{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
        "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"用户关心磁盘余量\"}}}'\n"
        "printf '%s\\n' '{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"{\\\"state\\\":\\\"command\\\","
        "\\\"title\\\":\\\"查看磁盘\\\",\\\"reason\\\":\\\"先确认\"}}}'\n"
        "while [ ! -f \"$dir/go\" ]; do sleep 0.05; done\n"
        "printf '%s\\n' '{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,"
        "\"result\":\"```json\\n{\\\"state\\\":\\\"command\\\",\\\"title\\\":\\\"查看磁盘\\\","
        "\\\"reason\\\":\\\"先确认剩余空间。\\\",\\\"command\\\":\\\"df -h\\\"}\\n```\"}'\n";
    const QString fakeClaude = writeFakeCli(dir, QStringLiteral("claude"), script);
    QVERIFY(!fakeClaude.isEmpty());

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.backend = AiBackend::LocalCli;
    settings.cliAgentId = QStringLiteral("claude");
    settings.cliExecutablePath = fakeClaude;
    settings.executionPolicy = AiExecutionPolicy::Manual;
    QVERIFY(settings.isConfigured());
    QVERIFY(settingsService.save(settings));

    AiAssistantWidget widget(&settingsService);
    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    inputEdit->setPlainText(QStringLiteral("服务器磁盘还够吗"));
    sendButton->click();

    // 首次调用可能要先查询登录 Shell 的 PATH，给足时间。进程还没结束时，思考过程和
    // 已到达的半截理由就必须出现在实时面板里。
    const auto labelText = [&widget](const QString &name) {
        const QLabel *label = widget.findChild<QLabel *>(name);
        return label != nullptr && label->isVisible() ? label->text() : QString();
    };
    widget.resize(420, 640);
    widget.show();
    QTRY_VERIFY_WITH_TIMEOUT(
        labelText(QStringLiteral("aiThinkingReasoning")).contains(QStringLiteral("用户关心磁盘余量")),
        20000);
    QTRY_VERIFY_WITH_TIMEOUT(
        labelText(QStringLiteral("aiThinkingDraft")).contains(QStringLiteral("先确认")), 5000);
    QVERIFY(labelText(QStringLiteral("aiThinkingDraft")).contains(QStringLiteral("查看磁盘")));
    QVERIFY(widget.findChild<QPushButton *>(QStringLiteral("aiExecuteButton")) == nullptr);
    QFile goFile(dir.filePath(QStringLiteral("go")));
    QVERIFY(goFile.open(QIODevice::WriteOnly));
    goFile.close();

    QTRY_VERIFY_WITH_TIMEOUT(
        widget.findChild<QPushButton *>(QStringLiteral("aiExecuteButton")) != nullptr, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(aiIsIdle(sendButton), 5000);

    const QString stdinText = readTextFile(dir.filePath(QStringLiteral("stdin.txt")));
    QVERIFY2(stdinText.contains(QStringLiteral("### SYSTEM")), qPrintable(stdinText.left(200)));
    QVERIFY(stdinText.contains(QStringLiteral("单步")));
    QVERIFY(stdinText.contains(QStringLiteral("服务器磁盘还够吗")));
    const QStringList args = readTextFile(dir.filePath(QStringLiteral("args.txt")))
                                 .split(QLatin1Char('\n'));
    const int toolsIndex = args.indexOf(QStringLiteral("--tools"));
    QVERIFY(toolsIndex >= 0);
    QVERIFY(args.at(toolsIndex + 1).isEmpty());

    bool sawReason = false;
    for (QLabel *label : widget.findChildren<QLabel *>()) {
        sawReason = sawReason || label->text().contains(QStringLiteral("先确认剩余空间"));
    }
    QVERIFY(sawReason);
}

void AiAssistantWidgetTest::localCliFailureSurfacesStderr()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString fakeCodex = writeFakeCli(
        dir, QStringLiteral("codex"),
        "cat > /dev/null\necho 'Not logged in. Run codex login first.' >&2\nexit 3\n");
    QVERIFY(!fakeCodex.isEmpty());

    AiSettingsService settingsService(nullptr, nullptr);
    AiSettings settings;
    settings.backend = AiBackend::LocalCli;
    settings.cliAgentId = QStringLiteral("codex");
    settings.cliExecutablePath = fakeCodex;
    QVERIFY(settingsService.save(settings));

    AiAssistantWidget widget(&settingsService);
    QSignalSpy statusSpy(&widget, &AiAssistantWidget::statusMessage);
    auto *inputEdit = widget.findChild<QTextEdit *>(QStringLiteral("aiInputEdit"));
    auto *sendButton = widget.findChild<QPushButton *>(QStringLiteral("aiSendButton"));
    inputEdit->setPlainText(QStringLiteral("看看负载"));
    sendButton->click();

    QTRY_VERIFY_WITH_TIMEOUT(!statusSpy.isEmpty(), 20000);
    const QString message = statusSpy.last().at(0).toString();
    QVERIFY2(message.contains(QStringLiteral("Not logged in")), qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("3")), qPrintable(message));
    QTRY_VERIFY_WITH_TIMEOUT(aiIsIdle(sendButton), 5000);
}

QTEST_MAIN(AiAssistantWidgetTest)
#include "AiAssistantWidgetTest.moc"
