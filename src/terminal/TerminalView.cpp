#include "terminal/TerminalView.h"

#include "settings/TerminalSettings.h"
#include "terminal/VtermBridge.h"

#include <QApplication>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSet>

#include <utility>
#include <QScrollBar>
#include <QClipboard>
#include <QCompleter>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QKeySequence>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPaintEvent>
#include <QAbstractItemView>
#include <QStringListModel>
#include <QTextCursor>
#include <QTimer>

#include <algorithm>

namespace {

/**
 * @brief 认证接线的按需诊断通道。
 *
 * 默认关闭。真实 macOS 键盘与输入法的行为无法用 QTest 合成按键覆盖，出问题时只能靠
 * 用户环境里的实测数据定位；因此保留一条可用环境变量打开的通道，而不是靠反复猜测。
 * 打开方式：QT_LOGGING_RULES="shellium.auth.debug=true"
 */
Q_LOGGING_CATEGORY(authLog, "shellium.auth", QtWarningMsg)

/**
 * @brief 判断按键是否包含“终端意义上的物理 Control 键”。
 *
 * Qt 在 macOS 为保持标准快捷键跨平台一致，会把 Command 映射为 ControlModifier，物理
 * Control 映射为 MetaModifier。终端协议必须使用物理 Control，否则 Ctrl+C 不会产生 ETX，
 * 而 Command+C 反而可能错误终止程序。
 */
bool hasTerminalControlModifier(const QKeyEvent *event)
{
#if defined(Q_OS_MACOS)
    return event->modifiers().testFlag(Qt::MetaModifier);
#else
    return event->modifiers().testFlag(Qt::ControlModifier);
#endif
}

/**
 * @brief 从远端 PTY 实际回显的当前行恢复 ssh 命令。
 * @param bufferedCommand Qt 键盘事件累计的命令，正常情况下直接使用。
 * @param cursorLine libvterm 光标所在行的可见文本，包含 Shell 提示符和实际回显命令。
 * @return 识别到回显 ssh 命令时返回该命令，否则返回原始键盘缓冲。
 *
 * macOS 输入法、快捷键和自动重复事件在真实键盘环境下可能让本地字符缓冲与 PTY 回显
 * 不一致。远端 Shell 最终执行的是 PTY 中的内容，因此只对明确位于提示符之后的 ssh 命令
 * 使用回显兜底；普通命令仍使用本地缓冲，避免把历史输出误识别成新连接。
 */
QString recoverEchoedSshCommand(const QString &bufferedCommand, const QString &cursorLine)
{
    static const QRegularExpression bufferedSshPattern(
        QStringLiteral(R"(^\s*(?:command\s+)?(?:\S*/)?ssh(?:\s+.*)?\s*$)"));
    if (bufferedSshPattern.match(bufferedCommand).hasMatch()) {
        // 本地缓冲已经是完整 ssh 命令时优先使用它。远端回显可能比键盘输入稍慢，如果
        // 无条件使用光标行，会把尚未回显的主机名尾字符截断成错误目标。
        return bufferedCommand;
    }

    static const QRegularExpression echoedSshPattern(
        QStringLiteral(R"((?:^|[#>$%❯➜]\s*)((?:command\s+)?(?:\S*/)?ssh(?:\s+.*))\s*$)"));
    const QRegularExpressionMatch match = echoedSshPattern.match(cursorLine);
    return match.hasMatch() ? match.captured(1).trimmed() : bufferedCommand;
}

} // namespace

