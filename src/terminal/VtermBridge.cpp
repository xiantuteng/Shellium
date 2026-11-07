#include "terminal/VtermBridge.h"

#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>

#include <algorithm>

namespace {

/**
 * @brief 标记内部行高锚点的 QTextFormat 自定义属性编号。
 *
 * 不能按字符值识别锚点：真实终端内容完全可能以“中”或零宽字符结尾。写入专用属性后，
 * 视图只遮住渲染层生成的锚点，不会误伤用户数据。
 */
constexpr int kLineHeightAnchorProperty = QTextFormat::UserProperty + 1;

/**
 * @brief 解析前景色，并将 `ll` 常用的目录蓝统一为 `#015fff`。
 *
 * 服务器的 LS_COLORS 可以使用 `38;5;27`（解析为 #005fff），也可能直接用
 * `38;2;0;95;255` TrueColor；引擎交来的已经是解析后的 RGB，所以只比较 RGB 即可同时覆盖
 * 两种写法。该色不属于主题可覆盖的 ANSI 0-15 号色，因此需要在这里单独处理。
 */
QColor foregroundQColor(const GhosttyScreen::Rgb &color, const QColor &fallback)
{
    if (!color.valid) {
        return fallback;
    }
    const QColor resolved(color.r, color.g, color.b);
    if (resolved == QColor(QStringLiteral("#005fff"))) {
        return QColor(QStringLiteral("#015fff"));
    }
    return resolved;
}

QColor backgroundQColor(const GhosttyScreen::Rgb &color, const QColor &fallback)
{
    return color.valid ? QColor(color.r, color.g, color.b) : fallback;
}

GhosttyScreen::Rgb toRgb(const QColor &color)
{
    return GhosttyScreen::Rgb{static_cast<std::uint8_t>(color.red()),
                              static_cast<std::uint8_t>(color.green()),
                              static_cast<std::uint8_t>(color.blue()), true};
}

/// 引擎一次最多喂这么多字节再同步历史：库内历史上限（见构造函数）必须大于单次切片可能
/// 滚出的行数，否则最旧的新行会在读取前被库淘汰。每滚出一行至少需要 1 字节。
constexpr qsizetype kFeedSliceBytes = 4096;
constexpr std::size_t kEngineScrollbackLines = 8192;

} // namespace

VtermBridge::VtermBridge(QObject *parent)
    : QObject(parent)
    , m_screen(std::make_unique<GhosttyScreen>(80, 24))
{
    if (!m_screen->isValid()) {
        qFatal("无法创建 libghostty-vt 终端实例");
    }
    // 滚动历史由本类自己的 PackedLine 队列持有（带行/字节双上限）；引擎内的历史只是中转
    // 站，保留 8192 行，足以容纳一个 4 KiB 切片滚出的全部行（见 kFeedSliceBytes）。
    m_screen->setScrollbackMaxLines(kEngineScrollbackLines);
    connectEngineCallbacks();
    applyColors();
}

void VtermBridge::connectEngineCallbacks()
{
    // 引擎回调在 feed() 内同步触发，且不得在回调里再次 feed。
    m_screen->onWritePty = [this](const std::string &bytes) {
        emit protocolResponse(QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())));
    };
    m_screen->onTitleChanged = [this](const std::string &title) {
        emit titleChanged(QString::fromUtf8(title.data(), static_cast<qsizetype>(title.size())));
    };
    m_screen->onBell = [this]() {
        // BEL 只是一个普通字节（0x07），二进制文件里满地都是：`cat` 一个 34MB 的 jar 就含
        // 122873 个。一个字节一次 bell 意味着 GUI 线程要在解析循环里同步响十几万次铃，
        // 界面完全没有机会重绘。真实终端都有这条限流，xterm 的 bellSuppressTime 默认是
        // 200ms，这里取同一个值；抑制窗口内的铃声直接丢弃而不是排队补响。
        constexpr qint64 kBellSuppressMilliseconds = 200;
        if (m_lastBellElapsed.isValid() && m_lastBellElapsed.elapsed() < kBellSuppressMilliseconds) {
            return;
        }
        m_lastBellElapsed.restart();
        emit bell();
    };
    m_screen->onUnknownOsc = [this](const std::string &content) {
        // 引擎只认识标准 OSC；Shell 集成使用私有编号 6773，其余未识别 OSC 一律静默丢弃
        // （引擎不会把它们当文本打印）。content 形如 "6773;<payload>"。
        static const QByteArray prefix = QByteArrayLiteral("6773;");
        const QByteArray bytes(content.data(), static_cast<qsizetype>(content.size()));
        if (!bytes.startsWith(prefix)) {
            return;
        }
        // 单条事件最长也就一行命令；超长载荷只可能来自异常或恶意输出，直接丢弃。
        if (bytes.size() - prefix.size() > 8192) {
            return;
        }
        emit shellIntegrationEvent(bytes.mid(prefix.size()));
    };
}

void VtermBridge::applyColors()
{
    m_screen->setDefaultColors(toRgb(m_defaultForeground), toRgb(m_defaultBackground));
    if (m_ansiPalette.size() == 16) {
        std::vector<GhosttyScreen::Rgb> colors;
        colors.reserve(16);
        for (const QColor &color : std::as_const(m_ansiPalette)) {
            colors.push_back(toRgb(color));
        }
        m_screen->setAnsiPalette(colors);
    }
}

VtermBridge::~VtermBridge() = default;

void VtermBridge::feed(const QByteArray &bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    const QByteArray safe = boundUnterminatedString(bytes);
    // 切片喂入并在每片之后同步滚动历史：引擎没有"行被推入历史"的通知，只能靠取走新行。
    for (qsizetype offset = 0; offset < safe.size(); offset += kFeedSliceBytes) {
        const qsizetype length = std::min(kFeedSliceBytes, safe.size() - offset);
        m_screen->feed(safe.constData() + offset, static_cast<std::size_t>(length));
        refreshEngineState();
    }
}

