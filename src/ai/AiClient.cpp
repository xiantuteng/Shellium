#include "ai/AiClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

AiClient::AiClient(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

QString AiClient::normalizedBase(const QString &endpointUrl)
{
    QString trimmed = endpointUrl.trimmed();
    while (trimmed.endsWith(QLatin1Char('/'))) {
        trimmed.chop(1);
    }
    return trimmed;
}

QString AiClient::roleName(const AiChatRole role)
{
    switch (role) {
    case AiChatRole::System:
        return QStringLiteral("system");
    case AiChatRole::User:
        return QStringLiteral("user");
    case AiChatRole::Assistant:
        return QStringLiteral("assistant");
    }
    return QStringLiteral("user");
}

void AiClient::testConnection(const AiSettings &settings)
{
    if (m_activeTestReply != nullptr) {
        m_activeTestReply->abort();
        m_activeTestReply = nullptr;
    }
    if (settings.endpointUrl.trimmed().isEmpty()) {
        emit testConnectionFinished(false, tr("请先填写大模型服务地址"));
        return;
    }

    QNetworkRequest request(QUrl(normalizedBase(settings.endpointUrl) + QStringLiteral("/models")));
    if (!settings.apiKey.trimmed().isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + settings.apiKey.trimmed().toUtf8());
    }

    m_activeTestReply = m_network->get(request);
    connect(m_activeTestReply, &QNetworkReply::finished, this, [this]() {
        QNetworkReply *reply = m_activeTestReply;
        m_activeTestReply = nullptr;
        if (reply == nullptr) {
            return;
        }
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit testConnectionFinished(
                false,
                tr("连接失败：%1").arg(reply->errorString()));
            return;
        }
        const QByteArray body = reply->readAll();
        const QJsonDocument document = QJsonDocument::fromJson(body);
        int modelCount = -1;
        if (document.isObject()) {
            const QJsonValue data = document.object().value(QStringLiteral("data"));
            if (data.isArray()) {
                modelCount = data.toArray().size();
            }
        }
        emit testConnectionFinished(
            true,
            modelCount >= 0 ? tr("连接成功，网关返回 %1 个可用模型").arg(modelCount)
                            : tr("连接成功"));
    });
}

void AiClient::sendMessage(const AiSettings &settings, const QList<AiChatMessage> &messages)
{
    if (m_activeChatReply != nullptr) {
        emit replyFailed(tr("上一条请求尚未结束"));
        return;
    }
    if (!settings.isConfigured()) {
        emit replyFailed(tr("请先在设置中完成大模型地址和模型名称配置"));
        return;
    }

    QJsonArray messageArray;
    for (const AiChatMessage &message : messages) {
        QJsonObject entry;
        entry.insert(QStringLiteral("role"), roleName(message.role));
        entry.insert(QStringLiteral("content"), message.content);
        messageArray.append(entry);
    }
    QJsonObject body;
    body.insert(QStringLiteral("model"), settings.model);
    body.insert(QStringLiteral("messages"), messageArray);
    body.insert(QStringLiteral("temperature"), 0.2);
    body.insert(QStringLiteral("stream"), true);

    QNetworkRequest request(
        QUrl(normalizedBase(settings.endpointUrl) + QStringLiteral("/chat/completions")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!settings.apiKey.trimmed().isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + settings.apiKey.trimmed().toUtf8());
    }

    m_streamBuffer.clear();
    m_streamAccumulatedContent.clear();

    m_activeChatReply = m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_activeChatReply, &QNetworkReply::readyRead, this, [this]() {
        if (m_activeChatReply == nullptr) {
            return;
        }
        m_streamBuffer.append(m_activeChatReply->readAll());
        processStreamBuffer();
    });
    connect(m_activeChatReply, &QNetworkReply::finished, this, [this]() {
        QNetworkReply *reply = m_activeChatReply;
        m_activeChatReply = nullptr;
        if (reply == nullptr) {
            return;
        }
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit replyFailed(tr("请求失败：%1").arg(reply->errorString()));
            return;
        }
        // 收尾前再扫一遍缓冲区：readyRead 可能还没来得及处理最后一批到达的字节。
        processStreamBuffer();
        if (!m_streamAccumulatedContent.isEmpty()) {
            emit replyFinished(m_streamAccumulatedContent);
            return;
        }

        // 走到这里说明缓冲区里从头到尾没有出现过一个完整的 SSE 事件（没有"data: "开头、
        // 以空行结束的分段）——网关没有真的流式返回，stream 参数被当成未知字段忽略了，
        // 缓冲区里躺着的其实是一次性吐回来的完整 JSON。退回旧的非流式解析方式。
        const QJsonDocument document = QJsonDocument::fromJson(m_streamBuffer);
        if (!document.isObject()) {
            emit replyFailed(tr("网关返回了无法解析的响应"));
            return;
        }
        const QJsonArray choices = document.object().value(QStringLiteral("choices")).toArray();
        if (choices.isEmpty()) {
            const QString apiError =
                document.object().value(QStringLiteral("error")).toObject()
                    .value(QStringLiteral("message")).toString();
            emit replyFailed(apiError.isEmpty() ? tr("网关未返回任何回复") : apiError);
            return;
        }
        const QString content = choices.first().toObject()
                                    .value(QStringLiteral("message")).toObject()
                                    .value(QStringLiteral("content")).toString();
        emit replyFinished(content);
    });
}

void AiClient::processStreamBuffer()
{
    while (true) {
        const int boundary = m_streamBuffer.indexOf("\n\n");
        if (boundary < 0) {
            return;
        }
        const QByteArray eventBlock = m_streamBuffer.left(boundary);
        m_streamBuffer.remove(0, boundary + 2);

        for (const QByteArray &rawLine : eventBlock.split('\n')) {
            const QByteArray line = rawLine.trimmed();
            if (!line.startsWith("data:")) {
                continue;
            }
            const QByteArray payload = line.mid(5).trimmed();
            if (payload.isEmpty() || payload == "[DONE]") {
                continue;
            }
            const QJsonDocument document = QJsonDocument::fromJson(payload);
            if (!document.isObject()) {
                continue;
            }
            const QJsonArray choices = document.object().value(QStringLiteral("choices")).toArray();
            if (choices.isEmpty()) {
                continue;
            }
            const QString delta = choices.first().toObject()
                                       .value(QStringLiteral("delta")).toObject()
                                       .value(QStringLiteral("content")).toString();
            if (delta.isEmpty()) {
                continue;
            }
            m_streamAccumulatedContent += delta;
            emit replyChunkReceived(m_streamAccumulatedContent);
        }
    }
}

void AiClient::abortActiveRequest()
{
    if (m_activeChatReply != nullptr) {
        m_activeChatReply->abort();
    }
}
