#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QPlainTextEdit>
#include <QStringList>

struct TerminalSettings;
class QInputMethodEvent;
class QCompleter;
class QContextMenuEvent;
class QMenu;
class QMimeData;
class QPaintEvent;
class QPainter;
class QWheelEvent;
class QStringListModel;
class QTimer;
class VtermBridge;

/**
 * @brief 负责显示远端输出并把键盘事件转换为终端字节流的轻量终端视图。
 *
 * 当前实现把 OpenSSH 分配的远端 PTY 接入 Qt 文本视图，支持普通输入、控制键和常用方向键。
 * 该类刻意隔离输入协议，后续替换为 libvterm 渲染时，TerminalPane 和 SFTP 架构无需改动。
 */
class TerminalView final : public QPlainTextEdit {
    Q_OBJECT

public:
    /**
     * @brief 普通终端粘贴在发送给 PTY 前的安全检查结果。
     *
     * text 已统一换行并移除不可见的危险控制字符；removedControlCharacters 和
     * containsLineBreak 用于决定是否必须让用户明确确认，避免粘贴内容静默执行多条命令。
     */
    struct PastePreparation {
        /** @brief 已统一换行并移除危险控制字符、可在确认后发送给 PTY 的文本。 */
        QString text;

        /** @brief 清理过程中移除的 C0、DEL 或 C1 控制字符数量。 */
        qsizetype removedControlCharacters = 0;

        /** @brief 清理后的内容是否仍含换行；含换行意味着可能一次执行多条命令。 */
        bool containsLineBreak = false;

        /**
         * @brief 是否存在需要用户确认的风险。
         * @param multiLineConfirmationEnabled 用户是否开启了"多行粘贴确认"设置。
         *
         * 被移除的控制字符始终需要确认：那是用户从剪贴板内容表面看不出来的改动，静默放行
         * 等于替他做了一次安全决定。多行则相反——用户复制的就是自己看得见的多行脚本，是否
         * 打断由设置决定，默认直接粘贴。
         */
        [[nodiscard]] bool requiresConfirmation(const bool multiLineConfirmationEnabled) const
        {
            return (containsLineBreak && multiLineConfirmationEnabled)
                || removedControlCharacters > 0;
        }
    };

    explicit TerminalView(QWidget *parent = nullptr);

    /**
     * @brief 返回最近一次已提交的终端字符列数。
     * @return 至少为 1；视图尚未完成首次布局时返回标准初始值 80。
     *
     * 新建 PTY 工作线程必须在 start() 前读取该值，否则视图早已完成布局、但连接建立后
     * 没有新的 resizeEvent 时，远端会一直停留在默认 80 列直到用户再次拖动窗口。
     */
    [[nodiscard]] int terminalColumns() const { return m_terminalColumns; }

    /**
     * @brief 返回最近一次已提交的终端字符行数。
     * @return 至少为 1；视图尚未完成首次布局时返回标准初始值 24。
     */
    [[nodiscard]] int terminalRows() const { return m_terminalRows; }

    /**
     * @brief 清理剪贴板文本并计算粘贴风险，不访问剪贴板或弹出界面。
     * @param clipboardText 剪贴板提供的原始 Unicode 文本。
     * @return 可发送文本、被移除控制字符数量以及是否含换行。
     *
     * 允许普通可打印字符、Tab 和换行；CRLF/CR 会统一为 LF。ESC、NUL、DEL 以及 C1
     * 控制字符会被删除，因为它们可注入终端控制序列或改变下游程序状态。该纯函数单独公开，
     * 是为了让安全边界能在不操作系统剪贴板、不弹确认框的单元测试中稳定复核。
     */
    [[nodiscard]] static PastePreparation prepareClipboardPaste(const QString &clipboardText);

    /**
     * @brief 将 SSH 进程输出追加到视图。
     * @param bytes OpenSSH 标准输出或标准错误中的原始字节。
     */
    void appendTerminalOutput(const QByteArray &bytes);

