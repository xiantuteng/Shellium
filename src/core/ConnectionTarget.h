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
    QString id;
    QString displayName;
    QString host;
    QString username;
    quint16 port = 22;
    QString password;
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
     * @brief 进程内 direct-tcpip 路由标识；非空时 SFTP 复用已认证外层会话。
     *
     * 该值不应持久化或跨进程使用，只用于把嵌套目标关联到当前 TerminalPane 的隧道提供者。
     */
    QString tunnelRouteId;

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
