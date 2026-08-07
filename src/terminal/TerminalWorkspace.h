#pragma once

#include <QWidget>

class QSplitter;
class RemoteFileManagerWidget;
class TerminalPane;

/**
 * @brief 管理一个标签页内的终端与该标签专属的远程文件管理器。
 *
 * 每个标签页承载一个终端会话；需要同时操作多台服务器时新建标签页即可。远程文件管理器
 * 属于本工作区实例，不与其他标签页共享路径、SFTP 请求、重试状态或会话凭据。
 */
class TerminalWorkspace final : public QWidget {
    Q_OBJECT

public:
    explicit TerminalWorkspace(QWidget *parent = nullptr);

    /** @brief 返回本标签页的终端面板。 */
    [[nodiscard]] TerminalPane *activePane() const;

    /**
     * @brief 显示或隐藏当前标签页内部的远程文件管理区域。
     * @param visible true 表示显示，false 表示隐藏；不会影响其他标签页的显示状态。
     */
    void setFileManagerVisible(bool visible);

    /** @brief 返回当前标签页是否配置为显示远程文件管理区域。 */
    [[nodiscard]] bool isFileManagerVisible() const;

signals:
    /** @brief 当前面板的嵌套服务器链变化。 */
    void activeTargetsChanged(TerminalPane *pane);

    /** @brief 终端面板产生可展示给用户的状态消息。 */
    void statusMessage(const QString &message);

private:
    /**
     * @brief 将终端的连接链同步给本标签页专属文件管理器。
     * @param followInnermost 是否自动选中连接链最内层服务器。
     */
    void synchronizeFileManager(bool followInnermost);

    QSplitter *m_pageSplitter = nullptr;
    RemoteFileManagerWidget *m_fileManager = nullptr;
    TerminalPane *m_pane = nullptr;
};
