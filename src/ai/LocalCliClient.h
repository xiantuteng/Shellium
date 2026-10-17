#pragma once

#include "ai/AiChatTypes.h"
#include "ai/AiSettings.h"
#include "ai/LocalAgentCli.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <memory>

class QProcess;
class QTemporaryDir;
class QTimer;
template <typename T>
class QFutureWatcher;

/**
 * @brief 通过本机智能体 CLI（claude -p、codex exec 等）完成一次对话决策。
 *
 * 对外接口刻意与 AiClient 的对话部分保持一致（sendMessage / abortActiveRequest 加
 * replyFinished / replyFailed），AiAssistantWidget 可以按设置在两者之间切换而不改协议
 * 处理逻辑。以 stream-json 输出的 CLI（目前是 claude）还会像 AiClient 一样发出
 * replyChunkReceived / reasoningChunkReceived 供界面实时展示；其余 CLI 只在结束时给出结果。
 *
 * 每次请求启动一个新进程，工作目录是一个专用的空临时目录，并按 LocalAgentCli::
 * buildInvocation() 关闭工具或进入只读模式——CLI 只负责给出下一步，执行永远发生在
 * Shellium 的终端里。
 */
class LocalCliClient final : public QObject {
    Q_OBJECT

public:
    explicit LocalCliClient(QObject *parent = nullptr);
    ~LocalCliClient() override;

    /**
     * @brief 发起一次对话决策。
     * @param settings 需要 backend 为 LocalCli，cliAgentId 指向 catalog() 中的条目。
     * @param messages 完整对话，含系统规则与本轮输入。
     *
     * 首次调用时若可执行文件路径未知且搜索目录尚未缓存，会先在工作线程查询登录 Shell，
     * 界面不会因此卡住。
     */
    void sendMessage(const AiSettings &settings, const QList<AiChatMessage> &messages);

    /**
     * @brief 终止进行中的 CLI 进程；之后不再发出 replyFinished / replyFailed。
     *
     * 语义与 AiClient::abortActiveRequest() 相同：用户主动停止不是失败，界面收尾由调用方负责。
     */
    void abortActiveRequest();

    /** @brief 是否有请求正在进行（包括定位可执行文件阶段）。 */
    [[nodiscard]] bool isBusy() const;

    /** @brief 单次请求的超时上限，默认 5 分钟；测试用它缩短等待。 */
    void setTimeoutMs(int timeoutMs);

signals:
    /** @brief 正文增量到达；参数是到目前为止的完整正文，语义同 AiClient::replyChunkReceived。 */
    void replyChunkReceived(const QString &accumulatedContent);

    /** @brief 思考过程增量到达；语义同 AiClient::reasoningChunkReceived。 */
    void reasoningChunkReceived(const QString &accumulatedReasoning);

    /** @brief CLI 正常退出并给出回答；content 已经过 LocalAgentCli::extractReply() 整理。 */
    void replyFinished(const QString &content);

    /** @brief 启动失败、非零退出、超时或输出为空；message 为可读原因，附带 stderr 末尾。 */
    void replyFailed(const QString &message);

private:
    /** @brief 已知搜索目录后真正启动进程。 */
    void launch(const QStringList &searchDirectories);

    /** @brief 消费一批标准输出：流式格式交给解析器并发出增量信号，纯文本格式原样累积。 */
    void consumeStandardOutput(const QByteArray &bytes);

    /** @brief 进程结束后的统一收尾：读取输出、校验退出码并发出结果信号。 */
    void handleFinished(int exitCode, bool crashed);

    /** @brief 发出失败信号并清理本次请求的全部状态。 */
    void fail(const QString &message);

    /** @brief 释放进程、计时器与临时目录，不发出任何信号。 */
    void cleanup();

    AiSettings m_settings;
    QList<AiChatMessage> m_messages;
    QPointer<QProcess> m_process;
    QTimer *m_timeoutTimer = nullptr;
    QFutureWatcher<QStringList> *m_resolveWatcher = nullptr;
    std::unique_ptr<QTemporaryDir> m_outputDir;
    QString m_outputFilePath;
    /** @brief 本次调用的标准输出格式；流式格式下 m_stdout 不累积，改由 m_streamParser 解析。 */
    LocalAgentCli::OutputFormat m_outputFormat = LocalAgentCli::OutputFormat::PlainText;
    LocalAgentCli::ClaudeStreamParser m_streamParser;
    QByteArray m_stdout;
    QByteArray m_stderr;
    QString m_displayName;
    int m_timeoutMs = 5 * 60 * 1000;
    /** @brief 每次请求递增；工作线程返回时用它识别已被中止或被新请求取代的旧结果。 */
    quint64 m_generation = 0;
    bool m_busy = false;
};
