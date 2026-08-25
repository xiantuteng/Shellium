#include "terminal/TerminalView.h"

#include "settings/TerminalSettings.h"
#include "terminal/VtermBridge.h"
#include "ui/UiIcons.h"

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
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QKeySequence>
#include <QListView>
#include <QMenu>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QPainter>
#include <QPaintEvent>
#include <QAbstractItemView>
#include <QGlyphRun>
#include <QMimeData>
#include <QPlainTextDocumentLayout>
#include <QRawFont>
#include <QStringListModel>
#include <QTextBlock>
#include <QTextFragment>
#include <QUrl>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>

#include <limits>

#include <algorithm>
#include <cmath>
#include <optional>

namespace {

/**
 * @brief 连续分隔条拖动停止后，等待多少毫秒再提交最终 PTY 尺寸。
 *
 * 80ms 足以合并常见 60/120Hz 鼠标拖动事件，同时结束拖动后的主观延迟仍低于一次普通
 * 网络往返。使用单次防抖而非固定频率节流，是为了不向远端发送任何已经过期的中间尺寸。
 */
constexpr int kTerminalResizeDebounceMilliseconds = 80;

/**
 * @brief 渲染合帧间隔（毫秒），约等于 60fps 一帧。
 *
 * 远端一次 `cat 大文件` 会被切成成百上千个网络分片。每个分片都同步重绘，代价不是"多画
 * 几次"而是把文档编辑、重排和重绘全部乘以分片数，界面因此被自己的输出压死。攒够一帧再
 * 画既保证了肉眼流畅度，也让事件循环在两帧之间有机会处理用户的按键与滚动。
 */
constexpr int kRenderCoalesceMilliseconds = 16;

/**
 * @brief 命令提示弹层与当前终端行之间保留的逻辑像素间距。
 *
 * QCompleter 默认紧贴传入矩形的底边放置。终端光标矩形有时只覆盖字体的插入符高度，低于
 * 实际行框；额外留出这段空隙可以吸收字体上下缘和高 DPI 取整误差，避免弹层压住正在输入
 * 的命令，同时又不会浪费明显的垂直空间。
 */
constexpr int kCompletionPopupVerticalGap = 4;

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
 * @brief 判断按键是否是“把终端选区复制到剪贴板”的快捷键。
 *
 * 终端不能沿用普通文本框的单一约定：Ctrl+C 在终端里必须作为 SIGINT 发给远端，因此
 * Windows 同时支持 Ctrl+Shift+C 和 Ctrl+Insert，Linux 保留 Ctrl+Insert，macOS 使用
 * Command+C。任何平台都不能把裸 Ctrl+C 截获为复制，因为它必须继续向终端发送 SIGINT。
 */
bool isClipboardCopyShortcut(const QKeyEvent *event)
{
    if (event->key() == Qt::Key_Insert) {
        return event->modifiers().testFlag(Qt::ControlModifier)
            && !event->modifiers().testFlag(Qt::ShiftModifier);
    }
#if defined(Q_OS_WIN)
    if (event->key() == Qt::Key_C) {
        const Qt::KeyboardModifiers modifiers = event->modifiers();
        return modifiers.testFlag(Qt::ControlModifier)
            && modifiers.testFlag(Qt::ShiftModifier)
            && !modifiers.testFlag(Qt::AltModifier)
            && !modifiers.testFlag(Qt::MetaModifier);
    }
#endif
#if defined(Q_OS_MACOS)
    return event->matches(QKeySequence::Copy);
#else
    // 非 macOS 不能用 QKeySequence::Copy：它在这些平台等于 Ctrl+C，会把中断前台程序的
    // 唯一按键变成复制，`while true` 之类的循环再也停不下来。
    return false;
#endif
}

/**
 * @brief 判断按键是否是“把剪贴板文本粘贴进终端”的快捷键。
 *
 * 与复制规则对称：Windows 增加 Ctrl+Shift+V，Linux 用 Shift+Insert，macOS 用 Command+V。
 */
bool isClipboardPasteShortcut(const QKeyEvent *event)
{
    if (event->key() == Qt::Key_Insert) {
        return event->modifiers().testFlag(Qt::ShiftModifier)
            && !event->modifiers().testFlag(Qt::ControlModifier);
    }
#if defined(Q_OS_WIN)
    if (event->key() == Qt::Key_V) {
        const Qt::KeyboardModifiers modifiers = event->modifiers();
        return modifiers.testFlag(Qt::ControlModifier)
            && modifiers.testFlag(Qt::ShiftModifier)
            && !modifiers.testFlag(Qt::AltModifier)
            && !modifiers.testFlag(Qt::MetaModifier);
    }
#endif
#if defined(Q_OS_MACOS)
    return event->matches(QKeySequence::Paste);
#else
    // 与复制同理：Ctrl+V 在 Shell 里是 literal-next（bash/zsh 的 quoted-insert），
    // 拦下来当粘贴会让用户无法输入原始控制字符。
    return false;
#endif
}

/**
 * @brief 把 Qt 修饰键映射为 libvterm 的修饰掩码。
 *
 * 只翻译 Shift/Control/Alt 三种终端协议认识的修饰。macOS 的 Command 不参与：它是应用级
 * 快捷键，不该出现在发往 PTY 的字节里。
 */
VTermModifier toVtermModifiers(const QKeyEvent *event)
{
    unsigned int modifiers = VTERM_MOD_NONE;
    if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        modifiers |= VTERM_MOD_SHIFT;
    }
    if (hasTerminalControlModifier(event)) {
        modifiers |= VTERM_MOD_CTRL;
    }
    if (event->modifiers().testFlag(Qt::AltModifier)) {
        modifiers |= VTERM_MOD_ALT;
    }
    return static_cast<VTermModifier>(modifiers);
}

/**
 * @brief 把 Qt 键码映射为 libvterm 键枚举。
 * @return 该键需要由 libvterm 按当前终端模式编码时返回对应枚举，否则返回 std::nullopt。
 *
 * 这些键的字节形式不是固定的：DECCKM（应用光标键）开启后方向键从 `CSI A` 变成 `SS3 A`，
 * 而 vim、less、htop 进入全屏时都会开启它。硬编码 CSI 形式会让 vim 在插入模式下把方向键
 * 读成 ESC 加一个普通字母——按上箭头不是移动光标，而是退出插入模式并执行一条编辑命令。
 */