void VtermBridge::refreshEngineState()
{
    const bool alternate = m_screen->isAlternateScreen();
    if (alternate != m_alternateScreen) {
        m_alternateScreen = alternate;
        // 主备屏的文档构成完全不同（备用屏不含滚动历史），必须整体重建。
        invalidateDocument();
        emit alternateScreenChanged(alternate);
    }
    syncScrollback();
    const GhosttyScreen::Cursor cursor = m_screen->cursor();
    m_cursorRow = cursor.row;
    m_cursorColumn = cursor.column;
    m_cursorVisible = cursor.visible;
}

void VtermBridge::syncScrollback()
{
    bool historyErased = false;
    const std::vector<GhosttyScreen::Row> rows = m_screen->takeNewHistoryRows(&historyErased);
    if (historyErased && !m_suppressHistoryErase) {
        // 远端发了 ED 3（`clear` 通常会带上）：丢弃自己保存的历史，文档也只能整体重建。
        m_scrollback.clear();
        m_scrollbackApproxBytes = 0;
        invalidateDocument();
    }
    for (const GhosttyScreen::Row &row : rows) {
        // 只增不减的全局行号是增量同步的坐标系（在 appendScrollbackLine 内递增）。
        appendScrollbackLine(convertRow(row));
    }
}

QByteArray VtermBridge::boundUnterminatedString(const QByteArray &bytes)
{
    // 背景：`tail`/`cat` 一个 JAR 或可执行文件会把原始字节打进 PTY。ZIP 中央目录这类
    // 段落里必然混着 ESC，一旦凑出 `ESC ]`（OSC）或 `ESC P`（DCS）而后续始终没有 ST/BEL
    // 收尾，libvterm 会一直把字节往字符串缓冲里塞——之后 Shell 打印的提示符、用户敲的
    // 命令统统被吞进这个未结束的字符串，屏幕就此定格发黑，退出会话也不恢复。真实终端里
    // 得手动 `reset`。xterm/VTE/Termius 等不会黑屏，是因为它们一遇到字符串序列内部的
    // C0 控制字符（除 ESC 和几个合法收尾符外）就中止该序列——二进制里满是 CR/LF/TAB，
    // 残缺的 `ESC ]` 撑不过几个字节。libvterm 0.3.3 没有这条规则，会一直缓冲到显式收尾。
    // 这里在字节进入 libvterm 之前补齐这个行为：
    //   1) 字符串态里出现非法 C0 控制字符 → 注入 CAN(0x18) 强制中止（已验证 libvterm
    //      收到 CAN 会退出字符串状态），随后该字节按普通输入处理；
    //   2) 兜底：即使整段噪音恰好没有 C0，超过 kMaxStringPayload 字节仍未收尾也中止。
    //
    // 对正常输出零影响：普通文本走不到 InString 分支；合法的标题/超链接/DCS 载荷都是可打印
    // 字节、且远不到长度阈值。状态跨 feed() 调用保留，网络分片不会漏判。
    constexpr int kMaxStringPayload = 8192;

    // 字符串序列里允许出现、不代表"损坏"的 C0：BEL/CAN/SUB/NUL 是收尾符（单独处理），
    // ESC 可能是 ST 的前半（单独处理）。除此以外的 C0（0x01-0x06、0x08-0x1F 等）在合法
    // OSC/DCS 载荷里不会出现，视为二进制噪音并中止。
    const auto isCorruptingControl = [](unsigned char byte) {
        return byte < 0x20 && byte != 0x07 && byte != 0x18 && byte != 0x1a
            && byte != 0x00 && byte != 0x1b;
    };

    QByteArray output;
    output.reserve(bytes.size() + 1);
    for (const char rawByte : bytes) {
        const auto byte = static_cast<unsigned char>(rawByte);

        switch (m_stringScan) {
        case StringScan::Normal:
            output.append(rawByte);
            if (byte == 0x1b) {
                m_stringScan = StringScan::AfterEsc;
            }
            break;

        case StringScan::AfterEsc:
            output.append(rawByte);
            // OSC `]` / DCS `P` / SOS `X` / PM `^` / APC `_` 都是不定长字符串序列。
            if (byte == 0x5d || byte == 0x50 || byte == 0x58 || byte == 0x5e
                || byte == 0x5f) {
                m_stringScan = StringScan::InString;
                m_stringScanLength = 0;
            } else if (byte != 0x1b) {
                m_stringScan = StringScan::Normal;
            }
            break;

        case StringScan::InString:
            // BEL / CAN / SUB / NUL 都会让 libvterm 结束当前字符串；ESC 可能是 ST 的前半，
            // 无论后半是不是 `\`，字符串都到此为止。任一情况都回到常规扫描。
            if (byte == 0x07 || byte == 0x18 || byte == 0x1a || byte == 0x00) {
                output.append(rawByte);
                m_stringScan = StringScan::Normal;
                break;
            }
            if (byte == 0x1b) {
                output.append(rawByte);
                m_stringScan = StringScan::AfterEsc;
                break;
            }
            if (isCorruptingControl(byte) || m_stringScanLength >= kMaxStringPayload) {
                // 非法 C0 或超长仍未收尾：判定为二进制噪音凑出的残缺序列，强制中止。
                output.append('\x18');
                output.append(rawByte);
                m_stringScan = StringScan::Normal;
                // 记录下来，供 TerminalPane 在下一次 Shell 提示符出现时自动复位一次。
                m_corruptionDetected = true;
                break;
            }
            output.append(rawByte);
            ++m_stringScanLength;
            break;
        }
    }
    return output;
}