    /**
     * @brief 进入本地行捕获：按键不再发往远端，也不回显。
     *
     * 用于 Shellium 自己接管认证时在终端里索要密码。必须走本地捕获而不是让远端回显：
     * 此时根本没有远端可言——那一层的 SSH 会话还没认证完成。
     */
    void beginLocalLineCapture();

    /** @brief 是否正处于本地行捕获状态。 */
    [[nodiscard]] bool isCapturingLocalLine() const { return m_localCapture; }

    /**
     * @brief 替换当前主机的历史命令补全候选。
     * @param commands 按最近使用优先排列的完整命令；空项和重复项会被丢弃。
     *
     * 候选仅在用户普通输入命令时显示，不会在密码本地捕获期间出现。TerminalView 不自行
     * 访问数据库，因此本方法也可由内存模型或未来远程同步历史提供数据。
     */
    void setCommandCompletions(const QStringList &commands);

    /**
     * @brief 以程序方式向当前终端提交一条完整命令。
     * @param command 不带结尾换行的命令；空命令会被忽略。
     *
     * Snippet 执行必须走此入口而不是直接写 worker：它会先发送 UTF-8 命令和回车，再发出
     * commandSubmitted，使嵌套 SSH 识别与命令历史和真实键盘输入保持同一条业务路径。
     */
    void submitCommand(const QString &command);

    /**
     * @brief 把命令文本插入当前终端输入行，但不提交执行。
     * @param command 不含回车的命令文本；空字符串会被忽略。
     *
     * 除了把 UTF-8 字节发送给 PTY，还同步更新 m_currentCommand，使用户随后继续键入、使用补全
     * 或按回车提交时，命令历史和嵌套 SSH 识别能拿到完整命令，而不是只看到后续手输部分。
     */
    void insertCommandText(const QString &command);

    /**
     * @brief 实时应用终端外观与行为设置。
     * @param settings 已规范化或待规范化的设置快照。
     *
     * 该方法不访问数据库；所有标签页由 MainWindow 订阅设置服务后统一调用，因此实时
     * 预览、保存和系统深浅色变化都经过同一条实现路径。
     */
    void applySettings(const TerminalSettings &settings);

    /**
     * @brief 按快捷键临时放大/缩小终端字号，不写回持久化设置。
     * @param deltaPoints 字号变化量，正数放大、负数缩小；结果夹在 6~72pt 之间。
     */
    void zoomFont(int deltaPoints);

    /** @brief 把字号重置为最近一次 applySettings 配置的基准值。 */
    void resetZoom();

    /**
     * @brief 在当前终端文档中查找文本并选中命中项。
     * @param text 非空查找文本。
     * @param backward true 表示向前查找，false 表示向后查找。
     * @return 找到文本返回 true；到达边界后会从另一端再查一次。
     */
    bool findTerminalText(const QString &text, bool backward = false);

    /** @brief 返回可导出的纯文本终端内容，不包含光标或富文本样式。 */
    [[nodiscard]] QString terminalPlainText() const;

    /**
     * @brief 创建包含 Qt 标准文本动作和 Shellium SFTP 入口的终端右键菜单。
     * @return 新创建的菜单；调用方拥有并负责销毁。
     *
     * 把菜单构造从 contextMenuEvent 分离，既让键盘菜单、自动化测试和未来触控入口能够
     * 复用同一组动作，也避免测试依赖不同 QPA 插件对原生弹出菜单的实现差异。
     */
    [[nodiscard]] QMenu *createTerminalContextMenu();

signals:
    /** @brief 用户输入了需要立即发送给远端 PTY 的字节。 */
    void inputBytes(const QByteArray &bytes);

    /** @brief 用户按下回车后，发送一份完整命令用于嵌套 SSH 识别。 */
    void commandSubmitted(const QString &command);

    /**
     * @brief 本地行捕获结束，交出用户输入的整行内容。
     * @param line 用户输入的整行。
     * @param cancelled 用户是否按 Ctrl+C 主动放弃。
     *
     * 空行与"放弃"必须区分：在密码提示处直接回车，OpenSSH 的行为是重新提示而不是中止。
     * 把两者混为一谈会让任何一次输入丢失都升级成不可恢复的连接失败。
     */
    void localLineSubmitted(const QString &line, bool cancelled);

