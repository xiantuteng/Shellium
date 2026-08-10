#include "settings/TerminalSettings.h"

#include "storage/ApplicationDatabase.h"

#include <QApplication>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>
#include <QThread>

#include <algorithm>
#include <cmath>

namespace {

/** @brief 设置表中键名前缀，集中定义可避免保存和加载使用不同拼写。 */
const QString kThemeKey = QStringLiteral("appearance/theme");
const QString kColorSchemeKey = QStringLiteral("appearance/color_scheme");
const QString kFontFamilyKey = QStringLiteral("terminal/font_family");
const QString kFontSizeKey = QStringLiteral("terminal/font_size");
const QString kLineHeightKey = QStringLiteral("terminal/line_height_percent");
const QString kLetterSpacingKey = QStringLiteral("terminal/letter_spacing");
const QString kCursorShapeKey = QStringLiteral("terminal/cursor_shape");
const QString kCursorBlinkKey = QStringLiteral("terminal/cursor_blink");
const QString kScrollbackKey = QStringLiteral("terminal/scrollback_lines");
const QString kAudibleBellKey = QStringLiteral("terminal/audible_bell");
/** @brief 命令行历史自动提示开关；缺失该键时由结构默认值保持关闭。 */
const QString kCommandAutoCompletionKey = QStringLiteral("terminal/command_auto_completion");
const QString kAutomaticReconnectKey = QStringLiteral("connection/automatic_reconnect");
const QString kReconnectMaxAttemptsKey = QStringLiteral("connection/reconnect_max_attempts");
const QString kReconnectBaseDelayKey = QStringLiteral("connection/reconnect_base_delay_seconds");
const QString kShortcutPrefix = QStringLiteral("shortcut/");

/** @brief 把主题枚举序列化为不受本地化影响的稳定字符串。 */
QString serializeTheme(const ThemeMode mode)
{
    switch (mode) {
    case ThemeMode::Light:
        return QStringLiteral("light");
    case ThemeMode::Dark:
        return QStringLiteral("dark");
    case ThemeMode::FollowSystem:
    default:
        return QStringLiteral("system");
    }
}

/** @brief 解析主题字符串；未知值返回空，调用方据此回退默认值并报告警告。 */
std::optional<ThemeMode> parseTheme(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QLatin1String("system")) {
        return ThemeMode::FollowSystem;
    }
    if (normalized == QLatin1String("light")) {
        return ThemeMode::Light;
    }
    if (normalized == QLatin1String("dark")) {
        return ThemeMode::Dark;
    }
    return std::nullopt;
}

/** @brief 把光标形态序列化为稳定字符串。 */
QString serializeCursorShape(const TerminalCursorShape shape)
{
    switch (shape) {
    case TerminalCursorShape::Underline:
        return QStringLiteral("underline");
    case TerminalCursorShape::IBeam:
        return QStringLiteral("ibeam");
    case TerminalCursorShape::Block:
    default:
        return QStringLiteral("block");
    }
}

/** @brief 解析光标形态；未知值返回空以触发安全回退。 */
std::optional<TerminalCursorShape> parseCursorShape(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QLatin1String("block")) {
        return TerminalCursorShape::Block;
    }
    if (normalized == QLatin1String("underline")) {
        return TerminalCursorShape::Underline;
    }
    if (normalized == QLatin1String("ibeam")) {
        return TerminalCursorShape::IBeam;
    }
    return std::nullopt;
}

/** @brief 解析明确的布尔文本；拒绝将任意非空文本静默当成 true。 */
std::optional<bool> parseBool(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QLatin1String("1") || normalized == QLatin1String("true")) {
        return true;
    }
    if (normalized == QLatin1String("0") || normalized == QLatin1String("false")) {
        return false;
    }
    return std::nullopt;
}

/** @brief 保存布尔值时使用数据库类型化接口相同的 0/1 形式。 */
QString serializeBool(const bool value)
{
    return value ? QStringLiteral("1") : QStringLiteral("0");
}

/**
 * @brief 构造一套覆盖常见 QWidget 角色的应用调色板。
 * @param dark 是否为深色主题；文字、菜单与禁用态据此选择对比度。
 */
// 应用级配色只由界面外观（浅色/深色）决定：终端配色仅作用于终端网格，若把它掺进 QPalette，
// 换一个终端主题就会顺带改掉设置界面、配色卡名称等所有控件的文字色，导致文字看不清。
QPalette createApplicationPalette(const bool dark)
{
    QPalette palette;
    const QColor window = dark ? QColor(QStringLiteral("#20232a"))
                               : QColor(QStringLiteral("#f2f4f7"));
    const QColor base = dark ? QColor(QStringLiteral("#17191f"))
                             : QColor(QStringLiteral("#ffffff"));
    const QColor alternate = dark ? QColor(QStringLiteral("#272b34"))
                                  : QColor(QStringLiteral("#e9edf3"));
    const QColor disabled = dark ? QColor(QStringLiteral("#858b98"))
                                 : QColor(QStringLiteral("#7a808a"));
    const QColor text = dark ? QColor(QStringLiteral("#e6e9ef"))
                             : QColor(QStringLiteral("#1c1f26"));
    const QColor highlight = dark ? QColor(QStringLiteral("#33507a"))
                                  : QColor(QStringLiteral("#cfe0f7"));
    const QColor link = dark ? QColor(QStringLiteral("#6fb4ff"))
                             : QColor(QStringLiteral("#1a6dd6"));

    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, alternate);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, window);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::ToolTipBase, base);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Highlight, highlight);
    palette.setColor(QPalette::HighlightedText,
                     dark ? QColor(Qt::white) : QColor(QStringLiteral("#111318")));
    palette.setColor(QPalette::Link, link);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    return palette;
}

} // namespace

