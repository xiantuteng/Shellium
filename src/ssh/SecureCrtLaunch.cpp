#include "ssh/SecureCrtLaunch.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVector>
#include <QXmlStreamReader>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace {

constexpr auto kShimBundleIdentifier = "app.shellium.securecrt-shim";
constexpr auto kShimMarkerName = "shellium-securecrt-shim";
constexpr int kMaxLaunchPayloadBytes = 65536;

QString optionName(QString token)
{
    while (token.startsWith(QLatin1Char('/')) || token.startsWith(QLatin1Char('-'))) {
        token.remove(0, 1);
    }
    return token.toLower();
}

bool isOptionToken(const QString &token)
{
    if (token.size() < 2) {
        return false;
    }
    if (token.startsWith(QLatin1Char('/'))) {
        return true;
    }
    if (token.startsWith(QLatin1String("--"))) {
        return true;
    }
    return token.startsWith(QLatin1Char('-')) && token.at(1).isLetter();
}

bool optionTakesValue(const QString &name)
{
    return name == QLatin1String("n")
        || name == QLatin1String("p")
        || name == QLatin1String("l")
        || name == QLatin1String("password")
        || name == QLatin1String("passphrase")
        || name == QLatin1String("auth")
        || name == QLatin1String("session")
        || name == QLatin1String("arg")
        || name == QLatin1String("script")
        || name == QLatin1String("c");
}

QString stripMatchingQuotes(QString text)
{
    if (text.size() >= 2) {
        const QChar first = text.front();
        const QChar last = text.back();
        if ((first == QLatin1Char('"') && last == QLatin1Char('"'))
            || (first == QLatin1Char('\'') && last == QLatin1Char('\''))) {
            text = text.mid(1, text.size() - 2);
        }
    }
    return text.trimmed();
}

void applyLoginName(ConnectionTarget *target, const QString &loginName)
{
    const QString trimmed = loginName.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    const int separator = trimmed.indexOf(QLatin1Char('#'));
    if (separator > 0) {
        target->loginUsername = trimmed;
        target->username = trimmed.left(separator);
        return;
    }
    target->username = trimmed;
    target->loginUsername.clear();
}

bool applyDestination(SecureCrtLaunchRequest *request, const QString &specifier, const bool portExplicit)
{
    QString userPart;
    QString hostPart = specifier.trimmed();
    const int at = hostPart.lastIndexOf(QLatin1Char('@'));
    if (at >= 0) {
        userPart = hostPart.left(at);
        hostPart = hostPart.mid(at + 1).trimmed();
    }
    if (!userPart.isEmpty() && request->target.username.isEmpty() && request->target.loginUsername.isEmpty()) {
        applyLoginName(&request->target, userPart);
    }

    QString host = hostPart;
    QString portText;
    if (host.startsWith(QLatin1Char('['))) {
        const int end = host.indexOf(QLatin1Char(']'));
        if (end < 0) {
            request->error = SecureCrtLaunchRequest::Error::MissingDestination;
            return false;
        }
        portText = host.mid(end + 1);
        host = host.mid(1, end - 1);
        if (portText.startsWith(QLatin1Char(':'))) {
            portText.remove(0, 1);
        } else {
            portText.clear();
        }
    } else {
        const int colon = host.lastIndexOf(QLatin1Char(':'));
        // 多个冒号是未加方括号的 IPv6，不能把其中一段当成端口。
        if (colon > 0 && host.indexOf(QLatin1Char(':')) == colon) {
            portText = host.mid(colon + 1);
            host = host.left(colon);
        }
    }
    if (host.isEmpty()) {
        request->error = SecureCrtLaunchRequest::Error::MissingDestination;
        return false;
    }
    request->target.host = host;
    if (!portExplicit && !portText.isEmpty()) {
        bool ok = false;
        const int port = portText.toInt(&ok);
        if (!ok || port < 1 || port > 65535) {
            request->error = SecureCrtLaunchRequest::Error::InvalidPort;
            request->errorDetail = portText;
            return false;
        }
        request->target.port = static_cast<quint16>(port);
    }
    return true;
}

