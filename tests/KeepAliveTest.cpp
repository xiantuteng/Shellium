#include "ssh/SshSessionNode.h"

#include <QTest>

/**
 * @brief 验证空闲连接保活的判定策略。
 *
 * 保活的发送本身需要一个真实服务器才能观察，但"什么时候该发、该给哪几层发"是纯逻辑，
 * 也正是最容易写错的部分（漏发外层、边界差一毫秒、握手期误发），因此单独抽出来覆盖。
 */
class KeepAliveTest final : public QObject {
    Q_OBJECT

private slots:
    /** @brief 空闲不足间隔时不得发送；恰好达到间隔就必须发送。 */
    void firesOnlyAfterIdleIntervalElapsed();

    /** @brief 未就绪、未开始计时或显式关闭时都不得发送。 */
    void staysSilentWhenSessionCannotOrNeedNotBeProbed();

    /** @brief 空闲的外层必须收到保活，哪怕栈顶正忙。 */
    void probesEveryIdleLayerNotOnlyTheTop();

    /** @brief 各层空闲情况不同时，只挑真正空闲的那些层。 */
    void selectsExactlyTheIdleLayers();

    /** @brief 默认间隔必须短于常见 NAT 表项寿命，否则保活形同虚设。 */
    void defaultIntervalStaysBelowCommonNatTimeout();

    /** @brief 一次发送失败后必须至少退避 1 秒，不能按 10ms 泵送周期忙重试。 */
    void failedSendUsesMinimumRetryBackoff();
};

void KeepAliveTest::firesOnlyAfterIdleIntervalElapsed()
{
    constexpr qint64 interval = SshKeepAlive::kIdleIntervalMs;
    QVERIFY(!SshKeepAlive::shouldSend(true, 0, interval));
    QVERIFY(!SshKeepAlive::shouldSend(true, interval - 1, interval));
    // 边界必须是"达到即发"：写成严格大于会让判定依赖调度抖动，在计时精度恰好落在整数
    // 毫秒时白白多等一整轮。
    QVERIFY(SshKeepAlive::shouldSend(true, interval, interval));
    QVERIFY(SshKeepAlive::shouldSend(true, interval + 1, interval));
    QVERIFY(SshKeepAlive::shouldSend(true, interval * 100, interval));
}

void KeepAliveTest::staysSilentWhenSessionCannotOrNeedNotBeProbed()
{
    constexpr qint64 interval = SshKeepAlive::kIdleIntervalMs;
    // 建立阶段由握手超时兜底；此时会话上还没有可发保活的加密通道。
    QVERIFY(!SshKeepAlive::shouldSend(false, interval * 10, interval));
    // 计时器尚未启动（该层刚构造）不能被当成"已空闲很久"。
    QVERIFY(!SshKeepAlive::shouldSend(true, -1, interval));
    // 间隔非正表示关闭保活。
    QVERIFY(!SshKeepAlive::shouldSend(true, interval * 10, 0));
    QVERIFY(!SshKeepAlive::shouldSend(true, interval * 10, -1));
}

void KeepAliveTest::probesEveryIdleLayerNotOnlyTheTop()
{
    constexpr qint64 interval = SshKeepAlive::kIdleIntervalMs;
    // 用户长时间只在内层敲命令：栈顶一直活跃，而承载它的两层外层已经空闲很久。外层一旦
    // 被中间设备掐断，内层随之全部失效，所以外层必须照发。
    const QList<SshKeepAlive::LayerIdleState> layers{
        {true, interval * 3},
        {true, interval * 2},
        {true, 5},
    };
    QCOMPARE(SshKeepAlive::layersNeedingKeepAlive(layers, interval), (QList<int>{0, 1}));
}

void KeepAliveTest::selectsExactlyTheIdleLayers()
{
    constexpr qint64 interval = SshKeepAlive::kIdleIntervalMs;
    const QList<SshKeepAlive::LayerIdleState> layers{
        {true, 0},                 // 刚有数据往来
        {true, interval},          // 恰好到期
        {false, interval * 5},     // 仍在建立
        {true, interval - 1},      // 差一毫秒
        {true, interval + 1},      // 已过期
    };
    QCOMPARE(SshKeepAlive::layersNeedingKeepAlive(layers, interval), (QList<int>{1, 4}));

    QVERIFY(SshKeepAlive::layersNeedingKeepAlive({}, interval).isEmpty());
}

void KeepAliveTest::defaultIntervalStaysBelowCommonNatTimeout()
{
    // 观察到的最短一档 NAT/防火墙空闲回收在 60 秒量级；间隔必须留出至少一次重发余量。
    QVERIFY(SshKeepAlive::kIdleIntervalMs > 0);
    QVERIFY(SshKeepAlive::kIdleIntervalMs <= 30000);
}

void KeepAliveTest::failedSendUsesMinimumRetryBackoff()
{
    constexpr qint64 interval = SshKeepAlive::kIdleIntervalMs;
    const QList<SshKeepAlive::LayerIdleState> layers{
        {true, interval * 2, -1},
        {true, interval * 2, SshKeepAlive::kMinimumRetryIntervalMs - 1},
        {true, interval * 2, SshKeepAlive::kMinimumRetryIntervalMs},
    };
    // 从未尝试与恰好到达退避边界的层可发送；刚失败的中间层必须保持安静。
    QCOMPARE(SshKeepAlive::layersNeedingKeepAlive(layers, interval), (QList<int>{0, 2}));
    QVERIFY(SshKeepAlive::kMinimumRetryIntervalMs >= 1000);
}

QTEST_APPLESS_MAIN(KeepAliveTest)

#include "KeepAliveTest.moc"
