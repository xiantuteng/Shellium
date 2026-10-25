#pragma once

#include "core/ConnectionTarget.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

class QObject;

/**
 * @brief 绿盟网页运维助手拉起 SecureCRT 时使用的命令行。
 *
 * macOS 版助手不走 URL Scheme，而是直接执行
 * `/Applications/SecureCRT.app/Contents/MacOS/SecureCRT`，参数形如
 * `/N 标题 /SSH2 /P 端口 /AUTH keyboard-interactive /PASSWORD 占位符 账号#票据@主机`。
 * 票据是堡垒机发给当前已登录用户的一次性会话凭证，只应留在本次连接的内存里。
 */
struct SecureCrtLaunchRequest {
    /** @brief 解析出的连接。username 不含票据，loginUsername 才是实际登录名。 */
    ConnectionTarget target;

    enum class Error {
        None,
        UnsupportedProtocol,
        MissingDestination,
        InvalidPort,
        IncompleteOption
    };

    Error error = Error::None;
    /** @brief 错误里可以展示的非敏感片段，例如协议名或端口原文。 */
    QString errorDetail;
};

/**
 * @brief 把 SecureCRT 风格的 argv 解析成一次连接。
 * @param arguments 含 argv[0] 的完整参数。普通启动参数返回空。
 * @return 识别为 SecureCRT 拉起时返回请求；否则返回空，调用方按正常启动继续。
 */
[[nodiscard]] std::optional<SecureCrtLaunchRequest> parseSecureCrtLaunch(const QStringList &arguments);

/**
 * @brief 在界面语言装载完成后，把解析错误转成给用户看的句子。
 * @param request 必须是已识别的请求。没有错误时返回空串。
 */
[[nodiscard]] QString secureCrtLaunchErrorText(const SecureCrtLaunchRequest &request);

/**
 * @brief 同一用户已在运行的 Shellium 用来接收后续点击的本地套接字名。
 */
[[nodiscard]] QString secureCrtLaunchServerName();

/**
 * @brief 把一次已解析的连接交给正在运行的实例。
 * @param serverName secureCrtLaunchServerName() 或测试里的临时名字。
 * @param request 不含错误的连接请求。密码只通过本机、仅当前用户可访问的套接字传递。
 * @param timeoutMs 等待对端确认的最长时间。
 * @param errorMessage 失败时接收不含密码和票据的原因；可为空。
 * @return 对端已确认接收时返回 true。无人在听时返回 false，调用方应自己成为主实例。
 */
[[nodiscard]] bool deliverSecureCrtLaunch(const QString &serverName,
                                          const SecureCrtLaunchRequest &request,
                                          int timeoutMs,
                                          QString *errorMessage);

/**
 * @brief 监听后续的 SecureCRT 拉起。必须在事件循环运行期间保持存活。
 */
class SecureCrtLaunchServer;

[[nodiscard]] SecureCrtLaunchServer *startSecureCrtLaunchServer(
    QObject *parent,
    const QString &serverName,
    const std::function<void(const SecureCrtLaunchRequest &)> &handler,
    QString *errorMessage);

/**
 * @brief 安装或更新堡垒机助手会执行的 SecureCRT 兼容入口。
 *
 * 助手把路径写死为 `/Applications/SecureCRT.app/Contents/MacOS/SecureCRT`。这里放的是
 * 一个转发给 Shellium 的启动器，bundle id 使用 Shellium 自己的标识。已经存在的 VanDyke
 * SecureCRT 不会被覆盖。
 */
struct SecureCrtShimResult {
    enum class Status {
        Installed,
        Updated,
        Unchanged,
        LeftExistingClient,
        LeftUnrelatedApp,
        Failed
    };

    Status status = Status::Failed;
    /** @brief 失败时的文件系统原因，或被保留的应用标识。 */
    QString detail;
};

[[nodiscard]] SecureCrtShimResult ensureSecureCrtShim(const QString &shelliumExecutable,
                                                      const QString &bundlePath);

/** @brief macOS 上助手写死的 SecureCRT 应用包路径。其他平台返回空串。 */
[[nodiscard]] QString defaultSecureCrtShimBundlePath();
