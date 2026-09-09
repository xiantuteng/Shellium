#include <QAbstractTextDocumentLayout>
#include <QAbstractItemView>
#include <QClipboard>
#include <QCompleter>
#include <QFontMetrics>
#include <QFontDatabase>
#include <QGlyphRun>
#include <QRawFont>
#include <QTextLayout>
#include <QImage>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextDocument>
#include <QtTest>

#include <algorithm>

#include "settings/TerminalSettings.h"
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
    /** @brief 验证终端中文使用系统界面中文字形，同时保留等宽英文字体。 */
    void chineseFontMatchesSystem();
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
    /** @brief 行高锚点在普通与选中状态下都不得暴露真实“中”字或进入剪贴板。 */
    void selectedLineHeightAnchorsRemainInvisible();
    /** @brief 选区停在锚点之间时，复制出去的内容里也不能残留任何锚点字符。 */
    void partialSelectionDropsLineHeightAnchors();
    void insertClipboardShortcutsBypassRemoteInsertKey();
    void incrementalRenderMatchesFullRebuild();
    void cursorBlinkLeavesDocumentUntouched();
    void cursorBlinkRepaintsCursorPixels();
    void selectionAndScrollbackBlocksSurviveNewOutput();
    void lastRowStaysVisibleInsideFullScreenApplication();
    void scrollReturnsToBottomAfterLeavingFullScreenApplication();
    /** @brief 上翻到顶部后，任何用户输入都必须把视图拉回输出末尾。 */
    void typingWhileScrolledUpJumpsBackToOutputTail();
    void columnCountStaysReportedAfterScrollbarTogglesOnAltScreenEntry();
    void realCapturedVimByteStreamShowsStatusLine();
    void realChunkedSshByteStreamShowsStatusLine();
    void realChunkedSshByteStreamVisibleInTerminalViewWidget();
    void everyRenderedLineHasIdenticalHeightRegardlessOfContent();
    void gridFillsViewportWithoutWastedRows();
    void themeBackgroundSurvivesLeavingFullScreenApplication();
    /** @brief 主题自带的 16 色表必须真正替换 libvterm 的 VGA 默认值，并随主题切换刷新。 */
    void themeAnsiPaletteReplacesVgaDefaults();
    /** @brief 自动提示必须默认关闭；开启后的弹层字体、行高和位置要符合终端视觉约束。 */
    void commandCompletionHonorsSwitchAndUsesCompactTerminalLayout();
};

/**
 * @brief 比较实际排版得到的字体，避免只检查配置而漏掉 Qt 隐式选择其他中文字体的问题。
 * 无参数、无返回值；字体不一致时由 QtTest 输出期望与实际字体族，便于定位平台差异。
 */
void TerminalAltScreenTest::chineseFontMatchesSystem()
{
    // 使用真实汉字解析系统界面的后备字体；系统主字体通常只包含拉丁字形。
    const auto resolvedFamily = [](const QFont &font, const QString &text) {
        // 参数分别为待测字体与样本文字；返回实际字形族，排版失败则返回空串使断言失败。
        QTextLayout layout(text, font);
        layout.beginLayout();
        layout.createLine();
        layout.endLayout();
        const auto runs = layout.glyphRuns();
        return runs.isEmpty() ? QString() : runs.first().rawFont().familyName();
    };
    const QString systemChinese = resolvedFamily(
        QFontDatabase::systemFont(QFontDatabase::GeneralFont), QStringLiteral("中文连接"));
    QVERIFY(!systemChinese.isEmpty());
    TerminalView view;
    TerminalSettings settings;
    settings.fontFamily = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    view.applySettings(settings);
    QCOMPARE(resolvedFamily(view.font(), QStringLiteral("中文连接")), systemChinese);
    // 显式后备链是此修复的保证：即使当前机器恰好默认选对字体，也不能退回隐式匹配。
    QVERIFY(view.font().families().contains(systemChinese));
    QCOMPARE(resolvedFamily(view.font(), QStringLiteral("ABC123")),
             resolvedFamily(QFont(settings.fontFamily), QStringLiteral("ABC123")));
}

void TerminalAltScreenTest::commandCompletionHonorsSwitchAndUsesCompactTerminalLayout()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    view.appendTerminalOutput(QByteArrayLiteral("[root@server ~]# "));
    QTest::qWait(50);

    QCompleter *completer = view.findChild<QCompleter *>();
    QVERIFY(completer != nullptr);
    QVERIFY(completer->popup() != nullptr);
    view.setCommandCompletions({QStringLiteral("git status"),
                                QStringLiteral("git log --oneline"),
                                QStringLiteral("git diff")});

    // 默认设置下即使已有历史候选且输入满足两个字符，也不能自动打断用户或抢占方向键。
    TerminalSettings settings;
    QVERIFY(!settings.commandAutoCompletion);
    view.applySettings(settings);
    QTest::keyClicks(&view, QStringLiteral("gi"));
    QCoreApplication::processEvents();
    QVERIFY(!completer->popup()->isVisible());

    settings.commandAutoCompletion = true;
    view.applySettings(settings);
    QTRY_VERIFY(completer->popup()->isVisible());

    // 弹层是独立顶层窗口，若不显式同步字体就会退回应用 UI 字体。完整 QFont 相等同时锁住
    // 字体族、字号和用户配置的字符间距，防止后续只同步 family 而漏掉其它度量。
    QCOMPARE(completer->popup()->font(), view.font());

    // 主窗口对普通列表设置了较大的最小行高；命令提示必须局部覆盖它。这里只允许在终端字形
    // 高度之外增加少量内边距，从行为层面验证候选列表确实采用紧凑布局。
    const int candidateRowHeight = completer->popup()->sizeHintForRow(0);
    QVERIFY(candidateRowHeight > 0);
    QVERIFY2(candidateRowHeight <= QFontMetrics(view.font()).height() + 6,
             qPrintable(QStringLiteral("命令提示行高 %1px，终端字体高度 %2px，列表仍过于稀疏")
                            .arg(candidateRowHeight)
                            .arg(QFontMetrics(view.font()).height())));

    // 当前命令位于首行，窗口下方空间充足，QCompleter 应选择向下弹出。弹层顶边必须越过完整
    // 终端行而非只越过细窄插入符，否则下边缘较低的字体仍会被弹层盖住。
    QTextCursor documentCursor(view.document());
    documentCursor.movePosition(QTextCursor::End);
    const QRect caretRect = view.cursorRect(documentCursor);
    const QPoint caretTopLeft = view.viewport()->mapToGlobal(caretRect.topLeft());
    const int terminalLineBottom = caretTopLeft.y()
        + qMax(caretRect.height(), QFontMetrics(view.font()).lineSpacing()) - 1;
    QVERIFY2(completer->popup()->geometry().top() > terminalLineBottom,
             qPrintable(QStringLiteral("命令提示顶边 %1 未越过当前终端行底边 %2")
                            .arg(completer->popup()->geometry().top())
                            .arg(terminalLineBottom)));

    settings.commandAutoCompletion = false;
    view.applySettings(settings);
    QVERIFY(!completer->popup()->isVisible());
}

