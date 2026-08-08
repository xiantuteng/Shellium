#include "forwarding/Socks5Protocol.h"

#include <QHostAddress>

#include <QtEndian>

namespace Socks5Protocol {

GreetingResult parseGreeting(const QByteArray &buffer)
{
    GreetingResult result;
    if (buffer.size() < 2) {
        return result;
    }
    const quint8 version = static_cast<quint8>(buffer.at(0));
    const quint8 methodCount = static_cast<quint8>(buffer.at(1));
    if (version != 0x05 || methodCount == 0) {
        result.status = ParseStatus::Reject;
        result.consumed = qMin<qsizetype>(buffer.size(), 2);
        result.response = QByteArray::fromHex("05ff");
        result.error = QStringLiteral("客户端没有发送合法的 SOCKS5 方法协商");
        return result;
    }
    const qsizetype frameSize = 2 + methodCount;
    if (buffer.size() < frameSize) {
        return result;
    }
    result.consumed = frameSize;
    if (!buffer.mid(2, methodCount).contains('\0')) {
        result.status = ParseStatus::Reject;
        result.response = QByteArray::fromHex("05ff");
        result.error = QStringLiteral("客户端不支持无需认证的 SOCKS5 方法");
        return result;
    }
    result.status = ParseStatus::Complete;
    result.response = QByteArray::fromHex("0500");
    return result;
}

ConnectResult parseConnectRequest(const QByteArray &buffer)
{
    ConnectResult result;
    if (buffer.size() < 4) {
        return result;
    }
    const quint8 version = static_cast<quint8>(buffer.at(0));
    const quint8 command = static_cast<quint8>(buffer.at(1));
    const quint8 reserved = static_cast<quint8>(buffer.at(2));
    const quint8 addressType = static_cast<quint8>(buffer.at(3));
    if (version != 0x05 || reserved != 0x00) {
        result.status = ParseStatus::Reject;
        result.replyCode = 0x01;
        result.error = QStringLiteral("SOCKS5 CONNECT 头无效");
        return result;
    }
    if (command != 0x01) {
        result.status = ParseStatus::Reject;
        result.replyCode = 0x07;
        result.error = QStringLiteral("动态转发仅支持 SOCKS5 CONNECT 命令");
        return result;
    }

    qsizetype addressOffset = 4;
    qsizetype addressLength = 0;
    if (addressType == 0x01) {
        addressLength = 4;
    } else if (addressType == 0x04) {
        addressLength = 16;
    } else if (addressType == 0x03) {
        if (buffer.size() < 5) {
            return result;
        }
        addressLength = static_cast<quint8>(buffer.at(4));
        addressOffset = 5;
        if (addressLength == 0) {
            result.status = ParseStatus::Reject;
            result.replyCode = 0x08;
            result.error = QStringLiteral("SOCKS5 域名长度不能为 0");
            return result;
        }
    } else {
        result.status = ParseStatus::Reject;
        result.replyCode = 0x08;
        result.error = QStringLiteral("SOCKS5 地址类型不受支持");
        return result;
    }

    const qsizetype frameSize = addressOffset + addressLength + 2;
    if (buffer.size() < frameSize) {
        return result;
    }
    const QByteArray addressBytes = buffer.mid(addressOffset, addressLength);
    if (addressType == 0x03) {
        result.host = QString::fromUtf8(addressBytes);
        if (result.host.contains(QChar::Null)) {
            result.status = ParseStatus::Reject;
            result.replyCode = 0x08;
            result.error = QStringLiteral("SOCKS5 域名包含 NUL 字符");
            return result;
        }
    } else {
        QHostAddress address;
        if (addressType == 0x01) {
            const quint32 ipv4 = qFromBigEndian<quint32>(
                reinterpret_cast<const uchar *>(addressBytes.constData()));
            address = QHostAddress(ipv4);
        } else {
            Q_IPV6ADDR ipv6{};
            std::copy(addressBytes.cbegin(), addressBytes.cend(),
                      reinterpret_cast<char *>(ipv6.c));
            address = QHostAddress(ipv6);
        }
        result.host = address.toString();
    }

    result.port = qFromBigEndian<quint16>(
        reinterpret_cast<const uchar *>(buffer.constData() + frameSize - 2));
    if (result.host.trimmed().isEmpty() || result.port == 0) {
        result.status = ParseStatus::Reject;
        result.replyCode = 0x04;
        result.error = QStringLiteral("SOCKS5 目标地址或端口无效");
        return result;
    }
    result.status = ParseStatus::Complete;
    result.replyCode = 0;
    result.consumed = frameSize;
    return result;
}

QByteArray reply(const quint8 replyCode)
{
    QByteArray response(10, '\0');
    response[0] = 0x05;
    response[1] = static_cast<char>(replyCode);
    response[2] = 0x00;
    response[3] = 0x01; // ATYP IPv4，后续 0.0.0.0:0 表示未提供具体绑定地址。
    return response;
}

} // namespace Socks5Protocol