        /**
     * @brief 终端输出出现明确的 SSH 密码提示。
     *
     * 信号不携带输入内容，只用于让 TerminalPane 将下一次提交行视为会话凭据；普通输出中
     * 出现 password 单词不会触发，必须匹配以冒号结尾的标准认证提示。
     */
    void authenticationPromptDetected();

    /**
     * @brief 终端输出表明一次嵌套 ssh 已经失败并返回了外层 Shell。
     *
     * 覆盖认证被拒、主机名解析失败、连接被拒/超时、主机密钥校验失败等全部会让 OpenSSH
     * 直接退出的情形。只匹配 `Permission denied (...)` 这类最终结果，不匹配仍允许重试的
     * `Permission denied, please try again.`。
     */
    void nestedSshFailed();

    /**
     * @brief 用户按下 Ctrl+C，可能中断了一次正在建立的嵌套 ssh。
     *
     * 本信号不判断当前是否真的在连接——那是 TerminalPane 的职责；这里只如实报告按键，
     * 避免视图层持有连接链状态。
     */
    void nestedSshAborted();

    /**
     * @brief 终端渲染结果的最后一行出现常见交互式 Shell 提示符。
     *
     * TerminalPane 仅在等待嵌套 SSH 认证时使用该信号，用于确认内层 Shell 已真正可用；
     * 普通命令执行期间产生的提示符不会改变服务器连接链。
     */
    void shellPromptDetected();

    /**
     * @brief 远端 Shell 集成脚本上报了一条事件，原样转发自 VtermBridge。
     * @param payload OSC 载荷。
     */
    void shellIntegrationEvent(const QByteArray &payload);

    /** @brief 视图像素尺寸变化后计算出的终端字符列数和行数。 */
    void terminalSizeChanged(int columns, int rows);

    /**
     * @brief 用户从终端右键菜单请求把本地文件上传到当前层服务器的当前远程目录。
     *
     * TerminalView 不持有服务器、路径或凭据，只发出用户意图；同标签页的
     * TerminalWorkspace 会把请求路由给专属 RemoteFileManagerWidget。这样既不会让纯
     * 终端视图依赖 SFTP 模块，也能保证多标签页之间不会误用其他会话的上传目标。
     */
    void uploadToCurrentDirectoryRequested();

protected:
    /** @brief 将 Qt 键盘事件编码为常见的 xterm/VT 控制序列。 */
    void keyPressEvent(QKeyEvent *event) override;

    /** @brief 记录视图像素变化，并在拖动稳定后统一提交终端字符尺寸。 */
    void resizeEvent(QResizeEvent *event) override;

    /**
     * @brief 阻止 Qt 把 Tab 当成控件焦点导航键。
     * @param next true 表示向前切换焦点，false 表示向后切换。
     * @return 始终返回 false，使 Tab/Shift+Tab 留在终端并交给远端 Shell。
     */
    bool focusNextPrevChild(bool next) override;

    /**
     * @brief 接收 macOS 输入法提交文本并转发给远端终端。
     *
     * QPlainTextEdit 处于只读模式时默认会关闭输入法，必须显式处理提交事件，否则真实键盘
     * 可能只产生回车事件而丢失普通字符；QTest 合成按键无法覆盖这一原生平台差异。
     */
    void inputMethodEvent(QInputMethodEvent *event) override;

    /** @brief 在基础文本绘制后叠加 IBeam 光标；块状与下划线由桥接层格式化。 */
    void paintEvent(QPaintEvent *event) override;

    /**
     * @brief 备用屏下把滚轮转换为方向键发往远端，其余情况沿用文档滚动。
     *
     * vim、less、man 的"历史"保存在程序自己手里，本地文档只有一屏，滚轮滚不动任何东西。
     * xterm 的 alternateScroll 行为是把滚轮翻译成上下键，用户因此能用触控板翻阅内容。
     */
    void wheelEvent(QWheelEvent *event) override;

    /** @brief 告知系统输入法终端仍可接收文本，并提供候选窗口定位信息。 */
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

