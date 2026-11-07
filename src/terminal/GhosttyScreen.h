#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <ghostty/vt.h>

/**
 * @brief libghostty-vt 的薄封装，不依赖 Qt，只暴露终端桥接层需要的最小接口。
 *
 * 线程模型：libghostty-vt 非线程安全，所有调用必须发生在同一线程（GUI 线程）。
 * 回调在 feed() 内同步触发，回调内禁止再次调用 feed()。
 */
class GhosttyScreen final {
public:
    /// 颜色解析后的 RGB；valid=false 表示“终端默认色”，由调用方按主题处理。
    struct Rgb {
        std::uint8_t r = 0;
        std::uint8_t g = 0;
        std::uint8_t b = 0;
        bool valid = false;
    };

    struct Cell {
        std::string utf8;        ///< 空串表示空白格
        Rgb foreground;
        Rgb background;
        bool bold = false;
        bool italic = false;
        bool underline = false;
        bool strike = false;
        bool reverse = false;
        bool conceal = false;
        int width = 1;           ///< 宽字符为 2；宽字符的尾随占位格不会出现在结果里
    };

    using Row = std::vector<Cell>;

    struct Cursor {
        int row = 0;
        int column = 0;
        bool visible = true;
    };

    GhosttyScreen(int columns, int rows);
    ~GhosttyScreen();
    GhosttyScreen(const GhosttyScreen &) = delete;
    GhosttyScreen &operator=(const GhosttyScreen &) = delete;

    [[nodiscard]] bool isValid() const { return m_terminal != nullptr; }

    void feed(const char *bytes, std::size_t length);
    void resize(int columns, int rows);
    void reset();

    void setDefaultColors(Rgb foreground, Rgb background);
    /// 恰好 16 个 ANSI 色；其余 240 个保持库默认。
    void setAnsiPalette(const std::vector<Rgb> &colors);
    /// 0 表示不保留历史。
    void setScrollbackMaxLines(std::size_t lines);

    [[nodiscard]] int columns() const { return m_columns; }
    [[nodiscard]] int rows() const { return m_rows; }
    [[nodiscard]] bool isAlternateScreen() const;
    [[nodiscard]] Cursor cursor() const;
    /// 主屏历史行数（备用屏没有历史）。
    [[nodiscard]] std::size_t scrollbackRows() const;

    /// 读取当前活动区第 row 行（0 起）。越界返回空行。
    [[nodiscard]] Row activeRow(int row) const;
    /// 读取历史第 index 行（0 为最旧）。越界返回空行。
    [[nodiscard]] Row historyRow(std::size_t index) const;

    /// 按当前远端模式（DECCKM/DECPAM/Kitty 等）编码按键，返回应发送给远端的字节。
    [[nodiscard]] std::string encodeKey(GhosttyKey key, GhosttyMods mods) const;
    /// 括号粘贴起止标记；远端未开启 DECSET 2004 时返回空串。
    [[nodiscard]] std::string pasteMarker(bool begin) const;

    std::function<void(const std::string &)> onWritePty;
    std::function<void()> onBell;
    std::function<void(const std::string &)> onTitleChanged;
    /// 未识别 OSC（含编号，不含 ESC ] 与终止符）；用于私有 OSC 6773。
    std::function<void(const std::string &)> onUnknownOsc;

private:
    static void writePtyThunk(GhosttyTerminal, void *, const std::uint8_t *, std::size_t);
    static void bellThunk(GhosttyTerminal, void *);
    static void titleThunk(GhosttyTerminal, void *);
    static void unknownThunk(GhosttyTerminal, void *, const GhosttyTerminalUnknownSequence *);

    [[nodiscard]] Row readRow(const GhosttyPoint &point) const;
    [[nodiscard]] bool modeEnabled(GhosttyMode mode) const;
    [[nodiscard]] Rgb resolve(const GhosttyStyleColor &color) const;

    GhosttyTerminal m_terminal = nullptr;
    GhosttyKeyEncoder m_keyEncoder = nullptr;
    GhosttyKeyEvent m_keyEvent = nullptr;
    int m_columns = 80;
    int m_rows = 24;
    GhosttyColorRgb m_palette[256] = {};
};