std::optional<VTermKey> terminalNavigationKey(const int key)
{
    switch (key) {
    case Qt::Key_Up: return VTERM_KEY_UP;
    case Qt::Key_Down: return VTERM_KEY_DOWN;
    case Qt::Key_Left: return VTERM_KEY_LEFT;
    case Qt::Key_Right: return VTERM_KEY_RIGHT;
    case Qt::Key_Home: return VTERM_KEY_HOME;
    case Qt::Key_End: return VTERM_KEY_END;
    case Qt::Key_PageUp: return VTERM_KEY_PAGEUP;
    case Qt::Key_PageDown: return VTERM_KEY_PAGEDOWN;
    case Qt::Key_Insert: return VTERM_KEY_INS;
    case Qt::Key_Delete: return VTERM_KEY_DEL;
    default:
        break;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) {
        return static_cast<VTermKey>(VTERM_KEY_FUNCTION_0 + 1 + (key - Qt::Key_F1));
    }
    return std::nullopt;
}

/**
 * @brief 返回 Ctrl 与非字母键组合对应的 ASCII 控制码。
 * @return 该组合有标准控制码时返回 0x00~0x1f 的字符，否则返回 std::nullopt。
 *
 * Ctrl+[ 就是 ESC，是 vim 用户退出插入模式最常用的按法之一；Ctrl+\ 和 Ctrl+_ 分别是
 * 退出信号和 readline 撤销。只处理 A~Z 会让这些键被整个吞掉，用户按下去毫无反应。
 */
std::optional<char> controlCodeForNonLetterKey(const int key)
{
    switch (key) {
    case Qt::Key_At:
    case Qt::Key_2: return '\x00';        // Ctrl+@ / Ctrl+2 → NUL
    case Qt::Key_BracketLeft: return '\x1b';   // Ctrl+[ → ESC
    case Qt::Key_Backslash: return '\x1c';     // Ctrl+\ → FS
    case Qt::Key_BracketRight: return '\x1d';  // Ctrl+] → GS
    case Qt::Key_AsciiCircum:
    case Qt::Key_6: return '\x1e';        // Ctrl+^ / Ctrl+6 → RS
    case Qt::Key_Underscore:
    case Qt::Key_Minus: return '\x1f';    // Ctrl+_ / Ctrl+- → US
    case Qt::Key_Space: return '\x00';    // Ctrl+Space → NUL
    default:
        return std::nullopt;
    }
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
    , m_resizeDebounceTimer(new QTimer(this))
    , m_renderCoalesceTimer(new QTimer(this))
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
    // 允许从系统文件管理器把本地文件拖进终端，触发"上传到当前目录"确认——见
    // dragEnterEvent/dropEvent。QPlainTextEdit 是 QAbstractScrollArea：真正接收操作系统
    // 拖拽事件、也是屏幕上可见区域的是 viewport() 这个子控件，不是本控件自身。只设置
    // 本控件的 acceptDrops 时，macOS 的原生视图从未注册接受拖拽类型，文件拖进来会直接
    // 显示"禁止"光标、Qt 完全收不到事件——RemoteFileManagerWidget 里的文件树能正常接收
    // 拖拽，是因为 QAbstractItemView::setDragDropMode 内部已经调用了这一行。
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
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
    connect(m_vtermBridge, &VtermBridge::alternateScreenChanged, this, [this](const bool active) {
        // 全屏程序接管屏幕后就不存在"当前正在输入的命令"了：vim 里敲的 i、d、w 都是编辑
        // 命令。继续累积前缀会弹出历史补全浮层，而浮层随后会抢走 Tab、回车和 Esc——那正是
        // vim 最关键的三个键，表现为"进了 vim 就没法编辑"。
        m_currentCommand.clear();
        m_commandCompleter->popup()->hide();
        // 备用屏没有可回溯的历史，滚动条只会误导用户去滚动一块固定画布。
        setVerticalScrollBarPolicy(active ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
        renderPreservingScroll();
        viewport()->update();
    });

    // 补全只做前缀匹配并保持模型传入的最近使用顺序。UnsortedModel 可以避免 QCompleter
    // 为二分查找重新排序，从而把多年未用但字典序靠前的命令放到最近命令之前。
    m_commandCompleter->setWidget(this);
    m_commandCompleter->setModel(m_completionModel);
    m_commandCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_commandCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_commandCompleter->setModelSorting(QCompleter::UnsortedModel);
    m_commandCompleter->setFilterMode(Qt::MatchStartsWith);
    m_commandCompleter->setMaxVisibleItems(12);
    // 补全弹层是独立的顶层 QListView，不会继承 MainWindow/TerminalView 的样式表，
    // 需要单独硬编码一份深色配色（参考 Termius 的浮层观感），避免沿用系统默认的
    // 陈旧方框样式。
    QAbstractItemView *completionPopup = m_commandCompleter->popup();
    completionPopup->setObjectName(QStringLiteral("CommandCompletionPopup"));
    if (auto *completionList = qobject_cast<QListView *>(completionPopup)) {
        // QCompleter 的默认弹层当前是 QListView。候选都使用同一种字体和单行文本，固定统一
        // 行尺寸可避免每次滚动都重新计算高度；spacing 归零则把空间留给文字而不是空白。
        completionList->setUniformItemSizes(true);
        completionList->setSpacing(0);
    }
    completionPopup->setStyleSheet(QStringLiteral(
        "QListView#CommandCompletionPopup {"
        "    background-color: #1c2333;"
        "    color: #e4e8f5;"
        "    border: 1px solid #2e3650;"
        "    border-radius: 8px;"
        "    padding: 2px;"
        "    outline: 0;"
        "}"
        "QListView#CommandCompletionPopup::item {"
        // 主窗口通用列表样式带有 28px 最小行高；必须在局部显式覆盖，否则即使减小 padding，
        // 候选仍会保持稀疏。1px 上下留白足以避免字形贴边，并能在同样高度展示更多命令。
        "    min-height: 0px;"
        "    padding: 1px 8px;"
        "    border-radius: 4px;"
        "}"
        "QListView#CommandCompletionPopup::item:hover {"
        "    background-color: #2a3148;"
        "}"
        "QListView#CommandCompletionPopup::item:selected {"
        "    background-color: #39415d;"
        "    color: #ffffff;"
        "}"));
    connect(m_commandCompleter,
            qOverload<const QString &>(&QCompleter::activated),
            this,
            &TerminalView::acceptCompletion);

    m_cursorBlinkTimer->setInterval(600);
    connect(m_cursorBlinkTimer, &QTimer::timeout, this, [this]() {
        m_cursorPhaseVisible = !m_cursorPhaseVisible;
        // 闪烁绝不能触碰 QTextDocument。光标此前被写进单元格的 QTextCharFormat，于是每次
        // 相位翻转都要整体重建文档：一万行历史时单次重建约半秒，而定时器每 600ms 就来一次
        // ——界面等于长期满负荷，用户什么都不做也卡。现在三种形态全部由 paintEvent 叠加，
        // 闪烁只需要让光标所在的那一小块重绘。
        updateCursorArea();
    });

    // 合帧定时器只在"欠一次渲染"时启动，且不会被后续输出续期：持续输出下它以固定帧率
    // 稳定出帧，而不是像防抖那样一直被推迟到输出停下来才画第一帧。
    m_renderCoalesceTimer->setSingleShot(true);
    m_renderCoalesceTimer->setInterval(kRenderCoalesceMilliseconds);
    connect(m_renderCoalesceTimer, &QTimer::timeout, this, [this]() {
        m_lastRenderElapsed.restart();
        renderPreservingScroll();
    });

    // 定时器每次 start() 都会重新计时，因此拖动期间不会提交中间值；只有最后一个
    // resizeEvent 之后安静 80ms，libvterm 与 PTY 才会一起切换到最终字符网格。
    m_resizeDebounceTimer->setSingleShot(true);
    m_resizeDebounceTimer->setInterval(kTerminalResizeDebounceMilliseconds);
    connect(m_resizeDebounceTimer,
            &QTimer::timeout,
            this,
            &TerminalView::commitTerminalGeometry);

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
    UiIcons::modernizePopup(menu);
    menu->addSeparator();
    QAction *uploadAction = menu->addAction(tr("上传到当前远程目录…"));
    connect(uploadAction, &QAction::triggered,
            this, &TerminalView::uploadToCurrentDirectoryRequested);
    return menu;
}

void TerminalView::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    QPlainTextEdit::dragEnterEvent(event);
}