    /**
     * @brief 在 Qt 标准文本菜单末尾增加“上传到当前远程目录”动作。
     * @param event 携带菜单弹出位置的上下文菜单事件；Qt 保证非空。
     *
     * 保留复制、全选等标准动作，避免为了一个业务入口破坏平台原生文本操作；实际文件
     * 选择与连接校验由远程文件管理器完成。
     */
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    /** @brief 根据当前输入前缀显示或隐藏历史补全弹层。 */
    void updateCompletionPopup();

    /**
     * @brief 弹出粘贴确认框，展示风险说明与清理后的完整内容。
     * @param prepared 已清理并完成风险判定的粘贴内容。
     * @return 用户确认发送返回 true；取消或关闭窗口返回 false。
     *
     * 内容区只读且不接受焦点，因此回车不会被文本框吞掉，而是直接触发默认的"粘贴"按钮——
     * 这是本对话框最常见的操作，不该强迫用户离开键盘去点按钮。
     */
    [[nodiscard]] bool confirmPaste(const PastePreparation &prepared);

    /**
     * @brief 计算 libvterm 视觉光标对应的文档光标矩形，供 paintEvent 与补全弹层共用。
     * @return 有效位置时返回对应矩形；无法取得有效位置（vterm 尚未渲染或位置越界）时返回空 QRect。
     */
    QRect visualCursorRect() const;

    /**
     * @brief 由插入符矩形换算出光标覆盖的整个单元格矩形。
     * @param caretRect visualCursorRect() 给出的插入符矩形。
     *
     * cursorRect() 只给出一两像素宽的插入符，块状与下划线光标需要真实格宽，双宽字符还要
     * 占满两格。
     */
    QRect cursorCellRect(const QRect &caretRect) const;

    /** @brief 只失效光标所在的一小块区域；闪烁不再触碰 QTextDocument。 */
    void updateCursorArea();

    /**
     * @brief 在 Qt 完成选区绘制后遮住被选中的内部行高锚点。
     * @param painter 已绑定到终端 viewport 的绘制器。
     * @param repaintArea 本次 paintEvent 实际需要刷新的区域。
     *
     * 行高锚点中的真实“中”字平时依靠透明前景色隐藏，但 Qt 选区会强制改用
     * HighlightedText 颜色，导致内部字形显现。本方法只遍历当前可见段，并用选区背景覆盖
     * 带专用格式标记的锚点；用户真实输出的同名汉字不会受影响。
     */
    void paintSelectedLineAnchorMasks(QPainter &painter, const QRect &repaintArea) const;

    /** @brief 标记欠一帧渲染；已经排队时不重复排队，保证持续输出下的稳定帧率。 */
    void scheduleRender();

    /**
     * @brief 若还欠着一帧渲染则立即补上。
     *
     * 任何要读取 QTextDocument 内容的调用（查找、导出全文）都必须先调用它，否则读到的是
     * 上一帧的文档，最后一屏输出会凭空消失。
     */
    void flushPendingRender();

    /** @brief 接受补全候选，只向 PTY 发送尚未输入的后缀。 */
    void acceptCompletion(const QString &completion);

    /** @brief 当前尚未按回车提交的输入；同时用于识别 ssh 命令和一次性复用内层密码。 */
    QString m_currentCommand;

    /** @brief 负责 VT 状态机、屏幕渲染和终端协议应答的桥接对象。 */
    VtermBridge *m_vtermBridge = nullptr;

    /** @brief 是否正在本地捕获整行输入（例如 Shellium 自己索要的密码）。 */
    bool m_localCapture = false;

    /** @brief 本地捕获期间累积的输入；不写入终端文档，也不发往任何远端。 */
    QString m_localLine;

    /** @brief 最近的终端输出尾部，用于跨网络数据块识别 SSH 密码提示。 */
    QByteArray m_authenticationPromptBuffer;

    /** @brief 防止同一个密码提示因多次屏幕刷新而重复触发凭据捕获。 */
    bool m_authenticationPromptReported = false;

