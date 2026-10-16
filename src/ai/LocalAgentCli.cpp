#include "ai/LocalAgentCli.h"

#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

#if defined(Q_OS_UNIX)
#include <pwd.h>
#include <unistd.h>
#endif

namespace LocalAgentCli {

namespace {

const QString kPathBeginMarker = QStringLiteral("__SHELLIUM_PATH_BEGIN__");
const QString kPathEndMarker = QStringLiteral("__SHELLIUM_PATH_END__");

/** @brief PATH 分隔符：Windows 为 ';'，其余平台为 ':'。 */
QChar pathListSeparator()
{
    return QDir::listSeparator();
}

/**
 * @brief 用户的登录 Shell。
 *
 * 从 Dock 启动的应用不一定带 SHELL 环境变量，此时回落到 passwd 记录，最后才用 /bin/zsh
 * （macOS 默认）或 /bin/sh。
 */
QString loginShell()
{
    const QString fromEnvironment = qEnvironmentVariable("SHELL");
    if (!fromEnvironment.isEmpty() && QFileInfo(fromEnvironment).isExecutable()) {
        return fromEnvironment;
    }
#if defined(Q_OS_UNIX)
    if (const passwd *entry = getpwuid(getuid()); entry != nullptr && entry->pw_shell != nullptr) {
        const QString shell = QString::fromLocal8Bit(entry->pw_shell);
        if (QFileInfo(shell).isExecutable()) {
            return shell;
        }
    }
#endif
#if defined(Q_OS_MACOS)
    return QStringLiteral("/bin/zsh");
#else
    return QStringLiteral("/bin/sh");
#endif
}

/** @brief 各 CLI 官方安装脚本、版本管理器和包管理器的常见落点。 */
QStringList wellKnownDirectories()
{
    const QString home = QDir::homePath();
    QStringList directories;
#if defined(Q_OS_WIN)
    const QString appData = qEnvironmentVariable("APPDATA");
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (!appData.isEmpty()) {
        directories << appData + QStringLiteral("/npm");
    }
    if (!localAppData.isEmpty()) {
        directories << localAppData + QStringLiteral("/Programs/cursor-agent")
                    << localAppData + QStringLiteral("/pnpm");
    }
    directories << home + QStringLiteral("/.local/bin")
                << home + QStringLiteral("/.bun/bin")
                << home + QStringLiteral("/.volta/bin")
                << home + QStringLiteral("/scoop/shims");
#else
    directories << home + QStringLiteral("/.local/bin")
                << home + QStringLiteral("/.claude/local")
                << home + QStringLiteral("/bin")
                << home + QStringLiteral("/.npm-global/bin")
                << home + QStringLiteral("/.bun/bin")
                << home + QStringLiteral("/.volta/bin")
                << home + QStringLiteral("/.local/share/mise/shims")
                << home + QStringLiteral("/.asdf/shims")
                << home + QStringLiteral("/.local/share/pnpm")
                << home + QStringLiteral("/Library/pnpm")
                << home + QStringLiteral("/.cargo/bin")
                << home + QStringLiteral("/.deno/bin")
                << QStringLiteral("/opt/homebrew/bin")
                << QStringLiteral("/usr/local/bin")
                << QStringLiteral("/usr/bin");
    // nvm 每个 Node 版本各有一个 bin；版本号降序，优先使用较新的安装。
    QDir nvmVersions(home + QStringLiteral("/.nvm/versions/node"));
    QStringList versions = nvmVersions.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    std::sort(versions.begin(), versions.end(), [](const QString &left, const QString &right) {
        return QString::compare(left, right, Qt::CaseInsensitive) > 0;
    });
    for (const QString &version : std::as_const(versions)) {
        directories << nvmVersions.filePath(version) + QStringLiteral("/bin");
    }
#endif
    return directories;
}

QMutex &cacheMutex()
{
    static QMutex mutex;
    return mutex;
}

/** @brief 缓存的搜索目录；空 optional 表示尚未查询过。 */
std::optional<QStringList> &cachedDirectories()
{
    static std::optional<QStringList> directories;
    return directories;
}

QString roleHeading(const AiChatRole role)
{
    switch (role) {
    case AiChatRole::System:
        return QStringLiteral("SYSTEM");
    case AiChatRole::Assistant:
        return QStringLiteral("ASSISTANT");
    case AiChatRole::User:
        break;
    }
    return QStringLiteral("USER");
}

/**
 * @brief 找出文本中所有顶层 JSON 对象的起止位置，正确跳过字符串内的花括号与转义。
 */
QList<QPair<int, int>> topLevelObjectSpans(const QString &text)
{
    QList<QPair<int, int>> spans;
    int depth = 0;
    int start = -1;
    bool inString = false;
    bool escaped = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == QLatin1Char('\\')) {
                escaped = true;
            } else if (ch == QLatin1Char('"')) {
                inString = false;
            }
            continue;
        }
        if (ch == QLatin1Char('"') && depth > 0) {
            inString = true;
        } else if (ch == QLatin1Char('{')) {
            if (depth == 0) {
                start = i;
            }
            ++depth;
        } else if (ch == QLatin1Char('}') && depth > 0) {
            --depth;
            if (depth == 0 && start >= 0) {
                spans.append({start, i + 1});
                start = -1;
            }
        }
    }
    return spans;
}

