#pragma once

#include <QColor>
#include <QPlainTextEdit>
#include <QStringList>

struct TerminalSettings;
class QInputMethodEvent;
class QCompleter;
class QContextMenuEvent;
class QMenu;
class QPaintEvent;
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

        /** @brief 是否存在需要用户确认的多行或控制字符风险。 */
        [[nodiscard]] bool requiresConfirmation() const
        {
            return containsLineBreak || removedControlCharacters > 0;
        }
    };

    explicit TerminalView(QWidget *parent = nullptr);

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

        /** @brief 根据当前等宽字体度量计算 PTY 尺寸并通知 SSH 通道。 */
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

    /** @brief 基于当前主机持久命令历史的弹出式补全器。 */
    QCompleter *m_commandCompleter = nullptr;

    /** @brief 补全器的数据模型；候选顺序保持“最近使用优先”。 */
    QStringListModel *m_completionModel = nullptr;

    /** @brief 光标闪烁定时器；关闭闪烁时停止，避免隐藏窗口仍持续唤醒 GUI 线程。 */
    QTimer *m_cursorBlinkTimer = nullptr;

    /** @brief 当前光标闪烁相位和形态，供 paintEvent 与 VtermBridge 同步使用。 */
    bool m_cursorPhaseVisible = true;
    int m_cursorShape = 0;
    QColor m_cursorColor;

    /** @brief 当前行高百分比，用于把像素高度换算成准确 PTY 行数。 */
    int m_lineHeightPercent = 100;

    /** @brief 最近一次 applySettings 中配置的基准字号，Cmd+0 重置缩放时回到该值。 */
    int m_basePointSize = 13;

    /** @brief 依据当前 viewport 尺寸重算列/行数并通知远端 PTY，缩放和 resizeEvent 共用。 */
    void recalculateTerminalGeometry();
};