QHash<QString, QKeySequence> TerminalSettings::defaultShortcuts()
{
    return {
        {QStringLiteral("connection.new"), QKeySequence(QStringLiteral("Ctrl+Shift+N"))},
        {QStringLiteral("terminal.local"), QKeySequence(QStringLiteral("Ctrl+Shift+L"))},
        {QStringLiteral("tab.new"), QKeySequence(QStringLiteral("Ctrl+T"))},
        {QStringLiteral("tab.close"), QKeySequence(QStringLiteral("Ctrl+W"))},
        {QStringLiteral("files.toggle"), QKeySequence(QStringLiteral("Ctrl+Shift+F"))},
        {QStringLiteral("vault.lock"), QKeySequence(QStringLiteral("Ctrl+Shift+K"))},
        {QStringLiteral("terminal.search"), QKeySequence::Find},
        {QStringLiteral("terminal.export"), QKeySequence(QStringLiteral("Ctrl+Shift+E"))},
        {QStringLiteral("command.palette"), QKeySequence(QStringLiteral("Ctrl+Shift+P"))},
        {QStringLiteral("settings.open"), QKeySequence(QStringLiteral("Ctrl+,"))},
    };
}

QStringList TerminalSettings::shortcutIds()
{
    // 固定顺序让设置对话框和持久化测试在不同 Qt/QHash 版本下保持一致。
    return {
        QStringLiteral("connection.new"),
        QStringLiteral("terminal.local"),
        QStringLiteral("tab.new"),
        QStringLiteral("tab.close"),
        QStringLiteral("files.toggle"),
        QStringLiteral("vault.lock"),
        QStringLiteral("terminal.search"),
        QStringLiteral("terminal.export"),
        QStringLiteral("command.palette"),
        QStringLiteral("settings.open"),
    };
}

QString TerminalSettings::shortcutDisplayName(const QString &id)
{
    static const QHash<QString, QString> names = {
        {QStringLiteral("connection.new"), QObject::tr("连接服务器")},
        {QStringLiteral("terminal.local"), QObject::tr("打开本地终端")},
        {QStringLiteral("tab.new"), QObject::tr("新建标签页")},
        {QStringLiteral("tab.close"), QObject::tr("关闭标签页")},
        {QStringLiteral("files.toggle"), QObject::tr("显示/隐藏远程文件")},
        {QStringLiteral("vault.lock"), QObject::tr("锁定凭据保险库")},
        {QStringLiteral("terminal.search"), QObject::tr("搜索终端输出")},
        {QStringLiteral("terminal.export"), QObject::tr("导出终端输出")},
        {QStringLiteral("command.palette"), QObject::tr("打开命令面板")},
        {QStringLiteral("settings.open"), QObject::tr("打开设置")},
    };
    return names.value(id, id);
}

TerminalSettings TerminalSettings::normalized() const
{
    TerminalSettings result = *this;
    result.fontFamily = result.fontFamily.trimmed();
    // 未知配色 id 一律回退默认，避免旧版本、手工改库或插件写入的值让终端拿到空调色板。
    result.colorSchemeId = result.colorSchemeId.trimmed();
    if (themePreset(result.colorSchemeId) == nullptr) {
        result.colorSchemeId = QStringLiteral("default");
    }
    result.fontPointSize = std::clamp(result.fontPointSize, 6, 72);
    result.lineHeightPercent = std::clamp(result.lineHeightPercent, 80, 250);
    result.letterSpacing = std::clamp(result.letterSpacing, -2.0, 10.0);
    result.scrollbackLines = std::clamp(result.scrollbackLines, 100, 1000000);
    result.reconnectMaxAttempts = std::clamp(result.reconnectMaxAttempts, 0, 10);
    result.reconnectBaseDelaySeconds = std::clamp(result.reconnectBaseDelaySeconds, 1, 60);

    const QHash<QString, QKeySequence> defaults = defaultShortcuts();
    for (const QString &id : shortcutIds()) {
        if (!result.shortcuts.contains(id)) {
            result.shortcuts.insert(id, defaults.value(id));
        }
    }
    // 丢弃旧版本、插件或手工修改数据库留下的未知动作。服务只负责内置固定动作；插件应
    // 使用自己的命名空间和设置存储，不能借此覆盖主程序动作。
    for (auto iterator = result.shortcuts.begin(); iterator != result.shortcuts.end();) {
        if (!defaults.contains(iterator.key())) {
            iterator = result.shortcuts.erase(iterator);
        } else {
            ++iterator;
        }
    }
    return result;
}

bool TerminalSettings::validate(QString *errorMessage) const
{
    const TerminalSettings normalizedSettings = normalized();
    QHash<QString, QString> assignedSequences;
    for (const QString &id : shortcutIds()) {
        const QKeySequence sequence = normalizedSettings.shortcuts.value(id);
        if (sequence.isEmpty()) {
            continue;
        }
        const QString portable = sequence.toString(QKeySequence::PortableText);
        const QString existingAction = assignedSequences.value(portable);
        if (!existingAction.isEmpty()) {
            if (errorMessage != nullptr) {
                *errorMessage = QObject::tr("快捷键 %1 同时分配给“%2”和“%3”")
                                    .arg(portable,
                                         shortcutDisplayName(existingAction),
                                         shortcutDisplayName(id));
            }
            return false;
        }
        assignedSequences.insert(portable, id);
    }
    return true;
}

ThemeMode TerminalSettings::effectiveThemeMode() const
{
    if (themeMode != ThemeMode::FollowSystem) {
        return themeMode;
    }

    if (QGuiApplication::instance() != nullptr) {
        const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
        if (scheme == Qt::ColorScheme::Dark) {
            return ThemeMode::Dark;
        }
        if (scheme == Qt::ColorScheme::Light) {
            return ThemeMode::Light;
        }
        // 部分 Linux 桌面不会声明 ColorScheme。此时按系统调色板亮度判断，比永远选浅色
        // 更符合“跟随系统”的用户预期。
        const QColor windowColor = QGuiApplication::palette().color(QPalette::Window);
        return windowColor.lightness() < 128 ? ThemeMode::Dark : ThemeMode::Light;
    }
    return ThemeMode::Light;
}

