#pragma once

#include "ai/AiChatTypes.h"

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <optional>

template <typename T>
class QFutureWatcher;

/**
 * @brief 本机智能体 CLI（claude、codex、cursor-agent、gemini、qwen）的识别与调用约定。
 *
 * 这些 CLI 本身都是"会动手"的智能体：默认可以读写当前目录、执行本地 Shell。Shellium 只把
 * 它们当作一个"会思考的模型"来用——每次调用都关掉工具或进入只读/问答模式，并在一个空的
 * 临时目录里运行，真正的命令仍然由 Shellium 按单步协议下发到用户正在看的 SSH 终端，经过
 * 同一套风险分级和审批。这样用户已有的 CLI 订阅和登录态可以直接复用，而远程主机上发生
 * 的每一件事依旧都在终端里可见、可停止。
 */
namespace LocalAgentCli {

/** @brief 提示词交给 CLI 的方式。 */
enum class PromptDelivery {
    /** @brief 写入标准输入后关闭；没有命令行长度上限，也不会出现在 ps 输出里。 */
    Stdin,
    /** @brief 作为最后一个命令行参数传入，供不支持从标准输入读取提示词的 CLI 使用。 */
    Argument
};

/** @brief 一个受支持的智能体 CLI 的静态描述。 */
struct AgentSpec {
    /** @brief 稳定标识，持久化到 AiSettings::cliAgentId。 */
    QString id;
    /** @brief 界面展示名，产品名不翻译。 */
    QString displayName;
    /** @brief 在搜索目录中依次尝试的可执行文件名。 */
    QStringList executableNames;
    PromptDelivery promptDelivery = PromptDelivery::Stdin;
};

/** @brief 一次检测中实际找到的 CLI。 */
struct DetectedAgent {
    QString id;
    QString displayName;
    /** @brief 可执行文件的绝对路径。 */
    QString executablePath;
    /** @brief `--version` 输出的首行；探测失败或超时时为空，不影响可用性判断。 */
    QString version;
};

/**
 * @brief 启动一次 CLI 所需的全部参数。
 *
 * 纯数据结构，便于测试直接断言"每个 CLI 都以只读/无工具模式启动"，不需要真的跑进程。
 */
struct Invocation {
    QString program;
    QStringList arguments;
    /** @brief 写入标准输入的内容；为空表示直接关闭标准输入。 */
    QByteArray standardInput;
    /** @brief CLI 把最终回答写到这个文件（codex -o）；为空表示从标准输出读取。 */
    QString outputFilePath;
};

/** @brief 全部受支持的 CLI，顺序即设置页下拉框的展示顺序。 */
[[nodiscard]] const QList<AgentSpec> &catalog();

/** @brief 按 id 查找 CLI 描述；未知 id 返回空。 */
[[nodiscard]] std::optional<AgentSpec> specById(const QString &id);

/**
 * @brief 向用户的登录 Shell 查询它最终使用的 PATH。
 * @param timeoutMs 等待上限；rc 文件卡住时到点直接放弃，返回空字符串。
 * @return 以 ':' 分隔的 PATH；Windows 或查询失败时为空。
 *
 * 从 Finder/Dock 启动的 macOS 应用只继承 launchd 的极简 PATH（/usr/bin:/bin:...），
 * mise、nvm、volta、Homebrew 安装的 CLI 一个都看不到；只有让登录交互 Shell 跑一遍 rc
 * 文件才能拿到用户在终端里看到的那份 PATH。该调用会阻塞，只能在工作线程中使用。
 */
[[nodiscard]] QString queryLoginShellPath(int timeoutMs = 4000);

/**
 * @brief 合并出查找 CLI 的目录列表：登录 Shell PATH、当前进程 PATH 与常见安装目录。
 * @param loginShellPath queryLoginShellPath() 的结果，可为空。
 * @return 去重后、保持优先级顺序的目录列表；不存在的目录会被剔除。
 */
[[nodiscard]] QStringList searchDirectories(const QString &loginShellPath);

/**
 * @brief 返回缓存的搜索目录；首次调用会查询登录 Shell，因此同样只能在工作线程中调用。
 * @param refresh true 时丢弃缓存重新查询，供设置页"重新检测"使用。
 */
[[nodiscard]] QStringList cachedSearchDirectories(bool refresh = false);

/** @brief 缓存是否已经就绪；就绪后 cachedSearchDirectories() 不再阻塞。 */
[[nodiscard]] bool searchDirectoriesCached();

/**
 * @brief 在给定目录中查找所有受支持的 CLI，不探测版本。
 * @param directories 按优先级排列的搜索目录。
 */
[[nodiscard]] QList<DetectedAgent> findAgents(const QStringList &directories);

/**
 * @brief 运行 `<executable> --version` 并取首个非空行。
 * @param executablePath CLI 绝对路径。
 * @param environment 子进程环境，PATH 中必须包含 node 等解释器所在目录。
 * @param timeoutMs 超时上限，超时后终止进程并返回空字符串。
 */
[[nodiscard]] QString probeVersion(const QString &executablePath,
                                   const QProcessEnvironment &environment,
                                   int timeoutMs = 5000);

/**
 * @brief 完整的阻塞式检测：查询搜索目录、查找 CLI 并探测版本。只能在工作线程中调用。
 * @param refresh 是否丢弃已缓存的搜索目录。
 */
[[nodiscard]] QList<DetectedAgent> detectAll(bool refresh = false);

/**
 * @brief 为子进程构造运行环境：在系统环境基础上把 PATH 替换为合并后的搜索目录。
 *
 * 通过 npm 安装的 CLI 首行是 `#!/usr/bin/env node`；GUI 进程的 PATH 里没有 node 时，
 * 即使用绝对路径启动 CLI 也会立刻失败，所以 PATH 必须和检测时用的保持一致。
 */
[[nodiscard]] QProcessEnvironment childEnvironment(const QStringList &directories);

/**
 * @brief 把多轮对话渲染成一段供 CLI 一次性读取的纯文本。
 *
 * CLI 的非交互模式只接受"一条提示词"，没有 messages 数组；这里用显式的角色分节把系统
 * 规则、历史和本轮输入串起来，并在开头声明"不要使用任何工具、只输出协议 JSON"。
 */
[[nodiscard]] QString renderTranscript(const QList<AiChatMessage> &messages);

/**
 * @brief 生成一次调用的完整参数。
 * @param spec 目标 CLI。
 * @param executablePath CLI 绝对路径。
 * @param model 可选的模型名，为空时不传 --model。
 * @param prompt renderTranscript() 的结果。
 * @param outputFilePath 需要输出文件的 CLI（codex）把最终回答写到这里；其余 CLI 忽略。
 *
 * 每个 CLI 都显式关闭工具或进入只读模式：claude 用 `--tools ""`，codex 用只读沙箱，
 * cursor-agent 用 ask 模式。即便模型无视提示词想在本机动手，CLI 自身也会拒绝。
 */
[[nodiscard]] Invocation buildInvocation(const AgentSpec &spec,
                                         const QString &executablePath,
                                         const QString &model,
                                         const QString &prompt,
                                         const QString &outputFilePath);

/**
 * @brief 把 CLI 的原始输出整理成协议校验可以直接接收的文本。
 *
 * 去掉 ANSI 转义；若回答被包在 ```json 围栏里或前后带了寒暄，则取出其中唯一的顶层 JSON
 * 对象。提取不出来时原样返回，交给上层的协议纠正流程处理，不在这里猜测。
 */
[[nodiscard]] QString extractReply(const QString &rawOutput);

} // namespace LocalAgentCli

Q_DECLARE_METATYPE(LocalAgentCli::DetectedAgent)

/**
 * @brief 在后台线程执行 LocalAgentCli::detectAll() 并把结果投递回界面线程。
 *
 * 查询登录 Shell 和逐个跑 `--version` 合计可能需要数秒，放在界面线程会让设置页卡住。
 */
class LocalAgentCliDetector final : public QObject {
    Q_OBJECT

public:
    explicit LocalAgentCliDetector(QObject *parent = nullptr);
    ~LocalAgentCliDetector() override;

    /**
     * @brief 开始一次检测；上一次尚未完成时忽略本次调用。
     * @param refresh 是否重新查询登录 Shell PATH。
     */
    void start(bool refresh = false);

    /** @brief 是否有检测正在进行。 */
    [[nodiscard]] bool isRunning() const;

signals:
    /** @brief 检测完成，agents 按 catalog() 顺序排列，只包含实际找到的 CLI。 */
    void finished(const QList<LocalAgentCli::DetectedAgent> &agents);

private:
    QFutureWatcher<QList<LocalAgentCli::DetectedAgent>> *m_watcher = nullptr;
};
