#pragma once

#include "ai/AiChatTypes.h"
#include "ai/AiSettings.h"

#include <QList>
#include <QObject>
#include <QString>

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
     * @brief 发起一次对话请求。
     * @param settings 当前生效的端点、密钥、模型配置；调用前应确认 isConfigured()。
     * @param messages 完整对话历史，包含本次待发送的最新一条用户消息。
     *
     * 结果通过 replyFinished 或 replyFailed 异步返回；调用方需要等待其中之一发出后才能
     * 发起下一次请求，避免同一实例上的两次请求互相覆盖响应处理状态。
     */
    void sendMessage(const AiSettings &settings, const QList<AiChatMessage> &messages);

    /** @brief 中止当前进行中的对话请求；用户切换上下文或关闭面板时调用。 */
    void abortActiveRequest();

signals:
    /** @brief 连通性探测完成；ok 为 true 时 message 是模型数量之类的简短摘要。 */
    void testConnectionFinished(bool ok, const QString &message);

    /** @brief 对话请求成功，content 是模型返回的完整文本。 */
    void replyFinished(const QString &content);

    /** @brief 对话请求失败，message 是可读原因（HTTP 状态、网络错误或响应格式异常）。 */
    void replyFailed(const QString &message);

private:
    /** @brief 去掉末尾斜杠，规范化用户填写的端点地址。 */
    [[nodiscard]] static QString normalizedBase(const QString &endpointUrl);

    /** @brief 把对话角色枚举转成 OpenAI 兼容协议要求的字符串。 */
    [[nodiscard]] static QString roleName(AiChatRole role);

    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_activeTestReply = nullptr;
    QNetworkReply *m_activeChatReply = nullptr;
};