void TerminalView::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    QPlainTextEdit::dragMoveEvent(event);
}

void TerminalView::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasUrls()) {
        QPlainTextEdit::dropEvent(event);
        return;
    }
    QStringList localPaths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            localPaths.append(url.toLocalFile());
        }
    }
    // 即使拖拽整体被接受，也可能全是远程/网络 URL；此时没有本地文件可传，交回默认处理
    // 而不是吞掉事件。
    if (localPaths.isEmpty()) {
        QPlainTextEdit::dropEvent(event);
        return;
    }
    event->acceptProposedAction();
    emit filesDropped(localPaths);
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
    // 补全弹层是独立顶层窗口，不会可靠继承 TerminalView 的字体。显式复制完整 QFont 可让
    // 字体族、字号和字符间距都与终端一致，设置页实时预览及快捷键缩放也不会产生两套字形。
    m_commandCompleter->popup()->setFont(terminalFont);
    m_commandCompletionEnabled = normalized.commandAutoCompletion;
    m_multiLinePasteConfirmation = normalized.multiLinePasteConfirmation;
    if (!m_commandCompletionEnabled) {
        // 设置实时关闭时立即收起已经显示的弹层；仅阻止后续 updateCompletionPopup 不足以
        // 清理当前顶层窗口，会让用户误以为开关没有生效。
        m_commandCompleter->popup()->hide();
    }

    m_basePointSize = normalized.fontPointSize;
    m_lineHeightPercent = normalized.lineHeightPercent;
    m_cursorShape = static_cast<int>(normalized.cursorShape);
    m_cursorColor = terminalPalette.cursor;
    m_cursorPhaseVisible = true;
    m_vtermBridge->setDefaultColors(terminalPalette.foreground, terminalPalette.background);
    m_vtermBridge->setAnsiPalette(terminalPalette.ansi);
    m_vtermBridge->setScrollbackLimit(normalized.scrollbackLines);
    m_vtermBridge->setLineHeightPercent(normalized.lineHeightPercent);
    // 锚点字体依赖当前字体（要取它的 CJK 后备），必须在 setFont() 之后重新求一次。
    m_vtermBridge->setLineAnchors(buildLineAnchors());
    // 字号、字距和默认色都会改变每一段的排版结果，已渲染的段无法复用。
    m_vtermBridge->invalidateDocument();

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
    // 字体或行高变化会改变 PTY 的字符尺寸；主动重新计算，让远端及时收到 SIGWINCH，
    // 而不是必须等用户再拖动一次窗口。
    m_resizeDebounceTimer->stop();
    commitTerminalGeometry();
    if (m_commandCompletionEnabled) {
        // 用户可能在已经输入前缀时从设置页开启功能，立即重新评估可避免必须再敲一个字符。
        updateCompletionPopup();
    }
    viewport()->update();
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
    // 命令候选必须跟随终端的临时缩放；缩放只改运行时字体，不写回设置，但两者视觉契约相同。
    m_commandCompleter->popup()->setFont(terminalFont);
    // 锚点字体跟着字号走：字号变了它的像素高度也变，不同步会让行高锚点和 cellHeight
    // 对不上，网格重新开始溢出或留白。
    m_vtermBridge->setLineAnchors(buildLineAnchors());
    // 字号调整是离散操作，不需要等待窗口拖动防抖；立即同步字符网格可让缩放反馈更直接。
    m_resizeDebounceTimer->stop();
    commitTerminalGeometry();
    updateCompletionPopup();
    viewport()->update();
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

void TerminalView::scrollToOutputTail()
{
    if (m_vtermBridge->isAlternateScreen()) {
        // 备用屏由 renderPreservingScroll 固定贴顶，这里贴底会把第 0 行推出视口。
        return;
    }
    QScrollBar *bar = verticalScrollBar();
    bar->setValue(bar->maximum());
}

void TerminalView::sendUserInput(const QByteArray &bytes)
{
    // 先贴底再发字节：随后的回显与输出经 renderPreservingScroll 重建时，isScrolledToBottom()
    // 已经为真，会继续跟随末尾，用户敲的每个字符都留在视口内。
    scrollToOutputTail();
    emit inputBytes(bytes);
}

void TerminalView::renderPreservingScroll()
{
    renderPreservingScroll(isScrolledToBottom(), verticalScrollBar()->value());
}

void TerminalView::scheduleRender()
{
    if (m_renderCoalesceTimer->isActive()) {
        return;
    }
    // 空闲之后的第一次输出立刻画出来，不进合帧队列：键盘回显、提示符、单行命令结果都属于
    // 这一类，压一帧会让打字变"黏"。只有上一帧刚画完不久（说明正处于持续输出）才排队，
    // 这样合帧只在真正需要限流的时候生效。
    // 这个计时器只跟踪"由输出触发的渲染"。改字号、改配色同样会渲染，但它们不代表远端正在
    // 刷屏；把它们算进来会让紧随其后的第一段输出白白压一帧。
    const qint64 sinceLastRender = m_lastRenderElapsed.isValid()
        ? m_lastRenderElapsed.elapsed()
        : kRenderCoalesceMilliseconds;
    if (sinceLastRender >= kRenderCoalesceMilliseconds) {
        m_lastRenderElapsed.restart();
        renderPreservingScroll();
        return;
    }
    m_renderCoalesceTimer->start(static_cast<int>(kRenderCoalesceMilliseconds - sinceLastRender));
}