TerminalView::TerminalView(QWidget *parent)
    : QPlainTextEdit(parent)
    , m_vtermBridge(new VtermBridge(this))
    , m_commandCompleter(new QCompleter(this))
    , m_completionModel(new QStringListModel(this))
    , m_cursorBlinkTimer(new QTimer(this))
{
    // 主窗口会统一装饰表单中的 QPlainTextEdit。终端必须有稳定对象名供 QSS 排除通用表单
    // 背景，否则父级卡片色会覆盖本控件按终端主题设置的 QPalette::Base。
    setObjectName(QStringLiteral("TerminalView"));
    setReadOnly(true);
    // macOS 会根据该属性决定是否把字符交给输入法。只读文本控件默认关闭此属性，但终端
    // 虽然不直接编辑 QTextDocument，仍然必须接收普通英文和中文输入法的提交事件。
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setUndoRedoEnabled(false);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    // 终端列数已经按 viewport 宽度算好并通过 SIGWINCH 告知远端，正常渲染内容不会超出
    // viewport。允许横向滚动条按需出现/消失会改变 viewport 高度，进而触发新一轮
    // resize→SIGWINCH→重排→滚动条再次切换的死循环，表现为窗口不停上下跳动；关闭横向
    // 滚动条彻底切断这个反馈环。
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 终端中的 Tab 属于 Shell 补全协议，不应切换到右侧文件管理器或工具栏。
    setTabChangesFocus(false);
    setFocusPolicy(Qt::StrongFocus);
    setContextMenuPolicy(Qt::DefaultContextMenu);
    setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background: #111318; color: #e6e6e6; border: 0; padding: 6px; }"));

    QFont terminalFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    terminalFont.setPointSize(13);
    setFont(terminalFont);

    // 终端收到 DSR/DA 等查询后，libvterm 会生成必须回送给 PTY 的协议字节。
    connect(m_vtermBridge, &VtermBridge::protocolResponse,
            this, &TerminalView::inputBytes);
    connect(m_vtermBridge, &VtermBridge::bell, this, [this]() {
        if (property("shelliumAudibleBell").toBool()) {
            QApplication::beep();
        }
    });
    connect(m_vtermBridge,
            &VtermBridge::shellIntegrationEvent,
            this,
            &TerminalView::shellIntegrationEvent);

    // 补全只做前缀匹配并保持模型传入的最近使用顺序。UnsortedModel 可以避免 QCompleter
    // 为二分查找重新排序，从而把多年未用但字典序靠前的命令放到最近命令之前。
    m_commandCompleter->setWidget(this);
    m_commandCompleter->setModel(m_completionModel);
    m_commandCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_commandCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_commandCompleter->setModelSorting(QCompleter::UnsortedModel);
    m_commandCompleter->setFilterMode(Qt::MatchStartsWith);
    m_commandCompleter->setMaxVisibleItems(10);
    connect(m_commandCompleter,
            qOverload<const QString &>(&QCompleter::activated),
            this,
            &TerminalView::acceptCompletion);

    m_cursorBlinkTimer->setInterval(600);
    connect(m_cursorBlinkTimer, &QTimer::timeout, this, [this]() {
        m_cursorPhaseVisible = !m_cursorPhaseVisible;
        m_vtermBridge->setCursorPresentation(
            static_cast<VtermBridge::CursorShape>(m_cursorShape), m_cursorPhaseVisible);
        // 闪烁只改变光标单元格，却同样会整体重建文档。这里必须保持滚动位置，否则用户
        // 什么都不做，视图每 600ms 就自己跳回顶部一次。
        renderPreservingScroll();
        viewport()->update();
    });

    // 即使设置服务尚未接入，终端也使用同一份规范化默认值，避免构造路径与设置路径出现
    // 两套字体、颜色或铃声行为。
    applySettings(TerminalSettings{}.normalized());
}

void TerminalView::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu *menu = createTerminalContextMenu();
    // 菜单执行结束后 deleteLater() 可避免在某个 triggered 槽仍处理事件时同步销毁动作。
    menu->exec(event->globalPos());
    menu->deleteLater();
}

QMenu *TerminalView::createTerminalContextMenu()
{
    // createStandardContextMenu() 返回由调用方负责销毁的菜单。继续使用 Qt 的标准
    // 复制/全选动作，只在末尾追加 SFTP 入口，保证平台原生文本操作不因业务菜单被替换。
    QMenu *menu = createStandardContextMenu();
    menu->addSeparator();
    QAction *uploadAction = menu->addAction(tr("上传到当前远程目录…"));
    connect(uploadAction, &QAction::triggered,
            this, &TerminalView::uploadToCurrentDirectoryRequested);
    return menu;
}

