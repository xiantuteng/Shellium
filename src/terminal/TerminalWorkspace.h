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

    /**
     * @brief 用户在终端的断线恢复提示中选择了关闭本标签页。
     * @param workspace 发出请求的工作区；主窗口据此定位标签索引。
     */
    void closeRequested(TerminalWorkspace *workspace);

    /**
     * @brief 当前标签页的结构化终端状态发生变化。
     * @param server 当前服务器连接说明；本地终端和未连接会使用明确中文标识。
     * @param encoding 当前终端输入输出编码，现阶段固定为 UTF-8。
     * @param columns 终端可见列数。
     * @param rows 终端可见行数。
     *
     * 该结构化信号保留给自动化、无障碍或未来的会话概览使用，消费者不能从短暂的
     * statusMessage 文本反向解析。当前界面由 TerminalPane 直接绘制顶部会话状态栏，
     * 每个 TerminalWorkspace 仍保存自己的尺寸，因此标签切换不会串用其他会话状态。
     */
    void terminalStatusChanged(const QString &server,
                               const QString &encoding,
                               int columns,
                               int rows);

private:
    /**
     * @brief 将终端的连接链同步给本标签页专属文件管理器。
     * @param followInnermost 是否自动选中连接链最内层服务器。
     */
    void synchronizeFileManager(bool followInnermost);

    /** @brief 根据当前连接层和最近字符尺寸发出结构化终端状态，供非视觉消费者复用。 */
    void emitTerminalStatus();

    /**
     * @brief 处理终端视图拖入的本地文件：确认后上传到当前活动层的当前目录。
     * @param localPaths 已由 TerminalView 过滤为本地文件/目录的绝对路径列表。
     *
     * “当前活动层”严格取自 TerminalPane::activeTarget()/activeWorkingDirectory()，因此
     * 嵌套第 2 层、第 N 层拖入时会落到该层自己的目录，而不是根连接的目录；未连接、本地
     * 终端或尚未获取到 cwd 时只提示，不弹出确认框。
     */
    void handleFilesDropped(const QStringList &localPaths);

    QSplitter *m_pageSplitter = nullptr;
    RemoteFileManagerWidget *m_fileManager = nullptr;
    TerminalPane *m_pane = nullptr;

    /** @brief 本标签最近一次终端列数，窗口尚未布局时使用标准 80 列。 */
    int m_terminalColumns = 80;

    /** @brief 本标签最近一次终端行数，窗口尚未布局时使用标准 24 行。 */
    int m_terminalRows = 24;
};
