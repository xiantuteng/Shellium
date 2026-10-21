#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QObject>
#include <QVector>

#include <cstddef>
#include <deque>
#include <vterm.h>

class QTextBlock;
class QTextCharFormat;
class QTextCursor;
class QTextDocument;

/**
 * @brief 封装 libvterm 的 VT220/xterm 状态机，并生成可由 Qt 绘制的终端文档。
 *
 * 所有 feed/resize/render 调用均发生在 GUI 线程；SSH 与本地 PTY 的输出通过 Qt 队列信号
 * 进入该线程，因此 libvterm 实例无需额外锁。桥接层维护有界滚动历史（默认按行数与字节
 * 双上限裁剪），并正确处理光标移动、清屏、备用屏幕、256 色和 TrueColor。
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

    /**
     * @brief 结束异常控制序列并重置终端模式、画笔和当前屏幕，保留已有滚动历史。
     *
     * 无参数、无返回值；仅供用户主动恢复或会话边界调用，不得按网络分片自动调用，
     * 否则会破坏正常跨包 UTF-8、颜色和全屏程序状态。当前可见主屏归档后再清屏，
     * 便于断线后追查原始输出；已经写入历史的显式颜色不会被重写。
     */
    void resetTerminal();

    /**
     * @brief 自上次复位以来，是否曾强制中止过一段残缺的 OSC/DCS 字符串。
     *
     * 由 boundUnterminatedString() 在识别出二进制噪音时置位，resetTerminal() 清零。
     * 调用方（TerminalPane）借此在下一次 Shell 提示符出现时——也就是造成污染的那条命令
     * 已经退出——自动恢复一次，而不是对每条命令都盲目复位。
     */
    [[nodiscard]] bool hasPendingCorruption() const { return m_corruptionDetected; }

    /** @brief 改变终端字符尺寸并触发 libvterm 重排可重排内容。 */
    void resize(int rows, int columns);

    /**
     * @brief 更新终端默认前景与背景色，并让已渲染文档整体失效。
     * @param foreground 未显式指定 ANSI 颜色的文本颜色。
     * @param background 未显式指定 ANSI 颜色的背景颜色。
     *
     * 光标颜色不在这里：光标由 TerminalView 叠加绘制，不参与文档内容。
     */
    void setDefaultColors(const QColor &foreground, const QColor &background);

    /**
     * @brief 覆盖 ANSI 0-15 号色，使远端显式着色的文本也跟随当前主题。
     * @param colors 恰好 16 个有效颜色；数量不足或含无效色时整体忽略，保留上一次的调色板。
     *
     * libvterm 内置的是 VGA 那套值，其中 ANSI 蓝 (#0000cd) 落在任何深色背景上都几乎读不出
     * 来——`ls` 的目录名正是用它。主题只改默认前景背景色是不够的，必须连这 16 号色一起换。
     */
    void setAnsiPalette(const QVector<QColor> &colors);

    /** @brief 设置滚动历史行数上限，并立即丢弃超过新上限的最旧行。 */
    void setScrollbackLimit(int lineCount);

    /**
     * @brief 设置滚动历史近似字节上限；0 表示仅按行数裁剪。
     * @param maxBytes 历史 Cell 向量的软上限（含 QString 载荷估算）。
     *
     * 长日志行在行数远未触顶时就会把常驻内存顶爆；字节上限与行数上限取更严的一方。
     */
    void setScrollbackByteLimit(qint64 maxBytes);

    /** @brief 当前滚动历史行数（不含当前屏）。 */
    [[nodiscard]] int scrollbackLineCount() const { return static_cast<int>(m_scrollback.size()); }

    /** @brief 设置行高百分比；异常值会限制在 80% 到 250%。 */
    void setLineHeightPercent(int percent);

    /**
     * @brief 设置"行高锚点"，用于把每一段的渲染高度统一成同一个常数。
     * @param anchors 追加到每段末尾的 (字符, 字体) 序列；为空表示不加锚点。
     *
     * QPlainTextEdit 用的 QPlainTextDocumentLayout 完全忽略
     * QTextBlockFormat::setLineHeight()（试过 FixedHeight，传多大都不生效），一段的高度
     * 只由 QTextLine 决定，而 QTextLine 的高度是该行用到的所有字体里 ascent 最大值加
     * descent 最大值。于是纯 ASCII 行、含中文的行、含 emoji 的行天生互不相同（PT Mono
     * 15pt 实测分别是 27px / 32px / 33px）："30 行"的实际像素高度随内容而变，最后一行
     * （vim 状态行）时而被挤出视口，编辑时还会上下漂移。
     *
     * 行高只能被抬高、不可能被压低，因此统一高度的唯一下限就是最高的那种字体。这里在
     * 每一段末尾追加锚点字符并分别指定字体，使每一段都必然包含来自这些字体的字形段，
     * 高度因此恒等于最大值，与该行实际写了什么无关。
     *
     * 锚点字符必须真的存在于对应字体里，否则 Qt 会静默换字体、metrics 也就不是想要的
     * 那个：U+200B 不在 PingFang 里，用它当 CJK 锚点会让含中文的行比别人高 1px。
     * U+3000（表意空格）在 PingFang 里，才能拿到和真实中文完全一致的度量。
     */
    void setLineAnchors(const QVector<QPair<QChar, QFont>> &anchors);

    /**
     * @brief 每段末尾被追加的锚点字符序列；导出与复制时需要按行剔除这个后缀。
     *
     * 不能无差别地删除这些字符：U+3000 是合法的正文字符，用户自己敲的表意空格必须保留。
     * 只有出现在行尾、且顺序完全吻合的那一段才是渲染锚点。
     */
    [[nodiscard]] static QString lineAnchorSuffix();

    /**
     * @brief 判断字符格式是否属于内部行高锚点。
     * @param format 待检查的 QTextDocument 字符格式。
     * @return 由 writeRenderedLine() 写入的行高锚点返回 true，普通终端字符返回 false。
     *
     * 行高锚点使用真实 CJK 字形才能取得与正文完全一致的后备字体度量。视图在选区绘制后
     * 需要精确识别并遮住这些内部字形，不能仅凭字符内容判断，否则用户真实输入的“中”字
     * 也会被误认为锚点。
     */
    [[nodiscard]] static bool isLineAnchorFormat(const QTextCharFormat &format);

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
     * 三种光标形态全部由 TerminalView 在 paintEvent 中叠加绘制，这个位置是它们唯一的
     * 定位依据。光标不写进 QTextCharFormat 是刻意的：闪烁一旦需要改文档格式，就必须为
     * 每次相位翻转重建整份文档，而那正是长会话卡死的根源。
     */
    [[nodiscard]] int renderedCursorPosition() const { return m_renderedCursorPosition; }

    /**
     * @brief 光标所在单元格的字符；块状光标反色绘制时需要把它重新画在色块上。
     * @return 该格为空时返回空字符串，调用方按空格处理。
     */
    [[nodiscard]] QString renderedCursorText() const { return m_renderedCursorText; }

    /** @brief 光标所在单元格的原始背景色；块状光标用它作为字符颜色以保证对比度。 */
    [[nodiscard]] QColor renderedCursorBackground() const { return m_renderedCursorBackground; }

    /** @brief 远端是否要求显示光标（DECTCEM）；全屏程序绘制期间会临时隐藏。 */
    [[nodiscard]] bool isCursorVisible() const { return m_cursorVisible; }

    /** @brief libvterm 屏幕模型中的光标列（从 0 开始），不依赖 QTextDocument 是否已同步。 */
    [[nodiscard]] int cursorColumn() const { return m_cursorColumn; }

    /**
     * @brief 返回终端光标当前所在屏幕行的可见文本。
     * @return 已移除行尾空白的文本；光标位置无效时返回空字符串。
     *
     * 该值来自 libvterm 屏幕模型，而不是 QTextDocument 的最后一段，因此即使屏幕下方仍有
     * 旧内容，也能准确取得用户正在输入、且远端 PTY 实际回显的命令行。
     */
    [[nodiscard]] QString cursorLineText() const;

    /** @brief 光标所在逻辑行（自动折行的多段屏幕行拼接而成）及光标在其中的字符偏移。 */
    struct InputLine {
        QString text;
        qsizetype cursorOffset = 0;
    };

    /**
     * @brief 返回光标所在的逻辑行，保留行尾空白。
     *
     * 远端 Shell 的 Tab 补全、历史回溯与行编辑都在服务器上完成，本地按键缓冲看不到它们的
     * 结果；用户真正执行的命令只存在于 PTY 回显里。长命令（例如带完整 URL 的 wget）会被
     * 终端自动折到下一屏幕行，只读光标那一行会截掉前半段，因此按 libvterm 的续行标记把
     * 整条逻辑行拼回来。行尾空白不裁剪：提示符结尾的空格是调用方比对提示符前缀的依据。
     */
    [[nodiscard]] InputLine cursorInputLine() const;

    /**
     * @brief 把滚动历史与当前屏幕同步到 QTextDocument。
     * @param document 终端视图持有的文档。
     * @return true 表示本次是整体重建，false 表示只做了增量更新。
     *
     * 默认走增量：滚动历史一旦推入就不再变化，文档里对应的段可以原样留着，只需要删掉
     * 被上限淘汰的最旧几段、补上新推入的几段，再重建尾部那一屏。代价从 O(全部历史)
     * 降到 O(本次新增行 + 一屏)——这是 `tail -f` 之类持续输出不再卡死界面的关键。
     *
     * 只有在字号、行高、配色、网格尺寸或主备屏切换等会让既有段全部失效的场景，才由
     * invalidateDocument() 标记为整体重建。返回值让视图知道是否需要恢复选区：增量更新
     * 时 QTextCursor 会自行随编辑调整，整体重建则必须按偏移重新夹取。
     */
    bool render(QTextDocument *document);

    /**
     * @brief 下一次 render() 是否会让当前屏幕少掉已经画出来的行。
     *
     * 多行进度条重绘的第一包常常是“光标上移 N 行 + 擦到屏尾”。libvterm 此刻的屏幕比文档里
     * 那一屏短；若马上同步进文档，贴底的视图会先上跳，下一包把进度条写回来再弹下去。
     * 调用方可以据此把这一帧按住，等补齐或超时后再画。尚未渲染过、文档已整体失效、或正处于
     * 备用屏时返回 false：这些场景没有“已经画出来的行”可供比较。
     */
    [[nodiscard]] bool renderedScreenWouldShrink() const;

    /**
     * @brief 标记文档已全部失效，下一次 render() 整体重建。
     *
     * 字号、行高、配色这类改动会让已经写进文档的每一段都过期，增量更新无法表达。
     */
    void invalidateDocument();

    /**
     * @brief 原地重写文档中一段滚动历史的段，让 Qt 释放它们的排版缓存。
     * @param document 最近一次 render() 同步过的文档。
     * @param firstBlock 起始段号（含）。
     * @param lastBlock 结束段号（含）；超出历史区间的部分自动忽略，尾部那一屏永远不动。
     * @return 实际重写的段数；文档未同步、处于备用屏等无法安全重写时返回 0。
     *
     * 这是多标签内存的第二大来源。QPlainTextEdit 每排版一段，该段的 QTextEngine 就会
     * 留下一块 HarfBuzz 塑形缓冲（hb_buffer，按行长约 3–6 KB），直到这一段被删除才释放；
     * QTextLayout::clearLayout()、markContentsDirty() 等公开接口都不会释放它。`tail -f`
     * 时每一行都会经过视口、都会被排版，10000 行历史就是约 60 MB。唯一的公开释放途径是
     * 让段本身消失，于是这里删掉这些段再按同样内容写回：段数、行高、文字全部不变，
     * QPlainTextDocumentLayout 对多段变更只清空不重排，新段直到再次进入视口才排版。
     *
     * 调用方负责保证没有选区、查找高亮等指向这段文字的 QTextCursor——删除重写会让它们
     * 塌缩到删除点。
     */
    int releaseBlockLayouts(QTextDocument *document, int firstBlock, int lastBlock);

    /** @brief 文档首段对应的全局历史行号；与段号相加即得到跨淘汰稳定的行标识。 */
    [[nodiscard]] qint64 documentFirstLineNumber() const { return m_documentScrollbackFirst; }

    /** @brief 文档中来自滚动历史的段数（尾部那一屏不计入）。 */
    [[nodiscard]] int documentScrollbackBlockCount() const { return m_documentScrollbackCount; }

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
    /**
     * @brief 让 libvterm 的当前画笔按刚设置的默认色重新取一次副本。
     *
     * 画笔里存的是解析好的 RGB，改默认色不会回头修它；DECSC/DECRC（vim 进出备用屏用的
     * CSI ?1049h/l）会把这份陈旧 RGB 当成显式背景色回灌，退出 vim 后新打印的每一行都会
     * 顶着一块与主题不符的底色。
     */
    void refreshPenDefaultColors();

    /**
     * @brief 在字节进入 libvterm 之前，给"超长仍未收尾"的 OSC/DCS 字符串补一个 CAN。
     *
     * `tail` 一个二进制文件会把原始字节打进 PTY，凑出的残缺 `ESC ]` / `ESC P` 若始终没有
     * ST/BEL 收尾，libvterm 会把随后的提示符与用户输入一并吞进未结束的字符串，屏幕定格。
     * 扫描器状态跨 feed() 调用保留，只在明显超出正常字符串长度时才注入，正常输出零影响。
     */
    QByteArray boundUnterminatedString(const QByteArray &bytes);

    /** @brief boundUnterminatedString() 的跨调用扫描状态。 */
    enum class StringScan { Normal, AfterEsc, InString };
    StringScan m_stringScan = StringScan::Normal;
    int m_stringScanLength = 0;

    /** @brief hasPendingCorruption() 的存储；resetTerminal() 清零。 */
    bool m_corruptionDetected = false;

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

    /**
     * @brief 一段属性相同的相邻单元格在紧凑行里的样式描述。
     *
     * 颜色存 QRgb 而不是 QColor：QColor 占 16 字节且每格各存两份；终端颜色全部来自
     * libvterm 解析好的 8 位 RGB，QRgb 足以无损往返。无效色（仅理论上可能出现）用标志位
     * 记录，保证解包后与 convertCell() 的结果逐字段相等——增量渲染靠这个相等性判断
     * "一行从屏幕滚进历史时内容没变"，一旦不等就会整段重写并抹掉用户的选区。
     */
    struct PackedRun {
        QRgb foreground = 0;
        QRgb background = 0;
        quint16 column = 0; ///< 本段起始列；延续到下一段的起始列为止。
        quint8 flags = 0;   ///< 见 PackedRunFlag。
    };

    /** @brief PackedRun::flags 的位定义。 */
    enum PackedRunFlag : quint8 {
        PackedBold = 1U << 0U,
        PackedItalic = 1U << 1U,
        PackedUnderline = 1U << 2U,
        PackedStrike = 1U << 3U,
        PackedReverse = 1U << 4U,
        PackedConceal = 1U << 5U,
        PackedForegroundInvalid = 1U << 6U,
        PackedBackgroundInvalid = 1U << 7U,
    };

    /**
     * @brief 滚动历史里一行的紧凑形态。
     *
     * 这是多标签内存占用的主因所在：逐格保存 Cell 时每格约 72 字节本体 + 一次 QString
     * 堆分配（约 32 字节），一条 170 列的日志行约 20 KB，默认 10000 行历史单标签就是
     * 约 200 MB。改存"整行文本 + 样式段"之后同一行约 0.5 KB，缩小约 40 倍。
     *
     * 只有写入文档（render）时才临时解包成 Line，解包结果与打包前逐字段相等。
     */
    struct PackedLine {
        /** @brief 各格文本依次拼接；双宽右半格不占字符，空格子按 0 个字符计。 */
        QString text;
        /**
         * @brief 每格一字节的布局描述：低 2 位是宽度，高 6 位是该格文本的 UTF-16 长度。
         *
         * 为空表示"每格恰好一个 UTF-16 字符且宽度为 1"——纯 ASCII 日志行都走这条捷径，
         * 不额外花一字节每格。
         */
        QByteArray cellLayout;
        /** @brief 按起始列升序排列的样式段；双宽右半格不参与分段。 */
        QVector<PackedRun> runs;
        /** @brief 行的格数（已裁掉行尾空白）。 */
        int columns = 0;
    };

    /**
     * @brief 一行内容在文档中的最终形态。
     *
     * 增量同步靠"这一段和上次写进去的完全一样吗"来决定跳过还是重写，因此比较的必须是
     * 真正会落到文档里的东西：末尾空白裁到哪一列、光标停在哪一列（光标列会强制保留空格）
     * 都会改变结果，只比较单元格是不够的。
     */
    struct RenderedLine {
        Line cells;
        int lastColumn = -1;   ///< 需要写入文档的最后一列；-1 表示空行。
        int cursorColumn = -1; ///< 该行是光标行时的光标列，否则为 -1。
    };

    /** @brief 将 libvterm 单元格转换为可长期保存的 Qt 值对象。 */
    [[nodiscard]] Cell convertCell(const VTermScreenCell &cell) const;

    /** @brief 读取当前屏幕指定行。 */
    [[nodiscard]] Line screenLine(int row) const;

    /**
     * @brief 把一行单元格整理成最终写入形态。
     * @param line 待渲染的单元格。
     * @param screenRow 该行在当前屏幕中的行号；滚动历史传 -1。
     */
    [[nodiscard]] RenderedLine buildRenderedLine(const Line &line, int screenRow) const;

    /**
     * @brief 把一行写入光标所在段，并在遇到光标列时记录光标位置。
     *
     * 相邻且属性完全相同的单元格会合并成一次 insertText。终端里绝大多数行只有一到两种
     * 属性，逐格插入等于把每行的 QTextCharFormat 构造和文档写入放大几十倍。
     */
    void writeRenderedLine(QTextCursor &cursor, const RenderedLine &line);

    /**
     * @brief 把一行紧凑历史直接写入光标所在段，结果与先解包再 writeRenderedLine 相同。
     *
     * 整体重建（缩放、换字号、退出 vim）要一次写入全部历史；跳过逐格解包能省下每格一次
     * QString 分配。
     */
    void writePackedLine(QTextCursor &cursor, const PackedLine &packed) const;

    /** @brief 给光标所在段设置统一的行高格式；每一段写入正文前都要先调用。 */
    void writeBlockFormat(QTextCursor &cursor) const;

    /** @brief 在段尾追加行高锚点，见 setLineAnchors()。 */
    void writeLineAnchors(QTextCursor &cursor) const;

    /** @brief 由紧凑样式段构造字符格式，与 charFormatForCell() 对同一样式的结果一致。 */
    [[nodiscard]] static QTextCharFormat charFormatForRun(const PackedRun &run);

    /** @brief 按样式段还原出一个只带样式、不带文本的单元格（宽度为默认的 1）。 */
    [[nodiscard]] static Cell styledCell(const PackedRun &run);

    /**
     * @brief 为一段没有被重写的段落补记光标位置。
     * @param block 文档中对应的段。
     * @param line 该段的渲染描述；cursorColumn 为 -1 时什么都不做。
     *
     * 段落被判定为"和上次一样"而跳过重写时，光标位置无人计算，补全弹层和光标绘制会失去
     * 锚点。这里按列换算字符偏移补上——双宽字符占两列一个字符，不能直接拿列号当偏移。
     */
    void locateCursorInBlock(const QTextBlock &block, const RenderedLine &line);

    /** @brief 两行的最终写入形态是否完全一致；一致即可跳过重写，选区也随之保住。 */
    [[nodiscard]] static bool isSameRenderedLine(const RenderedLine &left,
                                                 const RenderedLine &right);

    /** @brief 当前屏幕需要渲染的最后一行行号；下方全是空行时按光标行截断。 */
    [[nodiscard]] int lastVisibleScreenRow() const;

    /** @brief 把整份文档清空重建，并重置增量同步状态。 */
    void rebuildDocument(QTextDocument *document);

    /** @brief 单元格是否只有空白；不构造临时 QString，渲染热路径每格都要判断一次。 */
    [[nodiscard]] static bool isBlankCell(const Cell &cell);

    /** @brief 两个单元格的显示属性是否完全一致，可合并为同一次 insertText。 */
    [[nodiscard]] static bool hasSameStyle(const Cell &left, const Cell &right);

    /** @brief 由单元格属性构造字符格式。 */
    [[nodiscard]] static QTextCharFormat charFormatForCell(const Cell &cell);

    /** @brief 删除文档开头的 count 段，用于丢弃被上限淘汰的滚动历史。 */
    static void removeLeadingBlocks(QTextDocument *document, int count);

    /** @brief 删除文档末尾的 count 段，用于替换上一次渲染留下的那一屏。 */
    static void removeTrailingBlocks(QTextDocument *document, int count);

    /** @brief 估算一行紧凑历史实际占用的堆内存字节，用于字节上限裁剪。 */
    [[nodiscard]] static qsizetype estimateLineBytes(const PackedLine &line);

    /**
     * @brief 去掉行尾空白格后打包成紧凑形态。
     *
     * 行尾空白本来就不会写进文档（见 buildRenderedLine），保留它们只会按全宽列数常驻。
     */
    [[nodiscard]] static PackedLine packLine(const Line &line);

    /** @brief 把紧凑行还原成逐格形态；结果与打包前（裁掉行尾空白后）逐字段相等。 */
    [[nodiscard]] static Line unpackLine(const PackedLine &packed);

    /** @brief 按行数与字节双上限从头部淘汰最旧行。 */
    void enforceScrollbackLimits();

    /** @brief 追加一行滚动历史并立即执行上限裁剪。 */
    void appendScrollbackLine(Line line);

    /**
     * @brief 把落在当前网格之外的光标夹回网格内。
     *
     * 必须在每次 vterm_set_size 之前调用：越界的光标会让 libvterm 0.3.3 直接 abort 掉
     * 整个进程。越界只可能由 DECRC 造成，详见实现处的注释。
     */
    void clampCursorIntoGrid();

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
    /** @brief 用 deque 保证头部淘汰为 O(1)；QVector::remove(0,…) 在洪流时是 O(n²)。 */
    std::deque<PackedLine> m_scrollback;
    /** @brief m_scrollback 的近似字节合计，与 estimateLineBytes 同步维护。 */
    qint64 m_scrollbackApproxBytes = 0;
    int m_rows = 24;
    int m_columns = 80;
    /**
     * @brief 上一次真正发出 bell 信号的时刻，用于把密集的 BEL 合成一声。
     *
     * 未启动表示"还没响过"，第一声必须立即发出——限流只针对连续的铃声。
     */
    QElapsedTimer m_lastBellElapsed;

    int m_cursorRow = 0;
    int m_cursorColumn = 0;
    bool m_cursorVisible = true;
    bool m_alternateScreen = false;
    int m_scrollbackLimit = 10000;
    /** @brief 滚动历史字节软上限；与 TerminalSettings 默认值一致为 32 MiB，0 表示禁用。 */
    qint64 m_scrollbackByteLimit = 32 * 1024 * 1024;
    int m_lineHeightPercent = 100;
    /** @brief 统一行高的锚点；为空表示不追加（行高退回随内容变化）。 */
    QVector<QPair<QChar, QFont>> m_lineAnchors;

    /** @brief 最近一次渲染得到的光标文档位置。 */
    int m_renderedCursorPosition = -1;

    /** @brief 最近一次渲染时光标所在格的字符与背景色，供叠加绘制反色块使用。 */
    QString m_renderedCursorText;
    QColor m_renderedCursorBackground;

    /**
     * @brief 累计推入过滚动历史的行数，含已被上限淘汰的部分。
     *
     * m_scrollback 只保留最近 m_scrollbackLimit 行，光看它的下标无法判断"文档里那几段
     * 到底对应哪几行"。用一个只增不减的全局行号，增量同步才能算出该从头部删几段、
     * 从尾部补几段。
     */
    qint64 m_scrollbackPushed = 0;

    /** @brief 上次同步的文档；换了文档对象必须整体重建。 */
    QTextDocument *m_documentTarget = nullptr;

    /** @brief 文档首段对应的全局历史行号。 */
    qint64 m_documentScrollbackFirst = 0;

    /** @brief 文档中来自滚动历史的段数。 */
    int m_documentScrollbackCount = 0;

    /**
     * @brief 文档尾部那一屏上次写进去的内容。
     *
     * 增量同步只允许改动尾部这一段区间，而这份缓存让它能进一步做到"逐段比对、只重写真正
     * 变了的段"。这不只是省事：一行从屏幕滚进历史时内容并没有变，跳过重写等于让那一段
     * 在文档里原地保留，用户正在上面拖选的高亮才不会被下一波输出抹掉。
     */
    QVector<RenderedLine> m_documentScreenLines;

    /** @brief 下一次 render() 是否必须整体重建。 */
    bool m_documentDirty = true;
};
