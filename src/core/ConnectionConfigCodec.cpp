#include "core/ConnectionConfigCodec.h"

#include "storage/ApplicationDatabase.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

#include <algorithm>

namespace {

/** @brief 去掉 OpenSSH 行尾注释；引号内的 # 是路径或参数正文，必须保留。 */
QString stripComment(const QString &line)
{
    bool singleQuoted = false;
    bool doubleQuoted = false;
    for (qsizetype index = 0; index < line.size(); ++index) {
        const QChar character = line.at(index);
        if (character == QLatin1Char('\'') && !doubleQuoted) {
            singleQuoted = !singleQuoted;
        } else if (character == QLatin1Char('"') && !singleQuoted) {
            doubleQuoted = !doubleQuoted;
        } else if (character == QLatin1Char('#') && !singleQuoted && !doubleQuoted) {
            return line.left(index);
        }
    }
    return line;
}

/** @brief 拆分 OpenSSH 的 keyword [=] value，同时保留 value 内部空格供后续引号解析。 */
bool splitDirective(const QString &line, QString *keyword, QString *value)
{
    qsizetype separator = -1;
    for (qsizetype index = 0; index < line.size(); ++index) {
        if (line.at(index).isSpace() || line.at(index) == QLatin1Char('=')) {
            separator = index;
            break;
        }
    }
    if (separator <= 0) {
        return false;
    }
    *keyword = line.left(separator).trimmed().toLower();
    qsizetype valueStart = separator;
    while (valueStart < line.size()
           && (line.at(valueStart).isSpace() || line.at(valueStart) == QLatin1Char('='))) {
        ++valueStart;
    }
    *value = line.mid(valueStart).trimmed();
    return !keyword->isEmpty() && !value->isEmpty();
}

/** @brief 解析必须恰好得到一个 token 的参数，支持 OpenSSH 常见引号路径。 */
bool singleToken(const QString &raw, QString *token)
{
    const QStringList tokens = QProcess::splitCommand(raw);
    if (tokens.size() != 1) {
        return false;
    }
    *token = tokens.constFirst();
    return true;
}

/** @brief 为 OpenSSH 输出引用包含空白或 # 的值，避免再次导入时被截断。 */
QString quoteOpenSsh(const QString &value)
{
    if (!value.contains(QRegularExpression(QStringLiteral("[\\s#\\\"]")))) {
        return value;
    }
    QString escaped = value;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

/** @brief JSON 数组只能包含字符串，空项被拒绝以防静默改变配置。 */
bool readStringArray(const QJsonValue &value, QStringList *result)
{
    if (value.isUndefined()) {
        result->clear();
        return true;
    }
    if (!value.isArray()) {
        return false;
    }
    QStringList values;
    for (const QJsonValue &entry : value.toArray()) {
        if (!entry.isString() || entry.toString().trimmed().isEmpty()) {
            return false;
        }
        values.append(entry.toString().trimmed());
    }
    *result = values;
    return true;
}

/** @brief 递归查找任何可能携带明文秘密的字段，字段名比较不区分大小写。 */
bool containsForbiddenSecretField(const QJsonValue &value, QString *fieldName)
{
    static const QSet<QString> forbidden = {
        QStringLiteral("password"), QStringLiteral("passphrase"), QStringLiteral("secret"),
        QStringLiteral("token"), QStringLiteral("privatekey"), QStringLiteral("privatekeydata"),
        QStringLiteral("encryptedpayload"), QStringLiteral("credentials")
    };
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
            if (forbidden.contains(iterator.key().toLower())) {
                *fieldName = iterator.key();
                return true;
            }
            if (containsForbiddenSecretField(iterator.value(), fieldName)) {
                return true;
            }
        }
    } else if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) {
            if (containsForbiddenSecretField(entry, fieldName)) {
                return true;
            }
        }
    }
    return false;
}

