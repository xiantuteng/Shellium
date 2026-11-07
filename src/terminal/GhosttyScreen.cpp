#include "GhosttyScreen.h"

#include <algorithm>
#include <cstring>

namespace {

constexpr std::uint32_t kNominalCellWidthPx = 8;
constexpr std::uint32_t kNominalCellHeightPx = 16;
constexpr std::size_t kUnknownSequenceMaxBytes = 8192 + 16;

void appendCodepoint(std::string &out, std::uint32_t cp)
{
    if (cp < 0x80U) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

GhosttyColorRgb toGhostty(const GhosttyScreen::Rgb &c)
{
    return GhosttyColorRgb{c.r, c.g, c.b};
}

} // namespace

GhosttyScreen::GhosttyScreen(const int columns, const int rows)
    : m_columns(std::max(1, columns)), m_rows(std::max(1, rows))
{
    if (ghostty_terminal_new(nullptr, &m_terminal, static_cast<std::uint16_t>(m_columns),
                             static_cast<std::uint16_t>(m_rows)) != GHOSTTY_SUCCESS) {
        m_terminal = nullptr;
        return;
    }

    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_USERDATA, this);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                         reinterpret_cast<const void *>(&GhosttyScreen::writePtyThunk));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_BELL,
                         reinterpret_cast<const void *>(&GhosttyScreen::bellThunk));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED,
                         reinterpret_cast<const void *>(&GhosttyScreen::titleThunk));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_UNKNOWN_SEQUENCE,
                         reinterpret_cast<const void *>(&GhosttyScreen::unknownThunk));
    const std::size_t maxBytes = kUnknownSequenceMaxBytes;
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_UNKNOWN_MAX_BYTES, &maxBytes);

    ghostty_key_encoder_new(nullptr, &m_keyEncoder);
    ghostty_key_event_new(nullptr, &m_keyEvent);

}

GhosttyScreen::~GhosttyScreen()
{
    if (m_historyTracker != nullptr) {
        ghostty_tracked_grid_ref_free(m_historyTracker);
    }
    if (m_keyEvent != nullptr) {
        ghostty_key_event_free(m_keyEvent);
    }
    if (m_keyEncoder != nullptr) {
        ghostty_key_encoder_free(m_keyEncoder);
    }
    if (m_terminal != nullptr) {
        ghostty_terminal_free(m_terminal);
    }
}

void GhosttyScreen::feed(const char *bytes, const std::size_t length)
{
    if (m_terminal == nullptr || length == 0) {
        return;
    }
    ghostty_terminal_vt_write(m_terminal, reinterpret_cast<const std::uint8_t *>(bytes), length);
}

void GhosttyScreen::resize(const int columns, const int rows)
{
    if (m_terminal == nullptr) {
        return;
    }
    m_columns = std::max(1, columns);
    m_rows = std::max(1, rows);
    ghostty_terminal_resize(m_terminal, static_cast<std::uint16_t>(m_columns),
                            static_cast<std::uint16_t>(m_rows), kNominalCellWidthPx, kNominalCellHeightPx);
}

void GhosttyScreen::reset()
{
    if (m_terminal != nullptr) {
        ghostty_terminal_reset(m_terminal);
    }
}

void GhosttyScreen::setDefaultColors(const Rgb foreground, const Rgb background)
{
    if (m_terminal == nullptr) {
        return;
    }
    const GhosttyColorRgb fg = toGhostty(foreground);
    const GhosttyColorRgb bg = toGhostty(background);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &fg);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &bg);
}

void GhosttyScreen::setAnsiPalette(const std::vector<Rgb> &colors)
{
    if (m_terminal == nullptr || colors.size() != 16) {
        return;
    }
    GhosttyColorRgb palette[256];
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_COLOR_PALETTE_DEFAULT, palette);
    for (std::size_t i = 0; i < 16; ++i) {
        palette[i] = toGhostty(colors[i]);
    }
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, palette);
}

void GhosttyScreen::setScrollbackMaxLines(const std::size_t lines)
{
    if (m_terminal != nullptr) {
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &lines);
    }
}

bool GhosttyScreen::modeEnabled(const GhosttyMode mode) const
{
    if (m_terminal == nullptr) {
        return false;
    }
    GhosttyTerminalModeConfig config{};
    config.mode = mode;
    if (ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_MODE, &config) != GHOSTTY_SUCCESS) {
        return false;
    }
    return config.value;
}

bool GhosttyScreen::isAlternateScreen() const
{
    if (m_terminal == nullptr) {
        return false;
    }
    GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen);
    return screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE;
}

GhosttyScreen::Cursor GhosttyScreen::cursor() const
{
    Cursor result;
    if (m_terminal == nullptr) {
        return result;
    }
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    bool visible = true;
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_X, &x);
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_Y, &y);
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_VISIBLE, &visible);
    result.column = x;
    result.row = y;
    result.visible = visible;
    return result;
}