void TerminalAltScreenTest::themeBackgroundSurvivesLeavingFullScreenApplication()
{
    // 主题背景刻意不等于 VtermBridge 构造期的兜底色：两者相等时这条用例什么都测不到，
    // 而真实应用里正是"构造时先用兜底色、applySettings() 之后才换成主题色"这个顺序。
    const QColor themeBackground(QStringLiteral("#1f1f28"));
    const QColor themeForeground(QStringLiteral("#dcd7ba"));

    VtermBridge bridge;
    bridge.resize(10, 40);
    bridge.setDefaultColors(themeForeground, themeBackground);

    const auto backgroundOfLastLine = [&bridge]() {
        QTextDocument document;
        bridge.render(&document);
        return document.lastBlock().begin().fragment().charFormat().background().color();
    };

    bridge.feed(QByteArrayLiteral("before-vi"));
    QCOMPARE(backgroundOfLastLine(), themeBackground);

    // vim 进出备用屏用的 CSI ?1049h/l 内含 DECSC/DECRC：libvterm 会把当前画笔存档再回灌。
    // 画笔里存的是复位那一刻按当时默认色解析好的 RGB，改默认色并不会回头修它，回灌之后
    // 那份兜底色就被当成"显式指定的背景色"，退出 vim 之后新打印的每一行都会顶着一块比
    // 终端背景更黑、且与用户所选主题对不上的底色。
    bridge.feed(QByteArrayLiteral("\x1b[?1049h\x1b[H\x1b[2Jfile line\x1b[?1049l"));
    QVERIFY(!bridge.isAlternateScreen());
    bridge.feed(QByteArrayLiteral("\r\n[root@tidb01 ~]# after-vi"));
    QCOMPARE(backgroundOfLastLine(), themeBackground);

    // 裸的 DECSC/DECRC 走的是同一条回灌路径，任何用它保存光标的程序都会踩到。
    bridge.feed(QByteArrayLiteral("\r\n\x1b" "7" "\x1b" "8" "after-decrc"));
    QCOMPARE(backgroundOfLastLine(), themeBackground);

    // 远端显式指定的颜色必须原样保留：修复只允许在画笔当前就是默认色时重新取副本，
    // 否则改一次主题就会把远端正在使用的颜色一起抹掉。
    bridge.feed(QByteArrayLiteral("\r\n\x1b[41mexplicit-red-bg"));
    const QColor explicitRed = backgroundOfLastLine();
    QVERIFY2(explicitRed != themeBackground, "显式 ANSI 背景色不应回落成主题背景");
    bridge.setDefaultColors(themeForeground, QColor(QStringLiteral("#101820")));
    bridge.feed(QByteArrayLiteral("still-red"));
    QCOMPARE(backgroundOfLastLine(), explicitRed);
}

void TerminalAltScreenTest::themeAnsiPaletteReplacesVgaDefaults()
{
    // `ls` 给目录名用的正是 SGR 34（ANSI 蓝）。libvterm 内置的是 VGA 那套值 #0000e0，它在
    // 任何深色主题上都暗到读不出来，而且完全不随用户所选主题变化——只改默认前景背景色的
    // 主题实现修不掉这个问题。
    const QColor vgaBlue(0, 0, 224);

    VtermBridge bridge;
    bridge.resize(10, 40);
    bridge.setDefaultColors(QColor(QStringLiteral("#dcd7ba")),
                            QColor(QStringLiteral("#1f1f28")));

    const auto foregroundOfLastLine = [&bridge]() {
        QTextDocument document;
        bridge.render(&document);
        return document.lastBlock().begin().fragment().charFormat().foreground().color();
    };

    bridge.feed(QByteArrayLiteral("\x1b[34mmysqldata\x1b[0m"));
    QCOMPARE(foregroundOfLastLine(), vgaBlue);

    TerminalSettings wave;
    wave.colorSchemeId = QStringLiteral("kanagawa_wave");
    bridge.setAnsiPalette(wave.effectivePalette().ansi);
    QCOMPARE(foregroundOfLastLine(), QColor(QStringLiteral("#7e9cd8")));

    // 换主题时已经渲染出来的行必须一起跟着变，否则同一屏里会并存两套配色。
    TerminalSettings flexoki;
    flexoki.colorSchemeId = QStringLiteral("flexoki_dark");
    bridge.setAnsiPalette(flexoki.effectivePalette().ansi);
    QCOMPARE(foregroundOfLastLine(), QColor(QStringLiteral("#4385be")));

    // 不完整或含无效色的调色板整体忽略：只换一半会把前景和背景刷成同一个颜色。
    bridge.setAnsiPalette({QColor(QStringLiteral("#ff0000"))});
    bridge.setAnsiPalette(QVector<QColor>(16, QColor()));
    // 被忽略的调色板不会让文档失效，而 render() 默认走增量、需要文档里已有上一次的内容。
    // 这里显式整体重建，比较的才是 libvterm 当前调色板解析出来的颜色。
    bridge.invalidateDocument();
    QCOMPARE(foregroundOfLastLine(), QColor(QStringLiteral("#4385be")));

    // 端到端：主题选择要真的经由 TerminalView 传到桥接层，而不是只存在设置结构里。
    TerminalView view;
    TerminalSettings settings;
    settings.colorSchemeId = QStringLiteral("kanagawa_wave");
    view.applySettings(settings);
    view.appendTerminalOutput(QByteArrayLiteral("\x1b[34mmysqldata\x1b[0m"));
    QTest::qWait(50);
    QCOMPARE(view.document()->lastBlock().begin().fragment().charFormat().foreground().color(),
             QColor(QStringLiteral("#7e9cd8")));

    // Linux 的 `ls --color` 通常用 `01;34` 呈现目录名。单独覆盖这条带粗体的
    // 实际序列，可以防止测试只验证 SGR 34，却漏掉用户真正看到的着色路径。
    TerminalView termiusView;
    settings.colorSchemeId = QStringLiteral("termius_dark");
    termiusView.applySettings(settings);
    termiusView.appendTerminalOutput(QByteArrayLiteral("\x1b[01;34mlog\x1b[0m"));
    QTest::qWait(50);
    QCOMPARE(termiusView.document()->lastBlock().begin().fragment().charFormat()
                 .foreground().color(),
             QColor(QStringLiteral("#015fff")));

    // 用户的 `ll` 实际通过 xterm 256 色 27 号（`38;5;27`）输出目录名，
    // 该索引不在主题的 ANSI 0-15 号表内。这条回归测试保证 #015fff
    // 覆盖到实际路径，而不是再次只修改不会被远端命中的 4/12 号色槽。
    TerminalView indexedColorView;
    indexedColorView.applySettings(settings);
    indexedColorView.appendTerminalOutput(
        QByteArrayLiteral("\x1b[38;5;27mindexed-log\x1b[0m"));
    QTest::qWait(50);
    QCOMPARE(indexedColorView.document()->lastBlock().begin().fragment().charFormat()
                 .foreground().color(),
             QColor(QStringLiteral("#015fff")));

    // 部分 LS_COLORS 配置会把同一个 `#005fff` 写成 TrueColor；必须与 256 色
    // 分支得到一致结果，否则服务器换一种等价的转义写法后问题会再现。
    TerminalView trueColorView;
    trueColorView.applySettings(settings);
    trueColorView.appendTerminalOutput(
        QByteArrayLiteral("\x1b[38;2;0;95;255mtruecolor-log\x1b[0m"));
    QTest::qWait(50);
    QCOMPARE(trueColorView.document()->lastBlock().begin().fragment().charFormat()
                 .foreground().color(),
             QColor(QStringLiteral("#015fff")));
}

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