const QList<TerminalThemePreset> &TerminalSettings::themePresets()
{
    // 静态数据表：新增一套配色只需在这里加一行，序列化、设置界面与终端渲染会自动跟上。
    // 第一项的颜色只是列表里的占位（真正的取值由 effectivePalette() 按主题解析），因此填的是
    // 内置深色配色，保证任何直接读取该项 palette 的调用方也能拿到一套可用颜色。
    static const QList<TerminalThemePreset> presets = {
        {QStringLiteral("default"),
         QObject::tr("默认"),
         true,
         {QColor(QStringLiteral("#111318")), QColor(QStringLiteral("#e6e6e6")),
          QColor(QStringLiteral("#f6c85f")), QColor(QStringLiteral("#34598a")),
          QColor(QStringLiteral("#66b3ff"))}},
        {QStringLiteral("termius_dark"),
         QStringLiteral("Termius Dark"),
         true,
         {QColor(QStringLiteral("#131a24")), QColor(QStringLiteral("#d5dae1")),
          QColor(QStringLiteral("#2bd981")), QColor(QStringLiteral("#26303f")),
          QColor(QStringLiteral("#4aa3ff"))}},
        {QStringLiteral("termius_light"),
         QStringLiteral("Termius Light"),
         false,
         {QColor(QStringLiteral("#c9d0d7")), QColor(QStringLiteral("#2c3542")),
          QColor(QStringLiteral("#1f8f56")), QColor(QStringLiteral("#b3bcc6")),
          QColor(QStringLiteral("#0b57d0"))}},
        {QStringLiteral("flexoki_dark"),
         QStringLiteral("Flexoki Dark"),
         true,
         {QColor(QStringLiteral("#100f0f")), QColor(QStringLiteral("#cecdc3")),
          QColor(QStringLiteral("#879a39")), QColor(QStringLiteral("#282726")),
          QColor(QStringLiteral("#4385be"))}},
        {QStringLiteral("flexoki_light"),
         QStringLiteral("Flexoki Light"),
         false,
         {QColor(QStringLiteral("#fffcf0")), QColor(QStringLiteral("#100f0f")),
          QColor(QStringLiteral("#879a39")), QColor(QStringLiteral("#e6e4d9")),
          QColor(QStringLiteral("#205ea6"))}},
        {QStringLiteral("kanagawa_wave"),
         QStringLiteral("Kanagawa Wave"),
         true,
         {QColor(QStringLiteral("#1f1f28")), QColor(QStringLiteral("#dcd7ba")),
          QColor(QStringLiteral("#c8c093")), QColor(QStringLiteral("#2d4f67")),
          QColor(QStringLiteral("#7e9cd8"))}},
        {QStringLiteral("kanagawa_dragon"),
         QStringLiteral("Kanagawa Dragon"),
         true,
         {QColor(QStringLiteral("#181616")), QColor(QStringLiteral("#c5c9c5")),
          QColor(QStringLiteral("#c8c093")), QColor(QStringLiteral("#2d4f67")),
          QColor(QStringLiteral("#8ba4b0"))}},
        {QStringLiteral("kanagawa_lotus"),
         QStringLiteral("Kanagawa Lotus"),
         false,
         {QColor(QStringLiteral("#f2ecbc")), QColor(QStringLiteral("#545464")),
          QColor(QStringLiteral("#43436c")), QColor(QStringLiteral("#dcd7ba")),
          QColor(QStringLiteral("#4d699b"))}},
        {QStringLiteral("hacker_blue"),
         QStringLiteral("Hacker Blue"),
         true,
         {QColor(QStringLiteral("#030814")), QColor(QStringLiteral("#29b6ff")),
          QColor(QStringLiteral("#29b6ff")), QColor(QStringLiteral("#0b2a44")),
          QColor(QStringLiteral("#7fdcff"))}},
        {QStringLiteral("hacker_green"),
         QStringLiteral("Hacker Green"),
         true,
         {QColor(QStringLiteral("#001400")), QColor(QStringLiteral("#22dd22")),
          QColor(QStringLiteral("#33ff33")), QColor(QStringLiteral("#0b3d0b")),
          QColor(QStringLiteral("#66ff88"))}},
        {QStringLiteral("hacker_red"),
         QStringLiteral("Hacker Red"),
         true,
         {QColor(QStringLiteral("#150000")), QColor(QStringLiteral("#d22b2b")),
          QColor(QStringLiteral("#ff3b3b")), QColor(QStringLiteral("#3d0b0b")),
          QColor(QStringLiteral("#ff7a7a"))}},
        {QStringLiteral("everforest_dark"),
         QStringLiteral("Everforest Dark"),
         true,
         {QColor(QStringLiteral("#2d353b")), QColor(QStringLiteral("#d3c6aa")),
          QColor(QStringLiteral("#d3c6aa")), QColor(QStringLiteral("#475258")),
          QColor(QStringLiteral("#7fbbb3"))}},
        {QStringLiteral("everforest_light"),
         QStringLiteral("Everforest Light"),
         false,
         {QColor(QStringLiteral("#fdf6e3")), QColor(QStringLiteral("#5c6a72")),
          QColor(QStringLiteral("#5c6a72")), QColor(QStringLiteral("#edeada")),
          QColor(QStringLiteral("#3a94c5"))}},
        {QStringLiteral("night_owl"),
         QStringLiteral("Night Owl"),
         true,
         {QColor(QStringLiteral("#011627")), QColor(QStringLiteral("#d6deeb")),
          QColor(QStringLiteral("#80a4c2")), QColor(QStringLiteral("#1d3b53")),
          QColor(QStringLiteral("#82aaff"))}},
        {QStringLiteral("light_owl"),
         QStringLiteral("Light Owl"),
         false,
         {QColor(QStringLiteral("#fbfbfb")), QColor(QStringLiteral("#403f53")),
          QColor(QStringLiteral("#403f53")), QColor(QStringLiteral("#e0e0e0")),
          QColor(QStringLiteral("#4876d6"))}},
        {QStringLiteral("aura"),
         QStringLiteral("Aura"),
         true,
         {QColor(QStringLiteral("#15141b")), QColor(QStringLiteral("#edecee")),
          QColor(QStringLiteral("#a277ff")), QColor(QStringLiteral("#29263b")),
          QColor(QStringLiteral("#61ffca"))}},
        {QStringLiteral("rose_pine"),
         QStringLiteral("Rosé Pine"),
         true,
         {QColor(QStringLiteral("#191724")), QColor(QStringLiteral("#e0def4")),
          QColor(QStringLiteral("#524f67")), QColor(QStringLiteral("#26233a")),
          QColor(QStringLiteral("#9ccfd8"))}},
        {QStringLiteral("rose_pine_moon"),
         QStringLiteral("Rosé Pine Moon"),
         true,
         {QColor(QStringLiteral("#232136")), QColor(QStringLiteral("#e0def4")),
          QColor(QStringLiteral("#59546d")), QColor(QStringLiteral("#2a273f")),
          QColor(QStringLiteral("#9ccfd8"))}},
        {QStringLiteral("rose_pine_dawn"),
         QStringLiteral("Rosé Pine Dawn"),
         false,
         {QColor(QStringLiteral("#faf4ed")), QColor(QStringLiteral("#575279")),
          QColor(QStringLiteral("#575279")), QColor(QStringLiteral("#f2e9e1")),
          QColor(QStringLiteral("#56949f"))}},
        {QStringLiteral("cobalt2"),
         QStringLiteral("Cobalt2"),
         true,
         {QColor(QStringLiteral("#193549")), QColor(QStringLiteral("#ffffff")),
          QColor(QStringLiteral("#ffc600")), QColor(QStringLiteral("#0d3a58")),
          QColor(QStringLiteral("#ff9d00"))}},
        {QStringLiteral("octocat_dark"),
         QStringLiteral("Octocat Dark"),
         true,
         {QColor(QStringLiteral("#0d1117")), QColor(QStringLiteral("#c9d1d9")),
          QColor(QStringLiteral("#58a6ff")), QColor(QStringLiteral("#21262d")),
          QColor(QStringLiteral("#58a6ff"))}},
        {QStringLiteral("octocat_light"),
         QStringLiteral("Octocat Light"),
         false,
         {QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#24292f")),
          QColor(QStringLiteral("#0969da")), QColor(QStringLiteral("#eaeef2")),
          QColor(QStringLiteral("#0969da"))}},
        {QStringLiteral("ayu_dark"),
         QStringLiteral("Ayu Dark"),
         true,
         {QColor(QStringLiteral("#0b0e14")), QColor(QStringLiteral("#bfbdb6")),
          QColor(QStringLiteral("#ffb454")), QColor(QStringLiteral("#1b2733")),
          QColor(QStringLiteral("#59c2ff"))}},
        {QStringLiteral("ayu_light"),
         QStringLiteral("Ayu Light"),
         false,
         {QColor(QStringLiteral("#fcfcfc")), QColor(QStringLiteral("#5c6166")),
          QColor(QStringLiteral("#ff9940")), QColor(QStringLiteral("#e7e8e9")),
          QColor(QStringLiteral("#399ee6"))}},
        {QStringLiteral("cyberpunk"),
         QStringLiteral("Cyberpunk"),
         true,
         {QColor(QStringLiteral("#241734")), QColor(QStringLiteral("#e6e1ff")),
          QColor(QStringLiteral("#00f0b5")), QColor(QStringLiteral("#3b2a57")),
          QColor(QStringLiteral("#00e5ff"))}},
        {QStringLiteral("cyberpunk_scarlet"),
         QStringLiteral("Cyberpunk Scarlet"),
         true,
         {QColor(QStringLiteral("#0d0208")), QColor(QStringLiteral("#ff2e63")),
          QColor(QStringLiteral("#00ffb3")), QColor(QStringLiteral("#33091a")),
          QColor(QStringLiteral("#ff5c8a"))}},
        {QStringLiteral("romania_night"),
         QStringLiteral("Romania Night"),
         true,
         {QColor(QStringLiteral("#10222b")), QColor(QStringLiteral("#f2e8cf")),
          QColor(QStringLiteral("#fcbf49")), QColor(QStringLiteral("#1d3d4a")),
          QColor(QStringLiteral("#77bfa3"))}},
        {QStringLiteral("romania_day"),
         QStringLiteral("Romania Day"),
         false,
         {QColor(QStringLiteral("#f7e7e4")), QColor(QStringLiteral("#7a2e2e")),
          QColor(QStringLiteral("#c8553d")), QColor(QStringLiteral("#efd3ce")),
          QColor(QStringLiteral("#1f7a8c"))}},
        {QStringLiteral("aubergine"),
         QStringLiteral("Aubergine"),
         true,
         {QColor(QStringLiteral("#2b0b2b")), QColor(QStringLiteral("#efe1f0")),
          QColor(QStringLiteral("#7fe1a1")), QColor(QStringLiteral("#43164a")),
          QColor(QStringLiteral("#c58af9"))}},
        {QStringLiteral("peach_fresh"),
         QStringLiteral("Peach Fresh"),
         false,
         {QColor(QStringLiteral("#f7b490")), QColor(QStringLiteral("#5a2a16")),
          QColor(QStringLiteral("#b3502b")), QColor(QStringLiteral("#eda07a")),
          QColor(QStringLiteral("#2e6e7e"))}},
        {QStringLiteral("nineteen84_dark"),
         QStringLiteral("1984 Dark"),
         true,
         {QColor(QStringLiteral("#101033")), QColor(QStringLiteral("#d8d8e8")),
          QColor(QStringLiteral("#8ae234")), QColor(QStringLiteral("#22224a")),
          QColor(QStringLiteral("#7a88ff"))}},
        {QStringLiteral("nineteen84_light"),
         QStringLiteral("1984 Light"),
         false,
         {QColor(QStringLiteral("#dde4f0")), QColor(QStringLiteral("#2b2f45")),
          QColor(QStringLiteral("#2f8f4e")), QColor(QStringLiteral("#c7d1e6")),
          QColor(QStringLiteral("#3355cc"))}},
        {QStringLiteral("winter_night"),
         QStringLiteral("Winter Night"),
         true,
         {QColor(QStringLiteral("#0e2233")), QColor(QStringLiteral("#e8e2cf")),
          QColor(QStringLiteral("#f0c674")), QColor(QStringLiteral("#1b3a4f")),
          QColor(QStringLiteral("#8fc1e3"))}},
        {QStringLiteral("winter_day"),
         QStringLiteral("Winter Day"),
         false,
         {QColor(QStringLiteral("#ede6d6")), QColor(QStringLiteral("#33403a")),
          QColor(QStringLiteral("#4e7a63")), QColor(QStringLiteral("#dcd3be")),
          QColor(QStringLiteral("#2f6690"))}},
        {QStringLiteral("tokyo_night"),
         QStringLiteral("Tokyo Night"),
         true,
         {QColor(QStringLiteral("#1a1b26")), QColor(QStringLiteral("#c0caf5")),
          QColor(QStringLiteral("#c0caf5")), QColor(QStringLiteral("#33467c")),
          QColor(QStringLiteral("#7aa2f7"))}},
        {QStringLiteral("tokyo_day"),
         QStringLiteral("Tokyo Day"),
         false,
         {QColor(QStringLiteral("#e1e2e7")), QColor(QStringLiteral("#3760bf")),
          QColor(QStringLiteral("#3760bf")), QColor(QStringLiteral("#c4c8da")),
          QColor(QStringLiteral("#2e7de9"))}},
        {QStringLiteral("catppuccin_latte"),
         QStringLiteral("Catppuccin Latte"),
         false,
         {QColor(QStringLiteral("#eff1f5")), QColor(QStringLiteral("#4c4f69")),
          QColor(QStringLiteral("#dc8a78")), QColor(QStringLiteral("#dce0e8")),
          QColor(QStringLiteral("#1e66f5"))}},
        {QStringLiteral("catppuccin_mocha"),
         QStringLiteral("Catppuccin Mocha"),
         true,
         {QColor(QStringLiteral("#1e1e2e")), QColor(QStringLiteral("#cdd6f4")),
          QColor(QStringLiteral("#f5e0dc")), QColor(QStringLiteral("#313244")),
          QColor(QStringLiteral("#89b4fa"))}},
        {QStringLiteral("diwali"),
         QStringLiteral("Diwali"),
         true,
         {QColor(QStringLiteral("#2b1046")), QColor(QStringLiteral("#ffe8c2")),
          QColor(QStringLiteral("#ffb300")), QColor(QStringLiteral("#43206b")),
          QColor(QStringLiteral("#ff7043"))}},
        {QStringLiteral("movember"),
         QStringLiteral("Movember"),
         true,
         {QColor(QStringLiteral("#241206")), QColor(QStringLiteral("#f3d9b1")),
          QColor(QStringLiteral("#e0a356")), QColor(QStringLiteral("#3d2110")),
          QColor(QStringLiteral("#7fb069"))}},
        {QStringLiteral("atom_one_dark"),
         QStringLiteral("Atom One Dark"),
         true,
         {QColor(QStringLiteral("#282c34")), QColor(QStringLiteral("#abb2bf")),
          QColor(QStringLiteral("#528bff")), QColor(QStringLiteral("#3e4451")),
          QColor(QStringLiteral("#61afef"))}},
        {QStringLiteral("atom_one_light"),
         QStringLiteral("Atom One Light"),
         false,
         {QColor(QStringLiteral("#fafafa")), QColor(QStringLiteral("#383a42")),
          QColor(QStringLiteral("#526fff")), QColor(QStringLiteral("#e5e5e6")),
          QColor(QStringLiteral("#4078f2"))}},
        {QStringLiteral("halloween"),
         QStringLiteral("Halloween"),
         true,
         {QColor(QStringLiteral("#1b0a20")), QColor(QStringLiteral("#f2b25b")),
          QColor(QStringLiteral("#ff7518")), QColor(QStringLiteral("#35132f")),
          QColor(QStringLiteral("#a3e048"))}},
        {QStringLiteral("dia_de_muertos"),
         QStringLiteral("Dia De Muertos"),
         false,
         {QColor(QStringLiteral("#fff6e6")), QColor(QStringLiteral("#2e1a47")),
          QColor(QStringLiteral("#e4572e")), QColor(QStringLiteral("#f6e3c5")),
          QColor(QStringLiteral("#17696a"))}},
        {QStringLiteral("gruvbox_dark"),
         QStringLiteral("Gruvbox Dark"),
         true,
         {QColor(QStringLiteral("#282828")), QColor(QStringLiteral("#ebdbb2")),
          QColor(QStringLiteral("#ebdbb2")), QColor(QStringLiteral("#504945")),
          QColor(QStringLiteral("#83a598"))}},
        {QStringLiteral("gruvbox_light"),
         QStringLiteral("Gruvbox Light"),
         false,
         {QColor(QStringLiteral("#fbf1c7")), QColor(QStringLiteral("#3c3836")),
          QColor(QStringLiteral("#3c3836")), QColor(QStringLiteral("#ebdbb2")),
          QColor(QStringLiteral("#076678"))}},
        {QStringLiteral("material_dark"),
         QStringLiteral("Material Dark"),
         true,
         {QColor(QStringLiteral("#263238")), QColor(QStringLiteral("#eeffff")),
          QColor(QStringLiteral("#ffcc00")), QColor(QStringLiteral("#314549")),
          QColor(QStringLiteral("#80cbc4"))}},
        {QStringLiteral("material_light"),
         QStringLiteral("Material Light"),
         false,
         {QColor(QStringLiteral("#fafafa")), QColor(QStringLiteral("#546e7a")),
          QColor(QStringLiteral("#272727")), QColor(QStringLiteral("#e7e7e8")),
          QColor(QStringLiteral("#39adb5"))}},
        {QStringLiteral("manhattan"),
         QStringLiteral("Manhattan"),
         true,
         {QColor(QStringLiteral("#0a0a0a")), QColor(QStringLiteral("#d6d6d6")),
          QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#2a2a2a")),
          QColor(QStringLiteral("#9e9e9e"))}},
        {QStringLiteral("plastic_world"),
         QStringLiteral("Plastic World"),
         false,
         {QColor(QStringLiteral("#ff4fd8")), QColor(QStringLiteral("#ffffff")),
          QColor(QStringLiteral("#fff176")), QColor(QStringLiteral("#e23fbf")),
          QColor(QStringLiteral("#9cff57"))}},
        {QStringLiteral("basic"),
         QStringLiteral("Basic"),
         false,
         {QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#000000")),
          QColor(QStringLiteral("#000000")), QColor(QStringLiteral("#b5d5ff")),
          QColor(QStringLiteral("#007700"))}},
        {QStringLiteral("homebrew"),
         QStringLiteral("Homebrew"),
         true,
         {QColor(QStringLiteral("#000000")), QColor(QStringLiteral("#00ff00")),
          QColor(QStringLiteral("#00ff00")), QColor(QStringLiteral("#003300")),
          QColor(QStringLiteral("#33ff66"))}},
        {QStringLiteral("grass"),
         QStringLiteral("Grass"),
         true,
         {QColor(QStringLiteral("#13773d")), QColor(QStringLiteral("#fff0a5")),
          QColor(QStringLiteral("#8ae234")), QColor(QStringLiteral("#0b5a2c")),
          QColor(QStringLiteral("#ffd54f"))}},
        {QStringLiteral("man_page"),
         QStringLiteral("Man Page"),
         false,
         {QColor(QStringLiteral("#fef49c")), QColor(QStringLiteral("#000000")),
          QColor(QStringLiteral("#7f7f7f")), QColor(QStringLiteral("#f2de7a")),
          QColor(QStringLiteral("#0645ad"))}},
        {QStringLiteral("novel"),
         QStringLiteral("Novel"),
         false,
         {QColor(QStringLiteral("#dfdbc3")), QColor(QStringLiteral("#3b2322")),
          QColor(QStringLiteral("#73635a")), QColor(QStringLiteral("#cfc9ae")),
          QColor(QStringLiteral("#0d6a8c"))}},
        {QStringLiteral("ocean"),
         QStringLiteral("Ocean"),
         true,
         {QColor(QStringLiteral("#224fbc")), QColor(QStringLiteral("#ffffff")),
          QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#1a3c8f")),
          QColor(QStringLiteral("#b3e5fc"))}},
        {QStringLiteral("pro"),
         QStringLiteral("Pro"),
         true,
         {QColor(QStringLiteral("#000000")), QColor(QStringLiteral("#f2f2f2")),
          QColor(QStringLiteral("#4d4d4d")), QColor(QStringLiteral("#414141")),
          QColor(QStringLiteral("#00c400"))}},
        {QStringLiteral("red_sands"),
         QStringLiteral("Red Sands"),
         true,
         {QColor(QStringLiteral("#7a251e")), QColor(QStringLiteral("#d7c9a7")),
          QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#5a1a14")),
          QColor(QStringLiteral("#e8b87a"))}},
        {QStringLiteral("solarized_dark"),
         QStringLiteral("Solarized Dark"),
         true,
         {QColor(QStringLiteral("#002b36")), QColor(QStringLiteral("#839496")),
          QColor(QStringLiteral("#d33682")), QColor(QStringLiteral("#073642")),
          QColor(QStringLiteral("#268bd2"))}},
        {QStringLiteral("solarized_light"),
         QStringLiteral("Solarized Light"),
         false,
         {QColor(QStringLiteral("#fdf6e3")), QColor(QStringLiteral("#657b83")),
          QColor(QStringLiteral("#586e75")), QColor(QStringLiteral("#eee8d5")),
          QColor(QStringLiteral("#268bd2"))}},
        {QStringLiteral("silver_aerogel"),
         QStringLiteral("Silver Aerogel"),
         false,
         {QColor(QStringLiteral("#929292")), QColor(QStringLiteral("#000000")),
          QColor(QStringLiteral("#4d4d4d")), QColor(QStringLiteral("#b0b0b0")),
          QColor(QStringLiteral("#003c8f"))}},
        {QStringLiteral("dracula"),
         QStringLiteral("Dracula"),
         true,
         {QColor(QStringLiteral("#282a36")), QColor(QStringLiteral("#f8f8f2")),
          QColor(QStringLiteral("#ff79c6")), QColor(QStringLiteral("#44475a")),
          QColor(QStringLiteral("#8be9fd"))}},
        {QStringLiteral("monokai"),
         QStringLiteral("Monokai"),
         true,
         {QColor(QStringLiteral("#272822")), QColor(QStringLiteral("#f8f8f2")),
          QColor(QStringLiteral("#f92672")), QColor(QStringLiteral("#49483e")),
          QColor(QStringLiteral("#66d9ef"))}},
        {QStringLiteral("nord"),
         QStringLiteral("Nord Dark"),
         true,
         {QColor(QStringLiteral("#2e3440")), QColor(QStringLiteral("#d8dee9")),
          QColor(QStringLiteral("#88c0d0")), QColor(QStringLiteral("#434c5e")),
          QColor(QStringLiteral("#81a1c1"))}},
        {QStringLiteral("nord_light"),
         QStringLiteral("Nord Light"),
         false,
         {QColor(QStringLiteral("#e5e9f0")), QColor(QStringLiteral("#2e3440")),
          QColor(QStringLiteral("#5e81ac")), QColor(QStringLiteral("#d8dee9")),
          QColor(QStringLiteral("#5e81ac"))}},
    };
    return presets;
}

