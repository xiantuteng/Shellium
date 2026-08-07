#pragma once

#include "core/ConnectionTarget.h"

#include <QWidget>

class QBoxLayout;
class QSplitter;
class RemoteFileManagerWidget;
class TerminalPane;

/**
 * @brief 管理一个标签页内的终端分屏树和该标签专属的远程文件管理器。
 *
 * 同方向分屏会复用父 QSplitter，交叉方向则创建新的嵌套 QSplitter。这样既能表达复杂布局，
 * 又能让 Qt 原生分隔条负责比例调整和窗口缩放。远程文件管理器属于本工作区实例，不与
 * 其他标签页共享路径、SFTP 请求、重试状态或会话凭据。
 */
class TerminalWorkspace final : public QWidget {
    Q_OBJECT

public:
    explicit TerminalWorkspace(QWidget *parent = nullptr);

    /** @brief 返回用户最后聚焦的终端面板。 */
    [[nodiscard]] TerminalPane *activePane() const;

    /**
     * @brief 在当前面板旁创建一个新分屏并立即连接目标。
     * @param orientation Qt::Horizontal 表示左右分屏，Qt::Vertical 表示上下分屏。
     * @param target 新面板要连接的服务器。
     * @return 新创建的面板；无当前面板时返回 nullptr。
     */
    TerminalPane *splitActive(Qt::Orientation orientation, const ConnectionTarget &target);

    /** @brief 关闭当前分屏；工作区至少保留一个空面板。 */
    void closeActivePane();

    /** @brief 返回工作区内所有终端面板，顺序仅用于遍历而非布局持久化。 */
    [[nodiscard]] QList<TerminalPane *> panes() const;

    /**
     * @brief 显示或隐藏当前标签页内部的远程文件管理区域。
     * @param visible true 表示显示，false 表示隐藏；不会影响其他标签页的显示状态。
     */
    void setFileManagerVisible(bool visible);

    /** @brief 返回当前标签页是否配置为显示远程文件管理区域。 */
    [[nodiscard]] bool isFileManagerVisible() const;

signals:
    /** @brief 当前分屏变化，文件管理器据此切换服务器列表。 */
    void activePaneChanged(TerminalPane *pane);

    /** @brief 当前面板的嵌套服务器链变化。 */
    void activeTargetsChanged(TerminalPane *pane);

    /** @brief 子面板产生可展示给用户的状态消息。 */
    void statusMessage(const QString &message);

private:
    /** @brief 创建面板并统一连接其激活、目标变化与状态信号。 */
    TerminalPane *createPane();

    /** @brief 递归清理只剩一个子项的冗余分隔器，保持布局树简单。 */
    void collapseRedundantSplitter(QSplitter *splitter);

    /**
     * @brief 将当前激活分屏的连接链同步给本标签页专属文件管理器。
     * @param pane 当前分屏；为空或尚未完成认证时清空可管理目标。
     * @param followInnermost 是否自动选中连接链最内层服务器。
     */
    void synchronizeFileManager(TerminalPane *pane, bool followInnermost);

    QSplitter *m_pageSplitter = nullptr;
    QWidget *m_terminalArea = nullptr;
    QBoxLayout *m_rootLayout = nullptr;
    RemoteFileManagerWidget *m_fileManager = nullptr;
    TerminalPane *m_activePane = nullptr;
};
