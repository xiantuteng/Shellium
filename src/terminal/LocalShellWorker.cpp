#include "terminal/LocalShellWorker.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QMutexLocker>
#include <QStandardPaths>

#include <algorithm>
#include <vector>
#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <consoleapi.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(Q_OS_MACOS)
#include <util.h>
#else
#include <pty.h>
#endif
#endif

namespace {

/**
 * @brief 已 emit 给 GUI 线程、但尚未被消费的字节数上限。
 *
 * 读取端必须有背压，否则 `cat` 一个大文件会这样把界面压死：PTY 读循环一路读到 EAGAIN，
 * 每块都 emit 一个队列信号；而 GUI 线程那一侧要把字节喂进 libvterm、重排 QTextDocument，
 * 实测吞吐只有约 8 MB/s（Release 构建，Debug 更低），比 `cat` 往 PTY 里灌的速度慢一到
 * 两个数量级。差额全部堆在 GUI 线程的事件队列里——34MB 的 jar 实测让常驻内存冲到 234MB，
 * 而且这段时间事件队列里排在后面的重绘、按键一次都轮不到，界面就是这样"卡住"的。
 *
 * 真实终端不会这样，因为它们读得没那么急：PTY 缓冲区填满后 `cat` 自己会阻塞在 write()
 * 上。这里把那个行为补回来——在途字节超过上限就先不读 PTY，让内核缓冲区顶住。
 *
 * 1 MiB 约合 GUI 线程 0.1 秒的工作量：既足够让正常命令输出完全不受限流影响（普通命令
 * 的输出远小于这个量），又把界面的最坏响应延迟压在一帧到几帧之内。
 */
constexpr qint64 kOutputBacklogHighWaterBytes = 1 << 20;

/**
 * @brief 背压生效期间两次重试之间的等待（毫秒）。
 *
 * 这段时间里不能去 poll POLLIN：数据就在那儿，poll 会立刻返回，循环退化成 100% CPU 的
 * 空转。改为只等一小段时间再回来看 GUI 线程消费完没有，同时保持键盘写入通路畅通——用户
 * 必须还能按下 Ctrl+C 打断 `cat`，这也是真实终端在输出缓冲区打满时的行为。
 */
constexpr int kOutputBacklogRetryMilliseconds = 5;

#if !defined(Q_OS_WIN)
/** @brief 读取当前用户 passwd 记录里登记的登录 Shell，查不到时返回空串。 */
QString accountShellPath()
{
    long bufferSize = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufferSize <= 0) {
        bufferSize = 16384;
    }
    std::vector<char> buffer(static_cast<size_t>(bufferSize));
    struct passwd entry {};
    struct passwd *result = nullptr;
    if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &result) != 0
        || result == nullptr || result->pw_shell == nullptr) {
        return {};
    }
    return QFile::decodeName(result->pw_shell);
}
#endif

/**
 * @brief 解析当前平台的默认交互式 Shell。
 * @param requestedPath 用户或测试显式指定的路径；非空时原样保留。
 * @return 可供当前平台启动的程序路径。
 *
 * Windows 优先 PowerShell 7，其次系统 Windows PowerShell，最后退回 ComSpec/cmd；POSIX
 * 启动用户的登录 Shell，规则见 LocalShellWorker::resolvePosixShell。这里只探测可执行文件，
 * 不伪造 Windows Terminal 环境变量。
 */
QString resolveShellPath(const QString &requestedPath)
{
    if (!requestedPath.trimmed().isEmpty()) {
        return requestedPath;
    }
#if defined(Q_OS_WIN)
    const QString powerShell7 = QStandardPaths::findExecutable(QStringLiteral("pwsh.exe"));
    if (!powerShell7.isEmpty()) {
        return powerShell7;
    }
    const QString windowsPowerShell =
        QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
    if (!windowsPowerShell.isEmpty()) {
        return windowsPowerShell;
    }
    const QString comSpec = qEnvironmentVariable("ComSpec");
    return comSpec.isEmpty() ? QStringLiteral("cmd.exe") : comSpec;
#else
    return LocalShellWorker::resolvePosixShell(qEnvironmentVariable("SHELL"),
                                               accountShellPath(),
                                               QFile::exists(QStringLiteral("/.flatpak-info")));
#endif
}

