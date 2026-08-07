#include "terminal/TerminalView.h"

#include "terminal/VtermBridge.h"

#include <QApplication>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QScrollBar>
#include <QClipboard>
#include <QFontMetrics>
#include <QKeySequence>
#include <QTextCursor>

#include <algorithm>

namespace {

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
{
    setReadOnly(true);
    // macOS 会根据该属性决定是否把字符交给输入法。只读文本控件默认关闭此属性，但终端
    // 虽然不直接编辑 QTextDocument，仍然必须接收普通英文和中文输入法的提交事件。
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setUndoRedoEnabled(false);
    setLineWrapMode(QPlainTextEdit::NoWrap);
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
    connect(m_vtermBridge, &VtermBridge::bell, this, []() {
        QApplication::beep();
    });
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

    // OpenSSH 的最终失败使用括号列出已尝试认证方法；带逗号的 "please try again" 仍是
    // 可重试提示，不能在此撤销嵌套目标。尾缓冲可覆盖错误信息被网络拆包的情况。
    const bool hasFinalAuthenticationFailure =
        normalizedPrompt.contains("permission denied (");
    if (hasFinalAuthenticationFailure && !m_authenticationFailureReported) {
        m_authenticationFailureReported = true;
        emit authenticationFailed();
    }

    QScrollBar *bar = verticalScrollBar();
    const bool wasNearBottom = bar->maximum() - bar->value() < 80;
    m_vtermBridge->feed(bytes);
    m_vtermBridge->render(document());

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

    // 仅当用户原本就在底部附近时自动跟随输出，避免阅读旧日志时被强制拉回底部。
    if (wasNearBottom) {
        bar->setValue(bar->maximum());
    }
}

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    QByteArray encoded;
    const bool terminalControl = hasTerminalControlModifier(event);

    if (event->matches(QKeySequence::Paste)) {
        // 只发送纯文本并移除 NUL。NUL 在交互式 Shell 中没有可见意义，却可能让下游程序
        // 误判字符串终止；其余换行和控制字符仍保留，方便粘贴多行脚本和终端快捷操作。
        QString clipboardText = QGuiApplication::clipboard()->text();
        clipboardText.remove(QChar::Null);
        encoded = clipboardText.toUtf8();
        m_currentCommand.append(clipboardText.section(QLatin1Char('\n'), -1));
    } else if (event->matches(QKeySequence::Copy)) {
        copy();
        event->accept();
        return;
    } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        encoded = QByteArrayLiteral("\r");
        const QString submittedCommand = recoverEchoedSshCommand(
            m_currentCommand, m_vtermBridge->cursorLineText());
        m_currentCommand.clear();
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
            emit commandSubmitted(QStringLiteral("Ctrl-D"));
            m_currentCommand.clear();
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
    event->accept();
}

void TerminalView::focusInEvent(QFocusEvent *event)
{
    QPlainTextEdit::focusInEvent(event);
    emit activated();
}

void TerminalView::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    const QFontMetrics metrics(font());
    const int cellWidth = std::max(metrics.horizontalAdvance(QLatin1Char('M')), 1);
    const int cellHeight = std::max(metrics.lineSpacing(), 1);
    // viewport 排除了滚动条和边框，得到的行列数比直接使用 QWidget::size 更接近实际绘制区域。
    const int columns = std::max(viewport()->width() / cellWidth, 1);
    const int rows = std::max(viewport()->height() / cellHeight, 1);
    m_vtermBridge->resize(rows, columns);
    m_vtermBridge->render(document());
    emit terminalSizeChanged(columns, rows);
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