bool applySshUrl(SecureCrtLaunchRequest *request, const QString &text)
{
    const QUrl url(text);
    if (!url.isValid() || url.scheme().compare(QLatin1String("ssh"), Qt::CaseInsensitive) != 0) {
        return false;
    }
    const QString urlUser = url.userName(QUrl::FullyDecoded);
    const QString urlPassword = url.password(QUrl::FullyDecoded);
    if (!urlUser.isEmpty()) {
        applyLoginName(&request->target, urlUser);
    }
    if (!urlPassword.isEmpty() && request->target.password.isEmpty()) {
        request->target.password = urlPassword;
    }
    if (!url.host().isEmpty()) {
        request->target.host = url.host();
    }
    if (url.port() > 0) {
        request->target.port = static_cast<quint16>(url.port());
    }
    return !request->target.host.isEmpty();
}

QString shellSingleQuoted(QString text)
{
    text.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + text + QLatin1Char('\'');
}

QString shimScript(const QString &executable)
{
    return QStringLiteral("#!/bin/bash\n")
        + QStringLiteral("# Shellium compatibility entry. NSFOCUS LauncherApp executes this path\n")
        + QStringLiteral("# when the bastion's SecureCRT button is clicked.\n")
        + QStringLiteral("exec ") + shellSingleQuoted(executable) + QStringLiteral(" \"$@\"\n");
}

QString shimInfoPlist()
{
    return QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "\t<key>CFBundleExecutable</key>\n"
        "\t<string>SecureCRT</string>\n"
        "\t<key>CFBundleIdentifier</key>\n"
        "\t<string>app.shellium.securecrt-shim</string>\n"
        "\t<key>CFBundleName</key>\n"
        "\t<string>SecureCRT</string>\n"
        "\t<key>CFBundlePackageType</key>\n"
        "\t<string>APPL</string>\n"
        "\t<key>CFBundleShortVersionString</key>\n"
        "\t<string>1.0</string>\n"
        "\t<key>CFBundleVersion</key>\n"
        "\t<string>1</string>\n"
        "\t<key>LSUIElement</key>\n"
        "\t<true/>\n"
        "</dict>\n"
        "</plist>\n");
}

bool writeFile(const QString &path, const QByteArray &contents, const bool executable, QString *errorMessage)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage != nullptr) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    if (file.write(contents) != contents.size() || !file.commit()) {
        if (errorMessage != nullptr) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    if (!executable) {
        return true;
    }
    QFile written(path);
    const QFileDevice::Permissions permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner
        | QFileDevice::ExeOwner | QFileDevice::ReadGroup | QFileDevice::ExeGroup
        | QFileDevice::ReadOther | QFileDevice::ExeOther;
    if (!written.setPermissions(permissions)) {
        if (errorMessage != nullptr) {
            *errorMessage = written.errorString();
        }
        return false;
    }
    return true;
}

QString identifierFromXml(const QByteArray &raw)
{
    QXmlStreamReader reader(raw);
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement() || reader.name() != QLatin1String("key")) {
            continue;
        }
        if (reader.readElementText() != QLatin1String("CFBundleIdentifier")) {
            continue;
        }
        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isStartElement() && reader.name() == QLatin1String("string")) {
                return reader.readElementText();
            }
        }
    }
    return {};
}

#ifdef Q_OS_MACOS
QString identifierFromPlistFile(const QString &path)
{
    const QByteArray utf8 = path.toUtf8();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(utf8.constData()),
        utf8.size(),
        false);
    if (url == nullptr) {
        return {};
    }
    CFReadStreamRef stream = CFReadStreamCreateWithFile(kCFAllocatorDefault, url);
    CFRelease(url);
    if (stream == nullptr || !CFReadStreamOpen(stream)) {
        if (stream != nullptr) {
            CFRelease(stream);
        }
        return {};
    }
    CFPropertyListRef plist = CFPropertyListCreateWithStream(
        kCFAllocatorDefault, stream, 0, kCFPropertyListImmutable, nullptr, nullptr);
    CFReadStreamClose(stream);
    CFRelease(stream);
    if (plist == nullptr || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
        if (plist != nullptr) {
            CFRelease(plist);
        }
        return {};
    }
    const void *value = CFDictionaryGetValue(static_cast<CFDictionaryRef>(plist), CFSTR("CFBundleIdentifier"));
    QString identifier;
    if (value != nullptr && CFGetTypeID(value) == CFStringGetTypeID()) {
        const auto ident = static_cast<CFStringRef>(value);
        const CFIndex length = CFStringGetLength(ident);
        QVector<UniChar> characters(static_cast<int>(length));
        CFStringGetCharacters(ident, CFRangeMake(0, length), characters.data());
        identifier = QString::fromUtf16(reinterpret_cast<const char16_t *>(characters.constData()),
                                        static_cast<qsizetype>(length));
    }
    CFRelease(plist);
    return identifier;
}
#endif