    /** @brief 防止同一条最终 Permission denied 输出重复撤销嵌套目标。 */
    bool m_authenticationFailureReported = false;

    /**
     * @brief 是否允许按历史命令自动展示补全弹层，默认关闭。
     *
     * 当前输入缓冲和历史记录在关闭时仍正常维护，因为它们还承担嵌套 SSH 识别、命令历史
     * 持久化等职责；这里只控制自动弹层，不能用清空命令缓冲的方式实现开关。
     */
    bool m_commandCompletionEnabled = false;

    /** @brief 粘贴多行内容前是否先展示内容并要求确认；默认关闭，即直接粘贴。 */
    bool m_multiLinePasteConfirmation = false;

    /** @brief 基于当前主机持久命令历史的弹出式补全器。 */
    QCompleter *m_commandCompleter = nullptr;

    /** @brief 补全器的数据模型；候选顺序保持“最近使用优先”。 */
    QStringListModel *m_completionModel = nullptr;

    /** @brief 光标闪烁定时器；关闭闪烁时停止，避免隐藏窗口仍持续唤醒 GUI 线程。 */
    QTimer *m_cursorBlinkTimer = nullptr;

    /**
     * @brief 连续窗口尺寸事件的防抖定时器。
     *
     * 分隔条拖动时 Qt 会产生高频 resizeEvent。若每次都立即向 PTY 发送尺寸，远端 readline
     * 会连续处理 SIGWINCH 并重绘提示符，而这些旧尺寸重绘字节到达时本地网格可能已经再次
     * 改变，最终出现重复提示符和大段空格。定时器只在拖动短暂停顿后提交最终尺寸。
     */
    QTimer *m_resizeDebounceTimer = nullptr;

    /**
     * @brief 渲染合帧定时器。
     *
     * 远端一次大输出会被切成成百上千个分片。逐片同步重绘会让文档编辑与重排乘以分片数，
     * 界面被自己的输出压死。这里刻意用"欠帧才排队、排队后不续期"的语义而不是防抖：持续
     * 输出时必须以固定帧率持续出帧，防抖则会一直推迟到输出停止才画出第一帧。
     */
    QTimer *m_renderCoalesceTimer = nullptr;

    /** @brief 距上一次真正渲染的时间；用于判断当前是"空闲后首帧"还是"持续输出中"。 */
    QElapsedTimer m_lastRenderElapsed;

    /** @brief 备用屏滚轮增量的累加值；触控板的小增量攒够一行才发一次方向键。 */
    int m_wheelAccumulator = 0;

    /** @brief 当前光标闪烁相位和形态，供 paintEvent 与 VtermBridge 同步使用。 */
    bool m_cursorPhaseVisible = true;
    int m_cursorShape = 0;
    QColor m_cursorColor;

    /** @brief 当前行高百分比，用于把像素高度换算成准确 PTY 行数。 */
    int m_lineHeightPercent = 100;

    /** @brief 最近一次 applySettings 中配置的基准字号，Cmd+0 重置缩放时回到该值。 */
    int m_basePointSize = 13;

    /** @brief 最近一次已提交给 libvterm 和 PTY 的字符列数，用于过滤等价像素 resize。 */
    int m_terminalColumns = 80;

    /** @brief 最近一次已提交给 libvterm 和 PTY 的字符行数，用于过滤等价像素 resize。 */
    int m_terminalRows = 24;

    /** @brief 最近一次从真实文档量到的行高像素值；0 表示尚未量过。 */
    int m_lastMeasuredCellHeight = 0;

    /**
     * @brief 视图是否至少收到过一次由 Qt 布局系统产生的 resizeEvent。
     *
     * QWidget 构造期间的默认 viewport 尺寸不是最终布局尺寸，若过早据此覆盖标准 80×24，
     * 隐藏标签或尚未 show 的测试视图会得到几十列的伪尺寸。首次真实布局前保持标准值。
     */
    bool m_hasReceivedResizeEvent = false;

