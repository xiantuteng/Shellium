#include "ai/AiAssistantWidget.h"
#include "ai/AiChatTypes.h"
#include "ai/AiClient.h"
#include "ai/AiSettings.h"
#include "ai/AiSettingsService.h"

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
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSharedPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
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
};

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
    QCOMPARE(executeButton->text(), QStringLiteral("已执行"));

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
    QVERIFY(waitingLabel->text().contains(QStringLiteral("正在等待命令执行完成")));
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
    QVERIFY2(lastMessage.contains(QStringLiteral("[命令 `which nginx` 的执行结果]")),
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
        QStringLiteral("违反了单步协议")));
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
    QCOMPARE(statusSpy.last().at(0).toString(), QStringLiteral("命令已复制到剪贴板"));

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
    QTRY_VERIFY(labelWithText(QStringLiteral("正在分析")));

    // 等待期间按钮不能是灰的：它必须就地变成停止入口，否则用户只能干等或关掉整个标签。
    QVERIFY(sendButton->isEnabled());
    QCOMPARE(sendButton->property("aiButtonMode").toString(), QStringLiteral("stop"));

    sendButton->click();

    QTRY_VERIFY2(!labelWithText(QStringLiteral("正在分析")), "停止后等待指示器必须消失");
    QVERIFY(labelWithText(QStringLiteral("已停止本次分析")));
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

QTEST_MAIN(AiAssistantWidgetTest)
#include "AiAssistantWidgetTest.moc"
