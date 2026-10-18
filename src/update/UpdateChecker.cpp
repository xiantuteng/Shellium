#include "update/UpdateChecker.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSysInfo>
#include <QVersionNumber>

namespace {

/** @brief 单次检查的传输超时；官网清单只有几 KB，超过这个时间基本可以判定网络不通。 */
constexpr int kTransferTimeoutMs = 15000;

/** @brief 清单正文上限，防止被劫持的响应把任意大的内容读进内存。 */
constexpr qint64 kMaxManifestBytes = 256 * 1024;

/** @brief 解析版本号，兼容 GitHub tag 风格的前导 v 与 "-beta" 一类后缀。 */
QVersionNumber parseVersion(const QString &text)
{
    QString trimmed = text.trimmed();
    if (trimmed.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        trimmed.remove(0, 1);
    }
    qsizetype suffixIndex = -1;
    const QVersionNumber version = QVersionNumber::fromString(trimmed, &suffixIndex);
    if (version.isNull() || suffixIndex <= 0) {
        return {};
    }
    // 去掉末尾的 0 段：QVersionNumber 认为 0.4.0 比 0.4 新，同一个版本不能因写法不同被提示。
    return version.normalized();
}

} // namespace

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_endpoint(QString::fromLatin1(kDefaultEndpoint))
{
    qRegisterMetaType<UpdateRelease>();
}

void UpdateChecker::setEndpoint(const QUrl &endpoint)
{
    m_endpoint = endpoint;
}

void UpdateChecker::setCurrentVersion(const QString &version)
{
    m_currentVersionOverride = version;
}

QString UpdateChecker::currentVersion() const
{
    return m_currentVersionOverride.isEmpty() ? QCoreApplication::applicationVersion()
                                              : m_currentVersionOverride;
}

void UpdateChecker::check(const QString &skippedVersion, const bool manual)
{
    if (isChecking()) {
        if (manual) {
            m_reportAsManual = true;
        }
        return;
    }
    m_reportAsManual = manual;
    const QString current = currentVersion();
    if (parseVersion(current).isNull()) {
        emit checkFailed(tr("The version of this build is unknown."), manual);
        return;
    }

    QNetworkRequest request(m_endpoint);
    request.setTransferTimeout(kTransferTimeoutMs);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Shellium/%1 (%2; %3)")
                          .arg(current, QSysInfo::productType(), QSysInfo::currentCpuArchitecture()));
    // 清单随发布即时变化，绕过本地缓存，避免发布后很久还读到旧版本号。
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);

    QNetworkReply *reply = m_network->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, current, skippedVersion]() {
        reply->deleteLater();
        m_reply.clear();
        // 取走标志再发信号：槽里如果立刻再发起检查，不能沿用这一次的手动/自动属性。
        const bool manual = m_reportAsManual;
        m_reportAsManual = false;

        if (reply->error() != QNetworkReply::NoError) {
            emit checkFailed(reply->errorString(), manual);
            return;
        }
        const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        // file:// 等非 HTTP 地址没有状态码，只对 HTTP 响应做检查。
        if (status.isValid() && status.toInt() != 200) {
            emit checkFailed(tr("The update server returned HTTP %1.").arg(status.toInt()), manual);
            return;
        }
        const QByteArray payload = reply->read(kMaxManifestBytes + 1);
        if (payload.size() > kMaxManifestBytes) {
            emit checkFailed(tr("The update information is too large."), manual);
            return;
        }

        QString parseError;
        const std::optional<UpdateRelease> release = parseManifest(payload, &parseError);
        if (!release.has_value()) {
            emit checkFailed(parseError, manual);
            return;
        }
        if (shouldNotify(release->version, current, skippedVersion, manual)) {
            emit updateAvailable(*release, manual);
        } else {
            emit noUpdateAvailable(release->version, manual);
        }
    });
}

std::optional<UpdateRelease> UpdateChecker::parseManifest(const QByteArray &payload,
                                                          QString *errorMessage)
{
    const auto fail = [errorMessage](const QString &message) -> std::optional<UpdateRelease> {
        if (errorMessage != nullptr) {
            *errorMessage = message;
        }
        return std::nullopt;
    };

    QJsonParseError jsonError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &jsonError);
    if (jsonError.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(tr("The update information could not be read."));
    }
    const QJsonObject root = document.object();
    // 官网在 R2 里还没有清单时返回 version: null，这不是错误，但也没有可比较的版本。
    const QString version = root.value(QStringLiteral("version")).toString().trimmed();
    if (version.isEmpty()) {
        return fail(tr("No release has been published yet."));
    }
    if (parseVersion(version).isNull()) {
        return fail(tr("The update information could not be read."));
    }

    UpdateRelease release;
    release.version = version.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)
        ? version.mid(1)
        : version;
    release.publishedAt = QDateTime::fromString(
        root.value(QStringLiteral("publishedAt")).toString(), Qt::ISODate);
    return release;
}

bool UpdateChecker::isNewer(const QString &candidate, const QString &current)
{
    const QVersionNumber candidateVersion = parseVersion(candidate);
    const QVersionNumber currentVersion = parseVersion(current);
    if (candidateVersion.isNull() || currentVersion.isNull()) {
        return false;
    }
    return QVersionNumber::compare(candidateVersion, currentVersion) > 0;
}

bool UpdateChecker::shouldNotify(const QString &latest,
                                 const QString &current,
                                 const QString &skippedVersion,
                                 const bool manual)
{
    if (!isNewer(latest, current)) {
        return false;
    }
    if (manual || parseVersion(skippedVersion).isNull()) {
        return true;
    }
    return isNewer(latest, skippedVersion);
}