void TerminalAltScreenTest::selectedLineHeightAnchorsRemainInvisible()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    // 最后把终端光标移回首行，避免 libvterm 为末行光标额外保留一个空格干扰复制断言。
    view.appendTerminalOutput(
        QByteArrayLiteral("plain ascii line\r\nsecond line\x1b[1;1H"));
    QTest::qWait(100);

    const QString anchorMarker = VtermBridge::lineAnchorSuffix();
    QVERIFY(!anchorMarker.isEmpty());
    const int anchorPosition = view.document()->toPlainText().indexOf(QChar(0x4E2D));
    QVERIFY2(anchorPosition >= 0, "测试内容没有生成 CJK 行高锚点");
    QTextCursor anchorProbe(view.document());
    anchorProbe.setPosition(anchorPosition);
    anchorProbe.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
    QVERIFY2(VtermBridge::isLineAnchorFormat(anchorProbe.charFormat()),
             "段尾“中”字没有内部锚点标记，视图无法与用户真实内容安全区分");

    // 全选会走与截图相同的选区绘制路径。抓取真实 viewport 后检查整个锚点字符格必须保持
    // 纯选区背景色；只要“中”的任一笔画显形，该区域就会出现 HighlightedText 色像素。
    view.setFocus();
    view.selectAll();
    const QPixmap selectedFrame = view.viewport()->grab();
    QVERIFY(!selectedFrame.isNull());
    QTextCursor anchorStart(view.document());
    anchorStart.setPosition(anchorPosition);
    QTextCursor anchorEnd(view.document());
    anchorEnd.setPosition(anchorPosition + 1);
    const QRect startCaret = view.cursorRect(anchorStart);
    const QRect endCaret = view.cursorRect(anchorEnd);
    const QRect anchorArea(std::min(startCaret.left(), endCaret.left()),
                           startCaret.top(),
                           std::abs(endCaret.left() - startCaret.left()),
                           startCaret.height());
    QVERIFY2(anchorArea.width() > 2 && anchorArea.height() > 2,
             "无法取得可检查的行高锚点字形区域");

    const QImage selectedImage = selectedFrame.toImage().convertToFormat(QImage::Format_ARGB32);
    const qreal pixelRatio = selectedFrame.devicePixelRatio();
    const QColor expectedBackground = view.palette().color(
        view.hasFocus() ? QPalette::Active : QPalette::Inactive,
        QPalette::Highlight);
    const QRect pixelArea(qFloor(anchorArea.left() * pixelRatio) + 1,
                          qFloor(anchorArea.top() * pixelRatio) + 1,
                          qCeil(anchorArea.width() * pixelRatio) - 2,
                          qCeil(anchorArea.height() * pixelRatio) - 2);
    for (int y = pixelArea.top(); y <= pixelArea.bottom(); ++y) {
        for (int x = pixelArea.left(); x <= pixelArea.right(); ++x) {
            QCOMPARE(QColor::fromRgba(selectedImage.pixel(x, y)), expectedBackground);
        }
    }

    // 绘制层修复不能改变既有复制契约，内部锚点仍需在进入剪贴板前被完整剥除。
    QGuiApplication::clipboard()->setText(QStringLiteral("stale clipboard"));
    pressCopyShortcut(view);
    const QString copied = QGuiApplication::clipboard()->text();
    QVERIFY(!copied.contains(anchorMarker));
    QCOMPARE(copied, QStringLiteral("plain ascii line\nsecond line"));
}

