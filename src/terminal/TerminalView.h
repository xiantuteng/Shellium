#pragma once

#include "terminal/TerminalSearch.h"

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QPlainTextEdit>
#include <QStringList>

struct TerminalSettings;
class QInputMethodEvent;
class QCompleter;
class QContextMenuEvent;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
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
     * @brief 本地恢复终端显示和协议状态，保留历史，不发送远程命令或更改连接。
     *
     * 无参数、无返回值；当前屏幕归档后清空，后续输出按默认模式显示。
     * 该操作不能修复远端 stty 设置，调用者应仅在会话切换或用户明确恢复时使用。
     */
    void resetTerminal();

    /**
     * @brief 自上次复位以来，是否有输出被识别为二进制噪音并被强制中止过。
     *
     * 委托给 VtermBridge::hasPendingCorruption()；TerminalPane 借此在下一次远端 Shell
     * 提示符出现时自动调用一次 resetTerminal()，而不是对每条命令都盲目复位。
     */
    [[nodiscard]] bool hasPendingCorruption() const;

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
     * @brief 在当前终端文档中查找文本，选中命中项并滚动到它。
     * @param text 查找文本；为空时清除当前命中并返回空结果。
     * @param step Refine 用于输入框内容变化，Previous / Next 分别向上 / 向下移动一个命中。
     * @return 当前命中序号与命中总数；到达边界后会从另一端环回。
     */
    TerminalSearchResult searchTerminalText(const QString &text, TerminalSearchStep step);

    /**
     * @brief 清除查找留下的选中命中，滚动位置保持不变。
     *
     * 选区会让离屏排版释放一直推迟（见 releaseOffscreenLayouts），关闭查找栏时必须清掉。
     */
    void clearTerminalSearch();

    /** @brief 返回当前选中的终端文本，已剔除行高锚点；没有选区时返回空字符串。 */
    [[nodiscard]] QString selectedTerminalText() const;

    /** @brief 返回可导出的纯文本终端内容，不包含光标或富文本样式。 */
    [[nodiscard]] QString terminalPlainText() const;

    /**
     * @brief 创建包含复制、粘贴、全选和 Shellium SFTP 入口的终端右键菜单。
     * @return 新创建的菜单；调用方拥有并负责销毁。
     *
     * 刻意不基于 createStandardContextMenu()：那份菜单的文案依赖 Qt 自带的 qtbase 翻译，
     * 未随应用分发，切换界面语言后"复制 / 全选"仍是英文。粘贴与快捷键走同一条
     * pasteFromClipboard() 路径；无选区时"复制"、剪贴板无文本时"粘贴"置灰。
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

    /**
     * @brief 用户把本地文件从系统文件管理器拖拽到本终端视图上。
     * @param localPaths 拖拽条目对应的本地绝对文件或目录路径；已剔除非本地 URL。
     *
     * TerminalView 同样不持有服务器、路径或凭据，只负责识别拖拽是否携带本地文件；是否
     * 已连接、上传到哪一层、哪个目录，全部交给同标签页的 TerminalWorkspace 判断，理由与
     * uploadToCurrentDirectoryRequested 相同。
     */
    void filesDropped(const QStringList &localPaths);