const TerminalThemePreset *TerminalSettings::themePreset(const QString &id)
{
    const QList<TerminalThemePreset> &presets = themePresets();
    // 线性查找足够：表只有几十项，且调用点是设置界面构建与主题解析，都不在渲染热路径上。
    for (const TerminalThemePreset &preset : presets) {
        if (preset.id == id) {
            return &preset;
        }
    }
    return nullptr;
}

QStringList TerminalSettings::recommendedFontFamilies()
{
    // 编程字体列表按“终端客户端常见预置 + 各平台自带等宽字体”排列。这里只给名字，是否真的
    // 安装由设置界面与 QFontDatabase 求交集后决定。
    return {
        QStringLiteral("Source Code Pro"),
        QStringLiteral("Source Code Pro Medium"),
        QStringLiteral("JetBrains Mono"),
        QStringLiteral("Fira Code"),
        QStringLiteral("Fira Mono"),
        QStringLiteral("Fira Mono Medium"),
        QStringLiteral("Cascadia Code"),
        QStringLiteral("Cascadia Mono"),
        QStringLiteral("IBM Plex Mono"),
        QStringLiteral("Roboto Mono"),
        QStringLiteral("Inconsolata"),
        QStringLiteral("Inconsolata-g"),
        QStringLiteral("Anonymous Pro"),
        QStringLiteral("Ubuntu Mono"),
        QStringLiteral("Ubuntu Sans Mono"),
        QStringLiteral("Droid Sans Mono"),
        QStringLiteral("DejaVu Sans Mono"),
        QStringLiteral("Liberation Mono"),
        QStringLiteral("Noto Sans Mono"),
        QStringLiteral("PT Mono"),
        QStringLiteral("Hack"),
        QStringLiteral("Iosevka"),
        QStringLiteral("Space Mono"),
        QStringLiteral("Victor Mono"),
        QStringLiteral("Meslo"),
        QStringLiteral("Meslo LG S"),
        QStringLiteral("MesloLGS NF"),
        QStringLiteral("Menlo"),
        QStringLiteral("Monaco"),
        QStringLiteral("SF Mono"),
        QStringLiteral("Andale Mono"),
        QStringLiteral("Consolas"),
        QStringLiteral("Courier New"),
        QStringLiteral("Sarasa Mono SC"),
        QStringLiteral("Maple Mono"),
    };
}

