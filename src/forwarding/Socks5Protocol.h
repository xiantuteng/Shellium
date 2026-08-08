#pragma once

#include <QByteArray>
#include <QString>

/**
 * @brief SOCKS5 无认证 CONNECT 握手的纯协议解析器。
 *
 * 动态转发只实现 RFC 1928 的 CONNECT；BIND、UDP ASSOCIATE 与用户名密码认证会返回明确
 * 拒绝码。解析器不做网络 IO，既便于对恶意分片/长度做单元测试，也避免监听线程把不完整
 * 报文误当成目标地址。
 */
namespace Socks5Protocol {

/** @brief 增量解析状态。 */
enum class ParseStatus {
    NeedMore, ///< 当前缓冲还不完整，调用方应继续读取。
    Complete, ///< 报文合法且已完整解析。
    Reject    ///< 报文不支持或已损坏，调用方应回复后关闭连接。
};

/** @brief 方法协商解析结果。 */
struct GreetingResult {
    ParseStatus status = ParseStatus::NeedMore;
    qsizetype consumed = 0;
    QByteArray response; ///< Complete 时为 05 00，Reject 时为 05 FF。
    QString error;
};

/** @brief CONNECT 请求解析结果。 */
struct ConnectResult {
    ParseStatus status = ParseStatus::NeedMore;
    qsizetype consumed = 0;
    QString host;
    quint16 port = 0;
    quint8 replyCode = 0x01; ///< Reject 时使用的 RFC 1928 REP；Complete 时为 0。
    QString error;
};

/** @brief 解析 VER、NMETHODS 与 METHODS，并仅接受 no-auth(0x00)。 */
[[nodiscard]] GreetingResult parseGreeting(const QByteArray &buffer);

/** @brief 解析一个 SOCKS5 CONNECT 请求，支持 IPv4、域名和 IPv6 地址。 */
[[nodiscard]] ConnectResult parseConnectRequest(const QByteArray &buffer);

/**
 * @brief 构造 SOCKS5 回复。
 * @param replyCode RFC 1928 REP，0 表示成功。
 *
 * 当前监听器不向客户端暴露 SSH channel 的真实绑定地址，因此统一返回 IPv4 0.0.0.0:0；
 * 这符合协议允许的“地址不可用”表达，并避免误报本地监听地址为远端出口地址。
 */
[[nodiscard]] QByteArray reply(quint8 replyCode);

} // namespace Socks5Protocol