void TerminalView::applySettings(const TerminalSettings &settings)
{
    const TerminalSettings normalized = settings.normalized();
    const TerminalPalette terminalPalette = normalized.effectivePalette();

    QFont terminalFont = normalized.fontFamily.isEmpty()
        ? QFontDatabase::systemFont(QFontDatabase::FixedFont)
        : QFont(normalized.fontFamily);
    terminalFont.setPointSize(normalized.fontPointSize);
    terminalFont.setLetterSpacing(QFont::AbsoluteSpacing, normalized.letterSpacing);
    setFont(terminalFont);

    m_basePointSize = normalized.fontPointSize;
    m_lineHeightPercent = normalized.lineHeightPercent;
    m_cursorShape = static_cast<int>(normalized.cursorShape);
    m_cursorColor = terminalPalette.cursor;
    m_cursorPhaseVisible = true;
    m_vtermBridge->setDefaultColors(terminalPalette.foreground,
                                    terminalPalette.background,
                                    terminalPalette.cursor);
    m_vtermBridge->setScrollbackLimit(normalized.scrollbackLines);
    m_vtermBridge->setLineHeightPercent(normalized.lineHeightPercent);
    m_vtermBridge->setCursorPresentation(
        static_cast<VtermBridge::CursorShape>(m_cursorShape), true);

    // 使用控件调色板而非固定背景样式，选择颜色与主题可在设置实时预览时同步更新。终端
    // 单元格的显式 ANSI 色仍由 libvterm 提供，不会被这里覆盖。
    QPalette viewPalette = palette();
    viewPalette.setColor(QPalette::Base, terminalPalette.background);
    viewPalette.setColor(QPalette::Text, terminalPalette.foreground);
    viewPalette.setColor(QPalette::Highlight, terminalPalette.selection);
    setPalette(viewPalette);
    // 背景必须在本控件的样式表里写成具体颜色。主窗口全局样式表用 palette(base) 描述终端
    // 背景，而它取的是 QApplication 调色板的 Base（另一档更浅的窗口色），并非这里刚设置的
    // 终端背景色；两者相差一档，表现为"有字符的区域和空白区域颜色不一样"。控件自身的
    // 样式表优先级高于应用级，写死颜色可让整块终端与 libvterm 单元格背景完全一致。
    setStyleSheet(QStringLiteral("QPlainTextEdit#TerminalView {"
                                 " background: %1; color: %2; border: 0; padding: 6px; }")
                      .arg(terminalPalette.background.name(),
                           terminalPalette.foreground.name()));

    setProperty("shelliumAudibleBell", normalized.audibleBell);
    if (normalized.cursorBlink) {
        m_cursorBlinkTimer->start();
    } else {
        m_cursorBlinkTimer->stop();
    }
    renderPreservingScroll();
    viewport()->update();

    // 字体或行高变化会改变 PTY 的字符尺寸；主动重新计算，让远端及时收到 SIGWINCH，
    // 而不是必须等用户再拖动一次窗口。
    recalculateTerminalGeometry();
}

void TerminalView::zoomFont(const int deltaPoints)
{
    QFont terminalFont = font();
    const int clamped = std::clamp(terminalFont.pointSize() + deltaPoints, 6, 72);
    if (clamped == terminalFont.pointSize()) {
        return;
    }
    terminalFont.setPointSize(clamped);
    setFont(terminalFont);
    renderPreservingScroll();
    viewport()->update();
    recalculateTerminalGeometry();
}

void TerminalView::resetZoom()
{
    zoomFont(m_basePointSize - font().pointSize());
}

bool TerminalView::isScrolledToBottom() const
{
    const QScrollBar *bar = verticalScrollBar();
    // QPlainTextEdit 的垂直滚动条以"块"（行）为单位。留 1 行余量吸收重建带来的取整差异，
    // 但不能再放宽：阈值一旦覆盖大段历史，用户上翻阅读时的新输出又会把他强行拉回底部。
    return bar->maximum() - bar->value() <= 1;
}

void TerminalView::renderPreservingScroll()
{
    renderPreservingScroll(isScrolledToBottom(), verticalScrollBar()->value());
}

void TerminalView::renderPreservingScroll(const bool followBottom, const int previousValue)
{
    m_vtermBridge->render(document());
    QScrollBar *bar = verticalScrollBar();
    bar->setValue(followBottom ? bar->maximum()
                               : std::clamp(previousValue, bar->minimum(), bar->maximum()));
}

void TerminalView::recalculateTerminalGeometry()
{
    const QFontMetrics metrics(font());
    const int cellWidth = std::max(metrics.horizontalAdvance(QLatin1Char('M')), 1);
    const int baseHeight = std::max(metrics.lineSpacing(), 1);
    const int cellHeight = std::max(baseHeight * m_lineHeightPercent / 100, 1);
    const int columns = std::max(viewport()->width() / cellWidth, 1);
    const int rows = std::max(viewport()->height() / cellHeight, 1);
    m_vtermBridge->resize(rows, columns);
    emit terminalSizeChanged(columns, rows);
}

