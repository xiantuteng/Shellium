#pragma once

#include <QtGlobal>
#include <QString>

#include <utility>

/**
 * @brief 表示当前操作系统原生网络套接字句柄。
 *
 * POSIX 文件描述符通常是 int，而 64 位 Windows 的 SOCKET 是指针宽度无符号整数。
 * 统一使用 qintptr 可以避免 Windows 上把有效句柄截断为 32 位，同时可直接传给 Qt 的
 * QTcpSocket::setSocketDescriptor()。无效值统一为 -1，调用方不得把普通文件句柄混入本类型。
 */
using NativeSocketHandle = qintptr;

/** @brief 跨平台无效套接字哨兵值。 */
inline constexpr NativeSocketHandle InvalidNativeSocket = static_cast<NativeSocketHandle>(-1);

/**
 * @brief 判断原生套接字句柄是否有效。
 * @param handle 待检查的句柄。
 * @return 不是跨平台无效哨兵时返回 true。
 */
[[nodiscard]] bool isNativeSocketValid(NativeSocketHandle handle) noexcept;

/**
 * @brief 关闭原生套接字。
 * @param handle 由当前调用方拥有的套接字；无效句柄会被安全忽略。
 *
 * Windows 必须使用 closesocket，POSIX 必须使用 close。集中封装可避免把 SOCKET 错当成
 * CRT 文件描述符，亦让所有失败分支保持相同的所有权语义。
 */
void closeNativeSocket(NativeSocketHandle handle) noexcept;

/**
 * @brief 将套接字切换为阻塞或非阻塞模式。
 * @param handle 有效套接字。
 * @param nonBlocking true 表示非阻塞，false 表示阻塞。
 * @param errorMessage 失败时接收平台错误文本，可为空。
 * @return 设置成功返回 true。
 */
[[nodiscard]] bool setNativeSocketNonBlocking(NativeSocketHandle handle,
                                              bool nonBlocking,
                                              QString *errorMessage = nullptr);

/**
 * @brief 调整套接字发送和接收缓冲区。
 * @param handle 有效套接字。
 * @param bytes 期望的缓冲区字节数，必须大于零。
 * @param errorMessage 失败时接收平台错误文本，可为空。
 * @return 两个方向均设置成功返回 true。
 */
[[nodiscard]] bool setNativeSocketBufferSize(NativeSocketHandle handle,
                                             int bytes,
                                             QString *errorMessage = nullptr);

/**
 * @brief 创建一对已经互相连通的流式套接字。
 * @param first 成功时接收第一端所有权。
 * @param second 成功时接收第二端所有权。
 * @param errorMessage 失败时接收可读错误，可为空。
 * @return 成功返回 true；失败时两个输出均保持无效。
 *
 * POSIX 使用 AF_UNIX socketpair。Windows 没有 socketpair，故通过只绑定回环地址的临时
 * TCP 监听器完成 connect/accept，并禁用句柄继承；监听器绝不暴露到非本机网络。
 */
[[nodiscard]] bool createConnectedSocketPair(NativeSocketHandle *first,
                                             NativeSocketHandle *second,
                                             QString *errorMessage = nullptr);

/**
 * @brief 从套接字读取字节。
 * @param handle 有效套接字。
 * @param buffer 接收缓冲区。
 * @param size 缓冲区容量。
 * @return 正数为读取量，0 为对端关闭，-1 为失败或暂时无数据；后者可用
 *         nativeSocketWouldBlock() 区分。
 */
qint64 readNativeSocket(NativeSocketHandle handle, char *buffer, qint64 size) noexcept;

/**
 * @brief 向套接字写入字节。
 * @param handle 有效套接字。
 * @param buffer 待发送缓冲区。
 * @param size 待发送字节数。
 * @return 正数为写入量，0 为未写入，-1 为失败或暂时不可写。
 */
qint64 writeNativeSocket(NativeSocketHandle handle, const char *buffer, qint64 size) noexcept;

/** @brief 返回最近一次套接字调用是否仅因“稍后重试”而失败。 */
[[nodiscard]] bool nativeSocketWouldBlock() noexcept;

/** @brief 返回最近一次套接字错误的本地化可读文本。 */
[[nodiscard]] QString lastNativeSocketError();

/**
 * @brief 独占持有一个原生套接字的移动型 RAII 包装器。
 *
 * 失败路径由析构自动关闭；把句柄成功交给 libssh 或 QTcpSocket 后必须调用 release()，
 * 以明确转移所有权并阻止重复关闭。
 */
class UniqueNativeSocket final {
public:
    UniqueNativeSocket() noexcept = default;
    explicit UniqueNativeSocket(NativeSocketHandle handle) noexcept : m_handle(handle) {}
    ~UniqueNativeSocket() { reset(); }

    UniqueNativeSocket(const UniqueNativeSocket &) = delete;
    UniqueNativeSocket &operator=(const UniqueNativeSocket &) = delete;

    UniqueNativeSocket(UniqueNativeSocket &&other) noexcept : m_handle(other.release()) {}
    UniqueNativeSocket &operator=(UniqueNativeSocket &&other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    /** @brief 返回当前句柄但不转移所有权。 */
    [[nodiscard]] NativeSocketHandle get() const noexcept { return m_handle; }

    /** @brief 返回当前对象是否持有有效套接字。 */
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return isNativeSocketValid(m_handle);
    }

    /** @brief 释放所有权并返回句柄，调用方随后负责关闭或继续转交。 */
    [[nodiscard]] NativeSocketHandle release() noexcept
    {
        return std::exchange(m_handle, InvalidNativeSocket);
    }

    /** @brief 关闭旧句柄并接管 replacement；默认仅关闭旧句柄。 */
    void reset(NativeSocketHandle replacement = InvalidNativeSocket) noexcept
    {
        if (isNativeSocketValid(m_handle)) {
            closeNativeSocket(m_handle);
        }
        m_handle = replacement;
    }

private:
    NativeSocketHandle m_handle = InvalidNativeSocket;
};
