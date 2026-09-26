#include <QApplication>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QtTest>

#if defined(Q_OS_MACOS)
#include <malloc/malloc.h>
#endif

#include "terminal/TerminalView.h"
#include "terminal/VtermBridge.h"

namespace {

/** @brief 当前进程堆上已分配的字节数；无法测量的平台返回 -1。 */
qint64 heapBytesInUse()
{
#if defined(Q_OS_MACOS)
    malloc_statistics_t statistics {};
    malloc_zone_statistics(nullptr, &statistics);
    return static_cast<qint64>(statistics.size_in_use);
#else
    return -1;
#endif
}

/**
 * @brief 一条典型的 Spring Boot 日志行，约 170 列、带两段颜色。
 *
 * 用户报告的"7 个标签 1.7 GB"正是在这类日志持续滚动的标签上出现的。
 */
QByteArray springLogLine(const int index)
{
    return QByteArray("2026-09-25 10:11:12.345 \x1b[32m INFO\x1b[0m 12345 --- [nio-8080-exec-")
        + QByteArray::number(index % 10)
        + "] \x1b[36mc.e.s.o.OrderService\x1b[0m : processed order id="
        + QByteArray::number(100000 + index)
        + " user=alice amount=123.45 status=OK latency=12ms trace=abcdef0123456789\r\n";
}

/**
 * @brief 覆盖紧凑历史必须无损往返的全部单元格形态。
 *
 * 16/256/真彩色、粗斜下划删除反显隐藏、双宽中文、组合字符（一格两个 UTF-16 单元）、
 * emoji 代理对、制表符跳过的"从未写入"空格子，以及两种最容易被行尾裁剪误伤的结尾：
 * 双宽字符结尾、表意空格（双宽且算空白）结尾。
 */
QByteArray richLine(const int index)
{
    QByteArray line = QByteArray("\x1b[1;31mERR\x1b[0m ") + QByteArray::number(index)
        + " \x1b[3;4;38;5;208mwarn\x1b[0m \x1b[9;7mX\x1b[0m\x1b[8mhid\x1b[0m 中文\tcol"
          " e\xcc\x81 \xf0\x9f\x98\x80 \x1b[38;2;1;2;3;48;2;200;100;50mrgb\x1b[0m 尾";
    if (index % 2 == 0) {
        line += "\xe3\x80\x80";
    }
    return line + "\r\n";
}

/** @brief 逐段、逐片段比较正文与字符格式；纯文本相同而颜色不同的差异也能抓到。 */
QString firstDocumentDifference(const QTextDocument &left, const QTextDocument &right)
{
    if (left.blockCount() != right.blockCount()) {
        return QStringLiteral("段数不同：") + QString::number(left.blockCount())
            + QStringLiteral(" vs ") + QString::number(right.blockCount());
    }
    QTextBlock leftBlock = left.begin();
    QTextBlock rightBlock = right.begin();
    for (; leftBlock.isValid(); leftBlock = leftBlock.next(), rightBlock = rightBlock.next()) {
        if (leftBlock.text() != rightBlock.text()) {
            return QStringLiteral("第 ") + QString::number(leftBlock.blockNumber())
                + QStringLiteral(" 段正文不同：") + leftBlock.text()
                + QStringLiteral(" | ") + rightBlock.text();
        }
        if (leftBlock.blockFormat() != rightBlock.blockFormat()) {
            return QStringLiteral("第 ") + QString::number(leftBlock.blockNumber())
                + QStringLiteral(" 段段落格式不同");
        }
        QTextBlock::iterator leftFragment = leftBlock.begin();
        QTextBlock::iterator rightFragment = rightBlock.begin();
        for (; !leftFragment.atEnd() && !rightFragment.atEnd(); ++leftFragment, ++rightFragment) {
            const QTextFragment a = leftFragment.fragment();
            const QTextFragment b = rightFragment.fragment();
            if (a.text() != b.text() || a.charFormat() != b.charFormat()) {
                return QStringLiteral("第 ") + QString::number(leftBlock.blockNumber())
                    + QStringLiteral(" 段片段不同：") + a.text() + QStringLiteral(" | ") + b.text();
            }
        }
        if (leftFragment.atEnd() != rightFragment.atEnd()) {
            return QStringLiteral("第 ") + QString::number(leftBlock.blockNumber())
                + QStringLiteral(" 段片段数不同");
        }
    }
    return {};
}

/** @brief 视口首段段号；firstVisibleBlock() 是受保护成员，测试从像素位置反查。 */
int firstVisibleBlockNumber(TerminalView &view)
{
    return view.cursorForPosition(QPoint(0, 0)).blockNumber();
}

/**
 * @brief 统计视口附近以外、仍然带着排版结果的段数。
 *
 * 排过版的段 lineCount() > 0，且它的 QTextEngine 留着一块 HarfBuzz 塑形缓冲；释放后
 * 段被重建，新段尚未排版，lineCount() 为 0。保留区之外的段必须一个不剩：实现在视口
 * 上方保留两屏、下方保留三屏（Qt 自己会去排版的范围）。
 */
int laidOutBlocksAwayFromViewport(TerminalView &view)
{
    const int firstVisible = firstVisibleBlockNumber(view);
    // 与 TerminalView 的保留区同一算法：上方两屏、下方三屏，"一屏"按视口高度除以字体行高。
    const int page = std::max(1, view.viewport()->height() / std::max(1, view.fontMetrics().height()));
    int count = 0;
    for (QTextBlock block = view.document()->begin(); block.isValid(); block = block.next()) {
        const int number = block.blockNumber();
        // 两处按设计不释放，不计入：首段前面没有段分隔符，删除重写只能换掉它的正文、换不掉
        // 它的块数据；末尾那一屏是 libvterm 的当前屏幕，每批输出都会原地改写，释放了也会
        // 马上重新排版。两者合计至多一屏多一段。
        const bool screenTail = number >= view.document()->blockCount() - view.terminalRows();
        if (number == 0 || screenTail
            || (number >= firstVisible - 2 * page && number <= firstVisible + 3 * page)) {
            continue;
        }
        if (block.layout() != nullptr && block.layout()->lineCount() > 0) {
            ++count;
        }
    }
    return count;
}

/** @brief 分小批灌入日志并让每一批都真正画出来，模拟 `tail -f` 逐行经过视口。 */
void streamLogThroughViewport(TerminalView &view, const int lines)
{
    constexpr int kBatch = 5;
    for (int index = 0; index < lines; index += kBatch) {
        QByteArray batch;
        for (int offset = 0; offset < kBatch; ++offset) {
            batch += springLogLine(index + offset);
        }
        view.appendTerminalOutput(batch);
        view.viewport()->repaint();
        QCoreApplication::processEvents();
    }
    QTest::qWait(50);
}

bool showAndSettle(TerminalView &view)
{
    view.resize(1200, 700);
    view.show();
    if (!QTest::qWaitForWindowExposed(&view)) {
        return false;
    }
    // 首帧会按真实行高纠正一次网格，等防抖提交完再开始灌数据。
    QTest::qWait(300);
    return true;
}

} // namespace