void TerminalAltScreenTest::partialSelectionDropsLineHeightAnchors()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    view.appendTerminalOutput(
        QByteArrayLiteral("plain ascii line\r\nsecond line\x1b[1;1H"));
    QTest::qWait(100);

    // 找到首段的第一个锚点字符。锚点具体有几个由本机字体探测决定，用例不能假定个数——
    // 正是那个假定（写死“中”+U+200B 的后缀）让只有一个锚点时“中”被复制了出去。
    const QTextBlock firstBlock = view.document()->firstBlock();
    int anchorOffset = -1;
    for (QTextBlock::iterator part = firstBlock.begin(); !part.atEnd(); ++part) {
        const QTextFragment fragment = part.fragment();
        if (fragment.isValid() && VtermBridge::isLineAnchorFormat(fragment.charFormat())) {
            anchorOffset = fragment.position();
            break;
        }
    }
    QVERIFY2(anchorOffset >= 0, "测试内容没有生成行高锚点");

    // 选到第一个锚点之后就停下：用户拖选到行尾时选区常常正好落在锚点之间，此时行尾与
    // 任何写死的锚点后缀都对不上。
    QTextCursor selection(view.document());
    selection.setPosition(firstBlock.position());
    selection.setPosition(anchorOffset + 1, QTextCursor::KeepAnchor);
    view.setTextCursor(selection);

    QGuiApplication::clipboard()->setText(QStringLiteral("stale clipboard"));
    pressCopyShortcut(view);
    const QString copied = QGuiApplication::clipboard()->text();
    QVERIFY2(!copied.contains(QChar(0x4E2D)),
             qPrintable(QStringLiteral("锚点“中”进入了剪贴板：[%1]").arg(copied)));
    QVERIFY2(!copied.contains(QChar(0x200B)),
             qPrintable(QStringLiteral("零宽锚点进入了剪贴板：[%1]").arg(copied)));
    QCOMPARE(copied, QStringLiteral("plain ascii line"));

    // 导出路径同样只认字符格式，不得因锚点个数不同而漏剔。
    const QString exported = view.terminalPlainText();
    QVERIFY2(!exported.contains(QChar(0x4E2D)),
             qPrintable(QStringLiteral("锚点“中”泄漏到导出文本：[%1]").arg(exported)));
    QVERIFY2(!exported.contains(QChar(0x200B)),
             qPrintable(QStringLiteral("零宽锚点泄漏到导出文本：[%1]").arg(exported)));
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

    // 告诉 PTY 的行数绝不能超过控件真正能完整显示的段数。多报一行在主屏只是顶掉最旧
    // 一行，但备用屏下整块画布会被推出视口顶部，光标随之消失。这里不要求恰好相等：
    // computeCellHeight() 按最坏情况（探测中文/符号的真实行高）预算，纯 ASCII 内容下会
    // 比实际能塞下的行数更保守，少报几行是安全的，多报一行都不行。
    for (int height = 380; height <= 460; height += 8) {
        view.resize(640, height);
        QTest::qWait(160);
        const int displayableRows = view.document()->blockCount()
            - view.verticalScrollBar()->maximum();
        QVERIFY2(view.terminalRows() <= displayableRows,
                 qPrintable(QStringLiteral("terminalRows()=%1 超过了控件实际能显示的 %2 行")
                                .arg(view.terminalRows())
                                .arg(displayableRows)));
    }
}

void TerminalAltScreenTest::cursorStaysVisibleAfterScrollbackThenFullScreenApplication()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));

    fillScrollback(view, view.terminalRows() * 3);
    // 行高只有排过一次版才量得到，首帧之后网格会自我纠正一次；必须等它稳定下来再取行数，
    // 否则拿到的是纠正前的旧值，据此生成的绘制序列和最终网格对不上。
    QTest::qWait(400);
    const int rows = view.terminalRows();
    QVERIFY(view.verticalScrollBar()->maximum() > 0);

    view.appendTerminalOutput(fullScreenApplicationFrame(rows, 1));
    QTest::qWait(400);

    // 备用屏的文档就是整块网格，滚动历史不参与渲染，第 0 行必须落在视口顶部。
    QCOMPARE(view.document()->blockCount(), view.terminalRows());
    QCOMPARE(view.verticalScrollBar()->value(), 0);
    // 段末尾的锚点是统一行高用的渲染字符，不属于终端内容，比对前先剔除。
    QString firstLine = view.document()->begin().text();
    if (firstLine.endsWith(VtermBridge::lineAnchorSuffix())) {
        firstLine.chop(VtermBridge::lineAnchorSuffix().size());
    }
    QCOMPARE(firstLine, QStringLiteral("file row 1"));

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

void TerminalAltScreenTest::lastRowStaysVisibleInsideFullScreenApplication()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));
    fillScrollback(view, view.terminalRows() * 3);

    for (int height = 380; height <= 460; height += 8) {
        view.resize(640, height);
        QTest::qWait(160);
        const int rows = view.terminalRows();
        QVERIFY(rows > 1);

        // vim 的状态行画在网格最后一行；光标停在第一行不能掩盖"最后一行被推出视口"这个问题。
        view.appendTerminalOutput(fullScreenApplicationFrame(rows, 1));
        QTest::qWait(50);

        QCOMPARE(view.document()->blockCount(), rows);
        const QTextBlock lastBlock = view.document()->lastBlock();
        QVERIFY(lastBlock.isValid());
        QTextCursor lastRowCursor(lastBlock);
        const QRect lastRowArea = view.cursorRect(lastRowCursor);
        QVERIFY2(view.viewport()->rect().contains(lastRowArea.topLeft()),
                 qPrintable(QStringLiteral("高度 %1 时最后一行 %2 落在视口 %3 之外——"
                                           "状态行/命令行不可见")
                                .arg(height)
                                .arg(QDebug::toString(lastRowArea),
                                     QDebug::toString(view.viewport()->rect()))));

        // 退出全屏程序，回到主屏，为下一轮高度测试做准备。
        view.appendTerminalOutput(QByteArrayLiteral("\x1b[?1049l"));
        QTest::qWait(50);
    }
}

void TerminalAltScreenTest::scrollReturnsToBottomAfterLeavingFullScreenApplication()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));

    const int rows = view.terminalRows();
    fillScrollback(view, rows * 3);
    QVERIFY(view.verticalScrollBar()->maximum() > 0);
    QCOMPARE(view.verticalScrollBar()->value(), view.verticalScrollBar()->maximum());

    // 打开 vim：备用屏必须贴顶显示。
    view.appendTerminalOutput(fullScreenApplicationFrame(rows, 1));
    QTest::qWait(50);
    QCOMPARE(view.verticalScrollBar()->value(), 0);

    // 退出 vim 回到主屏：exit alternate screen (?1049l)，随后 Shell 重新打印提示符。
    view.appendTerminalOutput(QByteArrayLiteral("\x1b[?1049l"));
    view.appendTerminalOutput(QByteArrayLiteral("[root@server ~]# "));
    QTest::qWait(50);

    QCOMPARE(view.verticalScrollBar()->value(), view.verticalScrollBar()->maximum());
}