bool TerminalView::findTerminalText(const QString &text, const bool backward)
{
    if (text.isEmpty()) {
        return false;
    }
    const QTextDocument::FindFlags flags = backward ? QTextDocument::FindBackward
                                                     : QTextDocument::FindFlags{};
    if (find(text, flags)) {
        return true;
    }

    // 到达文档边界时环回，连续按快捷键可以遍历全部命中，而无需手工滚回开头。
    QTextCursor cursor(document());
    cursor.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
    setTextCursor(cursor);
    return find(text, flags);
}

QString TerminalView::terminalPlainText() const
{
    return document()->toPlainText();
}

void TerminalView::setCommandCompletions(const QStringList &commands)
{
    QStringList normalized;
    QSet<QString> seen;
    normalized.reserve(commands.size());
    for (const QString &command : commands) {
        if (command.trimmed().isEmpty() || seen.contains(command)) {
            continue;
        }
        seen.insert(command);
        normalized.append(command);
    }
    m_completionModel->setStringList(normalized);
    updateCompletionPopup();
}

void TerminalView::submitCommand(const QString &command)
{
    if (m_localCapture || command.trimmed().isEmpty()) {
        return;
    }
    // 命令和回车合并发送可保持 worker 队列中的原子顺序；commandSubmitted 随后触发历史
    // 与嵌套识别，上层仍然看到和真实键盘回车完全一致的提交时点。
    emit inputBytes(command.toUtf8() + QByteArrayLiteral("\r"));
    emit commandSubmitted(command);
    m_currentCommand.clear();
    m_commandCompleter->popup()->hide();
}

void TerminalView::updateCompletionPopup()
{
    if (m_localCapture || m_currentCommand.size() < 2 || m_completionModel->rowCount() == 0) {
        m_commandCompleter->popup()->hide();
        return;
    }
    m_commandCompleter->setCompletionPrefix(m_currentCommand);
    if (m_commandCompleter->completionCount() <= 0) {
        m_commandCompleter->popup()->hide();
        return;
    }

    // 终端文档的文本光标不等于远端 PTY 光标，但它始终位于视图末尾附近；以此作为弹层
    // 锚点比固定在左上角更自然，也不会尝试从终端输出反推 Shell 的行编辑状态。
    QRect popupRect = cursorRect();
    popupRect.setWidth(qMax(320, m_commandCompleter->popup()->sizeHintForColumn(0)
                                    + m_commandCompleter->popup()->verticalScrollBar()->sizeHint().width()));
    m_commandCompleter->complete(popupRect);
}

void TerminalView::acceptCompletion(const QString &completion)
{
    if (m_localCapture || completion.size() < m_currentCommand.size()
        || !completion.startsWith(m_currentCommand, Qt::CaseInsensitive)) {
        m_commandCompleter->popup()->hide();
        return;
    }
    const QString suffix = completion.mid(m_currentCommand.size());
    if (!suffix.isEmpty()) {
        emit inputBytes(suffix.toUtf8());
    }
    m_currentCommand = completion;
    m_commandCompleter->popup()->hide();
}

TerminalView::PastePreparation TerminalView::prepareClipboardPaste(
    const QString &clipboardText)
{
    PastePreparation result;
    result.text.reserve(clipboardText.size());

    for (qsizetype index = 0; index < clipboardText.size(); ++index) {
        const QChar character = clipboardText.at(index);
        if (character == QLatin1Char('\r')) {
            // CRLF 只生成一个 LF；裸 CR 也规范化为 LF，避免同一份剪贴板文本在不同平台
            // 上表现为不同的命令提交次数。
            if (index + 1 < clipboardText.size()
                && clipboardText.at(index + 1) == QLatin1Char('\n')) {
                ++index;
            }
            result.text.append(QLatin1Char('\n'));
            result.containsLineBreak = true;
            continue;
        }
        if (character == QLatin1Char('\n')) {
            result.text.append(character);
            result.containsLineBreak = true;
            continue;
        }
        if (character == QLatin1Char('\t')) {
            // Tab 是 Shell 补全、缩进文本和交互式程序中的正常输入，不属于危险控制字符。
            result.text.append(character);
            continue;
        }

        const ushort code = character.unicode();
        const bool c0Control = code < 0x20;
        const bool deleteControl = code == 0x7f;
        const bool c1Control = code >= 0x80 && code <= 0x9f;
        if (c0Control || deleteControl || c1Control) {
            // ESC 可伪造 CSI/OSC 序列，NUL 会改变 C 字符串语义，其余 C0/C1 字符也可能
            // 触发终端模式切换；普通粘贴没有理由静默放行这些不可见字节。
            ++result.removedControlCharacters;
            continue;
        }
        result.text.append(character);
    }
    return result;
}

