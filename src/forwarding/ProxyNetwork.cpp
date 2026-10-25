#include "forwarding/ProxyNetwork.h"

#include <QtGlobal>

#include <array>
#include <algorithm>

namespace ProxyNetwork {
namespace {

constexpr int kIpv4Bytes = 4;
constexpr int kIpv6Bytes = 16;
constexpr int kIpv6Groups = 8;

struct IpAddress {
    bool v6 = false;
    std::array<quint8, kIpv6Bytes> bytes{};
};

QString clipEntry(const QString &entry)
{
    QString text = entry.simplified();
    constexpr int kMaximumLength = 80;
    if (text.size() > kMaximumLength) {
        text.truncate(kMaximumLength - 1);
        text.append(QChar(0x2026));
    }
    return text;
}

bool parseIpv4(const QString &text, quint8 out[kIpv4Bytes])
{
    const QStringList parts = text.split(QLatin1Char('.'));
    if (parts.size() != kIpv4Bytes) {
        return false;
    }
    for (int index = 0; index < kIpv4Bytes; ++index) {
        const QString part = parts.at(index);
        if (part.isEmpty() || part.size() > 3) {
            return false;
        }
        for (const QChar character : part) {
            if (!character.isDigit()) {
                return false;
            }
        }
        bool ok = false;
        const int value = part.toInt(&ok, 10);
        if (!ok || value < 0 || value > 255) {
            return false;
        }
        out[index] = static_cast<quint8>(value);
    }
    return true;
}

bool parseIpv6(const QString &text, quint8 out[kIpv6Bytes])
{
    if (text.isEmpty() || text.contains(QLatin1Char('%'))) {
        return false;
    }

    QString input = text;
    const int dot = input.indexOf(QLatin1Char('.'));
    if (dot >= 0) {
        const int lastColon = input.lastIndexOf(QLatin1Char(':'));
        if (lastColon < 0 || dot < lastColon) {
            return false;
        }
        quint8 ipv4[kIpv4Bytes] = {};
        if (!parseIpv4(input.mid(lastColon + 1), ipv4)) {
            return false;
        }
        const quint16 high = static_cast<quint16>((ipv4[0] << 8) | ipv4[1]);
        const quint16 low = static_cast<quint16>((ipv4[2] << 8) | ipv4[3]);
        input = input.left(lastColon + 1)
            + QString::number(high, 16)
            + QLatin1Char(':')
            + QString::number(low, 16);
    }

    const int compression = input.indexOf(QStringLiteral("::"));
    if (compression >= 0 && compression != input.lastIndexOf(QStringLiteral("::"))) {
        return false;
    }

    QStringList head;
    QStringList tail;
    if (compression >= 0) {
        const QString headText = input.left(compression);
        const QString tailText = input.mid(compression + 2);
        if (!headText.isEmpty()) {
            head = headText.split(QLatin1Char(':'));
        }
        if (!tailText.isEmpty()) {
            tail = tailText.split(QLatin1Char(':'));
        }
        // "::" 至少要折叠一组零，所以显式组数量必须小于 8。
        if (head.size() + tail.size() >= kIpv6Groups) {
            return false;
        }
    } else {
        head = input.split(QLatin1Char(':'));
        if (head.size() != kIpv6Groups) {
            return false;
        }
    }

    const int missing = kIpv6Groups - head.size() - tail.size();
    QStringList groups = head;
    for (int index = 0; index < missing; ++index) {
        groups.append(QStringLiteral("0"));
    }
    groups.append(tail);
    if (groups.size() != kIpv6Groups) {
        return false;
    }

    for (int index = 0; index < kIpv6Groups; ++index) {
        const QString group = groups.at(index);
        if (group.isEmpty() || group.size() > 4) {
            return false;
        }
        for (const QChar character : group) {
            if (!character.isDigit() && (character.toLower() < QLatin1Char('a')
                                         || character.toLower() > QLatin1Char('f'))) {
                return false;
            }
        }
        bool ok = false;
        const uint value = group.toUInt(&ok, 16);
        if (!ok || value > 0xFFFF) {
            return false;
        }
        out[index * 2] = static_cast<quint8>(value >> 8);
        out[index * 2 + 1] = static_cast<quint8>(value & 0xFF);
    }
    return true;
}

bool parseIp(const QString &text, IpAddress *address)
{
    if (address == nullptr) {
        return false;
    }
    *address = {};
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.contains(QLatin1Char('%'))) {
        return false;
    }
    if (trimmed.contains(QLatin1Char(':'))) {
        address->v6 = true;
        return parseIpv6(trimmed, address->bytes.data());
    }
    return parseIpv4(trimmed, address->bytes.data());
}

/** @brief 把 IPv4 映射地址收成 IPv4，便于 10.0.0.0/8 能命中 ::ffff:10.x。 */
bool normalizeMapped(IpAddress *address, int *prefix)
{
    if (address == nullptr || !address->v6) {
        return false;
    }
    for (int index = 0; index < 10; ++index) {
        if (address->bytes[static_cast<size_t>(index)] != 0) {
            return false;
        }
    }
    if (address->bytes[10] != 0xFF || address->bytes[11] != 0xFF) {
        return false;
    }
    // 前缀还没覆盖 ::ffff:0:0/96 时，这是普通 IPv6 网段，不能改写成 IPv4。
    if (prefix != nullptr && *prefix < 96) {
        return false;
    }
    IpAddress ipv4;
    std::copy_n(address->bytes.begin() + 12, kIpv4Bytes, ipv4.bytes.begin());
    *address = ipv4;
    if (prefix != nullptr) {
        *prefix -= 96;
    }
    return true;
}

void maskPrefix(IpAddress *address, const int prefix)
{
    if (address == nullptr) {
        return;
    }
    const int totalBytes = address->v6 ? kIpv6Bytes : kIpv4Bytes;
    const int fullBytes = prefix / 8;
    const int remainder = prefix % 8;
    int index = fullBytes;
    if (remainder != 0 && index < totalBytes) {
        const auto mask = static_cast<quint8>(0xFF << (8 - remainder));
        address->bytes[static_cast<size_t>(index)] &= mask;
        ++index;
    }
    for (; index < totalBytes; ++index) {
        address->bytes[static_cast<size_t>(index)] = 0;
    }
}

QString canonicalIp(const IpAddress &address)
{
    if (!address.v6) {
        return QStringLiteral("%1.%2.%3.%4")
            .arg(address.bytes[0])
            .arg(address.bytes[1])
            .arg(address.bytes[2])
            .arg(address.bytes[3]);
    }

    std::array<quint16, kIpv6Groups> groups{};
    for (int index = 0; index < kIpv6Groups; ++index) {
        groups[static_cast<size_t>(index)] = static_cast<quint16>(
            (address.bytes[static_cast<size_t>(index * 2)] << 8)
            | address.bytes[static_cast<size_t>(index * 2 + 1)]);
    }
    int bestStart = -1;
    int bestLength = 0;
    int runStart = -1;
    for (int index = 0; index <= kIpv6Groups; ++index) {
        const bool zero = index < kIpv6Groups && groups[static_cast<size_t>(index)] == 0;
        if (zero) {
            if (runStart < 0) {
                runStart = index;
            }
            continue;
        }
        if (runStart >= 0) {
            const int length = index - runStart;
            if (length > bestLength) {
                bestLength = length;
                bestStart = runStart;
            }
            runStart = -1;
        }
    }
    if (bestLength < 2) {
        bestStart = -1;
    }

    QString text;
    for (int index = 0; index < kIpv6Groups;) {
        if (index == bestStart) {
            text.append(QStringLiteral("::"));
            index += bestLength;
            continue;
        }
        if (!text.isEmpty() && !text.endsWith(QLatin1Char(':'))) {
            text.append(QLatin1Char(':'));
        }
        text.append(QString::number(groups[static_cast<size_t>(index)], 16));
        ++index;
    }
    if (text.endsWith(QLatin1Char(':')) && !text.endsWith(QStringLiteral("::"))) {
        text.chop(1);
    }
    return text;
}

QString canonicalCidr(IpAddress network, int prefix)
{
    normalizeMapped(&network, &prefix);
    maskPrefix(&network, prefix);
    return canonicalIp(network) + QLatin1Char('/') + QString::number(prefix);
}

bool parseCidr(const QString &token, IpAddress *network, int *prefix)
{
    if (network == nullptr || prefix == nullptr) {
        return false;
    }
    const QString trimmed = token.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    const int slash = trimmed.indexOf(QLatin1Char('/'));
    if (slash != trimmed.lastIndexOf(QLatin1Char('/'))) {
        return false;
    }
    const QString addressText = slash < 0 ? trimmed : trimmed.left(slash);
    if (!parseIp(addressText, network)) {
        return false;
    }
    const int maximumPrefix = network->v6 ? 128 : 32;
    if (slash < 0) {
        *prefix = maximumPrefix;
        return true;
    }
    const QString prefixText = trimmed.mid(slash + 1);
    if (prefixText.isEmpty()) {
        return false;
    }
    for (const QChar character : prefixText) {
        if (!character.isDigit()) {
            return false;
        }
    }
    bool ok = false;
    const int value = prefixText.toInt(&ok, 10);
    if (!ok || value < 0 || value > maximumPrefix) {
        return false;
    }
    *prefix = value;
    return true;
}

bool samePrefix(const IpAddress &left, const IpAddress &right, const int prefix)
{
    const int totalBytes = left.v6 ? kIpv6Bytes : kIpv4Bytes;
    const int fullBytes = prefix / 8;
    const int remainder = prefix % 8;
    for (int index = 0; index < fullBytes; ++index) {
        if (left.bytes[static_cast<size_t>(index)] != right.bytes[static_cast<size_t>(index)]) {
            return false;
        }
    }
    if (remainder == 0 || fullBytes >= totalBytes) {
        return true;
    }
    const auto mask = static_cast<quint8>(0xFF << (8 - remainder));
    return (left.bytes[static_cast<size_t>(fullBytes)] & mask)
        == (right.bytes[static_cast<size_t>(fullBytes)] & mask);
}

bool matchesCidr(IpAddress address, IpAddress network, int prefix)
{
    const int addressPrefixSentinel = address.v6 ? 128 : 32;
    int addressPrefix = addressPrefixSentinel;
    normalizeMapped(&address, &addressPrefix);
    normalizeMapped(&network, &prefix);
    if (address.v6 != network.v6 || prefix < 0) {
        return false;
    }
    const int maximumPrefix = address.v6 ? 128 : 32;
    if (prefix > maximumPrefix) {
        return false;
    }
    return samePrefix(address, network, prefix);
}

bool matchesAny(const QStringList &networks, const IpAddress &address)
{
    for (const QString &networkText : networks) {
        IpAddress network;
        int prefix = 0;
        if (!parseCidr(networkText, &network, &prefix)) {
            continue;
        }
        if (matchesCidr(address, network, prefix)) {
            return true;
        }
    }
    return false;
}

} // namespace