/**
 * @brief 多标签内存占用的回归。
 *
 * 用户开 7 个标签就占 1.7 GB，拆开来是两处：滚动历史逐格保存（每格约 100 字节，单标签
 * 约 200 MB）和 QPlainTextEdit 给每个排过版的段留下的 HarfBuzz 缓冲（单标签约 60 MB）。
 * 这里既锁住内存量级，也锁住"省内存不能损害体验"的那一半：渲染结果逐片段不变、选区不丢、
 * 后台标签不丢历史、导出拿到最新输出。
 */
class TerminalMemoryTest : public QObject
{
    Q_OBJECT

private slots:
    /** @brief 从紧凑历史整体重建的文档必须与逐行增量渲染的文档逐片段一致。 */
    void richHistoryRebuildMatchesIncrementalRender();
    /** @brief 富内容行从屏幕滚进历史时不得被重写，压在上面的选区必须原样保留。 */
    void richLineKeepsSelectionWhileScrollingIntoHistory();
    /** @brief 10000 行 170 列日志的滚动历史必须保持在十几 MB 以内（原先约 190 MB）。 */
    void historyMemoryStaysCompact();
    /** @brief 字节上限必须在行数上限之前生效，把超长历史裁掉。 */
    void byteLimitTrimsHistoryBeforeLineLimit();
    /** @brief 滚出视口的段必须释放排版缓存，且滚动位置与正文不受影响。 */
    void offscreenLayoutsAreReleased();
    /** @brief 用户有选区时不得重写文档；选区清除后才继续释放。 */
    void selectionPostponesLayoutRelease();
    /** @brief 隐藏的标签不同步文档，但不丢历史、导出拿到最新输出、切回后补齐。 */
    void hiddenViewKeepsHistoryAndCatchesUp();
};