TerminalPalette TerminalSettings::effectivePalette() const
{
    // "default" 之外的配色都是搭配好的一整套固定主题（对应主流终端模拟器的同名预设），不随
    // 浅色/深色切换而改变，因此命中预设后直接返回，不看 effectiveThemeMode()。
    if (colorSchemeId != QLatin1String("default")) {
        if (const TerminalThemePreset *preset = themePreset(colorSchemeId)) {
            return preset->palette;
        }
    }

    if (effectiveThemeMode() == ThemeMode::Dark) {
        return {
            QColor(QStringLiteral("#111318")),
            QColor(QStringLiteral("#e6e6e6")),
            QColor(QStringLiteral("#f6c85f")),
            QColor(QStringLiteral("#34598a")),
            QColor(QStringLiteral("#66b3ff")),
        };
    }
    return {
        QColor(QStringLiteral("#ffffff")),
        QColor(QStringLiteral("#20242b")),
        QColor(QStringLiteral("#0b57d0")),
        QColor(QStringLiteral("#b8d5ff")),
        QColor(QStringLiteral("#0057b8")),
    };
}

bool TerminalSettings::operator==(const TerminalSettings &other) const
{
    return themeMode == other.themeMode
        && colorSchemeId == other.colorSchemeId
        && fontFamily == other.fontFamily
        && fontPointSize == other.fontPointSize
        && lineHeightPercent == other.lineHeightPercent
        && qFuzzyCompare(letterSpacing + 1.0, other.letterSpacing + 1.0)
        && cursorShape == other.cursorShape
        && cursorBlink == other.cursorBlink
        && scrollbackLines == other.scrollbackLines
        && audibleBell == other.audibleBell
        && commandAutoCompletion == other.commandAutoCompletion
        && automaticReconnect == other.automaticReconnect
        && reconnectMaxAttempts == other.reconnectMaxAttempts
        && reconnectBaseDelaySeconds == other.reconnectBaseDelaySeconds
        && shortcuts == other.shortcuts;
}