QString bundleIdentifierAt(const QString &plistPath)
{
    QFile file(plistPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray raw = file.readAll();
#ifdef Q_OS_MACOS
    if (raw.startsWith("bplist")) {
        return identifierFromPlistFile(plistPath);
    }
#endif
    return identifierFromXml(raw);
}

QByteArray launchPayload(const SecureCrtLaunchRequest &request)
{
    QJsonObject object;
    object.insert(QStringLiteral("v"), 1);
    object.insert(QStringLiteral("host"), request.target.host);
    object.insert(QStringLiteral("port"), request.target.port);
    object.insert(QStringLiteral("username"), request.target.username);
    object.insert(QStringLiteral("loginUsername"), request.target.loginUsername);
    object.insert(QStringLiteral("password"), request.target.password);
    object.insert(QStringLiteral("displayName"), request.target.displayName);
    object.insert(QStringLiteral("identityFile"), request.target.identityFile);
    object.insert(QStringLiteral("keyboardInteractive"),
                  request.target.authPreference == ConnectionTarget::AuthPreference::KeyboardInteractive);
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

void wipe(QByteArray *bytes)
{
    if (bytes == nullptr) {
        return;
    }
    bytes->fill('\0');
    bytes->clear();
}

std::optional<SecureCrtLaunchRequest> requestFromPayload(QByteArray payload)
{
    const auto parsed = QJsonDocument::fromJson(payload);
    wipe(&payload);
    if (!parsed.isObject()) {
        return std::nullopt;
    }
    const QJsonObject object = parsed.object();
    if (object.value(QStringLiteral("v")).toInt() != 1) {
        return std::nullopt;
    }
    SecureCrtLaunchRequest request;
    request.target.host = object.value(QStringLiteral("host")).toString();
    request.target.port = static_cast<quint16>(object.value(QStringLiteral("port")).toInt(22));
    request.target.username = object.value(QStringLiteral("username")).toString();
    request.target.loginUsername = object.value(QStringLiteral("loginUsername")).toString();
    request.target.password = object.value(QStringLiteral("password")).toString();
    request.target.displayName = object.value(QStringLiteral("displayName")).toString();
    request.target.identityFile = object.value(QStringLiteral("identityFile")).toString();
    request.target.authPreference = object.value(QStringLiteral("keyboardInteractive")).toBool()
        ? ConnectionTarget::AuthPreference::KeyboardInteractive
        : ConnectionTarget::AuthPreference::Automatic;
    request.target.confidence = ConnectionTarget::Confidence::Confirmed;
    if (!request.target.isValid()) {
        request.target.password.fill(QChar::Null);
        request.target.password.clear();
        request.target.loginUsername.clear();
        return std::nullopt;
    }
    return request;
}

} // namespace

std::optional<SecureCrtLaunchRequest> parseSecureCrtLaunch(const QStringList &arguments)
{
    SecureCrtLaunchRequest request;
    request.target.port = 22;
    bool sawSecureCrtVerb = false;
    bool sawSsh2 = false;
    bool sawOtherProtocol = false;
    bool portExplicit = false;
    QString explicitLogin;
    QString otherProtocol;
    QStringList positionals;

    for (int index = 1; index < arguments.size(); ++index) {
        const QString token = arguments.at(index);
        if (!isOptionToken(token)) {
            if (token.startsWith(QLatin1String("ssh://"), Qt::CaseInsensitive)) {
                sawSecureCrtVerb = true;
                applySshUrl(&request, token);
            } else {
                positionals.append(stripMatchingQuotes(token));
            }
            continue;
        }
        const QString name = optionName(token);
        QString value;
        if (optionTakesValue(name)) {
            if (index + 1 >= arguments.size()) {
                request.error = SecureCrtLaunchRequest::Error::IncompleteOption;
                request.errorDetail = token;
                return request;
            }
            value = stripMatchingQuotes(arguments.at(++index));
        }
        if (name == QLatin1String("ssh2")) {
            sawSsh2 = true;
            sawSecureCrtVerb = true;
        } else if (name == QLatin1String("ssh1") || name == QLatin1String("telnet")
                   || name == QLatin1String("rlogin") || name == QLatin1String("serial")
                   || name == QLatin1String("raw") || name == QLatin1String("tapi")
                   || name == QLatin1String("sftp")) {
            sawOtherProtocol = true;
            otherProtocol = name;
            sawSecureCrtVerb = true;
        } else if (name == QLatin1String("n")) {
            request.target.displayName = value;
        } else if (name == QLatin1String("p")) {
            bool ok = false;
            const int port = value.toInt(&ok);
            if (!ok || port < 1 || port > 65535) {
                request.error = SecureCrtLaunchRequest::Error::InvalidPort;
                request.errorDetail = value;
                return request;
            }
            request.target.port = static_cast<quint16>(port);
            portExplicit = true;
        } else if (name == QLatin1String("l")) {
            explicitLogin = value;
        } else if (name == QLatin1String("password")) {
            request.target.password = value;
            sawSecureCrtVerb = true;
        } else if (name == QLatin1String("auth")) {
            if (value.contains(QLatin1String("keyboard-interactive"), Qt::CaseInsensitive)) {
                request.target.authPreference = ConnectionTarget::AuthPreference::KeyboardInteractive;
            }
            sawSecureCrtVerb = true;
        }
    }

    if (!sawSecureCrtVerb) {
        return std::nullopt;
    }
    if (request.error != SecureCrtLaunchRequest::Error::None) {
        return request;
    }
    if (!sawSsh2 && sawOtherProtocol) {
        request.error = SecureCrtLaunchRequest::Error::UnsupportedProtocol;
        request.errorDetail = otherProtocol;
        return request;
    }
    if (!explicitLogin.isEmpty()) {
        applyLoginName(&request.target, explicitLogin);
    }
    if (request.target.host.isEmpty()) {
        if (positionals.isEmpty()) {
            request.error = SecureCrtLaunchRequest::Error::MissingDestination;
            return request;
        }
        if (!applyDestination(&request, positionals.constFirst(), portExplicit)) {
            return request;
        }
    }
    if (request.target.host.isEmpty()) {
        request.error = SecureCrtLaunchRequest::Error::MissingDestination;
    }
    request.target.confidence = ConnectionTarget::Confidence::Confirmed;
    return request;
}

QString secureCrtLaunchErrorText(const SecureCrtLaunchRequest &request)
{
    switch (request.error) {
    case SecureCrtLaunchRequest::Error::None:
        return {};
    case SecureCrtLaunchRequest::Error::UnsupportedProtocol:
        return QCoreApplication::translate(
            "SecureCrtLaunch",
            "Shellium can open the SSH2 sessions this bastion sends to SecureCRT. This launch requested %1, which Shellium does not emulate.")
            .arg(request.errorDetail);
    case SecureCrtLaunchRequest::Error::MissingDestination:
        return QCoreApplication::translate(
            "SecureCrtLaunch",
            "The SecureCRT launch did not include a server address.");
    case SecureCrtLaunchRequest::Error::InvalidPort:
        return QCoreApplication::translate(
            "SecureCrtLaunch",
            "The SecureCRT launch port is invalid: %1")
            .arg(request.errorDetail);
    case SecureCrtLaunchRequest::Error::IncompleteOption:
        return QCoreApplication::translate(
            "SecureCrtLaunch",
            "The SecureCRT option %1 is missing its value.")
            .arg(request.errorDetail);
    }
    return {};
}

QString secureCrtLaunchServerName()
{
#ifdef Q_OS_WIN
    const QString user = qEnvironmentVariable("USERNAME");
#else
    const QString user = QString::number(static_cast<qulonglong>(::getuid()));
#endif
    return QStringLiteral("shellium-securecrt-%1").arg(user.isEmpty() ? QStringLiteral("user") : user);
}

bool waitForSocket(QLocalSocket *socket, const int timeoutMs, const bool waitUntilConnected)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (waitUntilConnected && socket->state() == QLocalSocket::ConnectedState) {
            return true;
        }
        if (!waitUntilConnected && socket->bytesAvailable() > 0) {
            return true;
        }
        const QLocalSocket::LocalSocketError error = socket->error();
        if (socket->state() == QLocalSocket::UnconnectedState
            && (error == QLocalSocket::ServerNotFoundError
                || error == QLocalSocket::ConnectionRefusedError
                || error == QLocalSocket::SocketAccessError)) {
            return false;
        }
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        const int remaining = qMax(timeoutMs - static_cast<int>(timer.elapsed()), 1);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(socket, &QLocalSocket::connected, &loop, &QEventLoop::quit);
        QObject::connect(socket, &QLocalSocket::readyRead, &loop, &QEventLoop::quit);
        QObject::connect(socket, &QLocalSocket::disconnected, &loop, &QEventLoop::quit);
        QObject::connect(socket, &QLocalSocket::errorOccurred, &loop, &QEventLoop::quit);
        deadline.start(remaining);
        loop.exec();
        if (waitUntilConnected && socket->state() == QLocalSocket::ConnectedState) {
            return true;
        }
        if (!waitUntilConnected && socket->bytesAvailable() > 0) {
            return true;
        }
    }
    return waitUntilConnected
        ? socket->state() == QLocalSocket::ConnectedState
        : socket->bytesAvailable() > 0;
}