void TerminalView::appendTerminalOutput(const QByteArray &bytes)
{
    if (bytes.isEmpty()) {
        return;
    }

    // SSH 密码提示可能被网络分成多个数据块，因此保留短尾缓冲后再匹配。只检测固定的
    // "password:" / "password for ...:" 形式，避免把日志里的 password 字段误判为认证。
    m_authenticationPromptBuffer.append(bytes);
    if (m_authenticationPromptBuffer.size() > 512) {
        m_authenticationPromptBuffer = m_authenticationPromptBuffer.last(512);
    }
    const QByteArray normalizedPrompt = m_authenticationPromptBuffer.toLower();
    const bool hasPasswordPrompt = normalizedPrompt.contains("password:")
        || (normalizedPrompt.contains("password for ") && normalizedPrompt.endsWith(':'));
    if (hasPasswordPrompt && !m_authenticationPromptReported) {
        m_authenticationPromptReported = true;
        emit authenticationPromptDetected();
    }

    // 嵌套 ssh 的失败形式远不止认证被拒：解析不了主机名、连不上、主机密钥不符都会让
    // OpenSSH 直接退出并回到外层 Shell。只识别 "Permission denied (" 会让这些失败留下
    // 一个永远不会被撤销的幽灵层，之后文件管理器就会指向一台用户从未真正进入的服务器。
    //
    // "permission denied (" 用括号限定是有意的：带逗号的 "please try again" 仍可重试，
    // 不能据此撤销目标。
    static const char *const failureMarkers[] = {
        "permission denied (",
        "could not resolve hostname",
        "connection refused",
        "connection closed by",
        "connection reset by",
        "no route to host",
        "connection timed out",
        "operation timed out",
        "network is unreachable",
        "host key verification failed",
        "remote host identification has changed",
        "no matching host key type",
        "too many authentication failures",
    };
    bool hasFailure = false;
    for (const char *marker : failureMarkers) {
        if (normalizedPrompt.contains(marker)) {
            hasFailure = true;
            break;
        }
    }
    if (hasFailure && !m_authenticationFailureReported) {
        m_authenticationFailureReported = true;
        emit nestedSshFailed();
    }

    // 必须在 feed() 之前采样：feed 之后文档已经变长，再判断"是否在底部"得到的是新文档的
    // 结论，用户原本贴底的状态会被误判成停在历史位置。
    const bool wasAtBottom = isScrolledToBottom();
    const int previousValue = verticalScrollBar()->value();
    m_vtermBridge->feed(bytes);
    renderPreservingScroll(wasAtBottom, previousValue);

    // 密码正确后，OpenSSH 只有在认证、会话创建和远端 Shell 启动完成后才会绘制提示符。
    // 必须读取 libvterm 的真实光标行，不能使用 QTextDocument 最后一段：全屏输出或光标移动
    // 可能在光标下方保留一个旧的 "#" 提示符，后者会把尚未输入密码误判成认证成功。
    // 这里只发出候选信号，TerminalPane 还会用认证状态和是否已有密码做二次限定。
    static const QRegularExpression shellPromptPattern(
        QStringLiteral(R"((?:[#>$%]|➜|❯)\s*$)"));
    const QString cursorLine = m_vtermBridge->cursorLineText();
    if (shellPromptPattern.match(cursorLine).hasMatch()) {
        emit shellPromptDetected();
    }

}

