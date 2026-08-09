#pragma once

#include <QColor>
#include <QObject>
#include <QVector>

#include <vterm.h>

class QTextDocument;

/**
 * @brief 封装 libvterm 的 VT220/xterm 状态机，并生成可由 Qt 绘制的终端文档。
 *
 * 所有 feed/resize/render 调用均发生在 GUI 线程；SSH 与本地 PTY 的输出通过 Qt 队列信号
 * 进入该线程，因此 libvterm 实例无需额外锁。桥接层维护 10000 行滚动历史，并正确处理
 * 光标移动、清屏、备用屏幕、256 色和 TrueColor。
 */
class VtermBridge final : public QObject {
    Q_OBJECT

public:
    /** @brief 终端光标的绘制形态；桥接层使用独立枚举以避免依赖设置 UI。 */
    enum class CursorShape {
        Block,     ///< 使用高亮背景覆盖当前单元格。
        Underline, ///< 在当前单元格底部绘制横线。
        IBeam      ///< 由 TerminalView 在当前单元格左侧绘制竖线。
    };

    explicit VtermBridge(QObject *parent = nullptr);
    ~VtermBridge() override;

    VtermBridge(const VtermBridge &) = delete;
    VtermBridge &operator=(const VtermBridge &) = delete;

    /** @brief 把远端或本地 PTY 原始输出送入终端状态机。 */
    void feed(const QByteArray &bytes);

    /** @brief 改变终端字符尺寸并触发 libvterm 重排可重排内容。 */
    void resize(int rows, int columns);

    /**
     * @brief 更新终端默认色与光标颜色。
     * @param foreground 未显式指定 ANSI 颜色的文本颜色。
     * @param background 未显式指定 ANSI 颜色的背景颜色。
     * @param cursorColor 光标绘制颜色。
     */
    void setDefaultColors(const QColor &foreground,
                          const QColor &background,
                          const QColor &cursorColor);

    /** @brief 设置滚动历史上限，并立即丢弃超过新上限的最旧行。 */
    void setScrollbackLimit(int lineCount);

    /** @brief 设置行高百分比；异常值会限制在 80% 到 250%。 */
    void setLineHeightPercent(int percent);

    /** @brief 设置光标形态和闪烁相位；不会覆盖远端要求隐藏光标的状态。 */
    void setCursorPresentation(CursorShape shape, bool phaseVisible);

    /**
     * @brief 当前是否处于备用屏（vim、top、less 等全屏程序会切换到该屏）。
     *
     * 备用屏与主屏的显示契约完全不同：主屏是一条不断向下增长的历史，用户按底部对齐阅读；
     * 备用屏则是一块尺寸恰好等于 PTY 网格的画布，程序按绝对行列坐标自行绘制，行 0 必须
     * 落在视口顶部。两者混在一起渲染会让网格整体下沉到滚动历史之下，被顶部裁掉的正是
     * 光标最常停留的前几行。
     */
    [[nodiscard]] bool isAlternateScreen() const { return m_alternateScreen; }

    /**
     * @brief 按 libvterm 的键盘模型编码一个特殊键，并通过 protocolResponse 发出字节。
     * @param key libvterm 键枚举，例如 VTERM_KEY_UP、VTERM_KEY_PAGEUP。
     * @param modifiers libvterm 修饰键掩码。
     *
     * 方向键、Home/End 和小键盘的字节形式取决于远端是否开启了 DECCKM（应用光标键）与
     * DECPAM（应用小键盘）——vim 进入全屏后就会开启。硬编码 CSI 形式会让 vim 在插入模式
     * 下把方向键读成 ESC 加一个普通字母，表现为方向键不动光标反而插入乱码。libvterm 自己
     * 保存着这些模式，交给它编码是唯一不会随远端模式漂移的做法。
     */
    void sendKey(VTermKey key, VTermModifier modifiers = VTERM_MOD_NONE);

    /**
     * @brief 发出括号粘贴的起止标记，仅在远端开启了 DECSET 2004 时才真正产生字节。
     *
     * vim 进入插入模式后会把粘贴内容当成逐个按键处理，autoindent 于是给每一行叠加一层
     * 缩进，十行代码粘完变成阶梯状。括号粘贴让远端知道这段内容来自剪贴板而非键盘。
     * libvterm 保存着远端是否开启该模式，未开启时这两个调用不产生任何输出。
     */
    void beginPaste();
    void endPaste();

    /**
     * @brief 返回最近一次 render() 中光标对应的 QTextDocument 字符位置。
     * @return 光标没有进入已渲染区域时返回 -1。
     *
     * IBeam 无法仅靠 QTextCharFormat 表达，因此 TerminalView 使用该位置叠加竖线。
     */
    [[nodiscard]] int renderedCursorPosition() const { return m_renderedCursorPosition; }