std::size_t GhosttyScreen::scrollbackRows() const
{
    if (m_terminal == nullptr) {
        return 0;
    }
    std::size_t rows = 0;
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS, &rows);
    return rows;
}

GhosttyScreen::Rgb GhosttyScreen::resolve(const GhosttyStyleColor &color,
                                          const GhosttyColorRgb *palette)
{
    switch (color.tag) {
    case GHOSTTY_STYLE_COLOR_RGB:
        return Rgb{color.value.rgb.r, color.value.rgb.g, color.value.rgb.b, true};
    case GHOSTTY_STYLE_COLOR_PALETTE: {
        const GhosttyColorRgb &c = palette[color.value.palette];
        return Rgb{c.r, c.g, c.b, true};
    }
    default:
        return Rgb{};
    }
}

GhosttyScreen::Row GhosttyScreen::readRow(const GhosttyPoint &basePoint) const
{
    Row row;
    row.reserve(static_cast<std::size_t>(m_columns));
    if (m_terminal == nullptr) {
        return row;
    }

    // 调色板可能被远端 OSC 4 改写，每行现取一次，避免缓存过期。
    GhosttyColorRgb palette[256] = {};
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_COLOR_PALETTE, palette);

    for (int x = 0; x < m_columns; ++x) {
        GhosttyPoint point = basePoint;
        point.value.coordinate.x = static_cast<std::uint16_t>(x);

        GhosttyGridRef ref{};
        if (ghostty_terminal_grid_ref(m_terminal, point, &ref) != GHOSTTY_SUCCESS) {
            break;
        }

        GhosttyCell raw{};
        if (ghostty_grid_ref_cell(&ref, &raw) != GHOSTTY_SUCCESS) {
            break;
        }

        GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
        ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide);
        if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL || wide == GHOSTTY_CELL_WIDE_SPACER_HEAD) {
            Cell spacer;
            spacer.width = 0; // 占位格：渲染循环据此跳过，列号保持对齐
            row.push_back(std::move(spacer));
            continue;
        }

        Cell cell;
        cell.width = wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : 1;

        std::uint32_t buffer[32];
        std::size_t written = 0;
        if (ghostty_grid_ref_graphemes(&ref, buffer, std::size(buffer), &written) == GHOSTTY_SUCCESS) {
            for (std::size_t i = 0; i < written && i < std::size(buffer); ++i) {
                appendCodepoint(cell.utf8, buffer[i]);
            }
        }

        GhosttyStyle style{};
        ghostty_style_default(&style);
        ghostty_grid_ref_style(&ref, &style);
        cell.foreground = resolve(style.fg_color, palette);
        cell.background = resolve(style.bg_color, palette);
        cell.bold = (style.flags & GHOSTTY_STYLE_FLAG_BOLD) != 0;
        cell.italic = (style.flags & GHOSTTY_STYLE_FLAG_ITALIC) != 0;
        cell.reverse = (style.flags & GHOSTTY_STYLE_FLAG_INVERSE) != 0;
        cell.conceal = (style.flags & GHOSTTY_STYLE_FLAG_INVISIBLE) != 0;
        cell.strike = (style.flags & GHOSTTY_STYLE_FLAG_STRIKETHROUGH) != 0;
        cell.underline = GHOSTTY_STYLE_UNDERLINE(style) != 0;
        row.push_back(std::move(cell));
    }
    return row;
}

GhosttyScreen::Row GhosttyScreen::activeRow(const int row) const
{
    if (row < 0 || row >= m_rows) {
        return {};
    }
    GhosttyPoint point{};
    point.tag = GHOSTTY_POINT_TAG_ACTIVE;
    point.value.coordinate.y = static_cast<std::uint32_t>(row);
    return readRow(point);
}

GhosttyScreen::Row GhosttyScreen::historyRow(const std::size_t index) const
{
    if (index >= scrollbackRows()) {
        return {};
    }
    GhosttyPoint point{};
    point.tag = GHOSTTY_POINT_TAG_HISTORY;
    point.value.coordinate.y = static_cast<std::uint32_t>(index);
    return readRow(point);
}

bool GhosttyScreen::rowContinues(const int row) const
{
    if (m_terminal == nullptr || row < 0 || row >= m_rows) {
        return false;
    }
    GhosttyPoint point{};
    point.tag = GHOSTTY_POINT_TAG_ACTIVE;
    point.value.coordinate.y = static_cast<std::uint32_t>(row);
    GhosttyGridRef ref{};
    if (ghostty_terminal_grid_ref(m_terminal, point, &ref) != GHOSTTY_SUCCESS) {
        return false;
    }
    GhosttyRow raw{};
    if (ghostty_grid_ref_row(&ref, &raw) != GHOSTTY_SUCCESS) {
        return false;
    }
    bool continuation = false;
    ghostty_row_get(raw, GHOSTTY_ROW_DATA_WRAP_CONTINUATION, &continuation);
    return continuation;
}