bool deliverSecureCrtLaunch(const QString &serverName,
                            const SecureCrtLaunchRequest &request,
                            const int timeoutMs,
                            QString *errorMessage)
{
    QLocalSocket socket;
    socket.connectToServer(serverName);
    // 用事件循环而不是 waitForReadyRead：同一次点击若发生在已经运行的进程里，服务端
    // 也在这条线程上，阻塞式等待不会让它接受连接。
    if (socket.state() != QLocalSocket::ConnectedState && !waitForSocket(&socket, timeoutMs, true)) {
        if (errorMessage != nullptr) {
            *errorMessage = socket.errorString();
        }
        return false;
    }

    QByteArray payload = launchPayload(request);
    const qint64 written = socket.write(payload);
    wipe(&payload);
    if (written < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = socket.errorString();
        }
        return false;
    }
    socket.flush();

    QByteArray response;
    QElapsedTimer timer;
    timer.start();
    while (!response.contains('\n') && timer.elapsed() < timeoutMs) {
        if (socket.bytesAvailable() == 0) {
            const int remaining = qMax(timeoutMs - static_cast<int>(timer.elapsed()), 1);
            if (!waitForSocket(&socket, remaining, false) && socket.bytesAvailable() == 0) {
                break;
            }
        }
        response += socket.readAll();
        if (response.size() > 256) {
            break;
        }
    }
    const bool accepted = response.startsWith("OK");
    if (!accepted && errorMessage != nullptr) {
        *errorMessage = QCoreApplication::translate(
            "SecureCrtLaunch",
            "The running Shellium did not accept the SecureCRT launch.");
    }
    return accepted;
}