TerminalSettingsService::TerminalSettingsService(ApplicationDatabase *database, QObject *parent)
    : QObject(parent)
    , m_database(database)
    , m_currentSettings(TerminalSettings{}.normalized())
{
    Q_ASSERT(QThread::currentThread() == thread());

    // 系统深浅色切换不改变持久化值，但 FollowSystem 的有效配色已经变化，必须重新广播。
    // 直接连接安全，因为 QStyleHints 与本服务都属于 GUI 主线程。
    if (QGuiApplication::instance() != nullptr) {
        connect(QGuiApplication::styleHints(),
                &QStyleHints::colorSchemeChanged,
                this,
                [this](Qt::ColorScheme) {
                    if (m_currentSettings.themeMode == ThemeMode::FollowSystem) {
                        applyApplicationPalette();
                        emit settingsChanged(m_currentSettings);
                    }
                });
    }
}

bool TerminalSettingsService::load(QString *errorMessage)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_database == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = tr("设置服务没有可用的应用数据库");
        }
        return false;
    }

    TerminalSettings loaded;
    loaded.shortcuts = TerminalSettings::defaultShortcuts();
    QStringList warnings;

    const auto readValue = [this, errorMessage](const QString &key,
                                                 bool *queryFailed)
        -> std::optional<QString> {
        QString queryError;
        const std::optional<QString> value = m_database->setting(key, &queryError);
        if (!queryError.isEmpty()) {
            *queryFailed = true;
            if (errorMessage != nullptr) {
                *errorMessage = queryError;
            }
        }
        return value;
    };

    bool queryFailed = false;
    const auto assignInteger = [&](const QString &key, int *destination) {
        const std::optional<QString> raw = readValue(key, &queryFailed);
        if (!raw.has_value() || queryFailed) {
            return;
        }
        bool ok = false;
        const int value = raw->toInt(&ok);
        if (ok) {
            *destination = value;
        } else {
            warnings.append(tr("设置项 %1 的值“%2”不是整数，已恢复默认值").arg(key, *raw));
        }
    };
    const auto assignBoolean = [&](const QString &key, bool *destination) {
        const std::optional<QString> raw = readValue(key, &queryFailed);
        if (!raw.has_value() || queryFailed) {
            return;
        }
        const std::optional<bool> parsed = parseBool(*raw);
        if (parsed.has_value()) {
            *destination = *parsed;
        } else {
            warnings.append(tr("设置项 %1 的值“%2”不是布尔值，已恢复默认值").arg(key, *raw));
        }
    };

    if (const std::optional<QString> raw = readValue(kThemeKey, &queryFailed);
        raw.has_value() && !queryFailed) {
        const std::optional<ThemeMode> parsed = parseTheme(*raw);
        if (parsed.has_value()) {
            loaded.themeMode = *parsed;
        } else {
            warnings.append(tr("未知主题“%1”，已恢复为跟随系统").arg(*raw));
        }
    }
    if (queryFailed) {
        return false;
    }

    if (const std::optional<QString> raw = readValue(kColorSchemeKey, &queryFailed);
        raw.has_value() && !queryFailed) {
        const QString candidate = raw->trimmed();
        if (TerminalSettings::themePreset(candidate) != nullptr) {
            loaded.colorSchemeId = candidate;
        } else {
            warnings.append(tr("未知终端配色“%1”，已恢复默认配色").arg(*raw));
        }
    }
    if (queryFailed) {
        return false;
    }

    if (const std::optional<QString> raw = readValue(kFontFamilyKey, &queryFailed);
        raw.has_value() && !queryFailed) {
        loaded.fontFamily = *raw;
    }
    assignInteger(kFontSizeKey, &loaded.fontPointSize);
    assignInteger(kLineHeightKey, &loaded.lineHeightPercent);

    if (const std::optional<QString> raw = readValue(kLetterSpacingKey, &queryFailed);
        raw.has_value() && !queryFailed) {
        bool ok = false;
        const qreal value = raw->toDouble(&ok);
        if (ok && std::isfinite(value)) {
            loaded.letterSpacing = value;
        } else {
            warnings.append(tr("字符间距“%1”无效，已恢复默认值").arg(*raw));
        }
    }

    if (const std::optional<QString> raw = readValue(kCursorShapeKey, &queryFailed);
        raw.has_value() && !queryFailed) {
        const std::optional<TerminalCursorShape> parsed = parseCursorShape(*raw);
        if (parsed.has_value()) {
            loaded.cursorShape = *parsed;
        } else {
            warnings.append(tr("未知光标形态“%1”，已恢复为块状光标").arg(*raw));
        }
    }
    assignBoolean(kCursorBlinkKey, &loaded.cursorBlink);
    assignInteger(kScrollbackKey, &loaded.scrollbackLines);
    assignBoolean(kAudibleBellKey, &loaded.audibleBell);
    assignBoolean(kCommandAutoCompletionKey, &loaded.commandAutoCompletion);
    assignBoolean(kAutomaticReconnectKey, &loaded.automaticReconnect);
    assignInteger(kReconnectMaxAttemptsKey, &loaded.reconnectMaxAttempts);
    assignInteger(kReconnectBaseDelayKey, &loaded.reconnectBaseDelaySeconds);

    for (const QString &id : TerminalSettings::shortcutIds()) {
        const std::optional<QString> raw = readValue(kShortcutPrefix + id, &queryFailed);
        if (!raw.has_value() || queryFailed) {
            continue;
        }
        if (raw->isEmpty()) {
            loaded.shortcuts.insert(id, {});
            continue;
        }
        const QKeySequence parsed = QKeySequence::fromString(*raw, QKeySequence::PortableText);
        if (parsed.isEmpty()) {
            warnings.append(tr("动作“%1”的快捷键“%2”无效，已恢复默认值")
                                .arg(TerminalSettings::shortcutDisplayName(id), *raw));
        } else {
            loaded.shortcuts.insert(id, parsed);
        }
    }
    if (queryFailed) {
        return false;
    }

    loaded = loaded.normalized();
    QString validationError;
    if (!loaded.validate(&validationError)) {
        // 外部编辑数据库可能制造重复快捷键。保留外观和终端设置，只把快捷键整体恢复默认，
        // 这样用户仍能打开设置界面修正，而不会因 QAction::AmbiguousShortcut 无法操作。
        warnings.append(validationError + tr("；快捷键已整体恢复默认值"));
        loaded.shortcuts = TerminalSettings::defaultShortcuts();
    }

    const bool changed = !(loaded == m_currentSettings);
    m_currentSettings = loaded;
    applyApplicationPalette();
    if (changed) {
        emit settingsChanged(m_currentSettings);
    }
    for (const QString &warning : std::as_const(warnings)) {
        emit warningOccurred(warning);
    }
    return true;
}

