#include "ai/AiSettingsService.h"

#include "ai/LocalAgentCli.h"
#include "core/HostRecord.h"
#include "security/Vault.h"
#include "storage/ApplicationDatabase.h"

namespace {
const QString kEndpointKey = QStringLiteral("ai.endpointUrl");
const QString kModelKey = QStringLiteral("ai.model");
const QString kThinkingKey = QStringLiteral("ai.thinkingEnabled");
const QString kBackendKey = QStringLiteral("ai.backend");
const QString kCliAgentKey = QStringLiteral("ai.cliAgent");
const QString kCliPathKey = QStringLiteral("ai.cliPath");
const QString kCliModelKey = QStringLiteral("ai.cliModel");
const QString kExecutionPolicyKey = QStringLiteral("ai.executionPolicy");

/** @brief 持久化用的稳定字符串，不直接存枚举整数，避免以后调整枚举顺序时串值。 */
QString backendToString(const AiBackend backend)
{
    return backend == AiBackend::LocalCli ? QStringLiteral("cli") : QStringLiteral("http");
}

AiBackend backendFromString(const QString &value)
{
    return value == QStringLiteral("cli") ? AiBackend::LocalCli : AiBackend::Http;
}

QString policyToString(const AiExecutionPolicy policy)
{
    switch (policy) {
    case AiExecutionPolicy::Manual:
        return QStringLiteral("manual");
    case AiExecutionPolicy::FullAuto:
        return QStringLiteral("full");
    case AiExecutionPolicy::AutoReadOnly:
        break;
    }
    return QStringLiteral("readonly");
}

AiExecutionPolicy policyFromString(const QString &value)
{
    if (value == QStringLiteral("manual")) {
        return AiExecutionPolicy::Manual;
    }
    if (value == QStringLiteral("full")) {
        return AiExecutionPolicy::FullAuto;
    }
    return AiExecutionPolicy::AutoReadOnly;
}
} // namespace

AiSettingsService::AiSettingsService(ApplicationDatabase *database,
                                     CredentialVault *credentialVault,
                                     QObject *parent)
    : QObject(parent)
    , m_database(database)
    , m_credentialVault(credentialVault)
{
}

QString AiSettingsService::apiKeyCredentialId()
{
    // 全应用只有一份 AI 配置，固定 id 即可；不像主机凭据那样需要按记录区分。
    return QStringLiteral("ai-assistant-api-key");
}

bool AiSettingsService::load(QString *errorMessage)
{
    AiSettings settings;
    if (m_database != nullptr) {
        settings.endpointUrl = m_database->setting(kEndpointKey).value_or(QString());
        settings.model = m_database->setting(kModelKey).value_or(QString());
        // 缺省视为关闭：没有这条记录的老库应当继续按"不发思考字段"的方式请求网关。
        settings.thinkingEnabled =
            m_database->setting(kThinkingKey).value_or(QStringLiteral("0")) == QStringLiteral("1");
        settings.backend = backendFromString(m_database->setting(kBackendKey).value_or(QString()));
        settings.cliAgentId = m_database->setting(kCliAgentKey).value_or(QString());
        settings.cliExecutablePath = m_database->setting(kCliPathKey).value_or(QString());
        // 老版本会原样保存误填的 "--model xxx"，读取时顺手整理，用户无需重新填写。
        settings.cliModel =
            LocalAgentCli::normalizeModelName(m_database->setting(kCliModelKey).value_or(QString()));
        // 全自动绝不能因为"读不到配置"而被默认打开：未知值一律回落到只读自动执行。
        settings.executionPolicy =
            policyFromString(m_database->setting(kExecutionPolicyKey).value_or(QString()));
    }
    if (m_credentialVault != nullptr) {
        QString secretError;
        const std::optional<QByteArray> secret =
            m_credentialVault->loadSecret(apiKeyCredentialId(), &secretError);
        if (secret.has_value()) {
            settings.apiKey = QString::fromUtf8(*secret);
        } else if (!secretError.isEmpty() && secretError != QStringLiteral("指定凭据不存在")) {
            // 密钥缺失是首次使用的正常状态；只有解锁失败、密文损坏这类真实错误才上报，
            // 否则每次启动都会对着从未配置过 AI 的用户弹一条误导性的失败提示。
            if (errorMessage != nullptr) {
                *errorMessage = secretError;
            }
            m_current = settings;
            return false;
        }
    }
    m_current = settings;
    return true;
}

bool AiSettingsService::save(const AiSettings &input, QString *errorMessage)
{
    AiSettings settings = input;
    settings.cliModel = LocalAgentCli::normalizeModelName(settings.cliModel);
    if (m_database != nullptr) {
        QString dbError;
        if (!m_database->setSetting(kEndpointKey, settings.endpointUrl, &dbError)
            || !m_database->setSetting(kModelKey, settings.model, &dbError)
            || !m_database->setSetting(kThinkingKey,
                                       settings.thinkingEnabled ? QStringLiteral("1")
                                                                : QStringLiteral("0"),
                                       &dbError)
            || !m_database->setSetting(kBackendKey, backendToString(settings.backend), &dbError)
            || !m_database->setSetting(kCliAgentKey, settings.cliAgentId, &dbError)
            || !m_database->setSetting(kCliPathKey, settings.cliExecutablePath, &dbError)
            || !m_database->setSetting(kCliModelKey, settings.cliModel, &dbError)
            || !m_database->setSetting(kExecutionPolicyKey,
                                       policyToString(settings.executionPolicy),
                                       &dbError)) {
            if (errorMessage != nullptr) {
                *errorMessage = dbError;
            }
            return false;
        }
    }

    if (m_credentialVault != nullptr) {
        const QString trimmedKey = settings.apiKey.trimmed();
        if (trimmedKey.isEmpty()) {
            // saveSecret 拒绝空密文；清空密钥改走直接删除，避免留下一条"看起来已配置"
            // 但其实无法解密出任何内容的孤立记录。
            if (!m_current.apiKey.trimmed().isEmpty() && m_database != nullptr) {
                QString removeError;
                if (!m_database->removeCredential(apiKeyCredentialId(), nullptr, &removeError)
                    && errorMessage != nullptr) {
                    *errorMessage = removeError;
                }
            }
        } else {
            QString secretError;
            if (!m_credentialVault->saveSecret(apiKeyCredentialId(),
                                               QStringLiteral("AI 助手 API Key"),
                                               CredentialType::Token,
                                               trimmedKey.toUtf8(),
                                               &secretError)) {
                if (errorMessage != nullptr) {
                    *errorMessage = secretError;
                }
                return false;
            }
        }
    }

    m_current = settings;
    emit settingsChanged(m_current);
    return true;
}