class SecureCrtLaunchServer : public QObject {
    Q_OBJECT

public:
    explicit SecureCrtLaunchServer(QObject *parent)
        : QObject(parent)
    {
    }

    bool listen(const QString &name,
                const std::function<void(const SecureCrtLaunchRequest &)> &handler,
                QString *errorMessage)
    {
        m_handler = handler;
        m_server.setSocketOptions(QLocalServer::UserAccessOption);
        if (m_server.listen(name)) {
            connect(&m_server, &QLocalServer::newConnection, this, &SecureCrtLaunchServer::acceptConnection);
            return true;
        }
        // 上次崩溃可能留下无人监听的套接字文件。只有确认连不上时才清掉，避免把正在运行的
        // 实例踢下线。
        QLocalSocket probe;
        probe.connectToServer(name);
        const bool live = probe.waitForConnected(200);
        probe.disconnectFromServer();
        if (live) {
            if (errorMessage != nullptr) {
                *errorMessage = m_server.errorString();
            }
            return false;
        }
        QLocalServer::removeServer(name);
        if (m_server.listen(name)) {
            connect(&m_server, &QLocalServer::newConnection, this, &SecureCrtLaunchServer::acceptConnection);
            return true;
        }
        if (errorMessage != nullptr) {
            *errorMessage = m_server.errorString();
        }
        return false;
    }

private:
    void acceptConnection()
    {
        while (QLocalSocket *socket = m_server.nextPendingConnection()) {
            socket->setParent(this);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
                readSocket(socket);
            });
            connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);
            connect(socket, &QLocalSocket::destroyed, this, [this, socket]() {
                QByteArray leftover = m_buffers.take(socket);
                wipe(&leftover);
                m_completedSockets.remove(socket);
            });
            if (socket->bytesAvailable() > 0) {
                readSocket(socket);
            }
        }
    }

    void readSocket(QLocalSocket *socket)
    {
        if (socket == nullptr || m_completedSockets.contains(socket)) {
            return;
        }
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        if (buffer.size() > kMaxLaunchPayloadBytes) {
            wipe(&buffer);
            socket->write("ERR\n");
            socket->disconnectFromServer();
            m_completedSockets.insert(socket);
            return;
        }
        const int newline = buffer.indexOf('\n');
        if (newline < 0) {
            return;
        }
        QByteArray line = buffer.left(newline);
        wipe(&buffer);
        const std::optional<SecureCrtLaunchRequest> request = requestFromPayload(line);
        wipe(&line);
        if (!request.has_value()) {
            socket->write("ERR\n");
            socket->disconnectFromServer();
            m_completedSockets.insert(socket);
            return;
        }
        // 先确认再打开会话。对端确认前不能弹主机密钥对话框，否则点击方会一直等到超时。
        socket->write("OK\n");
        socket->flush();
        socket->disconnectFromServer();
        m_completedSockets.insert(socket);
        if (m_handler) {
            m_handler(*request);
        }
    }

    QLocalServer m_server;
    std::function<void(const SecureCrtLaunchRequest &)> m_handler;
    QHash<QLocalSocket *, QByteArray> m_buffers;
    QSet<QLocalSocket *> m_completedSockets;
};