QStringList tokenize(const QString &text)
{
    QStringList tokens;
    QString current;
    const auto flush = [&tokens, &current]() {
        const QString trimmed = current.trimmed();
        current.clear();
        if (!trimmed.isEmpty()) {
            tokens.append(trimmed);
        }
    };
    for (const QChar character : text) {
        const bool separator = character.isSpace()
            || character == QLatin1Char(',')
            || character == QLatin1Char(';')
            || character == QChar(0xFF0C)  // ，
            || character == QChar(0xFF1B)  // ；
            || character == QChar(0x3001); // 、
        if (separator) {
            flush();
            continue;
        }
        current.append(character);
    }
    flush();
    return tokens;
}

ParseResult normalize(const QStringList &networks)
{
    ParseResult result;
    QStringList seen;
    for (const QString &entry : networks) {
        const QStringList tokens = tokenize(entry);
        for (const QString &token : tokens) {
            if (result.networks.size() >= kMaxProxyNetworks) {
                result.networks.clear();
                result.tooMany = true;
                return result;
            }
            IpAddress network;
            int prefix = 0;
            if (!parseCidr(token, &network, &prefix)) {
                result.networks.clear();
                result.invalidEntry = clipEntry(token);
                return result;
            }
            const QString canonical = canonicalCidr(network, prefix);
            if (seen.contains(canonical)) {
                continue;
            }
            seen.append(canonical);
            result.networks.append(canonical);
        }
    }
    return result;
}

