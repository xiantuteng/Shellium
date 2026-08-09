#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QFrame>
#include <QList>
#include <QString>
#include <QWidget>

class QAbstractSpinBox;
class QBoxLayout;
class QButtonGroup;
class QLabel;
class QVBoxLayout;

/**
 * @brief 设置界面共用的现代化表单控件族。
 *
 * 这些控件只负责呈现与交互，不读写数据库，也不感知 TerminalSettings 的字段含义，因此设置
 * 对话框、连接对话框和后续新增的偏好页可以复用同一套视觉语言：分组卡片承载“左侧说明 +
 * 右侧控件”的行，布尔项用开关而不是复选框，数值项用带加减按钮的步进器，互斥的少量选项用
 * 分段控件，终端配色则用可直接看到底色的色板卡。
 *
 * 颜色统一从控件当前 QPalette 与共享强调色推导，不在各控件内部硬编码灰阶，保证深浅主题
 * 切换后所有控件同步变化。
 */
namespace ShelliumUi {

/** @brief 返回全应用统一的强调色，与 MainWindow 全局样式表使用同一常量。 */
[[nodiscard]] QColor accentColor();

} // namespace ShelliumUi

/**
 * @brief iOS/Termius 风格的滑动开关，用于替代设置表单里的复选框。
 *
 * 继承 QAbstractButton 而不是 QCheckBox：仍然提供 isChecked()/setChecked()/toggled()，调用方
 * 无需改写业务代码，但整块控件自绘，不受平台复选框指示器尺寸影响。文字不画在开关上，说明
 * 文本由所在卡片的行标签承担，避免出现“文字 + 开关”两处对齐基准。
 */
class ToggleSwitch final : public QAbstractButton {
    Q_OBJECT
    Q_PROPERTY(qreal knobPosition READ knobPosition WRITE setKnobPosition)

public:
    /**
     * @brief 创建一个默认关闭的开关。
     * @param parent Qt 父控件。
     */
    explicit ToggleSwitch(QWidget *parent = nullptr);

    /** @brief 返回开关固定尺寸，保证同一卡片内多行开关右边缘严格对齐。 */
    [[nodiscard]] QSize sizeHint() const override;

    /** @brief 滑块当前位置，0 表示最左（关），1 表示最右（开）；仅供动画使用。 */
    [[nodiscard]] qreal knobPosition() const { return m_knobPosition; }

    /** @brief 设置滑块位置并重绘；动画之外的调用会立即生效。 */
    void setKnobPosition(qreal position);

protected:
    void paintEvent(QPaintEvent *event) override;
    void checkStateSet() override;
    void nextCheckState() override;

private:
    /** @brief 把滑块动画到与当前选中态一致的位置。 */
    void animateToCurrentState();

    qreal m_knobPosition = 0.0;
};

/**
 * @brief 把任意 QAbstractSpinBox 包装成“减号 / 数值 / 加号”三段式步进器。
 *
 * 采用包装而不是自绘数值输入，是为了保留 QSpinBox 既有的范围裁剪、后缀、键盘输入与
 * 可访问性；调用方仍持有原始 spin box 指针读写数值，对象名也保持不变，因此已有逻辑与
 * 测试无需适配。原生上下箭头会被隐藏，避免出现两套增减入口。
 */
class SettingsStepper final : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief 用给定 spin box 构造步进器。
     * @param spinBox 被包装的数值控件，不可为空；构造后由本控件重新父子化并布局。
     * @param parent Qt 父控件。
     */
    explicit SettingsStepper(QAbstractSpinBox *spinBox, QWidget *parent = nullptr);

private:
    QAbstractSpinBox *m_spinBox = nullptr;
};

/**
 * @brief 少量互斥选项的分段控件，用于主题模式、光标形态一类“一眼可见全部取值”的设置。
 *
 * 相比下拉框，分段控件把所有取值同时暴露在同一行，减少一次点击并直接体现取值个数。每个
 * 分段携带一个整数 data，与设置结构里的枚举一一对应，界面显示文本变化不会影响持久化。
 */
