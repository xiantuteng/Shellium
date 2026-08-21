#pragma once

#include "ai/AiChatTypes.h"
#include "ai/AiSettings.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * @brief 面向 OpenAI 兼容网关的最小 HTTP 客户端。
 *
 * “大模型地址”覆盖官方 OpenAI、Azure OpenAI 兼容模式，以及绝大多数自建网关（vLLM、
 * Ollama 的 OpenAI 兼容层等），它们共同点是 POST {base}/chat/completions 提交
 * messages 数组、GET {base}/models 可用于不产生副作用的连通性探测。本类只覆盖这两个
 * 端点，不做流式 SSE——首版先保证请求可用，流式体验留待后续迭代。
 *
 * 每个实例同一时间只处理一个进行中的对话请求；调用方在收到上一次 finished/failed 信号
 * 前不应发起新的 sendMessage，测试连接请求另计，不与对话请求互相阻塞。
 */
class AiClient final : public QObject {
    Q_OBJECT

public:
    explicit AiClient(QObject *parent = nullptr);

    /**
     * @brief 发起一次连通性探测：GET {base}/models。
     * @param settings 待测试的端点、密钥；模型名不参与本次请求。
     *
     * 结果通过 testConnectionFinished 异步返回；重复调用会先中止上一次未完成的测试。
     */
    void testConnection(const AiSettings &settings);

    /**
     * @brief 发起一次对话请求，按 SSE 流式协议提交（请求体 stream: true）。
     * @param settings 当前生效的端点、密钥、模型配置；调用前应确认 isConfigured()。
     * @param messages 完整对话历史，包含本次待发送的最新一条用户消息。
     *
     * 每收到一段增量文本就发出一次 replyChunkReceived（携带累计到目前为止的完整内容），
     * 供界面边到边渲染，不需要等全部生成完才有反应。最终结果仍通过 replyFinished 或
     * replyFailed 异步返回；调用方需要等待其中之一发出后才能发起下一次请求，避免同一
     * 实例上的两次请求互相覆盖响应处理状态。
     *
     * 网关不支持流式、把 stream 参数当作未知字段忽略、直接吐回完整 JSON 的情况也会被
     * 兼容：这种响应体里找不到任何 SSE 事件边界，replyChunkReceived 不会触发，最终仍从
     * 缓冲区里按一次性响应解析出内容，正常发出 replyFinished。
     */
    void sendMessage(const AiSettings &settings, const QList<AiChatMessage> &messages);

    /**
     * @brief 拉取网关可用模型列表：GET {base}/models，取每个条目的 id。
     * @param settings 待查询的端点、密钥；模型名不参与本次请求。
     *
     * 与 testConnection 共用同一个端点但各走各的请求：测试连接只回答"通不通"，这里要把
     * 结果填进设置页的下拉框，两者可能被用户先后点击，互相中止会让先点的那个静默失败。
     * 结果通过 modelsFetched 异步返回；重复调用会中止上一次未完成的拉取。
     */
    void fetchModels(const AiSettings &settings);

    /**
     * @brief 中止当前进行中的对话请求；用户主动停止、切换上下文或关闭面板时调用。
     *
     * 中止后本次请求不再发出 replyFinished / replyFailed：用户按下"停止"是一次成功的
     * 交互，把底层的"Operation canceled"当成请求失败弹给用户只会制造一条假故障。调用方
     * 因此必须在调用处自行收尾界面状态，不能等待信号。没有进行中的请求时调用无副作用。
     */
    void abortActiveRequest();

signals:
    /** @brief 连通性探测完成；ok 为 true 时 message 是模型数量之类的简短摘要。 */
    void testConnectionFinished(bool ok, const QString &message);

    /**
     * @brief 模型列表拉取结束。
     * @param ok 请求成功且响应里确实解析出了模型 id 时为 true。
     * @param models 按网关返回顺序排列的模型 id，ok 为 false 时为空。
     * @param message 可读的结果说明，成功时是数量摘要，失败时是原因。
     */
    void modelsFetched(bool ok, const QStringList &models, const QString &message);

    /**
     * @brief 流式响应每到一段新增量就触发一次。
     * @param accumulatedContent 从本次请求开始到目前为止拼接出的完整文本，不是单独的增量
     *        片段——调用方直接拿去替换上一次显示的内容即可，不需要自己做拼接。
     */
    void replyChunkReceived(const QString &accumulatedContent);

    /** @brief 对话请求成功，content 是模型返回的完整文本。 */
    void replyFinished(const QString &content);

    /** @brief 对话请求失败，message 是可读原因（HTTP 状态、网络错误或响应格式异常）。 */
    void replyFailed(const QString &message);

private:
    /** @brief 去掉末尾斜杠，规范化用户填写的端点地址。 */
    [[nodiscard]] static QString normalizedBase(const QString &endpointUrl);

    /** @brief 把对话角色枚举转成 OpenAI 兼容协议要求的字符串。 */
    [[nodiscard]] static QString roleName(AiChatRole role);

    /**
     * @brief 从 m_streamBuffer 里取出所有已经凑齐的 SSE 事件，逐个解析并发出增量信号。
     *
     * 一次 TCP 分片不一定凑够一个完整事件（以空行分隔），因此每次读到新字节都要重新扫描
     * 缓冲区，把能确定已经收全的事件消费掉，不能确定的部分留在缓冲区里等下一批字节。
     */
    void processStreamBuffer();

    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_activeTestReply = nullptr;
    QNetworkReply *m_activeModelsReply = nullptr;
    QNetworkReply *m_activeChatReply = nullptr;

    /** @brief 当前对话请求是否由 abortActiveRequest() 主动中止，用于抑制随之而来的失败信号。 */
    bool m_chatAbortRequested = false;

    /** @brief 当前请求尚未被 processStreamBuffer 消费掉的原始字节。 */
    QByteArray m_streamBuffer;
    /** @brief 当前请求已经从 SSE 增量里拼出的完整文本；非空即说明网关确实在走流式协议。 */
    QString m_streamAccumulatedContent;
};
