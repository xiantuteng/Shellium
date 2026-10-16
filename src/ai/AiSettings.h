#pragma once

#include <QMetaType>
#include <QString>

/**
 * @brief AI 助手的"大脑"从哪里来。
 *
 * Http 走 OpenAI 兼容网关；LocalCli 调用本机已安装并登录的智能体 CLI（claude、codex 等），
 * 复用用户已有的订阅与鉴权，无需另配 API Key。两种来源共用同一套单步协议和执行审批，
 * CLI 只负责"想"，命令永远由 Shellium 自己下发到终端。
 */
enum class AiBackend {
    Http,
    LocalCli
};

/**
 * @brief 智能体给出命令后，哪些风险等级可以不经点击直接下发。
 *
 * 风险等级由 CommandRiskClassifier 的本地静态规则判定，不采信模型自述。
 */
enum class AiExecutionPolicy {
    /** @brief 每条命令都要用户点击"执行"，与引入自动执行之前的行为一致。 */
    Manual,
    /** @brief 只读且不需要交互的命令自动执行；修改系统或高风险命令仍需人工确认。 */
    AutoReadOnly,
    /** @brief 全自动：所有命令都直接下发，由用户在面板里显式开启并确认后才生效。 */
    FullAuto
};

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

    /** @brief 模型来源；缺省沿用 HTTP 网关，老配置升级后行为不变。 */
    AiBackend backend = AiBackend::Http;

    /** @brief 选中的本地智能体 CLI 标识，取值见 LocalAgentCli::catalog() 的 id。 */
    QString cliAgentId;

    /**
     * @brief 用户手动指定的 CLI 可执行文件路径；为空时按检测结果自动定位。
     *
     * 自动检测覆盖了常见安装位置，但 nvm/asdf 一类按项目切换的安装方式可能找不到，
     * 留一个手填入口作为兜底。
     */
    QString cliExecutablePath;

    /** @brief 传给 CLI 的 --model 参数；为空表示使用 CLI 自身的默认模型。 */
    QString cliModel;

    /** @brief 命令自动执行策略，缺省只自动执行只读命令。 */
    AiExecutionPolicy executionPolicy = AiExecutionPolicy::AutoReadOnly;

    /** @brief 当前模型来源具备发起请求的最基本条件。 */
    [[nodiscard]] bool isConfigured() const
    {
        if (backend == AiBackend::LocalCli) {
            return !cliAgentId.trimmed().isEmpty();
        }
        return !endpointUrl.trimmed().isEmpty() && !model.trimmed().isEmpty();
    }
};

Q_DECLARE_METATYPE(AiSettings)