bool isJsonObject(const QString &text)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &error);
    return error.error == QJsonParseError::NoError && document.isObject();
}

} // namespace

const QList<AgentSpec> &catalog()
{
    static const QList<AgentSpec> agents = {
        {QStringLiteral("claude"), QStringLiteral("Claude Code"),
         {QStringLiteral("claude")}, PromptDelivery::Stdin},
        {QStringLiteral("codex"), QStringLiteral("OpenAI Codex"),
         {QStringLiteral("codex")}, PromptDelivery::Stdin},
        {QStringLiteral("cursor-agent"), QStringLiteral("Cursor Agent"),
         {QStringLiteral("cursor-agent")}, PromptDelivery::Argument},
        {QStringLiteral("gemini"), QStringLiteral("Gemini CLI"),
         {QStringLiteral("gemini")}, PromptDelivery::Argument},
        {QStringLiteral("qwen"), QStringLiteral("Qwen Code"),
         {QStringLiteral("qwen")}, PromptDelivery::Argument},
    };
    return agents;
}

std::optional<AgentSpec> specById(const QString &id)
{
    for (const AgentSpec &spec : catalog()) {
        if (spec.id == id) {
            return spec;
        }
    }
    return std::nullopt;
}

QString queryLoginShellPath(const int timeoutMs)
{
#if defined(Q_OS_WIN)
    Q_UNUSED(timeoutMs);
    return {};
#else
    const QString shell = loginShell();
    const bool fish = QFileInfo(shell).fileName() == QStringLiteral("fish");
    // fish 的 $PATH 是列表，直接 %s 会把各项首尾相接，必须先用 ':' 连起来。
    const QString pathExpression = fish ? QStringLiteral("(string join : $PATH)")
                                        : QStringLiteral("\"$PATH\"");
    const QString script = QStringLiteral("printf '\\n%1%s%2\\n' %3")
                               .arg(kPathBeginMarker, kPathEndMarker, pathExpression);

    QProcess process;
    process.setStandardInputFile(QProcess::nullDevice());
    process.setProcessChannelMode(QProcess::SeparateChannels);
    // -i -l：mise/nvm/conda 的激活脚本多写在 .zshrc/.bashrc 这类只对交互 Shell 生效的文件里。
    process.start(shell, {QStringLiteral("-i"), QStringLiteral("-l"), QStringLiteral("-c"), script});
    if (!process.waitForStarted(timeoutMs)) {
        return {};
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(500);
        return {};
    }
    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    const int begin = output.lastIndexOf(kPathBeginMarker);
    if (begin < 0) {
        return {};
    }
    const int valueStart = begin + kPathBeginMarker.size();
    const int end = output.indexOf(kPathEndMarker, valueStart);
    if (end < 0) {
        return {};
    }
    return output.mid(valueStart, end - valueStart).trimmed();
#endif
}

QStringList searchDirectories(const QString &loginShellPath)
{
    QStringList candidates;
    candidates += loginShellPath.split(pathListSeparator(), Qt::SkipEmptyParts);
    candidates += qEnvironmentVariable("PATH").split(pathListSeparator(), Qt::SkipEmptyParts);
    candidates += wellKnownDirectories();

    QStringList directories;
    QSet<QString> seen;
    for (const QString &candidate : std::as_const(candidates)) {
        const QString cleaned = QDir::cleanPath(candidate.trimmed());
        if (cleaned.isEmpty() || seen.contains(cleaned) || !QFileInfo(cleaned).isDir()) {
            continue;
        }
        seen.insert(cleaned);
        directories.append(cleaned);
    }
    return directories;
}

