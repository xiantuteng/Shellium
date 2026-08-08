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

/** @brief 把终端配色风格序列化为不受本地化影响的稳定字符串。 */
QString serializeColorScheme(const TerminalColorScheme scheme)
{
    switch (scheme) {
    case TerminalColorScheme::SolarizedDark:
        return QStringLiteral("solarized_dark");
    case TerminalColorScheme::Dracula:
        return QStringLiteral("dracula");
    case TerminalColorScheme::Monokai:
        return QStringLiteral("monokai");
    case TerminalColorScheme::Nord:
        return QStringLiteral("nord");
    case TerminalColorScheme::Default:
    default:
        return QStringLiteral("default");
    }
}

/** @brief 解析终端配色风格字符串；未知值返回空，调用方据此回退默认值并报告警告。 */
std::optional<TerminalColorScheme> parseColorScheme(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QLatin1String("default")) {
        return TerminalColorScheme::Default;
    }
    if (normalized == QLatin1String("solarized_dark")) {
        return TerminalColorScheme::SolarizedDark;
    }
    if (normalized == QLatin1String("dracula")) {
        return TerminalColorScheme::Dracula;
    }
    if (normalized == QLatin1String("monokai")) {
        return TerminalColorScheme::Monokai;
    }
    if (normalized == QLatin1String("nord")) {
        return TerminalColorScheme::Nord;
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
 * @param terminalPalette 当前主题的终端基础色。
 * @param dark 是否为深色主题；菜单和禁用文字据此选择对比度。
 */
QPalette createApplicationPalette(const TerminalPalette &terminalPalette, const bool dark)
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

    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, terminalPalette.foreground);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, alternate);
    palette.setColor(QPalette::Text, terminalPalette.foreground);
    palette.setColor(QPalette::Button, window);
    palette.setColor(QPalette::ButtonText, terminalPalette.foreground);
    palette.setColor(QPalette::ToolTipBase, base);
    palette.setColor(QPalette::ToolTipText, terminalPalette.foreground);
    palette.setColor(QPalette::Highlight, terminalPalette.selection);
    palette.setColor(QPalette::HighlightedText,
                     dark ? QColor(Qt::white) : QColor(QStringLiteral("#111318")));
    palette.setColor(QPalette::Link, terminalPalette.link);
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

TerminalPalette TerminalSettings::effectivePalette() const
{
    // 除 Default 外的配色风格都是固定搭配好的一整套主题（对应主流终端模拟器的同名预设），
    // 不再随浅色/深色切换而改变，因此在这里直接短路，不看 effectiveThemeMode()。
    switch (colorScheme) {
    case TerminalColorScheme::SolarizedDark:
        return {
            QColor(QStringLiteral("#002b36")),
            QColor(QStringLiteral("#839496")),
            QColor(QStringLiteral("#d33682")),
            QColor(QStringLiteral("#073642")),
            QColor(QStringLiteral("#268bd2")),
        };
    case TerminalColorScheme::Dracula:
        return {
            QColor(QStringLiteral("#282a36")),
            QColor(QStringLiteral("#f8f8f2")),
            QColor(QStringLiteral("#ff79c6")),
            QColor(QStringLiteral("#44475a")),
            QColor(QStringLiteral("#8be9fd")),
        };
    case TerminalColorScheme::Monokai:
        return {
            QColor(QStringLiteral("#272822")),
            QColor(QStringLiteral("#f8f8f2")),
            QColor(QStringLiteral("#f92672")),
            QColor(QStringLiteral("#49483e")),
            QColor(QStringLiteral("#66d9ef")),
        };
    case TerminalColorScheme::Nord:
        return {
            QColor(QStringLiteral("#2e3440")),
            QColor(QStringLiteral("#d8dee9")),
            QColor(QStringLiteral("#88c0d0")),
            QColor(QStringLiteral("#434c5e")),
            QColor(QStringLiteral("#81a1c1")),
        };
    case TerminalColorScheme::Default:
    default:
        break;
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
        && colorScheme == other.colorScheme
        && fontFamily == other.fontFamily
        && fontPointSize == other.fontPointSize
        && lineHeightPercent == other.lineHeightPercent
        && qFuzzyCompare(letterSpacing + 1.0, other.letterSpacing + 1.0)
        && cursorShape == other.cursorShape
        && cursorBlink == other.cursorBlink
        && scrollbackLines == other.scrollbackLines
        && audibleBell == other.audibleBell
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
        const std::optional<TerminalColorScheme> parsed = parseColorScheme(*raw);
        if (parsed.has_value()) {
            loaded.colorScheme = *parsed;
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
                || !write(kColorSchemeKey, serializeColorScheme(normalizedSettings.colorScheme))
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
    const TerminalPalette terminalPalette = m_currentSettings.effectivePalette();
    m_boundApplication->setPalette(createApplicationPalette(
        terminalPalette, m_currentSettings.effectiveThemeMode() == ThemeMode::Dark));
}