void TerminalView::flushPendingRender()
{
    if (m_renderCoalesceTimer->isActive()) {
        renderPreservingScroll();
    }
}

void TerminalView::renderPreservingScroll(const bool followBottom, const int previousValue)
{
    // 已经落到这里就不再欠一次渲染，否则合帧定时器还会多跑一轮空转。
    m_renderCoalesceTimer->stop();

    // 增量同步不会动到滚动历史那些段，QTextCursor 会随文档编辑自行调整，选区天然保留。
    // 但整体重建（改字号、改配色、切备用屏）仍会清空文档，选区必须按偏移救回来——否则
    // 用户刚选中的文本瞬间消失，按下复制键拿到的是空字符串。
    const QTextCursor previousCursor = textCursor();
    const bool hadSelection = previousCursor.hasSelection();
    const int selectionAnchor = previousCursor.anchor();
    const int selectionPosition = previousCursor.position();

    const bool rebuilt = m_vtermBridge->render(document());
    // 首帧之后才量得到真实行高；量到变化就纠正一次网格。
    noteRenderedCellHeight();

    if (rebuilt && hadSelection) {
        // 重建后的文档长度未必和之前一致，夹取到当前长度即可：宁可让选区略微偏移，也不
        // 能让它整个消失。
        const int lastPosition = document()->characterCount() - 1;
        QTextCursor restored(document());
        restored.setPosition(std::clamp(selectionAnchor, 0, lastPosition));
        restored.setPosition(std::clamp(selectionPosition, 0, lastPosition),
                             QTextCursor::KeepAnchor);
        setTextCursor(restored);
    }

    QScrollBar *bar = verticalScrollBar();
    if (m_vtermBridge->isAlternateScreen()) {
        // 备用屏的文档就是整块网格，第 0 行必须落在视口顶部。这里不能沿用主屏的"贴底"
        // 语义：网格哪怕只比可显示段数多一行，贴底也会把第 0 行连同光标推出视口。
        bar->setValue(bar->minimum());
        return;
    }
    bar->setValue(followBottom ? bar->maximum()
                               : std::clamp(previousValue, bar->minimum(), bar->maximum()));
}

namespace {

/** @brief 问 Qt：用这个字体排这段文字时，最终真正画出字形的是哪个字体族。 */
QString rawFontFamilyFor(const QFont &font, const QString &sample)
{
    QTextLayout layout(sample, font);
    layout.beginLayout();
    const QTextLine line = layout.createLine();
    if (line.isValid()) {
        const_cast<QTextLine &>(line).setLineWidth(std::numeric_limits<qreal>::max());
    }
    layout.endLayout();
    const QList<QGlyphRun> runs = layout.glyphRuns();
    return runs.isEmpty() ? QString() : runs.first().rawFont().familyName();
}

} // namespace

QVector<QPair<QChar, QFont>> TerminalView::buildLineAnchors() const
{
    // 终端里可能出现的字体不止等宽正文字体一种：中日韩、emoji 都会让 Qt 静默换用系统
    // 后备字体，而它们比正文字体更高。行高取的是所有字体 ascent 的最大值加 descent 的
    // 最大值，所以必须把这几类字体各锚一个字符，统一后的高度才真的是最大值；只锚 CJK
    // 的话，含 emoji 的行仍会比别人高 1px（PT Mono 15pt 实测 32 与 33 之差）。
    //
    // 锚点字符必须真的存在于目标字体中，否则 Qt 会再次换字体、拿到的就不是想要的度量：
    // U+200B 不在 PingFang 里，用它锚 CJK 会差 1px；U+3000 在，才能和真实中文完全一致。
    // 反过来 emoji 字体里有 U+200B，用它既拿得到度量又不占宽度。
    QVector<QPair<QChar, QFont>> anchors;

    // CJK 锚点刻意用一个真实汉字、并且不改字体族：让它沿着和正文中文完全相同的后备
    // 链去匹配字体，度量才严格一致。显式指定族名反而会挑到另一个字体——实测正文中文
    // 与"指定 PingFang 的 U+3000"之间就差了 1px，足以让最后一行漂移。它由渲染层画成
    // 透明，不会被看见。
    anchors.append({QChar(0x4E2D), font()});

    const QString cjkFamily = rawFontFamilyFor(font(), QStringLiteral("中"));

    const QString emojiFamily = rawFontFamilyFor(font(), QStringLiteral("\U0001F600"));
    if (!emojiFamily.isEmpty() && emojiFamily != cjkFamily) {
        QFont emojiFont = font();
        emojiFont.setFamily(emojiFamily);
        anchors.append({QChar(0x200B), emojiFont});
    }
    return anchors;
}

QString TerminalView::textWithoutLineAnchors(const int from, const int to) const
{
    QString text;
    if (to <= from) {
        return text;
    }
    // 锚点各自成为独立的 fragment（插入时带着专用字符格式），因此按 fragment 取舍即可，
    // 不必逐字符探格式。段与段之间补回换行，与 toPlainText()/selectedText() 的形态一致。
    for (QTextBlock block = document()->findBlock(from);
         block.isValid() && block.position() < to;
         block = block.next()) {
        if (block.position() > from) {
            text += QLatin1Char('\n');
        }
        for (QTextBlock::iterator part = block.begin(); !part.atEnd(); ++part) {
            const QTextFragment fragment = part.fragment();
            if (!fragment.isValid()
                || VtermBridge::isLineAnchorFormat(fragment.charFormat())) {
                continue;
            }
            const int start = std::max(fragment.position(), from);
            const int end = std::min(fragment.position() + fragment.length(), to);
            if (end <= start) {
                continue;
            }
            text += fragment.text().mid(start - fragment.position(), end - start);
        }
    }
    return text;
}

int TerminalView::computeCellHeight() const
{
    // 唯一可信的行高只有一个来源：本控件自己那份文档实际排出来的高度。
    //
    // 走过的弯路记在这里，免得再试一次：
    //   * QFontMetrics::lineSpacing() 是字体的"设计"行距，和实排高度差着 20%
    //     （PT Mono 15pt：设计 23px，实排 27px），按它算必然多报行数、挤掉状态行。
    //   * 另建一个 QTextDocument 当探针也不行：那份文档没有依附到任何窗口，量出来的是
    //     dpr=1 下的结果；Retina 屏上控件自己的文档按 dpr=2 排版，两者能差出 17%，
    //     表现就是"行数算少了、底部空一大块"。
    // 锚点已经保证每一段高度完全相同，所以随便取一段量都是同一个值。
    if (const QTextBlock first = document()->firstBlock(); first.isValid()) {
        if (auto *layout = document()->documentLayout()) {
            const int measured = static_cast<int>(
                std::ceil(layout->blockBoundingRect(first).height()));
            if (measured > 0) {
                return std::max(measured, 1);
            }
        }
    }

    // 还没有任何内容、也就无从测量时的兜底：设计行距只是个下限，首帧渲染之后
    // noteRenderedCellHeight() 会用真实值把网格纠正过来。
    return std::max(QFontMetrics(font()).lineSpacing() * m_lineHeightPercent / 100, 1);
}