void TerminalMemoryTest::richHistoryRebuildMatchesIncrementalRender()
{
    // 增量渲染的历史段是从屏幕逐格写进去的；整体重建的历史段则直接从紧凑格式写出。两条
    // 路径必须写出完全相同的文档，否则改字号、缩放、退出 vim 之后，历史的颜色或字符就会
    // 悄悄变样。
    VtermBridge incremental;
    VtermBridge rebuilt;
    incremental.resize(8, 90);
    rebuilt.resize(8, 90);
    QTextDocument incrementalDocument;
    QTextDocument rebuiltDocument;

    for (int index = 0; index < 60; ++index) {
        const QByteArray line = richLine(index);
        incremental.feed(line);
        incremental.render(&incrementalDocument);
        rebuilt.feed(line);
    }
    QVERIFY(rebuilt.scrollbackLineCount() > 40);
    QVERIFY2(rebuilt.render(&rebuiltDocument), "首次渲染必然是整体重建");

    const QString difference = firstDocumentDifference(incrementalDocument, rebuiltDocument);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

void TerminalMemoryTest::richLineKeepsSelectionWhileScrollingIntoHistory()
{
    // 增量渲染判断"这一行滚进历史时有没有变"用的是解包后的单元格与屏幕单元格逐字段比较。
    // 解包只要有一个字段对不上（空格子被补成空格、无效色被补成黑色……），这一段就会被
    // 删掉重写，用户选中的文字随之塌缩——表现为"日志一滚动，选区就没了"。
    VtermBridge bridge;
    bridge.resize(6, 90);
    QTextDocument document;
    bridge.feed(richLine(0));
    bridge.feed(richLine(1));
    bridge.render(&document);

    for (const int blockNumber : {0, 1}) {
        QTextCursor selection(&document);
        const QTextBlock block = document.findBlockByNumber(blockNumber);
        selection.setPosition(block.position());
        selection.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
        const QString selectedBefore = selection.selectedText();
        QVERIFY(selectedBefore.contains(QStringLiteral("中文")));

        VtermBridge replay;
        replay.resize(6, 90);
        QTextDocument replayDocument;
        replay.feed(richLine(0));
        replay.feed(richLine(1));
        replay.render(&replayDocument);
        QTextCursor replaySelection(&replayDocument);
        const QTextBlock replayBlock = replayDocument.findBlockByNumber(blockNumber);
        replaySelection.setPosition(replayBlock.position());
        replaySelection.setPosition(replayBlock.position() + replayBlock.length() - 1,
                                    QTextCursor::KeepAnchor);

        for (int filler = 0; filler < 12; ++filler) {
            replay.feed(QByteArray("filler ") + QByteArray::number(filler) + "\r\n");
            QVERIFY2(!replay.render(&replayDocument), "逐行输出必须走增量路径");
            QVERIFY2(replaySelection.hasSelection(),
                     qPrintable(QStringLiteral("第 %1 段滚进历史后选区塌缩").arg(blockNumber)));
            QCOMPARE(replaySelection.selectedText(), selectedBefore);
        }
        QVERIFY(replay.scrollbackLineCount() >= 2);
    }
}

void TerminalMemoryTest::historyMemoryStaysCompact()
{
    if (heapBytesInUse() < 0) {
        QSKIP("当前平台没有可用的堆统计接口");
    }
    VtermBridge bridge;
    bridge.resize(50, 200);
    bridge.setScrollbackLimit(10000);
    bridge.setScrollbackByteLimit(0);

    const qint64 before = heapBytesInUse();
    QByteArray chunk;
    for (int index = 0; index < 10060; ++index) {
        chunk += springLogLine(index);
        if (chunk.size() > 64 * 1024) {
            bridge.feed(chunk);
            chunk.clear();
        }
    }
    bridge.feed(chunk);
    const qint64 used = heapBytesInUse() - before;

    QCOMPARE(bridge.scrollbackLineCount(), 10000);
    // 逐格保存时这里约 190 MB；紧凑格式实测约 6 MB。阈值留足余量，只拦量级回退。
    QVERIFY2(used < 20LL * 1024 * 1024,
             qPrintable(QStringLiteral("10000 行历史占用 %1 MB").arg(used / 1048576.0)));
}

void TerminalMemoryTest::byteLimitTrimsHistoryBeforeLineLimit()
{
    VtermBridge bridge;
    bridge.resize(24, 200);
    bridge.setScrollbackLimit(100000);
    bridge.setScrollbackByteLimit(256 * 1024);
    QByteArray output;
    for (int index = 0; index < 5000; ++index) {
        output += springLogLine(index);
    }
    bridge.feed(output);

    // 每行约 0.6 KB，256 KB 大约容得下四百多行；行数上限远未触顶，裁剪只能来自字节上限。
    QVERIFY2(bridge.scrollbackLineCount() < 1000,
             qPrintable(QString::number(bridge.scrollbackLineCount())));
    QVERIFY2(bridge.scrollbackLineCount() > 100,
             qPrintable(QString::number(bridge.scrollbackLineCount())));

    // 放宽上限不会凭空找回已淘汰的行，但之后的输出按新上限累积。
    const int trimmedCount = bridge.scrollbackLineCount();
    bridge.setScrollbackByteLimit(0);
    QCOMPARE(bridge.scrollbackLineCount(), trimmedCount);
    bridge.feed(output);
    QVERIFY(bridge.scrollbackLineCount() > trimmedCount + 4000);
}

void TerminalMemoryTest::offscreenLayoutsAreReleased()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    streamLogThroughViewport(view, 1500);

    const QString textBefore = view.document()->toPlainText();
    QScrollBar *bar = view.verticalScrollBar();
    QCOMPARE(bar->value(), bar->maximum());

    // 清理按一秒一轮的节拍分批进行，给足几轮的时间。
    QTRY_COMPARE_WITH_TIMEOUT(laidOutBlocksAwayFromViewport(view), 0, 6000);

    // 释放只换掉段的内部数据：正文、段数、滚动位置都必须和之前完全一样。
    QCOMPARE(view.document()->toPlainText(), textBefore);
    QCOMPARE(bar->value(), bar->maximum());

    // 用户滚回历史中部查看，再回到底部：看过的段同样要在滚出视口后释放。
    bar->setValue(bar->maximum() / 2);
    view.viewport()->repaint();
    QCoreApplication::processEvents();
    const int middleBlock = firstVisibleBlockNumber(view);
    bar->setValue(bar->maximum());
    view.viewport()->repaint();
    QCoreApplication::processEvents();
    QTRY_COMPARE_WITH_TIMEOUT(laidOutBlocksAwayFromViewport(view), 0, 6000);
    QVERIFY(middleBlock > view.terminalRows() * 6);
    QCOMPARE(view.document()->toPlainText(), textBefore);

    // 清理必须收敛：空闲的标签不能每秒重写一轮文档、白白耗 CPU。候选区间是保守估计，
    // 可能还有几批"登记过但其实没排版"的段在按节拍处理，先等这些批次做完，之后文档
    // 一次都不应再被改动。
    QTest::qWait(3000);
    const int revisionAfterRelease = view.document()->revision();
    QTest::qWait(2500);
    QCOMPARE(view.document()->revision(), revisionAfterRelease);
}

