#include "terminal/VtermBridge.h"

#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>

#include <algorithm>

namespace {

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
}

void VtermBridge::setDefaultColors(const QColor &foreground,
                                   const QColor &background,
                                   const QColor &cursorColor)
{
    const QColor safeForeground = foreground.isValid() ? foreground : QColor(Qt::white);
    const QColor safeBackground = background.isValid() ? background : QColor(Qt::black);
    m_cursorColor = cursorColor.isValid() ? cursorColor : safeForeground;

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
}

void VtermBridge::setScrollbackLimit(const int lineCount)
{
    m_scrollbackLimit = std::clamp(lineCount, 100, 1000000);
    if (m_scrollback.size() > m_scrollbackLimit) {
        // 从头部批量删除比逐行 removeFirst 更高效；设置调小时可能一次裁掉数万行。
        m_scrollback.remove(0, m_scrollback.size() - m_scrollbackLimit);
    }
}

void VtermBridge::setLineHeightPercent(const int percent)
{
    m_lineHeightPercent = std::clamp(percent, 80, 250);
}

void VtermBridge::setCursorPresentation(const CursorShape shape, const bool phaseVisible)
{
    m_cursorShape = shape;
    m_cursorPhaseVisible = phaseVisible;
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

void VtermBridge::render(QTextDocument *document) const
{
    if (document == nullptr) {
        return;
    }

    document->clear();
    m_renderedCursorPosition = -1;
    QTextCursor cursor(document);
    cursor.beginEditBlock();

    const auto insertLine = [this, &cursor](const Line &line,
                                            const int visualRow,
                                            const bool currentScreen) {
        QTextBlockFormat blockFormat;
        blockFormat.setLineHeight(m_lineHeightPercent, QTextBlockFormat::ProportionalHeight);
        cursor.setBlockFormat(blockFormat);
        int lastContentColumn = -1;
        for (int column = 0; column < line.size(); ++column) {
            if (!line.at(column).text.trimmed().isEmpty()
                || (currentScreen && visualRow == m_cursorRow && column <= m_cursorColumn)) {
                lastContentColumn = column;
            }
        }

        for (int column = 0; column <= lastContentColumn; ++column) {
            const Cell &cell = line.at(column);
            if (cell.width == 0) {
                continue;
            }
            QColor foreground = cell.foreground;
            QColor background = cell.background;
            const bool cursorCell = currentScreen && m_cursorVisible && m_cursorPhaseVisible
                && visualRow == m_cursorRow && column == m_cursorColumn;
            if (cell.reverse) {
                std::swap(foreground, background);
            }
            if (cursorCell) {
                m_renderedCursorPosition = cursor.position();
                if (m_cursorShape == CursorShape::Block) {
                    // 块状光标使用可配置光标色；文本采用该单元格原背景色以保证深浅主题下
                    // 都有对比度，同时不修改 libvterm 的真实屏幕状态。
                    foreground = background;
                    background = m_cursorColor;
                }
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
            if (cursorCell && m_cursorShape == CursorShape::Underline) {
                format.setFontUnderline(true);
                format.setUnderlineColor(m_cursorColor);
                format.setUnderlineStyle(QTextCharFormat::SingleUnderline);
            }
            format.setFontStrikeOut(cell.strike);
            cursor.insertText(cell.text.isEmpty() ? QStringLiteral(" ") : cell.text, format);
        }
    };

    for (const Line &line : m_scrollback) {
        insertLine(line, -1, false);
        cursor.insertBlock();
    }
    int lastScreenRow = std::clamp(m_cursorRow, 0, m_rows - 1);
    for (int row = lastScreenRow + 1; row < m_rows; ++row) {
        const Line candidate = screenLine(row);
        const bool hasContent = std::any_of(candidate.cbegin(), candidate.cend(), [](const Cell &cell) {
            return !cell.text.trimmed().isEmpty();
        });
        if (hasContent) {
            lastScreenRow = row;
        }
    }
    for (int row = 0; row <= lastScreenRow; ++row) {
        insertLine(screenLine(row), row, true);
        if (row < lastScreenRow) {
            cursor.insertBlock();
        }
    }
    cursor.endEditBlock();
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
    static_cast<VtermBridge *>(user)->m_scrollback.clear();
    return 1;
}