void TerminalView::noteRenderedCellHeight()
{
    // 行高只有在真正排过一次版之后才知道，而网格行数又依赖它——先用兜底值渲染一帧，
    // 量到真实值后再纠正一次。锚点保证了行高与内容无关，因此这个过程一定收敛：纠正后
    // 重新渲染量到的还是同一个值，不会来回抖。
    const int measured = computeCellHeight();
    if (measured <= 0 || measured == m_lastMeasuredCellHeight) {
        return;
    }
    const bool firstMeasurement = m_lastMeasuredCellHeight == 0;
    m_lastMeasuredCellHeight = measured;
    if (firstMeasurement && m_terminalRows <= 0) {
        return;
    }
    // 不能在渲染过程中同步重排：commitTerminalGeometry() 会再次渲染。排到事件队列里
    // 等这一帧画完再说。
    QTimer::singleShot(0, this, [this]() { commitTerminalGeometry(); });
}

void TerminalView::recalculateTerminalGeometry()
{
    if (!m_hasReceivedResizeEvent) {
        // 构造函数中的 QWidget 默认大小只是 Qt 的临时值，并不代表标签页最终可用空间。
        // 等待首次 resizeEvent 后再提交，可避免会话先收到一个错误的狭窄字符网格。
        return;
    }
    const QFontMetrics metrics(font());
    const int cellWidth = std::max(metrics.horizontalAdvance(QLatin1Char('M')), 1);
    const int cellHeight = computeCellHeight();
    // 垂直滚动条会在"主屏有滚动历史"和"备用屏没有"之间自动显隐（连同进出备用屏时的
    // AlwaysOff/AsNeeded 切换），每次显隐都会让 viewport 变宽几个像素、蹭过一次字符列宽的
    // 边界。全屏程序刚接管屏幕、或刚把屏幕还给主屏的那一刻,恰好正是这类显隐最密集发生的
    // 时候——多出来的 SIGWINCH 会让 vim/nvi 立即整屏重绘，而"已打开文件"这类只画一次的
    // 提示行（状态栏）在重绘里不会被重新画出。这里始终按"滚动条已预留出空间"来算列数，
    // 使列数与它当前是否可见无关，从根上掐掉这次多余的重算。
    const int scrollbarReserve = verticalScrollBar()->isVisible()
        ? 0
        : verticalScrollBar()->sizeHint().width();
    const int usableWidth = std::max(viewport()->width() - scrollbarReserve, 0);
    const int columns = std::max(usableWidth / cellWidth, 1);
    // 行数必须等于 QPlainTextEdit 真正能完整显示的段数，而不是"视口高度能塞下几行"。
    // 控件在 viewport 上下各留一份 documentMargin，并且只有严格小于视口高度的累计高度才算
    // 一行可见（等于视口高度的那一行会被判定放不下）。按前者计算会比控件多报一行：告诉
    // PTY 的网格比画得出来的多一行，主屏上只是最旧一行被顶掉，看不出来；备用屏下整块画布
    // 被推高一行，vim 的第 0 行连同停在那里的光标一起消失在视口上方。
    const int documentMargin = static_cast<int>(document()->documentMargin());
    const int usableHeight = viewport()->height() - 2 * documentMargin - 1;
    const int rows = std::max(usableHeight / cellHeight, 1);
    if (columns == m_terminalColumns && rows == m_terminalRows) {
        // resizeEvent 以像素为单位触发，同一个字符列宽内可能产生多次事件；这些事件不能
        // 继续向 PTY 传播，否则只会制造无意义的 SIGWINCH 和远端提示符重绘。
        return;
    }
    m_terminalColumns = columns;
    m_terminalRows = rows;
    m_vtermBridge->resize(rows, columns);
    emit terminalSizeChanged(columns, rows);
}

void TerminalView::commitTerminalGeometry()
{
    // libvterm resize 可能回流屏幕或改变滚动历史长度，必须在重排前采样滚动意图，再在
    // 文档整体重建后恢复。这样防抖修复不会引入拖动结束后跳回顶部的新问题。
    const bool followBottom = isScrolledToBottom();
    const int previousValue = verticalScrollBar()->value();
    recalculateTerminalGeometry();
    renderPreservingScroll(followBottom, previousValue);
}