    /**
     * @brief 返回终端光标当前所在屏幕行的可见文本。
     * @return 已移除行尾空白的文本；光标位置无效时返回空字符串。
     *
     * 该值来自 libvterm 屏幕模型，而不是 QTextDocument 的最后一段，因此即使屏幕下方仍有
     * 旧内容，也能准确取得用户正在输入、且远端 PTY 实际回显的命令行。
     */
    [[nodiscard]] QString cursorLineText() const;

    /**
     * @brief 将滚动历史和当前屏幕完整渲染到 QTextDocument。
     * @param document 终端视图持有的文档，调用期间会整体重建。
     */
    void render(QTextDocument *document) const;

signals:
    /** @brief libvterm 为设备状态查询、光标位置查询等生成的协议响应。 */
    void protocolResponse(const QByteArray &bytes);

    /** @brief 终端标题通过 OSC 序列发生变化。 */
    void titleChanged(const QString &title);

    /** @brief 收到 BEL 控制字符，供界面实现视觉或声音铃声。 */
    void bell();

    /**
     * @brief 远端 Shell 集成脚本上报了一条事件。
     * @param payload OSC 载荷，形如 `connect;<以 0x1e 分隔的 argv>`。
     *
     * 这是把“用户想连哪台机器”从猜测变成事实的关键通路：载荷里的参数是远端 Shell 自己
     * 展开好的真实 argv，alias、变量、历史记录调用、输入法都不会再造成偏差。
     */
    void shellIntegrationEvent(const QByteArray &payload);

    /**
     * @brief 远端进入或退出备用屏。
     * @param active true 表示 vim 之类的全屏程序已接管屏幕。
     *
     * TerminalView 据此切换到"网格即视口"的显示方式，并停止把按键当作 Shell 命令行解析。
     */
    void alternateScreenChanged(bool active);

private:
    /** @brief 与 VTermScreenCell 解耦的滚动历史单元格。 */
    struct Cell {
        QString text;
        QColor foreground;
        QColor background;
        bool bold = false;
        bool italic = false;
        bool underline = false;
        bool strike = false;
        bool reverse = false;
        bool conceal = false;
        int width = 1;
    };

    using Line = QVector<Cell>;

    /** @brief 将 libvterm 单元格转换为可长期保存的 Qt 值对象。 */
    [[nodiscard]] Cell convertCell(const VTermScreenCell &cell) const;

    /** @brief 读取当前屏幕指定行。 */
    [[nodiscard]] Line screenLine(int row) const;

    /** @brief libvterm 输出协议响应时调用。 */
    static void outputCallback(const char *bytes, size_t length, void *user);
    static int damageCallback(VTermRect rect, void *user);
    static int moveRectCallback(VTermRect destination, VTermRect source, void *user);
    static int moveCursorCallback(VTermPos position, VTermPos oldPosition, int visible, void *user);
    static int termPropertyCallback(VTermProp property, VTermValue *value, void *user);
    static int bellCallback(void *user);
    static int resizeCallback(int rows, int columns, void *user);
    static int pushScrollbackCallback(int columns, const VTermScreenCell *cells, void *user);
    static int popScrollbackCallback(int columns, VTermScreenCell *cells, void *user);
    static int clearScrollbackCallback(void *user);

    /**
     * @brief libvterm 未识别 OSC 的回调；Shell 集成事件由此进入。
     * @return 恒返回 1 表示已消费，避免序列被当作可打印文本渲染到屏幕上。
     */
    static int oscCallback(int command, VTermStringFragment fragment, void *user);

    VTerm *m_vterm = nullptr;
    VTermScreen *m_screen = nullptr;

    /**
     * @brief 跨分片累积的 OSC 载荷。
     *
     * libvterm 会把一条 OSC 按网络分包切成多个 fragment 逐次回调，必须等到 final 标志
     * 才能解析；否则被拆开的命令行会被解析成半截主机名。
     */
    QByteArray m_oscBuffer;
    QVector<Line> m_scrollback;
    int m_rows = 24;
    int m_columns = 80;
    int m_cursorRow = 0;
    int m_cursorColumn = 0;
    bool m_cursorVisible = true;
    bool m_alternateScreen = false;
    int m_scrollbackLimit = 10000;
    int m_lineHeightPercent = 100;
    CursorShape m_cursorShape = CursorShape::Block;
    bool m_cursorPhaseVisible = true;
    QColor m_cursorColor = QColor(QStringLiteral("#e6e6e6"));

    /** @brief 最近一次渲染得到的光标文档位置；render() 为 const，因此缓存声明为 mutable。 */
    mutable int m_renderedCursorPosition = -1;
};
