#include <QApplication>
#include <QByteArray>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTextDocument>
#include <QtTest>

#include "terminal/LocalShellWorker.h"
#include "terminal/VtermBridge.h"

namespace {

/** @brief 终端网格行列数；列数取大值，让"填满整屏"只需要一段连续字节。 */
constexpr int kRows = 24;
constexpr int kColumns = 200;

/**
 * @brief 造一条填满整块网格、且全程不含换行的逻辑行。
 *
 * `cat` 一个二进制文件时屏幕就是这个样子：长段字节里没有 \n，libvterm 把它们连成一条
 * 横跨整屏的续行，光标停在这条行的最后一格。
 */
QByteArray screenFillingUnbrokenLine()
{
    return QByteArray(kRows * kColumns, 'x');
}

} // namespace

/**
 * @brief `cat 二进制文件` 这类输出洪流下的终端回归。
 *
 * 覆盖三个由同一条命令暴露出来的缺陷：libvterm 回流缩放时的进程 abort、BEL 风暴、
 * 以及 PTY 读取端缺少背压导致的界面冻结。
 */
class TerminalFloodTest : public QObject
{
    Q_OBJECT

private slots:
    /** @brief 屏幕被一条超长续行填满时缩小网格不得让进程 abort。 */
    void shrinkingGridWithScreenFillingLineSurvives();
    /** @brief 缩放必须真的生效，不能因为规避崩溃而被跳过。 */
    void resizeStillAppliesNewGeometry();
    /** @brief DECRC 灌回来的越界光标不得让下一次缩放 abort 掉进程。 */
    void staleSavedCursorSurvivesResize();
    /** @brief 密集 BEL 必须被合成为稀疏的响铃事件。 */
    void denseBellsAreCoalesced();
    /** @brief 孤立的 BEL 不受限流影响，第一声必须立即发出。 */
    void firstBellRingsImmediately();
    /** @brief GUI 线程不回执时，PTY 读取端必须自行停下而不是无限灌数据。 */
    void unconsumedOutputStopsPtyReader();
};

void TerminalFloodTest::shrinkingGridWithScreenFillingLineSurvives()
{
    // 回归目标：libvterm 0.3.3 的 screen_resize 在回流时用
    //     if(old_row_start <= old_cursor.row && old_cursor.row < old_row_end)
    // 补救光标，末尾的 `<` 是 off-by-one。光标恰好停在这条放不进新网格的逻辑行最后一行
    // 时两侧都不成立，libvterm 随即 fprintf + abort()——不是断言，Release 下照样死。
    //
    // 这条用例一旦回归，表现不是断言失败而是整个测试进程被 SIGABRT 打死，QTest 会把它
    // 报成 crash。这正是用户遇到的现象：两份栈完全一致的 Shellium 崩溃报告。
    VtermBridge bridge;
    bridge.resize(kRows, kColumns);
    bridge.feed(screenFillingUnbrokenLine());

    // 逐行收缩，覆盖"刚好差一行"到"只剩一行"的整个区间。
    for (int rows = kRows - 1; rows >= 1; --rows) {
        bridge.resize(rows, kColumns);
    }
    // 同时收缩列数：libvterm 的 new_height = ceil(width / new_cols) 会随列数变小而变大，
    // 缩列同样能把逻辑行顶出新网格。
    bridge.resize(kRows, kColumns);
    bridge.feed(screenFillingUnbrokenLine());
    bridge.resize(kRows, kColumns / 4);

    // 真实的窗口拖拽是"缩放—继续输出—再缩放"的连续多轮，每一轮都带着上一轮留下的状态。
    // 这一段才是关键：只做单次缩放的用例曾经放过了一个缺陷修复——当时的做法是"识别危险
    // 状态再临时关掉回流"，单次缩放全部拦住，多轮缩放却在真实文件上照样 abort。
    bridge.resize(kRows, kColumns);
    for (int round = 0; round < 40; ++round) {
        bridge.feed(screenFillingUnbrokenLine());
        bridge.resize(kRows - round % 9, kColumns - (round % 5) * 20);
        bridge.feed(QByteArray(600, 'q'));
        bridge.resize(kRows - (round + 4) % 11, kColumns - ((round + 2) % 7) * 15);
    }

    // 活到这里就说明没有 abort；再确认桥接层状态仍然可用。
    QTextDocument document;
    bridge.render(&document);
    QVERIFY(document.blockCount() > 0);
}