bool TerminalSettingsService::save(const TerminalSettings &settings, QString *errorMessage)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_database == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = tr("设置服务没有可用的应用数据库");
        }
        return false;
    }

    const TerminalSettings normalizedSettings = settings.normalized();
    if (!normalizedSettings.validate(errorMessage)) {
        return false;
    }

    const bool saved = m_database->runInTransaction(
        [this, &normalizedSettings](QString *transactionError) {
            const auto write = [this, transactionError](const QString &key,
                                                        const QString &value) {
                return m_database->setSetting(key, value, transactionError);
            };
            if (!write(kThemeKey, serializeTheme(normalizedSettings.themeMode))
                || !write(kColorSchemeKey, normalizedSettings.colorSchemeId)
                || !write(kFontFamilyKey, normalizedSettings.fontFamily)
                || !write(kFontSizeKey, QString::number(normalizedSettings.fontPointSize))
                || !write(kLineHeightKey, QString::number(normalizedSettings.lineHeightPercent))
                || !write(kLetterSpacingKey,
                          QString::number(normalizedSettings.letterSpacing, 'f', 2))
                || !write(kCursorShapeKey,
                          serializeCursorShape(normalizedSettings.cursorShape))
                || !write(kCursorBlinkKey, serializeBool(normalizedSettings.cursorBlink))
                || !write(kScrollbackKey, QString::number(normalizedSettings.scrollbackLines))
                || !write(kAudibleBellKey, serializeBool(normalizedSettings.audibleBell))
                || !write(kCommandAutoCompletionKey,
                          serializeBool(normalizedSettings.commandAutoCompletion))
                || !write(kAutomaticReconnectKey,
                          serializeBool(normalizedSettings.automaticReconnect))
                || !write(kReconnectMaxAttemptsKey,
                          QString::number(normalizedSettings.reconnectMaxAttempts))
                || !write(kReconnectBaseDelayKey,
                          QString::number(normalizedSettings.reconnectBaseDelaySeconds))) {
                return false;
            }
            for (const QString &id : TerminalSettings::shortcutIds()) {
                if (!write(kShortcutPrefix + id,
                           normalizedSettings.shortcuts.value(id).toString(
                               QKeySequence::PortableText))) {
                    return false;
                }
            }
            return true;
        },
        errorMessage);
    if (!saved) {
        return false;
    }

    const bool changed = !(normalizedSettings == m_currentSettings);
    m_currentSettings = normalizedSettings;
    applyApplicationPalette();
    if (changed) {
        emit settingsChanged(m_currentSettings);
    }
    return true;
}

void TerminalSettingsService::applyTransient(const TerminalSettings &settings)
{
    Q_ASSERT(QThread::currentThread() == thread());
    const TerminalSettings normalizedSettings = settings.normalized();
    if (normalizedSettings == m_currentSettings) {
        return;
    }
    m_currentSettings = normalizedSettings;
    applyApplicationPalette();
    emit settingsChanged(m_currentSettings);
}

QKeySequence TerminalSettingsService::shortcut(const QString &actionId) const
{
    return m_currentSettings.shortcuts.value(actionId);
}

void TerminalSettingsService::bindApplicationTheme(QApplication *application)
{
    m_boundApplication = application;
    applyApplicationPalette();
}

void TerminalSettingsService::applyApplicationPalette()
{
    if (m_boundApplication == nullptr) {
        return;
    }
    m_boundApplication->setPalette(
        createApplicationPalette(m_currentSettings.effectiveThemeMode() == ThemeMode::Dark));
}
