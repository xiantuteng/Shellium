#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTextBlock>
#include <QTextDocument>
#include <QtTest>

#include "terminal/TerminalView.h"
#include "terminal/VtermBridge.h"

namespace {

/**
 * @brief 终端协议意义上的物理 Control 修饰。
 *
 * Qt 在 macOS 把 Command 映射为 ControlModifier、物理 Control 映射为 MetaModifier，
 * TerminalView 也按这个约定判断，合成按键必须使用同一套修饰否则测的是 Command。
 */
constexpr Qt::KeyboardModifier kTerminalControl =
#if defined(Q_OS_MACOS)
    Qt::MetaModifier;
#else
    Qt::ControlModifier;
#endif

/**
 * @brief 生成一段占满整块网格的备用屏绘制序列，模拟 vim 打开文件。
 * @param rows 终端网格行数。
 * @param cursorRow 绘制结束后光标停留的行（1 起）；vim 打开文件时是第 1 行。
 *
 * 序列结构取自本机 `vim -u NONE -N` 在真实 PTY 上的实际输出：切备用屏、开应用光标键、
 * 清屏、逐行绝对定位绘制、最后定位光标并重新显示光标。
 */
QByteArray fullScreenApplicationFrame(const int rows, const int cursorRow)
{
    QByteArray bytes;
    bytes += "\x1b[?1049h\x1b[?1h\x1b=";
    bytes += "\x1b[H\x1b[2J\x1b[?25l";
    for (int row = 1; row <= rows; ++row) {
        bytes += QByteArray("\x1b[") + QByteArray::number(row) + ";1Hfile row "
            + QByteArray::number(row);
    }
    bytes += QByteArray("\x1b[") + QByteArray::number(cursorRow) + ";1H";
    bytes += "\x1b[?12l\x1b[?25h";
    return bytes;
}

/**
 * @brief 造出一屏以上的普通 Shell 输出，让 libvterm 把旧行推进滚动历史。
 *
 * 末尾必须等一帧：渲染是合帧的，appendTerminalOutput 只把字节喂给 libvterm 并排队一次
 * 渲染，文档与滚动条要到下一帧才更新。
 */
void fillScrollback(TerminalView &view, const int lines)
{
    for (int index = 0; index < lines; ++index) {
        view.appendTerminalOutput(QByteArray("history line ")
                                  + QByteArray::number(index) + "\r\n");
    }
    QTest::qWait(50);
}

/**
 * @brief 在当前平台上按下"粘贴"快捷键。
 *
 * macOS 是 Command+V（Qt 映射为 ControlModifier），其余平台是 Shift+Insert——那里的
 * Ctrl+V 必须留给 Shell 的 literal-next。
 */
void pressPasteShortcut(TerminalView &view)
{
#if defined(Q_OS_MACOS)
    QTest::keyClick(&view, Qt::Key_V, Qt::ControlModifier);
#else
    QTest::keyClick(&view, Qt::Key_Insert, Qt::ShiftModifier);
#endif
}

/** @brief 在当前平台上按下"复制"快捷键；非 macOS 上 Ctrl+C 必须留给 SIGINT。 */
void pressCopyShortcut(TerminalView &view)
{
#if defined(Q_OS_MACOS)
    QTest::keyClick(&view, Qt::Key_C, Qt::ControlModifier);
#else
    QTest::keyClick(&view, Qt::Key_Insert, Qt::ControlModifier);
#endif
}

/** @brief 显示视图并等待首次真实布局，使 terminalRows() 反映实际网格。 */
bool showAndSettle(TerminalView &view)
{
    view.resize(640, 400);
    view.show();
    if (!QTest::qWaitForWindowExposed(&view)) {
        return false;
    }
    QTest::qWait(200);
    return true;
}

} // namespace