void VtermBridge::resetTerminal()
{
    // CAN 取消残留的 CSI/OSC/DCS，ST 再结束字符串；直接软复位画笔无法退出解析器的
    // 字符串状态，下一条提示仍可能被吞掉。先退出备用屏，避免归档全屏程序的临时画布。
    const QByteArray cancelAndLeave = QByteArrayLiteral("\x18\x1b\\\x1b[?1049l");
    m_screen->feed(cancelAndLeave.constData(), static_cast<std::size_t>(cancelAndLeave.size()));
    refreshEngineState();
    int lastRow = lastVisibleScreenRow();
    // 空终端没有需要归档的内容；跳过尾部空行，避免首次连接凭空多出一行并改变提示位置。
    while (lastRow >= 0) {
        const Line line = screenLine(lastRow);
        if (std::any_of(line.cbegin(), line.cend(),
                        [](const Cell &cell) { return !isBlankCell(cell); })) {
            break;
        }
        --lastRow;
    }
    for (int row = 0; row <= lastRow; ++row) {
        appendScrollbackLine(screenLine(row));
    }
    m_stringScan = StringScan::Normal;
    m_stringScanLength = 0;
    m_corruptionDetected = false;
    // 硬复位同时恢复隐藏属性、颜色、光标、字符集和键盘模式；仅发 SGR 0 无法覆盖这些
    // 状态。默认色及调色板属于当前主题配置，libvterm 复位继续使用这些配置。
    m_suppressHistoryErase = true; // 复位会清空引擎历史，但我们刚归档的内容要保留
    m_screen->reset();
    // 主题色属于配置而非终端状态，复位后重新推一遍，避免引擎退回库默认色。
    applyColors();
    refreshEngineState();
    m_suppressHistoryErase = false;
    invalidateDocument();
}

void VtermBridge::resize(const int rows, const int columns)
{
    const int normalizedRows = std::max(rows, 1);
    const int normalizedColumns = std::max(columns, 1);
    if (normalizedRows == m_rows && normalizedColumns == m_columns) {
        return;
    }
    m_rows = normalizedRows;
    m_columns = normalizedColumns;
    m_screen->resize(m_columns, m_rows);
    // 缩小行数时引擎会把顶部几行挤进历史，必须立刻取走，否则会和之后的新行混在一起。
    refreshEngineState();
    // 已经写进文档的段与新的行列不再对应。
    invalidateDocument();
}

void VtermBridge::setDefaultColors(const QColor &foreground, const QColor &background)
{
    m_defaultForeground = foreground.isValid() ? foreground : QColor(Qt::white);
    m_defaultBackground = background.isValid() ? background : QColor(Qt::black);
    applyColors();
    // 默认色变化会改写每一格未显式着色的文本，已渲染的段全部过期。
    invalidateDocument();
}

void VtermBridge::setAnsiPalette(const QVector<QColor> &colors)
{
    // 半套调色板比不换更糟：前景换了背景没换会直接把文字和底色刷成同一个颜色。宁可整体
    // 忽略，让终端保持上一套自洽的配色。
    if (colors.size() != 16) {
        return;
    }
    for (const QColor &color : colors) {
        if (!color.isValid()) {
            return;
        }
    }
    m_ansiPalette = colors;
    applyColors();
    // 已渲染的段保存的是解析后的 RGB，调色板换了它们不会自己更新。
    invalidateDocument();
}

void VtermBridge::setScrollbackLimit(const int lineCount)
{
    m_scrollbackLimit = std::clamp(lineCount, 100, 1000000);
    // 这里不必标记文档失效：增量同步靠全局行号识别被淘汰的段，会自行从头部删除。
    enforceScrollbackLimits();
}

void VtermBridge::setScrollbackByteLimit(const qint64 maxBytes)
{
    m_scrollbackByteLimit = maxBytes <= 0 ? 0 : maxBytes;
    enforceScrollbackLimits();
}

qsizetype VtermBridge::estimateLineBytes(const PackedLine &line)
{
    // 每个非空的 Qt 容器各有一次堆分配（约 16 字节头 + 载荷），再加 deque 里的行本体。
    constexpr qsizetype kAllocationHeader = 16;
    qsizetype bytes = static_cast<qsizetype>(sizeof(PackedLine));
    if (!line.text.isEmpty()) {
        bytes += kAllocationHeader + line.text.capacity() * static_cast<qsizetype>(sizeof(QChar));
    }
    if (!line.cellLayout.isEmpty()) {
        bytes += kAllocationHeader + line.cellLayout.capacity();
    }
    if (!line.runs.isEmpty()) {
        bytes += kAllocationHeader
            + line.runs.capacity() * static_cast<qsizetype>(sizeof(PackedRun));
    }
    return bytes;
}

VtermBridge::PackedLine VtermBridge::packLine(const Line &line)
{
    // 行尾空白不会写进文档（buildRenderedLine 按最后一个非空白格截断），打包时直接丢掉。
    // 双宽右半格（width 0）必须跟左半格一起保留，遇到它就停。
    qsizetype columns = line.size();
    while (columns > 0) {
        const Cell &last = line.at(columns - 1);
        if (last.width == 0 || !isBlankCell(last)) {
            break;
        }
        --columns;
    }

    PackedLine packed;
    packed.columns = static_cast<int>(columns);
    bool simple = true;
    for (qsizetype column = 0; column < columns; ++column) {
        const Cell &cell = line.at(column);
        if (cell.width != 1 || cell.text.size() != 1) {
            simple = false;
            break;
        }
    }

    packed.text.reserve(columns);
    if (!simple) {
        packed.cellLayout.resize(columns);
    }
    const Cell *runStyle = nullptr;
    for (qsizetype column = 0; column < columns; ++column) {
        const Cell &cell = line.at(column);
        if (!simple) {
            // VTERM_MAX_CHARS_PER_CELL 个 UCS-4 码位最多 12 个 UTF-16 单元，6 位足够。
            const auto textLength = static_cast<quint8>(std::min<qsizetype>(cell.text.size(), 63));
            const auto width = static_cast<quint8>(std::clamp(cell.width, 0, 3));
            packed.cellLayout[column] = static_cast<char>((textLength << 2U) | width);
        }
        // 双宽右半格的样式由 convertCell() 固定为默认值，解包时同样按默认值还原；让它参与
        // 分段只会把一段中文切成一格一段。
        if (cell.width == 0) {
            continue;
        }
        packed.text.append(cell.text.left(63));
        if (runStyle == nullptr || !hasSameStyle(*runStyle, cell)) {
            runStyle = &cell;
            PackedRun run;
            run.column = static_cast<quint16>(std::min<qsizetype>(column, 0xffff));
            run.foreground = cell.foreground.rgb();
            run.background = cell.background.rgb();
            run.flags = static_cast<quint8>(
                (cell.bold ? PackedBold : 0U) | (cell.italic ? PackedItalic : 0U)
                | (cell.underline ? PackedUnderline : 0U) | (cell.strike ? PackedStrike : 0U)
                | (cell.reverse ? PackedReverse : 0U) | (cell.conceal ? PackedConceal : 0U)
                | (cell.foreground.isValid() ? 0U : PackedForegroundInvalid)
                | (cell.background.isValid() ? 0U : PackedBackgroundInvalid));
            packed.runs.append(run);
        }
    }
    packed.text.squeeze();
    packed.runs.squeeze();
    return packed;
}