void TerminalFloodTest::resizeStillAppliesNewGeometry()
{
    // 上一条用例的修复是"必要时临时关掉回流"。必须确认它关掉的只是回流，缩放本身仍然
    // 生效——否则终端会和 PTY 汇报的行列数脱节，全屏程序会画错。
    VtermBridge bridge;
    bridge.resize(kRows, kColumns);
    bridge.feed(screenFillingUnbrokenLine());
    bridge.resize(10, 60);

    // 新网格是 60 列：换行后写 65 个字符必须在第 60 列折行，光标行只剩最后 5 个。
    bridge.feed(QByteArrayLiteral("\r\n") + QByteArray(65, 'z'));
    QCOMPARE(bridge.cursorLineText(), QString(5, QLatin1Char('z')));

    // 光标也必须真的被放进新网格——这正是 libvterm 回流时失手的那一步。
    QTextDocument document;
    bridge.render(&document);
    QVERIFY2(bridge.renderedCursorPosition() >= 0,
             "缩放后光标没有落在文档里，说明网格与内容对不上");
}

void TerminalFloodTest::staleSavedCursorSurvivesResize()
{
    // 回归目标：libvterm 0.3.3 的 savecursor(state, 0)（DECRC，`ESC 8`）直接把
    // state->saved.pos 灌给 state->pos，**不做任何网格夹取**；而缩小网格的 on_resize 只夹
    // state->pos，从不更新 saved.pos。于是"大网格里 ESC 7 → 缩小 → ESC 8"就留下一个越界
    // 的光标行，下一次 vterm_set_size 在 resize_buffer() 里永远找不到它，随即
    // fprintf + abort() 打死整个进程。
    //
    // `cat` 一个二进制文件必然撞上：ESC 7 / ESC 8 只是两个字节的巧合组合，而缩放随时会
    // 发生（改窗口、缩放字号，甚至首帧的行高纠正）。这条路径与回流无关——关掉回流照样
    // 复现，所以它必须有自己的用例。
    //
    // 回归时同样表现为整个测试进程被 SIGABRT 打死。
    VtermBridge bridge;
    bridge.resize(19, 97);
    bridge.feed(QByteArrayLiteral("\x1b[19;40H"));   // 光标到最后一行
    bridge.feed(QByteArrayLiteral("\x1b" "7"));      // DECSC：保存第 18 行
    bridge.resize(18, 97);                           // 缩小：state->pos 被夹到 17
    bridge.feed(QByteArrayLiteral("\x1b" "8"));      // DECRC：把 18 灌回来，越界
    bridge.resize(19, 97);                           // 没有修复时在这里 abort

    // 再走一遍相反方向，确认修复不是只对某一个方向生效。
    bridge.feed(QByteArrayLiteral("\x1b[19;90H\x1b" "7"));
    bridge.resize(6, 40);
    bridge.feed(QByteArrayLiteral("\x1b" "8"));
    bridge.resize(24, 200);

    QTextDocument document;
    bridge.render(&document);
    QVERIFY(document.blockCount() > 0);
}

void TerminalFloodTest::denseBellsAreCoalesced()
{
    // 回归目标：用户 `cat` 的 34MB jar 含 122873 个 BEL 字节。不限流就是十几万次
    // QApplication::beep()——机器持续"嘟嘟"响，GUI 线程也在这期间完全不重绘。
    VtermBridge bridge;
    bridge.resize(kRows, kColumns);
    QSignalSpy bells(&bridge, &VtermBridge::bell);

    constexpr int kBellCount = 20000;
    QElapsedTimer elapsed;
    elapsed.start();
    bridge.feed(QByteArray(kBellCount, '\a'));
    const qint64 feedMilliseconds = elapsed.elapsed();

    // 抑制窗口是 200ms。这一次 feed 用不了多久，因此最多只该放出很少的几声。
    // 用实际耗时推出上界，避免在极慢的机器上误报。
    const int allowed = static_cast<int>(feedMilliseconds / 200) + 2;
    QVERIFY2(bells.count() <= allowed,
             qPrintable(QStringLiteral("%1 个 BEL 字节在 %2ms 内放出了 %3 次响铃，"
                                       "上限 %4——限流失效，机器会一直嘟嘟响")
                            .arg(kBellCount).arg(feedMilliseconds)
                            .arg(bells.count()).arg(allowed)));
    // 但不能一声不响：响铃本身是有意义的提示。
    QCOMPARE(bells.count(), 1);
}