class TerminalAltScreenTest : public QObject
{
    Q_OBJECT

private slots:
    void reportedRowsMatchDisplayableRows();
    void cursorStaysVisibleAfterScrollbackThenFullScreenApplication();
    void cursorCellIsPaintedInsideFullScreenApplication();
    void navigationKeysFollowApplicationCursorKeyMode();
    void pageInsertAndFunctionKeysReachRemote();
    void controlWithNonLetterKeysSendControlCodes();
    void fullScreenApplicationDoesNotSubmitShellCommands();
    void wheelScrollsFullScreenApplicationWithArrowKeys();
    void pasteIsBracketedWhenRemoteEnablesIt();
    void selectionSurvivesCursorBlinkRepaint();
    void insertClipboardShortcutsBypassRemoteInsertKey();
    void incrementalRenderMatchesFullRebuild();
    void cursorBlinkLeavesDocumentUntouched();
    void cursorBlinkRepaintsCursorPixels();
    void selectionAndScrollbackBlocksSurviveNewOutput();
};

void TerminalAltScreenTest::incrementalRenderMatchesFullRebuild()
{
    // 增量渲染唯一不能出错的地方，就是它必须和"从头重建一遍"的结果一模一样。这里让两个
    // 桥接层吃下完全相同的字节：一个逐行渲染（走增量），一个最后只渲染一次（走全量）。
    // 行数刻意超过滚动历史上限，把"从文档头部删除被淘汰的段"这条最容易错位的路径也覆盖到。
    VtermBridge incremental;
    VtermBridge oneShot;
    incremental.resize(10, 40);
    oneShot.resize(10, 40);
    incremental.setScrollbackLimit(100);
    oneShot.setScrollbackLimit(100);

    QTextDocument incrementalDocument;
    QTextDocument oneShotDocument;
    bool sawIncrementalUpdate = false;
    for (int index = 0; index < 400; ++index) {
        const QByteArray line = QByteArray("line ") + QByteArray::number(index)
            + " \x1b[31mred\x1b[0m tail\r\n";
        incremental.feed(line);
        if (!incremental.render(&incrementalDocument)) {
            sawIncrementalUpdate = true;
        }
        oneShot.feed(line);
    }
    QVERIFY2(sawIncrementalUpdate, "逐行渲染必须真的走增量，否则这个用例什么都没验证");
    QVERIFY2(oneShot.render(&oneShotDocument), "首次渲染必然是整体重建");

    QCOMPARE(incrementalDocument.blockCount(), oneShotDocument.blockCount());
    QCOMPARE(incrementalDocument.toPlainText(), oneShotDocument.toPlainText());
    QCOMPARE(incremental.renderedCursorPosition(), oneShot.renderedCursorPosition());

    // 颜色也必须一致：增量路径若把段落格式或字符格式漏掉，纯文本比较是发现不了的。
    QTextCursor incrementalCursor(&incrementalDocument);
    QTextCursor oneShotCursor(&oneShotDocument);
    incrementalCursor.setPosition(incrementalDocument.characterCount() / 2);
    oneShotCursor.setPosition(oneShotDocument.characterCount() / 2);
    QCOMPARE(incrementalCursor.charFormat().foreground().color(),
             oneShotCursor.charFormat().foreground().color());
}

void TerminalAltScreenTest::cursorBlinkLeavesDocumentUntouched()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    fillScrollback(view, 200);

    // 输出让滚动条出现，viewport 随之变窄并触发一次防抖后的网格重算；等它安定下来再采样，
    // 否则测到的是这次一次性重排，而不是闪烁本身。
    QTest::qWait(300);

    // QTextDocument::revision() 每次编辑都会自增。闪烁若还在重建文档，它必然一路上涨——
    // 一万行历史时那正是每 600ms 半秒的开销，界面等于长期满负荷，用户什么都不做也卡。
    const int revisionBeforeBlink = view.document()->revision();
    QTest::qWait(1500);
    QCOMPARE(view.document()->revision(), revisionBeforeBlink);
}

void TerminalAltScreenTest::cursorBlinkRepaintsCursorPixels()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    view.appendTerminalOutput(QByteArrayLiteral("[root@server ~]# "));
    QTest::qWait(300);

    // 光标现在完全由 paintEvent 叠加绘制。文档不再变化，闪烁若也没有真的重绘像素，光标
    // 就彻底不见了——这一条正是"不许碰文档"改动最容易掉进去的坑。
    const QImage firstPhase = view.viewport()->grab().toImage();
    bool sawDifferentPhase = false;
    for (int attempt = 0; attempt < 30 && !sawDifferentPhase; ++attempt) {
        QTest::qWait(50);
        sawDifferentPhase = view.viewport()->grab().toImage() != firstPhase;
    }
    QVERIFY2(sawDifferentPhase, "光标必须仍在闪，即画面在两个相位之间发生像素变化");
}