void GhosttyScreen::retrackHistory(const std::size_t total)
{
    if (total == 0) {
        if (m_historyTracker != nullptr) {
            ghostty_tracked_grid_ref_free(m_historyTracker);
            m_historyTracker = nullptr;
        }
        return;
    }
    GhosttyPoint point{};
    point.tag = GHOSTTY_POINT_TAG_HISTORY;
    point.value.coordinate.y = static_cast<std::uint32_t>(total - 1);
    if (m_historyTracker == nullptr) {
        if (ghostty_terminal_grid_ref_track(m_terminal, point, &m_historyTracker) != GHOSTTY_SUCCESS) {
            m_historyTracker = nullptr;
        }
    } else {
        ghostty_tracked_grid_ref_set(m_historyTracker, m_terminal, point);
    }
}

std::vector<GhosttyScreen::Row> GhosttyScreen::takeNewHistoryRows(bool *historyErased)
{
    if (historyErased != nullptr) {
        *historyErased = false;
    }
    std::vector<Row> rows;
    if (m_terminal == nullptr || isAlternateScreen()) {
        return rows;
    }
    const std::size_t total = scrollbackRows();
    std::size_t start = 0;
    if (historyErased != nullptr && m_historyTracker != nullptr && total == 0) {
        *historyErased = true;
    }
    if (m_historyTracker != nullptr) {
        GhosttyPointCoordinate coordinate{};
        if (ghostty_tracked_grid_ref_point(m_historyTracker, GHOSTTY_POINT_TAG_HISTORY, &coordinate)
            == GHOSTTY_SUCCESS) {
            start = static_cast<std::size_t>(coordinate.y) + 1;
        }
    }
    start = std::min(start, total);
    rows.reserve(total - start);
    for (std::size_t index = start; index < total; ++index) {
        rows.push_back(historyRow(index));
    }
    retrackHistory(total);
    return rows;
}

std::string GhosttyScreen::encodeKey(const GhosttyKey key, const GhosttyMods mods) const
{
    if (m_terminal == nullptr || m_keyEncoder == nullptr || m_keyEvent == nullptr) {
        return {};
    }
    ghostty_key_encoder_set_from_terminal(m_keyEncoder, m_terminal);
    ghostty_key_event_set_action(m_keyEvent, GHOSTTY_KEY_ACTION_PRESS);
    ghostty_key_event_set_key(m_keyEvent, key);
    ghostty_key_event_set_mods(m_keyEvent, mods);

    std::uint8_t buf[64];
    std::size_t written = 0;
    if (ghostty_key_encoder_encode(m_keyEncoder, m_keyEvent, buf, sizeof(buf), &written)
        != GHOSTTY_SUCCESS) {
        return {};
    }
    return std::string(reinterpret_cast<const char *>(buf), written);
}

std::string GhosttyScreen::pasteMarker(const bool begin) const
{
    if (!modeEnabled(GHOSTTY_MODE_BRACKETED_PASTE)) {
        return {};
    }
    return begin ? "\x1b[200~" : "\x1b[201~";
}

void GhosttyScreen::writePtyThunk(GhosttyTerminal, void *userdata, const std::uint8_t *data,
                                  const std::size_t length)
{
    auto *self = static_cast<GhosttyScreen *>(userdata);
    if (self->onWritePty && length > 0) {
        self->onWritePty(std::string(reinterpret_cast<const char *>(data), length));
    }
}

void GhosttyScreen::bellThunk(GhosttyTerminal, void *userdata)
{
    auto *self = static_cast<GhosttyScreen *>(userdata);
    if (self->onBell) {
        self->onBell();
    }
}

void GhosttyScreen::titleThunk(GhosttyTerminal terminal, void *userdata)
{
    auto *self = static_cast<GhosttyScreen *>(userdata);
    if (!self->onTitleChanged) {
        return;
    }
    GhosttyString title{};
    if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_TITLE, &title) == GHOSTTY_SUCCESS
        && title.ptr != nullptr) {
        self->onTitleChanged(std::string(reinterpret_cast<const char *>(title.ptr), title.len));
    } else {
        self->onTitleChanged({});
    }
}

void GhosttyScreen::unknownThunk(GhosttyTerminal, void *userdata,
                                 const GhosttyTerminalUnknownSequence *sequence)
{
    auto *self = static_cast<GhosttyScreen *>(userdata);
    if (sequence == nullptr || sequence->tag != GHOSTTY_TERMINAL_UNKNOWN_SEQUENCE_OSC
        || !self->onUnknownOsc) {
        return;
    }
    const GhosttyTerminalUnknownOscSequence &osc = sequence->value.osc;
    if (osc.truncated || osc.content.ptr == nullptr) {
        return; // 残缺的私有序列宁可丢弃，也不能把半截 payload 交给上层解析
    }
    self->onUnknownOsc(
        std::string(reinterpret_cast<const char *>(osc.content.ptr), osc.content.len));
}