void TerminalView::beginLocalLineCapture()
{
    qCDebug(authLog, "beginLocalLineCapture (previous bufLen=%lld)",
            static_cast<long long>(m_localLine.size()));
    m_localCapture = true;
    m_localLine.clear();
    // 密码捕获期间绝不能展示历史命令：候选会遮挡提示，也可能让旁观者看到以前的敏感操作。
    m_commandCompleter->popup()->hide();
}

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    // macOS 下 Command 映射为 Qt::ControlModifier（hasTerminalControlModifier 顶部注释），
    // 因此 Cmd+=/Cmd+-/Cmd+0 在这里表现为 ControlModifier 而非 MetaModifier。缩放不产生
    // 需要发给远端 PTY 的字节，必须在密码本地捕获分支之前拦截，否则会被当作普通字符。
    if (event->modifiers().testFlag(Qt::ControlModifier)
        && !event->modifiers().testFlag(Qt::ShiftModifier)
        && !event->modifiers().testFlag(Qt::AltModifier)) {
        if (event->key() == Qt::Key_Equal || event->key() == Qt::Key_Plus) {
            zoomFont(1);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Minus) {
            zoomFont(-1);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_0) {
            resetZoom();
            event->accept();
            return;
        }
    }

    QByteArray encoded;
    const bool terminalControl = hasTerminalControlModifier(event);

    if (m_localCapture) {
        // 本地捕获期间不向远端发送任何字节，也不回显：这一层的 SSH 会话尚未认证完成，
        // 远端还不存在；而密码本来就不该出现在屏幕上。
        // 只记录长度和键码，绝不记录字符内容——诊断日志不能成为密码泄漏渠道。
        qCDebug(authLog, "capture key=0x%x mods=0x%x textLen=%lld bufLen=%lld",
                event->key(), static_cast<unsigned>(event->modifiers()),
                static_cast<long long>(event->text().size()),
                static_cast<long long>(m_localLine.size()));
        if (event->isAutoRepeat()) {
            // 自动重复事件必须整体忽略。用户敲 ssh 命令时按住回车稍久，重复的回车会恰好
            // 落在密码捕获刚开启的瞬间，提交一个空行——现象是"提示一出现就立刻失败"。
            // QTest 不产生自动重复，因此这条路径无法用合成按键覆盖。
            event->accept();
            return;
        }
        if (event->matches(QKeySequence::Paste)) {
            // 密码通常是从密码管理器粘贴进来的，必须在这里显式处理：macOS 上 Cmd+V 携带
            // Qt::ControlModifier，会被下面的"追加普通字符"分支排除，进而被整个吞掉；
            // 用户随后按回车就提交了空密码。现象是"粘贴密码永远连不上，手打就正常"。
            QString pasted = QGuiApplication::clipboard()->text();
            // 剔除换行和其他控制字符：密码管理器常带尾随换行，而控制字符对密码没有意义。
            // 刻意不把换行当作提交——尾随换行不该在用户还想继续输入时静默提交。
            QString sanitized;
            sanitized.reserve(pasted.size());
            for (const QChar character : std::as_const(pasted)) {
                if (character >= QChar(0x20) && character != QChar(0x7f)) {
                    sanitized.append(character);
                }
            }
            m_localLine.append(sanitized);
        } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            const QString line = m_localLine;
            m_localLine.clear();
            m_localCapture = false;
            appendTerminalOutput(QByteArrayLiteral("\r\n"));
            emit localLineSubmitted(line, false);
        } else if (event->key() == Qt::Key_Backspace) {
            m_localLine.chop(1);
        } else if (terminalControl && event->key() == Qt::Key_C) {
            m_localLine.clear();
            m_localCapture = false;
            appendTerminalOutput(QByteArrayLiteral("\r\n"));
            emit localLineSubmitted(QString(), true);
        } else if (!event->text().isEmpty()
                   && !event->modifiers().testFlag(Qt::ControlModifier)
                   && !event->modifiers().testFlag(Qt::MetaModifier)) {
            m_localLine.append(event->text());
        }
        event->accept();
        return;
    }

    if (m_commandCompleter->popup()->isVisible()) {
        if (event->key() == Qt::Key_Escape) {
            m_commandCompleter->popup()->hide();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Return
            || event->key() == Qt::Key_Enter) {
            const QModelIndex current = m_commandCompleter->popup()->currentIndex();
            if (current.isValid()) {
                acceptCompletion(current.data(Qt::DisplayRole).toString());
                event->accept();
                return;
            }
        }
    }

    if (event->matches(QKeySequence::Paste)) {
        const PastePreparation prepared = prepareClipboardPaste(
            QGuiApplication::clipboard()->text());
        if (prepared.text.isEmpty()) {
            // 剪贴板为空或仅含危险控制字符时不向 PTY 发送任何内容，避免一次“空粘贴”
            // 被下游交互式程序误解为特殊输入。
            event->accept();
            return;
        }

        if (prepared.requiresConfirmation()) {
            QStringList riskDescriptions;
            if (prepared.containsLineBreak) {
                riskDescriptions.append(tr("内容包含 %1 行，发送后可能立即执行多条命令。")
                                            .arg(prepared.text.count(QLatin1Char('\n')) + 1));
            }
            if (prepared.removedControlCharacters > 0) {
                riskDescriptions.append(tr("已移除 %1 个可能注入终端控制序列的不可见字符。")
                                            .arg(prepared.removedControlCharacters));
            }
            riskDescriptions.append(tr("确认将清理后的内容发送到当前终端吗？"));
            const QMessageBox::StandardButton answer = QMessageBox::warning(
                this,
                tr("确认终端粘贴"),
                riskDescriptions.join(QLatin1Char('\n')),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel);
            if (answer != QMessageBox::Yes) {
                event->accept();
                return;
            }
        }

        // 多行能力仍然保留，但只有用户看见风险并明确确认后才会进入 PTY。命令缓冲只保留
        // 最后一个尚未换行的片段；已经包含换行的部分已由 PTY 提交，不能再次上报。
        encoded = prepared.text.toUtf8();
        m_currentCommand.append(prepared.text.section(QLatin1Char('\n'), -1));
    } else if (event->matches(QKeySequence::Copy)) {
        copy();
        event->accept();
        return;
    } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        encoded = QByteArrayLiteral("\r");
        const QString submittedCommand = recoverEchoedSshCommand(
            m_currentCommand, m_vtermBridge->cursorLineText());
        m_currentCommand.clear();
        m_commandCompleter->popup()->hide();
        // 提交密码或普通命令后允许识别下一次认证重试提示。
        m_authenticationPromptReported = false;
        m_authenticationFailureReported = false;
        m_authenticationPromptBuffer.clear();

        // 回车必须先进入 PTY 输入队列，再通知上层解析命令或复用密码。此前相反的顺序会
        // 让 SFTP 在内层 ssh 尚未收到密码回车时抢先创建连接，表现为终端已经登录但文件
        // 管理器仍停留在外层服务器。此分支自行发送并提前返回，避免函数末尾重复发送。
        if (event->modifiers().testFlag(Qt::AltModifier)) {
            encoded.prepend('\x1b');
        }
        emit inputBytes(encoded);
        emit commandSubmitted(submittedCommand);
        event->accept();
        return;
    } else if (event->key() == Qt::Key_Tab && !event->modifiers().testFlag(Qt::ShiftModifier)) {
        // ASCII HT(0x09) 由远端 readline/zle 解释，真正的候选生成仍在服务器 Shell 中完成。
        encoded = QByteArrayLiteral("\t");
    } else if (event->key() == Qt::Key_Backtab
               || (event->key() == Qt::Key_Tab
                   && event->modifiers().testFlag(Qt::ShiftModifier))) {
        // xterm 使用 CSI Z 表示反向 Tab；zsh 菜单补全和部分 TUI 会使用该序列。
        encoded = QByteArrayLiteral("\x1b[Z");
    } else if (event->key() == Qt::Key_Backspace) {
        encoded = QByteArray(1, '\x7f');
        if (!m_currentCommand.isEmpty()) {
            m_currentCommand.chop(1);
        }
    } else if (event->key() == Qt::Key_Up) {
        encoded = QByteArrayLiteral("\x1b[A");
    } else if (event->key() == Qt::Key_Down) {
        encoded = QByteArrayLiteral("\x1b[B");
    } else if (event->key() == Qt::Key_Right) {
        encoded = QByteArrayLiteral("\x1b[C");
    } else if (event->key() == Qt::Key_Left) {
        encoded = QByteArrayLiteral("\x1b[D");
    } else if (event->key() == Qt::Key_Home) {
        encoded = QByteArrayLiteral("\x1b[H");
    } else if (event->key() == Qt::Key_End) {
        encoded = QByteArrayLiteral("\x1b[F");
    } else if (event->key() == Qt::Key_Delete) {
        encoded = QByteArrayLiteral("\x1b[3~");
    } else if (terminalControl
               && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) {
        // ASCII 控制字符使用字母序号 1..26；这覆盖 Ctrl+C、Ctrl+D、Ctrl+Z 等 Shell 操作。
        encoded = QByteArray(1, static_cast<char>(event->key() - Qt::Key_A + 1));
        if (event->key() == Qt::Key_D) {
            // Ctrl-D 只有在当前输入行为空时才是 EOF；行中是 delete-char-forward，而在
            // cat/python/mysql 里退出的也是那个程序而非 Shell。误判会弹掉一层真实存在的
            // 服务器，导致文件管理器少一层。
            if (m_currentCommand.isEmpty()) {
                emit commandSubmitted(QStringLiteral("Ctrl-D"));
            }
            m_currentCommand.clear();
        } else if (event->key() == Qt::Key_C) {
            // Ctrl+C 会让正在连接的 ssh 直接退出。TerminalPane 仅在确实有未确认的嵌套
            // 目标时才据此回滚，普通命令的中断不会影响连接链。
            emit nestedSshAborted();
            m_currentCommand.clear();
            m_commandCompleter->popup()->hide();
        }
    } else if (!event->text().isEmpty()
               && !event->modifiers().testFlag(Qt::ControlModifier)
               && !event->modifiers().testFlag(Qt::MetaModifier)) {
        encoded = event->text().toUtf8();
        m_currentCommand.append(event->text());
    } else {
        // 复制、查找等标准只读文本操作仍交给 QPlainTextEdit 处理。
        QPlainTextEdit::keyPressEvent(event);
        return;
    }

    if (event->modifiers().testFlag(Qt::AltModifier) && !encoded.startsWith('\x1b')) {
        encoded.prepend('\x1b');
    }
    emit inputBytes(encoded);
    // 普通字符、粘贴和退格会改变前缀；方向键等不会改变 m_currentCommand，重复计算只涉及
    // 很小的内存模型，可换来所有输入路径的一致行为并避免漏掉输入法或未来新增按键分支。
    updateCompletionPopup();
    event->accept();
}