/** @brief 验证父子引用无缺失、无自环且最终都能到达根节点。 */
bool validateHierarchy(const QList<HostRecord> &records, QString *errorMessage)
{
    QHash<QString, QString> parents;
    for (const HostRecord &record : records) {
        if (record.id.isEmpty() || parents.contains(record.id)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("JSON 主机 id 为空或重复：%1").arg(record.id);
            }
            return false;
        }
        parents.insert(record.id, record.parentId);
    }
    for (const HostRecord &record : records) {
        QSet<QString> visited{record.id};
        QString parent = record.parentId;
        while (!parent.isEmpty()) {
            if (!parents.contains(parent)) {
                if (errorMessage != nullptr) {
                    *errorMessage = QStringLiteral("记录 %1 引用了不存在的父节点 %2")
                                        .arg(record.id, parent);
                }
                return false;
            }
            if (visited.contains(parent)) {
                if (errorMessage != nullptr) {
                    *errorMessage = QStringLiteral("JSON 主机层级存在环：%1").arg(parent);
                }
                return false;
            }
            visited.insert(parent);
            parent = parents.value(parent);
        }
    }
    return true;
}

} // namespace

bool ConnectionConfigCodec::importOpenSsh(const QByteArray &content,
                                          QList<HostRecord> *records,
                                          QString *errorMessage)
{
    if (records == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("导入结果参数不能为空");
        }
        return false;
    }
    QList<HostRecord> parsed;
    HostRecord *current = nullptr;
    const QStringList lines = QString::fromUtf8(content).split(QLatin1Char('\n'));
    for (qsizetype index = 0; index < lines.size(); ++index) {
        const QString line = stripComment(lines.at(index)).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        QString keyword;
        QString rawValue;
        if (!splitDirective(line, &keyword, &rawValue)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("OpenSSH 配置第 %1 行语法无效").arg(index + 1);
            }
            return false;
        }
        if (keyword == QStringLiteral("match") || keyword == QStringLiteral("include")) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("第 %1 行的 %2 无法被 Shellium 忠实表达，已拒绝导入")
                                    .arg(index + 1).arg(keyword);
            }
            return false;
        }
        if (keyword == QStringLiteral("host")) {
            QString alias;
            if (!singleToken(rawValue, &alias)
                || alias.contains(QRegularExpression(QStringLiteral("[*!?]")))) {
                if (errorMessage != nullptr) {
                    *errorMessage = QStringLiteral("第 %1 行 Host 必须是单个、不含通配符的别名")
                                        .arg(index + 1);
                }
                return false;
            }
            HostRecord record;
            record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            record.kind = HostNodeKind::Host;
            record.name = alias;
            record.host = alias;
            parsed.append(record);
            current = &parsed.last();
            continue;
        }
        if (current == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("第 %1 行是全局指令；Shellium 无法保持其作用域")
                                    .arg(index + 1);
            }
            return false;
        }

        QString value;
        if (!singleToken(rawValue, &value)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("第 %1 行参数需要使用引号包住空格").arg(index + 1);
            }
            return false;
        }
        if (keyword == QStringLiteral("hostname")) {
            current->host = value;
        } else if (keyword == QStringLiteral("user")) {
            current->username = value;
        } else if (keyword == QStringLiteral("port")) {
            bool ok = false;
            const uint port = value.toUInt(&ok);
            if (!ok || port == 0 || port > 65535) {
                if (errorMessage != nullptr) {
                    *errorMessage = QStringLiteral("第 %1 行端口超出 1..65535").arg(index + 1);
                }
                return false;
            }
            current->port = static_cast<quint16>(port);
        } else if (keyword == QStringLiteral("identityfile")) {
            current->identityFile = value;
        } else if (keyword == QStringLiteral("proxyjump")) {
            current->proxyJumps = value.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0
                ? QStringList{} : value.split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (QString &jump : current->proxyJumps) {
                jump = jump.trimmed();
            }
        } else {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("第 %1 行指令 %2 无法忠实保存，已拒绝导入")
                                    .arg(index + 1).arg(keyword);
            }
            return false;
        }
    }
    if (parsed.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("OpenSSH 配置中没有可导入的 Host 段");
        }
        return false;
    }
    *records = parsed;
    return true;
}