VtermBridge::Line VtermBridge::unpackLine(const PackedLine &packed)
{
    Line line;
    line.reserve(packed.columns);
    const bool simple = packed.cellLayout.isEmpty();
    qsizetype textPosition = 0;
    qsizetype runIndex = -1;
    Cell styled;
    for (int column = 0; column < packed.columns; ++column) {
        int width = 1;
        qsizetype textLength = 1;
        if (!simple) {
            const auto layout = static_cast<quint8>(packed.cellLayout.at(column));
            width = layout & 0x3U;
            textLength = layout >> 2U;
        }
        if (width == 0) {
            // 与 convertCell() 对双宽右半格的输出完全一致：只有宽度为 0，其余全是默认值。
            Cell continuation;
            continuation.width = 0;
            line.append(continuation);
            continue;
        }
        while (runIndex + 1 < packed.runs.size() && packed.runs.at(runIndex + 1).column <= column) {
            ++runIndex;
            styled = styledCell(packed.runs.at(runIndex));
        }
        Cell cell = styled;
        cell.width = width;
        cell.text = textLength > 0 ? packed.text.mid(textPosition, textLength) : QString();
        textPosition += textLength;
        line.append(std::move(cell));
    }
    return line;
}

void VtermBridge::enforceScrollbackLimits()
{
    while (!m_scrollback.empty()
           && (static_cast<int>(m_scrollback.size()) > m_scrollbackLimit
               || (m_scrollbackByteLimit > 0
                   && m_scrollbackApproxBytes > m_scrollbackByteLimit))) {
        m_scrollbackApproxBytes -= estimateLineBytes(m_scrollback.front());
        m_scrollback.pop_front();
    }
    if (m_scrollbackApproxBytes < 0 || m_scrollback.empty()) {
        m_scrollbackApproxBytes = 0;
    }
}

void VtermBridge::appendScrollbackLine(Line line)
{
    PackedLine packed = packLine(line);
    m_scrollbackApproxBytes += estimateLineBytes(packed);
    m_scrollback.push_back(std::move(packed));
    ++m_scrollbackPushed;
    enforceScrollbackLimits();
}

void VtermBridge::setLineHeightPercent(const int percent)
{
    const int normalized = std::clamp(percent, 80, 250);
    if (normalized == m_lineHeightPercent) {
        return;
    }
    m_lineHeightPercent = normalized;
    // 行高写在每一段的 QTextBlockFormat 上，只能整体重建。
    invalidateDocument();
}

void VtermBridge::setLineAnchors(const QVector<QPair<QChar, QFont>> &anchors)
{
    if (anchors == m_lineAnchors) {
        return;
    }
    m_lineAnchors = anchors;
    // 锚点写在每一段的正文里，只能整体重建才能让已渲染的段跟着变。
    invalidateDocument();
}

QString VtermBridge::lineAnchorSuffix()
{
    // 这是锚点的**上限集合**，不是某次渲染的实际结果：实际追加哪几个由 TerminalView 按
    // 本机字体探测决定（emoji 后备字体与 CJK 相同时就只有一个"中"）。因此它只能用来做
    // 断言与自检，绝不能拿去剔除锚点——剔除只认字符格式，见 TerminalView 的
    // textWithoutLineAnchors()。
    // U+4E2D 是真实汉字：只有真实汉字才会走到和正文中文完全相同的后备字体，度量才严格
    // 一致（U+3000/U+200B 这类特殊空白在某些字体里不存在，会被换成另一个字体，差 1px）。
    // U+200B 取 emoji 字体的度量且不占宽度。两者合起来才覆盖到最高的那种字体。
    static const QString suffix = QString(QChar(0x4E2D)) + QChar(0x200B);
    return suffix;
}

bool VtermBridge::isLineAnchorFormat(const QTextCharFormat &format)
{
    return format.property(kLineHeightAnchorProperty).toBool();
}

void VtermBridge::invalidateDocument()
{
    m_documentDirty = true;
}

