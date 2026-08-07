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
    explicit VtermBridge(QObject *parent = nullptr);
    ~VtermBridge() override;

    VtermBridge(const VtermBridge &) = delete;
    VtermBridge &operator=(const VtermBridge &) = delete;

    /** @brief 把远端或本地 PTY 原始输出送入终端状态机。 */
    void feed(const QByteArray &bytes);

    /** @brief 改变终端字符尺寸并触发 libvterm 重排可重排内容。 */
    void resize(int rows, int columns);

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

    VTerm *m_vterm = nullptr;
    VTermScreen *m_screen = nullptr;
    QVector<Line> m_scrollback;
    int m_rows = 24;
    int m_columns = 80;
    int m_cursorRow = 0;
    int m_cursorColumn = 0;
    bool m_cursorVisible = true;
    static constexpr int MaxScrollbackLines = 10000;
};
