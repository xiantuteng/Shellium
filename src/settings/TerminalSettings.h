#pragma once

#include <QColor>
#include <QHash>
#include <QKeySequence>
#include <QObject>
#include <QString>
#include <QStringList>

class ApplicationDatabase;
class QApplication;

/**
 * @brief 终端光标的可视形态。
 *
 * 三种形态都由 TerminalView 在文本视图之上绘制，不会把光标字符写入终端文档，因此复制、
 * 搜索和导出时不会混入用于绘制光标的伪字符。
 */
enum class TerminalCursorShape {
    Block,
    Underline,
    IBeam
};

/**
 * @brief 应用主题选择。
 *
 * FollowSystem 不是一套独立配色，而是根据操作系统当前 ColorScheme 动态解析成 Light 或
 * Dark；系统外观变化后 TerminalSettingsService 会重新发出 settingsChanged。
 */
enum class ThemeMode {
    FollowSystem,
    Light,
    Dark
};

/**
 * @brief 终端配色风格。
 *
 * Default 沿用 ThemeMode 解析出的浅色/深色两套内置配色；其余选项各自是一套固定配色，
 * 不再随浅色/深色主题切换而改变，效果与主流终端模拟器的预设主题一致。
 */
enum class TerminalColorScheme {
    Default,
    SolarizedDark,
    Dracula,
    Monokai,
    Nord
};

/**
 * @brief 一套终端主题使用的关键颜色。
 *
 * 颜色集中为值对象是为了让终端、设置预览和应用级 QPalette 使用同一来源，避免多个控件
 * 分别硬编码近似但不一致的深色值。
 */
struct TerminalPalette {
    QColor background;
    QColor foreground;
    QColor cursor;
    QColor selection;
    QColor link;
};

/**
 * @brief 可持久化的外观、终端行为和快捷键设置快照。
 *
 * 该结构只包含值，不持有数据库或 QWidget，可安全复制给每个 TerminalWorkspace。所有公开
 * 设置在进入终端前都会通过 normalized() 限制范围，损坏的数据库值不能造成不可读字体、
 * 负数滚动缓冲或异常大的内存占用。
 */
struct TerminalSettings {
    /** @brief 主题选择，默认深色，与大多数终端模拟器的开箱体验一致。 */
    ThemeMode themeMode = ThemeMode::Dark;

    /** @brief 终端配色风格，默认使用 ThemeMode 对应的内置浅色/深色配色。 */
    TerminalColorScheme colorScheme = TerminalColorScheme::Default;

    /** @brief 等宽字体族；空字符串表示使用 Qt 平台默认等宽字体。 */
    QString fontFamily;

    /** @brief 字体磅值，持久化和运行时都限制在 6 到 72。 */
    int fontPointSize = 13;

    /** @brief 行高百分比，100 表示字体默认行距，范围 80 到 250。 */
    int lineHeightPercent = 100;

    /** @brief 字符绝对附加间距（像素），范围 -2.0 到 10.0。 */
    qreal letterSpacing = 0.0;

    /** @brief 光标形态。 */
    TerminalCursorShape cursorShape = TerminalCursorShape::Block;

    /** @brief 是否以固定节奏闪烁光标。 */
    bool cursorBlink = true;

    /** @brief libvterm 保留的最大滚动历史行数，范围 100 到 1,000,000。 */
    int scrollbackLines = 10000;

    /** @brief 收到终端 BEL 字节时是否播放系统提示音。 */
    bool audibleBell = true;

    /**
     * @brief 已建立会话因网络异常退出时是否自动重连。
     *
     * 只对服务器未给出退出状态（-1）的异常断线生效；正常 exit、用户主动切换和首次认证
     * 失败不会触发，避免把明确退出误判成故障。
     */
    bool automaticReconnect = true;

    /** @brief 单次异常断线周期最多重试次数，范围 0 到 10；0 等价于关闭。 */
    int reconnectMaxAttempts = 3;

    /** @brief 指数退避的首轮等待秒数，范围 1 到 60；后续按 2 倍增长并封顶 60 秒。 */
    int reconnectBaseDelaySeconds = 1;

    /**
     * @brief 可配置的全局快捷键，键为稳定动作 ID，值使用 Qt 可移植序列。
     *
     * 空序列表示用户主动取消该动作的快捷键；未知动作 ID 不会持久化，避免拼写错误产生
     * 永远无法在设置界面清理的孤立键。
     */
    QHash<QString, QKeySequence> shortcuts;

    /** @brief 返回包含全部内置动作的默认快捷键表。 */
    [[nodiscard]] static QHash<QString, QKeySequence> defaultShortcuts();

