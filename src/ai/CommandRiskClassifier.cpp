#include "ai/CommandRiskClassifier.h"

#include <QCoreApplication>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace {

/**
 * @brief 命中即视为高风险的整段命令特征，按危害类型分组以便后续增补。
 *
 * 这里只覆盖常见的破坏性、不可逆或大范围影响的操作；没有命中不代表命令绝对安全，只是
 * 会退回默认的 Write 分级，仍然需要用户确认才能执行。
 */
const QList<QRegularExpression> &highRiskPatterns()
{
    static const QList<QRegularExpression> patterns = [] {
        const QStringList raw = {
            // 递归删除根/宽泛路径、常见系统目录
            QStringLiteral("\\brm\\s+.*-[a-zA-Z]*r[a-zA-Z]*f|\\brm\\s+.*-[a-zA-Z]*f[a-zA-Z]*r"),
            QStringLiteral("\\brm\\b[^\\n]*\\s(/|/etc|/var|/usr|/bin|/boot|/home)(\\s|$)"),
            // 磁盘/分区级别的破坏性写入
            QStringLiteral("\\bmkfs(\\.[a-zA-Z0-9]+)?\\b"),
            QStringLiteral("\\bdd\\s+.*\\bof=\\s*/dev/"),
            QStringLiteral(">\\s*/dev/sd[a-z]"),
            QStringLiteral("\\bfdisk\\b|\\bparted\\b|\\bmkswap\\b|\\bwipefs\\b"),
            // 权限/所有权大范围下放
            QStringLiteral("\\bchmod\\s+.*-R.*\\s(777|000)\\b"),
            QStringLiteral("\\bchown\\s+.*-R\\s+.*\\s/(\\s|$)"),
            // 系统级关停或重启
            QStringLiteral("\\b(shutdown|reboot|halt|poweroff|init\\s+0)\\b"),
            // fork 炸弹
            QStringLiteral(":\\(\\)\\s*\\{\\s*:\\|:&\\s*\\};\\s*:"),
            // 直接执行远程脚本
            QStringLiteral("\\b(curl|wget)\\b[^\\n|]*\\|\\s*(sudo\\s+)?(sh|bash|zsh)\\b"),
            // 数据库/数据层面的不可逆操作
            QStringLiteral("\\bdrop\\s+(table|database|schema)\\b"),
            QStringLiteral("\\btruncate\\s+table\\b"),
            // 防火墙清空、杀 1 号进程
            QStringLiteral("\\biptables\\s+.*-F\\b"),
            QStringLiteral("\\bkill\\s+.*-9\\s+1\\b"),
            // 包管理器的整机卸载/降级操作
            QStringLiteral("\\byum\\s+remove\\s+\\*|\\bdnf\\s+remove\\s+\\*|\\bapt(-get)?\\s+remove\\s+\\*"),
        };
        QList<QRegularExpression> compiled;
        compiled.reserve(raw.size());
        for (const QString &pattern : raw) {
            QRegularExpression expression(pattern, QRegularExpression::CaseInsensitiveOption);
            Q_ASSERT(expression.isValid());
            compiled.append(expression);
        }
        return compiled;
    }();
    return patterns;
}

/** @brief 命令名判定为只读时，仍会被下面这些重定向/管道写入符号打回 Write 分级。 */
const QRegularExpression &writeRedirectionPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral(">>?[^&]|\\|\\s*tee\\b|\\bsudo\\s+tee\\b"));
    return pattern;
}

/** @brief 已知不修改系统状态的命令名，只在没有写入重定向时才生效。 */
const QSet<QString> &readOnlyCommandNames()
{
    static const QSet<QString> names = {
        QStringLiteral("ls"), QStringLiteral("cat"), QStringLiteral("head"),
        QStringLiteral("tail"), QStringLiteral("grep"), QStringLiteral("egrep"),
        QStringLiteral("fgrep"), QStringLiteral("ps"), QStringLiteral("top"),
        QStringLiteral("df"), QStringLiteral("du"), QStringLiteral("free"),
        QStringLiteral("uptime"), QStringLiteral("whoami"), QStringLiteral("id"),
        QStringLiteral("pwd"), QStringLiteral("echo"), QStringLiteral("printf"),
        QStringLiteral("hostname"), QStringLiteral("uname"), QStringLiteral("date"),
        QStringLiteral("which"), QStringLiteral("whereis"), QStringLiteral("file"),
        QStringLiteral("stat"), QStringLiteral("wc"), QStringLiteral("sort"),
        QStringLiteral("uniq"), QStringLiteral("less"), QStringLiteral("more"),
        QStringLiteral("journalctl"), QStringLiteral("netstat"), QStringLiteral("ss"),
        QStringLiteral("lsof"), QStringLiteral("ping"), QStringLiteral("env"),
        QStringLiteral("printenv"), QStringLiteral("nproc"), QStringLiteral("lscpu"),
        QStringLiteral("lsblk"), QStringLiteral("mount"), QStringLiteral("id"),
        QStringLiteral("who"), QStringLiteral("w"), QStringLiteral("nginx"),
        QStringLiteral("node"), QStringLiteral("python"), QStringLiteral("python3"),
        QStringLiteral("java"), QStringLiteral("git"), QStringLiteral("dpkg"),
        QStringLiteral("rpm"),
    };
    return names;
}