void TerminalFloodTest::firstBellRingsImmediately()
{
    VtermBridge bridge;
    bridge.resize(kRows, kColumns);
    QSignalSpy bells(&bridge, &VtermBridge::bell);

    bridge.feed(QByteArrayLiteral("\a"));
    QCOMPARE(bells.count(), 1);

    // 越过抑制窗口之后必须重新可响，否则长时间会话里响铃会彻底消失。
    QTest::qWait(250);
    bridge.feed(QByteArrayLiteral("\a"));
    QCOMPARE(bells.count(), 2);
}

void TerminalFloodTest::unconsumedOutputStopsPtyReader()
{
    // 回归目标：读取端原本一路读到 EAGAIN，每块都 emit 一个队列信号，而 GUI 线程那侧
    // 处理速度低一到两个数量级。差额全部堆进事件队列，界面在此期间一次都不重绘——这就是
    // 用户看到的"卡住"。修复是有界在途字节数：GUI 线程没回执就别再读 PTY。
    //
    // 这里故意**不**回执，模拟一个被压垮的 GUI 线程，然后观察读取端会不会自己停下来。
    LocalShellWorker worker(QStringLiteral("/bin/zsh"));

    std::atomic<qint64> emitted {0};
    QObject::connect(&worker, &LocalShellWorker::outputReady, &worker,
                     [&emitted](const QByteArray &bytes) {
                         emitted.fetch_add(bytes.size(), std::memory_order_relaxed);
                     },
                     Qt::DirectConnection);

    QSignalSpy started(&worker, &LocalShellWorker::shellStarted);
    worker.start();
    QVERIFY2(started.wait(10000), "本地 Shell 没能在 10 秒内启动");

    // 让 Shell 把提示符打完再开闸，避免把启动输出算进洪流。
    QTest::qWait(1500);
    const qint64 baseline = emitted.load(std::memory_order_relaxed);

    // 灌一股远超上限的输出：128MiB 的 'y'。
    worker.enqueueInput(QByteArrayLiteral("head -c 134217728 /dev/zero | tr '\\0' 'y'\n"));

    // 给足时间——没有背压的话这点时间足够把上百 MB 全部 emit 出来。
    QTest::qWait(6000);
    const qint64 flooded = emitted.load(std::memory_order_relaxed) - baseline;

    // 关闭必须干净利落。背压停下读取之后，子进程正阻塞在写终端上——它停在内核的 tty
    // 输出等待里，连 SIGKILL 都要等它回到用户态才生效。关闭路径若不继续排空 PTY，这里
    // 就是死锁：实测 kill(pid, SIGKILL) 返回 0、kill(pid, 0) 仍返回 0，工作线程永远结束
    // 不了。用户关掉一个正在 `cat` 大文件的标签页走的就是这条路。
    //
    // 断言的是"关得快"而不是"关得掉"：SIGKILL 之后的轮询有上限，超时后 close(masterFd)
    // 会顺带把子进程放出来，因此即使不排空，线程最终也能结束——只是要拖 7 秒。只断言
    // "能结束"的用例抓不住这个缺陷（已实测：去掉排空照样通过）。实测排空到位是 76ms、
    // 缺排空是 7066ms，2 秒的阈值把两者干净地分开。
    constexpr qint64 kShutdownBudgetMilliseconds = 2000;
    QElapsedTimer shutdown;
    shutdown.start();
    worker.requestStop();
    const bool joined = worker.wait(10000);
    const qint64 shutdownMilliseconds = shutdown.elapsed();
    QVERIFY2(joined,
             qPrintable(QStringLiteral("requestStop() 之后工作线程 %1ms 仍未结束")
                            .arg(shutdownMilliseconds)));
    QVERIFY2(shutdownMilliseconds < kShutdownBudgetMilliseconds,
             qPrintable(QStringLiteral("关闭耗时 %1ms，超过 %2ms 预算——关闭路径没有排空 "
                                       "PTY，子进程卡在 tty 输出等待里直到兜底超时才被放出来")
                            .arg(shutdownMilliseconds).arg(kShutdownBudgetMilliseconds)));

    // 上限 1MiB，单次读缓冲 64KiB；再留一倍余量覆盖临界时刻的那一次读取。
    constexpr qint64 kBound = (1 << 20) + 2 * (64 * 1024);
    QVERIFY2(flooded > 0, "读取端一个字节都没发出来，用例没有真正跑起来");
    QVERIFY2(flooded <= kBound,
             qPrintable(QStringLiteral("GUI 线程一次都没回执，读取端却已经发出 %1 字节"
                                       "（上限 %2）——背压失效，事件队列会被无限堆积")
                            .arg(flooded).arg(kBound)));
}

QTEST_MAIN(TerminalFloodTest)
#include "TerminalFloodTest.moc"
