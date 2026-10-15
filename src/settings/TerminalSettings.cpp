#include "settings/TerminalSettings.h"

#include "storage/ApplicationDatabase.h"

#include <QApplication>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPalette>
#include <QSet>
#include <QStyleHints>
#include <QThread>

#include <algorithm>
#include <initializer_list>
#include <cmath>

namespace {

/** @brief 设置表中键名前缀，集中定义可避免保存和加载使用不同拼写。 */
const QString kLanguageKey = QStringLiteral("appearance/language");
/** @brief 沿用视图菜单已有的密度键，升级后无需迁移用户选择。 */
const QString kSidebarDensityKey = QStringLiteral("layout/sidebar_compact");
/** @brief 双击侧边栏分隔条的确认提示开关；缺失时保持默认开启以保护既有操作习惯。 */
const QString kSidebarDoubleClickPromptKey = QStringLiteral("layout/sidebar_double_click_prompt");
const QString kThemeKey = QStringLiteral("appearance/theme");
const QString kColorSchemeKey = QStringLiteral("appearance/color_scheme");
const QString kFontFamilyKey = QStringLiteral("terminal/font_family");
const QString kFontSizeKey = QStringLiteral("terminal/font_size");
const QString kLineHeightKey = QStringLiteral("terminal/line_height_percent");
const QString kLetterSpacingKey = QStringLiteral("terminal/letter_spacing");
const QString kCursorShapeKey = QStringLiteral("terminal/cursor_shape");
const QString kCursorBlinkKey = QStringLiteral("terminal/cursor_blink");
const QString kScrollbackKey = QStringLiteral("terminal/scrollback_lines");
const QString kScrollbackMaxMegabytesKey = QStringLiteral("terminal/scrollback_max_megabytes");
const QString kAudibleBellKey = QStringLiteral("terminal/audible_bell");
/** @brief 命令行历史自动提示开关；缺失该键时由结构默认值保持关闭。 */
const QString kCommandAutoCompletionKey = QStringLiteral("terminal/command_auto_completion");
/** @brief 写入命令历史的最短命令长度；缺失该键时取结构默认值 8。 */
const QString kHistoryMinCommandLengthKey =
    QStringLiteral("terminal/history_min_command_length");
/** @brief 多行粘贴确认开关；缺失该键时由结构默认值保持关闭（直接粘贴）。 */
const QString kMultiLinePasteConfirmationKey =
    QStringLiteral("terminal/multi_line_paste_confirmation");
const QString kAutomaticReconnectKey = QStringLiteral("connection/automatic_reconnect");
/** @brief 是否启用用户主密码保护凭据；与透明的本机密钥保存模式相互切换。 */
const QString kVaultPasswordEnabledKey = QStringLiteral("security/vault_password_enabled");
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

/**
 * @brief 把界面语言标签收敛为规范形式；无法识别的输入一律回退 "system"。
 *
 * 只做纯格式判断：接受字面量 "system" 与 BCP-47 / Qt 常见的 `ll` 或 `ll_CC` 形式（大小写
 * 归一为小写语言、大写地区）。是否真的存在对应 .qm 由 LocalizationService 负责，这里不联
 * 网、不查资源，保证设置层可在无 QApplication 的测试中调用。
 */
QString normalizeLanguageTag(const QString &value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty() || trimmed.compare(QLatin1String("system"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("system");
    }
    const QString unified = QString(trimmed).replace(QLatin1Char('-'), QLatin1Char('_'));
    const QStringList parts = unified.split(QLatin1Char('_'), Qt::SkipEmptyParts);
    if (parts.isEmpty() || parts.size() > 2) {
        return QStringLiteral("system");
    }
    const QString language = parts.at(0).toLower();
    if (language.size() < 2 || language.size() > 3) {
        return QStringLiteral("system");
    }
    for (const QChar ch : language) {
        if (!ch.isLetter()) {
            return QStringLiteral("system");
        }
    }
    if (parts.size() == 1) {
        return language;
    }
    const QString region = parts.at(1).toUpper();
    if (region.size() != 2) {
        return QStringLiteral("system");
    }
    for (const QChar ch : region) {
        if (!ch.isLetter()) {
            return QStringLiteral("system");
        }
    }
    return language + QLatin1Char('_') + region;
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

/** @brief 把侧边栏密度序列化为稳定字符串。 */
QString serializeSidebarDensity(const SidebarDensity density)
{
    switch (density) {
    case SidebarDensity::Compact:
        return QStringLiteral("compact");
    case SidebarDensity::Minimal:
        return QStringLiteral("minimal");
    case SidebarDensity::Relaxed:
    default:
        return QStringLiteral("relaxed");
    }
}

/**
 * @brief 解析侧边栏密度字符串；未知值返回空，调用方据此回退默认值并报告警告。
 *
 * 同时兼容精简模式上线前该键存的布尔值（true 对应紧凑、false 对应宽松），旧版本
 * 写入的数据库无需迁移即可继续读出正确的密度。
 */
std::optional<SidebarDensity> parseSidebarDensity(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QLatin1String("relaxed")) {
        return SidebarDensity::Relaxed;
    }
    if (normalized == QLatin1String("compact")) {
        return SidebarDensity::Compact;
    }
    if (normalized == QLatin1String("minimal")) {
        return SidebarDensity::Minimal;
    }
    const std::optional<bool> legacyBool = parseBool(value);
    if (legacyBool.has_value()) {
        return *legacyBool ? SidebarDensity::Compact : SidebarDensity::Relaxed;
    }
    return std::nullopt;
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

/**
 * @brief 把 16 个十六进制字符串转成 ANSI 调色板。
 *
 * 主题表里逐个写 QColor(QStringLiteral(...)) 会让每套主题占满 8 行，配色之间无法对齐着
 * 读；集中转换后一套主题的 16 色排成两行，与上游主题文档的顺序一一对应，抄错一位能一眼
 * 看出来。
 */
QVector<QColor> ansiFrom(const std::initializer_list<const char *> hex)
{
    Q_ASSERT(hex.size() == 16);
    QVector<QColor> colors;
    colors.reserve(static_cast<qsizetype>(hex.size()));
    for (const char *value : hex) {
        colors.append(QColor(QLatin1String(value)));
    }
    return colors;
}

/**
 * @brief 把已改名的配色 id 映射到现名。
 *
 * 0.2.5 及更早版本的 Shellium 配色叫 termius_dark / termius_light，数据库里存的仍是旧 id。
 * 不映射的话这些用户升级后会被未知 id 校验静默打回“默认”，等于丢了自己选的配色。
 */
QString migrateLegacyColorSchemeId(const QString &id)
{
    if (id == QLatin1String("termius_dark")) {
        return QStringLiteral("shellium_dark");
    }
    if (id == QLatin1String("termius_light")) {
        return QStringLiteral("shellium_light");
    }
    return id;
}

/**
 * @brief 为尚未存过"搜索主机"快捷键的旧设置腾出默认键位。
 * @param shortcuts 已按库中值覆盖默认值的快捷键表，原地修改。
 * @param storedIds 库中实际存在记录的动作 id。
 *
 * 保存设置会把全部快捷键整表写库，所以 0.2.x 用户库里都存着 files.toggle=Ctrl+Shift+F。
 * 新增的 hosts.search 默认也是 Ctrl+Shift+F，不迁移的话 validate() 判定重复，会把用户的
 * 全部自定义快捷键整体打回默认。旧默认值与"用户恰好选了同一个键"无法区分，按旧默认处理；
 * 新默认键位若已被用户占用则让 files.toggle 留空，hosts.search 仍冲突时同样留空，交给用户
 * 在设置里自行指定，绝不牵连其他动作。
 */
void migrateShortcutsWithoutHostSearch(QHash<QString, QKeySequence> *shortcuts,
                                       const QSet<QString> &storedIds)
{
    const QString hostSearchId = QStringLiteral("hosts.search");
    const QString filesToggleId = QStringLiteral("files.toggle");
    if (storedIds.contains(hostSearchId)) {
        return;
    }
    const QHash<QString, QKeySequence> defaults = TerminalSettings::defaultShortcuts();
    const auto takenByOther = [shortcuts](const QKeySequence &sequence, const QString &exceptId) {
        for (auto iterator = shortcuts->cbegin(); iterator != shortcuts->cend(); ++iterator) {
            if (iterator.key() != exceptId && iterator.value() == sequence) {
                return true;
            }
        }
        return false;
    };
    if (storedIds.contains(filesToggleId)
        && shortcuts->value(filesToggleId) == QKeySequence(QStringLiteral("Ctrl+Shift+F"))) {
        const QKeySequence filesDefault = defaults.value(filesToggleId);
        shortcuts->insert(filesToggleId,
                          takenByOther(filesDefault, filesToggleId) ? QKeySequence{} : filesDefault);
    }
    const QKeySequence hostSearchDefault = defaults.value(hostSearchId);
    shortcuts->insert(hostSearchId,
                      takenByOther(hostSearchDefault, hostSearchId) ? QKeySequence{}
                                                                    : hostSearchDefault);
}

} // namespace

QHash<QString, QKeySequence> TerminalSettings::defaultShortcuts()
{
    return {
        {QStringLiteral("connection.new"), QKeySequence(QStringLiteral("Ctrl+Shift+N"))},
        {QStringLiteral("terminal.local"), QKeySequence(QStringLiteral("Ctrl+Shift+L"))},
        {QStringLiteral("tab.new"), QKeySequence(QStringLiteral("Ctrl+T"))},
        {QStringLiteral("tab.close"), QKeySequence(QStringLiteral("Ctrl+W"))},
        // Shift+Cmd+F 让给全局"搜索主机"；远程文件管理改用 Shift+Cmd+O（Open files）。
        {QStringLiteral("hosts.search"), QKeySequence(QStringLiteral("Ctrl+Shift+F"))},
        {QStringLiteral("files.toggle"), QKeySequence(QStringLiteral("Ctrl+Shift+O"))},
        {QStringLiteral("vault.lock"), QKeySequence(QStringLiteral("Ctrl+Shift+K"))},
        {QStringLiteral("terminal.search"), QKeySequence::Find},
        {QStringLiteral("terminal.export"), QKeySequence(QStringLiteral("Ctrl+Shift+E"))},
        {QStringLiteral("command.palette"), QKeySequence(QStringLiteral("Ctrl+Shift+P"))},
        // macOS 的 Ctrl 即 Command，Cmd+K / Cmd+J 不会与终端按键冲突；其他平台的裸 Ctrl+K
        // 是 readline 的删到行尾、Ctrl+J 是换行，被应用快捷键截走会破坏 Shell 编辑，因此
        // 改用 Ctrl+Alt 组合。
#if defined(Q_OS_MACOS)
        {QStringLiteral("snippets.quick"), QKeySequence(QStringLiteral("Ctrl+K"))},
        {QStringLiteral("history.quick"), QKeySequence(QStringLiteral("Ctrl+J"))},
#else
        {QStringLiteral("snippets.quick"), QKeySequence(QStringLiteral("Ctrl+Alt+K"))},
        {QStringLiteral("history.quick"), QKeySequence(QStringLiteral("Ctrl+Alt+J"))},
#endif
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
        QStringLiteral("hosts.search"),
        QStringLiteral("files.toggle"),
        QStringLiteral("vault.lock"),
        QStringLiteral("terminal.search"),
        QStringLiteral("terminal.export"),
        QStringLiteral("command.palette"),
        QStringLiteral("snippets.quick"),
        QStringLiteral("history.quick"),
        QStringLiteral("settings.open"),
    };
}

QString TerminalSettings::shortcutDisplayName(const QString &id)
{
    // 不能用 static 缓存：tr() 只会在首次调用时按当时语言求值一次，之后切换语言这里
    // 永远返回旧译文，快捷键设置页的动作名就再也不跟随语言（设置页每次重译都会重新
    // 调用本函数，10 条目重建一个局部 QHash 的开销可忽略）。
    const QHash<QString, QString> names = {
        {QStringLiteral("connection.new"), QObject::tr("Connect to Server")},
        {QStringLiteral("terminal.local"), QObject::tr("Open a local terminal")},
        {QStringLiteral("tab.new"), QObject::tr("New tab")},
        {QStringLiteral("tab.close"), QObject::tr("Close tab")},
        {QStringLiteral("hosts.search"), QObject::tr("Search hosts")},
        {QStringLiteral("files.toggle"), QObject::tr("Show/hide remote files")},
        {QStringLiteral("vault.lock"), QObject::tr("Lock credential vault")},
        {QStringLiteral("terminal.search"), QObject::tr("Search terminal output")},
        {QStringLiteral("terminal.export"), QObject::tr("Export terminal output")},
        {QStringLiteral("command.palette"), QObject::tr("Open the command palette")},
        {QStringLiteral("snippets.quick"), QObject::tr("Quick run snippets")},
        {QStringLiteral("history.quick"), QObject::tr("Quick run command history")},
        {QStringLiteral("settings.open"), QObject::tr("Open settings")},
    };
    return names.value(id, id);
}

TerminalSettings TerminalSettings::normalized() const
{
    TerminalSettings result = *this;
    // 只做格式规范：把空白、大小写不规范或明显非法的语言码收敛为 "system"，真正“是否有对应
    // 翻译”的判断交给 LocalizationService::resolve()，避免设置层反向依赖翻译器与 QApplication。
    result.language = normalizeLanguageTag(result.language);
    result.fontFamily = result.fontFamily.trimmed();
    // 未知配色 id 一律回退默认，避免旧版本、手工改库或插件写入的值让终端拿到空调色板。
    result.colorSchemeId = migrateLegacyColorSchemeId(result.colorSchemeId.trimmed());
    if (themePreset(result.colorSchemeId) == nullptr) {
        result.colorSchemeId = QStringLiteral("default");
    }
    result.fontPointSize = std::clamp(result.fontPointSize, 6, 72);
    result.lineHeightPercent = std::clamp(result.lineHeightPercent, 80, 250);
    result.letterSpacing = std::clamp(result.letterSpacing, -2.0, 10.0);
    result.scrollbackLines = std::clamp(result.scrollbackLines, 100, 1000000);
    result.scrollbackMaxMegabytes = std::clamp(result.scrollbackMaxMegabytes, 1, 512);
    result.historyMinCommandLength = std::clamp(result.historyMinCommandLength, 0, 256);
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
                *errorMessage = QObject::tr("Shortcut %1 is assigned to both '%2' and '%3'")
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
    // 顺序即设置界面的展示顺序：Shellium 自有配色排最前，其后是“默认”与各上游主题。
    // “默认”项的颜色只是占位（真正的取值由 effectivePalette() 按主题解析），因此填的是
    // 内置深色配色，保证任何直接读取该项 palette 的调用方也能拿到一套可用颜色。
    // 每套主题都写全 16 色：上游主题照搬其发布值，自拟主题按正文对比度 ≥ 7:1、彩色
    // ≥ 3:1（相对背景）调过，避免某一号色在该主题背景上读不出来。
    static const QList<TerminalThemePreset> presets = {
        {QStringLiteral("shellium_dark"),
         QStringLiteral("Shellium Dark"),
         true,
         {QColor(QStringLiteral("#11151d")), QColor(QStringLiteral("#d4dbe5")),
          QColor(QStringLiteral("#2bd981")), QColor(QStringLiteral("#26364a")),
          QColor(QStringLiteral("#5aa9ff")),
          // Shellium 自有配色：底色与应用深色界面同一色系，正文用偏冷的柔白而不是品牌绿——整屏
          // 绿字长时间阅读很累，品牌绿只留给光标和 ANSI 绿。
          // 4 号普通蓝和 12 号亮蓝统一使用 #015fff。常见 DIR_COLORS 使用 `01;34`，其中粗体
          // 可能会把蓝色映射到 12 号亮色槽；两个色槽保持一致可避免同类目录名因远端粗体策略不同
          // 而出现两种蓝色。
          ansiFrom({"#1c2230", "#f2727a", "#2bd981", "#f0c674",
                    "#015fff", "#c792ea", "#4fd1c5", "#c5ccd6",
                    "#4b566b", "#ff8f95", "#6be8a8", "#ffd98e",
                    "#015fff", "#dbb0f5", "#7fe3d8", "#f4f6fa"})}},
        {QStringLiteral("shellium_light"),
         QStringLiteral("Shellium Light"),
         false,
         {QColor(QStringLiteral("#f7f9fb")), QColor(QStringLiteral("#1f2733")),
          QColor(QStringLiteral("#13985c")), QColor(QStringLiteral("#cfe3f7")),
          QColor(QStringLiteral("#0b62d6")),
          // 浅色版用近白冷灰底，品牌绿压深一档（#13985c）才能在白底上达到正文对比度。
          ansiFrom({"#1f2733", "#d03b3b", "#13985c", "#a06a00",
                    "#2f6fd0", "#8a4fc4", "#0f8291", "#6b7684",
                    "#5a6575", "#e5534b", "#17a063", "#b8800a",
                    "#4a86e8", "#a265d8", "#1a9aa8", "#111821"})}},
        {QStringLiteral("default"),
         QObject::tr("Default"),
         true,
         {QColor(QStringLiteral("#111318")), QColor(QStringLiteral("#e6e6e6")),
          QColor(QStringLiteral("#f6c85f")), QColor(QStringLiteral("#34598a")),
          QColor(QStringLiteral("#66b3ff"))}},
        {QStringLiteral("flexoki_dark"),
         QStringLiteral("Flexoki Dark"),
         true,
         {QColor(QStringLiteral("#100f0f")), QColor(QStringLiteral("#cecdc3")),
          QColor(QStringLiteral("#879a39")), QColor(QStringLiteral("#282726")),
          QColor(QStringLiteral("#4385be")),
          ansiFrom({"#100f0f", "#d14d41", "#879a39", "#d0a215",
                    "#4385be", "#ce5d97", "#3aa99f", "#cecdc3",
                    "#575653", "#e8705f", "#a0af54", "#e0b33c",
                    "#66a0d8", "#e07bb0", "#5abfb4", "#fffcf0"})}},
        {QStringLiteral("flexoki_light"),
         QStringLiteral("Flexoki Light"),
         false,
         {QColor(QStringLiteral("#fffcf0")), QColor(QStringLiteral("#100f0f")),
          QColor(QStringLiteral("#879a39")), QColor(QStringLiteral("#e6e4d9")),
          QColor(QStringLiteral("#205ea6")),
          ansiFrom({"#100f0f", "#af3029", "#66800b", "#ad8301",
                    "#205ea6", "#a02f6f", "#24837b", "#6f6e69",
                    "#575653", "#d14d41", "#879a39", "#d0a215",
                    "#4385be", "#ce5d97", "#3aa99f", "#1c1b1a"})}},
        {QStringLiteral("kanagawa_wave"),
         QStringLiteral("Kanagawa Wave"),
         true,
         {QColor(QStringLiteral("#1f1f28")), QColor(QStringLiteral("#dcd7ba")),
          QColor(QStringLiteral("#c8c093")), QColor(QStringLiteral("#2d4f67")),
          QColor(QStringLiteral("#7e9cd8")),
          ansiFrom({"#090618", "#c34043", "#76946a", "#c0a36e",
                    "#7e9cd8", "#957fb8", "#6a9589", "#c8c093",
                    "#727169", "#e82424", "#98bb6c", "#e6c384",
                    "#7fb4ca", "#938aa9", "#7aa89f", "#dcd7ba"})}},
        {QStringLiteral("kanagawa_dragon"),
         QStringLiteral("Kanagawa Dragon"),
         true,
         {QColor(QStringLiteral("#181616")), QColor(QStringLiteral("#c5c9c5")),
          QColor(QStringLiteral("#c8c093")), QColor(QStringLiteral("#2d4f67")),
          QColor(QStringLiteral("#8ba4b0")),
          ansiFrom({"#0d0c0c", "#c4746e", "#8a9a7b", "#c4b28a",
                    "#8ba4b0", "#a292a3", "#8ea4a2", "#c8c093",
                    "#a6a69c", "#e46876", "#87a987", "#e6c384",
                    "#7fb4ca", "#938aa9", "#7aa89f", "#c5c9c5"})}},
        {QStringLiteral("kanagawa_lotus"),
         QStringLiteral("Kanagawa Lotus"),
         false,
         {QColor(QStringLiteral("#f2ecbc")), QColor(QStringLiteral("#545464")),
          QColor(QStringLiteral("#43436c")), QColor(QStringLiteral("#dcd7ba")),
          QColor(QStringLiteral("#4d699b")),
          ansiFrom({"#1f1f28", "#c84053", "#6f894e", "#77713f",
                    "#4d699b", "#b35b79", "#597b75", "#545464",
                    "#8a8980", "#d7474b", "#6e915f", "#836f4a",
                    "#6693bf", "#624c83", "#5e857a", "#43436c"})}},
        {QStringLiteral("hacker_blue"),
         QStringLiteral("Hacker Blue"),
         true,
         {QColor(QStringLiteral("#030814")), QColor(QStringLiteral("#29b6ff")),
          QColor(QStringLiteral("#29b6ff")), QColor(QStringLiteral("#0b2a44")),
          QColor(QStringLiteral("#7fdcff")),
          ansiFrom({"#08203a", "#1c72c4", "#2f9fe0", "#56b8f0",
                    "#3f8fdd", "#6ac4ff", "#86d4ff", "#29b6ff",
                    "#10406e", "#3f9ce0", "#57bdf5", "#8ad4ff",
                    "#6bb6ff", "#9fdcff", "#b6e6ff", "#d8f2ff"})}},
        {QStringLiteral("hacker_green"),
         QStringLiteral("Hacker Green"),
         true,
         {QColor(QStringLiteral("#001400")), QColor(QStringLiteral("#22dd22")),
          QColor(QStringLiteral("#33ff33")), QColor(QStringLiteral("#0b3d0b")),
          QColor(QStringLiteral("#66ff88")),
          ansiFrom({"#0b3d0b", "#1aa61a", "#22dd22", "#4de84d",
                    "#2fc94f", "#5cff8f", "#8affb0", "#33ff33",
                    "#146414", "#3fd63f", "#5cff5c", "#86ff86",
                    "#6dffa0", "#a6ffc4", "#c2ffd8", "#e0ffe8"})}},
        {QStringLiteral("hacker_red"),
         QStringLiteral("Hacker Red"),
         true,
         {QColor(QStringLiteral("#150000")), QColor(QStringLiteral("#d22b2b")),
          QColor(QStringLiteral("#ff3b3b")), QColor(QStringLiteral("#3d0b0b")),
          QColor(QStringLiteral("#ff7a7a")),
          ansiFrom({"#3d0b0b", "#a61f1f", "#d22b2b", "#ef4b4b",
                    "#c93a3a", "#ff6b6b", "#ff9090", "#ff3b3b",
                    "#641414", "#d63f3f", "#ff5c5c", "#ff8686",
                    "#ff7a7a", "#ffa6a6", "#ffc2c2", "#ffe0e0"})}},
        {QStringLiteral("everforest_dark"),
         QStringLiteral("Everforest Dark"),
         true,
         {QColor(QStringLiteral("#2d353b")), QColor(QStringLiteral("#d3c6aa")),
          QColor(QStringLiteral("#a7c080")), QColor(QStringLiteral("#475258")),
          QColor(QStringLiteral("#7fbbb3")),
          ansiFrom({"#343f44", "#e67e80", "#a7c080", "#dbbc7f",
                    "#7fbbb3", "#d699b6", "#83c092", "#d3c6aa",
                    "#859289", "#f08a8c", "#b8d090", "#e8c88c",
                    "#8fcbc3", "#e3a8c4", "#93d0a2", "#e6dcc2"})}},
        {QStringLiteral("everforest_light"),
         QStringLiteral("Everforest Light"),
         false,
         {QColor(QStringLiteral("#fdf6e3")), QColor(QStringLiteral("#5c6a72")),
          QColor(QStringLiteral("#8da101")), QColor(QStringLiteral("#e6e2cc")),
          QColor(QStringLiteral("#3a94c5")),
          ansiFrom({"#5c6a72", "#f85552", "#8da101", "#dfa000",
                    "#3a94c5", "#df69ba", "#35a77c", "#939f91",
                    "#829181", "#e04040", "#6f8000", "#b07800",
                    "#3a94c5", "#c24f9e", "#2a8a65", "#343f44"})}},
        {QStringLiteral("night_owl"),
         QStringLiteral("Night Owl"),
         true,
         {QColor(QStringLiteral("#011627")), QColor(QStringLiteral("#d6deeb")),
          QColor(QStringLiteral("#80a4c2")), QColor(QStringLiteral("#1d3b53")),
          QColor(QStringLiteral("#82aaff")),
          ansiFrom({"#011627", "#ef5350", "#22da6e", "#addb67",
                    "#82aaff", "#c792ea", "#21c7a8", "#d6deeb",
                    "#575656", "#ef5350", "#22da6e", "#ffeb95",
                    "#82aaff", "#c792ea", "#7fdbca", "#ffffff"})}},
        {QStringLiteral("light_owl"),
         QStringLiteral("Light Owl"),
         false,
         {QColor(QStringLiteral("#fbfbfb")), QColor(QStringLiteral("#403f53")),
          QColor(QStringLiteral("#403f53")), QColor(QStringLiteral("#e0e0e0")),
          QColor(QStringLiteral("#4876d6")),
          ansiFrom({"#403f53", "#de3d3b", "#08916a", "#e0af02",
                    "#288ed7", "#d6438a", "#2aa298", "#989fb1",
                    "#7a8181", "#de3d3b", "#08916a", "#a67c00",
                    "#288ed7", "#d6438a", "#2aa298", "#27263a"})}},
        {QStringLiteral("aura"),
         QStringLiteral("Aura"),
         true,
         {QColor(QStringLiteral("#15141b")), QColor(QStringLiteral("#edecee")),
          QColor(QStringLiteral("#a277ff")), QColor(QStringLiteral("#29263b")),
          QColor(QStringLiteral("#61ffca")),
          ansiFrom({"#110f18", "#ff6767", "#61ffca", "#ffca85",
                    "#a277ff", "#f694ff", "#82e2ff", "#edecee",
                    "#4d4d4d", "#ff8080", "#7dffd4", "#ffd9a8",
                    "#b994ff", "#f9b3ff", "#a3eaff", "#ffffff"})}},
        {QStringLiteral("rose_pine"),
         QStringLiteral("Rosé Pine"),
         true,
         {QColor(QStringLiteral("#191724")), QColor(QStringLiteral("#e0def4")),
          QColor(QStringLiteral("#ebbcba")), QColor(QStringLiteral("#403d52")),
          QColor(QStringLiteral("#9ccfd8")),
          ansiFrom({"#26233a", "#eb6f92", "#31748f", "#f6c177",
                    "#9ccfd8", "#c4a7e7", "#ebbcba", "#e0def4",
                    "#6e6a86", "#eb6f92", "#31748f", "#f6c177",
                    "#9ccfd8", "#c4a7e7", "#ebbcba", "#e0def4"})}},
        {QStringLiteral("rose_pine_moon"),
         QStringLiteral("Rosé Pine Moon"),
         true,
         {QColor(QStringLiteral("#232136")), QColor(QStringLiteral("#e0def4")),
          QColor(QStringLiteral("#ea9a97")), QColor(QStringLiteral("#44415a")),
          QColor(QStringLiteral("#9ccfd8")),
          ansiFrom({"#393552", "#eb6f92", "#3e8fb0", "#f6c177",
                    "#9ccfd8", "#c4a7e7", "#ea9a97", "#e0def4",
                    "#6e6a86", "#eb6f92", "#3e8fb0", "#f6c177",
                    "#9ccfd8", "#c4a7e7", "#ea9a97", "#e0def4"})}},
        {QStringLiteral("rose_pine_dawn"),
         QStringLiteral("Rosé Pine Dawn"),
         false,
         {QColor(QStringLiteral("#faf4ed")), QColor(QStringLiteral("#575279")),
          QColor(QStringLiteral("#d7827e")), QColor(QStringLiteral("#dfdad9")),
          QColor(QStringLiteral("#56949f")),
          ansiFrom({"#575279", "#b4637a", "#286983", "#ea9d34",
                    "#56949f", "#907aa9", "#d7827e", "#9893a5",
                    "#797593", "#b4637a", "#286983", "#ea9d34",
                    "#56949f", "#907aa9", "#d7827e", "#26233a"})}},
        {QStringLiteral("cobalt2"),
         QStringLiteral("Cobalt2"),
         true,
         {QColor(QStringLiteral("#193549")), QColor(QStringLiteral("#ffffff")),
          QColor(QStringLiteral("#ffc600")), QColor(QStringLiteral("#0050a4")),
          QColor(QStringLiteral("#0088ff")),
          ansiFrom({"#000000", "#ff628c", "#3ad900", "#ffc600",
                    "#0088ff", "#fb94ff", "#80fcff", "#e1efff",
                    "#0050a4", "#ff8aa8", "#6ae65a", "#ffe066",
                    "#4dabff", "#fcb4ff", "#a6fdff", "#ffffff"})}},
        {QStringLiteral("octocat_dark"),
         QStringLiteral("Octocat Dark"),
         true,
         {QColor(QStringLiteral("#0d1117")), QColor(QStringLiteral("#c9d1d9")),
          QColor(QStringLiteral("#58a6ff")), QColor(QStringLiteral("#1f3a5f")),
          QColor(QStringLiteral("#58a6ff")),
          ansiFrom({"#484f58", "#ff7b72", "#3fb950", "#d29922",
                    "#58a6ff", "#bc8cff", "#39c5cf", "#b1bac4",
                    "#6e7681", "#ffa198", "#56d364", "#e3b341",
                    "#79c0ff", "#d2a8ff", "#56d4dd", "#f0f6fc"})}},
        {QStringLiteral("octocat_light"),
         QStringLiteral("Octocat Light"),
         false,
         {QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#24292f")),
          QColor(QStringLiteral("#0969da")), QColor(QStringLiteral("#b6d7ff")),
          QColor(QStringLiteral("#0969da")),
          ansiFrom({"#24292f", "#cf222e", "#116329", "#4d2d00",
                    "#0969da", "#8250df", "#1b7c83", "#6e7781",
                    "#57606a", "#a40e26", "#1a7f37", "#633c01",
                    "#218bff", "#a475f9", "#3192aa", "#1f2328"})}},
        {QStringLiteral("ayu_dark"),
         QStringLiteral("Ayu Dark"),
         true,
         {QColor(QStringLiteral("#0b0e14")), QColor(QStringLiteral("#bfbdb6")),
          QColor(QStringLiteral("#e6b450")), QColor(QStringLiteral("#273747")),
          QColor(QStringLiteral("#59c2ff")),
          ansiFrom({"#01060e", "#ea6c73", "#7fd962", "#f9af4f",
                    "#53bdfa", "#cda1fa", "#90e1c6", "#c7c7c7",
                    "#686868", "#f07178", "#aad94c", "#ffb454",
                    "#59c2ff", "#d2a6ff", "#95e6cb", "#ffffff"})}},
        {QStringLiteral("ayu_light"),
         QStringLiteral("Ayu Light"),
         false,
         {QColor(QStringLiteral("#fcfcfc")), QColor(QStringLiteral("#5c6166")),
          QColor(QStringLiteral("#ffaa33")), QColor(QStringLiteral("#d1e4f4")),
          QColor(QStringLiteral("#399ee6")),
          ansiFrom({"#5c6166", "#e65050", "#6cbf43", "#e59a23",
                    "#3199e1", "#9e75c7", "#46ba94", "#8a9199",
                    "#787b80", "#d94f4f", "#5f8f00", "#b87300",
                    "#2b7fc2", "#a37acc", "#2f9a7a", "#2a2e33"})}},
        {QStringLiteral("cyberpunk"),
         QStringLiteral("Cyberpunk"),
         true,
         {QColor(QStringLiteral("#241734")), QColor(QStringLiteral("#e6e1ff")),
          QColor(QStringLiteral("#00f0b5")), QColor(QStringLiteral("#3b2a57")),
          QColor(QStringLiteral("#00e5ff")),
          ansiFrom({"#1a1026", "#ff4f8b", "#00f0b5", "#ffd166",
                    "#4fb3ff", "#d86bff", "#00e5ff", "#e6e1ff",
                    "#5a4878", "#ff7aa8", "#5cffd1", "#ffe08f",
                    "#7cc8ff", "#e59aff", "#66f0ff", "#ffffff"})}},
        {QStringLiteral("cyberpunk_scarlet"),
         QStringLiteral("Cyberpunk Scarlet"),
         true,
         {QColor(QStringLiteral("#120309")), QColor(QStringLiteral("#f5c6d3")),
          QColor(QStringLiteral("#ff2e63")), QColor(QStringLiteral("#3a0d1f")),
          QColor(QStringLiteral("#00ffb3")),
          // 正文不再直接用猩红：整屏高饱和红字刺眼，红色留给光标和 ANSI 红承担主题辨识度。
          ansiFrom({"#1f0612", "#ff2e63", "#00ffb3", "#ffcc4d",
                    "#4fa8ff", "#ff5cc8", "#3ff5e0", "#f5c6d3",
                    "#5c2238", "#ff6b8f", "#5cffcc", "#ffdd80",
                    "#80c0ff", "#ff8ad8", "#80fff0", "#fff0f4"})}},
        {QStringLiteral("romania_night"),
         QStringLiteral("Romania Night"),
         true,
         {QColor(QStringLiteral("#10222b")), QColor(QStringLiteral("#f2e8cf")),
          QColor(QStringLiteral("#fcbf49")), QColor(QStringLiteral("#1d3d4a")),
          QColor(QStringLiteral("#77bfa3")),
          ansiFrom({"#0b1a21", "#e85d4a", "#77bfa3", "#fcbf49",
                    "#5fa8d3", "#c98bb9", "#62c5c1", "#f2e8cf",
                    "#4a6773", "#f07f6e", "#98d4bb", "#ffd27a",
                    "#88c2e6", "#dcaad0", "#8ad8d4", "#fffaf0"})}},
        {QStringLiteral("romania_day"),
         QStringLiteral("Romania Day"),
         false,
         {QColor(QStringLiteral("#f7e7e4")), QColor(QStringLiteral("#5a2626")),
          QColor(QStringLiteral("#c8553d")), QColor(QStringLiteral("#efd3ce")),
          QColor(QStringLiteral("#1f7a8c")),
          ansiFrom({"#3a2323", "#b3322a", "#3d7a3a", "#9a6a00",
                    "#1f5f8c", "#8c3a7a", "#1f7a8c", "#7a6262",
                    "#6b5050", "#c2392f", "#3f8a3b", "#9c6f00",
                    "#2f77aa", "#a64f93", "#1f7f92", "#2a1515"})}},
        {QStringLiteral("aubergine"),
         QStringLiteral("Aubergine"),
         true,
         {QColor(QStringLiteral("#2b0b2b")), QColor(QStringLiteral("#efe1f0")),
          QColor(QStringLiteral("#7fe1a1")), QColor(QStringLiteral("#4f1f56")),
          QColor(QStringLiteral("#c58af9")),
          ansiFrom({"#1e061e", "#ff6b81", "#7fe1a1", "#f6d27a",
                    "#8fb5ff", "#c58af9", "#6fd6d6", "#efe1f0",
                    "#7a4f7e", "#ff92a3", "#a2ecbd", "#fbe3a5",
                    "#b3ccff", "#dbb2ff", "#98e6e6", "#ffffff"})}},
        {QStringLiteral("peach_fresh"),
         QStringLiteral("Peach Fresh"),
         false,
         {QColor(QStringLiteral("#fbe6d6")), QColor(QStringLiteral("#4a2416")),
          QColor(QStringLiteral("#d0603a")), QColor(QStringLiteral("#f3c4a7")),
          QColor(QStringLiteral("#2e6e7e")),
          // 底色从高饱和的蜜桃橙降为淡桃色：原值整屏铺开刺眼，正文对比度也不足。
          ansiFrom({"#4a2416", "#b8321f", "#4c7a2e", "#94600a",
                    "#2b5f8a", "#93407a", "#2e6e7e", "#8a6a5a",
                    "#7a5444", "#d4472f", "#5f913c", "#ad7413",
                    "#3874a6", "#ab5592", "#3a8696", "#2e140a"})}},
        {QStringLiteral("nineteen84_dark"),
         QStringLiteral("1984 Dark"),
         true,
         {QColor(QStringLiteral("#101033")), QColor(QStringLiteral("#d8d8e8")),
          QColor(QStringLiteral("#8ae234")), QColor(QStringLiteral("#2a2a5a")),
          QColor(QStringLiteral("#7a88ff")),
          ansiFrom({"#1a1a40", "#ff5c8a", "#8ae234", "#ffd866",
                    "#7a88ff", "#d78cff", "#5ce1e6", "#d8d8e8",
                    "#525288", "#ff85a6", "#a8f060", "#ffe591",
                    "#a0aaff", "#e6b0ff", "#8aedf0", "#ffffff"})}},
        {QStringLiteral("nineteen84_light"),
         QStringLiteral("1984 Light"),
         false,
         {QColor(QStringLiteral("#e6ebf4")), QColor(QStringLiteral("#2b2f45")),
          QColor(QStringLiteral("#2f8f4e")), QColor(QStringLiteral("#c7d1e6")),
          QColor(QStringLiteral("#3355cc")),
          ansiFrom({"#2b2f45", "#c2334d", "#2f8f4e", "#8f6a00",
                    "#3355cc", "#8e3fb0", "#147f8f", "#5d6480",
                    "#4c5270", "#d94a63", "#2b8648", "#9a7300",
                    "#4a6be0", "#a656c8", "#137888", "#151827"})}},
        {QStringLiteral("winter_night"),
         QStringLiteral("Winter Night"),
         true,
         {QColor(QStringLiteral("#0e2233")), QColor(QStringLiteral("#e8e2cf")),
          QColor(QStringLiteral("#f0c674")), QColor(QStringLiteral("#1f4058")),
          QColor(QStringLiteral("#8fc1e3")),
          ansiFrom({"#0a1a28", "#e57373", "#9ccc8c", "#f0c674",
                    "#8fc1e3", "#c3a6e0", "#7fcfcf", "#e8e2cf",
                    "#4a6680", "#f09a9a", "#b8deaa", "#f6d898",
                    "#b0d4ee", "#d6c0ec", "#a2dede", "#ffffff"})}},
        {QStringLiteral("winter_day"),
         QStringLiteral("Winter Day"),
         false,
         {QColor(QStringLiteral("#f0eadc")), QColor(QStringLiteral("#33403a")),
          QColor(QStringLiteral("#4e7a63")), QColor(QStringLiteral("#dcd3be")),
          QColor(QStringLiteral("#2f6690")),
          ansiFrom({"#33403a", "#a8423a", "#4e7a63", "#8a6a1c",
                    "#2f6690", "#7d4f8a", "#2f7d7d", "#6b7068",
                    "#59665f", "#c4574e", "#4f8166", "#8f6f1c",
                    "#3d7cab", "#9563a3", "#2f8080", "#1c2420"})}},
        {QStringLiteral("tokyo_night"),
         QStringLiteral("Tokyo Night"),
         true,
         {QColor(QStringLiteral("#1a1b26")), QColor(QStringLiteral("#c0caf5")),
          QColor(QStringLiteral("#c0caf5")), QColor(QStringLiteral("#283457")),
          QColor(QStringLiteral("#7aa2f7")),
          ansiFrom({"#15161e", "#f7768e", "#9ece6a", "#e0af68",
                    "#7aa2f7", "#bb9af7", "#7dcfff", "#a9b1d6",
                    "#414868", "#f7768e", "#9ece6a", "#e0af68",
                    "#7aa2f7", "#bb9af7", "#7dcfff", "#c0caf5"})}},
        {QStringLiteral("tokyo_day"),
         QStringLiteral("Tokyo Day"),
         false,
         {QColor(QStringLiteral("#e1e2e7")), QColor(QStringLiteral("#3760bf")),
          QColor(QStringLiteral("#3760bf")), QColor(QStringLiteral("#cdd3ef")),
          QColor(QStringLiteral("#2e7de9")),
          ansiFrom({"#3760bf", "#f52a65", "#587539", "#8c6c3e",
                    "#2e7de9", "#9854f1", "#007197", "#6172b0",
                    "#a1a6c5", "#f52a65", "#587539", "#8c6c3e",
                    "#2e7de9", "#9854f1", "#007197", "#343b58"})}},
        {QStringLiteral("catppuccin_latte"),
         QStringLiteral("Catppuccin Latte"),
         false,
         {QColor(QStringLiteral("#eff1f5")), QColor(QStringLiteral("#4c4f69")),
          QColor(QStringLiteral("#dc8a78")), QColor(QStringLiteral("#ccd0da")),
          QColor(QStringLiteral("#1e66f5")),
          ansiFrom({"#5c5f77", "#d20f39", "#40a02b", "#df8e1d",
                    "#1e66f5", "#ea76cb", "#179299", "#8c8fa1",
                    "#6c6f85", "#d20f39", "#40a02b", "#df8e1d",
                    "#1e66f5", "#ea76cb", "#179299", "#4c4f69"})}},
        {QStringLiteral("catppuccin_mocha"),
         QStringLiteral("Catppuccin Mocha"),
         true,
         {QColor(QStringLiteral("#1e1e2e")), QColor(QStringLiteral("#cdd6f4")),
          QColor(QStringLiteral("#f5e0dc")), QColor(QStringLiteral("#45475a")),
          QColor(QStringLiteral("#89b4fa")),
          ansiFrom({"#45475a", "#f38ba8", "#a6e3a1", "#f9e2af",
                    "#89b4fa", "#f5c2e7", "#94e2d5", "#bac2de",
                    "#585b70", "#f38ba8", "#a6e3a1", "#f9e2af",
                    "#89b4fa", "#f5c2e7", "#94e2d5", "#a6adc8"})}},
        {QStringLiteral("diwali"),
         QStringLiteral("Diwali"),
         true,
         {QColor(QStringLiteral("#2b1046")), QColor(QStringLiteral("#ffe8c2")),
          QColor(QStringLiteral("#ffb300")), QColor(QStringLiteral("#4a2575")),
          QColor(QStringLiteral("#ff7043")),
          ansiFrom({"#1e0a33", "#ff5a5f", "#9ccc65", "#ffb300",
                    "#7fa8ff", "#e07bff", "#4dd0c8", "#ffe8c2",
                    "#6a4a90", "#ff8a8c", "#b9e08a", "#ffd060",
                    "#a6c2ff", "#eca6ff", "#7fe0da", "#fff6e6"})}},
        {QStringLiteral("movember"),
         QStringLiteral("Movember"),
         true,
         {QColor(QStringLiteral("#241206")), QColor(QStringLiteral("#f3d9b1")),
          QColor(QStringLiteral("#e0a356")), QColor(QStringLiteral("#45281a")),
          QColor(QStringLiteral("#7fb069")),
          ansiFrom({"#1a0c03", "#d9634c", "#7fb069", "#e0a356",
                    "#7aa6c2", "#c28aa8", "#79b8a8", "#f3d9b1",
                    "#6a4a34", "#ec8670", "#9cc68a", "#f0bf7d",
                    "#9cc0d8", "#d6a8c0", "#9ad0c2", "#fff0dc"})}},
        {QStringLiteral("atom_one_dark"),
         QStringLiteral("Atom One Dark"),
         true,
         {QColor(QStringLiteral("#282c34")), QColor(QStringLiteral("#abb2bf")),
          QColor(QStringLiteral("#528bff")), QColor(QStringLiteral("#3e4451")),
          QColor(QStringLiteral("#61afef")),
          ansiFrom({"#3f4451", "#e06c75", "#98c379", "#e5c07b",
                    "#61afef", "#c678dd", "#56b6c2", "#abb2bf",
                    "#5c6370", "#ef7b84", "#a9d48a", "#f0cf8f",
                    "#7cc0f5", "#d48ee8", "#6cc8d3", "#e6e9ef"})}},
        {QStringLiteral("atom_one_light"),
         QStringLiteral("Atom One Light"),
         false,
         {QColor(QStringLiteral("#fafafa")), QColor(QStringLiteral("#383a42")),
          QColor(QStringLiteral("#526fff")), QColor(QStringLiteral("#e5e5e6")),
          QColor(QStringLiteral("#4078f2")),
          ansiFrom({"#383a42", "#e45649", "#50a14f", "#c18401",
                    "#4078f2", "#a626a4", "#0184bc", "#a0a1a7",
                    "#696c77", "#ca1243", "#3e8a3d", "#a87300",
                    "#2b62d9", "#8e1f8c", "#0f6fa0", "#202227"})}},
        {QStringLiteral("halloween"),
         QStringLiteral("Halloween"),
         true,
         {QColor(QStringLiteral("#1b0a20")), QColor(QStringLiteral("#f3d9b6")),
          QColor(QStringLiteral("#ff7518")), QColor(QStringLiteral("#3d1736")),
          QColor(QStringLiteral("#a3e048")),
          // 正文从南瓜橙改为暖米白：整屏橙字久看发花，橙色留给光标与 ANSI 黄。
          ansiFrom({"#120614", "#ff5f3f", "#a3e048", "#ff9a2e",
                    "#9a7bff", "#d36bff", "#5fd6b0", "#f3d9b6",
                    "#5c3460", "#ff8566", "#bdf074", "#ffb85c",
                    "#b6a0ff", "#e399ff", "#88e6c6", "#fff2e0"})}},
        {QStringLiteral("dia_de_muertos"),
         QStringLiteral("Dia De Muertos"),
         false,
         {QColor(QStringLiteral("#fff6e6")), QColor(QStringLiteral("#2e1a47")),
          QColor(QStringLiteral("#e4572e")), QColor(QStringLiteral("#f6e3c5")),
          QColor(QStringLiteral("#17696a")),
          ansiFrom({"#2e1a47", "#c7362a", "#3f7d20", "#a66a00",
                    "#2b5aa8", "#a3307f", "#17696a", "#7a6a80",
                    "#5a4670", "#e4572e", "#55962d", "#c28100",
                    "#3d72c4", "#bf4599", "#218587", "#1b0e2d"})}},
        {QStringLiteral("gruvbox_dark"),
         QStringLiteral("Gruvbox Dark"),
         true,
         {QColor(QStringLiteral("#282828")), QColor(QStringLiteral("#ebdbb2")),
          QColor(QStringLiteral("#ebdbb2")), QColor(QStringLiteral("#504945")),
          QColor(QStringLiteral("#83a598")),
          ansiFrom({"#282828", "#cc241d", "#98971a", "#d79921",
                    "#458588", "#b16286", "#689d6a", "#a89984",
                    "#928374", "#fb4934", "#b8bb26", "#fabd2f",
                    "#83a598", "#d3869b", "#8ec07c", "#ebdbb2"})}},
        {QStringLiteral("gruvbox_light"),
         QStringLiteral("Gruvbox Light"),
         false,
         {QColor(QStringLiteral("#fbf1c7")), QColor(QStringLiteral("#3c3836")),
          QColor(QStringLiteral("#3c3836")), QColor(QStringLiteral("#ebdbb2")),
          QColor(QStringLiteral("#076678")),
          ansiFrom({"#3c3836", "#cc241d", "#98971a", "#d79921",
                    "#458588", "#b16286", "#689d6a", "#7c6f64",
                    "#928374", "#9d0006", "#79740e", "#b57614",
                    "#076678", "#8f3f71", "#427b58", "#282828"})}},
        {QStringLiteral("material_dark"),
         QStringLiteral("Material Dark"),
         true,
         {QColor(QStringLiteral("#263238")), QColor(QStringLiteral("#eeffff")),
          QColor(QStringLiteral("#ffcc00")), QColor(QStringLiteral("#3a5058")),
          QColor(QStringLiteral("#80cbc4")),
          ansiFrom({"#37474f", "#f07178", "#c3e88d", "#ffcb6b",
                    "#82aaff", "#c792ea", "#89ddff", "#eeffff",
                    "#546e7a", "#ff8b92", "#ddffa7", "#ffe585",
                    "#9cc4ff", "#e1acff", "#a3f7ff", "#ffffff"})}},
        {QStringLiteral("material_light"),
         QStringLiteral("Material Light"),
         false,
         {QColor(QStringLiteral("#fafafa")), QColor(QStringLiteral("#546e7a")),
          QColor(QStringLiteral("#272727")), QColor(QStringLiteral("#cceae7")),
          QColor(QStringLiteral("#39adb5")),
          ansiFrom({"#546e7a", "#e53935", "#91b859", "#f6a434",
                    "#6182b8", "#7c4dff", "#39adb5", "#90a4ae",
                    "#78909c", "#e0303a", "#5a8a28", "#a86a00",
                    "#3d6bc8", "#7c4dff", "#2a8a92", "#272727"})}},
        {QStringLiteral("manhattan"),
         QStringLiteral("Manhattan"),
         true,
         {QColor(QStringLiteral("#0a0a0a")), QColor(QStringLiteral("#d6d6d6")),
          QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#2a2a2a")),
          QColor(QStringLiteral("#9e9e9e")),
          // 灰阶主题：16 色整体降饱和，避免一行彩色输出打破整屏的黑白基调。
          ansiFrom({"#1a1a1a", "#c96a6a", "#8fb08f", "#cdb882",
                    "#8aa4c2", "#b494b4", "#8fb5b5", "#d6d6d6",
                    "#5c5c5c", "#e08a8a", "#a8c8a8", "#e2cfa0",
                    "#a8bfd8", "#cbaecb", "#a8cccc", "#ffffff"})}},
        {QStringLiteral("plastic_world"),
         QStringLiteral("Plastic World"),
         false,
         {QColor(QStringLiteral("#fff0fa")), QColor(QStringLiteral("#4a1d5c")),
          QColor(QStringLiteral("#e03cc0")), QColor(QStringLiteral("#ffc9ef")),
          QColor(QStringLiteral("#00a3a3")),
          // 原先荧光粉底配白字整屏刺眼且对比度不足；改为淡粉底深紫字，荧光粉留给光标与选区。
          ansiFrom({"#4a1d5c", "#e0245e", "#3a9a2e", "#b07800",
                    "#2f6fe0", "#c42fb0", "#0f9a9a", "#8a6a94",
                    "#7a4f8c", "#d6336b", "#2f8a25", "#9e6c00",
                    "#3a6fe6", "#e04fcc", "#138c8c", "#2a0d38"})}},
        {QStringLiteral("basic"),
         QStringLiteral("Basic"),
         false,
         {QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#000000")),
          QColor(QStringLiteral("#000000")), QColor(QStringLiteral("#b5d5ff")),
          QColor(QStringLiteral("#007700")),
          ansiFrom({"#000000", "#990000", "#00a600", "#999900",
                    "#0000b2", "#b200b2", "#00a6b2", "#7f7f7f",
                    "#666666", "#e50000", "#00a600", "#999900",
                    "#0000ff", "#e500e5", "#00a6b2", "#000000"})}},
        {QStringLiteral("homebrew"),
         QStringLiteral("Homebrew"),
         true,
         {QColor(QStringLiteral("#000000")), QColor(QStringLiteral("#2ee62e")),
          QColor(QStringLiteral("#33ff66")), QColor(QStringLiteral("#003300")),
          QColor(QStringLiteral("#33ff66")),
          ansiFrom({"#1a1a1a", "#e05050", "#2ee62e", "#e6e64d",
                    "#4d8cff", "#d966d9", "#33cccc", "#cccccc",
                    "#666666", "#ff7070", "#66ff66", "#ffff80",
                    "#80aaff", "#ff8cff", "#66e6e6", "#ffffff"})}},
        {QStringLiteral("grass"),
         QStringLiteral("Grass"),
         true,
         {QColor(QStringLiteral("#10603a")), QColor(QStringLiteral("#fff0a5")),
          QColor(QStringLiteral("#ffd54f")), QColor(QStringLiteral("#0a4527")),
          QColor(QStringLiteral("#ffd54f")),
          // 绿底上不能沿用通用表的绿与蓝：两者都会和底色糊在一起，整体抬亮成粉彩色。
          ansiFrom({"#073019", "#ffb0a8", "#b9f27c", "#ffd54f",
                    "#9fd3ff", "#ffa6e0", "#7ff0e0", "#fff0a5",
                    "#8fc9a3", "#ffc8c0", "#d4ff9e", "#ffe58a",
                    "#c2e4ff", "#ffc8ec", "#a8fff2", "#ffffff"})}},
        {QStringLiteral("man_page"),
         QStringLiteral("Man Page"),
         false,
         {QColor(QStringLiteral("#fef49c")), QColor(QStringLiteral("#000000")),
          QColor(QStringLiteral("#7f7f7f")), QColor(QStringLiteral("#e8d66c")),
          QColor(QStringLiteral("#0645ad")),
          ansiFrom({"#000000", "#b3261e", "#2e7d1f", "#806000",
                    "#1a4fa0", "#8a2a8a", "#0f7070", "#6b6346",
                    "#4d4d4d", "#d43a30", "#3a962a", "#9a7600",
                    "#2a66c4", "#a63ea6", "#1a8a8a", "#1a1a1a"})}},
        {QStringLiteral("novel"),
         QStringLiteral("Novel"),
         false,
         {QColor(QStringLiteral("#dfdbc3")), QColor(QStringLiteral("#3b2322")),
          QColor(QStringLiteral("#73635a")), QColor(QStringLiteral("#cfc9ae")),
          QColor(QStringLiteral("#0d6a8c")),
          ansiFrom({"#3b2322", "#a8322d", "#4f7a28", "#8a6a12",
                    "#2a5c8a", "#8a3f78", "#2a7a78", "#6e6255",
                    "#6b5a50", "#b23a33", "#567f2c", "#84650f",
                    "#3a72a6", "#a35290", "#2a7a78", "#1f1110"})}},
        {QStringLiteral("ocean"),
         QStringLiteral("Ocean"),
         true,
         {QColor(QStringLiteral("#224fbc")), QColor(QStringLiteral("#ffffff")),
          QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#1a3c8f")),
          QColor(QStringLiteral("#b3e5fc")),
          // 蓝底上的 ANSI 蓝必须抬到浅天蓝，否则目录名会直接消失在背景里。
          ansiFrom({"#0f2a6b", "#ffa6a6", "#9cf09c", "#ffe680",
                    "#b3d9ff", "#ffa6f0", "#80f0ff", "#e6eeff",
                    "#9db3ea", "#ffc4c4", "#bfffbf", "#fff0a6",
                    "#d6ebff", "#ffc8f6", "#a6f6ff", "#ffffff"})}},
        {QStringLiteral("pro"),
         QStringLiteral("Pro"),
         true,
         {QColor(QStringLiteral("#000000")), QColor(QStringLiteral("#f2f2f2")),
          QColor(QStringLiteral("#c7c7c7")), QColor(QStringLiteral("#414141")),
          QColor(QStringLiteral("#00c400")),
          ansiFrom({"#1a1a1a", "#e5534b", "#3fc43f", "#d9c43a",
                    "#4f8fff", "#d65cd6", "#33c4cc", "#c7c7c7",
                    "#666666", "#ff6e66", "#5fe05f", "#f0dc5a",
                    "#7aaaff", "#ef80ef", "#5ce0e6", "#ffffff"})}},
        {QStringLiteral("red_sands"),
         QStringLiteral("Red Sands"),
         true,
         {QColor(QStringLiteral("#7a251e")), QColor(QStringLiteral("#e6d9b8")),
          QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#5a1a14")),
          QColor(QStringLiteral("#e8b87a")),
          ansiFrom({"#3d120e", "#ff9a8a", "#b8d98f", "#f0c48a",
                    "#b0d0ee", "#f0b2d4", "#9fe0d6", "#e6d9b8",
                    "#c08a7f", "#ffb8ac", "#d0eab0", "#f8ddb5",
                    "#cde2f5", "#f6cce4", "#bfeee6", "#fff6e6"})}},
        {QStringLiteral("solarized_dark"),
         QStringLiteral("Solarized Dark"),
         true,
         {QColor(QStringLiteral("#002b36")), QColor(QStringLiteral("#839496")),
          QColor(QStringLiteral("#93a1a1")), QColor(QStringLiteral("#073642")),
          QColor(QStringLiteral("#268bd2")),
          ansiFrom({"#073642", "#dc322f", "#859900", "#b58900",
                    "#268bd2", "#d33682", "#2aa198", "#eee8d5",
                    "#002b36", "#cb4b16", "#586e75", "#657b83",
                    "#839496", "#6c71c4", "#93a1a1", "#fdf6e3"})}},
        {QStringLiteral("solarized_light"),
         QStringLiteral("Solarized Light"),
         false,
         {QColor(QStringLiteral("#fdf6e3")), QColor(QStringLiteral("#657b83")),
          QColor(QStringLiteral("#586e75")), QColor(QStringLiteral("#eee8d5")),
          QColor(QStringLiteral("#268bd2")),
          ansiFrom({"#073642", "#dc322f", "#859900", "#b58900",
                    "#268bd2", "#d33682", "#2aa198", "#93a1a1",
                    "#586e75", "#cb4b16", "#586e75", "#657b83",
                    "#839496", "#6c71c4", "#657b83", "#002b36"})}},
        {QStringLiteral("silver_aerogel"),
         QStringLiteral("Silver Aerogel"),
         false,
         {QColor(QStringLiteral("#c9c9c9")), QColor(QStringLiteral("#111111")),
          QColor(QStringLiteral("#3a3a3a")), QColor(QStringLiteral("#a8a8a8")),
          QColor(QStringLiteral("#003c8f")),
          // 原 #929292 中灰底与黑字对比度不足，抬到浅银灰，保留“银”的观感。
          ansiFrom({"#111111", "#a61d1d", "#1f6b1f", "#6b5300",
                    "#003c8f", "#7a1f7a", "#0f5f66", "#4d4d4d",
                    "#333333", "#c42a2a", "#2a852a", "#856800",
                    "#1f56b3", "#962e96", "#177680", "#000000"})}},
        {QStringLiteral("dracula"),
         QStringLiteral("Dracula"),
         true,
         {QColor(QStringLiteral("#282a36")), QColor(QStringLiteral("#f8f8f2")),
          QColor(QStringLiteral("#ff79c6")), QColor(QStringLiteral("#44475a")),
          QColor(QStringLiteral("#8be9fd")),
          ansiFrom({"#21222c", "#ff5555", "#50fa7b", "#f1fa8c",
                    "#bd93f9", "#ff79c6", "#8be9fd", "#f8f8f2",
                    "#6272a4", "#ff6e6e", "#69ff94", "#ffffa5",
                    "#d6acff", "#ff92df", "#a4ffff", "#ffffff"})}},
        {QStringLiteral("monokai"),
         QStringLiteral("Monokai"),
         true,
         {QColor(QStringLiteral("#272822")), QColor(QStringLiteral("#f8f8f2")),
          QColor(QStringLiteral("#f92672")), QColor(QStringLiteral("#49483e")),
          QColor(QStringLiteral("#66d9ef")),
          ansiFrom({"#272822", "#f92672", "#a6e22e", "#f4bf75",
                    "#66d9ef", "#ae81ff", "#a1efe4", "#f8f8f2",
                    "#75715e", "#ff5c94", "#bff259", "#ffd699",
                    "#8fe5f5", "#c4a3ff", "#c0f5ee", "#f9f8f5"})}},
        {QStringLiteral("nord"),
         QStringLiteral("Nord Dark"),
         true,
         {QColor(QStringLiteral("#2e3440")), QColor(QStringLiteral("#d8dee9")),
          QColor(QStringLiteral("#88c0d0")), QColor(QStringLiteral("#434c5e")),
          QColor(QStringLiteral("#81a1c1")),
          ansiFrom({"#3b4252", "#bf616a", "#a3be8c", "#ebcb8b",
                    "#81a1c1", "#b48ead", "#88c0d0", "#e5e9f0",
                    "#4c566a", "#bf616a", "#a3be8c", "#ebcb8b",
                    "#81a1c1", "#b48ead", "#8fbcbb", "#eceff4"})}},
        {QStringLiteral("nord_light"),
         QStringLiteral("Nord Light"),
         false,
         {QColor(QStringLiteral("#e5e9f0")), QColor(QStringLiteral("#2e3440")),
          QColor(QStringLiteral("#5e81ac")), QColor(QStringLiteral("#c2d0e7")),
          QColor(QStringLiteral("#5e81ac")),
          ansiFrom({"#3b4252", "#b74e58", "#5a7d3a", "#9a7a2a",
                    "#3e6a9e", "#8e5e87", "#3b8a9a", "#4c566a",
                    "#4c566a", "#b0545d", "#5f8240", "#8a6b1f",
                    "#4f74a3", "#96638f", "#3b7f8c", "#2e3440"})}},
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

QVector<QColor> TerminalSettings::defaultAnsiPalette(const bool dark)
{
    // 这两套值不属于任何一款上游主题，只承担一件事：让没有单独调过 16 色的预设也保持可读。
    // 深色一套整体抬到 VGA 之上（蓝色 #015fff 而非 #0000e0），浅色一套则压到白底之下。
    // 4 号蓝取 #015fff 是对着参考截图定的：`ls` 的目录名用的正是这一号色，它同时要在近黑
    // 背景上足够亮、又不能褪成青灰，把饱和度留住比单纯抬明度更接近参考图的观感。
    if (dark) {
        return ansiFrom({"#21252b", "#e06c75", "#98c379", "#e5c07b",
                         "#015fff", "#c678dd", "#56b6c2", "#c8ccd4",
                         "#5c6370", "#ff7b86", "#b5e08c", "#f2d68f",
                         "#4d8cff", "#dc98ee", "#74d3de", "#f2f4f8"});
    }
    return ansiFrom({"#2b2f36", "#c62828", "#2e7d32", "#8f6b00",
                     "#1a5fb4", "#8e24aa", "#00727c", "#5a6270",
                     "#4a515c", "#d8453f", "#3a8f3f", "#a67c00",
                     "#2b74d4", "#a13cc0", "#0d8a94", "#1b1f26"});
}

TerminalPalette TerminalSettings::effectivePalette() const
{
    // "default" 之外的配色都是搭配好的一整套固定主题（对应主流终端模拟器的同名预设），不随
    // 浅色/深色切换而改变，因此命中预设后直接返回，不看 effectiveThemeMode()。
    if (colorSchemeId != QLatin1String("default")) {
        if (const TerminalThemePreset *preset = themePreset(colorSchemeId)) {
            TerminalPalette palette = preset->palette;
            // 内置预设都写全了 16 色；这里仍按明暗补一份通用表兜底，防止将来新增的预设漏写
            // 时退回 libvterm 的 VGA 默认值。
            if (palette.ansi.size() != 16) {
                palette.ansi = defaultAnsiPalette(preset->dark);
            }
            return palette;
        }
    }

    if (effectiveThemeMode() == ThemeMode::Dark) {
        return {
            QColor(QStringLiteral("#111318")),
            QColor(QStringLiteral("#e6e6e6")),
            QColor(QStringLiteral("#f6c85f")),
            QColor(QStringLiteral("#34598a")),
            QColor(QStringLiteral("#66b3ff")),
            defaultAnsiPalette(true),
        };
    }
    return {
        QColor(QStringLiteral("#ffffff")),
        QColor(QStringLiteral("#20242b")),
        QColor(QStringLiteral("#0b57d0")),
        QColor(QStringLiteral("#b8d5ff")),
        QColor(QStringLiteral("#0057b8")),
        defaultAnsiPalette(false),
    };
}

bool TerminalSettings::operator==(const TerminalSettings &other) const
{
    return language == other.language
        && themeMode == other.themeMode
        && sidebarDensity == other.sidebarDensity
        && sidebarDoubleClickPrompt == other.sidebarDoubleClickPrompt
        && colorSchemeId == other.colorSchemeId
        && fontFamily == other.fontFamily
        && fontPointSize == other.fontPointSize
        && lineHeightPercent == other.lineHeightPercent
        && qFuzzyCompare(letterSpacing + 1.0, other.letterSpacing + 1.0)
        && cursorShape == other.cursorShape
        && cursorBlink == other.cursorBlink
        && scrollbackLines == other.scrollbackLines
        && scrollbackMaxMegabytes == other.scrollbackMaxMegabytes
        && audibleBell == other.audibleBell
        && commandAutoCompletion == other.commandAutoCompletion
        && historyMinCommandLength == other.historyMinCommandLength
        && multiLinePasteConfirmation == other.multiLinePasteConfirmation
        && automaticReconnect == other.automaticReconnect
        && vaultPasswordEnabled == other.vaultPasswordEnabled
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
            *errorMessage = tr("The settings service has no app database available");
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
            warnings.append(tr("The value '%2' of setting %1 is not an integer; the default has been restored").arg(key, *raw));
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
            warnings.append(tr("The value '%2' of setting %1 is not a boolean; the default has been restored").arg(key, *raw));
        }
    };

    if (const std::optional<QString> raw = readValue(kLanguageKey, &queryFailed);
        raw.has_value() && !queryFailed) {
        // normalized() 会把非法值收敛为 "system"，因此这里不再单独告警：数据库里存过的语言
        // 码要么合法，要么用户从没设过，两种情况都不该在启动时打扰用户。
        loaded.language = *raw;
    }
    if (queryFailed) {
        return false;
    }

    if (const std::optional<QString> raw = readValue(kThemeKey, &queryFailed);
        raw.has_value() && !queryFailed) {
        const std::optional<ThemeMode> parsed = parseTheme(*raw);
        if (parsed.has_value()) {
            loaded.themeMode = *parsed;
        } else {
            warnings.append(tr("Unknown theme '%1'; reverted to follow system").arg(*raw));
        }
    }
    if (queryFailed) {
        return false;
    }

    if (const std::optional<QString> raw = readValue(kColorSchemeKey, &queryFailed);
        raw.has_value() && !queryFailed) {
        const QString candidate = migrateLegacyColorSchemeId(raw->trimmed());
        if (TerminalSettings::themePreset(candidate) != nullptr) {
            loaded.colorSchemeId = candidate;
        } else {
            warnings.append(tr("Unknown terminal color scheme '%1'; reverted to the default scheme").arg(*raw));
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
            warnings.append(tr("Letter spacing '%1' is invalid; the default has been restored").arg(*raw));
        }
    }

    if (const std::optional<QString> raw = readValue(kCursorShapeKey, &queryFailed);
        raw.has_value() && !queryFailed) {
        const std::optional<TerminalCursorShape> parsed = parseCursorShape(*raw);
        if (parsed.has_value()) {
            loaded.cursorShape = *parsed;
        } else {
            warnings.append(tr("Unknown cursor shape '%1'; reverted to block cursor").arg(*raw));
        }
    }
    if (const std::optional<QString> raw = readValue(kSidebarDensityKey, &queryFailed);
        raw.has_value() && !queryFailed) {
        const std::optional<SidebarDensity> parsed = parseSidebarDensity(*raw);
        if (parsed.has_value()) {
            loaded.sidebarDensity = *parsed;
        } else {
            warnings.append(tr("Unknown sidebar density '%1'; reverted to relaxed").arg(*raw));
        }
    }
    if (queryFailed) {
        return false;
    }
    // 统一布尔解析同时兼容旧菜单写入的 0/1，并沿用读取失败与非法值处理。
    assignBoolean(kSidebarDoubleClickPromptKey, &loaded.sidebarDoubleClickPrompt);
    assignBoolean(kCursorBlinkKey, &loaded.cursorBlink);
    assignInteger(kScrollbackKey, &loaded.scrollbackLines);
    assignInteger(kScrollbackMaxMegabytesKey, &loaded.scrollbackMaxMegabytes);
    assignBoolean(kAudibleBellKey, &loaded.audibleBell);
    assignBoolean(kCommandAutoCompletionKey, &loaded.commandAutoCompletion);
    assignInteger(kHistoryMinCommandLengthKey, &loaded.historyMinCommandLength);
    assignBoolean(kMultiLinePasteConfirmationKey, &loaded.multiLinePasteConfirmation);
    assignBoolean(kAutomaticReconnectKey, &loaded.automaticReconnect);
    const std::optional<QString> vaultPasswordSetting =
        readValue(kVaultPasswordEnabledKey, &queryFailed);
    if (vaultPasswordSetting.has_value() && !queryFailed) {
        const std::optional<bool> parsed = parseBool(*vaultPasswordSetting);
        if (parsed.has_value()) {
            loaded.vaultPasswordEnabled = *parsed;
        } else {
            warnings.append(tr("The value '%2' of setting %1 is not a boolean; the default has been restored")
                                .arg(kVaultPasswordEnabledKey, *vaultPasswordSetting));
        }
    } else if (!queryFailed) {
        // 旧版本没有开关键，却可能已经创建了主密码保险库。此时必须保持原有保护和启动解锁
        // 行为，不能因为升级而把用户明确加密过的凭据静默降级为本机透明密钥模式。
        QString vaultConfigurationError;
        loaded.vaultPasswordEnabled =
            m_database->vaultConfiguration(&vaultConfigurationError).has_value();
        if (!vaultConfigurationError.isEmpty()) {
            queryFailed = true;
            if (errorMessage != nullptr) {
                *errorMessage = vaultConfigurationError;
            }
        }
    }
    assignInteger(kReconnectMaxAttemptsKey, &loaded.reconnectMaxAttempts);
    assignInteger(kReconnectBaseDelayKey, &loaded.reconnectBaseDelaySeconds);

    QSet<QString> storedShortcutIds;
    for (const QString &id : TerminalSettings::shortcutIds()) {
        const std::optional<QString> raw = readValue(kShortcutPrefix + id, &queryFailed);
        if (!raw.has_value() || queryFailed) {
            continue;
        }
        storedShortcutIds.insert(id);
        if (raw->isEmpty()) {
            loaded.shortcuts.insert(id, {});
            continue;
        }
        const QKeySequence parsed = QKeySequence::fromString(*raw, QKeySequence::PortableText);
        if (parsed.isEmpty()) {
            warnings.append(tr("The shortcut '%2' for action '%1' is invalid; the default has been restored")
                                .arg(TerminalSettings::shortcutDisplayName(id), *raw));
        } else {
            loaded.shortcuts.insert(id, parsed);
        }
    }
    if (queryFailed) {
        return false;
    }
    migrateShortcutsWithoutHostSearch(&loaded.shortcuts, storedShortcutIds);

    loaded = loaded.normalized();
    QString validationError;
    if (!loaded.validate(&validationError)) {
        // 外部编辑数据库可能制造重复快捷键。保留外观和终端设置，只把快捷键整体恢复默认，
        // 这样用户仍能打开设置界面修正，而不会因 QAction::AmbiguousShortcut 无法操作。
        warnings.append(validationError + tr("; all shortcuts have been restored to their defaults"));
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
            *errorMessage = tr("The settings service has no app database available");
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
            if (!write(kLanguageKey, normalizedSettings.language)
                || !write(kThemeKey, serializeTheme(normalizedSettings.themeMode))
                || !write(kSidebarDensityKey, serializeSidebarDensity(normalizedSettings.sidebarDensity))
                || !write(kSidebarDoubleClickPromptKey,
                          serializeBool(normalizedSettings.sidebarDoubleClickPrompt))
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
                || !write(kScrollbackMaxMegabytesKey,
                          QString::number(normalizedSettings.scrollbackMaxMegabytes))
                || !write(kAudibleBellKey, serializeBool(normalizedSettings.audibleBell))
                || !write(kCommandAutoCompletionKey,
                          serializeBool(normalizedSettings.commandAutoCompletion))
                || !write(kHistoryMinCommandLengthKey,
                          QString::number(normalizedSettings.historyMinCommandLength))
                || !write(kMultiLinePasteConfirmationKey,
                          serializeBool(normalizedSettings.multiLinePasteConfirmation))
                || !write(kAutomaticReconnectKey,
                          serializeBool(normalizedSettings.automaticReconnect))
                || !write(kVaultPasswordEnabledKey,
                          serializeBool(normalizedSettings.vaultPasswordEnabled))
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