QByteArray ConnectionConfigCodec::exportOpenSsh(const QList<HostRecord> &resolvedHosts,
                                                QString *errorMessage)
{
    QByteArray output("# Generated by Shellium; credentials are intentionally omitted.\n\n");
    QSet<QString> aliases;
    for (const HostRecord &record : resolvedHosts) {
        if (record.kind != HostNodeKind::Host) {
            continue;
        }
        if (record.host.trimmed().isEmpty()) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("主机 %1 缺少地址").arg(record.name);
            }
            return {};
        }
        QString alias = record.name.trimmed();
        if (alias.isEmpty() || alias.contains(QRegularExpression("[\\s*!?]"))) {
            alias = record.id;
        }
        if (alias.isEmpty() || aliases.contains(alias)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("OpenSSH 导出别名为空或重复：%1").arg(alias);
            }
            return {};
        }
        aliases.insert(alias);
        output += "Host " + alias.toUtf8() + '\n';
        output += "    HostName " + quoteOpenSsh(record.host).toUtf8() + '\n';
        if (!record.username.isEmpty()) {
            output += "    User " + quoteOpenSsh(record.username).toUtf8() + '\n';
        }
        if (record.port.has_value()) {
            output += "    Port " + QByteArray::number(*record.port) + '\n';
        }
        if (!record.identityFile.isEmpty()) {
            output += "    IdentityFile " + quoteOpenSsh(record.identityFile).toUtf8() + '\n';
        }
        if (!record.proxyJumps.isEmpty()) {
            output += "    ProxyJump " + record.proxyJumps.join(QLatin1Char(',')).toUtf8() + '\n';
        }
        output += '\n';
    }
    return output;
}

bool ConnectionConfigCodec::importJson(const QByteArray &content,
                                       QList<HostRecord> *records,
                                       QString *errorMessage)
{
    if (records == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("导入结果参数不能为空");
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(content, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("JSON 语法错误：%1").arg(parseError.errorString());
        }
        return false;
    }
    QString forbiddenField;
    if (containsForbiddenSecretField(document.object(), &forbiddenField)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("JSON 含禁止导入的敏感字段：%1").arg(forbiddenField);
        }
        return false;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() != QStringLiteral("shellium-hosts")
        || root.value(QStringLiteral("version")).toInt(-1) != JsonFormatVersion
        || !root.value(QStringLiteral("hosts")).isArray()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("不是受支持的 Shellium 主机 JSON 格式");
        }
        return false;
    }

    QList<HostRecord> parsed;
    const QJsonArray hosts = root.value(QStringLiteral("hosts")).toArray();
    for (qsizetype index = 0; index < hosts.size(); ++index) {
        if (!hosts.at(index).isObject()) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("hosts[%1] 不是对象").arg(index);
            }
            return false;
        }
        const QJsonObject object = hosts.at(index).toObject();
        HostRecord record;
        record.id = object.value(QStringLiteral("id")).toString().trimmed();
        record.parentId = object.value(QStringLiteral("parentId")).toString().trimmed();
        record.name = object.value(QStringLiteral("name")).toString().trimmed();
        record.host = object.value(QStringLiteral("host")).toString().trimmed();
        const QString kind = object.value(QStringLiteral("kind")).toString();
        record.kind = kind == QStringLiteral("folder") ? HostNodeKind::Folder : HostNodeKind::Host;
        if (kind != QStringLiteral("folder") && kind != QStringLiteral("host")) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("hosts[%1].kind 无效").arg(index);
            }
            return false;
        }
        if (record.id.isEmpty() || record.name.isEmpty()
            || (record.kind == HostNodeKind::Host && record.host.isEmpty())) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("hosts[%1] 缺少 id、name 或主机地址").arg(index);
            }
            return false;
        }
        const QJsonValue port = object.value(QStringLiteral("port"));
        if (!port.isUndefined() && !port.isNull()) {
            const int number = port.toInt(-1);
            if (number <= 0 || number > 65535) {
                if (errorMessage != nullptr) {
                    *errorMessage = QStringLiteral("hosts[%1].port 超出范围").arg(index);
                }
                return false;
            }
            record.port = static_cast<quint16>(number);
        }
        record.username = object.value(QStringLiteral("username")).toString();
        record.identityFile = object.value(QStringLiteral("identityFile")).toString();
        if (!readStringArray(object.value(QStringLiteral("proxyJumps")), &record.proxyJumps)
            || !readStringArray(object.value(QStringLiteral("tags")), &record.tags)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("hosts[%1] 的列表字段格式无效").arg(index);
            }
            return false;
        }
        record.allowLegacyHmacSha1 = object.value(QStringLiteral("allowLegacyHmacSha1")).toBool();
        record.sortOrder = object.value(QStringLiteral("sortOrder")).toInt();
        // 即使外部文件手工加入 credentialId，也不能把它连接到本机凭据表。
        record.credentialId.clear();
        parsed.append(record);
    }
    if (!validateHierarchy(parsed, errorMessage)) {
        return false;
    }
    *records = parsed;
    return true;
}

