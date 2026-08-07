#include "terminal/LocalShellWorker.h"

#include <QFile>
#include <QMutexLocker>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
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

LocalShellWorker::LocalShellWorker(QString shellPath, QObject *parent)
    : QThread(parent)
    , m_shellPath(std::move(shellPath))
{
}

LocalShellWorker::~LocalShellWorker()
{
    requestStop();
    // PTY 循环每 20ms 检查停止状态，正常情况下很快退出；保留 3 秒给 zsh 执行退出钩子。
    wait(3000);
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

void LocalShellWorker::run()
{
    if (!QFile::exists(m_shellPath)) {
        emit shellError(tr("本地 Shell 不存在：%1").arg(m_shellPath));
        return;
    }

    struct winsize initialSize {};
    {
        QMutexLocker locker(&m_mutex);
        initialSize.ws_col = static_cast<unsigned short>(m_columns);
        initialSize.ws_row = static_cast<unsigned short>(m_rows);
        m_sizeDirty = false;
    }

    int masterFd = -1;
    const pid_t childPid = forkpty(&masterFd, nullptr, nullptr, &initialSize);
    if (childPid < 0) {
        emit shellError(tr("无法创建本地 PTY：%1").arg(QString::fromLocal8Bit(std::strerror(errno))));
        return;
    }

    if (childPid == 0) {
        // fork 后只执行异步信号安全的 exec。通过 env 程序设置终端变量，避免在 Qt 多线程进程的
        // child 分支调用 setenv 等可能持有 libc 锁的函数。
        const QByteArray encodedShell = QFile::encodeName(m_shellPath);
        execl("/usr/bin/env",
              "env",
              "TERM=xterm-256color",
              "COLORTERM=truecolor",
              encodedShell.constData(),
              "-l",
              static_cast<char *>(nullptr));
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
                emit shellError(tr("向本地 PTY 写入失败：%1")
                                    .arg(QString::fromLocal8Bit(std::strerror(errno))));
                break;
            }
        }
        if (ioFailed) {
            break;
        }

        struct pollfd descriptor {masterFd, POLLIN | POLLHUP | POLLERR, 0};
        const int pollResult = poll(&descriptor, 1, 20);
        if (pollResult < 0 && errno != EINTR) {
            emit shellError(tr("轮询本地 PTY 失败：%1")
                                .arg(QString::fromLocal8Bit(std::strerror(errno))));
            ioFailed = true;
            break;
        }
        if (pollResult > 0 && (descriptor.revents & POLLIN) != 0) {
            while (true) {
                const ssize_t bytesRead = ::read(masterFd, readBuffer.data(), readBuffer.size());
                if (bytesRead > 0) {
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

    // 用户关闭终端时先发送 SIGHUP，符合真实终端关闭语义；超时后才使用 SIGKILL，防止
    // 后台任务永久占用进程。所有 waitpid 调用都在本工作线程，避免跨线程竞争回收子进程。
    kill(childPid, SIGHUP);
    int status = 0;
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (waitpid(childPid, &status, WNOHANG) == childPid) {
            close(masterFd);
            emit shellFinished(WIFEXITED(status) ? WEXITSTATUS(status)
                                                 : 128 + WTERMSIG(status));
            return;
        }
        QThread::msleep(20);
    }
    kill(childPid, SIGKILL);
    waitpid(childPid, &status, 0);
    close(masterFd);
    emit shellFinished(ioFailed ? 1 : 137);
}

bool LocalShellWorker::shouldStop() const
{
    QMutexLocker locker(&m_mutex);
    return m_stopRequested;
}
