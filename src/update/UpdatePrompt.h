#pragma once

#include "update/UpdateChecker.h"

#include <QCoreApplication>

class QMessageBox;
class QWidget;
class TerminalSettingsService;

/**
 * @brief 发现新版本后的提示框：前往下载、跳过此版本、稍后提醒。
 *
 * 提示框以 open() 非阻塞弹出并在关闭时自删，后台定时检查命中时不会打断正在进行的
 * 终端会话的事件循环。“跳过此版本”把版本号写进 TerminalSettings::skippedUpdateVersion，
 * 之后的自动检查对这个版本保持安静，更高的版本发布时照常提示。
 */
class UpdatePrompt final {
    Q_DECLARE_TR_FUNCTIONS(UpdatePrompt)

public:
    /**
     * @brief 弹出提示框。
     * @param parent 父窗口。
     * @param release 新版本信息。
     * @param currentVersion 当前运行的版本，显示在正文里。
     * @param settingsService 用于保存跳过的版本；为空时不提供“跳过此版本”按钮。
     * @return 已打开的提示框，供调用方去重或测试点击按钮。
     */
    static QMessageBox *show(QWidget *parent,
                             const UpdateRelease &release,
                             const QString &currentVersion,
                             TerminalSettingsService *settingsService);

    /**
     * @brief 把 version 记为跳过的版本并持久化。
     * @return 保存是否成功；失败原因写入 errorMessage。
     */
    static bool skipVersion(TerminalSettingsService *settingsService,
                            const QString &version,
                            QString *errorMessage = nullptr);
};
