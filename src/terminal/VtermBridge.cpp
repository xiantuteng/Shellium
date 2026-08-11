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

/** @brief 把 libvterm RGB/索引颜色统一转换为 QColor。 */
QColor toQColor(const VTermScreen *screen, VTermColor color)
{
    vterm_screen_convert_color_to_rgb(screen, &color);
    return QColor(color.rgb.red, color.rgb.green, color.rgb.blue);
}

} // namespace

VtermBridge::VtermBridge(QObject *parent)
    : QObject(parent)
    // 成员按声明顺序初始化，m_vterm 位于尺寸字段之前，不能在此读取尚未初始化的 m_rows。
    // 使用与字段默认值一致的常量可避免随机尺寸导致 libvterm resize 断言失败。
    , m_vterm(vterm_new(24, 80))
{
    if (m_vterm == nullptr) {
        qFatal("无法创建 libvterm 实例");
    }
    vterm_set_utf8(m_vterm, 1);
    vterm_output_set_callback(m_vterm, &VtermBridge::outputCallback, this);

    m_screen = vterm_obtain_screen(m_vterm);
    static const VTermScreenCallbacks callbacks = {
        &VtermBridge::damageCallback,
        &VtermBridge::moveRectCallback,
        &VtermBridge::moveCursorCallback,
        &VtermBridge::termPropertyCallback,
        &VtermBridge::bellCallback,
        &VtermBridge::resizeCallback,
        &VtermBridge::pushScrollbackCallback,
        &VtermBridge::popScrollbackCallback,
        &VtermBridge::clearScrollbackCallback
    };
    vterm_screen_set_callbacks(m_screen, &callbacks, this);

    // libvterm 自己只认识 OSC 0/1/2（窗口标题）。Shell 集成使用一个私有编号，因此必须
    // 注册未识别序列的兜底回调，否则整条序列会被当作普通文本打印到用户屏幕上。
    static const VTermStateFallbacks fallbacks = {
        nullptr,                    // control
        nullptr,                    // csi
        &VtermBridge::oscCallback,  // osc
        nullptr,                    // dcs
        nullptr,                    // apc
        nullptr,                    // pm
        nullptr                     // sos
    };
    vterm_screen_set_unrecognised_fallbacks(m_screen, &fallbacks, this);
    vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_SCROLL);
    vterm_screen_enable_altscreen(m_screen, 1);
    vterm_screen_enable_reflow(m_screen, true);

    VTermColor foreground;
    VTermColor background;
    vterm_color_rgb(&foreground, 230, 230, 230);
    vterm_color_rgb(&background, 17, 19, 24);
    vterm_screen_set_default_colors(m_screen, &foreground, &background);
    vterm_screen_reset(m_screen, 1);
}

int VtermBridge::oscCallback(const int command, const VTermStringFragment fragment, void *user)
{
    auto *bridge = static_cast<VtermBridge *>(user);
    // 只处理 Shellium 私有编号，其余未识别 OSC 一律静默丢弃：把它们渲染出来只会在用户
    // 屏幕上留下乱码。
    constexpr int kShelliumOsc = 6773;
    if (command != kShelliumOsc) {
        return 1;
    }

    if (fragment.initial) {
        bridge->m_oscBuffer.clear();
    }
    bridge->m_oscBuffer.append(fragment.str, static_cast<qsizetype>(fragment.len));
    // 单条事件最长也就一行命令；超长载荷只可能来自异常或恶意输出，直接丢弃以免无限增长。
    if (bridge->m_oscBuffer.size() > 8192) {
        bridge->m_oscBuffer.clear();
        return 1;
    }
    if (fragment.final) {
        const QByteArray payload = bridge->m_oscBuffer;
        bridge->m_oscBuffer.clear();
        emit bridge->shellIntegrationEvent(payload);
    }
    return 1;
}

VtermBridge::~VtermBridge()
{
    if (m_vterm != nullptr) {
        vterm_free(m_vterm);
    }
}