protected:
    /** @brief 将 Qt 键盘事件编码为常见的 xterm/VT 控制序列。 */
    void keyPressEvent(QKeyEvent *event) override;

    /** @brief 记录视图像素变化，并在拖动稳定后统一提交终端字符尺寸。 */
    void resizeEvent(QResizeEvent *event) override;

    /** @brief 标签切回前台时补上隐藏期间欠下的一次增量同步。 */
    void showEvent(QShowEvent *event) override;

    /**
     * @brief 标签隐藏时暂停文档同步，并安排释放排版缓存。
     *
     * PTY/SSH 输出仍会 feed 进 libvterm，会话状态与滚动历史完全不受影响；文档也原样保留，
     * 切回时不需要整体重建，滚动位置和选区都还在。
     */
    void hideEvent(QHideEvent *event) override;

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

    /**
     * @brief 在基础文本绘制后叠加光标。
     *
     * 有焦点时按设置绘制 IBeam、下划线或实心块；失去焦点时不论设置一律画成不闪烁的空心块，
     * 让用户一眼分辨哪个终端正在接收键盘输入。
     */
    void paintEvent(QPaintEvent *event) override;

    /** @brief 获得焦点后恢复设置中的光标形态与闪烁，并立即重绘光标格。 */
    void focusInEvent(QFocusEvent *event) override;

    /**
     * @brief 失去焦点后停止闪烁并把光标改画为空心块。
     *
     * 窗口切到后台、焦点移到侧栏或其他分屏终端都会走到这里；闪烁一并停下，空心块才不会
     * 在两个相位之间时隐时现，后台标签也不再每 600ms 唤醒一次 GUI 线程。
     */
    void focusOutEvent(QFocusEvent *event) override;

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
     * @brief 弹出 createTerminalContextMenu() 构造的终端右键菜单。
     * @param event 携带菜单弹出位置的上下文菜单事件；Qt 保证非空。
     *
     * 上传动作只发出意图，实际文件选择与连接校验由远程文件管理器完成。
     */
    void contextMenuEvent(QContextMenuEvent *event) override;

    /**
     * @brief 只接受携带本地文件 URL 的拖拽；文本等其他 MIME 类型交回默认处理。
     * @param event Qt 拖入事件；只读取其 MIME 类型，不在此处消费文件。
     */
    void dragEnterEvent(QDragEnterEvent *event) override;

    /**
     * @brief 拖拽悬停期间持续接受携带本地文件 URL 的移动，维持"允许放下"的光标状态。
     * @param event Qt 拖拽移动事件；在整个悬停过程中会持续多次触发，不止一次。
     *
     * QPlainTextEdit 自带一份按"能否作为纯文本插入"判定的 dragMoveEvent；本视图只读，
     * 文件 URL 也不是可插入文本，继承实现会在每次移动时都拒绝，表现为鼠标一直显示禁止
     * 图标、松手也不会触发 dropEvent。必须显式接管，理由与 dragEnterEvent 完全一致。
     */
    void dragMoveEvent(QDragMoveEvent *event) override;

    /**
     * @brief 提取拖拽携带的本地文件路径并转交给上层判断是否可以上传。
     * @param event Qt 落点事件。
     *
     * 是否已连接、上传到哪一层、哪个目录，本视图一概不知道；只把已经过滤为本地文件的
     * 路径列表通过 filesDropped 交给 TerminalWorkspace，理由与右键菜单的上传入口相同。
     */
    void dropEvent(QDropEvent *event) override;

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
     * @brief 把系统剪贴板文本粘贴进终端；快捷键与右键菜单共用此入口。
     *
     * 本地行捕获期间只追加到待提交的密码行，既不回显也不发往远端；其余情况经过
     * prepareClipboardPaste 清理与风险确认，并按远端模式包上括号粘贴标记后发给 PTY。
     * 两个入口必须共用同一条路径，否则菜单粘贴会绕过控制字符清理或括号粘贴。
     */
    void pasteFromClipboard();

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
     * @brief 每次渲染后登记可能带着排版缓存的段。
     * @param rebuilt 本次是否整体重建；重建后旧段连同缓存都已释放，候选区间清零。
     */
    void noteLayoutCandidates(bool rebuilt);

    /**
     * @brief 把一段全局行号并入"可能带着排版缓存"的候选区间，并确保清理计时器在跑。
     *
     * 用全局行号而不是段号：历史被上限淘汰时文档头部的段会被删掉，段号随之整体前移，
     * 全局行号则保持不变。
     */
    void markLayoutCandidates(qint64 firstLine, qint64 lastLine);

    /** @brief 把视口首段附近（上两屏、下三屏）登记为候选；绘制和滚动时都要调用。 */
    void markViewportLayoutCandidates();

    /**
     * @brief 释放视口之外那些段的排版缓存，见 VtermBridge::releaseBlockLayouts()。
     *
     * 分批进行，每批至多 kLayoutReleaseBatchBlocks 段；有选区或正在拖选时整轮推迟。
     */
    void releaseOffscreenLayouts();

    /**
     * @brief 若还欠着一帧渲染则立即补上。
     *
     * 任何要读取 QTextDocument 内容的调用（查找、导出全文）都必须先调用它，否则读到的是
     * 上一帧的文档，最后一屏输出会凭空消失。
     */
    void flushPendingRender();

    /** @brief 接受补全候选，只向 PTY 发送尚未输入的后缀。 */
    void acceptCompletion(const QString &completion);

    /**
     * @brief 在本轮输入的第一个字节发出前，记下光标之前的提示符文本。
     *
     * 此刻远端尚未回显任何输入，光标正好停在提示符末尾；回车时以它为锚点从回显行里切出
     * 命令。已有锚点、备用屏或本地捕获期间不做任何事。
     */
    void notePromptAnchor();

    /** @brief 一条命令已提交或作废：清空提示符锚点与"远端改过输入行"标记。 */
    void resetCommandLineTracking();

    /**
     * @brief 从 PTY 回显中取出用户当前输入行的内容。
     * @return 锚点失配（提示符被输出冲掉、预输入等）或内容为空时返回空字符串。
     *
     * 光标后的内容只有紧挨光标时才算命令的一部分；隔着两个以上空白的是 zsh RPROMPT
     * 之类右对齐的提示信息，不能被写进历史。
     */
    [[nodiscard]] QString echoedCommandLine() const;

    /**
     * @brief 回车时真正被执行的命令。
     *
     * 只要按过 Tab、方向键或 Ctrl/Alt 行编辑键，本地按键缓冲就不再代表远端输入行：
     * `./sta<Tab>` 在缓冲里仍是 `./sta`，上翻历史后缓冲甚至是空的。这时以回显为准，
     * 回显不可用才退回缓冲；纯手打的命令仍用缓冲，不受远端回显延迟影响。
     */
    [[nodiscard]] QString submittedCommandLine() const;

    /** @brief 当前尚未按回车提交的输入；同时用于识别 ssh 命令和一次性复用内层密码。 */
    QString m_currentCommand;

    /** @brief 本轮输入开始时光标前的提示符文本；见 notePromptAnchor()。 */
    QString m_promptAnchor;

    /** @brief m_promptAnchor 是否已为本轮输入记录过（空字符串本身也是合法的提示符）。 */
    bool m_hasPromptAnchor = false;

    /** @brief 本轮输入是否用过只有远端 Shell 才知道结果的编辑键（Tab、方向键等）。 */
    bool m_commandLineEditedRemotely = false;

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
     * @brief 光标落在空行行首后的"静置"定时器，超时前不绘制光标。
     *
     * Shell 换行与重绘提示符是两次独立写入：zsh 回车先发 "\r\n"（以及 PROMPT_SP 的
     * "\r \r"），十几毫秒后才发提示符。空闲后首帧立即渲染，于是两次写入之间会单独画出
     * 一帧"光标停在新行第 0 列"，连续回车时就是行首不停闪一下。见 appendTerminalOutput()。
     */
    QTimer *m_cursorSettleTimer = nullptr;

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

    /**
     * @brief 当前是否因隐藏而推迟文档同步。
     *
     * feed() 仍照常进入 libvterm；仅跳过 QTextDocument 更新，直至重新显示。
     */
    bool m_renderSuspended = false;

    /** @brief 隐藏期间是否欠着一次渲染；切回前台或 flushPendingRender() 时补上。 */
    bool m_renderOwed = false;

    /**
     * @brief 用户是否希望视图跟随输出末尾。
     *
     * 意图必须单独保存，不能每帧用滚动条现算：视口高度变化、Qt 重算文档尺寸都会让滚动范围
     * 变大而滚动值不动，现算会把这种漂移误判成用户上翻，新输出从此停在视口之外。
     */
    bool m_followOutputTail = true;

    /** @brief 程序化滚动期间为真，此时的滚动值变化不代表用户意图。 */
    bool m_scrollIntentLocked = false;

    /** @brief 定期释放视口外排版缓存的计时器，见 releaseOffscreenLayouts()。 */
    QTimer *m_layoutReleaseTimer = nullptr;

    /**
     * @brief 可能带着排版缓存的段的全局行号区间（闭区间）；first > last 表示为空。
     *
     * 只增不减地并入被绘制过、被原地改写过的段，释放后收缩。它让每轮清理只碰真正排过版
     * 的段，而不是把整份历史重写一遍。
     */
    qint64 m_layoutCandidateFirst = 0;
    qint64 m_layoutCandidateLast = -1;

    /** @brief 距上一次真正渲染的时间；用于判断当前是"空闲后首帧"还是"持续输出中"。 */
    QElapsedTimer m_lastRenderElapsed;


    /** @brief 备用屏滚轮增量的累加值；触控板的小增量攒够一行才发一次方向键。 */
    int m_wheelAccumulator = 0;

    /** @brief 当前光标闪烁相位和形态，供 paintEvent 与 VtermBridge 同步使用。 */
    bool m_cursorPhaseVisible = true;
    /** @brief 光标停在空行行首、正等待后续输出（通常是提示符）；为真时 paintEvent 不画光标。 */
    bool m_cursorSettling = false;
    /** @brief 设置中是否开启光标闪烁；失焦时计时器会停，重新获得焦点时据此决定是否恢复。 */
    bool m_cursorBlinkEnabled = true;
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

    /**
     * @brief 取出 [from, to) 区间的文本，并剔除其中的行高锚点。
     * @param from 起始文档位置。
     * @param to 结束文档位置（不含）。
     *
     * 以字符格式而不是行尾字符串判定锚点。锚点用的"中"是合法正文字符，按字符串剔除就
     * 必须精确匹配整段后缀，而实际写进文档的锚点是按本机字体探测结果动态决定的——emoji
     * 后备字体与 CJK 相同时就只有一个"中"，与写死的后缀对不上，于是那个本该看不见的
     * "中"被原样复制进剪贴板。选区停在锚点中间时同样对不上。字符格式是渲染层自己盖的
     * 章，两种情形都不会漏，也绝不会误伤用户真正输入的"中"。
     */
    [[nodiscard]] QString textWithoutLineAnchors(int from, int to) const;

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

    /** @brief 判断滚动条此刻是否停在文档底部（几何事实，不代表用户意图）。 */
    [[nodiscard]] bool isScrolledToBottom() const;

    /**
     * @brief 滚动值变化后更新"是否跟随输出末尾"的意图。
     *
     * 只有用户自己滚动（滚轮、拖滚动条、查找跳转、拖选自动滚动）才应改变意图；渲染、
     * 停放文本光标等程序化滚动期间 m_scrollIntentLocked 为真，直接跳过。
     */
    void noteScrollIntent();

    /**
     * @brief 滚动范围变化时，若用户本在跟随末尾，就把视图钉回新的底部。
     *
     * QPlainTextEdit 在视口变矮、文档尺寸重算时只保持"首段"不动，范围变大而滚动值不变，
     * 视图就悄悄离开了底部；若仍按"max - value <= 1"在下一帧现场判定，这次漂移会被当成
     * "用户上翻了"，之后的输出全部画在视口下方。
     */
    void keepFollowingOutputTail();

    /**
     * @brief 把视图拉回输出末尾，使随后的重建继续贴底。
     *
     * 用户上翻阅读历史后再次输入时，光标所在的命令行在视口之外；不拉回底部的话他敲的
     * 字符、按下回车后的输出全都发生在看不见的地方。备用屏例外：那里的文档整块对齐视口
     * 顶部（见 renderPreservingScroll），贴底会把第 0 行连同光标推出视口。
     */
    void scrollToOutputTail();

    /**
     * @brief 发送用户输入的字节，并先把视图拉回输出末尾。
     * @param bytes 已按终端协议编码好的字节。
     *
     * 所有源自用户交互的输入都必须走这里，任何绕过它的路径都会留下"输入了却看不见"的
     * 死角。远端回显与输出属于被动到达，不适用本方法。
     */
    void sendUserInput(const QByteArray &bytes);
};
