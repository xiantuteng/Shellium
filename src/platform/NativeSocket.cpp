#include "platform/NativeSocket.h"

#include <QString>

#include <algorithm>
#include <limits>

#if defined(Q_OS_WIN)
#include <WinSock2.h>
#include <Windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

#if defined(Q_OS_WIN)
/**
 * @brief 保证 Winsock 在首次使用前初始化，并在进程退出时成对清理。
 *
 * Qt 通常也会初始化 Winsock，但平台层不能依赖 QTcpSocket 曾经被构造过。函数静态对象的
 * 初始化受 C++ 运行库保护，可安全应对 SFTP 与转发线程同时首次进入。
 */
class WinsockRuntime final {
public:
    WinsockRuntime() { m_error = WSAStartup(MAKEWORD(2, 2), &m_data); }
    ~WinsockRuntime()
    {
        if (m_error == 0) {
            WSACleanup();
        }
    }
    [[nodiscard]] bool ready() const noexcept { return m_error == 0; }
    [[nodiscard]] int error() const noexcept { return m_error; }

private:
    WSADATA m_data {};
    int m_error = WSASYSNOTREADY;
};

WinsockRuntime &winsockRuntime()
{
    static WinsockRuntime runtime;
    return runtime;
}

SOCKET asSocket(const NativeSocketHandle handle) noexcept
{
    return static_cast<SOCKET>(handle);
}

QString windowsSocketErrorText(const int errorCode)
{
    wchar_t *message = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
                                            | FORMAT_MESSAGE_FROM_SYSTEM
                                            | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr,
                                        static_cast<DWORD>(errorCode),
                                        0,
                                        reinterpret_cast<wchar_t *>(&message),
                                        0,
                                        nullptr);
    const QString result = length > 0 && message != nullptr
        ? QString::fromWCharArray(message, static_cast<qsizetype>(length)).trimmed()
        : QStringLiteral("Winsock 错误 %1").arg(errorCode);
    if (message != nullptr) {
        LocalFree(message);
    }
    return result;
}

bool preventSocketInheritance(const SOCKET socketHandle)
{
    // 子进程继承隧道端点会导致父进程关闭后连接仍不结束，因此创建时立即清除继承标志。
    return SetHandleInformation(reinterpret_cast<HANDLE>(socketHandle), HANDLE_FLAG_INHERIT, 0)
        != FALSE;
}
#endif

} // namespace

bool isNativeSocketValid(const NativeSocketHandle handle) noexcept
{
#if defined(Q_OS_WIN)
    return asSocket(handle) != INVALID_SOCKET;
#else
    return handle >= 0;
#endif
}

void closeNativeSocket(const NativeSocketHandle handle) noexcept
{
    if (!isNativeSocketValid(handle)) {
        return;
    }
#if defined(Q_OS_WIN)
    closesocket(asSocket(handle));
#else
    ::close(static_cast<int>(handle));
#endif
}

bool setNativeSocketNonBlocking(const NativeSocketHandle handle,
                                const bool nonBlocking,
                                QString *errorMessage)
{
#if defined(Q_OS_WIN)
    u_long mode = nonBlocking ? 1UL : 0UL;
    const bool success = ioctlsocket(asSocket(handle), FIONBIO, &mode) == 0;
#else
    const int descriptor = static_cast<int>(handle);
    const int currentFlags = fcntl(descriptor, F_GETFL, 0);
    const bool success = currentFlags >= 0
        && fcntl(descriptor,
                 F_SETFL,
                 nonBlocking ? currentFlags | O_NONBLOCK : currentFlags & ~O_NONBLOCK) == 0;
#endif
    if (!success && errorMessage != nullptr) {
        *errorMessage = lastNativeSocketError();
    }
    return success;
}

bool setNativeSocketBufferSize(const NativeSocketHandle handle,
                               const int bytes,
                               QString *errorMessage)
{
    if (bytes <= 0) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("套接字缓冲区大小必须大于零");
        }
        return false;
    }
#if defined(Q_OS_WIN)
    const SOCKET socketHandle = asSocket(handle);
    const bool success = setsockopt(socketHandle, SOL_SOCKET, SO_SNDBUF,
                                    reinterpret_cast<const char *>(&bytes), sizeof(bytes)) == 0
        && setsockopt(socketHandle, SOL_SOCKET, SO_RCVBUF,
                      reinterpret_cast<const char *>(&bytes), sizeof(bytes)) == 0;
#else
    const int descriptor = static_cast<int>(handle);
    const bool success = setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) == 0
        && setsockopt(descriptor, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) == 0;
#endif
    if (!success && errorMessage != nullptr) {
        *errorMessage = lastNativeSocketError();
    }
    return success;
}

bool createConnectedSocketPair(NativeSocketHandle *first,
                               NativeSocketHandle *second,
                               QString *errorMessage)
{
    if (first == nullptr || second == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("套接字对输出参数不能为空");
        }
        return false;
    }
    *first = InvalidNativeSocket;
    *second = InvalidNativeSocket;