void TerminalAltScreenTest::typingWhileScrolledUpJumpsBackToOutputTail()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));

    fillScrollback(view, view.terminalRows() * 3);
    QScrollBar *bar = view.verticalScrollBar();
    QVERIFY(bar->maximum() > 0);

    // 用户上翻到最顶部阅读历史：此时命令行完全在视口之外。
    bar->setValue(bar->minimum());
    QCOMPARE(bar->value(), bar->minimum());

    // 敲一个普通字符：输入必须把视图拉回末尾，否则用户看不到自己打的字。
    QTest::keyClick(&view, Qt::Key_L);
    QCOMPARE(bar->value(), bar->maximum());

    // 远端回显随后到达，视图应继续贴底。
    view.appendTerminalOutput(QByteArrayLiteral("l"));
    QTest::qWait(50);
    QCOMPARE(bar->value(), bar->maximum());

    // 回车同样如此：再次上翻到顶部后按下回车，输出必须重新可见。
    bar->setValue(bar->minimum());
    QCOMPARE(bar->value(), bar->minimum());
    QTest::keyClick(&view, Qt::Key_Return);
    QCOMPARE(bar->value(), bar->maximum());

    view.appendTerminalOutput(QByteArrayLiteral("\r\ncommand output\r\n"));
    QTest::qWait(50);
    QCOMPARE(bar->value(), bar->maximum());

    // 备用屏不适用贴底语义：全屏程序里按键仍须保持第 0 行对齐视口顶部。
    view.appendTerminalOutput(fullScreenApplicationFrame(view.terminalRows(), 1));
    QTest::qWait(50);
    QCOMPARE(bar->value(), 0);
    QTest::keyClick(&view, Qt::Key_J);
    QCOMPARE(bar->value(), 0);
}

void TerminalAltScreenTest::columnCountStaysReportedAfterScrollbarTogglesOnAltScreenEntry()
{
    TerminalView view;
    QVERIFY(showAndSettle(view));

    // 先堆足够的滚动历史，让主屏垂直滚动条真的出现——只有它出现过，进出备用屏时
    // "滚动条显隐"才会真的改变 viewport 宽度，复现这条回归路径的前提才成立。
    fillScrollback(view, view.terminalRows() * 3);
    QTest::qWait(200);
    const int columnsBeforeAltScreen = view.terminalColumns();
    QVERIFY(view.verticalScrollBar()->isVisible());

    QSignalSpy sizeChangedSpy(&view, &TerminalView::terminalSizeChanged);

    // 打开全屏程序：备用屏没有滚动历史，垂直滚动条会自动隐藏，viewport 因此变宽。
    // 这次几何变化必须被列数计算主动屏蔽——它不代表用户真的调整了窗口，让它触发一次
    // SIGWINCH 只会让 vim/nvi 在还没把"已打开文件"提示画完之前就整屏重绘,那条提示
    // 是只画一次的，重绘之后就再也看不到了。
    view.appendTerminalOutput(fullScreenApplicationFrame(view.terminalRows(), 1));
    QTest::qWait(200);
    QVERIFY(!view.verticalScrollBar()->isVisible());
    QCOMPARE(view.terminalColumns(), columnsBeforeAltScreen);

    // 退出全屏程序回到主屏：滚动历史还在，滚动条重新出现，viewport 变窄。同样不能
    // 借着这次显隐把列数悄悄改回去——否则主屏内容又要经历一次无意义的 reflow。
    view.appendTerminalOutput(QByteArrayLiteral("\x1b[?1049l"));
    QTest::qWait(200);
    QVERIFY(view.verticalScrollBar()->isVisible());
    QCOMPARE(view.terminalColumns(), columnsBeforeAltScreen);

    QVERIFY2(sizeChangedSpy.isEmpty(),
             "进出备用屏时滚动条的显隐不应该触发 terminalSizeChanged——"
             "它会向远端发送一次跟用户操作无关的 SIGWINCH");
}

void TerminalAltScreenTest::realCapturedVimByteStreamShowsStatusLine()
{
    // 直接从真实远端（10.194.135.11 上的 vim，24x80）抓的原始字节：进入备用屏、开
    // DECCKM、设滚动区、清屏、在第 24 行打印一次性的 "文件已打开" 状态行，再回到第 1 行
    // 铺正文。用真实字节而不是手写的模拟帧，排除"手写测试帧凑巧覆盖不到的真实行为"。
    const QByteArray payload = QByteArray::fromHex(
        "1b5b3f31303439681b5b3f31681b3d1b5b313b3234721b5b3f31323b3235681b5b3f31326c"
        "1b5b3f3235681b5b32376d1b5b6d1b5b481b5b324a1b5b3f32356c1b5b32343b3148227e2f"
        "73616d706c652e747874222034304c2c20343830431b5b313b314868656c6c6f20776f7"
        "26c640d0a68656c6c6f20776f726c640d0a68656c6c6f20776f726c640d0a68656c6c6f2"
        "0776f726c640d0a68656c6c6f20776f726c640d0a68656c6c6f20776f726c640d0a68656"
        "c6c6f20776f726c640d0a68656c6c6f20776f726c640d0a68656c6c6f20776f726c640d0"
        "a68656c6c6f20776f726c640d0a68656c6c6f20776f726c640d0a68656c6c6f20776f726"
        "c640d0a68656c6c6f20776f726c640d0a68656c6c6f20776f726c640d0a68656c6c6f207"
        "76f726c640d0a68656c6c6f20776f726c640d0a68656c6c6f20776f726c640d0a68656c6"
        "c6f20776f726c640d0a68656c6c6f20776f726c640d0a68656c6c6f20776f726c640d0a6"
        "8656c6c6f20776f726c640d0a68656c6c6f20776f726c640d0a68656c6c6f20776f726c6"
        "41b5b313b31481b5b3f31326c1b5b3f323568");
    QVERIFY(!payload.isEmpty());

    VtermBridge bridge;
    bridge.resize(24, 80);
    bridge.feed(payload);
    QVERIFY(bridge.isAlternateScreen());

    QTextDocument document;
    bridge.render(&document);
    QCOMPARE(document.blockCount(), 24);

    const QTextBlock statusBlock = document.findBlockByNumber(23);
    QVERIFY(statusBlock.isValid());
    QVERIFY2(statusBlock.text().contains(QStringLiteral("sample.txt")),
             qPrintable(QStringLiteral("第 24 行本应是 vim 的状态行，实际内容是: \"%1\"")
                            .arg(statusBlock.text())));
}


