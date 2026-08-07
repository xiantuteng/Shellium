#pragma once

#include "core/ConnectionTarget.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QStringList>

/**
 * @brief 生成并解析远端 Shell 集成协议。
 *
 * 注入到远端交互式 Shell 的是一个 `ssh` 函数：用户照常敲 `ssh user@host`，函数不真正执行
 * ssh，而是把参数经 OSC 序列上报给 Shellium，由 Shellium 用 direct-tcpip 亲自建立这一层。
 *
 * 相对于解析终端按键或屏幕回显，这条路径的决定性优势是**参数由远端 Shell 自己展开**：
 * alias、shell 函数、`$VAR`、历史记录调用、输入法提交、折行长命令全都不会再造成偏差。
 *
 * 闭环在于 Shellium 亲自建立的每一层都会被再次注入同一个 shim，因此只需在第一层注入一次，
 * 整条链就自动覆盖，无需用户去每台服务器修改 rc 文件。
 */
class ShellShim final {
    Q_DECLARE_TR_FUNCTIONS(ShellShim)

public:
    /** @brief 远端上报的一条事件。 */
    struct Event {
        enum class Kind {
            None,
            /** @brief 用户执行了 ssh，请求 Shellium 接管这一层。 */
            Connect
        };

        Kind kind = Kind::None;
        /** @brief 远端 Shell 展开后的完整 argv，不含 `ssh` 本身。 */
        QStringList arguments;
    };

    /**
     * @brief 返回 shim 的脚本正文，供写入远端临时文件。
     *
     * 自我限定在 bash/zsh 交互式 Shell 内，其他 Shell 下不会定义任何东西。
     */
    [[nodiscard]] static QByteArray scriptBody();

    /**
     * @brief 生成把脚本写入远端临时文件的命令，交给独立的 exec 通道执行。
     * @param path 远端临时文件路径。
     *
     * 脚本正文经此通道传输，**绝不经过交互式终端**：远端 Shell 一旦接管，readline 会按
     * 自己的方式重绘超宽行，回显不再与发出的字节一一对应，本地过滤器就无论如何都扣不
     * 干净。用 here-doc 而非 base64 是为了不依赖远端是否有 base64 及其 -d/-D 差异。
     */
    [[nodiscard]] static QByteArray installCommand(const QByteArray &path);

    /**
     * @brief 返回在交互式 Shell 中加载该脚本的命令（不含结尾回车）。
     * @param path 远端临时文件路径。
     *
     * 刻意保持极短：短行 readline 会逐字回显，本地过滤器因此可以精确扣掉，用户看不到它。
     * 以空格开头，配合 HISTCONTROL=ignorespace 不进历史。脚本会在加载后自删。
     */
    [[nodiscard]] static QByteArray sourceCommand(const QByteArray &path);

    /**
     * @brief 解析一条 OSC 载荷。
     * @param payload VtermBridge 交出的原始载荷。
     * @return 无法识别时返回 Kind::None。
     */
    [[nodiscard]] static Event parseEvent(const QByteArray &payload);

    /**
     * @brief 把 shim 上报的 argv 转换成可直接建立的连接目标。
     * @param arguments 远端展开后的 ssh 参数。
     * @param currentTarget 当前栈顶服务器，用于继承默认用户名。
     * @param declineReason 无法接管时的原因，供回执给远端 shim。
     * @return 可接管时返回有效目标；否则返回无效目标并填写 declineReason。
     *
     * 无法完整表达的用法（一次性远程命令、纯转发、自定义 ProxyCommand 等）一律拒绝接管，
     * 交还给真正的 ssh 执行。宁可退回降级路径，也不能用一个错误的目标去建立会话。
     */
    [[nodiscard]] static ConnectionTarget resolveTarget(const QStringList &arguments,
                                                        const ConnectionTarget &currentTarget,
                                                        QString *declineReason);

    /** @brief 告知远端 shim 由 Shellium 接管，函数应直接返回。 */
    [[nodiscard]] static QByteArray acceptResponse();

    /** @brief 告知远端 shim 自行执行真正的 ssh。 */
    [[nodiscard]] static QByteArray declineResponse();
};