int VtermBridge::releaseBlockLayouts(QTextDocument *document, const int firstBlock,
                                     const int lastBlock)
{
    if (document == nullptr || document != m_documentTarget || m_documentDirty
        || m_alternateScreen) {
        return 0;
    }
    // 文档可能落后于历史（欠着一帧没渲染）：文档头部那几段对应的行也许已经被上限淘汰，
    // 手里没有原始内容就不能重写，跳过它们。
    const qint64 retainedFirst = m_scrollbackPushed - static_cast<qint64>(m_scrollback.size());
    const int evictedBlocks = static_cast<int>(
        std::clamp<qint64>(retainedFirst - m_documentScrollbackFirst, 0, m_documentScrollbackCount));
    const int first = std::max({firstBlock, evictedBlocks, 0});
    const int last = std::min(lastBlock, m_documentScrollbackCount - 1);
    if (first > last) {
        return 0;
    }

    // 从前一段的段分隔符开始删，这样起始段自身的 QTextBlockData（连同它的排版缓存）也会
    // 被释放；首段前面没有分隔符，只能保留它的块数据、重写正文。
    const QTextBlock firstTextBlock = document->findBlockByNumber(first);
    const QTextBlock lastTextBlock = document->findBlockByNumber(last);
    if (!firstTextBlock.isValid() || !lastTextBlock.isValid()) {
        return 0;
    }
    QTextCursor cursor(document);
    cursor.beginEditBlock();
    cursor.setPosition(first > 0 ? firstTextBlock.position() - 1 : firstTextBlock.position());
    cursor.setPosition(lastTextBlock.position() + lastTextBlock.length() - 1,
                       QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    for (int block = first; block <= last; ++block) {
        if (block > 0) {
            cursor.insertBlock();
        }
        const qint64 line = m_documentScrollbackFirst + block;
        writePackedLine(cursor, m_scrollback[static_cast<size_t>(line - retainedFirst)]);
    }
    cursor.endEditBlock();
    return last - first + 1;
}

void VtermBridge::sendKey(const GhosttyKey key, const GhosttyMods modifiers)
{
    // 编码器按终端当前保存的 DECCKM/DECPAM/Kitty 键盘模式生成字节，和 DSR/DA 应答走同一条
    // protocolResponse 通路，最终由 TerminalView 转成 inputBytes 发往 PTY。
    const std::string bytes = m_screen->encodeKey(key, modifiers);
    if (!bytes.empty()) {
        emit protocolResponse(QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())));
    }
}

void VtermBridge::beginPaste()
{
    const std::string marker = m_screen->pasteMarker(true);
    if (!marker.empty()) {
        emit protocolResponse(QByteArray(marker.data(), static_cast<qsizetype>(marker.size())));
    }
}

void VtermBridge::endPaste()
{
    const std::string marker = m_screen->pasteMarker(false);
    if (!marker.empty()) {
        emit protocolResponse(QByteArray(marker.data(), static_cast<qsizetype>(marker.size())));
    }
}

QString VtermBridge::cursorLineText() const
{
    if (m_cursorRow < 0 || m_cursorRow >= m_rows) {
        return {};
    }

    QString text;
    const Line line = screenLine(m_cursorRow);
    for (const Cell &cell : line) {
        // 宽度为 0 的单元格是宽字符的占位续格，重复拼接会破坏中文或 Nerd Font 字符。
        if (cell.width > 0) {
            text.append(cell.text.isEmpty() ? QStringLiteral(" ") : cell.text);
        }
    }
    return text.trimmed();
}

VtermBridge::InputLine VtermBridge::cursorInputLine() const
{
    InputLine result;
    if (m_cursorRow < 0 || m_cursorRow >= m_rows) {
        return result;
    }

    const auto continues = [this](const int row) { return m_screen->rowContinues(row); };
    int firstRow = m_cursorRow;
    while (firstRow > 0 && continues(firstRow)) {
        --firstRow;
    }
    int lastRow = m_cursorRow;
    while (lastRow + 1 < m_rows && continues(lastRow + 1)) {
        ++lastRow;
    }

    result.cursorOffset = -1;
    for (int row = firstRow; row <= lastRow; ++row) {
        const Line line = screenLine(row);
        for (int column = 0; column < line.size(); ++column) {
            if (row == m_cursorRow && column == m_cursorColumn) {
                result.cursorOffset = result.text.size();
            }
            const Cell &cell = line.at(column);
            // 宽度为 0 的单元格是宽字符的占位续格，重复拼接会破坏中文或 Nerd Font 字符。
            if (cell.width > 0) {
                result.text.append(cell.text.isEmpty() ? QStringLiteral(" ") : cell.text);
            }
        }
    }
    if (result.cursorOffset < 0) {
        result.cursorOffset = result.text.size();
    }
    return result;
}

bool VtermBridge::isBlankCell(const Cell &cell)
{
    for (const QChar character : cell.text) {
        if (!character.isSpace()) {
            return false;
        }
    }
    return true;
}

bool VtermBridge::hasSameStyle(const Cell &left, const Cell &right)
{
    return left.foreground == right.foreground && left.background == right.background
        && left.bold == right.bold && left.italic == right.italic
        && left.underline == right.underline && left.strike == right.strike
        && left.reverse == right.reverse && left.conceal == right.conceal;
}

QTextCharFormat VtermBridge::charFormatForCell(const Cell &cell)
{
    QColor foreground = cell.foreground;
    QColor background = cell.background;
    if (cell.reverse) {
        std::swap(foreground, background);
    }
    if (cell.conceal) {
        foreground = background;
    }

    QTextCharFormat format;
    format.setForeground(foreground);
    format.setBackground(background);
    format.setFontWeight(cell.bold ? QFont::Bold : QFont::Normal);
    format.setFontItalic(cell.italic);
    format.setFontUnderline(cell.underline);
    format.setFontStrikeOut(cell.strike);
    return format;
}