void TerminalAltScreenTest::selectionAndScrollbackBlocksSurviveNewOutput()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    view.appendTerminalOutput(QByteArrayLiteral("keep this line\r\n"));
    QTest::qWait(50);

    QTextCursor selection = view.textCursor();
    selection.movePosition(QTextCursor::Start);
    selection.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 14);
    view.setTextCursor(selection);
    QCOMPARE(view.textCursor().selectedText(), QStringLiteral("keep this line"));

    // 滚动历史里的段是只读的，新输出只会在尾部追加。用户一边看输出一边选文本时，选区
    // 必须一直在——旧实现每来一个网络分片就重建整份文档，选区活不过一次输出。
    for (int index = 0; index < 50; ++index) {
        view.appendTerminalOutput(QByteArray("noise ") + QByteArray::number(index) + "\r\n");
    }
    QTest::qWait(80);
    QCOMPARE(view.textCursor().selectedText(), QStringLiteral("keep this line"));
}

void TerminalAltScreenTest::selectionSurvivesCursorBlinkRepaint()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    view.appendTerminalOutput(QByteArrayLiteral("copy me\r\n"));
    QTest::qWait(50);

    QTextCursor selection = view.textCursor();
    selection.movePosition(QTextCursor::Start);
    selection.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 7);
    view.setTextCursor(selection);
    QCOMPARE(view.textCursor().selectedText(), QStringLiteral("copy me"));

    // 光标每 600ms 闪烁一次，而每次闪烁都会整体重建文档。选区若不跨渲染保留，用户刚
    // 选中的文本活不过一个闪烁周期，复制键拿到的只会是空字符串。
    QTest::qWait(1500);
    QCOMPARE(view.textCursor().selectedText(), QStringLiteral("copy me"));

    QGuiApplication::clipboard()->setText(QStringLiteral("stale clipboard"));
    QSignalSpy inputSpy(&view, &TerminalView::inputBytes);
    pressCopyShortcut(view);
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("copy me"));
    // 复制不是终端输入，一个字节都不该发给远端。
    QCOMPARE(inputSpy.count(), 0);
}

void TerminalAltScreenTest::insertClipboardShortcutsBypassRemoteInsertKey()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    view.appendTerminalOutput(QByteArrayLiteral("copy me\r\n"));
    QTest::qWait(50);

    QTextCursor selection = view.textCursor();
    selection.movePosition(QTextCursor::Start);
    selection.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 7);
    view.setTextCursor(selection);
    QGuiApplication::clipboard()->setText(QStringLiteral("stale clipboard"));

    // Insert 本身是导航键，会被 libvterm 编码成 CSI 2~。带 Ctrl/Shift 的两个组合必须在
    // 那之前被认成复制/粘贴，否则在 Windows 上这两个键按下去只会给远端发一个 Insert。
    QSignalSpy inputSpy(&view, &TerminalView::inputBytes);
    QTest::keyClick(&view, Qt::Key_Insert, Qt::ControlModifier);
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("copy me"));
    QCOMPARE(inputSpy.count(), 0);

    QGuiApplication::clipboard()->setText(QStringLiteral("pasted"));
    QTest::keyClick(&view, Qt::Key_Insert, Qt::ShiftModifier);
    QCOMPARE(inputSpy.count(), 1);
    QCOMPARE(inputSpy.at(0).at(0).toByteArray(), QByteArrayLiteral("pasted"));

    // 不带修饰的 Insert 仍然属于远端：全屏编辑器靠它切换插入/改写模式。
    inputSpy.clear();
    QTest::keyClick(&view, Qt::Key_Insert);
    QCOMPARE(inputSpy.count(), 1);
    QCOMPARE(inputSpy.at(0).at(0).toByteArray(), QByteArrayLiteral("\x1b[2~"));
}

