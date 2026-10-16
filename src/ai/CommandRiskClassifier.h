#pragma once

#include <QString>

/**
 * @brief AI 助手建议命令的执行风险分级。
 *
 * 分级只影响界面呈现和是否需要人工确认，绝不代表安全审计结论；调用方必须始终把 Write
 * 与 HighRisk 视为"需要用户看到真实命令并主动点击才会下发"，不能仅凭此分级自动执行。
 */
enum class CommandRiskLevel {
    /** @brief 只读、无副作用，可自动执行并直接把结果展示给用户。 */
    ReadOnly,
    /** @brief 会修改系统状态，需要用户逐条确认后才能执行。 */
    Write,
    /** @brief 破坏性或影响范围大的操作，除了确认外界面还必须用醒目的红色强调。 */
    HighRisk
};

namespace CommandRiskClassifier {

/**
 * @brief 判断一条候选命令的执行风险。
 * @param command 未经修改的完整命令文本，可能包含管道和多个子命令。
 * @return 命中任意高风险特征即返回 HighRisk；命中任意已知只读命令即返回 ReadOnly；
 *         其余一律按 Write 处理——把"不确定"的命令默认当成需要确认更安全。
 *
 * 判定基于命令名和常见危险参数的静态规则表，而不是把决定权交给模型自称"这是只读的"。
 * 规则本身保守但不完备：新出现的命令、别名或包装脚本会落入默认的 Write 分支，需要人工
 * 确认，这是有意的降级方向——错杀比放过更安全。
 */
[[nodiscard]] CommandRiskLevel classify(const QString &command);

/**
 * @brief 判断一条命令能否在无人值守的情况下自动下发。
 * @param command 未经修改的完整命令文本。
 * @return 仅当命令被判为 ReadOnly、不含命令替换，且不会停在交互界面或持续运行
 *         （top、less、tail -f、未带 -c 的 ping、需要输密码的 sudo 等）时返回 true。
 *
 * 自动执行后客户端要等 Shell 提示符回来才会续问模型；一条只读但永不退出的命令会让整条
 * 自动回路卡死在等待状态，所以"只读"本身不足以构成自动执行的条件。
 */
[[nodiscard]] bool isSafeForAutoExecution(const QString &command);

/** @brief 返回分级对应的中文短标签，供界面徽标直接展示。 */
[[nodiscard]] QString label(CommandRiskLevel level);

} // namespace CommandRiskClassifier
