#pragma once

#include <QMetaType>
#include <QString>

/**
 * @brief AI 助手连接大模型服务所需的可编辑配置快照。
 *
 * 只是值对象，不持有数据库或保险库；apiKey 在内存中以明文保存供直接发起 HTTP 请求，
 * 落盘和读取分别经 AiSettingsService 走 ApplicationDatabase（端点/模型）与
 * CredentialVault（密钥），两者持久化机制不同因此不在这里耦合。
 */
struct AiSettings {
    /** @brief OpenAI 兼容的 Chat Completions 接口地址，例如 https://your-gateway/v1 。 */
    QString endpointUrl;

    /** @brief 模型名称，随请求体一起提交给网关。 */
    QString model;

    /** @brief 鉴权密钥，以 Authorization: Bearer 形式发送；可为空表示网关不要求鉴权。 */
    QString apiKey;

    /**
     * @brief 是否要求模型开启"思考"（推理）模式。
     *
     * 关闭时请求体里完全不出现相关字段，与本开关引入之前的请求逐字节一致——OpenAI 这类
     * 严格网关会对无法识别的请求字段直接返回 400，默认不发才能保证老配置不受影响。开启
     * 时按 OpenAI 兼容生态里最通用的写法附加 enable_thinking: true（DashScope/Qwen、vLLM
     * 及多数自建网关采用），不认识该字段的网关要么忽略、要么明确报错，由用户自行取舍。
     */
    bool thinkingEnabled = false;

    /** @brief 端点和模型均已填写，具备发起请求的最基本条件。 */
    [[nodiscard]] bool isConfigured() const
    {
        return !endpointUrl.trimmed().isEmpty() && !model.trimmed().isEmpty();
    }
};

Q_DECLARE_METATYPE(AiSettings)
