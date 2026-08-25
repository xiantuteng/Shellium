#include "platform/SecureFileWriter.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <sddl.h>
#endif

namespace {

#if defined(Q_OS_WIN)
/** @brief 将 Win32 错误码转换成稳定诊断，并释放 FormatMessage 分配的缓冲。 */
QString windowsFileErrorText(const DWORD errorCode)
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

/**
 * @brief 转换为 CreateFileW 可接受的扩展长度绝对路径。
 *
 * 即使 manifest 声明 longPathAware，企业策略也可能尚未启用；显式使用扩展路径前缀可让中文
 * 用户目录和超过 MAX_PATH 的密钥目录保持一致。UNC 路径需要采用专用 UNC 扩展形式。
 */
QString extendedWindowsPath(const QString &path)
{
    QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (native.startsWith(QStringLiteral("\\\\?\\"))) {
        return native;
    }
    if (native.startsWith(QStringLiteral("\\\\"))) {
        return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    }
    return QStringLiteral("\\\\?\\") + native;
}
#endif

} // namespace

bool writeNewFileSecurely(const QString &path,
                          const QByteArray &content,
                          const QFileDevice::Permissions permissions,
                          QString *errorMessage)
{
    if (QFileInfo::exists(path)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("目标文件已存在，拒绝覆盖：%1").arg(path);
        }
        return false;
    }

#if defined(Q_OS_WIN)
    const bool ownerOnly = (permissions & (QFileDevice::ReadGroup
                                            | QFileDevice::WriteGroup
                                            | QFileDevice::ExeGroup
                                            | QFileDevice::ReadOther
                                            | QFileDevice::WriteOther
                                            | QFileDevice::ExeOther)) == 0;
    if (ownerOnly) {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        // D:P 禁止从父目录继承宽权限；OW、SYSTEM 和管理员保留完全控制，既保护私钥又允许
        // 系统备份/恢复。安全描述符随 CREATE_NEW 原子应用，不存在先公开后收紧的竞态窗口。
        constexpr wchar_t securitySddl[] = L"D:P(A;;FA;;;OW)(A;;FA;;;SY)(A;;FA;;;BA)";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                securitySddl, SDDL_REVISION_1, &descriptor, nullptr)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("无法创建私钥安全描述符：%1")
                                    .arg(windowsFileErrorText(GetLastError()));
            }
            return false;
        }
        SECURITY_ATTRIBUTES security {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        const QString nativePath = extendedWindowsPath(path);
        HANDLE file = CreateFileW(reinterpret_cast<const wchar_t *>(nativePath.utf16()),
                                  GENERIC_WRITE,
                                  0,
                                  &security,
                                  CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                  nullptr);
        LocalFree(descriptor);
        if (file == INVALID_HANDLE_VALUE) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("无法安全创建文件 %1：%2")
                                    .arg(path, windowsFileErrorText(GetLastError()));
            }
            return false;
        }

        qsizetype offset = 0;
        bool success = true;
        while (offset < content.size()) {
            DWORD written = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<qsizetype>(
                content.size() - offset, static_cast<qsizetype>(MAXDWORD)));
            if (!WriteFile(file, content.constData() + offset, chunk, &written, nullptr)
                || written == 0) {
                success = false;
                break;
            }
            offset += static_cast<qsizetype>(written);
        }
        if (success) {
            success = FlushFileBuffers(file) != FALSE;
        }
        const DWORD failureCode = success ? ERROR_SUCCESS : GetLastError();
        CloseHandle(file);
        if (!success) {
            // 目标原本不存在，因此删除本次未完整写入的文件是安全回滚，不会触碰用户数据。
            DeleteFileW(reinterpret_cast<const wchar_t *>(nativePath.utf16()));
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("写入安全文件 %1 失败：%2")
                                    .arg(path, windowsFileErrorText(failureCode));
            }
        }
        return success;
    }
#endif

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("无法创建文件 %1：%2").arg(path, file.errorString());
        }
        return false;
    }
    if (!file.setPermissions(permissions) || file.write(content) != content.size()) {
        file.cancelWriting();
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("无法安全写入文件 %1：%2")
                                .arg(path, file.errorString());
        }
        return false;
    }
    if (!file.commit()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("原子提交文件失败 %1：%2").arg(path, file.errorString());
        }
        return false;
    }
    return true;
}