SecureCrtLaunchServer *startSecureCrtLaunchServer(
    QObject *parent,
    const QString &serverName,
    const std::function<void(const SecureCrtLaunchRequest &)> &handler,
    QString *errorMessage)
{
    auto *server = new SecureCrtLaunchServer(parent);
    if (!server->listen(serverName, handler, errorMessage)) {
        delete server;
        return nullptr;
    }
    return server;
}

SecureCrtShimResult ensureSecureCrtShim(const QString &shelliumExecutable, const QString &bundlePath)
{
    SecureCrtShimResult result;
    const QFileInfo executableInfo(shelliumExecutable);
    if (!executableInfo.exists() || !executableInfo.isExecutable()) {
        result.status = SecureCrtShimResult::Status::Failed;
        result.detail = shelliumExecutable;
        return result;
    }

    const QString plistPath = bundlePath + QStringLiteral("/Contents/Info.plist");
    const QString executablePath = bundlePath + QStringLiteral("/Contents/MacOS/SecureCRT");
    const QString markerPath = bundlePath + QStringLiteral("/Contents/Resources/")
        + QString::fromLatin1(kShimMarkerName);
    const QString identifier = bundleIdentifierAt(plistPath);
    const bool markerExists = QFileInfo::exists(markerPath);
    const bool ours = markerExists
        || identifier.compare(QLatin1String(kShimBundleIdentifier), Qt::CaseInsensitive) == 0;
    if (QFileInfo::exists(bundlePath) && !ours) {
        result.status = identifier.contains(QLatin1String("vandyke"), Qt::CaseInsensitive)
            ? SecureCrtShimResult::Status::LeftExistingClient
            : SecureCrtShimResult::Status::LeftUnrelatedApp;
        result.detail = identifier.isEmpty() ? bundlePath : identifier;
        return result;
    }

    const QString script = shimScript(executableInfo.absoluteFilePath());
    QFile currentScript(executablePath);
    QFile currentMarker(markerPath);
    const bool sameScript = currentScript.open(QIODevice::ReadOnly)
        && currentScript.readAll() == script.toUtf8();
    const bool sameMarker = currentMarker.open(QIODevice::ReadOnly)
        && currentMarker.readAll().trimmed() == executableInfo.absoluteFilePath().toUtf8();
    currentScript.close();
    currentMarker.close();
    if (sameScript && sameMarker && QFileInfo(executablePath).isExecutable()) {
        result.status = SecureCrtShimResult::Status::Unchanged;
        return result;
    }

    QDir directory;
    if (!directory.mkpath(bundlePath + QStringLiteral("/Contents/MacOS"))
        || !directory.mkpath(bundlePath + QStringLiteral("/Contents/Resources"))) {
        result.status = SecureCrtShimResult::Status::Failed;
        result.detail = bundlePath;
        return result;
    }

    QString writeError;
    const bool existed = QFileInfo::exists(executablePath);
    if (!writeFile(plistPath, shimInfoPlist().toUtf8(), false, &writeError)
        || !writeFile(executablePath, script.toUtf8(), true, &writeError)
        || !writeFile(markerPath, executableInfo.absoluteFilePath().toUtf8() + '\n', false, &writeError)) {
        result.status = SecureCrtShimResult::Status::Failed;
        result.detail = writeError.isEmpty() ? bundlePath : writeError;
        return result;
    }
    result.status = existed ? SecureCrtShimResult::Status::Updated : SecureCrtShimResult::Status::Installed;
    return result;
}

QString defaultSecureCrtShimBundlePath()
{
#ifdef Q_OS_MACOS
    return QStringLiteral("/Applications/SecureCRT.app");
#else
    return {};
#endif
}

#include "SecureCrtLaunch.moc"