QStringList cachedSearchDirectories(const bool refresh)
{
    {
        QMutexLocker locker(&cacheMutex());
        if (!refresh && cachedDirectories().has_value()) {
            return *cachedDirectories();
        }
    }
    // 查询登录 Shell 可能耗时数秒，不能持锁进行，否则并发调用者会一起被卡住。
    const QStringList directories = searchDirectories(queryLoginShellPath());
    QMutexLocker locker(&cacheMutex());
    cachedDirectories() = directories;
    return directories;
}

bool searchDirectoriesCached()
{
    QMutexLocker locker(&cacheMutex());
    return cachedDirectories().has_value();
}

QList<DetectedAgent> findAgents(const QStringList &directories)
{
    QList<DetectedAgent> agents;
    if (directories.isEmpty()) {
        // 空列表会让 findExecutable 回落到进程 PATH，结果就不再由调用方给定的目录决定。
        return agents;
    }
    for (const AgentSpec &spec : catalog()) {
        for (const QString &name : spec.executableNames) {
            const QString path = QStandardPaths::findExecutable(name, directories);
            if (!path.isEmpty()) {
                agents.append(DetectedAgent{spec.id, spec.displayName, path, QString()});
                break;
            }
        }
    }
    return agents;
}

QString probeVersion(const QString &executablePath,
                     const QProcessEnvironment &environment,
                     const int timeoutMs)
{
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setStandardInputFile(QProcess::nullDevice());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setWorkingDirectory(QDir::tempPath());
    process.start(executablePath, {QStringLiteral("--version")});
    if (!process.waitForStarted(timeoutMs)) {
        return {};
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(500);
        return {};
    }
    const QStringList lines = QString::fromLocal8Bit(process.readAll())
                                  .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            return trimmed.left(80);
        }
    }
    return {};
}

QList<DetectedAgent> detectAll(const bool refresh)
{
    const QStringList directories = cachedSearchDirectories(refresh);
    QList<DetectedAgent> agents = findAgents(directories);
    const QProcessEnvironment environment = childEnvironment(directories);
    for (DetectedAgent &agent : agents) {
        agent.version = probeVersion(agent.executablePath, environment);
    }
    return agents;
}

QProcessEnvironment childEnvironment(const QStringList &directories)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if (!directories.isEmpty()) {
        environment.insert(QStringLiteral("PATH"), directories.join(pathListSeparator()));
    }
    // CLI 输出直接被程序解析，颜色和进度动画只会变成需要清洗的转义序列。
    environment.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    environment.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    return environment;
}

QString renderTranscript(const QList<AiChatMessage> &messages)
{
    QString transcript = QStringLiteral(
        "You are being used as a plain language model by Shellium, an SSH terminal client.\n"
        "Do NOT use any tools, do NOT read or modify local files, and do NOT run any command "
        "yourself: Shellium executes the command you choose on the user's remote terminal and "
        "sends you the real output in the next turn.\n"
        "Below is the conversation so far. Follow the SYSTEM section exactly and reply to the "
        "last USER section as the ASSISTANT with only the JSON object it requires.\n");
    for (const AiChatMessage &message : messages) {
        transcript += QStringLiteral("\n### ") + roleHeading(message.role) + QLatin1Char('\n');
        transcript += message.content.trimmed();
        transcript += QLatin1Char('\n');
    }
    transcript += QStringLiteral("\n### ASSISTANT\n");
    return transcript;
}