#if defined(Q_OS_WIN)
/** @brief 把 Win32 错误码转换为不会泄漏临时缓冲的可读中文诊断。 */
QString windowsErrorText(const DWORD errorCode)
{
    wchar_t *message = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
                                            | FORMAT_MESSAGE_FROM_SYSTEM
                                            | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr,
                                        errorCode,
                                        0,
                                        reinterpret_cast<wchar_t *>(&message),
                                        0,
                                        nullptr);
    const QString result = length > 0 && message != nullptr
        ? QString::fromWCharArray(message, static_cast<qsizetype>(length)).trimmed()
        : QStringLiteral("Windows 错误 %1").arg(errorCode);
    if (message != nullptr) {
        LocalFree(message);
    }
    return result;
}

/** @brief 关闭有效 Win32 HANDLE 并把变量清空，保证所有失败路径可重复清理。 */
void closeHandle(HANDLE &handle) noexcept
{
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    handle = nullptr;
}
#endif

} // namespace

LocalShellWorker::LocalShellWorker(QString shellPath, QObject *parent)
    : QThread(parent)
    , m_shellPath(resolveShellPath(shellPath))
{
}

LocalShellWorker::~LocalShellWorker()
{
    requestStop();
    // POSIX 与 ConPTY 循环都会快速观察停止状态；保留 5 秒覆盖正常退出宽限期和进程树
    // 强制清理，绝不能在 QThread 仍运行时析构对象。
    wait(5000);
}

void LocalShellWorker::enqueueInput(const QByteArray &bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    QMutexLocker locker(&m_mutex);
    m_pendingInput.append(bytes);
}

void LocalShellWorker::updateTerminalSize(const int columns, const int rows)
{
    QMutexLocker locker(&m_mutex);
    const int normalizedColumns = std::max(columns, 1);
    const int normalizedRows = std::max(rows, 1);
    if (normalizedColumns != m_columns || normalizedRows != m_rows) {
        m_columns = normalizedColumns;
        m_rows = normalizedRows;
        m_sizeDirty = true;
    }
}

void LocalShellWorker::requestStop()
{
    QMutexLocker locker(&m_mutex);
    m_stopRequested = true;
}

QList<QByteArray> LocalShellWorker::posixLaunchArguments(const QString &shellPath,
                                                         const QString &homeDirectory,
                                                         const bool insideFlatpak)
{
    const QByteArray encodedShell = QFile::encodeName(shellPath);
    if (insideFlatpak) {
        // --watch-bus：应用崩溃、D-Bus 连接断开时宿主上的 Shell 跟着结束，不会留下孤儿进程。
        return {QByteArrayLiteral("/usr/bin/flatpak-spawn"),
                QByteArrayLiteral("--host"),
                QByteArrayLiteral("--watch-bus"),
                QByteArrayLiteral("--directory=") + QFile::encodeName(homeDirectory),
                QByteArrayLiteral("--env=TERM=xterm-256color"),
                QByteArrayLiteral("--env=COLORTERM=truecolor"),
                encodedShell,
                QByteArrayLiteral("-l")};
    }
    // 通过 env 设置终端变量，避免 fork 后调用可能持锁的 setenv。
    return {QByteArrayLiteral("/usr/bin/env"),
            QByteArrayLiteral("TERM=xterm-256color"),
            QByteArrayLiteral("COLORTERM=truecolor"),
            encodedShell,
            QByteArrayLiteral("-l")};
}

