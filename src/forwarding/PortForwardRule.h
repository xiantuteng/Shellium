#pragma once

#include "forwarding/ProxyNetwork.h"

#include <QMetaType>
#include <QList>
#include <QString>
#include <QStringList>

/**
 * @brief SSH 端口转发类型，与 OpenSSH 的 -L、-R、-D 一一对应。
 *
 * 枚举值会持久化到 SQLite，禁止随意调整既有数值；未来新增类型只能在末尾追加，并同步
 * 数据库校验与 UI 映射。
 */
enum class PortForwardType {
    Local = 0,   ///< 本地监听，连接经当前 SSH 会话转发到远端视角的目标（-L）。
    Remote = 1,  ///< SSH 服务器监听，连接经当前会话回到本机目标（-R）。
    Dynamic = 2  ///< 本地 SOCKS5 监听，每个 CONNECT 请求自行给出远端目标（-D）。
};

/** @brief 一条按持久化 hostId 绑定的端口转发规则。 */
struct PortForwardRule {
    QString id;                 ///< 稳定主键；新建规则通常使用 UUID。
    QString hostId;             ///< hosts 表中的持久化主机 id，禁止绑定临时连接描述。
    QString name;               ///< 用户可读名称。
    PortForwardType type = PortForwardType::Local;
    QString bindHost = QStringLiteral("127.0.0.1"); ///< 监听地址；远程转发由 SSH 服务端解释。
    quint16 bindPort = 0;       ///< 监听端口；0 无效，避免 UI 显示与实际随机端口不一致。
    QString targetHost;         ///< -L 为远端目标，-R 为本机目标；-D 留空。
    quint16 targetPort = 0;     ///< -L/-R 必填；-D 留为 0。
    bool autoStart = false;     ///< 根连接成功后是否自动启动。
    /**
     * 仅动态 SOCKS5 使用。空列表表示每个 CONNECT 都经 SSH 转发。非空时只转发落在这些
     * 网段内的目标，其余目标由本机按系统默认路由直连。元素为规范 CIDR，例如 10.0.0.0/8。
     */
    QStringList proxyNetworks;

    /**
     * @brief 校验规则是否具备可执行所需的全部字段。
     * @param errorMessage 失败时接收具体字段原因；可传 nullptr。
     * @return 字段完整且端口、类型合法返回 true。
     */
    [[nodiscard]] bool isValid(QString *errorMessage = nullptr) const
    {
        const auto fail = [errorMessage](const QString &message) {
            if (errorMessage != nullptr) {
                *errorMessage = message;
            }
            return false;
        };
        if (id.trimmed().isEmpty()) {
            return fail(QStringLiteral("转发规则 id 不能为空"));
        }
        if (hostId.trimmed().isEmpty()) {
            return fail(QStringLiteral("转发规则必须绑定一台已保存主机"));
        }
        if (name.trimmed().isEmpty()) {
            return fail(QStringLiteral("转发规则名称不能为空"));
        }
        if (bindHost.trimmed().isEmpty() || bindPort == 0) {
            return fail(QStringLiteral("监听地址和端口必须有效"));
        }
        if (type != PortForwardType::Local && type != PortForwardType::Remote
            && type != PortForwardType::Dynamic) {
            return fail(QStringLiteral("转发类型无效"));
        }
        if (type != PortForwardType::Dynamic
            && (targetHost.trimmed().isEmpty() || targetPort == 0)) {
            return fail(QStringLiteral("本地和远程转发必须填写目标地址与端口"));
        }
        if (type == PortForwardType::Dynamic && !proxyNetworks.isEmpty()) {
            const ProxyNetwork::ParseResult parsed = ProxyNetwork::normalize(proxyNetworks);
            if (parsed.tooMany) {
                return fail(QStringLiteral("代理网段最多 %1 条")
                                .arg(ProxyNetwork::kMaxProxyNetworks));
            }
            if (!parsed.invalidEntry.isEmpty()) {
                return fail(QStringLiteral("代理网段无效：%1。请填写 IP 或 CIDR，例如 10.0.0.0/8")
                                .arg(parsed.invalidEntry));
            }
        }
        return true;
    }
};

/** @brief 一条运行中规则的生命周期状态。 */
enum class PortForwardState {
    Stopped = 0, ///< 未启动或已正常停止。
    Starting,    ///< 正在创建监听器或向 SSH 服务器申请远程监听。
    Running,     ///< 已可接收新连接。
    Stopping,    ///< 正在停止监听并等待在途连接退出。
    Failed       ///< 启动或运行失败；详情见 PortForwardStatistics::lastError。
};

/** @brief 可直接展示的端口转发统计快照。 */
struct PortForwardStatistics {
    PortForwardState state = PortForwardState::Stopped;
    quint64 activeConnections = 0;
    quint64 totalConnections = 0;
    quint64 bytesToTarget = 0;
    quint64 bytesFromTarget = 0;
    QString lastError;
};

Q_DECLARE_METATYPE(PortForwardRule)
Q_DECLARE_METATYPE(QList<PortForwardRule>)
Q_DECLARE_METATYPE(PortForwardState)
Q_DECLARE_METATYPE(PortForwardStatistics)
