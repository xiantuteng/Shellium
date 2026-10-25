#pragma once

#include <QString>
#include <QStringList>

/**
 * @brief 动态 SOCKS5 的目标网段匹配。
 *
 * 只依赖 Qt Core，方便数据库层在不链接 Qt Network 的测试目标里做同一套校验。
 * 空列表表示全部目标经 SSH 转发；非空时只有落在这些网段内的目标走隧道，其余由调用方
 * 按系统默认路由直连。单条 IP 会规范成 /32 或 /128。
 */
namespace ProxyNetwork {

/** @brief 一条规则允许保存的代理网段上限，避免超长配置拖慢每条连接的匹配。 */
constexpr int kMaxProxyNetworks = 64;

/** @brief 规范化结果。invalidEntry 与 tooMany 互斥；两者都空表示成功。 */
struct ParseResult {
    QStringList networks; ///< 已去重、掩掉主机位后的规范 CIDR，顺序与用户输入一致。
    QString invalidEntry; ///< 无法解析的原始片段；过长时已截断。
    bool tooMany = false;
};

/** @brief SOCKS 目标的出口选择。 */
enum class Egress {
    Tunnel, ///< 经当前 SSH 会话转发。
    Direct, ///< 由本机按系统默认路由直接连接。
    Reject  ///< 网段配置损坏，调用方应拒绝这次 SOCKS 请求。
};

/** @brief 一次 CONNECT 该怎么连，以及真正用于连接的主机名或 IP。 */
struct EgressDecision {
    Egress egress = Egress::Tunnel;
    QString targetHost; ///< Tunnel/Direct 时的连接目标；域名命中网段时改为命中的 IP。
    QString invalidEntry;
    bool tooMany = false;
};

/**
 * @brief 把用户输入拆成网段片段。
 * @param text 逗号、分号、顿号或空白分隔的网段文本。
 * @return 去掉空白后的非空片段；尚未校验是否为合法 IP/CIDR。
 */
[[nodiscard]] QStringList tokenize(const QString &text);

/**
 * @brief 校验并规范化代理网段。
 * @param networks 原始片段；每个元素还可以再包含分隔符。
 * @return 成功时 networks 为规范 CIDR。失败时 networks 为空，并标明无效片段或数量超限。
 */
[[nodiscard]] ParseResult normalize(const QStringList &networks);

/**
 * @brief 判断文本是不是不带端口、不带区域标识的 IP 字面量。
 * @param host SOCKS 请求里的目标主机。
 * @return IPv4 或 IPv6 字面量返回 true；域名返回 false。
 */
[[nodiscard]] bool isIpLiteral(const QString &host);

/**
 * @brief 决定这次 SOCKS 目标走隧道还是本机直连。
 * @param networks 已保存的网段；函数内部会再次规范化。
 * @param host SOCKS CONNECT 的目标，可以是 IP 或域名。
 * @param resolvedAddresses host 为域名时，系统 DNS 解析出的地址文本。
 * @param resolutionFailed 域名解析失败或没有得到任何地址。
 * @return 空网段列表一律 Tunnel，并保留原始 host。IP 按网段匹配。域名在解析结果命中时
 *         Tunnel 到命中的那个 IP，避免远端 DNS 再解析成网段外地址；未命中或解析失败则
 *         Direct，仍使用原始域名，交给系统解析和默认路由。
 */
[[nodiscard]] EgressDecision selectEgress(const QStringList &networks,
                                          const QString &host,
                                          const QStringList &resolvedAddresses,
                                          bool resolutionFailed);

} // namespace ProxyNetwork