void TerminalAltScreenTest::realChunkedSshByteStreamShowsStatusLine()
{
    // 同一份真实字节，但按 SshSessionStack 实际收到的 SSH channel 分片边界（1024/1024/
    // 1024/552 字节）分成四次单独 feed()——而不是拼成一个大缓冲一次性喂。这份抓包直接
    // 来自 10.194.135.11 上对 readme.txt 执行 vi 时的 107x44 真实终端。如果分片喂养和
    // 整体喂养结果不同，问题就出在跨 feed() 调用的状态处理上，而不是转义序列本身。
    const QByteArray chunks[] = {
        QByteArray::fromHex("1b5b3f31303439681b5b3f31681b3d1b5b313b3434721b5b3f31323b3235681b5b3f31326c1b5b3f3235681b5b32376d1b5b6d1b5b481b5b324a1b5b3f32356c1b5b34343b314822726561646d652e74787422205b6e6f656f6c5d2032314c2c20393434431b5b313b31484c696e7578e5ae89e8a385e6ada5e9aaa40d0ae4b88be8bdbde5ae89e8a385e58c850d0a31e38081e4b88be8bdbde5ae89e8a385e69687e4bbb61b5b343b394861e38081e591bde4bba4e8a18ce696b9e5bc8fe4b88be8bdbd1b5b353b31374877676574202d2d6e6f2d636865636b2d63657274696669636174652068747470733a2f2f256d677264636970252f68746d6c2f6c696e75785f6564725f696e7374616c6c65722e7461722e677a1b5b363b394862e380814d4752e799bbe5bd95e7958ce99da2e4b88be8bdbdefbc8ce68896e88085e7b3bbe7bb9fe7aea1e790862d3ee7bb88e7abafe983a8e7bdb22d3ee4b88be8bdbde5ae89e8a385e58c85e983a8e7bdb2e4b8ade4b88be8bdbd0d0a32e38081e8a7a3e58e8be5ae89e8a385e58c85efbc9a746172202d787a7666206c696e75785f6564725f696e7374616c6c65722e7461722e677a0d0a33e38081e689a7e8a18ce591bde4bba4efbc9a2e2f6167656e745f696e7374616c6c65722e73681b5b393b394861e38081e697a0e58f82e5ae89e8a385efbc88e59ca8e5908ce4b880e79baee5bd95e4b8ade69c896d616e616765725f696e666f2e747874e697b6e4bdbfe794a8efbc891b5b31303b3137482e2f6167656e745f696e7374616c6c65722e73681b5b31313b394862e38081e887aae5ae9ae4b989e5ae89e8a385efbc88e5ae89e8a385e59f9fe5908de38081e7abafe58fa3e38081e8b7afe5be84e59d87e58fafe98089efbc8ce58fafe4bbbbe6848fe887aae794b1e7bb84e59088efbc891b5b31323b3137482e2f6167656e745f696e7374616c6c65722e7368202d661b5b31333b3137482e2f6167656e745f696e7374616c6c65722e7368202d68207777772e6d67722e636f6d1b5b31343b3137482e2f6167656e745f696e7374616c6c65722e7368202d68207777772e6d67722e636f6d202d7020343433202d64202f726f6f742f6564722f202d661b5b31353b313748e58f82e695b0e58897e8a1a8efbc9a1b5b31363b3235482d6820686f73741b5b3743e887aae5ae9ae4b989e59f9fe5908de3808149501b5b31373b3235482d7020706f72741b5b3743e887aae5ae9ae4b989e7abafe58fa31b5b31383b3235482d64206469721b5b3843e887aae5ae9ae4b989e5ae89e8a385e8b7afe5be84efbc88e7bb9de5afb9e8b7afe5be84efbc891b5b31393b3235482d661b5b313243e5bcbae588b6e5ae89e8a3851b5b32303b3235482d2d68656c701b5b3843e698bee7a4bae5b8ae"),
        QByteArray::fromHex("e58aa9e69687e6a1a30d0a34e38081e5ae89e8a385e5ae8ce68890efbc8c4167656e74e5b086e887aae58aa8e8bf9ee68ea5e7bb88e7abafe6a380e6b58be5938de5ba94e5b9b3e58fb00d0a1b5b39346d7e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b32333b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b32343b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b32353b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b32363b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b32373b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b32383b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b32393b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33303b31487e202020202020202020202020202020202020202020202020202020202020"),
        QByteArray::fromHex("202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33313b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33323b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33333b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33343b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33353b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33363b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33373b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33383b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33393b31487e20202020202020202020202020202020202020202020202020202020"),
        QByteArray::fromHex("2020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b34303b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b34313b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b34323b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b34333b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b313b31481b5b3f31326c1b5b3f323568"),
    };
    VtermBridge bridge;
    bridge.resize(44, 107);
    for (const QByteArray &chunk : chunks) {
        bridge.feed(chunk);
    }
    QVERIFY(bridge.isAlternateScreen());

    QTextDocument document;
    bridge.render(&document);
    QCOMPARE(document.blockCount(), 44);

    const QTextBlock statusBlock = document.findBlockByNumber(43);
    QVERIFY(statusBlock.isValid());
    QVERIFY2(statusBlock.text().contains(QStringLiteral("readme.txt")),
             qPrintable(QStringLiteral("第 44 行本应是 vim 的状态行，实际内容是: \"%1\"")
                            .arg(statusBlock.text())));
}