Invocation buildInvocation(const AgentSpec &spec,
                           const QString &executablePath,
                           const QString &model,
                           const QString &prompt,
                           const QString &outputFilePath)
{
    Invocation invocation;
    invocation.program = executablePath;
    const QString trimmedModel = model.trimmed();
    QStringList &args = invocation.arguments;

    if (spec.id == QStringLiteral("claude")) {
        // --tools "" 关掉全部内置工具，模型只能回答文本；不持久化会话，免得每次提问都在
        // 用户的 Claude Code 历史里留下一条记录。
        args << QStringLiteral("-p") << QStringLiteral("--output-format") << QStringLiteral("text")
             << QStringLiteral("--tools") << QString()
             << QStringLiteral("--no-session-persistence");
        if (!trimmedModel.isEmpty()) {
            args << QStringLiteral("--model") << trimmedModel;
        }
    } else if (spec.id == QStringLiteral("codex")) {
        // 只读沙箱下 codex 即使决定执行命令也无法写入任何东西；最终回答单独写进文件，
        // 标准输出里的进度日志不参与解析。
        args << QStringLiteral("exec") << QStringLiteral("--sandbox") << QStringLiteral("read-only")
             << QStringLiteral("--skip-git-repo-check") << QStringLiteral("--ephemeral")
             << QStringLiteral("--color") << QStringLiteral("never");
        if (!outputFilePath.isEmpty()) {
            args << QStringLiteral("--output-last-message") << outputFilePath;
            invocation.outputFilePath = outputFilePath;
        }
        if (!trimmedModel.isEmpty()) {
            args << QStringLiteral("--model") << trimmedModel;
        }
        // "-" 表示从标准输入读取提示词。
        args << QStringLiteral("-");
    } else if (spec.id == QStringLiteral("cursor-agent")) {
        // ask 模式是 cursor-agent 的只读问答模式；--trust 跳过对空临时目录的信任询问，
        // 否则非交互模式会停在一个永远没人回答的提示上。
        args << QStringLiteral("-p") << QStringLiteral("--output-format") << QStringLiteral("text")
             << QStringLiteral("--mode") << QStringLiteral("ask") << QStringLiteral("--trust");
        if (!trimmedModel.isEmpty()) {
            args << QStringLiteral("--model") << trimmedModel;
        }
    } else {
        // gemini 与 qwen 同源：非交互模式下需要确认的工具调用会被直接拒绝。
        if (!trimmedModel.isEmpty()) {
            args << QStringLiteral("--model") << trimmedModel;
        }
        args << QStringLiteral("-p");
    }

    if (spec.promptDelivery == PromptDelivery::Stdin) {
        invocation.standardInput = prompt.toUtf8();
    } else {
        args << prompt;
    }
    return invocation;
}

QString extractReply(const QString &rawOutput)
{
    static const QRegularExpression ansiEscape(QStringLiteral(
        "\\x1B(?:\\[[0-9;?]*[ -/]*[@-~]|\\][^\\x07\\x1B]*(?:\\x07|\\x1B\\\\)|[@-Z\\\\-_])"));
    QString text = rawOutput;
    text.remove(ansiEscape);
    text = text.trimmed();
    if (isJsonObject(text)) {
        return text;
    }

    static const QRegularExpression fence(
        QStringLiteral("```(?:json)?\\s*\\n([\\s\\S]*?)\\n?```"));
    const QRegularExpressionMatch fenced = fence.match(text);
    if (fenced.hasMatch()) {
        const QString inner = fenced.captured(1).trimmed();
        if (isJsonObject(inner)) {
            return inner;
        }
    }

    // 只在恰好存在一个顶层对象时提取；出现多个对象往往是模型一次给了多步，必须原样交给
    // 协议校验去触发纠正，而不是替它挑一个。
    const QList<QPair<int, int>> spans = topLevelObjectSpans(text);
    if (spans.size() == 1) {
        const QString candidate = text.mid(spans.first().first,
                                           spans.first().second - spans.first().first);
        if (isJsonObject(candidate)) {
            return candidate;
        }
    }
    return text;
}

} // namespace LocalAgentCli

LocalAgentCliDetector::LocalAgentCliDetector(QObject *parent)
    : QObject(parent)
    , m_watcher(new QFutureWatcher<QList<LocalAgentCli::DetectedAgent>>(this))
{
    connect(m_watcher, &QFutureWatcher<QList<LocalAgentCli::DetectedAgent>>::finished,
            this, [this]() { emit finished(m_watcher->result()); });
}

LocalAgentCliDetector::~LocalAgentCliDetector()
{
    // 不等待工作线程：它只读写局部对象和受互斥量保护的缓存，不引用本对象；关闭设置页时
    // 若还要等登录 Shell 和逐个 --version 跑完，界面会无端卡住数秒。watcher 随本对象析构
    // 后，迟到的结果自然被丢弃。
}

void LocalAgentCliDetector::start(const bool refresh)
{
    if (isRunning()) {
        return;
    }
    m_watcher->setFuture(QtConcurrent::run([refresh]() {
        return LocalAgentCli::detectAll(refresh);
    }));
}

bool LocalAgentCliDetector::isRunning() const
{
    return m_watcher->isRunning();
}