/** @brief 部分命令只有特定子命令是只读的（如 systemctl status、git log）。 */
bool isReadOnlySubcommand(const QString &commandName, const QStringList &tokens)
{
    if (tokens.size() < 2) {
        return false;
    }
    const QString sub = tokens.at(1).toLower();
    if (commandName == QStringLiteral("systemctl")) {
        static const QSet<QString> readOnlySub = {
            QStringLiteral("status"), QStringLiteral("is-active"),
            QStringLiteral("is-enabled"), QStringLiteral("list-units"),
            QStringLiteral("list-unit-files"), QStringLiteral("show")};
        return readOnlySub.contains(sub);
    }
    if (commandName == QStringLiteral("git")) {
        static const QSet<QString> readOnlySub = {
            QStringLiteral("status"), QStringLiteral("log"), QStringLiteral("diff"),
            QStringLiteral("show"), QStringLiteral("branch"), QStringLiteral("remote"),
            QStringLiteral("blame")};
        return readOnlySub.contains(sub);
    }
    if (commandName == QStringLiteral("docker")) {
        static const QSet<QString> readOnlySub = {
            QStringLiteral("ps"), QStringLiteral("images"), QStringLiteral("logs"),
            QStringLiteral("inspect"), QStringLiteral("stats")};
        return readOnlySub.contains(sub);
    }
    return false;
}

/** @brief 提取一个管道/连接符分隔的命令段的首个可执行名，跳过 sudo/env 一类前缀。 */
QString leadingCommandName(const QStringList &tokens)
{
    static const QSet<QString> skippablePrefixes = {
        QStringLiteral("sudo"), QStringLiteral("env")};
    for (const QString &token : tokens) {
        const QString normalized = token.toLower();
        if (normalized.contains(QLatin1Char('=')) && !normalized.startsWith(QLatin1Char('-'))) {
            // env 风格的 KEY=VALUE 前缀，如 `FOO=bar systemctl status x`。
            continue;
        }
        if (skippablePrefixes.contains(normalized)) {
            continue;
        }
        return normalized;
    }
    return {};
}

/** @brief 按 ; && || | 拆出各个独立命令段，双引号/单引号内的分隔符不会被拆开。 */
QStringList splitCommandSegments(const QString &command)
{
    QStringList segments;
    QString current;
    QChar quote;
    for (int i = 0; i < command.size(); ++i) {
        const QChar ch = command.at(i);
        if (!quote.isNull()) {
            current.append(ch);
            if (ch == quote) {
                quote = QChar();
            }
            continue;
        }
        if (ch == QLatin1Char('\'') || ch == QLatin1Char('"')) {
            quote = ch;
            current.append(ch);
            continue;
        }
        const bool isSemicolon = ch == QLatin1Char(';');
        const bool isPipe = ch == QLatin1Char('|') && (i + 1 >= command.size() || command.at(i + 1) != QLatin1Char('|'));
        const bool isDoublePipeOrAnd =
            (ch == QLatin1Char('|') || ch == QLatin1Char('&')) && i + 1 < command.size()
            && command.at(i + 1) == ch;
        if (isSemicolon || isPipe || isDoublePipeOrAnd) {
            segments.append(current);
            current.clear();
            if (isDoublePipeOrAnd) {
                ++i;
            }
            continue;
        }
        current.append(ch);
    }
    if (!current.trimmed().isEmpty()) {
        segments.append(current);
    }
    return segments;
}

} // namespace

namespace CommandRiskClassifier {

CommandRiskLevel classify(const QString &command)
{
    const QString trimmed = command.trimmed();
    if (trimmed.isEmpty()) {
        return CommandRiskLevel::Write;
    }

    for (const QRegularExpression &pattern : highRiskPatterns()) {
        if (pattern.match(trimmed).hasMatch()) {
            return CommandRiskLevel::HighRisk;
        }
    }

    bool allSegmentsReadOnly = true;
    for (const QString &segment : splitCommandSegments(trimmed)) {
        const QString trimmedSegment = segment.trimmed();
        if (trimmedSegment.isEmpty()) {
            continue;
        }
        if (writeRedirectionPattern().match(trimmedSegment).hasMatch()) {
            allSegmentsReadOnly = false;
            break;
        }
        const QStringList tokens = trimmedSegment.split(
            QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        const QString commandName = leadingCommandName(tokens);
        if (commandName.isEmpty()) {
            allSegmentsReadOnly = false;
            break;
        }
        if (readOnlyCommandNames().contains(commandName)) {
            continue;
        }
        if (isReadOnlySubcommand(commandName, tokens)) {
            continue;
        }
        allSegmentsReadOnly = false;
        break;
    }

    return allSegmentsReadOnly ? CommandRiskLevel::ReadOnly : CommandRiskLevel::Write;
}

QString label(const CommandRiskLevel level)
{
    switch (level) {
    case CommandRiskLevel::ReadOnly:
        return QObject::tr("Read-only · allowed");
    case CommandRiskLevel::Write:
        return QObject::tr("Modifies the system · needs approval");
    case CommandRiskLevel::HighRisk:
        return QObject::tr("High risk · needs approval");
    }
    return {};
}

} // namespace CommandRiskClassifier
