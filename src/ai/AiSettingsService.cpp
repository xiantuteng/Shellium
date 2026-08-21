#include "ai/AiSettingsService.h"

#include "core/HostRecord.h"
#include "security/Vault.h"
#include "storage/ApplicationDatabase.h"

namespace {
const QString kEndpointKey = QStringLiteral("ai.endpointUrl");
const QString kModelKey = QStringLiteral("ai.model");
const QString kThinkingKey = QStringLiteral("ai.thinkingEnabled");
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

bool AiSettingsService::save(const AiSettings &settings, QString *errorMessage)
{
    if (m_database != nullptr) {
        QString dbError;
        if (!m_database->setSetting(kEndpointKey, settings.endpointUrl, &dbError)
            || !m_database->setSetting(kModelKey, settings.model, &dbError)
            || !m_database->setSetting(kThinkingKey,
                                       settings.thinkingEnabled ? QStringLiteral("1")
                                                                : QStringLiteral("0"),
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