QByteArray ConnectionConfigCodec::exportJson(const QList<HostRecord> &records,
                                             QString *errorMessage)
{
    if (!validateHierarchy(records, errorMessage)) {
        return {};
    }
    QJsonArray hosts;
    for (const HostRecord &record : records) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), record.id);
        object.insert(QStringLiteral("parentId"), record.parentId);
        object.insert(QStringLiteral("kind"), record.kind == HostNodeKind::Folder
                                                    ? QStringLiteral("folder")
                                                    : QStringLiteral("host"));
        object.insert(QStringLiteral("name"), record.name);
        object.insert(QStringLiteral("host"), record.host);
        object.insert(QStringLiteral("port"), record.port.has_value()
                                                   ? QJsonValue(static_cast<int>(*record.port))
                                                   : QJsonValue(QJsonValue::Null));
        object.insert(QStringLiteral("username"), record.username);
        object.insert(QStringLiteral("identityFile"), record.identityFile);
        object.insert(QStringLiteral("proxyJumps"), QJsonArray::fromStringList(record.proxyJumps));
        object.insert(QStringLiteral("tags"), QJsonArray::fromStringList(record.tags));
        object.insert(QStringLiteral("allowLegacyHmacSha1"), record.allowLegacyHmacSha1);
        object.insert(QStringLiteral("sortOrder"), record.sortOrder);
        // credentialId、密文和一切口令字段均不进入交换格式，避免导出文件成为秘密载体。
        hosts.append(object);
    }
    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("shellium-hosts"));
    root.insert(QStringLiteral("version"), JsonFormatVersion);
    root.insert(QStringLiteral("hosts"), hosts);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool ConnectionConfigCodec::persistImported(ApplicationDatabase &database,
                                            const QList<HostRecord> &records,
                                            const QString &attachRootParentId,
                                            QString *errorMessage)
{
    if (!validateHierarchy(records, errorMessage)) {
        return false;
    }
    QHash<QString, QString> remappedIds;
    for (const HostRecord &record : records) {
        remappedIds.insert(record.id, QUuid::createUuid().toString(QUuid::WithoutBraces));
    }

    QList<HostRecord> pending = records;
    return database.runInTransaction([&](QString *innerError) {
        QSet<QString> savedSourceIds;
        while (!pending.isEmpty()) {
            bool progressed = false;
            for (qsizetype index = pending.size() - 1; index >= 0; --index) {
                const HostRecord source = pending.at(index);
                if (!source.parentId.isEmpty() && !savedSourceIds.contains(source.parentId)) {
                    continue;
                }
                HostRecord imported = source;
                imported.id = remappedIds.value(source.id);
                imported.parentId = source.parentId.isEmpty()
                    ? attachRootParentId : remappedIds.value(source.parentId);
                imported.credentialId.clear();
                imported.createdAt = {};
                imported.updatedAt = {};
                if (!database.saveHost(imported, innerError)) {
                    return false;
                }
                savedSourceIds.insert(source.id);
                pending.removeAt(index);
                progressed = true;
            }
            if (!progressed) {
                *innerError = QStringLiteral("导入层级无法排序，可能含环或缺失父节点");
                return false;
            }
        }
        return true;
    }, errorMessage);
}
