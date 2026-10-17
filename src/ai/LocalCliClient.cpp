#include "ai/LocalCliClient.h"

#include "ai/LocalAgentCli.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

namespace {

/** @brief stderr 只保留末尾这么多字节，足够容纳 CLI 的错误说明，又不会让失败气泡刷屏。 */
constexpr qsizetype kMaxStderrBytes = 64 * 1024;
/** @brief 失败提示里附带的 stderr 末尾长度。 */
constexpr int kStderrTailChars = 400;

/**
 * @brief 所有 CLI 调用共用的空工作目录。
 *
 * 不用应用自身或用户主目录作为 cwd：CLI 会读取当前目录下的 CLAUDE.md、AGENTS.md 一类
 * 项目说明并把它们当作指令，空目录可以把这种外部影响完全隔离掉。
 */
QString workspaceDirectory()
{
    const QString path = QDir(QDir::tempPath()).filePath(QStringLiteral("shellium-ai-cli"));
    QDir().mkpath(path);
    return path;
}

QString stderrTail(const QByteArray &stderrBytes)
{
    const QString text = QString::fromLocal8Bit(stderrBytes).trimmed();
    if (text.size() <= kStderrTailChars) {
        return text;
    }
    return QStringLiteral("…") + text.right(kStderrTailChars);
}

} // namespace

LocalCliClient::LocalCliClient(QObject *parent)
    : QObject(parent)
    , m_timeoutTimer(new QTimer(this))
    , m_resolveWatcher(new QFutureWatcher<QStringList>(this))
{
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout, this, [this]() {
        fail(tr("%1 did not answer within %2 seconds and was stopped.")
                 .arg(m_displayName, QString::number(m_timeoutMs / 1000)));
    });
    connect(m_resolveWatcher, &QFutureWatcher<QStringList>::finished, this, [this]() {
        // 结果属于哪次请求由 property 记录；已被中止或被新请求取代时直接丢弃。
        const quint64 generation = m_resolveWatcher->property("generation").toULongLong();
        if (!m_busy || generation != m_generation) {
            return;
        }
        launch(m_resolveWatcher->result());
    });
}

LocalCliClient::~LocalCliClient()
{
    // 路径查询线程不引用本对象，无需等待；cleanup() 负责终止仍在运行的 CLI 进程。
    cleanup();
}

bool LocalCliClient::isBusy() const
{
    return m_busy;
}

void LocalCliClient::setTimeoutMs(const int timeoutMs)
{
    m_timeoutMs = qMax(1000, timeoutMs);
}

void LocalCliClient::sendMessage(const AiSettings &settings, const QList<AiChatMessage> &messages)
{
    cleanup();
    ++m_generation;
    m_settings = settings;
    m_messages = messages;
    m_busy = true;

    const std::optional<LocalAgentCli::AgentSpec> spec =
        LocalAgentCli::specById(settings.cliAgentId);
    m_displayName = spec.has_value() ? spec->displayName : settings.cliAgentId;
    if (!spec.has_value()) {
        // 延迟到下一轮事件循环再报错，保持"sendMessage 返回后才收到结果"的统一时序。
        QTimer::singleShot(0, this, [this, generation = m_generation]() {
            if (m_busy && generation == m_generation) {
                fail(tr("Unknown local agent CLI: %1").arg(m_settings.cliAgentId));
            }
        });
        return;
    }

    if (LocalAgentCli::searchDirectoriesCached()) {
        const QStringList directories = LocalAgentCli::cachedSearchDirectories();
        QTimer::singleShot(0, this, [this, directories, generation = m_generation]() {
            if (m_busy && generation == m_generation) {
                launch(directories);
            }
        });
        return;
    }
    m_resolveWatcher->setProperty("generation", QVariant::fromValue(m_generation));
    m_resolveWatcher->setFuture(QtConcurrent::run([]() {
        return LocalAgentCli::cachedSearchDirectories();
    }));
}

