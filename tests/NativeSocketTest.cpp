#include "platform/NativeSocket.h"

#include <QTest>

/**
 * @brief 验证 NativeSocket 在当前平台上的连接、读写、非阻塞与 RAII 所有权语义。
 *
 * Windows 使用回环 TCP 模拟 socketpair，POSIX 使用 AF_UNIX；本测试只依赖公开抽象，因此
 * 同一组断言可以及时发现两种实现的行为漂移，尤其是 64 位句柄被截断和重复关闭问题。
 */
class NativeSocketTest final : public QObject {
    Q_OBJECT

private slots:
    /** @brief 创建套接字对并验证双向字节传输与显式所有权转移。 */
    void transfersBytesInBothDirections();

    /** @brief 验证空套接字切换非阻塞后返回“稍后重试”，而不是永久连接错误。 */
    void reportsWouldBlockForEmptyNonBlockingSocket();
};

void NativeSocketTest::transfersBytesInBothDirections()
{
    NativeSocketHandle firstHandle = InvalidNativeSocket;
    NativeSocketHandle secondHandle = InvalidNativeSocket;
    QString error;
    QVERIFY2(createConnectedSocketPair(&firstHandle, &secondHandle, &error),
             qPrintable(error));
    UniqueNativeSocket first(firstHandle);
    UniqueNativeSocket second(secondHandle);

    const QByteArray outbound = QByteArrayLiteral("shellium-native-socket");
    QCOMPARE(writeNativeSocket(first.get(), outbound.constData(), outbound.size()),
             static_cast<qint64>(outbound.size()));
    QByteArray received(outbound.size(), Qt::Uninitialized);
    QCOMPARE(readNativeSocket(second.get(), received.data(), received.size()),
             static_cast<qint64>(outbound.size()));
    QCOMPARE(received, outbound);

    const QByteArray reply = QByteArrayLiteral("reply");
    QCOMPARE(writeNativeSocket(second.get(), reply.constData(), reply.size()),
             static_cast<qint64>(reply.size()));
    received.resize(reply.size());
    QCOMPARE(readNativeSocket(first.get(), received.data(), received.size()),
             static_cast<qint64>(reply.size()));
    QCOMPARE(received, reply);
}

void NativeSocketTest::reportsWouldBlockForEmptyNonBlockingSocket()
{
    NativeSocketHandle firstHandle = InvalidNativeSocket;
    NativeSocketHandle secondHandle = InvalidNativeSocket;
    QString error;
    QVERIFY2(createConnectedSocketPair(&firstHandle, &secondHandle, &error),
             qPrintable(error));
    UniqueNativeSocket first(firstHandle);
    UniqueNativeSocket second(secondHandle);
    QVERIFY2(setNativeSocketNonBlocking(first.get(), true, &error), qPrintable(error));

    char byte = '\0';
    QCOMPARE(readNativeSocket(first.get(), &byte, 1), static_cast<qint64>(-1));
    QVERIFY(nativeSocketWouldBlock());
}

QTEST_MAIN(NativeSocketTest)
#include "NativeSocketTest.moc"