    /** @brief 返回设置界面和持久化层采用的稳定动作顺序。 */
    [[nodiscard]] static QStringList shortcutIds();

    /** @brief 返回动作 ID 对应的中文名称；未知 ID 原样返回，便于诊断。 */
    [[nodiscard]] static QString shortcutDisplayName(const QString &id);

    /**
     * @brief 把所有数值限制到安全范围，并补齐缺失的内置快捷键。
     * @return 规范化后的副本；不会修改原对象。
     */
    [[nodiscard]] TerminalSettings normalized() const;

    /**
     * @brief 验证会造成歧义、不能靠范围裁剪修复的设置。
     * @param errorMessage 失败时接收中文原因；可为空。
     * @return 快捷键没有冲突且所有动作 ID 已知时返回 true。
     */
    [[nodiscard]] bool validate(QString *errorMessage = nullptr) const;

    /**
     * @brief 按当前系统外观把 FollowSystem 解析为 Light 或 Dark。
     * @return 只可能是 Light 或 Dark。
     */
    [[nodiscard]] ThemeMode effectiveThemeMode() const;

    /** @brief 返回当前有效主题的完整终端配色。 */
    [[nodiscard]] TerminalPalette effectivePalette() const;

    /** @brief 精确比较全部设置字段，供服务避免重复发信号。 */
    [[nodiscard]] bool operator==(const TerminalSettings &other) const;
};

Q_DECLARE_METATYPE(TerminalSettings)
Q_DECLARE_METATYPE(ThemeMode)
Q_DECLARE_METATYPE(TerminalColorScheme)

/**
 * @brief 负责加载、校验、原子保存并广播终端设置的应用级服务。
 *
 * 服务必须创建和使用于 GUI 主线程，因为 ApplicationDatabase 的 Qt SQL 连接具有线程归属，
 * 同时 settingsChanged 的直接连接会立即更新 QWidget。SettingsDialog 可调用 applyTransient()
 * 进行实时预览，取消时再恢复打开对话框前的快照；save() 才真正写入 SQLite。
 */
class TerminalSettingsService final : public QObject {
    Q_OBJECT

public:
    /**
     * @brief 创建设置服务。
     * @param database 已初始化的应用数据库；可为空以支持纯内存预览，但此时 save/load 会失败。
     * @param parent Qt 父对象。
     */
    explicit TerminalSettingsService(ApplicationDatabase *database, QObject *parent = nullptr);

    /**
     * @brief 从数据库加载完整设置；缺失项使用默认值，损坏项安全回退。
     * @param errorMessage 数据库查询失败时接收原因；单项格式损坏会发 warningOccurred 而不让
     *        整个应用无法启动。
     * @return 数据库可读且设置已应用时返回 true。
     */
    bool load(QString *errorMessage = nullptr);

    /**
     * @brief 在单个 SQLite 事务中保存完整设置并立即广播。
     * @param settings 待保存快照；写入前会规范化并检查快捷键冲突。
     * @param errorMessage 校验或数据库失败时接收原因。
     * @return 全部键提交成功返回 true；失败时当前运行设置和数据库都保持不变。
     */
    bool save(const TerminalSettings &settings, QString *errorMessage = nullptr);

    /**
     * @brief 仅在内存中应用设置，用于设置对话框实时预览。
     * @param settings 待预览快照；数值会先规范化，但不会写数据库。
     */
    void applyTransient(const TerminalSettings &settings);

    /** @brief 返回当前已经规范化的设置快照。 */
    [[nodiscard]] TerminalSettings currentSettings() const { return m_currentSettings; }

    /** @brief 返回指定动作当前快捷键；未知动作返回空序列。 */
    [[nodiscard]] QKeySequence shortcut(const QString &actionId) const;

    /**
     * @brief 把主题应用到 QApplication，并在后续设置变化时自动更新。
     * @param application 生命周期必须覆盖本服务；传空指针会解除应用级主题绑定。
     *
     * 该方法独立于 MainWindow，main() 可在创建服务后直接调用；TerminalWorkspace 的局部终端
     * 配色不依赖此绑定。
     */
    void bindApplicationTheme(QApplication *application);

signals:
    /** @brief 当前运行设置发生变化，所有工作区应实时重新应用。 */
    void settingsChanged(const TerminalSettings &settings);

    /** @brief 单项持久化值损坏但已安全回退，供顶部提示区或日志展示。 */
    void warningOccurred(const QString &message);

private:
    /** @brief 按当前有效主题刷新已绑定 QApplication 的全局调色板。 */
    void applyApplicationPalette();

    ApplicationDatabase *m_database = nullptr;
    QApplication *m_boundApplication = nullptr;
    TerminalSettings m_currentSettings;
};
