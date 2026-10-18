#pragma once

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

/** @brief 官网发布清单中与“是否需要升级”有关的字段。 */
struct UpdateRelease {
    /** @brief 已去掉前导 v 的版本号，例如 "0.3.6"。 */
    QString version;
    /** @brief 发布时间；清单缺失或格式不对时为无效时间，不影响版本判断。 */
    QDateTime publishedAt;
};

/**
 * @brief 从官网 /api/releases 读取最新版本并与当前版本比较。
 *
 * 官网下载页本身就靠这个接口决定显示哪个版本的安装包，客户端复用同一份清单，发布新版
 * 只需往 R2 上传 latest.json，不需要为检查更新再维护一份数据。
 *
 * 结果分三种信号返回，manual 原样带回，便于调用方区分“用户点了检查”（无论结果都要
 * 反馈）与后台定时检查（只有发现新版本才打扰用户）。被用户跳过的版本只对自动检查生效：
 * 用户主动点“检查更新”说明此刻就想看，跳过记录不应再把它藏起来。
 */
class UpdateChecker final : public QObject {
    Q_OBJECT

public:
    /** @brief 官网发布清单地址。 */
    static constexpr auto kDefaultEndpoint = "https://shellium.net/api/releases";
    /** @brief 发现新版本后引导用户前往的下载页。 */
    static constexpr auto kDownloadPage = "https://shellium.net/download";

    explicit UpdateChecker(QObject *parent = nullptr);

    /** @brief 替换清单地址，供测试指向本地 file:// 清单。 */
    void setEndpoint(const QUrl &endpoint);
    [[nodiscard]] QUrl endpoint() const { return m_endpoint; }

    /**
     * @brief 覆盖“当前版本”；为空时使用 QCoreApplication::applicationVersion()。
     *
     * 测试与裸 QApplication 下 applicationVersion 为空，此时 check() 直接以失败结束，
     * 不发起任何网络请求，避免主窗口测试在后台访问外网。
     */
    void setCurrentVersion(const QString &version);
    [[nodiscard]] QString currentVersion() const;

    /** @brief 是否有尚未返回的请求。 */
    [[nodiscard]] bool isChecking() const { return !m_reply.isNull(); }

    /**
     * @brief 发起一次检查。
     * @param skippedVersion 用户选择跳过的版本；仅在 manual 为 false 时参与判断。
     * @param manual 是否由用户主动触发。
     *
     * 已有请求进行中时，再次自动检查会被忽略；若这次是用户主动检查，则把尚未返回的
     * 请求改按手动结果回报，避免这次点击没有任何反馈，也避免跳过记录把结果藏起来。
     */
    void check(const QString &skippedVersion, bool manual);

    /**
     * @brief 解析发布清单 JSON。
     * @return 版本号缺失或无法解析为版本时返回空，并在 errorMessage 中给出原因。
     */
    [[nodiscard]] static std::optional<UpdateRelease> parseManifest(const QByteArray &payload,
                                                                    QString *errorMessage);

    /** @brief candidate 是否严格高于 current；任一方无法解析时返回 false。 */
    [[nodiscard]] static bool isNewer(const QString &candidate, const QString &current);

    /**
     * @brief 是否应当向用户提示 latest。
     *
     * 只有高于当前版本才提示；自动检查时若 latest 不高于被跳过的版本也不提示，这样跳过
     * 0.4.0 后 0.4.1 发布仍会照常提醒，而不是永久静默。
     */
    [[nodiscard]] static bool shouldNotify(const QString &latest,
                                           const QString &current,
                                           const QString &skippedVersion,
                                           bool manual);

signals:
    /** @brief 发现需要提示的新版本。 */
    void updateAvailable(const UpdateRelease &release, bool manual);
    /** @brief 没有需要提示的版本（已是最新，或自动检查时该版本已被跳过）。 */
    void noUpdateAvailable(const QString &latestVersion, bool manual);
    /** @brief 网络、HTTP 或清单格式错误。 */
    void checkFailed(const QString &errorMessage, bool manual);

private:
    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    QUrl m_endpoint;
    QString m_currentVersionOverride;
    /** @brief 当前请求是否应按手动检查回报；进行中又点了手动检查时会被抬成 true。 */
    bool m_reportAsManual = false;
};

Q_DECLARE_METATYPE(UpdateRelease)