#if defined(Q_OS_WIN)
    if (!winsockRuntime().ready()) {
        if (errorMessage != nullptr) {
            *errorMessage = windowsSocketErrorText(winsockRuntime().error());
        }
        return false;
    }

    UniqueNativeSocket listener(static_cast<NativeSocketHandle>(
        socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)));
    if (!listener) {
        if (errorMessage != nullptr) {
            *errorMessage = lastNativeSocketError();
        }
        return false;
    }
    if (!preventSocketInheritance(asSocket(listener.get()))) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("无法禁止临时监听套接字继承");
        }
        return false;
    }
    const BOOL exclusiveAddress = TRUE;
    if (setsockopt(asSocket(listener.get()), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char *>(&exclusiveAddress),
                   sizeof(exclusiveAddress)) != 0) {
        if (errorMessage != nullptr) {
            *errorMessage = lastNativeSocketError();
        }
        return false;
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(asSocket(listener.get()), reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0
        || listen(asSocket(listener.get()), 1) != 0) {
        if (errorMessage != nullptr) {
            *errorMessage = lastNativeSocketError();
        }
        return false;
    }
    int addressLength = sizeof(address);
    if (getsockname(asSocket(listener.get()), reinterpret_cast<sockaddr *>(&address),
                    &addressLength) != 0) {
        if (errorMessage != nullptr) {
            *errorMessage = lastNativeSocketError();
        }
        return false;
    }

    UniqueNativeSocket client(static_cast<NativeSocketHandle>(
        socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)));
    if (!client
        || connect(asSocket(client.get()), reinterpret_cast<const sockaddr *>(&address),
                   sizeof(address)) != 0) {
        if (errorMessage != nullptr) {
            *errorMessage = lastNativeSocketError();
        }
        return false;
    }
    UniqueNativeSocket server(static_cast<NativeSocketHandle>(
        accept(asSocket(listener.get()), nullptr, nullptr)));
    if (!server || !preventSocketInheritance(asSocket(client.get()))
        || !preventSocketInheritance(asSocket(server.get()))) {
        if (errorMessage != nullptr) {
            *errorMessage = lastNativeSocketError();
        }
        return false;
    }

    sockaddr_in clientLocalAddress {};
    sockaddr_in acceptedPeerAddress {};
    int clientAddressLength = sizeof(clientLocalAddress);
    int peerAddressLength = sizeof(acceptedPeerAddress);
    if (getsockname(asSocket(client.get()),
                    reinterpret_cast<sockaddr *>(&clientLocalAddress),
                    &clientAddressLength) != 0
        || getpeername(asSocket(server.get()),
                       reinterpret_cast<sockaddr *>(&acceptedPeerAddress),
                       &peerAddressLength) != 0
        || acceptedPeerAddress.sin_addr.s_addr != htonl(INADDR_LOOPBACK)
        || acceptedPeerAddress.sin_port != clientLocalAddress.sin_port) {
        // accept 与 connect 之间理论上可能被同机其他进程抢入。核对来源端口和回环地址后，
        // 返回的两端才可被视作同一对，避免把陌生本机连接接入 SSH 明文字节桥。
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Windows 回环套接字对来源校验失败");
        }
        return false;
    }
    *first = client.release();
    *second = server.release();
    return true;
#else
    int sockets[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        if (errorMessage != nullptr) {
            *errorMessage = lastNativeSocketError();
        }
        return false;
    }
    *first = sockets[0];
    *second = sockets[1];
    return true;
#endif
}

qint64 readNativeSocket(const NativeSocketHandle handle, char *buffer, const qint64 size) noexcept
{
    const int safeSize = static_cast<int>(std::min<qint64>(size, std::numeric_limits<int>::max()));
#if defined(Q_OS_WIN)
    return recv(asSocket(handle), buffer, safeSize, 0);
#else
    return ::recv(static_cast<int>(handle), buffer, static_cast<size_t>(safeSize), 0);
#endif
}

qint64 writeNativeSocket(const NativeSocketHandle handle,
                         const char *buffer,
                         const qint64 size) noexcept
{
    const int safeSize = static_cast<int>(std::min<qint64>(size, std::numeric_limits<int>::max()));
#if defined(Q_OS_WIN)
    return send(asSocket(handle), buffer, safeSize, 0);
#else
    return ::send(static_cast<int>(handle), buffer, static_cast<size_t>(safeSize), 0);
#endif
}

bool nativeSocketWouldBlock() noexcept
{
#if defined(Q_OS_WIN)
    const int errorCode = WSAGetLastError();
    return errorCode == WSAEWOULDBLOCK || errorCode == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

QString lastNativeSocketError()
{
#if defined(Q_OS_WIN)
    return windowsSocketErrorText(WSAGetLastError());
#else
    return QString::fromLocal8Bit(std::strerror(errno));
#endif
}