void TerminalView::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    // viewport 排除了滚动条和边框，得到的行列数比直接使用 QWidget::size 更接近实际绘制区域。
    recalculateTerminalGeometry();
    renderPreservingScroll();
}

void TerminalView::paintEvent(QPaintEvent *event)
{
    QPlainTextEdit::paintEvent(event);
    if (!m_cursorPhaseVisible
        || static_cast<TerminalCursorShape>(m_cursorShape) != TerminalCursorShape::IBeam) {
        return;
    }
    const int position = m_vtermBridge->renderedCursorPosition();
    if (position < 0 || position >= document()->characterCount()) {
        return;
    }

    QTextCursor cursor(document());
    cursor.setPosition(position);
    const QRect cursorArea = cursorRect(cursor);
    QPainter painter(viewport());
    painter.setPen(QPen(m_cursorColor, 2.0));
    painter.drawLine(cursorArea.topLeft(), cursorArea.bottomLeft());
}

bool TerminalView::focusNextPrevChild(const bool next)
{
    Q_UNUSED(next)
    // 返回 false 后 Qt 会继续把按键交给 TerminalView::keyPressEvent；该行为仅限终端控件，
    // 主窗口其他输入框仍保留标准 Tab 焦点导航。
    return false;
}

void TerminalView::inputMethodEvent(QInputMethodEvent *event)
{
    const QString committedText = event->commitString();
    qCDebug(authLog, "ime commitLen=%lld preeditLen=%lld capture=%d",
            static_cast<long long>(committedText.size()),
            static_cast<long long>(event->preeditString().size()),
            int(m_localCapture));
    if (m_localCapture) {
        // 输入法提交的字符同样只进本地缓冲；密码可能包含通过输入法输入的字符。
        for (const QChar character : committedText) {
            if (character == QLatin1Char('\r') || character == QLatin1Char('\n')) {
                const QString line = m_localLine;
                m_localLine.clear();
                m_localCapture = false;
                appendTerminalOutput(QByteArrayLiteral("\r\n"));
                emit localLineSubmitted(line, false);
                break;
            }
            m_localLine.append(character);
        }
        event->accept();
        return;
    }
    if (!committedText.isEmpty()) {
        emit inputBytes(committedText.toUtf8());
        // 输入法一次可能提交多个字符，因此按字符维护命令缓冲；换行代表提交当前命令。
        for (const QChar character : committedText) {
            if (character == QLatin1Char('\r') || character == QLatin1Char('\n')) {
                emit commandSubmitted(m_currentCommand);
                m_currentCommand.clear();
                // 输入法直接提交换行时同样要开启下一轮认证提示检测；否则第二次密码提示
                // 会因为第一次的 reported 状态而被忽略。
                m_authenticationPromptReported = false;
                m_authenticationFailureReported = false;
                m_authenticationPromptBuffer.clear();
            } else {
                m_currentCommand.append(character);
            }
        }
        updateCompletionPopup();
    }
    event->accept();
}

QVariant TerminalView::inputMethodQuery(const Qt::InputMethodQuery query) const
{
    if (query == Qt::ImEnabled) {
        return true;
    }
    if (query == Qt::ImCursorRectangle) {
        // 终端输出光标位于文档末尾；候选窗口应靠近最后一行而不是固定在窗口左上角。
        QTextCursor cursor(document());
        cursor.movePosition(QTextCursor::End);
        return cursorRect(cursor);
    }
    return QPlainTextEdit::inputMethodQuery(query);
}