void VtermBridge::removeLeadingBlocks(QTextDocument *document, const int count)
{
    if (count <= 0) {
        return;
    }
    if (count >= document->blockCount()) {
        document->clear();
        return;
    }
    // 选区 [0, 第 count 段起点) 覆盖前 count 段的正文以及它们各自的段分隔符，删除后段数
    // 恰好减少 count；少算那一个分隔符会残留一个空段，整篇内容随之下移一行。
    QTextCursor cursor(document);
    cursor.setPosition(document->findBlockByNumber(count).position(), QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
}

void VtermBridge::removeTrailingBlocks(QTextDocument *document, const int count)
{
    if (count <= 0) {
        return;
    }
    if (count >= document->blockCount()) {
        document->clear();
        return;
    }
    // 与删除头部相反：从最后 count 段之前的那个分隔符开始选到文档末尾。若从段首开始选，
    // 分隔符会留下来，尾部多出一个空段，下一次同步的段计数就全错位了。
    const QTextBlock firstRemoved = document->findBlockByNumber(document->blockCount() - count);
    QTextCursor cursor(document);
    cursor.setPosition(firstRemoved.position() - 1);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
}

VtermBridge::RenderedLine VtermBridge::buildRenderedLine(const Line &line, const int screenRow) const
{
    RenderedLine rendered;
    rendered.cells = line;
    rendered.cursorColumn = (screenRow >= 0 && screenRow == m_cursorRow
                             && m_cursorColumn >= 0 && m_cursorColumn < line.size())
        ? m_cursorColumn
        : -1;
    for (int column = 0; column < line.size(); ++column) {
        // 行尾空白不写进文档；但光标所在列必须保留，否则光标停在行尾时定位不到任何字符。
        if (!isBlankCell(line.at(column))
            || (rendered.cursorColumn >= 0 && column <= rendered.cursorColumn)) {
            rendered.lastColumn = column;
        }
    }
    return rendered;
}

bool VtermBridge::isSameRenderedLine(const RenderedLine &left, const RenderedLine &right)
{
    if (left.lastColumn != right.lastColumn || left.cursorColumn != right.cursorColumn) {
        return false;
    }
    for (int column = 0; column <= left.lastColumn; ++column) {
        const Cell &leftCell = left.cells.at(column);
        const Cell &rightCell = right.cells.at(column);
        if (leftCell.width != rightCell.width || leftCell.text != rightCell.text
            || !hasSameStyle(leftCell, rightCell)) {
            return false;
        }
    }
    return true;
}

void VtermBridge::writeBlockFormat(QTextCursor &cursor) const
{
    QTextBlockFormat blockFormat;
    blockFormat.setLineHeight(m_lineHeightPercent, QTextBlockFormat::ProportionalHeight);
    cursor.setBlockFormat(blockFormat);
}

void VtermBridge::writeLineAnchors(QTextCursor &cursor) const
{
    // 行高锚点必须落在每一段上，空行也不例外：只要漏掉一段，那一段就会矮几像素，整块
    // 网格的行距立刻不再均匀，最后一行也就重新开始漂移。零宽字符不占列宽，不会影响
    // 双宽字符的列对齐；导出与复制路径会把它剔除。
    for (const auto &anchor : m_lineAnchors) {
        QTextCharFormat anchorFormat;
        anchorFormat.setFont(anchor.second);
        // 专用属性让视图能区分“内部锚点中”与用户真实输入的“中”。仅依靠透明前景色不够：
        // Qt 绘制选区时会用 HighlightedText 覆盖前景色，正是原问题中锚点显形的原因。
        anchorFormat.setProperty(kLineHeightAnchorProperty, true);
        anchorFormat.setForeground(QColor(Qt::transparent));
        cursor.insertText(QString(anchor.first), anchorFormat);
    }
}

void VtermBridge::writePackedLine(QTextCursor &cursor, const PackedLine &packed) const
{
    writeBlockFormat(cursor);

    // 与 writeRenderedLine(buildRenderedLine(unpackLine(packed), -1)) 写出完全相同的内容，
    // 只是不再为每一格构造 Cell 和 QString：整体重建时要写上万行，逐格解包会把重建耗时
    // 拉长一半以上。
    const bool simple = packed.cellLayout.isEmpty();
    const auto cellWidth = [&packed, simple](const int column) {
        return simple ? 1 : static_cast<int>(static_cast<quint8>(packed.cellLayout.at(column)) & 0x3U);
    };
    const auto cellTextLength = [&packed, simple](const int column) -> qsizetype {
        return simple ? 1 : static_cast<quint8>(packed.cellLayout.at(column)) >> 2U;
    };
    const auto isBlankText = [](const QStringView text) {
        return std::all_of(text.cbegin(), text.cend(), [](const QChar c) { return c.isSpace(); });
    };

    // 与 buildRenderedLine 同一判据：最后一个非空白格之后的内容不写入。
    int lastColumn = -1;
    qsizetype textPosition = 0;
    for (int column = 0; column < packed.columns; ++column) {
        const qsizetype length = cellWidth(column) == 0 ? 0 : cellTextLength(column);
        if (!isBlankText(QStringView(packed.text).mid(textPosition, length))) {
            lastColumn = column;
        }
        textPosition += length;
    }

    QString run;
    run.reserve(lastColumn + 1);
    qsizetype runIndex = -1;
    qsizetype flushedRunIndex = -1;
    const auto flushRun = [this, &cursor, &run, &packed, &flushedRunIndex]() {
        if (run.isEmpty() || flushedRunIndex < 0) {
            return;
        }
        cursor.insertText(run, charFormatForRun(packed.runs.at(flushedRunIndex)));
        run.clear();
    };

    textPosition = 0;
    for (int column = 0; column <= lastColumn; ++column) {
        if (cellWidth(column) == 0) {
            continue;
        }
        while (runIndex + 1 < packed.runs.size() && packed.runs.at(runIndex + 1).column <= column) {
            ++runIndex;
        }
        if (runIndex != flushedRunIndex) {
            flushRun();
            flushedRunIndex = runIndex;
        }
        const qsizetype length = cellTextLength(column);
        if (length == 0) {
            run.append(QLatin1Char(' '));
        } else {
            run.append(QStringView(packed.text).mid(textPosition, length));
        }
        textPosition += length;
    }
    flushRun();

    writeLineAnchors(cursor);
}

VtermBridge::Cell VtermBridge::styledCell(const PackedRun &run)
{
    Cell styled;
    styled.foreground = (run.flags & PackedForegroundInvalid) != 0
        ? QColor() : QColor::fromRgb(run.foreground);
    styled.background = (run.flags & PackedBackgroundInvalid) != 0
        ? QColor() : QColor::fromRgb(run.background);
    styled.bold = (run.flags & PackedBold) != 0;
    styled.italic = (run.flags & PackedItalic) != 0;
    styled.underline = (run.flags & PackedUnderline) != 0;
    styled.strike = (run.flags & PackedStrike) != 0;
    styled.reverse = (run.flags & PackedReverse) != 0;
    styled.conceal = (run.flags & PackedConceal) != 0;
    return styled;
}

QTextCharFormat VtermBridge::charFormatForRun(const PackedRun &run)
{
    return charFormatForCell(styledCell(run));
}

void VtermBridge::writeRenderedLine(QTextCursor &cursor, const RenderedLine &line)
{
    writeBlockFormat(cursor);

    // 相邻同属性单元格合并成一次 insertText：终端一行通常只有一两种属性，逐格插入会把
    // QTextCharFormat 构造和文档写入放大到列数倍，正是全量渲染慢的主要常数因子。
    QString run;
    run.reserve(line.lastColumn + 1);
    const Cell *runStyle = nullptr;
    const auto flushRun = [&cursor, &run, &runStyle]() {
        if (run.isEmpty() || runStyle == nullptr) {
            return;
        }
        cursor.insertText(run, charFormatForCell(*runStyle));
        run.clear();
    };

    for (int column = 0; column <= line.lastColumn; ++column) {
        const Cell &cell = line.cells.at(column);
        // 宽度为 0 的格是双宽字符的右半格，内容已由左半格一并写入。
        if (cell.width == 0) {
            continue;
        }
        if (column == line.cursorColumn) {
            // 光标位置在合并写入前先落定：run 里还没提交的字符也要计入偏移。
            flushRun();
            m_renderedCursorPosition = cursor.position();
            m_renderedCursorText = cell.text;
            m_renderedCursorBackground = cell.reverse ? cell.foreground : cell.background;
        }
        if (runStyle == nullptr || !hasSameStyle(*runStyle, cell)) {
            flushRun();
            runStyle = &cell;
        }
        run.append(cell.text.isEmpty() ? QStringLiteral(" ") : cell.text);
    }
    flushRun();
    writeLineAnchors(cursor);
}

void VtermBridge::locateCursorInBlock(const QTextBlock &block, const RenderedLine &line)
{
    if (line.cursorColumn < 0 || !block.isValid()) {
        return;
    }
    // 列号不等于字符偏移：双宽字符占两列却只有一个字符，右半格根本没写进文档。必须按
    // writeRenderedLine 的同一套规则数一遍，否则中文行里的光标会越画越偏。
    int offset = 0;
    for (int column = 0; column < line.cursorColumn; ++column) {
        const Cell &cell = line.cells.at(column);
        if (cell.width == 0) {
            continue;
        }
        offset += cell.text.isEmpty() ? 1 : static_cast<int>(cell.text.size());
    }
    const Cell &cursorCell = line.cells.at(line.cursorColumn);
    m_renderedCursorPosition = block.position() + offset;
    m_renderedCursorText = cursorCell.text;
    m_renderedCursorBackground = cursorCell.reverse ? cursorCell.foreground
                                                    : cursorCell.background;
}

bool VtermBridge::renderedScreenWouldShrink() const
{
    // 还没画过、或下一帧本来就要整篇重建时，没有可对照的“当前这一屏”。备用屏按网格整块
    // 绘制，行数固定，不存在主屏这种“擦掉尾部空行就少几段”的收缩。
    if (m_alternateScreen || m_documentDirty || m_documentScreenLines.isEmpty()) {
        return false;
    }
    return lastVisibleScreenRow() + 1 < m_documentScreenLines.size();
}

int VtermBridge::lastVisibleScreenRow() const
{
    // 备用屏是一块尺寸恰好等于 PTY 网格的画布，全屏程序按绝对行列自绘，必须固定渲染
    // 全部 m_rows 行：少一行就会让整块画布上移一行，vim 的第 0 行连同光标一起被推出视口。
    if (m_alternateScreen) {
        return m_rows - 1;
    }
    int lastRow = std::clamp(m_cursorRow, 0, m_rows - 1);
    for (int row = m_rows - 1; row > lastRow; --row) {
        const Line candidate = screenLine(row);
        const bool hasContent = std::any_of(candidate.cbegin(),
                                            candidate.cend(),
                                            [](const Cell &cell) { return !isBlankCell(cell); });
        if (hasContent) {
            return row;
        }
    }
    return lastRow;
}

void VtermBridge::rebuildDocument(QTextDocument *document)
{
    document->clear();
    QTextCursor cursor(document);
    cursor.beginEditBlock();

    // 备用屏下滚动历史不参与渲染：文档一旦比视口高，QPlainTextEdit 会按底部对齐，被推出
    // 顶部的正是网格第 0 行——用户看到的现象是"光标消失、文件从中间开始显示"。
    const int scrollbackBlocks = m_alternateScreen ? 0 : static_cast<int>(m_scrollback.size());
    for (int index = 0; index < scrollbackBlocks; ++index) {
        if (index > 0) {
            cursor.insertBlock();
        }
        writePackedLine(cursor, m_scrollback[static_cast<size_t>(index)]);
    }

    const int lastScreenRow = lastVisibleScreenRow();
    m_documentScreenLines.clear();
    m_documentScreenLines.reserve(lastScreenRow + 1);
    for (int row = 0; row <= lastScreenRow; ++row) {
        if (scrollbackBlocks > 0 || row > 0) {
            cursor.insertBlock();
        }
        m_documentScreenLines.append(buildRenderedLine(screenLine(row), row));
        writeRenderedLine(cursor, m_documentScreenLines.last());
    }
    cursor.endEditBlock();

    m_documentTarget = document;
    m_documentScrollbackFirst = m_scrollbackPushed - scrollbackBlocks;
    m_documentScrollbackCount = scrollbackBlocks;
    m_documentDirty = false;
}

bool VtermBridge::render(QTextDocument *document)
{
    if (document == nullptr) {
        return false;
    }

    m_renderedCursorPosition = -1;
    m_renderedCursorText.clear();
    m_renderedCursorBackground = QColor();

    if (m_documentDirty || document != m_documentTarget) {
        rebuildDocument(document);
        return true;
    }

    // 增量路径：滚动历史一旦推入就是只读的，文档里对应的段可以原样保留。可能变化的只有
    // 头部被上限淘汰的几段，和尾部那一屏。
    const int scrollbackBlocks = m_alternateScreen ? 0 : static_cast<int>(m_scrollback.size());
    const qint64 firstRetained = m_scrollbackPushed - scrollbackBlocks;
    const qint64 documentEnd = m_documentScrollbackFirst + m_documentScrollbackCount;
    const qint64 droppedFromFront = firstRetained - m_documentScrollbackFirst;
    const qint64 appendCount = firstRetained + scrollbackBlocks - documentEnd;

    // 一次淘汰掉的历史比文档里现存的还多（例如一条命令直接刷了十万行），增量已无优势，
    // 中间那段历史谁都没见过，整体重建反而更简单可靠。
    if (droppedFromFront < 0 || appendCount < 0 || droppedFromFront > m_documentScrollbackCount) {
        rebuildDocument(document);
        return true;
    }

    // 尾部区间的目标内容 = 本次新进历史的几行 + 当前这一屏。把两者放在一起比对是关键：
    // 一行从屏幕滚进历史时内容并没有变，逐段比对会发现它和上次写的那一段完全一样，于是
    // 原地保留——文档里那一段既不重写也不移动，用户压在上面的选区自然不会被抹掉。
    QVector<RenderedLine> desiredTail;
    desiredTail.reserve(static_cast<qsizetype>(appendCount) + m_rows);
    for (qint64 index = scrollbackBlocks - appendCount; index < scrollbackBlocks; ++index) {
        desiredTail.append(buildRenderedLine(unpackLine(m_scrollback[static_cast<size_t>(index)]), -1));
    }
    const int lastScreenRow = lastVisibleScreenRow();
    for (int row = 0; row <= lastScreenRow; ++row) {
        desiredTail.append(buildRenderedLine(screenLine(row), row));
    }

    QTextCursor cursor(document);
    cursor.beginEditBlock();

    if (droppedFromFront > 0) {
        removeLeadingBlocks(document, static_cast<int>(droppedFromFront));
    }
    const int scrollbackPrefix = static_cast<int>(m_documentScrollbackCount - droppedFromFront);

    // 逐段比对，找出从头开始有多少段可以原样保留。
    const qsizetype comparableCount = std::min(m_documentScreenLines.size(), desiredTail.size());
    qsizetype unchanged = 0;
    while (unchanged < comparableCount
           && isSameRenderedLine(m_documentScreenLines.at(unchanged), desiredTail.at(unchanged))) {
        ++unchanged;
    }

    // 被跳过的段里可能就有光标行（例如用户只是移动了光标而没有改动文本），它的文档位置
    // 无人计算，补全弹层和光标绘制会失去锚点。
    for (qsizetype index = 0; index < unchanged; ++index) {
        if (desiredTail.at(index).cursorColumn >= 0) {
            locateCursorInBlock(document->findBlockByNumber(scrollbackPrefix
                                                            + static_cast<int>(index)),
                                desiredTail.at(index));
        }
    }

    // 两侧都存在的段就地换正文，不删段也不动段边界。这一点很要紧：如果改成"整段删掉再
    // 插回来"，紧贴着段尾的选区端点会被插入操作吸走，用户选中最后一行后每来一批输出，
    // 高亮就会一路蔓延到新内容上。
    const qsizetype overlap = std::min(m_documentScreenLines.size(), desiredTail.size());
    for (qsizetype index = unchanged; index < overlap; ++index) {
        const QTextBlock block = document->findBlockByNumber(scrollbackPrefix
                                                             + static_cast<int>(index));
        QTextCursor blockCursor(document);
        blockCursor.setPosition(block.position());
        blockCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        blockCursor.removeSelectedText();
        writeRenderedLine(blockCursor, desiredTail.at(index));
    }

    if (m_documentScreenLines.size() > desiredTail.size()) {
        removeTrailingBlocks(document,
                             static_cast<int>(m_documentScreenLines.size() - desiredTail.size()));
    } else {
        cursor.movePosition(QTextCursor::End);
        for (qsizetype index = overlap; index < desiredTail.size(); ++index) {
            // 文档此刻已有 scrollbackPrefix + index 段；只有非空文档才需要先起新段。
            if (scrollbackPrefix + index > 0) {
                cursor.insertBlock();
            }
            writeRenderedLine(cursor, desiredTail.at(index));
        }
    }
    cursor.endEditBlock();

    m_documentScrollbackFirst = firstRetained;
    m_documentScrollbackCount = scrollbackPrefix + static_cast<int>(appendCount);
    // 新进历史的那几行已经归入历史区间，尾部区间只剩当前这一屏。
    m_documentScreenLines = desiredTail.mid(static_cast<qsizetype>(appendCount));
    return false;
}

VtermBridge::Cell VtermBridge::convertCell(const GhosttyScreen::Cell &cell) const
{
    Cell converted;
    // 宽字符右半格 width=0：渲染循环据此跳过该格，内容已由左半格一并绘制。
    if (cell.width == 0) {
        converted.width = 0;
        return converted;
    }
    if (!cell.utf8.empty()) {
        converted.text = QString::fromUtf8(cell.utf8.data(), static_cast<qsizetype>(cell.utf8.size()));
    }
    converted.foreground = foregroundQColor(cell.foreground, m_defaultForeground);
    converted.background = backgroundQColor(cell.background, m_defaultBackground);
    converted.bold = cell.bold;
    converted.italic = cell.italic;
    converted.underline = cell.underline;
    converted.strike = cell.strike;
    converted.reverse = cell.reverse;
    converted.conceal = cell.conceal;
    converted.width = cell.width;
    return converted;
}

VtermBridge::Line VtermBridge::convertRow(const GhosttyScreen::Row &row) const
{
    Line line;
    line.reserve(static_cast<qsizetype>(row.size()));
    for (const GhosttyScreen::Cell &cell : row) {
        line.append(convertCell(cell));
    }
    return line;
}

VtermBridge::Line VtermBridge::screenLine(const int row) const
{
    Line line = convertRow(m_screen->activeRow(row));
    // 引擎在尺寸异常时可能返回短行；补足到网格宽度，调用方按列号索引。
    while (line.size() < m_columns) {
        line.append(Cell{});
    }
    return line;
}
