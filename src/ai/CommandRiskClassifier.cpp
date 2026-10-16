#include "ai/CommandRiskClassifier.h"

#include <QCoreApplication>
#include <QHash>
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
        QStringLiteral("lsblk"), QStringLiteral("id"),
        QStringLiteral("who"), QStringLiteral("w"), QStringLiteral("lsb_release"),
        QStringLiteral("findmnt"), QStringLiteral("getconf"), QStringLiteral("arch"),
        QStringLiteral("apt-cache"),
    };
    return names;
}

/**
 * @brief 部分命令只有特定子命令或参数是只读的（如 systemctl status、git log、python3 --version）。
 *
 * 解释器、包管理器和 git 曾经整体列在只读名单里，`python3 -c "..."`、`git push`、`dpkg -i`
 * 因此被标成"只读"；在引入自动执行后这会直接变成无人确认的写操作，所以这些命令只按
 * 明确的只读参数放行。
 */
bool isReadOnlySubcommand(const QString &commandName, const QStringList &tokens)
{
    if (commandName == QStringLiteral("mount")) {
        // 不带参数时只是列出挂载点；带任何参数都可能执行挂载。
        return tokens.size() == 1;
    }
    if (tokens.size() < 2) {
        return false;
    }
    const QString sub = tokens.at(1).toLower();
    if (tokens.size() == 2) {
        // 解释器与 nginx 只放行"查看版本 / 检查配置"这类单参数形态；`python -v` 是带
        // 详细日志的交互解释器，`nginx -s stop` 会停服务，都不能按名字整体放行。
        static const QHash<QString, QSet<QString>> versionOnly = {
            {QStringLiteral("node"), {QStringLiteral("--version"), QStringLiteral("-v")}},
            {QStringLiteral("python"), {QStringLiteral("--version"), QStringLiteral("-V")}},
            {QStringLiteral("python3"), {QStringLiteral("--version"), QStringLiteral("-V")}},
            {QStringLiteral("java"), {QStringLiteral("--version"), QStringLiteral("-version")}},
            {QStringLiteral("nginx"),
             {QStringLiteral("-v"), QStringLiteral("-V"), QStringLiteral("-t"), QStringLiteral("-T")}},
        };
        const auto it = versionOnly.constFind(commandName);
        if (it != versionOnly.constEnd()) {
            return it->contains(tokens.at(1));
        }
    }
    if (commandName == QStringLiteral("dpkg")) {
        static const QSet<QString> readOnlyFlags = {
            QStringLiteral("-l"), QStringLiteral("-L"), QStringLiteral("-s"),
            QStringLiteral("-S"), QStringLiteral("--list"), QStringLiteral("--listfiles"),
            QStringLiteral("--status"), QStringLiteral("--search"),
            QStringLiteral("--get-selections"), QStringLiteral("--print-architecture")};
        return readOnlyFlags.contains(tokens.at(1));
    }
    if (commandName == QStringLiteral("rpm")) {
        // rpm 的查询模式统一以 -q 开头（-q、-qa、-qi、-ql…）；安装、卸载、升级都不是。
        return tokens.at(1).startsWith(QStringLiteral("-q")) || tokens.at(1) == QStringLiteral("--query");
    }
    if (commandName == QStringLiteral("find")) {
        static const QRegularExpression mutatingAction(QStringLiteral(
            "\\s-(delete|exec|execdir|ok|okdir|fprint0?|fprintf|fls)\\b"));
        return !mutatingAction.match(tokens.join(QLatin1Char(' '))).hasMatch();
    }
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
            QStringLiteral("show"), QStringLiteral("blame")};
        if (readOnlySub.contains(sub)) {
            return true;
        }
        // branch/remote 只有"列出"形态是只读的；`git branch -D x`、`git remote add` 都会写仓库。
        if (sub == QStringLiteral("branch") || sub == QStringLiteral("remote")) {
            static const QSet<QString> listingFlags = {
                QStringLiteral("-a"), QStringLiteral("-r"), QStringLiteral("-v"),
                QStringLiteral("-vv"), QStringLiteral("--list"), QStringLiteral("--all")};
            for (int i = 2; i < tokens.size(); ++i) {
                if (!listingFlags.contains(tokens.at(i))) {
                    return false;
                }
            }
            return true;
        }
        return false;
    }
    if (commandName == QStringLiteral("docker")) {
        static const QSet<QString> readOnlySub = {
            QStringLiteral("ps"), QStringLiteral("images"), QStringLiteral("logs"),
            QStringLiteral("inspect"), QStringLiteral("stats")};
        return readOnlySub.contains(sub);
    }
    return false;
}