void LocalCliClient::launch(const QStringList &searchDirectories)
{
    const std::optional<LocalAgentCli::AgentSpec> spec =
        LocalAgentCli::specById(m_settings.cliAgentId);
    Q_ASSERT(spec.has_value());

    QString executable = m_settings.cliExecutablePath.trimmed();
    if (!executable.isEmpty() && !QFileInfo(executable).isExecutable()) {
        fail(tr("The configured path for %1 is not an executable file: %2")
                 .arg(m_displayName, executable));
        return;
    }
    if (executable.isEmpty()) {
        for (const LocalAgentCli::DetectedAgent &agent :
             LocalAgentCli::findAgents(searchDirectories)) {
            if (agent.id == spec->id) {
                executable = agent.executablePath;
                break;
            }
        }
    }
    if (executable.isEmpty()) {
        fail(tr("%1 was not found on this computer. Install it, or set its path in Settings → AI Assistant.")
                 .arg(m_displayName));
        return;
    }

    m_outputDir = std::make_unique<QTemporaryDir>();
    m_outputFilePath = m_outputDir->isValid()
        ? m_outputDir->filePath(QStringLiteral("reply.txt"))
        : QString();
    const LocalAgentCli::Invocation invocation = LocalAgentCli::buildInvocation(
        *spec, executable, m_settings.cliModel,
        LocalAgentCli::renderTranscript(m_messages), m_outputFilePath);

    m_outputFormat = invocation.outputFormat;
    m_streamParser = LocalAgentCli::ClaudeStreamParser();

    auto *process = new QProcess(this);
    m_process = process;
    process->setProcessEnvironment(LocalAgentCli::childEnvironment(searchDirectories));
    process->setWorkingDirectory(workspaceDirectory());
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process]() {
        consumeStandardOutput(process->readAllStandardOutput());
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, process]() {
        m_stderr += process->readAllStandardError();
        if (m_stderr.size() > kMaxStderrBytes) {
            m_stderr = m_stderr.right(kMaxStderrBytes);
        }
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && process == m_process) {
            fail(tr("Could not start %1: %2").arg(m_displayName, process->errorString()));
        }
    });
    connect(process, &QProcess::finished, this,
            [this, process](int exitCode, QProcess::ExitStatus status) {
                if (process != m_process) {
                    return;
                }
                handleFinished(exitCode, status == QProcess::CrashExit);
            });

    m_timeoutTimer->start(m_timeoutMs);
    process->start(invocation.program, invocation.arguments);
    if (!invocation.standardInput.isEmpty()) {
        process->write(invocation.standardInput);
    }
    // 无论是否写入内容都要关闭标准输入：claude/codex 读到 EOF 才开始回答。
    process->closeWriteChannel();
}

void LocalCliClient::consumeStandardOutput(const QByteArray &bytes)
{
    if (m_outputFormat != LocalAgentCli::OutputFormat::ClaudeStreamJson) {
        m_stdout += bytes;
        return;
    }
    const qsizetype textLength = m_streamParser.text().size();
    const qsizetype thinkingLength = m_streamParser.thinking().size();
    m_streamParser.feed(bytes);
    // 新消息开始时解析器会清空旧文本，长度变短同样意味着需要刷新界面。
    if (m_streamParser.thinking().size() != thinkingLength) {
        emit reasoningChunkReceived(m_streamParser.thinking());
    }
    if (m_streamParser.text().size() != textLength) {
        emit replyChunkReceived(m_streamParser.text());
    }
}

void LocalCliClient::handleFinished(const int exitCode, const bool crashed)
{
    if (!m_busy) {
        return;
    }
    if (m_process != nullptr) {
        consumeStandardOutput(m_process->readAllStandardOutput());
        m_stderr += m_process->readAllStandardError();
    }
    const bool streaming = m_outputFormat == LocalAgentCli::OutputFormat::ClaudeStreamJson;
    if (streaming) {
        m_streamParser.finish();
    }
    if (streaming && m_streamParser.hasResult() && m_streamParser.resultIsError()) {
        // 未登录、额度用尽等错误写在 result 事件里，标准错误往往是空的。
        const QString detail = m_streamParser.result().trimmed();
        fail(detail.isEmpty()
                 ? tr("%1 exited with code %2.").arg(m_displayName, QString::number(exitCode))
                 : tr("%1 exited with code %2: %3")
                       .arg(m_displayName, QString::number(exitCode), detail));
        return;
    }
    if (crashed || exitCode != 0) {
        const QString tail = stderrTail(m_stderr.isEmpty() ? m_stdout : m_stderr);
        fail(tail.isEmpty()
                 ? tr("%1 exited with code %2.").arg(m_displayName, QString::number(exitCode))
                 : tr("%1 exited with code %2: %3")
                       .arg(m_displayName, QString::number(exitCode), tail));
        return;
    }

    QString raw;
    if (streaming) {
        raw = m_streamParser.hasResult() ? m_streamParser.result() : m_streamParser.text();
    } else if (!m_outputFilePath.isEmpty()) {
        QFile file(m_outputFilePath);
        if (file.open(QIODevice::ReadOnly)) {
            raw = QString::fromUtf8(file.readAll());
        }
    }
    if (!streaming && raw.trimmed().isEmpty()) {
        raw = QString::fromUtf8(m_stdout);
    }
    const QString reply = LocalAgentCli::extractReply(raw);
    if (reply.isEmpty()) {
        fail(tr("%1 returned an empty answer.").arg(m_displayName));
        return;
    }
    cleanup();
    emit replyFinished(reply);
}

void LocalCliClient::fail(const QString &message)
{
    if (!m_busy) {
        return;
    }
    cleanup();
    emit replyFailed(message);
}

void LocalCliClient::abortActiveRequest()
{
    // 递增代号让工作线程里尚未返回的路径查询结果作废。
    ++m_generation;
    cleanup();
}

void LocalCliClient::cleanup()
{
    m_busy = false;
    m_timeoutTimer->stop();
    if (m_process != nullptr) {
        QProcess *process = m_process;
        m_process = nullptr;
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
        process->deleteLater();
    }
    m_outputDir.reset();
    m_outputFilePath.clear();
    m_stdout.clear();
    m_stderr.clear();
    m_streamParser = LocalAgentCli::ClaudeStreamParser();
}
