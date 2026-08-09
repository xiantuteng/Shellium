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

/** @brief 造出一屏以上的普通 Shell 输出，让 libvterm 把旧行推进滚动历史。 */
void fillScrollback(TerminalView &view, const int lines)
{
    for (int index = 0; index < lines; ++index) {
        view.appendTerminalOutput(QByteArray("history line ")
                                  + QByteArray::number(index) + "\r\n");
    }
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
};

void TerminalAltScreenTest::pasteIsBracketedWhenRemoteEnablesIt()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    QGuiApplication::clipboard()->setText(QStringLiteral("indented"));

    // 远端尚未开启括号粘贴：不能凭空多发标记字节。
    QSignalSpy inputSpy(&view, &TerminalView::inputBytes);
    QTest::keyClick(&view, Qt::Key_V, QKeySequence(QKeySequence::Paste)[0].keyboardModifiers());
    QCOMPARE(inputSpy.count(), 1);
    QCOMPARE(inputSpy.at(0).at(0).toByteArray(), QByteArrayLiteral("indented"));

    // vim 进入插入模式前会开启 DECSET 2004；此时粘贴内容必须夹在起止标记之间，否则
    // autoindent 会把每一行再缩进一层。
    view.appendTerminalOutput(QByteArrayLiteral("\x1b[?2004h"));
    inputSpy.clear();
    QTest::keyClick(&view, Qt::Key_V, QKeySequence(QKeySequence::Paste)[0].keyboardModifiers());
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
    const QColor cursorColor(QStringLiteral("#ff00ff"));
    VtermBridge bridge;
    bridge.resize(10, 40);
    bridge.setDefaultColors(QColor(QStringLiteral("#e6e6e6")),
                            QColor(QStringLiteral("#111318")),
                            cursorColor);
    bridge.setCursorPresentation(VtermBridge::CursorShape::Block, true);

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
    QVERIFY2(position >= 0, "备用屏下必须渲染出光标单元格");
    QTextCursor cursor(&document);
    cursor.setPosition(position);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    QCOMPARE(cursor.selectedText(), QStringLiteral("f"));
    QCOMPARE(cursor.charFormat().background().color(), cursorColor);
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