void TerminalAltScreenTest::pasteIsBracketedWhenRemoteEnablesIt()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    QGuiApplication::clipboard()->setText(QStringLiteral("indented"));

    // 远端尚未开启括号粘贴：不能凭空多发标记字节。
    QSignalSpy inputSpy(&view, &TerminalView::inputBytes);
    pressPasteShortcut(view);
    QCOMPARE(inputSpy.count(), 1);
    QCOMPARE(inputSpy.at(0).at(0).toByteArray(), QByteArrayLiteral("indented"));

    // vim 进入插入模式前会开启 DECSET 2004；此时粘贴内容必须夹在起止标记之间，否则
    // autoindent 会把每一行再缩进一层。
    view.appendTerminalOutput(QByteArrayLiteral("\x1b[?2004h"));
    inputSpy.clear();
    pressPasteShortcut(view);
    QCOMPARE(inputSpy.count(), 3);
    QCOMPARE(inputSpy.at(0).at(0).toByteArray(), QByteArrayLiteral("\x1b[200~"));
    QCOMPARE(inputSpy.at(1).at(0).toByteArray(), QByteArrayLiteral("indented"));
    QCOMPARE(inputSpy.at(2).at(0).toByteArray(), QByteArrayLiteral("\x1b[201~"));
}

void TerminalAltScreenTest::reportedRowsMatchDisplayableRows()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    fillScrollback(view, view.terminalRows() * 3);

    // 告诉 PTY 的行数必须等于控件真正能完整显示的段数。多报一行在主屏只是顶掉最旧一行，
    // 但备用屏下整块画布会被推出视口顶部，光标随之消失。
    for (int height = 380; height <= 460; height += 8) {
        view.resize(640, height);
        QTest::qWait(160);
        const int displayableRows = view.document()->blockCount()
            - view.verticalScrollBar()->maximum();
        QCOMPARE(view.terminalRows(), displayableRows);
    }
}

void TerminalAltScreenTest::cursorStaysVisibleAfterScrollbackThenFullScreenApplication()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));

    const int rows = view.terminalRows();
    fillScrollback(view, rows * 3);
    QVERIFY(view.verticalScrollBar()->maximum() > 0);

    view.appendTerminalOutput(fullScreenApplicationFrame(rows, 1));
    QTest::qWait(50);

    // 备用屏的文档就是整块网格，滚动历史不参与渲染，第 0 行必须落在视口顶部。
    QCOMPARE(view.document()->blockCount(), rows);
    QCOMPARE(view.verticalScrollBar()->value(), 0);
    QCOMPARE(view.document()->begin().text(), QStringLiteral("file row 1"));

    // 光标停在网格第一行；它必须真的落在可视区域内，而不只是存在于文档里。
    QTextCursor cursor(view.document());
    cursor.movePosition(QTextCursor::Start);
    const QRect cursorArea = view.cursorRect(cursor);
    QVERIFY2(view.viewport()->rect().contains(cursorArea.topLeft()),
             qPrintable(QStringLiteral("光标 %1 落在视口 %2 之外")
                            .arg(QDebug::toString(cursorArea),
                                 QDebug::toString(view.viewport()->rect()))));
}

