#pragma once

#include <QByteArray>
#include <QList>

/**
 * @brief 从远端输出中扣掉 Shellium 自己写入 PTY 后被原样回显回来的字节。
 *
 * Shellium 会主动向远端写入两类内容：Shell 集成脚本的注入行，以及给 shim 的 take/pass
 * 回执。远端 PTY 开着回显，这些字节会原样回到输出流里，在用户屏幕上变成一大段与操作
 * 无关的噪音。
 *
 * 在本地过滤而不是在远端关回显，是因为远端的两条路都危险：`read -s` 并非 POSIX，不支持
 * 时返回非零，用 `||` 回退会吃掉用户下一行输入；`stty -echo` 多数实现走 TCSAFLUSH，会
 * 丢弃已在缓冲区的输入，导致 shim 永久阻塞。本地过滤没有这些风险——最坏的情况只是没能
 * 匹配上，噪音照常显示。
 */
class TerminalEchoFilter final {
public:
    /**
     * @brief 登记一段预期会被远端回显的字节。
     * @param text 期望扣除的内容；按登记顺序依次匹配。
     * @param replacement 命中后代替它送进终端的字节，默认什么也不送。
     *
     * 不应包含结尾换行：发送的 `\n` 会被回显成 `\r\n`，形式并不一致。
     *
     * replacement 的用处是收拾**远端因为这条命令而多画出来的东西**。注入行是本程序凭空
     * 塞进交互式 Shell 的，远端执行完必然再打印一个提示符；只扣掉回显的话，屏幕上就会
     * 留下登录时那个提示符和它两个并排。登记 `\r\x1b[2K` 把光标退回并擦掉旧提示符所在
     * 行，随后到达的新提示符正好补在原位，用户只会看到一个。
     */
    void expect(const QByteArray &text, const QByteArray &replacement = QByteArray());

    /**
     * @brief 过滤一段远端输出。
     * @param incoming 原始输出字节。
     * @return 应当交给终端渲染的字节。
     *
     * 期望串可能被网络拆分到多个数据块中，因此尾部可能被暂时扣留，等下一块到达再判定。
     * 扣留量不会超过期望串长度减一，且设有总预算：远端把回显改写过（例如长行折行重绘）
     * 时会放弃匹配并把内容如数放行，绝不会无限期扣住用户看得见的输出。
     */
    [[nodiscard]] QByteArray filter(const QByteArray &incoming);

    /**
     * @brief 清空全部状态并交出被扣留的字节。
     * @return 之前扣留、现在应当立即显示的内容。
     *
     * 会话切换时必须调用：旧会话的期望串永远不会再出现，继续持有只会让下一段输出被误扣。
     */
    QByteArray reset();

private:
    /** @brief 尚未判定、可能是期望串前缀的尾部。 */
    QByteArray m_held;

    /** @brief 一条待匹配的期望串及其命中后的替代输出。 */
    struct Expectation {
        QByteArray needle;
        QByteArray replacement;
    };

    /** @brief 按登记顺序待匹配的期望串。 */
    QList<Expectation> m_expected;

    /** @brief 当前期望串已经扫过的字节数，用于在回显被改写时及时放弃。 */
    qsizetype m_scanned = 0;
};