QString LocalShellWorker::resolvePosixShell(const QString &environmentShell,
                                           const QString &accountShell,
                                           const bool insideFlatpak)
{
    if (insideFlatpak) {
        return QDir::isAbsolutePath(environmentShell) ? environmentShell
                                                      : QStringLiteral("/bin/bash");
    }
    const auto usable = [](const QString &path) {
        if (!QDir::isAbsolutePath(path)) {
            return false;
        }
        const QFileInfo info(path);
        const QString name = info.fileName();
        // /usr/sbin/nologin、/bin/false 可执行但只会立刻退出，选中它们等于终端打开即关闭。
        return info.isFile() && info.isExecutable() && name != QLatin1String("nologin")
            && name != QLatin1String("false");
    };
    for (const QString &candidate : {environmentShell,
                                     accountShell,
                                     QStringLiteral("/bin/bash"),
                                     QStringLiteral("/usr/bin/bash")}) {
        if (usable(candidate)) {
            return candidate;
        }
    }
    return QStringLiteral("/bin/sh");
}

void LocalShellWorker::run()
{
#if defined(Q_OS_WIN)
    if (QStandardPaths::findExecutable(m_shellPath).isEmpty() && !QFile::exists(m_shellPath)) {
        emit shellError(tr("The local shell does not exist: %1").arg(m_shellPath));
        return;
    }

    HANDLE pseudoConsoleInputRead = nullptr;
    HANDLE applicationInputWrite = nullptr;
    HANDLE applicationOutputRead = nullptr;
    HANDLE pseudoConsoleOutputWrite = nullptr;
    HPCON pseudoConsole = nullptr;
    PROCESS_INFORMATION processInfo {};
    HANDLE job = nullptr;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;

    const auto fail = [this](const QString &action) {
        emit shellError(tr("%1: %2").arg(action, windowsErrorText(GetLastError())));
    };
    const auto cleanup = [&]() {
        if (attributes != nullptr) {
            DeleteProcThreadAttributeList(attributes);
            HeapFree(GetProcessHeap(), 0, attributes);
            attributes = nullptr;
        }
        closeHandle(processInfo.hThread);
        closeHandle(processInfo.hProcess);
        closeHandle(applicationInputWrite);
        closeHandle(applicationOutputRead);
        closeHandle(pseudoConsoleInputRead);
        closeHandle(pseudoConsoleOutputWrite);
        if (pseudoConsole != nullptr) {
            ClosePseudoConsole(pseudoConsole);
            pseudoConsole = nullptr;
        }
        closeHandle(job);
    };

    SECURITY_ATTRIBUTES pipeSecurity {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    if (!CreatePipe(&pseudoConsoleInputRead, &applicationInputWrite, &pipeSecurity, 0)
        || !CreatePipe(&applicationOutputRead, &pseudoConsoleOutputWrite, &pipeSecurity, 0)
        || !SetHandleInformation(applicationInputWrite, HANDLE_FLAG_INHERIT, 0)
        || !SetHandleInformation(applicationOutputRead, HANDLE_FLAG_INHERIT, 0)) {
        fail(tr("Could not create the ConPTY pipe"));
        cleanup();
        return;
    }

    COORD initialSize {};
    {
        QMutexLocker locker(&m_mutex);
        initialSize.X = static_cast<SHORT>(std::clamp(m_columns, 1, 32767));
        initialSize.Y = static_cast<SHORT>(std::clamp(m_rows, 1, 32767));
        m_sizeDirty = false;
    }
    const HRESULT createResult = CreatePseudoConsole(initialSize,
                                                     pseudoConsoleInputRead,
                                                     pseudoConsoleOutputWrite,
                                                     0,
                                                     &pseudoConsole);
    if (FAILED(createResult)) {
        SetLastError(HRESULT_CODE(createResult));
        fail(tr("Could not create the Windows ConPTY"));
        cleanup();
        return;
    }
    // ConPTY 已复制管道句柄；父进程立即关闭自己的伪终端侧副本，避免 EOF 永远无法传播。
    closeHandle(pseudoConsoleInputRead);
    closeHandle(pseudoConsoleOutputWrite);

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attributeBytes));
    if (attributes == nullptr
        || !InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes)
        || !UpdateProcThreadAttribute(attributes,
                                      0,
                                      PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                      pseudoConsole,
                                      sizeof(pseudoConsole),
                                      nullptr,
                                      nullptr)) {
        fail(tr("Could not configure the ConPTY child process attributes"));
        cleanup();
        return;
    }

    STARTUPINFOEXW startupInfo {};
    startupInfo.StartupInfo.cb = sizeof(startupInfo);
    startupInfo.lpAttributeList = attributes;
    QString commandLine = QStringLiteral("\"") + QDir::toNativeSeparators(m_shellPath)
        + QStringLiteral("\"");
    const QString executableName = QFileInfo(m_shellPath).fileName().toLower();
    if (executableName == QStringLiteral("pwsh.exe")
        || executableName == QStringLiteral("powershell.exe")) {
        commandLine.append(QStringLiteral(" -NoLogo"));
    }
    std::wstring mutableCommand = commandLine.toStdWString();
    mutableCommand.push_back(L'\0');
    // 为子进程显式指定主目录，避免继承应用包或构建目录；不改变应用的全局 cwd。
    // 宽字符串存活至 CreateProcessW 返回；目录不可访问时沿用下方的启动错误处理。
    const std::wstring homeDirectory = QDir::toNativeSeparators(QDir::homePath()).toStdWString();
    if (!CreateProcessW(nullptr,
                        mutableCommand.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                        nullptr,
                        homeDirectory.c_str(),
                        &startupInfo.StartupInfo,
                        &processInfo)) {
        fail(tr("Could not start the Windows local shell"));
        cleanup();
        return;
    }

    job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        // Job 绑定失败不阻止终端使用；退出时仍会直接终止主进程，但记录错误便于排查残留树。
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits))
            || !AssignProcessToJobObject(job, processInfo.hProcess)) {
            emit shellError(tr("Could not configure process tree cleanup for the local shell: %1")
                                .arg(windowsErrorText(GetLastError())));
            closeHandle(job);
        }
    }
    closeHandle(processInfo.hThread);
    emit shellStarted();

    QByteArray readBuffer(64 * 1024, Qt::Uninitialized);
    bool ioFailed = false;
    while (!shouldStop()) {
        QByteArray pendingInput;
        bool resizeRequired = false;
        int columns = 80;
        int rows = 24;
        {
            QMutexLocker locker(&m_mutex);
            pendingInput.swap(m_pendingInput);
            resizeRequired = m_sizeDirty;
            columns = m_columns;
            rows = m_rows;
            m_sizeDirty = false;
        }
        if (resizeRequired && pseudoConsole != nullptr) {
            const COORD size {static_cast<SHORT>(std::clamp(columns, 1, 32767)),
                              static_cast<SHORT>(std::clamp(rows, 1, 32767))};
            const HRESULT resizeResult = ResizePseudoConsole(pseudoConsole, size);
            if (FAILED(resizeResult)) {
                emit shellError(tr("Failed to resize ConPTY: HRESULT 0x%1")
                                    .arg(static_cast<quint32>(resizeResult), 8, 16, QLatin1Char('0')));
            }
        }
        if (!pendingInput.isEmpty()) {
            DWORD written = 0;
            if (!WriteFile(applicationInputWrite,
                           pendingInput.constData(),
                           static_cast<DWORD>(pendingInput.size()),
                           &written,
                           nullptr)) {
                fail(tr("Failed to write to ConPTY"));
                ioFailed = true;
                break;
            }
            // 匿名管道的单次 WriteFile 理论上可部分完成；未写部分重新排到队首，避免丢键。
            if (written < static_cast<DWORD>(pendingInput.size())) {
                QMutexLocker locker(&m_mutex);
                m_pendingInput.prepend(pendingInput.mid(static_cast<qsizetype>(written)));
            }
        }

        // 与 POSIX 分支同一套背压：GUI 线程没消化完就先不读，让数据留在 ConPTY 管道里顶住
        // 写入端。见 kOutputBacklogHighWaterBytes。
        DWORD available = 0;
        if (!outputBacklogFull()
            && PeekNamedPipe(applicationOutputRead, nullptr, 0, nullptr, &available, nullptr)
            && available > 0) {
            DWORD bytesRead = 0;
            const DWORD requested = std::min<DWORD>(available,
                                                    static_cast<DWORD>(readBuffer.size()));
            if (ReadFile(applicationOutputRead, readBuffer.data(), requested, &bytesRead, nullptr)
                && bytesRead > 0) {
                m_outputInFlight.fetch_add(static_cast<qint64>(bytesRead),
                                           std::memory_order_relaxed);
                emit outputReady(QByteArray(readBuffer.constData(),
                                            static_cast<qsizetype>(bytesRead)));
            }
        }
        const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, 10);
        if (waitResult == WAIT_OBJECT_0) {
            // 子进程退出与匿名管道数据到达并非同一个事件。退出后再排空一次 ConPTY 输出，
            // 否则紧接 exit 的最后一行可能仍在管道中，却被 cleanup 直接关闭而丢失。
            while (PeekNamedPipe(applicationOutputRead,
                                 nullptr,
                                 0,
                                 nullptr,
                                 &available,
                                 nullptr)
                   && available > 0) {
                DWORD bytesRead = 0;
                const DWORD requested = std::min<DWORD>(
                    available, static_cast<DWORD>(readBuffer.size()));
                if (!ReadFile(applicationOutputRead,
                              readBuffer.data(),
                              requested,
                              &bytesRead,
                              nullptr)
                    || bytesRead == 0) {
                    break;
                }
                // 收尾排空不受背压限制：子进程已退出，剩下的就是最后那点输出，必须发完。
                m_outputInFlight.fetch_add(static_cast<qint64>(bytesRead),
                                           std::memory_order_relaxed);
                emit outputReady(QByteArray(readBuffer.constData(),
                                            static_cast<qsizetype>(bytesRead)));
            }
            DWORD exitCode = 0;
            GetExitCodeProcess(processInfo.hProcess, &exitCode);
            cleanup();
            emit shellFinished(static_cast<int>(exitCode));
            return;
        }
    }

    // 先关闭输入表达 EOF，给 PowerShell/cmd 一个正常收尾窗口；超时再借助 Job 清理整棵树。
    closeHandle(applicationInputWrite);
    if (WaitForSingleObject(processInfo.hProcess, 2500) == WAIT_TIMEOUT) {
        if (job != nullptr) {
            TerminateJobObject(job, 137);
        } else {
            TerminateProcess(processInfo.hProcess, 137);
        }
        WaitForSingleObject(processInfo.hProcess, 1000);
    }
    DWORD exitCode = ioFailed ? 1 : 0;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    cleanup();
    emit shellFinished(static_cast<int>(exitCode));