bool TerminalView::findTerminalText(const QString &text, const bool backward)
{
    if (text.isEmpty()) {
        return false;
    }
    // 查找必须看到最新内容：合帧可能还欠着一次渲染，刚刷出来的那一屏还不在文档里。
    flushPendingRender();
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

QMimeData *TerminalView::createMimeDataFromSelection() const
{
    // 复制、右键菜单的"复制"、拖拽全都经由这个虚函数取数据，在这里剔除统一行高用的零宽
    // 锚点字符，三条路径就一次性都干净了——否则粘到编辑器或聊天窗口里会多出看不见却真实
    // 存在的脏字符。
    auto *mime = new QMimeData;
    const QTextCursor selection = textCursor();
    mime->setText(
        textWithoutLineAnchors(selection.selectionStart(), selection.selectionEnd()));
    return mime;
}

QString TerminalView::terminalPlainText() const
{
    // 导出与日志读取的是文档全文，欠着的那一帧必须先落进文档，否则会漏掉最后一屏输出。
    const_cast<TerminalView *>(this)->flushPendingRender();
    // 行高锚点是渲染用的占位字符，不属于终端内容，导出前必须剔除。
    return textWithoutLineAnchors(0, document()->characterCount());
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
    sendUserInput(command.toUtf8() + QByteArrayLiteral("\r"));
    emit commandSubmitted(command);
    m_currentCommand.clear();
    m_commandCompleter->popup()->hide();
}

void TerminalView::insertCommandText(const QString &command)
{
    if (m_localCapture || command.isEmpty()) {
        return;
    }
    // 这里只写命令本身，绝不追加 CR/LF；是否执行完全交由用户在终端中按回车决定。
    sendUserInput(command.toUtf8());
    m_currentCommand += command;
    updateCompletionPopup();
}

QRect TerminalView::visualCursorRect() const
{
    const int position = m_vtermBridge->renderedCursorPosition();
    if (position < 0 || position >= document()->characterCount()) {
        return {};
    }
    QTextCursor cursor(document());
    cursor.setPosition(position);
    return cursorRect(cursor);
}

void TerminalView::updateCompletionPopup()
{
    if (!m_commandCompletionEnabled || m_localCapture || m_vtermBridge->isAlternateScreen()
        || m_currentCommand.size() < 2 || m_completionModel->rowCount() == 0) {
        m_commandCompleter->popup()->hide();
        return;
    }
    m_commandCompleter->setCompletionPrefix(m_currentCommand);
    if (m_commandCompleter->completionCount() <= 0) {
        m_commandCompleter->popup()->hide();
        return;
    }

    // 弹层锚点必须使用 libvterm 换算出的视觉光标位置（与 paintEvent 绘制光标的方式一致），
    // 而不是 QPlainTextEdit 自带的 textCursor()：后者的位置未必跟随远端 PTY 的真实输入位置
    // 同步更新，会导致弹层遮挡正在输入的命令。取不到有效位置时退回原生 cursorRect() 保底。
    QRect popupRect = visualCursorRect();
    if (popupRect.isNull()) {
        popupRect = cursorRect();
    }
    // QCompleter 把弹层放在传入矩形的底边。插入符矩形在部分字体/高 DPI 环境下比终端的
    // 实际文本行矮，直接传入会让弹层上缘切进字形下半部。把锚点扩到真实终端行高后再留一
    // 个小间隙，向下放置时不会挡当前命令；空间不足翻到上方时也以整行顶部为边界。
    popupRect.setHeight(qMax(popupRect.height(), computeCellHeight())
                        + kCompletionPopupVerticalGap);
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
        sendUserInput(suffix.toUtf8());
    }
    m_currentCommand = completion;
    m_commandCompleter->popup()->hide();
}

