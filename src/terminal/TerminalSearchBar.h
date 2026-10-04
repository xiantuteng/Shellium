#pragma once

#include "terminal/TerminalSearch.h"

#include <QColor>
#include <QWidget>

class QLabel;
class QLineEdit;
class QToolButton;

/**
 * @brief 嵌在终端会话顶部的查找栏。
 *
 * 取代原先的模态输入框：模态框挡住终端正文、每查一次都要重新弹出，也无法在命中之间
 * 来回跳。查找栏常驻在会话状态栏下方，边输入边查找，用上一个 / 下一个在命中之间移动，
 * 并显示"第几个 / 共几个"。本控件只负责交互与展示，实际查找由所属 TerminalPane 转交
 * TerminalView 完成，因此不持有任何终端对象。
 *
 * 键位：Enter 或 ↑ 向上找更早的输出，Shift+Enter 或 ↓ 向下找更新的输出，Esc 关闭。
 * 终端越靠下越新，往回翻才是查找输出时最常见的方向，因此 Enter 对应"上一个"。
 */
class TerminalSearchBar final : public QWidget {
    Q_OBJECT

public:
    /** @brief 创建默认隐藏的查找栏。 */
    explicit TerminalSearchBar(QWidget *parent = nullptr);

    /**
     * @brief 显示查找栏并把焦点交给输入框。
     * @param initialText 非空时替换输入框内容（例如终端中选中的文本）；为空时保留上次内容。
     *
     * 输入框内容会被全选，用户可直接键入覆盖；内容非空时立即按 Refine 查找一次，使命中
     * 与计数和输入框保持一致。
     */
    void activate(const QString &initialText = {});

    /** @brief 当前输入框中的查找文本。 */
    [[nodiscard]] QString text() const;

    /**
     * @brief 展示一次查找结果。
     * @param result 当前序号与总数；总数为 0 表示没有命中。
     */
    void setResult(const TerminalSearchResult &result);

    /**
     * @brief 按终端主题更新配色。
     * @param background 查找栏底色，与会话状态栏一致。
     * @param foreground 文字与图标颜色。
     * @param inputBackground 输入框底色，通常取终端正文背景。
     */
    void setColors(const QColor &background,
                   const QColor &foreground,
                   const QColor &inputBackground);

signals:
    /**
     * @brief 请求执行一步查找。
     * @param text 查找文本；为空表示清除命中。
     * @param step 本次查找的方向，见 TerminalSearchStep。
     */
    void searchRequested(const QString &text, TerminalSearchStep step);

    /** @brief 用户关闭了查找栏（Esc 或关闭按钮），所属面板应清除命中并把焦点还给终端。 */
    void closeRequested();

protected:
    /** @brief 拦截输入框中的 Enter / Shift+Enter / Esc 与平台"查找下一个"快捷键。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /** @brief 按当前结果和语言刷新计数文字、按钮可用性与"无结果"提示样式。 */
    void updateResultPresentation();

    /** @brief 重新生成样式表与图标；颜色或"无结果"状态变化时调用。 */
    void applyStyle();

    QLineEdit *m_input = nullptr;
    QLabel *m_resultLabel = nullptr;
    QToolButton *m_previousButton = nullptr;
    QToolButton *m_nextButton = nullptr;
    QToolButton *m_closeButton = nullptr;

    /** @brief 最近一次查找结果；语言切换后据此重新生成计数文字。 */
    TerminalSearchResult m_result;
    /** @brief 当前是否处于"有输入却没有命中"的状态，输入框据此显示警示边框。 */
    bool m_notFound = false;

    QColor m_background = QColor(QStringLiteral("#2b3044"));
    QColor m_foreground = QColor(QStringLiteral("#d8dce8"));
    QColor m_inputBackground = QColor(QStringLiteral("#1e2130"));
};
