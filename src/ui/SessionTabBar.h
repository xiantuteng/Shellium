#pragma once

#include <QColor>
#include <QTabBar>

class QMouseEvent;
class QPaintEvent;

/**
 * @brief 用于窗口标题区的胶囊式会话标签栏。
 *
 * Qt 原生 QTabBar 的外观更接近文档页签，并且关闭按钮会永久占用每个标签的空间。
 * 本控件保留 QTabBar 已验证的索引、拖动和滚动行为，只接管尺寸与绘制：当前标签使用
 * 高对比圆角底色，关闭按钮仅在当前或悬停标签中出现，从而得到接近现代终端客户端的
 * 紧凑标题栏体验。控件不持有任何终端对象，标签生命周期仍由 MainWindow 统一管理。
 */
class SessionTabBar final : public QTabBar {
public:
    /**
     * @brief 创建标题栏会话标签控件。
     * @param parent Qt 父控件，通常是窗口顶部的自定义标题区。
     */
    explicit SessionTabBar(QWidget *parent = nullptr);

    /**
     * @brief 更新标签栏颜色并立即重绘。
     * @param textColor 未选中标签的文字颜色。
     * @param selectedTextColor 当前标签的文字与图标颜色。
     * @param hoverColor 未选中标签悬停时的背景颜色。
     * @param selectedColor 当前标签的胶囊背景颜色。
     * @param accentColor 当前会话的连接感知强调色。
     *
     * 颜色由 MainWindow 根据深浅主题集中下发，避免该控件私自读取全局 Palette 后与始终
     * 保持深色的原生标题区产生反差。该方法只改变展示，不改变当前索引或标签顺序。
     */
    void setVisualColors(const QColor &textColor,
                         const QColor &selectedTextColor,
                         const QColor &hoverColor,
                         const QColor &selectedColor,
                         const QColor &accentColor);

protected:
    /** @brief 返回包含胶囊留白和按需关闭按钮空间的标签建议尺寸。 */
    QSize tabSizeHint(int index) const override;

    /** @brief 绘制所有可见标签、终端图标、标题和悬停关闭按钮。 */
    void paintEvent(QPaintEvent *event) override;

    /**
     * @brief 在标签栏空白区按下左键时启动原生窗口拖动。
     * @param event 当前鼠标按下事件。
     *
     * 只接管 tabAt() 返回 -1 的空白区域；标签胶囊内部仍交给 QTabBar，确保选择、拖动排序
     * 与自绘关闭按钮保持原有行为。
     */
    void mousePressEvent(QMouseEvent *event) override;

    /**
     * @brief 双击标签栏空白区时切换窗口的标准放大/恢复状态。
     * @param event 当前鼠标双击事件。
     *
     * 双击已有标签仍由 QTabBar 处理，避免标题栏窗口手势吞掉标签自身的扩展交互。
     */
    void mouseDoubleClickEvent(QMouseEvent *event) override;

    /** @brief 跟踪悬停标签，使关闭按钮和背景无需点击即可反馈。 */
    void mouseMoveEvent(QMouseEvent *event) override;

    /** @brief 鼠标离开后清除悬停态，避免关闭按钮残留。 */
    void leaveEvent(QEvent *event) override;

    /** @brief 命中自绘关闭按钮时发出 QTabBar::tabCloseRequested。 */
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    /**
     * @brief 计算指定标签内关闭按钮的点击区域。
     * @param index 标签索引；无效索引返回空矩形。
     * @return 相对于标签栏的关闭按钮矩形。
     */
    [[nodiscard]] QRect closeButtonRect(int index) const;

    /** @brief 当前鼠标悬停标签；-1 表示鼠标不位于任何标签。 */
    int m_hoveredIndex = -1;
    /** @brief 未选中标签文字色。 */
    QColor m_textColor = QColor(QStringLiteral("#aeb6cc"));
    /** @brief 当前标签文字色。 */
    QColor m_selectedTextColor = QColor(QStringLiteral("#f8f9fd"));
    /** @brief 普通标签悬停背景色。 */
    QColor m_hoverColor = QColor(QStringLiteral("#353c56"));
    /** @brief 当前标签胶囊背景色。 */
    QColor m_selectedColor = QColor(QStringLiteral("#424b68"));
    /** @brief 当前标签终端图标与微型状态点的强调色。 */
    QColor m_accentColor = QColor(QStringLiteral("#2bd981"));
};
