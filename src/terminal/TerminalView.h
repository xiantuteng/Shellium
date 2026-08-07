#pragma once

#include <QPlainTextEdit>

class QInputMethodEvent;
class VtermBridge;

/**
 * @brief 负责显示远端输出并把键盘事件转换为终端字节流的轻量终端视图。
 *
 * 当前实现把 OpenSSH 分配的远端 PTY 接入 Qt 文本视图，支持普通输入、控制键和常用方向键。
 * 该类刻意隔离输入协议，后续替换为 libvterm 渲染时，TerminalPane 和分屏/SFTP 架构无需改动。
 */
class TerminalView final : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit TerminalView(QWidget *parent = nullptr);

    /**
     * @brief 将 SSH 进程输出追加到视图。
     * @param bytes OpenSSH 标准输出或标准错误中的原始字节。
     */
    void appendTerminalOutput(const QByteArray &bytes);

signals:
    /** @brief 用户输入了需要立即发送给远端 PTY 的字节。 */
    void inputBytes(const QByteArray &bytes);

    /** @brief 用户按下回车后，发送一份完整命令用于嵌套 SSH 识别。 */
    void commandSubmitted(const QString &command);

    /** @brief 视图获得键盘焦点，用于确定当前分屏。 */
    void activated();

    /**
     * @brief 终端输出出现明确的 SSH 密码提示。
     *
     * 信号不携带输入内容，只用于让 TerminalPane 将下一次提交行视为会话凭据；普通输出中
     * 出现 password 单词不会触发，必须匹配以冒号结尾的标准认证提示。
     */
    void authenticationPromptDetected();

    /**
     * @brief OpenSSH 输出最终认证失败信息，当前嵌套连接已经返回外层 Shell。
     *
     * 只匹配 `Permission denied (...)` 最终结果，不匹配仍允许重试的
     * `Permission denied, please try again.`。
     */
    void authenticationFailed();

    /**
     * @brief 终端渲染结果的最后一行出现常见交互式 Shell 提示符。
     *
     * TerminalPane 仅在等待嵌套 SSH 认证时使用该信号，用于确认内层 Shell 已真正可用；
     * 普通命令执行期间产生的提示符不会改变服务器连接链。
     */
    void shellPromptDetected();

    /** @brief 视图像素尺寸变化后计算出的终端字符列数和行数。 */
    void terminalSizeChanged(int columns, int rows);

protected:
    /** @brief 将 Qt 键盘事件编码为常见的 xterm/VT 控制序列。 */
    void keyPressEvent(QKeyEvent *event) override;

    /** @brief 在分屏焦点变化时通知上层工作区。 */
    void focusInEvent(QFocusEvent *event) override;

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

    /** @brief 告知系统输入法终端仍可接收文本，并提供候选窗口定位信息。 */
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private:
    /** @brief 当前尚未按回车提交的输入；同时用于识别 ssh 命令和一次性复用内层密码。 */
    QString m_currentCommand;

    /** @brief 负责 VT 状态机、屏幕渲染和终端协议应答的桥接对象。 */
    VtermBridge *m_vtermBridge = nullptr;

    /** @brief 最近的终端输出尾部，用于跨网络数据块识别 SSH 密码提示。 */
    QByteArray m_authenticationPromptBuffer;

    /** @brief 防止同一个密码提示因多次屏幕刷新而重复触发凭据捕获。 */
    bool m_authenticationPromptReported = false;

    /** @brief 防止同一条最终 Permission denied 输出重复撤销嵌套目标。 */
    bool m_authenticationFailureReported = false;
};