void TerminalMemoryTest::selectionPostponesLayoutRelease()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    streamLogThroughViewport(view, 600);

    // 选中一段早已滚出视口的历史。重写这段文档会让选区塌缩，所以清理必须整轮让路。
    QTextCursor selection(view.document());
    const QTextBlock block = view.document()->findBlockByNumber(20);
    selection.setPosition(block.position());
    selection.setPosition(block.position() + 30, QTextCursor::KeepAnchor);
    view.setTextCursor(selection);
    const QString selectedText = view.textCursor().selectedText();
    QVERIFY(!selectedText.isEmpty());

    QTest::qWait(2500);
    QVERIFY(view.textCursor().hasSelection());
    QCOMPARE(view.textCursor().selectedText(), selectedText);
    QVERIFY2(laidOutBlocksAwayFromViewport(view) > 0, "有选区时不应重写任何段");

    QTextCursor cleared = view.textCursor();
    cleared.clearSelection();
    view.setTextCursor(cleared);
    QTRY_COMPARE_WITH_TIMEOUT(laidOutBlocksAwayFromViewport(view), 0, 6000);
}

void TerminalMemoryTest::hiddenViewKeepsHistoryAndCatchesUp()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    view.appendTerminalOutput(QByteArrayLiteral("before hide\r\n"));
    QTest::qWait(50);
    view.hide();

    // 后台标签持续刷屏：会话照常消费输出，历史一行都不能少——曾经的做法是给后台标签
    // 另设一个更小的历史上限，切回来时前面的日志已经找不回来了。
    QByteArray output;
    for (int index = 0; index < 3000; ++index) {
        output += springLogLine(index);
    }
    output += "hidden marker\r\n";
    view.appendTerminalOutput(output);
    QTest::qWait(50);

    QVERIFY2(!view.document()->toPlainText().contains(QStringLiteral("hidden marker")),
             "隐藏期间不应同步文档");
    // 导出、日志读取读的是文档：欠下的帧必须先补上，否则拿到的是切走之前的旧内容。
    QVERIFY(view.terminalPlainText().contains(QStringLiteral("hidden marker")));

    view.appendTerminalOutput(QByteArrayLiteral("second marker\r\n"));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY(view.document()->toPlainText().contains(QStringLiteral("second marker")));
    QVERIFY(view.document()->toPlainText().contains(QStringLiteral("before hide")));
    QVERIFY2(view.document()->blockCount() > 3000,
             qPrintable(QString::number(view.document()->blockCount())));
}

QTEST_MAIN(TerminalMemoryTest)

#include "TerminalMemoryTest.moc"