void TerminalAltScreenTest::realChunkedSshByteStreamVisibleInTerminalViewWidget()
{
    // 同一份真实分片字节，这次喂给完整的 TerminalView（真实 QPlainTextEdit 控件），
    // 而不是只测 VtermBridge 的文档输出——如果 VtermBridge 层是对的，但问题出在控件
    // 几何/滚动条这一层，只有跑完整控件才能复现。先把控件精确摆成 107 列 44 行，
    // 和抓包时远端收到的窗口大小完全一致，转义序列里的绝对定位才有意义。
    TerminalView view;
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    const QFontMetrics metrics(view.font());
    const int cellWidth = std::max(metrics.horizontalAdvance(QLatin1Char('M')), 1);
    const int baseHeight = std::max(metrics.lineSpacing(), 1);
    const int documentMargin = static_cast<int>(view.document()->documentMargin());

    // 先铺一屏带锚点的内容再收敛尺寸：行高必须等文档真的排过版才量得到，空文档时
    // terminalRows() 用的还是兜底值，照它收敛出来的窗口尺寸一旦有了内容就会被纠正掉。
    view.appendTerminalOutput(QByteArrayLiteral("\x1b[?1049h\x1b[H\x1b[2J"));
    view.appendTerminalOutput(
        QString::fromUtf8("\x1b[1;1H\xe4\xb8\xad\xe6\x96\x87 warmup").toUtf8());
    QTest::qWait(300);

    int width = cellWidth * 107 + 40;
    int height = baseHeight * 44 * 2 + 2 * documentMargin + 80;
    for (int attempt = 0; attempt < 60; ++attempt) {
        view.resize(width, height);
        QTest::qWait(200);
        if (view.terminalColumns() == 107 && view.terminalRows() == 44) {
            break;
        }
        if (view.terminalColumns() < 107) { width += cellWidth; }
        if (view.terminalColumns() > 107) { width -= cellWidth; }
        if (view.terminalRows() < 44) { height += baseHeight; }
        if (view.terminalRows() > 44) { height -= baseHeight; }
    }
    QCOMPARE(view.terminalColumns(), 107);
    QCOMPARE(view.terminalRows(), 44);

    const QByteArray chunks[] = {
        QByteArray::fromHex("1b5b3f31303439681b5b3f31681b3d1b5b313b3434721b5b3f31323b3235681b5b3f31326c1b5b3f3235681b5b32376d1b5b6d1b5b481b5b324a1b5b3f32356c1b5b34343b314822726561646d652e74787422205b6e6f656f6c5d2032314c2c20393434431b5b313b31484c696e7578e5ae89e8a385e6ada5e9aaa40d0ae4b88be8bdbde5ae89e8a385e58c850d0a31e38081e4b88be8bdbde5ae89e8a385e69687e4bbb61b5b343b394861e38081e591bde4bba4e8a18ce696b9e5bc8fe4b88be8bdbd1b5b353b31374877676574202d2d6e6f2d636865636b2d63657274696669636174652068747470733a2f2f256d677264636970252f68746d6c2f6c696e75785f6564725f696e7374616c6c65722e7461722e677a1b5b363b394862e380814d4752e799bbe5bd95e7958ce99da2e4b88be8bdbdefbc8ce68896e88085e7b3bbe7bb9fe7aea1e790862d3ee7bb88e7abafe983a8e7bdb22d3ee4b88be8bdbde5ae89e8a385e58c85e983a8e7bdb2e4b8ade4b88be8bdbd0d0a32e38081e8a7a3e58e8be5ae89e8a385e58c85efbc9a746172202d787a7666206c696e75785f6564725f696e7374616c6c65722e7461722e677a0d0a33e38081e689a7e8a18ce591bde4bba4efbc9a2e2f6167656e745f696e7374616c6c65722e73681b5b393b394861e38081e697a0e58f82e5ae89e8a385efbc88e59ca8e5908ce4b880e79baee5bd95e4b8ade69c896d616e616765725f696e666f2e747874e697b6e4bdbfe794a8efbc891b5b31303b3137482e2f6167656e745f696e7374616c6c65722e73681b5b31313b394862e38081e887aae5ae9ae4b989e5ae89e8a385efbc88e5ae89e8a385e59f9fe5908de38081e7abafe58fa3e38081e8b7afe5be84e59d87e58fafe98089efbc8ce58fafe4bbbbe6848fe887aae794b1e7bb84e59088efbc891b5b31323b3137482e2f6167656e745f696e7374616c6c65722e7368202d661b5b31333b3137482e2f6167656e745f696e7374616c6c65722e7368202d68207777772e6d67722e636f6d1b5b31343b3137482e2f6167656e745f696e7374616c6c65722e7368202d68207777772e6d67722e636f6d202d7020343433202d64202f726f6f742f6564722f202d661b5b31353b313748e58f82e695b0e58897e8a1a8efbc9a1b5b31363b3235482d6820686f73741b5b3743e887aae5ae9ae4b989e59f9fe5908de3808149501b5b31373b3235482d7020706f72741b5b3743e887aae5ae9ae4b989e7abafe58fa31b5b31383b3235482d64206469721b5b3843e887aae5ae9ae4b989e5ae89e8a385e8b7afe5be84efbc88e7bb9de5afb9e8b7afe5be84efbc891b5b31393b3235482d661b5b313243e5bcbae588b6e5ae89e8a3851b5b32303b3235482d2d68656c701b5b3843e698bee7a4bae5b8ae"),
        QByteArray::fromHex("e58aa9e69687e6a1a30d0a34e38081e5ae89e8a385e5ae8ce68890efbc8c4167656e74e5b086e887aae58aa8e8bf9ee68ea5e7bb88e7abafe6a380e6b58be5938de5ba94e5b9b3e58fb00d0a1b5b39346d7e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b32333b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b32343b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b32353b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b32363b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b32373b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b32383b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b32393b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33303b31487e202020202020202020202020202020202020202020202020202020202020"),
        QByteArray::fromHex("202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33313b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33323b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33333b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33343b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33353b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33363b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33373b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33383b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b33393b31487e20202020202020202020202020202020202020202020202020202020"),
        QByteArray::fromHex("2020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b34303b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b34313b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b34323b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b34333b31487e202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020202020201b5b313b31481b5b3f31326c1b5b3f323568"),
    };
    for (const QByteArray &chunk : chunks) {
        view.appendTerminalOutput(chunk);
        QTest::qWait(40);
    }
    QTest::qWait(200);

    QCOMPARE(view.document()->blockCount(), 44);
    const QTextBlock statusBlock = view.document()->findBlockByNumber(43);
    QVERIFY(statusBlock.isValid());
    QVERIFY2(statusBlock.text().contains(QStringLiteral("readme.txt")),
             qPrintable(QStringLiteral("第 44 行文档内容是: \"%1\"")
                            .arg(statusBlock.text())));

    QTextCursor statusCursor(statusBlock);
    const QRect statusArea = view.cursorRect(statusCursor);
    QVERIFY2(view.viewport()->rect().contains(statusArea.topLeft()),
             qPrintable(QStringLiteral("状态行 %1 落在视口 %2 之外")
                            .arg(QDebug::toString(statusArea),
                                 QDebug::toString(view.viewport()->rect()))));
}