class SegmentedControl final : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief 创建空的分段控件。
     * @param parent Qt 父控件。
     */
    explicit SegmentedControl(QWidget *parent = nullptr);

    /**
     * @brief 追加一个分段。
     * @param text 显示文本。
     * @param data 与该分段绑定的枚举整数值，必须在同一控件内唯一。
     */
    void addSegment(const QString &text, int data);

    /** @brief 返回当前选中分段的 data；没有任何分段时返回 -1。 */
    [[nodiscard]] int currentData() const;

    /**
     * @brief 选中携带指定 data 的分段。
     * @param data 目标枚举整数值；不存在时保持当前选择不变。
     */
    void setCurrentData(int data);

signals:
    /** @brief 用户或代码切换了选中分段，参数为新选中分段的 data。 */
    void currentDataChanged(int data);

private:
    QButtonGroup *m_group = nullptr;
    QBoxLayout *m_layout = nullptr;
};

/**
 * @brief 终端配色色板卡：直接用该配色的真实颜色绘制一小段“终端画面”。
 *
 * 配色名称对用户几乎没有信息量，能立刻分辨的是底色与前景对比。这里用配色自身的背景、
 * 前景、光标和链接色画三行示意文本，选中时以强调色描边，因此不依赖任何外部图片资源。
 */
class ThemeSwatchButton final : public QAbstractButton {
    Q_OBJECT

public:
    /**
     * @brief 创建一张配色卡。
     * @param title 配色显示名称。
     * @param subtitle 次要说明，可为空。
     * @param background 配色背景色。
     * @param foreground 配色前景色。
     * @param cursor 配色光标色。
     * @param link 配色链接色。
     * @param parent Qt 父控件。
     */
    ThemeSwatchButton(const QString &title,
                      const QString &subtitle,
                      const QColor &background,
                      const QColor &foreground,
                      const QColor &cursor,
                      const QColor &link,
                      QWidget *parent = nullptr);

    [[nodiscard]] QSize sizeHint() const override;

    /**
     * @brief 更新卡片展示的配色，供“默认（跟随主题）”在主题切换后同步预览。
     */
    void setPreviewColors(const QColor &background,
                          const QColor &foreground,
                          const QColor &cursor,
                          const QColor &link);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_subtitle;
    QColor m_background;
    QColor m_foreground;
    QColor m_cursor;
    QColor m_link;
};

/**
 * @brief 设置页里的分组卡片，承载“左侧说明 + 右侧控件”的行。
 *
 * 卡片本身只做视觉分组与行布局；它不持有任何设置值，行内控件的所有权仍属于调用方传入的
 * 父子关系。addRow() 会把说明文本与控件放在同一水平基线上，并让控件靠右，从而使一页内所有
 * 卡片的控件列自然对齐。
 */
class SettingsCard final : public QFrame {
    Q_OBJECT

public:
    /**
     * @brief 创建卡片。
     * @param title 卡片标题；为空时不显示标题行。
     * @param parent Qt 父控件。
     */
    explicit SettingsCard(const QString &title = QString(), QWidget *parent = nullptr);

    /**
     * @brief 追加一行“说明 + 控件”。
     * @param label 行标签文本。
     * @param field 行内控件，不可为空；会被重新父子化到卡片。
     * @param hint 标签下方的次要说明，可为空。
     * @return 传入的控件，便于链式设置属性。
     */
    QWidget *addRow(const QString &label, QWidget *field, const QString &hint = QString());

    /**
     * @brief 追加一个占满整行宽度的控件（色板网格、预览区、长段说明等）。
     * @param widget 目标控件，不可为空；会被重新父子化到卡片。
     */
    void addFullWidthWidget(QWidget *widget);

    /**
     * @brief 追加一段灰色说明文字，用于解释某个设置的边界条件。
     * @param text 说明内容，自动换行。
     * @return 新建的标签，便于调用方后续更新文本。
     */
    QLabel *addNote(const QString &text);

private:
    QVBoxLayout *m_layout = nullptr;
};