void TerminalAltScreenTest::cursorCellIsPaintedInsideFullScreenApplication()
{
    const QColor cellBackground(QStringLiteral("#111318"));
    VtermBridge bridge;
    bridge.resize(10, 40);
    bridge.setDefaultColors(QColor(QStringLiteral("#e6e6e6")), cellBackground);

    // 先让普通输出把旧行推进滚动历史：真实会话进 vim 时历史一定是非空的，而备用屏一旦
    // 把历史一起渲染，网格就会整体沉到文档底部。
    for (int index = 0; index < 30; ++index) {
        bridge.feed(QByteArray("history line ") + QByteArray::number(index) + "\r\n");
    }
    bridge.feed(fullScreenApplicationFrame(10, 3));
    QVERIFY(bridge.isAlternateScreen());

    QTextDocument document;
    bridge.render(&document);

    // 备用屏必须渲染满整块网格：少一行就会让画布整体上移一行。
    QCOMPARE(document.blockCount(), 10);

    const int position = bridge.renderedCursorPosition();
    QVERIFY2(position >= 0, "备用屏下必须定位出光标单元格");
    QTextCursor cursor(&document);
    cursor.setPosition(position);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    QCOMPARE(cursor.selectedText(), QStringLiteral("f"));

    // 光标不能写进字符格式：一旦写进去，每次闪烁都要重建整份文档，一万行历史时单次重建
    // 就要几百毫秒。它现在只提供定位信息，由 TerminalView 叠加绘制。
    QCOMPARE(cursor.charFormat().background().color(), cellBackground);
    QCOMPARE(bridge.renderedCursorText(), QStringLiteral("f"));
    QCOMPARE(bridge.renderedCursorBackground(), cellBackground);
}

void TerminalAltScreenTest::navigationKeysFollowApplicationCursorKeyMode()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    QSignalSpy inputSpy(&view, &TerminalView::inputBytes);

    // 主屏没有开启 DECCKM，方向键仍是普通 CSI 形式。
    QTest::keyClick(&view, Qt::Key_Up);
    QTest::keyClick(&view, Qt::Key_Home);
    QCOMPARE(inputSpy.count(), 2);
    QCOMPARE(inputSpy.at(0).at(0).toByteArray(), QByteArrayLiteral("\x1b[A"));
    QCOMPARE(inputSpy.at(1).at(0).toByteArray(), QByteArrayLiteral("\x1b[H"));

    // vim 进入全屏时开启 DECCKM，方向键必须切换到 SS3 形式。硬编码 CSI 会让 vim 在插入
    // 模式下把上箭头读成 ESC 加字母 A，即退出插入模式并追加到行尾。
    inputSpy.clear();
    view.appendTerminalOutput(fullScreenApplicationFrame(view.terminalRows(), 1));
    inputSpy.clear();
    QTest::keyClick(&view, Qt::Key_Up);
    QTest::keyClick(&view, Qt::Key_Down);
    QTest::keyClick(&view, Qt::Key_Left);
    QTest::keyClick(&view, Qt::Key_Right);
    QTest::keyClick(&view, Qt::Key_Home);
    QTest::keyClick(&view, Qt::Key_End);
    QCOMPARE(inputSpy.count(), 6);
    QCOMPARE(inputSpy.at(0).at(0).toByteArray(), QByteArrayLiteral("\x1bOA"));
    QCOMPARE(inputSpy.at(1).at(0).toByteArray(), QByteArrayLiteral("\x1bOB"));
    QCOMPARE(inputSpy.at(2).at(0).toByteArray(), QByteArrayLiteral("\x1bOD"));
    QCOMPARE(inputSpy.at(3).at(0).toByteArray(), QByteArrayLiteral("\x1bOC"));
    QCOMPARE(inputSpy.at(4).at(0).toByteArray(), QByteArrayLiteral("\x1bOH"));
    QCOMPARE(inputSpy.at(5).at(0).toByteArray(), QByteArrayLiteral("\x1bOF"));
}

void TerminalAltScreenTest::pageInsertAndFunctionKeysReachRemote()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    QSignalSpy inputSpy(&view, &TerminalView::inputBytes);

    // 这几个键此前没有任何分支处理，会被交回 QPlainTextEdit——按下去只滚动本地文档，
    // 远端程序收不到任何字节。
    QTest::keyClick(&view, Qt::Key_PageUp);
    QTest::keyClick(&view, Qt::Key_PageDown);
    QTest::keyClick(&view, Qt::Key_Insert);
    QTest::keyClick(&view, Qt::Key_Delete);
    QTest::keyClick(&view, Qt::Key_F1);
    QTest::keyClick(&view, Qt::Key_F5);
    QCOMPARE(inputSpy.count(), 6);
    QCOMPARE(inputSpy.at(0).at(0).toByteArray(), QByteArrayLiteral("\x1b[5~"));
    QCOMPARE(inputSpy.at(1).at(0).toByteArray(), QByteArrayLiteral("\x1b[6~"));
    QCOMPARE(inputSpy.at(2).at(0).toByteArray(), QByteArrayLiteral("\x1b[2~"));
    QCOMPARE(inputSpy.at(3).at(0).toByteArray(), QByteArrayLiteral("\x1b[3~"));
    QCOMPARE(inputSpy.at(4).at(0).toByteArray(), QByteArrayLiteral("\x1bOP"));
    QCOMPARE(inputSpy.at(5).at(0).toByteArray(), QByteArrayLiteral("\x1b[15~"));
}