bool isIpLiteral(const QString &host)
{
    IpAddress address;
    QString text = host.trimmed();
    const int zone = text.indexOf(QLatin1Char('%'));
    if (zone > 0) {
        text = text.left(zone);
    }
    return parseIp(text, &address);
}

EgressDecision selectEgress(const QStringList &networks,
                            const QString &host,
                            const QStringList &resolvedAddresses,
                            const bool resolutionFailed)
{
    EgressDecision decision;
    decision.targetHost = host.trimmed();
    const ParseResult parsed = normalize(networks);
    if (parsed.tooMany || !parsed.invalidEntry.isEmpty()) {
        decision.egress = Egress::Reject;
        decision.invalidEntry = parsed.invalidEntry;
        decision.tooMany = parsed.tooMany;
        return decision;
    }
    if (parsed.networks.isEmpty()) {
        decision.egress = Egress::Tunnel;
        return decision;
    }

    QString literalText = decision.targetHost;
    const int zone = literalText.indexOf(QLatin1Char('%'));
    if (zone > 0) {
        literalText = literalText.left(zone);
    }
    IpAddress literal;
    if (parseIp(literalText, &literal)) {
        decision.egress = matchesAny(parsed.networks, literal) ? Egress::Tunnel : Egress::Direct;
        return decision;
    }

    if (!resolutionFailed) {
        for (const QString &resolved : resolvedAddresses) {
            QString addressText = resolved.trimmed();
            const int resolvedZone = addressText.indexOf(QLatin1Char('%'));
            if (resolvedZone > 0) {
                addressText = addressText.left(resolvedZone);
            }
            IpAddress address;
            if (!parseIp(addressText, &address)) {
                continue;
            }
            if (matchesAny(parsed.networks, address)) {
                int prefix = address.v6 ? 128 : 32;
                normalizeMapped(&address, &prefix);
                decision.egress = Egress::Tunnel;
                // 连到命中的具体 IP，避免远端再次解析域名时落到网段之外。
                decision.targetHost = canonicalIp(address);
                return decision;
            }
        }
    }
    decision.egress = Egress::Direct;
    return decision;
}

} // namespace ProxyNetwork