void VtermBridge::feed(const QByteArray &bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    vterm_input_write(m_vterm, bytes.constData(), static_cast<size_t>(bytes.size()));
    vterm_screen_flush_damage(m_screen);
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
    vterm_set_size(m_vterm, m_rows, m_columns);
    vterm_screen_flush_damage(m_screen);
    // 改变网格会让 libvterm 回流当前屏幕，已经写进文档的段与新的行列不再对应。
    invalidateDocument();
}

void VtermBridge::setDefaultColors(const QColor &foreground, const QColor &background)
{
    const QColor safeForeground = foreground.isValid() ? foreground : QColor(Qt::white);
    const QColor safeBackground = background.isValid() ? background : QColor(Qt::black);

    VTermColor nativeForeground;
    VTermColor nativeBackground;
    vterm_color_rgb(&nativeForeground,
                    static_cast<uint8_t>(safeForeground.red()),
                    static_cast<uint8_t>(safeForeground.green()),
                    static_cast<uint8_t>(safeForeground.blue()));
    vterm_color_rgb(&nativeBackground,
                    static_cast<uint8_t>(safeBackground.red()),
                    static_cast<uint8_t>(safeBackground.green()),
                    static_cast<uint8_t>(safeBackground.blue()));
    vterm_screen_set_default_colors(m_screen, &nativeForeground, &nativeBackground);
    refreshPenDefaultColors();
    // 默认色变化会改写每一格未显式着色的文本，已渲染的段全部过期。
    invalidateDocument();
}

void VtermBridge::refreshPenDefaultColors()
{
    // libvterm 把"默认色"在两个地方各存了一份：一份是 state 里的 default_fg/default_bg，
    // vterm_screen_set_default_colors() 改的正是它；另一份是当前画笔 state->pen，它在
    // 复位时按当时的默认色取了一份**已经解析成 RGB 的副本**。改默认色不会回头去修画笔，
    // 于是画笔里留着构造期兜底色（#111318）那一份 RGB。
    //
    // 平时看不出来：正常输出走的是 screen 自己那份画笔，颜色是对的。但 DECSC/DECRC——
    // 也就是 vim 进出备用屏用的 CSI ?1049h/l——会把 state->pen 存档再回灌，回灌时那份
    // 陈旧 RGB 就被当成"显式指定的背景色"推给了 screen。表现为退出 vim 之后，新打印的
    // 每一行都顶着一块比终端背景更黑的底色，且和用户选的主题对不上。
    //
    // 没有公开接口能直接改 state->pen，只能借 SGR 39/49（前景/背景恢复默认）让 libvterm
    // 按新的默认色重新取一次副本。仅在画笔当前确实是"默认色"时才注入：远端如果正显式
    // 指定着颜色，它手里的色号本来就与默认色无关，注入只会把远端的颜色抹掉；而远端下一次
    // 自己发 SGR 0 时，libvterm 就会按此刻这份新的默认色重新解析，不需要我们插手。
    VTermState *state = vterm_obtain_state(m_vterm);
    if (state == nullptr) {
        return;
    }

    QByteArray resetSequence;
    VTermValue penValue {};
    if (vterm_state_get_penattr(state, VTERM_ATTR_FOREGROUND, &penValue) != 0
        && VTERM_COLOR_IS_DEFAULT_FG(&penValue.color)) {
        resetSequence += "\x1b[39m";
    }
    if (vterm_state_get_penattr(state, VTERM_ATTR_BACKGROUND, &penValue) != 0
        && VTERM_COLOR_IS_DEFAULT_BG(&penValue.color)) {
        resetSequence += "\x1b[49m";
    }
    if (resetSequence.isEmpty()) {
        return;
    }
    vterm_input_write(m_vterm, resetSequence.constData(),
                      static_cast<size_t>(resetSequence.size()));
}