void TerminalAltScreenTest::everyRenderedLineHasIdenticalHeightRegardlessOfContent()
{
    // 网格能不能铺满视口，前提是"每一行都一样高"。QPlainTextDocumentLayout 忽略
    // setLineHeight()，行高由该行用到的所有字体的 ascent/descent 最大值决定：纯 ASCII、
    // 中文、emoji 三种行天生互不相同。高度一旦随内容变化，同样的行数、实际像素高度就会
    // 随用户编辑而变——状态行时而被挤出视口、时而往上漂。
    //
    // 字体逐个测：不同字体的后备链不一样，PT Mono 15pt（用户实际配置）暴露过 Menlo 测
    // 不出来的 1px 偏差——只锚 CJK 时含 emoji 的行仍比别人高 1px。
    const QVector<QPair<QString, int>> fonts = {
        {QStringLiteral("PT Mono"), 15},
        {QStringLiteral("Menlo"), 13},
    };
    for (const auto &fontSpec : fonts) {
        TerminalView view;
        TerminalSettings settings;
        settings.fontFamily = fontSpec.first;
        settings.fontPointSize = fontSpec.second;
        view.applySettings(settings);
        view.resize(900, 600);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QTest::qWait(200);

        view.appendTerminalOutput(QByteArrayLiteral("\x1b[?1049h\x1b[H\x1b[2J"));
        view.appendTerminalOutput(QString::fromUtf8(
            "\x1b[1;1H\xe4\xb8\xad\xe6\x96\x87\xe8\xa1\x8c"
            "\x1b[2;1Hplain ascii line"
            "\x1b[3;1H~"
            "\x1b[4;1HLinux\xe5\xae\x89\xe8\xa3\x85 mixed"
            "\x1b[5;1H\xF0\x9F\x98\x80 emoji line"
            "\x1b[6;1H\xe2\x94\x83\xe2\x94\x80 box"
            "\x1b[7;1H\"readme.txt\" [noeol] 21L, 944C").toUtf8());
        QTest::qWait(200);

        QVERIFY(view.document()->blockCount() >= 7);
        QList<double> heights;
        for (int i = 0; i < 7; ++i) {
            heights.append(view.document()->documentLayout()
                               ->blockBoundingRect(view.document()->findBlockByNumber(i))
                               .height());
        }
        for (int i = 1; i < heights.size(); ++i) {
            QVERIFY2(qFuzzyCompare(heights.at(i), heights.at(0)),
                     qPrintable(QStringLiteral("%1 %2pt：第 %3 行高 %4，与第 0 行的 %5 不一致"
                                               "——行高随内容变化，网格无法稳定铺满视口")
                                    .arg(fontSpec.first)
                                    .arg(fontSpec.second)
                                    .arg(i)
                                    .arg(heights.at(i))
                                    .arg(heights.at(0))));
        }

        // 锚点是渲染用的字符，不能出现在导出文本里。
        QVERIFY(!view.terminalPlainText().contains(VtermBridge::lineAnchorSuffix()));

        // 但正文里合法的 U+3000（用户自己敲的表意空格）必须原样保留，不能被无差别删掉。
        view.appendTerminalOutput(QString::fromUtf8("\x1b[9;1Ha\xe3\x80\x80" "b").toUtf8());
        QTest::qWait(150);
        QVERIFY2(view.terminalPlainText().contains(QString::fromUtf8("a\xe3\x80\x80""b")),
                 "行内合法的表意空格被误当成锚点删掉了");
    }
}


void TerminalAltScreenTest::gridFillsViewportWithoutWastedRows()
{
    // 行数不但不能多报（多报会把状态行挤出视口），也不能少报：少报一行，备用屏下画布
    // 顶部对齐后底部就空出一条，用户看到的就是"状态栏浮在半空、下面一大片黑"。
    //
    // 这里必须拿像素算，不能用 blockCount - scrollBar()->maximum()：备用屏下滚动条被
    // 强制隐藏、maximum() 恒为 0，那个式子会退化成 rows == rows 的恒真式，什么都测不到。
    //
    // 也必须用真实屏幕跑（Retina 下 dpr=2）：行高只有依附到窗口的文档才量得准，脱离
    // 窗口的探针文档量的是 dpr=1 的值，Retina 上偏大 17%，正是少报行数的根因。
    TerminalView view;
    TerminalSettings settings;
    settings.fontFamily = QStringLiteral("PT Mono");
    settings.fontPointSize = 15;
    view.applySettings(settings);
    view.resize(900, 700);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTest::qWait(200);

    view.appendTerminalOutput(QByteArrayLiteral("\x1b[?1049h\x1b[H\x1b[2J"));
    view.appendTerminalOutput(
        QString::fromUtf8("\x1b[1;1H\xe4\xb8\xad\xe6\x96\x87 line").toUtf8());
    QTest::qWait(500);

    for (int height = 600; height <= 720; height += 40) {
        view.resize(900, height);
        QTest::qWait(500);

        // 锚点保证每段等高，取第一段就是真实行高。
        const double lineHeight = view.document()->documentLayout()
                                      ->blockBoundingRect(view.document()->firstBlock())
                                      .height();
        QVERIFY(lineHeight > 0);
        const int margin = static_cast<int>(view.document()->documentMargin());
        const int usable = view.viewport()->height() - 2 * margin - 1;
        const int fits = static_cast<int>(usable / lineHeight);

        QVERIFY2(view.terminalRows() == fits,
                 qPrintable(QStringLiteral("窗口高 %1：视口能放下 %2 行（行高 %3、可用 %4px），"
                                           "但汇报给 PTY 的是 %5 行——%6")
                                .arg(height).arg(fits).arg(lineHeight).arg(usable)
                                .arg(view.terminalRows())
                                .arg(view.terminalRows() < fits
                                         ? QStringLiteral("少报，底部会空出一条")
                                         : QStringLiteral("多报，最后一行会被挤出视口"))));
    }
}

QTEST_MAIN(TerminalAltScreenTest)
#include "TerminalAltScreenTest.moc"