bool TerminalView::confirmPaste(const PastePreparation &prepared)
{
    QStringList riskDescriptions;
    if (prepared.containsLineBreak) {
        riskDescriptions.append(tr("内容包含 %1 行，发送后可能立即执行多条命令。")
                                    .arg(prepared.text.count(QLatin1Char('\n')) + 1));
    }
    if (prepared.removedControlCharacters > 0) {
        riskDescriptions.append(tr("已移除 %1 个可能注入终端控制序列的不可见字符。")
                                    .arg(prepared.removedControlCharacters));
    }
    riskDescriptions.append(tr("确认将以下内容发送到当前终端吗？"));

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("TerminalPasteConfirmDialog"));
    dialog.setWindowTitle(tr("确认终端粘贴"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *summary = new QLabel(riskDescriptions.join(QLatin1Char('\n')), &dialog);
    summary->setWordWrap(true);
    layout->addWidget(summary);

    // 只有把真正会被发送的文本摆出来，"确认"才是一次有依据的判断而不是盲签。展示的是
    // 清理后的 prepared.text，与随后写入 PTY 的字节完全一致。
    auto *preview = new QPlainTextEdit(prepared.text, &dialog);
    preview->setObjectName(QStringLiteral("TerminalPasteConfirmPreview"));
    preview->setReadOnly(true);
    preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    preview->setFont(font());
    // 不接受焦点是回车能直达默认按钮的前提：只要预览框拿到焦点，回车就变成"在只读文本框里
    // 换行"，用户会看到按下回车毫无反应。滚动仍可用鼠标滚轮与滚动条完成。
    preview->setFocusPolicy(Qt::NoFocus);
    preview->setMinimumHeight(120);
    layout->addWidget(preview, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    QPushButton *pasteButton = buttons->addButton(tr("粘贴"), QDialogButtonBox::AcceptRole);
    pasteButton->setDefault(true);
    pasteButton->setAutoDefault(true);
    pasteButton->setFocus();
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    dialog.resize(560, 320);
    return dialog.exec() == QDialog::Accepted;
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

    m_vtermBridge->feed(bytes);
    // 渲染合帧：`tail -f`、`cat 大文件` 会把输出切成成百上千个网络分片，每片都同步重绘等于
    // 一秒钟做上千次文档编辑和布局，界面就是这样被自己的输出压死的。libvterm 已经吃下全部
    // 字节，屏幕状态是最新的；真正需要保证的只是"用户眼睛能看到的帧率"，因此攒够一帧再画。
    // 滚动意图在真正渲染前采样即可：feed() 只改 libvterm，不动 QTextDocument。
    scheduleRender();

    // 密码正确后，OpenSSH 只有在认证、会话创建和远端 Shell 启动完成后才会绘制提示符。
    // 必须读取 libvterm 的真实光标行，不能使用 QTextDocument 最后一段：全屏输出或光标移动
    // 可能在光标下方保留一个旧的 "#" 提示符，后者会把尚未输入密码误判成认证成功。
    // 这里只发出候选信号，TerminalPane 还会用认证状态和是否已有密码做二次限定。
    // 备用屏里光标行是 vim 的状态行或 htop 的表头，同样可能以 $ 或 % 结尾。把它当成 Shell
    // 提示符会让等待认证的上层误判内层 Shell 已就绪。
    static const QRegularExpression shellPromptPattern(
        QStringLiteral(R"((?:[#>$%]|➜|❯)\s*$)"));
    const QString cursorLine = m_vtermBridge->cursorLineText();
    if (!m_vtermBridge->isAlternateScreen()
        && shellPromptPattern.match(cursorLine).hasMatch()) {
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
        // 本地捕获同样是"用户正在输入"，即使不回显也要把提示行拉回视口：否则用户在
        // 一片历史输出里对着看不见的密码提示打字。
        scrollToOutputTail();
        if (isClipboardPasteShortcut(event)) {
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

    // 备用屏下每一个按键都属于全屏程序自己的键位表，视图不能再把它当作 Shell 命令行的
    // 一部分：既不做历史补全，也不发出会改动连接链的 commandSubmitted / Ctrl-D / Ctrl+C。
    const bool alternateScreen = m_vtermBridge->isAlternateScreen();

    // 方向键、翻页、Insert/Delete 和功能键的字节形式取决于远端当前的 DECCKM/DECPAM 模式，
    // 必须由保存着这些模式的 libvterm 编码；它生成的字节经 protocolResponse 转成 inputBytes。
    // Ctrl+Insert / Shift+Insert 是终端上唯一不与 SIGINT 冲突的复制粘贴键位，必须在
    // Insert 被当作导航键编码成 VTERM_KEY_INS 之前放行，否则它们只会作为普通 Insert
    // 发给远端，用户看到的现象是这两个键完全没有反应。
    const bool clipboardInsert = event->key() == Qt::Key_Insert
        && (isClipboardCopyShortcut(event) || isClipboardPasteShortcut(event));

    if (const std::optional<VTermKey> navigationKey = terminalNavigationKey(event->key());
        navigationKey.has_value() && !clipboardInsert) {
        if (m_commandCompleter->popup()->isVisible()
            && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
            // 补全浮层可见时上下键用于选择候选，交给 Qt 的默认处理。
            QPlainTextEdit::keyPressEvent(event);
            return;
        }
        // 方向键等导航键的字节由 libvterm 生成，不经过 sendUserInput，但同样是用户输入。
        scrollToOutputTail();
        m_vtermBridge->sendKey(*navigationKey, toVtermModifiers(event));
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

    if (isClipboardPasteShortcut(event)) {
        const PastePreparation prepared = prepareClipboardPaste(
            QGuiApplication::clipboard()->text());
        if (prepared.text.isEmpty()) {
            // 剪贴板为空或仅含危险控制字符时不向 PTY 发送任何内容，避免一次“空粘贴”
            // 被下游交互式程序误解为特殊输入。
            event->accept();
            return;
        }

        if (prepared.requiresConfirmation(m_multiLinePasteConfirmation) && !confirmPaste(prepared)) {
            event->accept();
            return;
        }

        // 多行能力仍然保留，但只有用户看见风险并明确确认后才会进入 PTY。命令缓冲只保留
        // 最后一个尚未换行的片段；已经包含换行的部分已由 PTY 提交，不能再次上报。
        encoded = prepared.text.toUtf8();
        if (!alternateScreen) {
            m_currentCommand.append(prepared.text.section(QLatin1Char('\n'), -1));
        }

        // 远端开启括号粘贴时，内容必须夹在 CSI 200~ / CSI 201~ 之间，否则 vim 会把粘贴
        // 当成逐个按键：autoindent 给每一行再叠一层缩进，十行代码粘完成阶梯状。远端未
        // 开启该模式时 libvterm 不会产生任何标记字节，行为与此前完全一致。
        m_vtermBridge->beginPaste();
        sendUserInput(encoded);
        m_vtermBridge->endPaste();
        if (!alternateScreen) {
            updateCompletionPopup();
        }
        event->accept();
        return;
    } else if (isClipboardCopyShortcut(event)) {
        copy();
        event->accept();
        return;
    } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        encoded = QByteArrayLiteral("\r");
        if (alternateScreen) {
            // vim 里的回车是换行或执行 ex 命令，既不是 Shell 命令提交，也不可能开启新的
            // SSH 层。照常上报会把编辑内容写进命令历史，还会让嵌套解析器认错服务器。
            if (event->modifiers().testFlag(Qt::AltModifier)) {
                encoded.prepend('\x1b');
            }
            sendUserInput(encoded);
            event->accept();
            return;
        }
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
        sendUserInput(encoded);
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
    } else if (terminalControl
               && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) {
        // ASCII 控制字符使用字母序号 1..26；这覆盖 Ctrl+C、Ctrl+D、Ctrl+Z 等 Shell 操作。
        encoded = QByteArray(1, static_cast<char>(event->key() - Qt::Key_A + 1));
        if (alternateScreen) {
            // vim 的 Ctrl-D/Ctrl-F 是翻页，htop 的 Ctrl+C 只结束前台程序，都不代表用户
            // 退出了一层 SSH。在备用屏里发出这些信号会凭空弹掉一层真实存在的服务器。
        } else if (event->key() == Qt::Key_D) {
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
    } else if (const std::optional<char> controlCode =
                   terminalControl ? controlCodeForNonLetterKey(event->key()) : std::nullopt;
               controlCode.has_value()) {
        // Ctrl+[ 是 vim 用户退出插入模式最常用的按法；只处理 A~Z 会让它被整个吞掉。
        encoded = QByteArray(1, *controlCode);
    } else if (!event->text().isEmpty()
               && !event->modifiers().testFlag(Qt::ControlModifier)
               && !event->modifiers().testFlag(Qt::MetaModifier)) {
        encoded = event->text().toUtf8();
        if (!alternateScreen) {
            m_currentCommand.append(event->text());
        }
    } else {
        // 复制、查找等标准只读文本操作仍交给 QPlainTextEdit 处理。
        QPlainTextEdit::keyPressEvent(event);
        return;
    }

    if (event->modifiers().testFlag(Qt::AltModifier) && !encoded.startsWith('\x1b')) {
        encoded.prepend('\x1b');
    }
    sendUserInput(encoded);
    // 普通字符、粘贴和退格会改变前缀；重复计算只涉及很小的内存模型，可换来所有输入路径的
    // 一致行为并避免漏掉输入法或未来新增按键分支。备用屏下没有命令行可补全。
    if (!alternateScreen) {
        updateCompletionPopup();
    }
    event->accept();
}

void TerminalView::wheelEvent(QWheelEvent *event)
{
    if (!m_vtermBridge->isAlternateScreen()
        || event->modifiers().testFlag(Qt::ControlModifier)) {
        // Ctrl+滚轮是缩放手势，不能被翻译成方向键送进全屏程序。
        QPlainTextEdit::wheelEvent(event);
        return;
    }

    // 一个标准滚轮档位是 120 个 1/8 度；按 xterm 惯例换算成 3 行。触控板会发出更小的
    // 增量，累加后不足一行时直接吞掉，避免每个像素都发一次方向键。
    m_wheelAccumulator += event->angleDelta().y();
    constexpr int kStepsPerNotch = 3;
    constexpr int kUnitsPerNotch = 120;
    const int lines = m_wheelAccumulator * kStepsPerNotch / kUnitsPerNotch;
    if (lines != 0) {
        m_wheelAccumulator -= lines * kUnitsPerNotch / kStepsPerNotch;
        const VTermKey key = lines > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN;
        for (int step = 0; step < std::abs(lines); ++step) {
            m_vtermBridge->sendKey(key);
        }
    }
    event->accept();
}

void TerminalView::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    m_hasReceivedResizeEvent = true;
    // 不在高频 resizeEvent 中立刻改变字符网格。远端 Shell 对每次 PTY 尺寸变化都可能重绘
    // 当前输入行，而 GUI 仍在继续变化；延迟到拖动稳定后一次提交，可保证重绘序列对应
    // 本地最终网格。QPlainTextEdit 已完成自身布局，等待期间旧网格只会短暂被裁切或留白。
    m_resizeDebounceTimer->start();
}

void TerminalView::paintEvent(QPaintEvent *event)
{
    QPlainTextEdit::paintEvent(event);

    // Qt 的选区调色板会覆盖透明前景色，因此必须在基础文本与选区都画完以后遮住内部锚点。
    // 只在确有选区时创建额外绘制器，避免持续输出的普通场景为每一帧承担无意义开销。
    if (textCursor().hasSelection()) {
        QPainter anchorMaskPainter(viewport());
        paintSelectedLineAnchorMasks(anchorMaskPainter, event->rect());
    }

    // 三种光标形态全部在这里叠加绘制。把光标画进 QTextCharFormat 看似省事，代价却是每次
    // 闪烁都要重建整份文档；改成叠加后，闪烁只是一次局部重绘，与历史长度完全无关。
    if (!m_cursorPhaseVisible || !m_vtermBridge->isCursorVisible()) {
        return;
    }
    const QRect cursorArea = visualCursorRect();
    if (cursorArea.isNull() || !cursorArea.intersects(event->rect())) {
        return;
    }

    // 光标最后绘制，避免它恰好停在行尾时被前面的锚点遮罩覆盖。
    QPainter painter(viewport());
    const auto shape = static_cast<TerminalCursorShape>(m_cursorShape);
    if (shape == TerminalCursorShape::IBeam) {
        painter.setPen(QPen(m_cursorColor, 2.0));
        painter.drawLine(cursorArea.topLeft(), cursorArea.bottomLeft());
        return;
    }

    const QRect cellArea = cursorCellRect(cursorArea);
    if (shape == TerminalCursorShape::Underline) {
        constexpr int kUnderlineThickness = 2;
        painter.fillRect(QRect(cellArea.left(),
                               cellArea.bottom() - kUnderlineThickness + 1,
                               cellArea.width(),
                               kUnderlineThickness),
                         m_cursorColor);
        return;
    }

    // 块状光标反色绘制：色块盖住整格后，必须把该格原来的字符重新画上去，否则光标停在
    // 哪个字符上，哪个字符就看不见了。字符颜色取该格原背景色，深浅主题下都有对比度。
    painter.fillRect(cellArea, m_cursorColor);
    const QString cursorText = m_vtermBridge->renderedCursorText();
    if (!cursorText.trimmed().isEmpty()) {
        const QColor textColor = m_vtermBridge->renderedCursorBackground();
        painter.setPen(textColor.isValid() ? textColor : palette().color(QPalette::Base));
        painter.setFont(font());
        painter.drawText(cellArea, Qt::AlignLeft | Qt::AlignVCenter, cursorText);
    }
}

void TerminalView::paintSelectedLineAnchorMasks(QPainter &painter,
                                                 const QRect &repaintArea) const
{
    const QTextCursor selection = textCursor();
    if (!selection.hasSelection()) {
        return;
    }

    const int selectionStart = selection.selectionStart();
    const int selectionEnd = selection.selectionEnd();
    const QPalette::ColorGroup colorGroup = !isEnabled()
        ? QPalette::Disabled
        : (hasFocus() ? QPalette::Active : QPalette::Inactive);
    const QBrush selectionBackground = palette().brush(colorGroup, QPalette::Highlight);

    // 只扫描当前可见段，避免用户全选数十万行历史后每次光标闪烁都遍历整份文档。段落的
    // 几何位置已经包含滚动偏移，可同时用于跳过本次脏区之外的可见段。
    for (QTextBlock block = firstVisibleBlock(); block.isValid(); block = block.next()) {
        const QRectF blockArea = blockBoundingGeometry(block).translated(contentOffset());
        if (blockArea.top() > repaintArea.bottom()) {
            break;
        }
        if (blockArea.bottom() < repaintArea.top()) {
            continue;
        }

        // 从段尾按格式属性反向寻找连续锚点。不能按“中”字或透明色判断：用户输出可能合法
        // 包含相同字符与 ANSI 透明色，只有桥接层写入的专用属性才具有明确业务语义。
        int firstAnchorOffset = block.text().size();
        while (firstAnchorOffset > 0) {
            QTextCursor probe(document());
            probe.setPosition(block.position() + firstAnchorOffset - 1);
            probe.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
            if (!VtermBridge::isLineAnchorFormat(probe.charFormat())) {
                break;
            }
            --firstAnchorOffset;
        }
        if (firstAnchorOffset == block.text().size()) {
            continue;
        }

        const int anchorStart = block.position() + firstAnchorOffset;
        const int anchorEnd = block.position() + block.text().size();
        const int coveredStart = std::max(anchorStart, selectionStart);
        const int coveredEnd = std::min(anchorEnd, selectionEnd);
        if (coveredStart >= coveredEnd) {
            continue;
        }

        QTextCursor startCursor(document());
        startCursor.setPosition(coveredStart);
        QTextCursor endCursor(document());
        endCursor.setPosition(coveredEnd);
        const QRect startCaret = cursorRect(startCursor);
        const QRect endCaret = cursorRect(endCursor);
        const int left = std::min(startCaret.left(), endCaret.left());
        const int right = std::max(startCaret.left(), endCaret.left());
        if (right <= left) {
            // 只选中 U+200B 之类零宽锚点时没有任何像素需要处理。
            continue;
        }

        const QRect maskArea(left, startCaret.top(), right - left, startCaret.height());
        if (maskArea.intersects(repaintArea)) {
            painter.fillRect(maskArea, selectionBackground);
        }
    }
}

QRect TerminalView::cursorCellRect(const QRect &caretRect) const
{
    // cursorRect() 给的是插入符：宽度只有一两个像素，无法表达"一整个单元格"。块状与下划线
    // 光标都需要真实格宽，双宽字符还要占满两格，因此按该格实际字符的字宽换算。
    const QFontMetrics metrics(font());
    const QString cursorText = m_vtermBridge->renderedCursorText();
    const int cellWidth = std::max(
        cursorText.trimmed().isEmpty() ? metrics.horizontalAdvance(QLatin1Char('M'))
                                       : metrics.horizontalAdvance(cursorText),
        1);
    return QRect(caretRect.left(), caretRect.top(), cellWidth, caretRect.height());
}

void TerminalView::updateCursorArea()
{
    const QRect cursorArea = visualCursorRect();
    if (cursorArea.isNull()) {
        // 光标不在已渲染区域（例如远端隐藏了光标）时无处可重绘，整块视口刷新也没有意义。
        return;
    }
    // 只失效光标所在的一小块：一次闪烁的代价从"重建上万段文档"降到"重画一个字符"。
    // 外扩两像素覆盖抗锯齿边缘与两像素粗的竖线。
    viewport()->update(cursorCellRect(cursorArea).adjusted(-2, -2, 2, 2));
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
        scrollToOutputTail();
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
        sendUserInput(committedText.toUtf8());
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