void VtermBridge::setScrollbackLimit(const int lineCount)
{
    m_scrollbackLimit = std::clamp(lineCount, 100, 1000000);
    if (m_scrollback.size() > m_scrollbackLimit) {
        // 从头部批量删除比逐行 removeFirst 更高效；设置调小时可能一次裁掉数万行。
        // 这里不必标记文档失效：增量同步靠全局行号识别被淘汰的段，会自行从头部删除。
        m_scrollback.remove(0, m_scrollback.size() - m_scrollbackLimit);
    }
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
    // 顺序必须和 writeRenderedLine() 的追加顺序完全一致，剔除时才能精确匹配行尾。
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

void VtermBridge::sendKey(const VTermKey key, const VTermModifier modifiers)
{
    // vterm_keyboard_key 会读取自己保存的 DECCKM/DECPAM 状态并把编码结果交给
    // outputCallback，因此这些字节和 libvterm 生成的 DSR/DA 应答走同一条 protocolResponse
    // 通路，最终由 TerminalView 转成 inputBytes 发往 PTY。
    vterm_keyboard_key(m_vterm, key, modifiers);
}

void VtermBridge::beginPaste()
{
    vterm_keyboard_start_paste(m_vterm);
}

void VtermBridge::endPaste()
{
    vterm_keyboard_end_paste(m_vterm);
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

void VtermBridge::writeRenderedLine(QTextCursor &cursor, const RenderedLine &line)
{
    QTextBlockFormat blockFormat;
    blockFormat.setLineHeight(m_lineHeightPercent, QTextBlockFormat::ProportionalHeight);
    cursor.setBlockFormat(blockFormat);

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
        writeRenderedLine(cursor, buildRenderedLine(m_scrollback.at(index), -1));
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
        desiredTail.append(buildRenderedLine(m_scrollback.at(static_cast<qsizetype>(index)), -1));
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

VtermBridge::Cell VtermBridge::convertCell(const VTermScreenCell &cell) const
{
    Cell converted;

    // 双宽字符（中日韩、全角标点、部分表情）占两个终端格。libvterm 把右半格标记为
    // 0xFFFFFFFF 而不是 0，因此不能只靠“chars[0] 为 0”判空：那个哨兵值不是合法码位，
    // QString::fromUcs4 会把它转成 U+FFFD，于是每个汉字后面都跟出一个 � 占位符。
    constexpr uint32_t kWideCharContinuation = 0xffffffffU;
    if (cell.chars[0] == kWideCharContinuation) {
        // 宽度置 0，渲染循环据此跳过该格；右半格的内容已经由左半格一并绘制。
        converted.width = 0;
        return converted;
    }

    qsizetype characterCount = 0;
    while (characterCount < VTERM_MAX_CHARS_PER_CELL && cell.chars[characterCount] != 0) {
        ++characterCount;
    }
    if (characterCount > 0) {
        static_assert(sizeof(char32_t) == sizeof(cell.chars[0]));
        converted.text = QString::fromUcs4(
            reinterpret_cast<const char32_t *>(cell.chars), characterCount);
    }
    converted.foreground = toQColor(m_screen, cell.fg);
    converted.background = toQColor(m_screen, cell.bg);
    converted.bold = cell.attrs.bold;
    converted.italic = cell.attrs.italic;
    converted.underline = cell.attrs.underline != VTERM_UNDERLINE_OFF;
    converted.strike = cell.attrs.strike;
    converted.reverse = cell.attrs.reverse;
    converted.conceal = cell.attrs.conceal;
    converted.width = cell.width;
    return converted;
}

VtermBridge::Line VtermBridge::screenLine(const int row) const
{
    Line line;
    line.reserve(m_columns);
    for (int column = 0; column < m_columns; ++column) {
        VTermScreenCell cell {};
        vterm_screen_get_cell(m_screen, VTermPos{row, column}, &cell);
        line.append(convertCell(cell));
    }
    return line;
}

void VtermBridge::outputCallback(const char *bytes, const size_t length, void *user)
{
    auto *bridge = static_cast<VtermBridge *>(user);
    emit bridge->protocolResponse(QByteArray(bytes, static_cast<qsizetype>(length)));
}

int VtermBridge::damageCallback(VTermRect, void *)
{
    return 1;
}

int VtermBridge::moveRectCallback(VTermRect, VTermRect, void *)
{
    return 1;
}

int VtermBridge::moveCursorCallback(const VTermPos position,
                                    VTermPos,
                                    const int visible,
                                    void *user)
{
    auto *bridge = static_cast<VtermBridge *>(user);
    bridge->m_cursorRow = position.row;
    bridge->m_cursorColumn = position.col;
    bridge->m_cursorVisible = visible != 0;
    return 1;
}

int VtermBridge::termPropertyCallback(const VTermProp property, VTermValue *value, void *user)
{
    auto *bridge = static_cast<VtermBridge *>(user);
    if ((property == VTERM_PROP_TITLE || property == VTERM_PROP_ICONNAME)
        && value != nullptr && value->string.str != nullptr) {
        emit bridge->titleChanged(QString::fromUtf8(value->string.str,
                                                    static_cast<qsizetype>(value->string.len)));
    } else if (property == VTERM_PROP_CURSORVISIBLE && value != nullptr) {
        bridge->m_cursorVisible = value->boolean != 0;
    } else if (property == VTERM_PROP_ALTSCREEN && value != nullptr) {
        const bool active = value->boolean != 0;
        if (active != bridge->m_alternateScreen) {
            bridge->m_alternateScreen = active;
            // 主备屏的文档构成完全不同（备用屏不含滚动历史），必须整体重建。
            bridge->invalidateDocument();
            emit bridge->alternateScreenChanged(active);
        }
    }
    return 1;
}

int VtermBridge::bellCallback(void *user)
{
    emit static_cast<VtermBridge *>(user)->bell();
    return 1;
}

int VtermBridge::resizeCallback(const int rows, const int columns, void *user)
{
    auto *bridge = static_cast<VtermBridge *>(user);
    bridge->m_rows = rows;
    bridge->m_columns = columns;
    bridge->invalidateDocument();
    return 1;
}

int VtermBridge::pushScrollbackCallback(const int columns,
                                        const VTermScreenCell *cells,
                                        void *user)
{
    auto *bridge = static_cast<VtermBridge *>(user);
    Line line;
    line.reserve(columns);
    for (int column = 0; column < columns; ++column) {
        line.append(bridge->convertCell(cells[column]));
    }
    bridge->m_scrollback.append(std::move(line));
    // 只增不减的全局行号是增量同步的坐标系：文档里存的是哪几行、该从头部删几段、该从
    // 尾部补几段，全部由它推算。
    ++bridge->m_scrollbackPushed;
    if (bridge->m_scrollback.size() > bridge->m_scrollbackLimit) {
        bridge->m_scrollback.remove(0,
            bridge->m_scrollback.size() - bridge->m_scrollbackLimit);
    }
    return 1;
}

int VtermBridge::popScrollbackCallback(const int columns,
                                       VTermScreenCell *cells,
                                       void *user)
{
    auto *bridge = static_cast<VtermBridge *>(user);
    if (bridge->m_scrollback.isEmpty()) {
        return 0;
    }
    // libvterm 在扩容回流时需要原生单元格；当前历史保存为 Qt 值对象，无法无损恢复所有属性。
    // 返回 0 表示没有可回流行，历史仍保留在文档上方，不影响正常滚动查看。
    Q_UNUSED(columns)
    Q_UNUSED(cells)
    return 0;
}

int VtermBridge::clearScrollbackCallback(void *user)
{
    auto *bridge = static_cast<VtermBridge *>(user);
    bridge->m_scrollback.clear();
    // 历史被整体丢弃，文档里现存的段无一对应得上，只能整体重建。
    bridge->invalidateDocument();
    return 1;
}