    /**
     * @brief 依据当前 viewport 尺寸重算列/行数，并同步更新 libvterm 与远端 PTY。
     *
     * 只有字符行列实际变化时才发出 terminalSizeChanged，避免像素宽度变化但仍落在同一字符
     * 网格时产生无意义的 PTY resize 和 SIGWINCH。
     */
    void recalculateTerminalGeometry();

    /**
     * @brief 按当前字体和行高百分比算出每个字符格应占的像素高度。
     *
     * 必须只用 ASCII 基准字体的行距，绝不能读某一段渲染后的实际高度：CJK/emoji 等宽
     * 字符缺字时 Qt 会换用系统后备字体，其行距往往比基准字体更高。若某几行恰好夹了
     * 中文字符，那几行的"实际高度"就会比其余行高出一截，同样 44 行文本的总像素高度
     * 就会超出 44 行本该占的高度，最后一行——通常是 vim 状态行——因此被推出可视区域。
     * 这个值算出来后要经 VtermBridge::setLineHeightPixels() 转成 FixedHeight 写进每一
     * 段的 QTextBlockFormat，把行高钉死为常数，不再受该行实际内容影响。
     */
    int computeCellHeight() const;

    /**
     * @brief 构造统一行高用的锚点：本机实际渲染 CJK 与 emoji 的那几个后备字体。
     *
     * 行高是"该行用到的所有字体里 ascent 最大值加 descent 最大值"，只能抬高不能压低，
     * 所以统一后的高度下限就是这几个字体合起来的最大值。具体族名随平台与用户装的字体
     * 而变，必须真实排一个汉字/emoji 问 Qt，不能写死族名。
     */
    [[nodiscard]] QVector<QPair<QChar, QFont>> buildLineAnchors() const;

    /** @brief 按行剔除行尾的锚点后缀；正文中间同样的字符必须保留。 */
    [[nodiscard]] static QString stripLineAnchors(const QString &text);

    /**
     * @brief 渲染后复核真实行高，与当前网格不符时纠正一次。
     *
     * 行高必须等文档真的排过一次版才知道（脱离窗口的探针文档在 Retina 上会量错），而
     * 行数又依赖行高，所以只能"先渲染、再纠正"。锚点保证行高与内容无关，纠正后量到的
     * 仍是同一个值，因此该过程收敛，不会反复触发 SIGWINCH。
     */
    void noteRenderedCellHeight();

    /**
     * @brief 提供复制/拖拽用的选区数据，并剔除统一行高用的零宽锚点字符。
     *
     * 复制、右键菜单和拖拽全都走这个虚函数，覆盖它一处即可让三条路径都拿到干净文本。
     */
    [[nodiscard]] QMimeData *createMimeDataFromSelection() const override;

    /**
     * @brief 提交当前稳定的字符尺寸并重绘文档，同时保持用户原有滚动位置。
     *
     * resize 防抖到期、字体缩放和终端设置变化都走该入口，保证 libvterm 重排后不会遗漏
     * 文档重建，也不会把正在阅读历史输出的用户强制拉回底部。
     */
    void commitTerminalGeometry();

    /**
     * @brief 重建终端文档，并保持重建前的滚动意图。
     *
     * VtermBridge::render 每次都会 document->clear() 后整体重建，滚动条随之归零。任何直接
     * 调用 render 的路径都必须经过本方法，否则该路径就会把视图弹回顶部——光标闪烁每 600ms
     * 触发一次重建，漏掉它意味着用户什么都不做也会被反复拉走。
     */
    void renderPreservingScroll();

    /**
     * @brief 使用预先采样的滚动意图重建终端文档。
     * @param followBottom 重建前是否停在底部；true 表示重建后继续贴底。
     * @param previousValue 重建前的滚动值，非贴底时原样还原。
     *
     * 追加输出必须在 feed() 之前采样：feed 之后文档已经变长，此时再判断"是否在底部"
     * 得到的是新文档的结论，用户原本贴底的状态会被误判成停在历史位置。
     */
    void renderPreservingScroll(bool followBottom, int previousValue);

    /** @brief 判断当前是否停在文档底部，作为重建后是否继续贴底的依据。 */
    [[nodiscard]] bool isScrolledToBottom() const;
};