#else
    // 沙箱里看不到宿主的 Shell，存在性只能交给宿主判断：不存在时 flatpak-spawn 以非零码退出，
    // 照常经 shellFinished 报告。
    const bool insideFlatpak = QFile::exists(QStringLiteral("/.flatpak-info"));
    if (!insideFlatpak && !QFile::exists(m_shellPath)) {
        emit shellError(tr("The local shell does not exist: %1").arg(m_shellPath));
        return;
    }

    struct winsize initialSize {};
    {
        QMutexLocker locker(&m_mutex);
        initialSize.ws_col = static_cast<unsigned short>(m_columns);
        initialSize.ws_row = static_cast<unsigned short>(m_rows);
        m_sizeDirty = false;
    }

    // 在 fork 前完成 Qt 字符串转换，避免子进程触碰其他线程可能持有的内存分配锁。
    const QByteArray encodedHomeDirectory = QFile::encodeName(QDir::homePath());
    const char *homeDirectory = encodedHomeDirectory.constData();
    const QList<QByteArray> launchArguments =
        posixLaunchArguments(m_shellPath, QDir::homePath(), insideFlatpak);
    std::vector<char *> launchArgv;
    launchArgv.reserve(static_cast<size_t>(launchArguments.size()) + 1);
    for (const QByteArray &argument : launchArguments) {
        launchArgv.push_back(const_cast<char *>(argument.constData()));
    }
    launchArgv.push_back(nullptr);
    int masterFd = -1;
    const pid_t childPid = forkpty(&masterFd, nullptr, nullptr, &initialSize);
    if (childPid < 0) {
        emit shellError(tr("Could not create the local PTY: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
        return;
    }

    if (childPid == 0) {
        // 仅改变子进程目录，避免影响应用其他线程的相对路径 IO。chdir 是异步信号安全的；
        // 主目录不可访问时以 126 退出，由现有 shellFinished 路径报告，不能静默用错误目录启动。
        if (chdir(homeDirectory) != 0) {
            _exit(126);
        }
        // fork 后仅使用异步信号安全调用；参数数组已在 fork 前备好。
        execv(launchArgv.front(), launchArgv.data());
        _exit(127);
    }

    const int currentFlags = fcntl(masterFd, F_GETFL, 0);
    if (currentFlags >= 0) {
        fcntl(masterFd, F_SETFL, currentFlags | O_NONBLOCK);
    }
    emit shellStarted();

    QByteArray readBuffer(64 * 1024, Qt::Uninitialized);
    bool ioFailed = false;
    while (!shouldStop()) {
        QByteArray pendingInput;
        bool resizeRequired = false;
        int columns = 80;
        int rows = 24;
        {
            QMutexLocker locker(&m_mutex);
            pendingInput.swap(m_pendingInput);
            resizeRequired = m_sizeDirty;
            columns = m_columns;
            rows = m_rows;
            m_sizeDirty = false;
        }

        if (resizeRequired) {
            struct winsize updatedSize {};
            updatedSize.ws_col = static_cast<unsigned short>(columns);
            updatedSize.ws_row = static_cast<unsigned short>(rows);
            // TIOCSWINSZ 会让内核向前台进程组发送 SIGWINCH，无需应用手工查找 zsh 子进程组。
            ioctl(masterFd, TIOCSWINSZ, &updatedSize);
        }

        qsizetype writeOffset = 0;
        while (writeOffset < pendingInput.size()) {
            const ssize_t written = ::write(masterFd,
                                            pendingInput.constData() + writeOffset,
                                            static_cast<size_t>(pendingInput.size() - writeOffset));
            if (written > 0) {
                writeOffset += written;
            } else if (written < 0 && (errno == EAGAIN || errno == EINTR)) {
                QThread::msleep(2);
            } else {
                ioFailed = true;
                emit shellError(tr("Failed to write to the local PTY: %1")
                                    .arg(QString::fromLocal8Bit(std::strerror(errno))));
                break;
            }
        }
        if (ioFailed) {
            break;
        }

        // GUI 线程还没消化完上一批就别再读了：不注册 POLLIN，让数据留在内核 PTY 缓冲区里
        // 顶住写入端。见 kOutputBacklogHighWaterBytes。
        const bool backlogFull = outputBacklogFull();
        struct pollfd descriptor {masterFd,
                                  static_cast<short>(backlogFull ? 0 : (POLLIN | POLLHUP | POLLERR)),
                                  0};
        const int pollResult = poll(&descriptor, 1,
                                    backlogFull ? kOutputBacklogRetryMilliseconds : 20);
        if (pollResult < 0 && errno != EINTR) {
            emit shellError(tr("Failed to poll the local PTY: %1")
                                .arg(QString::fromLocal8Bit(std::strerror(errno))));
            ioFailed = true;
            break;
        }
        if (pollResult > 0 && (descriptor.revents & POLLIN) != 0) {
            // 读到 EAGAIN 或攒够一批就交还控制权。原本这里一路读到 EAGAIN，而 `cat` 灌得
            // 比读得快，这个内循环能连续跑上几千轮、往 GUI 线程灌进整个文件。
            while (!outputBacklogFull()) {
                const ssize_t bytesRead = ::read(masterFd, readBuffer.data(), readBuffer.size());
                if (bytesRead > 0) {
                    m_outputInFlight.fetch_add(bytesRead, std::memory_order_relaxed);
                    emit outputReady(QByteArray(readBuffer.constData(), bytesRead));
                } else if (bytesRead < 0 && errno == EINTR) {
                    continue;
                } else {
                    break;
                }
            }
        }
        if (pollResult > 0 && (descriptor.revents & (POLLHUP | POLLERR)) != 0) {
            break;
        }

        int status = 0;
        if (waitpid(childPid, &status, WNOHANG) == childPid) {
            close(masterFd);
            const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status)
                                                  : 128 + WTERMSIG(status);
            emit shellFinished(exitCode);
            return;
        }
    }

    // 关闭期间必须继续排空 PTY，否则连 SIGKILL 都杀不掉子进程：输出缓冲区满而主端不再
    // 读时，写终端的进程停在内核的 tty 输出等待里，信号要等它回到用户态才生效——而它回不
    // 去。实测现象是 kill(pid, SIGKILL) 返回 0、kill(pid, 0) 仍然返回 0，waitpid 永远等不
    // 到它退出，整个工作线程就此卡死，析构时报 "QThread: Destroyed while thread is still
    // running"。用户关掉一个正在 `cat` 大文件的标签页就会撞上。
    //
    // 这里读到的字节直接丢弃：会话已经在关闭，没有任何地方还需要它们；也正因为丢弃，
    // 不能计入 m_outputInFlight。
    const auto drainPty = [masterFd, &readBuffer]() {
        while (::read(masterFd, readBuffer.data(), readBuffer.size()) > 0) {
        }
    };

    // 用户关闭终端时先发送 SIGHUP，符合真实终端关闭语义；超时后才使用 SIGKILL，防止
    // 后台任务永久占用进程。所有 waitpid 调用都在本工作线程，避免跨线程竞争回收子进程。
    kill(childPid, SIGHUP);
    int status = 0;
    for (int attempt = 0; attempt < 50; ++attempt) {
        drainPty();
        if (waitpid(childPid, &status, WNOHANG) == childPid) {
            close(masterFd);
            emit shellFinished(WIFEXITED(status) ? WEXITSTATUS(status)
                                                 : 128 + WTERMSIG(status));
            return;
        }
        QThread::msleep(20);
    }
    kill(childPid, SIGKILL);
    // SIGKILL 之后同样要边排空边等，理由同上。轮询上限保证线程一定能结束：即使子进程真的
    // 无法回收，也不能让 requestStop() 的调用方永远等在 wait() 上。
    for (int attempt = 0; attempt < 250; ++attempt) {
        drainPty();
        if (waitpid(childPid, &status, WNOHANG) == childPid) {
            break;
        }
        QThread::msleep(20);
    }
    close(masterFd);
    emit shellFinished(ioFailed ? 1 : 137);
#endif
}

bool LocalShellWorker::shouldStop() const
{
    QMutexLocker locker(&m_mutex);
    return m_stopRequested;
}

void LocalShellWorker::notifyOutputConsumed(const qint64 bytes)
{
    if (bytes <= 0) {
        return;
    }
    m_outputInFlight.fetch_sub(bytes, std::memory_order_relaxed);
}

bool LocalShellWorker::outputBacklogFull() const
{
    return m_outputInFlight.load(std::memory_order_relaxed) >= kOutputBacklogHighWaterBytes;
}
