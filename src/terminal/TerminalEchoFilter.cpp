#include "terminal/TerminalEchoFilter.h"

#include <algorithm>
#include <utility>

namespace {

/**
 * @brief 放弃匹配前允许扫过的字节上限。
 *
 * 远端可能把回显改写过——长行折行时 readline 会插入自己的重绘序列，期望串就再也匹配不上。
 * 没有上限的话，那次登记会一直挂着，让后续每一块输出都白白经过一次前缀扫描，并持续扣留
 * 尾部。宁可放行噪音，也不能让用户的输出被无限期扣住。
 */
constexpr qsizetype kScanBudget = 8192;

/**
 * @brief 可选期望放弃前允许扫过的字节上限。
 *
 * 可选期望描述的是"紧接着可能再来一次的重复回显"：真会发生的话，两次之间只隔着一个
 * 提示符，几十到几百字节就够了。窗口开得越大，越可能在很久以后误扣掉一段恰好同形的
 * 远端输出，因此给它一个远小于必然期望的预算。
 */
constexpr qsizetype kOptionalScanBudget = 1024;

/**
 * @brief 求 buffer 的最长后缀，使其同时是 needle 的前缀。
 * @return 应当扣留的字节数；无部分匹配时为 0。
 *
 * 期望串可能横跨两个数据块，尾部这部分必须留到下一块到达后再判定，否则会把本该扣掉的
 * 前半段放行出去。
 */
qsizetype partialSuffixLength(const QByteArray &buffer, const QByteArray &needle)
{
    const qsizetype maximum = std::min(buffer.size(), needle.size() - 1);
    for (qsizetype length = maximum; length > 0; --length) {
        if (buffer.right(length) == needle.left(length)) {
            return length;
        }
    }
    return 0;
}

} // namespace

void TerminalEchoFilter::expect(const QByteArray &text, const QByteArray &replacement)
{
    append(text, replacement, false);
}

void TerminalEchoFilter::expectOptional(const QByteArray &text,
                                        const QByteArray &replacement)
{
    append(text, replacement, true);
}

void TerminalEchoFilter::append(const QByteArray &text,
                                const QByteArray &replacement,
                                const bool optional)
{
    if (text.isEmpty()) {
        return;
    }
    m_expected.append(Expectation{text, replacement, optional, 0});
}

void TerminalEchoFilter::releaseUnclaimedHold(QByteArray &output)
{
    if (m_held.isEmpty()) {
        return;
    }
    qsizetype keep = 0;
    for (const Expectation &candidate : std::as_const(m_expected)) {
        if (candidate.optional) {
            continue;
        }
        keep = std::max(keep, partialSuffixLength(m_held, candidate.needle));
    }
    output += m_held.left(m_held.size() - keep);
    m_held = m_held.right(keep);
}

QByteArray TerminalEchoFilter::filter(const QByteArray &incoming)
{
    if (m_expected.isEmpty() && m_held.isEmpty()) {
        // 常态快路径：没有待匹配内容时不做任何拷贝。
        return incoming;
    }

    QByteArray buffer = m_held + incoming;
    m_held.clear();
    QByteArray output;

    while (true) {
        if (m_expected.isEmpty()) {
            output += buffer;
            break;
        }

        // 取所有期望串中最早命中的那个，而不是死守队首。注入行这类长内容可能因为远端
        // readline 的重绘而永远匹配不上；若坚持 FIFO，排在它后面的 take 回执就再也没有
        // 机会被扣掉。
        qsizetype bestAt = -1;
        qsizetype bestIndex = -1;
        for (qsizetype index = 0; index < m_expected.size(); ++index) {
            const qsizetype at = buffer.indexOf(m_expected.at(index).needle);
            if (at >= 0 && (bestAt < 0 || at < bestAt)) {
                bestAt = at;
                bestIndex = index;
            }
        }
        if (bestAt >= 0) {
            output += buffer.left(bestAt);
            // 替代串必须紧跟在命中位置写出，不能挪到本轮末尾：它多半是光标控制序列，
            // 与它后面那段远端输出的相对次序就是全部意义所在。
            output += m_expected.at(bestIndex).replacement;
            buffer.remove(0, bestAt + m_expected.at(bestIndex).needle.size());
            m_expected.removeAt(bestIndex);
            continue;
        }

        // 没有完整命中：按必然期望里最长的那个部分后缀扣留，任何一个都不能被截断。可选
        // 期望不参与扣留——它多半不会兑现，为它扣住字节等于让常态输出替例外情形买单。
        qsizetype keep = 0;
        for (const Expectation &candidate : std::as_const(m_expected)) {
            if (candidate.optional) {
                continue;
            }
            keep = std::max(keep, partialSuffixLength(buffer, candidate.needle));
        }
        const qsizetype releasable = buffer.size() - keep;
        output += buffer.left(releasable);
        m_held = buffer.mid(releasable);

        // 预算逐条计：一条期望被远端改写而永远匹配不上，不该连累其他还有机会兑现的。
        for (qsizetype index = m_expected.size() - 1; index >= 0; --index) {
            Expectation &candidate = m_expected[index];
            candidate.scanned += releasable;
            if (candidate.scanned > (candidate.optional ? kOptionalScanBudget
                                                        : kScanBudget)) {
                m_expected.removeAt(index);
            }
        }
        releaseUnclaimedHold(output);
        break;
    }
    return output;
}

QByteArray TerminalEchoFilter::reset()
{
    const QByteArray pending = m_held;
    m_held.clear();
    m_expected.clear();
    return pending;
}
