#pragma once

#include <QColor>
#include <QHash>
#include <QList>
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
 * @brief 一套内置终端配色预设。
 *
 * 配色数量已达数十套，用枚举逐一列举会让序列化、界面构建和新增主题三处同时膨胀，因此改用
 * 数据表：id 是持久化用的稳定标识（不受显示名称与本地化影响），name 是界面显示名，dark 只
 * 用于界面分组和对比度判断。唯一的特例是 id "default"，它没有固定颜色，跟随 ThemeMode
 * 解析出的浅色/深色内置配色。
 */
struct TerminalThemePreset {
    QString id;
    QString name;
    bool dark = true;
    TerminalPalette palette;
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

    /**
     * @brief 终端配色预设 id，默认 "default" 表示跟随 ThemeMode 的内置浅色/深色配色。
     *
     * 未知 id（旧版本、手工改库或插件写入）会在 normalized() 中回退为 "default"，不会让终端
     * 拿到一套没有颜色的空调色板。
     */
    QString colorSchemeId = QStringLiteral("default");

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
     * @brief 是否根据当前主机的历史命令自动弹出输入建议，默认关闭。
     *
     * 该开关只控制终端输入时的自动弹层，不影响命令历史的记录、命令面板或远端 Shell 自带
     * 的 Tab 补全。默认关闭可以避免升级后突然出现浮层并改变既有键盘操作习惯；用户明确
     * 开启后，全部已打开和新建终端会通过设置广播同步生效。
     */
    bool commandAutoCompletion = false;

    /**
     * @brief 已建立会话因网络异常退出时是否自动重连。
     *
     * 只对服务器未给出退出状态（-1）的异常断线生效；正常 exit、用户主动切换和首次认证
     * 失败不会触发，避免把明确退出误判成故障。
     */
    bool automaticReconnect = true;

    /**
     * @brief 是否使用用户主密码保护自动保存的主机凭据，默认关闭。
     *
     * 关闭时凭据仍会使用本机随机密钥加密并自动用于连接，但界面禁止显示明文；开启后改用
     * 用户主密码派生密钥保存，且只有成功解锁后才允许在主机编辑页查看密码。该开关不影响
     * 私钥文件路径等普通主机配置的持久化。
     */
    bool vaultPasswordEnabled = false;

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

    /**
     * @brief 返回全部内置配色预设，顺序即设置界面的展示顺序。
     *
     * 第一项固定是 id 为 "default" 的跟随主题项；其余按“深浅成对、系列相邻”排列，方便用户在
     * 长列表里对比同一系列的明暗版本。
     */
    [[nodiscard]] static const QList<TerminalThemePreset> &themePresets();

    /**
     * @brief 查找配色预设。
     * @param id 预设 id。
     * @return 命中的预设指针；未知 id 返回 nullptr，返回值指向静态表，可长期持有。
     */
    [[nodiscard]] static const TerminalThemePreset *themePreset(const QString &id);

    /**
     * @brief 返回推荐的等宽/编程字体族名列表，顺序即推荐优先级。
     *
     * 列表是纯建议数据，不代表本机一定安装；设置界面负责与 QFontDatabase 求交集，只展示真实
     * 可用的字体，避免用户选到一个会被静默替换成其它字体的名字。
     */
    [[nodiscard]] static QStringList recommendedFontFamilies();

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
