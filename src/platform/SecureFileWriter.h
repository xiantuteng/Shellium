#pragma once

#include <QByteArray>
#include <QFileDevice>
#include <QString>

/**
 * @brief 以当前平台真正可执行的安全权限创建一个此前不存在的文件。
 * @param path 目标 Unicode 路径；已存在时拒绝覆盖。
 * @param content 要完整写入的字节。
 * @param permissions POSIX 权限；Windows 仅区分“仅所有者”与普通公开文件。
 * @param errorMessage 失败时接收可展示的中文诊断，可为空。
 * @return 文件完整落盘返回 true。
 *
 * 私钥不能先按父目录默认 ACL 创建、再事后收紧，否则另一个本机用户可能在窗口期读到内容。
 * Windows 因此在 CreateFileW 时直接附带受保护 DACL；POSIX 则借助 QSaveFile 的原子提交
 * 与 mode 权限。该接口只创建新文件，不承担覆盖用户现有数据的职责。
 */
[[nodiscard]] bool writeNewFileSecurely(const QString &path,
                                        const QByteArray &content,
                                        QFileDevice::Permissions permissions,
                                        QString *errorMessage = nullptr);
