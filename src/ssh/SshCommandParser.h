#pragma once

#include "core/ConnectionTarget.h"

/**
 * @brief 解析终端中提交的 SSH/退出命令，用于维护嵌套服务器连接链。
 *
 * 解析器只负责识别用户明确提交的命令，不读取终端输出中的任意文本，避免把日志里的
 * `ssh example` 误判为真实连接。未识别的复杂 Shell 语法会安全地返回 None，并由用户
 * 在文件管理器中手动选择或补充目标。
 */
class SshCommandParser final {
public:
    /** @brief 命令对连接链造成的状态变化类型。 */
    enum class Action {
        None,
        EnterServer,
        LeaveServer
    };

    /** @brief 一次解析的结构化结果。 */
    struct Result {
        Action action = Action::None;
        ConnectionTarget target;
    };

    /**
     * @brief 解析用户刚刚提交的一整行命令。
     * @param commandLine 原始命令行，不包含回车符。
     * @param currentTarget 当前实际所在的服务器，作为嵌套目标的默认用户和跳板来源。
     * @return 连接链变化以及解析出的目标；普通命令返回 Action::None。
     */
    [[nodiscard]] static Result parse(const QString &commandLine,
                                      const ConnectionTarget &currentTarget);
};
