#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>

/**
 * @brief 描述一个可以由终端或 SFTP 独立连接的 SSH 目标。
 *
 * 对于嵌套 SSH，proxyJumps 保存从本机到目标服务器之前需要经过的完整跳板链。
 * password 只存在于当前进程内存中，当前版本不会将其写入磁盘或日志。
 */
struct ConnectionTarget {
    /**
     * @brief 该目标在连接链中的可信程度。
     *
     * Confirmed 表示这一层由本程序亲自建立并完成认证，层的存在与身份是确定事实。
     * Inferred 表示它仅由解析终端命令推测得出，可能与远端真实状态不一致——界面必须把
     * 这种不确定性显式呈现给用户，因为在错误的服务器上执行删除是不可撤销的。
     */
    enum class Confidence { Confirmed, Inferred };

    /**
     * @brief 本次连接优先使用的 SSH 认证方式。
     *
     * Automatic 保持原有顺序：none、公钥/Agent，然后是密码。KeyboardInteractive 用于堡垒机
     * 按 SecureCRT 方式拉起的会话：登录名里的一次性票据必须走 keyboard-interactive，不能先拿
     * 本机 Agent 里的多把密钥去试，否则很容易在票据生效前耗尽服务端的认证次数。
     */
    enum class AuthPreference { Automatic, KeyboardInteractive };

    QString id;
    QString displayName;
    QString host;
    QString username;
    quint16 port = 22;
    QString password;

    /**
     * @brief 实际送给 SSH 服务端的登录名。为空时使用 username。
     *
     * 绿盟堡垒机把一次性票据拼在账号后面，形如 account#ticket。票据只能留在这个进程内字段，
     * username 仍是给人看的账号，这样标题、日志和主机库都不会记下票据。
     */
    QString loginUsername;

    /** @brief 本次连接的认证偏好，只在进程内有效，不写入主机库。 */
    AuthPreference authPreference = AuthPreference::Automatic;
    QString identityFile;
    QStringList proxyJumps;
    QString parentId;

    /**
     * @brief 内层 SSH 是否仍处于终端交互认证阶段。
     *
     * 该字段只在当前进程中流转，不持久化。文件管理器收到认证失败时据此判断是否应等待
     * 终端里的密码重试，避免在用户第一次输错密码后又弹出第二个 SFTP 密码输入框。
     */
    bool interactiveAuthenticationPending = false;

    /**
     * @brief 本层来源的可信度，默认按推测处理。
     *
     * 默认值刻意取 Inferred：新增代码路径若忘记标注来源，应当落到需要用户确认的保守分支，
     * 而不是被当作已验证事实。
     */
    Confidence confidence = Confidence::Inferred;

    /**
     * @brief 进程内 direct-tcpip 路由标识；非空时 SFTP 复用已认证外层会话。
     *
     * 该值不应持久化或跨进程使用，只用于把嵌套目标关联到当前 TerminalPane 的隧道提供者。
     */
    QString tunnelRouteId;

    /**
     * @brief 当前目标是否只是受管主机链中的中转层。
     *
     * 该状态只在进程内使用，不持久化。中转层仍由 SshSessionStack 完成认证并提供转发，
     * 但不应出现在文件管理器的普通目标列表中，也不应被连接历史误记为用户最终进入的主机。
     */
    bool transit = false;

    /**
     * @brief 是否为该目标显式启用旧版 hmac-sha1 双向 MAC。
     *
     * 默认关闭。只有服务器明确仅支持 hmac-sha1 且用户知情确认时才打开，避免为了兼容
     * 一台旧服务器而降低其他连接的算法安全基线。
     */
    bool allowLegacyHmacSha1 = false;

    /**
     * @brief 判断目标是否具备建立连接所需的最小信息。
     * @return host 非空时返回 true，否则返回 false。
     */
    [[nodiscard]] bool isValid() const;

    /**
     * @brief 生成 OpenSSH/libssh 可识别的 user@host:port 描述。
     * @param includeDefaultPort 是否在端口为 22 时仍显式输出端口。
     * @return 用于 ProxyJump 或界面展示的连接描述。
     */
    [[nodiscard]] QString connectionSpecifier(bool includeDefaultPort = false) const;

    /**
     * @brief 返回稳定的主机键，用于“本次运行信任该主机”等内存状态。
     * @return 由用户名、主机名和端口组成的键，不包含密码等敏感信息。
     */
    [[nodiscard]] QString stableKey() const;
};

Q_DECLARE_METATYPE(ConnectionTarget)
Q_DECLARE_METATYPE(QList<ConnectionTarget>)