void TerminalAltScreenTest::controlWithNonLetterKeysSendControlCodes()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    QSignalSpy inputSpy(&view, &TerminalView::inputBytes);

    // Ctrl+[ 就是 ESC，是 vim 用户退出插入模式最常用的按法之一。只处理 A~Z 会让它被
    // 整个吞掉，用户按下去毫无反应，只能改用 Esc 键。
    QTest::keyClick(&view, Qt::Key_BracketLeft, kTerminalControl);
    QTest::keyClick(&view, Qt::Key_Backslash, kTerminalControl);
    QCOMPARE(inputSpy.count(), 2);
    QCOMPARE(inputSpy.at(0).at(0).toByteArray(), QByteArray(1, '\x1b'));
    QCOMPARE(inputSpy.at(1).at(0).toByteArray(), QByteArray(1, '\x1c'));
}

void TerminalAltScreenTest::fullScreenApplicationDoesNotSubmitShellCommands()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    view.appendTerminalOutput(fullScreenApplicationFrame(view.terminalRows(), 1));
    QVERIFY(view.terminalRows() > 0);

    QSignalSpy commandSpy(&view, &TerminalView::commandSubmitted);
    QSignalSpy abortSpy(&view, &TerminalView::nestedSshAborted);
    QSignalSpy inputSpy(&view, &TerminalView::inputBytes);

    // vim 里敲的是编辑命令，不是 Shell 命令行：回车只是换行，Ctrl-D 是翻半页，
    // Ctrl+C 中断的是当前编辑动作。把它们上报会污染命令历史，还会凭空弹掉一层服务器。
    QTest::keyClicks(&view, QStringLiteral("iso"));
    QTest::keyClick(&view, Qt::Key_Return);
    QTest::keyClick(&view, Qt::Key_D, kTerminalControl);
    QTest::keyClick(&view, Qt::Key_C, kTerminalControl);

    QCOMPARE(commandSpy.count(), 0);
    QCOMPARE(abortSpy.count(), 0);
    // 字节本身仍须原样送达远端。
    QCOMPARE(inputSpy.count(), 6);
    QCOMPARE(inputSpy.at(3).at(0).toByteArray(), QByteArrayLiteral("\r"));
    QCOMPARE(inputSpy.at(4).at(0).toByteArray(), QByteArray(1, '\x04'));
    QCOMPARE(inputSpy.at(5).at(0).toByteArray(), QByteArray(1, '\x03'));
}

void TerminalAltScreenTest::wheelScrollsFullScreenApplicationWithArrowKeys()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    view.appendTerminalOutput(fullScreenApplicationFrame(view.terminalRows(), 1));

    QSignalSpy inputSpy(&view, &TerminalView::inputBytes);
    QWheelEvent wheelUp(QPointF(10, 10),
                        view.mapToGlobal(QPoint(10, 10)),
                        QPoint(0, 120),
                        QPoint(0, 120),
                        Qt::NoButton,
                        Qt::NoModifier,
                        Qt::NoScrollPhase,
                        false);
    QApplication::sendEvent(view.viewport(), &wheelUp);

    // 全屏程序的历史在它自己手里，本地文档只有一屏，滚轮必须翻译成方向键才滚得动。
    QCOMPARE(inputSpy.count(), 3);
    for (int index = 0; index < 3; ++index) {
        QCOMPARE(inputSpy.at(index).at(0).toByteArray(), QByteArrayLiteral("\x1bOA"));
    }
}

QTEST_MAIN(TerminalAltScreenTest)
#include "TerminalAltScreenTest.moc"
