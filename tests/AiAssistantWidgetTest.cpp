#include "ai/AiAssistantWidget.h"
#include "ai/AiSettings.h"
#include "ai/AiSettingsService.h"

#include <QApplication>
#include <QCheckBox>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSharedPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTextEdit>

namespace {

QByteArray chatCompletionBody(const QString &content)
{
    const QJsonObject message{{QStringLiteral("role"), QStringLiteral("assistant")},
                              {QStringLiteral("content"), content}};
    const QJsonObject choice{{QStringLiteral("message"), message}};
    const QJsonObject root{{QStringLiteral("choices"), QJsonArray{choice}}};
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
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
 * 给用户，从不读取真实执行结果；现在改成轮询 terminalPlainText() 快照，稳定后把差量文本
 * 整理成"[命令 ... 的执行结果]"自动发给模型。这里用一个手写的极简 HTTP 服务器顶替真实网关，
 * 断言第二次请求体里确实带着"命令执行后新增的那部分文本"，而不是完整历史或空上下文。
 */
class AiAssistantWidgetTest final : public QObject {
    Q_OBJECT

private slots:
    void executingCommandAutoCapturesOutputAndContinuesConversation();
    void newSessionClearsHistoryAndReadPosition();
    void assistantNarrativeRendersAsMarkdown();
};

void AiAssistantWidgetTest::executingCommandAutoCapturesOutputAndContinuesConversation()
{
    MiniAiServer server;
    QVERIFY(server.isListening());
    server.responses.append(chatCompletionBody(
        QStringLiteral("先检查是否已安装：\n```bash\nwhich nginx\n```")));
    server.responses.append(chatCompletionBody(QStringLiteral("已确认未安装，任务先到这里。")));

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

    // 模拟命令已经在终端里真实跑完：新增一段输出，随后不再变化。
    terminalText += QStringLiteral("which nginx\nnginx not found\n[user@host ~]$ ");

    QTRY_COMPARE_WITH_TIMEOUT(server.capturedBodies.size(), 2, 5000);

    const QJsonArray secondMessages = requestMessages(server.capturedBodies.at(1));
    QVERIFY(!secondMessages.isEmpty());
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
    QTRY_VERIFY_WITH_TIMEOUT(sendButton->isEnabled(), 5000);
    // 第二轮回复没有代码块，不应该再多出一张可执行命令卡片。
    QCOMPARE(widget.findChildren<QPushButton *>(QStringLiteral("aiExecuteButton")).size(), 1);

    // 真实命令输出只喂给模型，不应该在聊天区里额外冒出一条"已读取执行结果"气泡——那是
    // 改版前的做法，现在应该完全无痕。
    for (QLabel *label : widget.findChildren<QLabel *>()) {
        QVERIFY2(!label->text().contains(QStringLiteral("执行结果")), qPrintable(label->text()));
    }
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
    server.responses.append(chatCompletionBody(QStringLiteral("好的，已经记下这个任务。")));
    server.responses.append(chatCompletionBody(QStringLiteral("这是全新会话里的第一条回复。")));

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
    QTRY_VERIFY_WITH_TIMEOUT(sendButton->isEnabled(), 5000);

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
    server.responses.append(
        chatCompletionBody(QStringLiteral("**已完成**，共 3 步：\n1. 检查\n2. 安装\n3. 验证")));

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
    QTRY_VERIFY_WITH_TIMEOUT(sendButton->isEnabled(), 5000);

    bool foundMarkdownBubble = false;
    for (QLabel *label : widget.findChildren<QLabel *>()) {
        if (label->text().contains(QStringLiteral("已完成"))) {
            foundMarkdownBubble = true;
            QCOMPARE(label->textFormat(), Qt::MarkdownText);
        }
    }
    QVERIFY(foundMarkdownBubble);
}

QTEST_MAIN(AiAssistantWidgetTest)
#include "AiAssistantWidgetTest.moc"