/**
 * @brief 命令替换与进程替换：`$(...)`、反引号、`<(...)`。
 *
 * 被替换的内层命令会先于外层执行，`echo $(rm -rf x)` 的外层虽是 echo，实际效果却是删除，
 * 因此出现任何替换语法都不能再被判为只读。
 */
const QRegularExpression &commandSubstitutionPattern()
{
    static const QRegularExpression pattern(QStringLiteral("\\$\\(|`|[<>]\\("));
    return pattern;
}

/**
 * @brief 判断一个命令段是否会停在交互界面、等待输入或持续运行不退出。
 * @param tokens 已按空白拆分的命令段。
 */
bool segmentNeedsInteraction(const QStringList &tokens)
{
    if (tokens.isEmpty()) {
        return false;
    }
    QStringList lowered;
    lowered.reserve(tokens.size());
    for (const QString &token : tokens) {
        lowered.append(token.toLower());
    }
    const QString first = lowered.first();
    // sudo/su/doas 可能弹出密码提示；无人值守地把命令停在密码输入处，比不执行更糟。
    static const QSet<QString> elevation = {
        QStringLiteral("sudo"), QStringLiteral("su"), QStringLiteral("doas")};
    if (elevation.contains(first)) {
        return true;
    }
    QString name;
    for (const QString &token : lowered) {
        if (token.contains(QLatin1Char('=')) && !token.startsWith(QLatin1Char('-'))) {
            continue;
        }
        if (token == QStringLiteral("env")) {
            continue;
        }
        name = token;
        break;
    }
    static const QSet<QString> fullScreen = {
        QStringLiteral("less"), QStringLiteral("more"), QStringLiteral("most"),
        QStringLiteral("vi"), QStringLiteral("vim"), QStringLiteral("nano"),
        QStringLiteral("htop"), QStringLiteral("watch"), QStringLiteral("ssh")};
    if (fullScreen.contains(name)) {
        return true;
    }
    if (name == QStringLiteral("top")) {
        // 批处理模式（top -b -n 1）会正常退出，其余形态都是全屏刷新。
        return !lowered.contains(QStringLiteral("-b"));
    }
    const auto hasFollowFlag = [&lowered]() {
        for (const QString &token : lowered) {
            if (token == QStringLiteral("-f") || token == QStringLiteral("-F")
                || token == QStringLiteral("--follow")
                || (token.startsWith(QLatin1Char('-')) && !token.startsWith(QStringLiteral("--"))
                    && token.contains(QLatin1Char('f')) && token.size() <= 4)) {
                return true;
            }
        }
        return false;
    };
    if (name == QStringLiteral("tail") || name == QStringLiteral("journalctl")) {
        return hasFollowFlag();
    }
    if ((name == QStringLiteral("docker") || name == QStringLiteral("kubectl"))
        && lowered.size() > 1 && lowered.at(1) == QStringLiteral("logs")) {
        return hasFollowFlag();
    }
    if (name == QStringLiteral("docker") && lowered.size() > 1
        && lowered.at(1) == QStringLiteral("stats")) {
        return !lowered.contains(QStringLiteral("--no-stream"));
    }
    if (name == QStringLiteral("ping")) {
        // Linux 下不带 -c 的 ping 永不退出。
        for (const QString &token : lowered) {
            if (token == QStringLiteral("-c") || token.startsWith(QStringLiteral("-c"))) {
                return false;
            }
        }
        return true;
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

    if (commandSubstitutionPattern().match(trimmed).hasMatch()) {
        return CommandRiskLevel::Write;
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

bool isSafeForAutoExecution(const QString &command)
{
    if (classify(command) != CommandRiskLevel::ReadOnly) {
        return false;
    }
    for (const QString &segment : splitCommandSegments(command.trimmed())) {
        const QStringList tokens = segment.trimmed().split(
            QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (segmentNeedsInteraction(tokens)) {
            return false;
        }
    }
    return true;
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
